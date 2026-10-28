import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("collect_licenses", Path(__file__).parents[1] / "scripts/collect_licenses.py")
licenses = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(licenses)


class LicenseChecks(unittest.TestCase):
    def test_build_dev_only_subtrees_are_excluded_but_shared_dependencies_remain(self):
        def node(name, *dependencies):
            return {"id": name, "deps": [{"pkg": package, "dep_kinds": [{"kind": kind}]} for package, kind in dependencies]}
        metadata = {
            "workspace_members": ["app"],
            "packages": [{"id": name, "name": name, "version": "1", "source": None if name == "app" else "registry"}
                         for name in ["app", "runtime", "shared", "build-tool", "test-tool", "build-child"]],
            "resolve": {"nodes": [node("app", ("runtime", None), ("build-tool", "build"), ("test-tool", "dev")),
                                  node("runtime", ("shared", None)), node("shared"),
                                  node("build-tool", ("shared", None), ("build-child", None)),
                                  node("test-tool"), node("build-child")]}}
        self.assertEqual([p["name"] for p in licenses.normal_packages(metadata)], ["runtime", "shared"])

    def test_notice_keeps_authorship_and_mpl_source_link(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "LICENSE").write_text("Copyright Example author\nLicense text", encoding="utf-8")
            text = licenses.dependency_notice("rust", "example", "1.2.3", "MPL-2.0", path, "https://example.org/source/1.2.3", [])
            self.assertIn("Copyright Example author", text)
            self.assertIn("https://example.org/source/1.2.3", text)
            self.assertIn("unmodified source code", text)

    def test_missing_notice_is_not_silently_omitted(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Missing license information"):
                licenses.dependency_notice("rust", "example", "1", "MIT", Path(directory), "source", [])


if __name__ == "__main__":
    unittest.main()
