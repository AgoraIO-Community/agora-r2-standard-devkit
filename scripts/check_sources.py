#!/usr/bin/env python3
"""Check product boundaries and pinned inputs without inspecting local references."""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

from vendor_workspace import load_lock, validate_sources


ROOT = Path(__file__).resolve().parent.parent
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.MULTILINE)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
COMMENTS_AND_LITERALS = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*', re.DOTALL)


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def locked_value(path, name):
    values = re.findall(rf"^{re.escape(name)}=(.+)$", path.read_text(encoding="utf-8"),
                        re.MULTILINE)
    if len(values) != 1 or not re.fullmatch(r"[0-9a-f]{64}", values[0]):
        raise RuntimeError(f"missing or invalid {name} in {path}")
    return values[0]


def tree_digest(root, directories):
    """Match the SHA256-of-sha256sum-lines format used by the revision locks."""
    files = []
    for directory in directories:
        base = root / directory
        if base.is_symlink() or not base.is_dir():
            raise RuntimeError(f"missing or linked immutable source tree: {base}")
        for path in base.rglob("*"):
            if path.is_symlink() or not (path.is_dir() or path.is_file()):
                raise RuntimeError(f"special file in immutable source tree: {path}")
            if path.is_file():
                if path.stat().st_mode & 0o111:
                    raise RuntimeError(f"executable in immutable source tree: {path}")
                relative = path.relative_to(root).as_posix()
                files.append((f"./{relative}" if directory == "." else relative, path))
    records = b"".join(f"{sha256(path)}  {name}\n".encode("utf-8")
                       for name, path in sorted(files))
    return hashlib.sha256(records).hexdigest()


def check_snapshots(root):
    sdk = root / "components/mybot/mybot_sdk"
    aosl = root / "components/mybot/mybot_aosl"
    checks = [
        (tree_digest(sdk, ("include", "src")),
         locked_value(sdk / "SDK_REVISION", "src_include_sha256"), "MyBot SDK source"),
        (sha256(sdk / "LICENSE"),
         locked_value(sdk / "SDK_REVISION", "license_sha256"), "MyBot SDK license"),
        (tree_digest(aosl / "aosl", (".",)),
         locked_value(aosl / "AOSL_REVISION", "vendored_content_sha256"), "AOSL source"),
    ]
    cmake = (sdk / "CMakeLists.txt").read_text(encoding="utf-8")
    for name, relative in (
            ("header", "include/agora_rtc_api.h"),
            ("library", "lib/arm/libagora-rtc-sdk.a")):
        pin = re.findall(rf'set\(_rtsa_{name}_sha256\s+"([0-9a-f]{{64}})"\s*\)', cmake)
        if len(pin) != 1:
            raise RuntimeError(f"missing or ambiguous RTSA {name} pin in {sdk / 'CMakeLists.txt'}")
        checks.append((sha256(root / "components/mybot/mybot_rtsa" / relative),
                       pin[0], f"RTSA {name}"))
    for actual, expected, label in checks:
        if actual != expected:
            raise RuntimeError(f"{label} digest mismatch: {actual}; expected {expected}")
        print(f"PASS {label}: {actual}")


def check_boundaries(root):
    sdk = root / "components/mybot/mybot_sdk"
    private_names = {path.name for path in (sdk / "src").rglob("*.h")}
    platform = sdk / "platforms/bk7259"
    project = root / "projects/mybot"
    application = project / "ap/ap_main.c"
    test_implementations = {
        project / "tests/gimbal/test_gimbal.c": platform / "bk7259_gimbal.c",
        project / "tests/https/test_https.c": platform / "bk7259_https.c",
    }
    failures = []
    count = 0
    for base in (platform, project):
        if base.is_symlink() or not base.is_dir():
            raise RuntimeError(f"missing or linked product source tree: {base}")
        for path in sorted(base.rglob("*")):
            if path.is_symlink():
                raise RuntimeError(f"linked product input: {path}")
            if not path.is_file():
                continue
            is_source = path.suffix in SOURCE_SUFFIXES
            if not is_source and path.name != "CMakeLists.txt" and path.suffix != ".cmake":
                continue
            text = path.read_text(encoding="utf-8")
            relative = path.relative_to(root)
            if re.search(r"MYBOT_SDK_DIR|/home/|/Users/|mybot_sdk/src|mybot_sdk/include", text):
                failures.append(f"{relative}: external SDK path or source-tree dependency")
            if not is_source:
                continue
            count += 1
            uncommented = COMMENTS_AND_LITERALS.sub(
                lambda match: "\n" * match.group().count("\n")
                if match.group().startswith(("/*", "//")) else match.group(), text)
            for match in INCLUDE.finditer(uncommented):
                header = match.group(1)
                parts = Path(header).parts
                allowed = header.startswith("mybot/platform/") and ".." not in parts and \
                    (sdk / "include" / header).is_file()
                if path == application and header in ("mybot/mybot.h", "mybot/mybot_version.h"):
                    allowed = True
                # Existing host harnesses include their platform implementation to
                # exercise static functions. They may not include private SDK sources.
                if path in test_implementations and \
                        (path.parent / header).resolve() == test_implementations[path]:
                    allowed = True
                protected = header.startswith("mybot/") or Path(header).name in private_names or \
                    "mybot_sdk" in parts or "agora_rtc_api.h" in parts
                if protected and not allowed:
                    line = uncommented.count("\n", 0, match.start()) + 1
                    failures.append(f"{relative}:{line}: prohibited SDK include {header}")
            code = COMMENTS_AND_LITERALS.sub("", uncommented)
            if path != application and re.search(r"\bmybot_(?:start|stop|is_running)\s*\(", code):
                failures.append(f"{relative}: SDK lifecycle is owned by ap_main.c")
            if path != application and re.search(r"\bmybot_get_state\s*\(", code):
                failures.append(f"{relative}: unapproved SDK state query")
            if re.search(r"\bagora_[a-zA-Z0-9_]+\s*\(", code):
                failures.append(f"{relative}: direct Agora API call outside SDK")
    if failures:
        raise RuntimeError("\n".join(failures))
    print(f"PASS product include boundaries: {count} source files")
    print("NOTE existing host harnesses may include only their matching platform implementation")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root to verify")
    root = parser.parse_args(argv).root.resolve()
    try:
        manifest, _ = load_lock(root)
        validate_sources(root, manifest)
        for name, repository in manifest["repositories"].items():
            url = subprocess.run(
                ["git", "config", "--file", str(root / ".gitmodules"),
                 "--get", f"submodule.{name}.url"],
                check=True, capture_output=True, text=True,
            ).stdout.strip()
            if url != repository["url"]:
                raise RuntimeError(f"{name} .gitmodules URL does not match official vendor lock")
            print(f"PASS official {name}: {repository['commit']} (tracked tree clean)")
        check_snapshots(root)
        check_boundaries(root)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR {error}", file=sys.stderr)
        return 1
    print("Source checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
