import io
import os
import subprocess
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

import build_all
import prepare_vendor


class BuildAllTests(unittest.TestCase):
    def test_https_switch_preserves_endpoint_names_and_language_variants(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            config = project / "ap/config/bk7259_ap/defconfig"
            config.parent.mkdir(parents=True)
            for https in (False, True):
                with self.subTest(https=https):
                    setting = ("CONFIG_MYBOT_HTTPS=y" if https else
                               "# CONFIG_MYBOT_HTTPS is not set")
                    config.write_text("CONFIG_MYBOT_LANGUAGE_ZH_CN=y\n"
                                      "# CONFIG_MYBOT_LANGUAGE_EN_US is not set\n"
                                      f"CONFIG_MYBOT_VIDEO=y\n{setting}\n", encoding="utf-8")
                    with patch.object(build_all, "PROJECT", project):
                        lines, found, options, video, enabled = build_all.source_config()
                    self.assertTrue(video)
                    self.assertEqual(enabled, https)
                    for language, host in (("zh-CN", "botstation.shengwang.cn"),
                                           ("en-US", "botstation.agora.io")):
                        selected = build_all.variant_config(lines, found, options, language)
                        self.assertIn(setting + "\n", selected)
                        scheme = "https" if https else "http"
                        self.assertEqual(build_all.endpoint(language, enabled),
                                         f"{scheme}://{host}/api".encode("ascii"))
                        suffix = "" if https else "-http"
                        self.assertEqual(build_all.firmware_name(language, video, enabled),
                                         f"bk7259-{language}-video{suffix}")
                        self.assertEqual(build_all.firmware_name(language, False, enabled),
                                         f"bk7259-{language}{suffix}")

    def test_firmware_rejects_wrong_or_multiple_endpoint_schemes(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            built = project / "build/bk7259/mybot"
            image = built / "package/all-app.bin"
            config = built / "bk7259_ap/config/sdkconfig.h"
            elf = built / "bk7259_ap/app.elf"
            image.parent.mkdir(parents=True)
            config.parent.mkdir(parents=True)
            image.write_bytes(b"combined AP and CP image")
            for language in build_all.LANGUAGES:
                for https in (False, True):
                    with self.subTest(language=language, https=https):
                        selected = "ZH_CN" if language == "zh-CN" else "EN_US"
                        config.write_text(f"#define CONFIG_MYBOT_LANGUAGE_{selected} 1\n"
                                          "#define CONFIG_MYBOT_VIDEO 1\n" +
                                          ("#define CONFIG_MYBOT_HTTPS 1\n" if https else ""),
                                          encoding="utf-8")
                        expected = build_all.endpoint(language, https)
                        elf.write_bytes(b"ELF\0" + expected + b"\0")
                        self.assertEqual(build_all.check_firmware(
                            project, language, True, https), image)
                        for choice in build_all.LANGUAGES:
                            for mode in (False, True):
                                if (choice, mode) == (language, https):
                                    continue
                                other = build_all.endpoint(choice, mode)
                                for contents in (other, expected + b"\0" + other):
                                    elf.write_bytes(contents)
                                    with self.assertRaisesRegex(RuntimeError, "endpoint"):
                                        build_all.check_firmware(project, language, True, https)
                        elf.write_bytes(expected)
                        with self.assertRaisesRegex(RuntimeError, "configuration"):
                            build_all.check_firmware(project, language, True, not https)

    def test_prepare_cli_selects_http_variant_without_modifying_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "projects/mybot"
            config = project / "ap/config/bk7259_ap/defconfig"
            config.parent.mkdir(parents=True)
            original = ("CONFIG_MYBOT_LANGUAGE_ZH_CN=y\n"
                        "# CONFIG_MYBOT_LANGUAGE_EN_US is not set\n"
                        "CONFIG_MYBOT_VIDEO=y\n# CONFIG_MYBOT_HTTPS is not set\n")
            config.write_text(original, encoding="utf-8")
            stage = root / "prepared"
            components = stage / "bk_solution_ai/components"
            components.mkdir(parents=True)
            build_root = root / "build"
            with patch.object(build_all, "ROOT", root), \
                    patch.object(build_all, "PROJECT", project), \
                    patch.object(prepare_vendor, "prepare_vendor", return_value=stage), \
                    patch("sys.argv", ["prepare_vendor.py", "--language", "en-US",
                                       "--build-root", str(build_root)]), \
                    redirect_stdout(io.StringIO()) as output:
                self.assertEqual(prepare_vendor.main(), 0)
            copied = (build_root / "build-bk7259-en-US-video-http/solution/projects/mybot" /
                      "ap/config/bk7259_ap/defconfig").read_text(encoding="utf-8")
            self.assertIn("CONFIG_MYBOT_LANGUAGE_EN_US=y\n", copied)
            self.assertIn("# CONFIG_MYBOT_HTTPS is not set\n", copied)
            self.assertEqual(config.read_text(encoding="utf-8"), original)
            self.assertIn("build-bk7259-en-US-video-http", output.getvalue())
            self.assertIn(f"SDK_DIR={stage / 'bk_avdk_smp'}", output.getvalue())

    def test_no_clean_removes_stale_source_but_keeps_build_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "source"
            config = project / "ap/config/bk7259_ap/defconfig"
            config.parent.mkdir(parents=True)
            config.write_text("original\n", encoding="utf-8")
            components = root / "components"
            components.mkdir()
            variant = root / "variant"

            with patch.object(build_all, "PROJECT", project), \
                    patch.object(build_all, "COMPONENTS", components):
                copy = build_all.prepare_variant(variant, "first\n", clean=True)
                (copy / "stale.h").write_text("stale", encoding="utf-8")
                cache = copy / "build/cache"
                cache.parent.mkdir()
                cache.write_text("cached", encoding="utf-8")
                (project / "new.h").write_text("current", encoding="utf-8")

                build_all.prepare_variant(variant, "second\n", clean=False)

            self.assertFalse((copy / "stale.h").exists())
            self.assertEqual((copy / "new.h").read_text(encoding="utf-8"), "current")
            self.assertEqual(cache.read_text(encoding="utf-8"), "cached")
            self.assertEqual((copy / "ap/config/bk7259_ap/defconfig").read_text(
                encoding="utf-8"), "second\n")

    def test_publish_restores_previous_pair_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            outputs = [root / "zh.bin", root / "en.bin"]
            for output in outputs:
                output.write_bytes(b"previous")
                (staged / output.name).write_bytes(b"current")

            real_replace = os.replace

            def fail_second_image(source, target):
                if Path(source) == staged / "en.bin":
                    raise OSError("simulated publish failure")
                return real_replace(source, target)

            with patch.object(build_all.os, "replace", side_effect=fail_second_image):
                with self.assertRaisesRegex(OSError, "simulated publish failure"):
                    build_all.publish_outputs(staged, outputs)

            for output in outputs:
                self.assertEqual(output.read_bytes(), b"previous")

    def test_publish_rejects_non_file_target_before_replacing_any_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            first = root / "zh.bin"
            second = root / "en.bin"
            first.write_bytes(b"previous")
            second.mkdir()
            (staged / first.name).write_bytes(b"current")
            (staged / second.name).write_bytes(b"current")

            with self.assertRaisesRegex(RuntimeError, "not a regular file"):
                build_all.publish_outputs(staged, [first, second])

            self.assertEqual(first.read_bytes(), b"previous")

    def test_changed_vendor_invalidates_no_clean_build_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "source"
            config = project / "ap/config/bk7259_ap/defconfig"
            config.parent.mkdir(parents=True)
            config.write_text("original\n", encoding="utf-8")
            first_components = root / "vendor-first/components"
            second_components = root / "vendor-second/components"
            first_components.mkdir(parents=True)
            second_components.mkdir(parents=True)
            with patch.object(build_all, "PROJECT", project):
                copy = build_all.prepare_variant(root / "variant", "first\n", clean=True,
                                                 components=first_components)
                (copy / "build").mkdir()
                (copy / "build/cache").write_text("stale objects", encoding="utf-8")
                build_all.prepare_variant(root / "variant", "second\n", clean=False,
                                          components=second_components)
            self.assertFalse((copy / "build").exists())
            self.assertEqual((root / "variant/solution/components").resolve(), second_components)

    def test_second_language_build_failure_preserves_previous_firmware_pair(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "projects/mybot"
            config = project / "ap/config/bk7259_ap/defconfig"
            config.parent.mkdir(parents=True)
            config.write_text("CONFIG_MYBOT_LANGUAGE_ZH_CN=y\n"
                              "# CONFIG_MYBOT_LANGUAGE_EN_US is not set\n"
                              "CONFIG_MYBOT_VIDEO=y\nCONFIG_MYBOT_HTTPS=y\n", encoding="utf-8")
            vendor = root / "prepared"
            (vendor / "bk_solution_ai/components").mkdir(parents=True)
            outputs = root / "releases"
            outputs.mkdir()
            images = [outputs / f"bk7259-{language}-video.bin" for language in build_all.LANGUAGES]
            for image in images:
                image.write_bytes(b"previous")
            built = root / "new-image.bin"
            built.write_bytes(b"new")
            args = Namespace(language="both", build_root=root / "build", output_root=outputs,
                             dry_run=False, no_clean=False)
            with patch.object(build_all, "ROOT", root), \
                    patch.object(build_all, "PROJECT", project), \
                    patch.object(build_all, "parse_args", return_value=args), \
                    patch.object(build_all, "select_python", return_value=Path("/usr/bin/python3")), \
                    patch.object(build_all, "prepare_vendor", return_value=vendor) as prepare, \
                    patch.object(build_all, "check_firmware", return_value=built), \
                    patch.object(build_all.subprocess, "run", side_effect=[
                        None, subprocess.CalledProcessError(1, "make")]) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    build_all.main()
            prepare.assert_called_once()
            for call in run.call_args_list:
                self.assertIn(f"SDK_DIR={vendor / 'bk_avdk_smp'}", call.args[0])
            for image in images:
                self.assertEqual(image.read_bytes(), b"previous")


if __name__ == "__main__":
    unittest.main()
