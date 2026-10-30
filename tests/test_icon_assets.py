"""The release package must be reproducible and contain only bounded own artwork."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))


@unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow is installed by the native build")
class IconAssetTests(unittest.TestCase):
    def test_pack_is_reproducible_and_all_replacements_have_artwork(self):
        import build_icon_assets as icons
        rows = icons.patches()
        self.assertEqual(len(rows), 979)
        self.assertEqual(icons.build(), icons.build())
        for row in rows:
            for style in ("light", "dark"):
                pixels = icons.pixels(row["asset"], style)
                self.assertEqual(len(pixels), row["size"] ** 2)
                for p in pixels:
                    self.assertTrue(all((p >> shift & 255) <= p >> 24 for shift in (0, 8, 16)))

    def test_repository_screenshot_sources_and_support_states_remain_distinct(self):
        import build_icon_assets as icons
        rows = {r["hash"]: r for r in icons.patches() if r["width"] == r["height"] == 16}
        # Prepared lock recovered from the running Designer, plus the menu and
        # supplier-package sources identified in the reported screenshots.
        expected = {
            "1236a28942191f6c": "repository-lock",
            "dd4059ca3400ad1b": "repository-checkout",
            "f11538bc194811d0": "repository-checkin",
            "acfdb4cebf1cc047": "repository-undo-checkout",
            "46e298d598e28596": "repository-get",
            "92655d90f2f26733": "support-package",
        }
        for key, name in expected.items():
            self.assertEqual(rows[key]["asset"], f"{name}-16.png")
        groups = (
            ("repository-lock", "repository-owned", "repository-owned-lock",
             "repository-blocked-lock", "repository-owned-lock-overlay",
             "repository-blocked-lock-overlay"),
            ("support-package", "support-package-neutral", "support-lock",
             "support-lock-neutral", "support-unavailable", "support-warning",
             "support-enabled", "support-module", "support-module-editable",
             "support-settings", "support-settings-editable", "support-tree",
             "support-packages"),
        )
        for style in ("light", "dark"):
            for names in groups:
                rendered = {tuple(icons.pixels(f"{name}-16.png", style)) for name in names}
                self.assertEqual(len(rendered), len(names), (style, names))

    def test_metadata_children_from_reported_trees(self):
        import build_icon_assets as icons
        rows = {r["hash"]: r for r in icons.patches() if r["width"] == r["height"] == 16}
        expected = {
            "a90d4ac01d57cf6b": "meta-enumeration-value",
            "11608f470a7a2044": "meta-characteristic-plan",
            "aab0becdadaa1763": "meta-account-plan",
            "93eb6ef8057b2d7e": "meta-calculation-plan",
            "423276a08535d3da": "meta-accounting-flag",
            "e0466cef1ea223fc": "meta-subconto-flag",
            "b18e90119db99351": "meta-dimension",
            "2839920a995e6982": "meta-resource",
            "5ebe72dd471e9a85": "meta-recalculation",
        }
        for key, name in expected.items():
            self.assertEqual(rows[key]["asset"], f"{name}-16.png")
        # Enum values live in an atlas with a partial trailing cell. Preserve
        # its real width instead of assuming every strip is a multiple of 16.
        enum_slots = [r for r in icons.patches()
                      if r["width"] == 727 and r["asset"] == "meta-enumeration-value-16.png"]
        self.assertEqual({r["x"] for r in enum_slots}, {160, 176})


if __name__ == "__main__":
    unittest.main()
