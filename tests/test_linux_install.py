"""Desktop entry quoting and replacing an installed generation without truncation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import os

spec = importlib.util.spec_from_file_location("install_linux", Path(__file__).resolve().parents[1] / "scripts/install_linux.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class LinuxInstallTests(unittest.TestCase):
    def test_exec_argument_escapes_desktop_and_exec_layers(self):
        self.assertEqual(installer.exec_argument('/home/Имя с пробелом/50%/$x`"\\app'),
                         '"/home/Имя с пробелом/50%%/\\\\$x\\\\`\\\\"\\\\\\\\app"')

    @unittest.skipUnless(os.name == "posix", "Linux symlink and executable semantics")
    def test_update_keeps_open_generation_and_desktop_points_to_current(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "source"
            source.mkdir()
            for name in installer.FILES:
                (source / name).write_bytes(name.encode())
            data = Path(root) / "Каталог с пробелом"
            first = installer.install(source, data)
            executable = Path(first["executable"])
            previous = executable.resolve()
            with executable.open("rb") as opened:
                (source / "v8-chroma").write_bytes(b"new executable")
                second = installer.install(source, data)
                self.assertEqual(opened.read(), b"v8-chroma")
                self.assertTrue(previous.exists())
                self.assertEqual(executable.read_bytes(), b"new executable")
                self.assertNotEqual(executable.resolve(), previous)
            self.assertEqual(first, second)
            self.assertTrue(os.access(executable, os.X_OK))
            desktop = Path(first["desktop_entry"]).read_text()
            self.assertIn("Exec=" + installer.exec_argument(executable), desktop)
            self.assertIn("Terminal=false\n", desktop)
            self.assertEqual(installer.install(source, data), second)
