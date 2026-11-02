import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("release_checks", Path(__file__).parents[1] / "scripts/release_checks.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def fixture(name, delayed=False):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<H", data, 0x94, 240)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<I", data, opt + 60, 512)
    struct.pack_into("<I", data, opt + 108, 16)
    index = 13 if delayed else 1
    struct.pack_into("<II", data, opt + 112 + 8 * index, 0x1000, 64 if delayed else 40)
    struct.pack_into("<IIII", data, opt + 240 + 8, 512, 0x1000, 512, 512)
    if delayed:
        struct.pack_into("<II", data, 512, 1, 0x1100)
    else:
        struct.pack_into("<I", data, 512 + 12, 0x1100)
    encoded = name.encode() + b"\0"
    data[768:768 + len(encoded)] = encoded
    return data


class ReleaseChecks(unittest.TestCase):
    def test_notes_select_exact_stable_or_rc_section_and_preserve_subheadings(self):
        changelog = "# Изменения\n\n## 1.2.30 — дата\n- Другая версия.\n\n## 1.2.3-rc.1 — дата\n- RC.\n\n## 1.2.3 — дата\n### Исправлено\n- Шрифт.\n\n## 1.2.2 — дата\n- Старое.\n"
        self.assertEqual(release.release_notes(changelog, "v1.2.3"), "### Исправлено\n- Шрифт.")
        self.assertEqual(release.release_notes(changelog, "v1.2.3-rc.1"), "- RC.")
        self.assertEqual(release.release_notes(changelog.replace("\n", "\r\n"), "v1.2.2"), "- Старое.")

    def test_notes_reject_missing_empty_or_duplicate_version(self):
        for changelog in ["## 1.2.30\n- Другая версия.\n", "## 1.2.3\n\n## 1.2.2\n- Старое.\n",
                          "## 1.2.3\n- Один.\n## 1.2.3 — дата\n- Два.\n"]:
            with self.assertRaises(ValueError):
                release.release_notes(changelog, "v1.2.3")

    def test_all_manifests_and_locks_must_match_the_tag(self):
        root = Path(__file__).parents[1]
        paths = ["Cargo.toml", "Cargo.lock", "ui/package.json", "ui/package-lock.json", "ui/src-tauri/Cargo.toml", "ui/src-tauri/tauri.conf.json"]
        old_version = release.versions()
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory)
            for name in paths:
                destination = copy / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text((root / name).read_text(encoding="utf-8").replace(old_version, "1.2.3-rc.1"), encoding="utf-8")
            self.assertEqual(release.versions(copy, "v1.2.3-rc.1"), "1.2.3-rc.1")
            with self.assertRaisesRegex(ValueError, "does not match"):
                release.versions(copy, "v1.2.3")
            lock = copy / "ui/package-lock.json"
            changed = json.loads(lock.read_text(encoding="utf-8"))
            changed["packages"][""]["version"] = "1.2.3"
            lock.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Versions differ"):
                release.versions(copy, "v1.2.3-rc.1")

    def test_tags_reject_alpha_leading_zeros_and_injection(self):
        self.assertEqual(release.release_version("v1.2.3-rc.2"), "1.2.3-rc.2")
        self.assertEqual(release.release_version("v0.1.0"), "0.1.0")
        for value in ["v01.2.3", "v1.2.3-alpha.1", "v1.2.3-rc.0", "v1.2.3-rc.01", "v1.2.3\n", "v1.2.3;cmd", "1.2.3"]:
            with self.assertRaises(ValueError, msg=value):
                release.release_version(value)

    def test_direct_and_delayed_loader_and_vc_runtime_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.exe"
            for delayed in (False, True):
                for name in ["KERNEL32.dll", "COMBASE.dll"]:
                    path.write_bytes(fixture(name, delayed))
                    self.assertEqual(release.verify_binary(path), [name.lower()])
                for name in ["WebView2Loader.dll", "VCRUNTIME140.dll", "libstdc++-6.dll", "../kernel32.dll", "combase-helper.dll"]:
                    path.write_bytes(fixture(name, delayed))
                    with self.assertRaises(ValueError, msg=name):
                        release.verify_binary(path)

    def test_truncated_and_wrong_architecture_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.exe"
            data = fixture("kernel32.dll")
            struct.pack_into("<H", data, 0x84, 0x14C)
            for invalid in [b"", data, fixture("kernel32.dll")[:600]]:
                path.write_bytes(invalid)
                with self.assertRaises((ValueError, struct.error)):
                    release.verify_binary(path)


if __name__ == "__main__":
    unittest.main()
