#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the R2 user guide and its real provisioning-page illustration offline."""

import argparse
import ast
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


DOCS = Path(__file__).resolve().parent
ROOT = DOCS.parent
ASSETS = DOCS / "images/r2-user-guide"
OUTPUT = DOCS / "R2_操作手册.pdf"


def browser_run(browser, work, name, arguments):
    command = [
        browser, "--headless=new", "--no-sandbox", "--disable-dev-shm-usage",
        "--disable-gpu", "--disable-background-networking", "--disable-extensions",
        "--no-first-run", "--no-default-browser-check", "--hide-scrollbars",
        "--allow-file-access-from-files", f"--user-data-dir={work / name}",
        *arguments,
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f"Chrome failed ({result.returncode}): {result.stderr[-4000:]}")


def portal_html():
    source = (ROOT / "components/mybot/mybot_sdk/platforms/bk7259/bk7259_wifi_portal.c").read_text()
    match = re.search(
        r'static const char s_portal_html\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', source)
    if not match:
        raise RuntimeError("cannot locate production provisioning HTML")
    html = "".join(ast.literal_eval(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    # Use the unchanged production page with one illustrative SSID. No password
    # or device data is included, and no actual connection is submitted.
    return html.replace("scan()</script>",
                        "scan();q('#ssid').value='Home-WiFi';</script>")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pdf-only", action="store_true",
        help="render the PDF using the existing UI and provisioning-page PNGs",
    )
    args = parser.parse_args()
    browser = next((found for command in ("google-chrome", "chromium", "chromium-browser")
                    if (found := shutil.which(command))), None)
    if not browser:
        raise RuntimeError("install Chrome or Chromium to render the PDF")
    required = (
        "ready", "provision-active", "pair-code", "conversation-listening",
        "conversation-thinking", "conversation-speaking", "settings",
        "provision-confirmation", "factory-confirmation", "restoring",
        "provision-failed", "reset-failed",
    )
    if args.pdf_only:
        required += ("wifi-portal",)
    for name in required:
        if not (ASSETS / f"{name}.png").is_file():
            raise RuntimeError(f"missing UI image: {name}.png; see {ASSETS / 'README.md'}")
    with tempfile.TemporaryDirectory(prefix="mybot-r2-user-guide-") as directory:
        work = Path(directory)
        if not args.pdf_only:
            page = work / "wifi-portal.html"
            page.write_text(portal_html(), encoding="utf-8")
            browser_run(browser, work, "portal-profile", [
                "--window-size=480,680", "--force-device-scale-factor=1",
                "--virtual-time-budget=1500", f"--screenshot={ASSETS / 'wifi-portal.png'}",
                page.as_uri(),
            ])
        browser_run(browser, work, "pdf-profile", [
            "--no-pdf-header-footer", "--print-to-pdf-no-header", "--virtual-time-budget=2000",
            "--export-tagged-pdf", f"--print-to-pdf={OUTPUT}",
            (DOCS / "r2-user-guide.html").as_uri(),
        ])
    if not OUTPUT.is_file() or OUTPUT.stat().st_size == 0:
        raise RuntimeError("Chrome did not produce the PDF")
    print(f"Created {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
