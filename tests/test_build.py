import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("project_build", Path(__file__).parents[1] / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class BuildPolicy(unittest.TestCase):
    def test_release_requires_the_same_ci_tag_and_static_crt(self):
        approved = {"GITHUB_ACTIONS": "true", "GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v1.2.3-rc.1",
                    "RUSTFLAGS": "-C target-feature=+crt-static"}
        build.release_guard("v1.2.3-rc.1", approved)
        for key in approved:
            changed = approved.copy()
            del changed[key]
            with self.assertRaises(ValueError, msg=key):
                build.release_guard("v1.2.3-rc.1", changed)
        with self.assertRaises(ValueError):
            build.release_guard("v1.2.3", approved)


if __name__ == "__main__":
    unittest.main()
