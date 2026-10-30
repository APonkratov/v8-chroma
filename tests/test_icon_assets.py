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
        self.assertEqual(len(rows), 1189)
        self.assertEqual(icons.build(), icons.build())
        for row in rows:
            for style in ("light", "dark"):
                pixels = icons.pixels(row["asset"], style)
                self.assertEqual(len(pixels), row["size"] ** 2)
                for p in pixels:
                    self.assertTrue(all((p >> shift & 255) <= p >> 24 for shift in (0, 8, 16)))

    def test_spreadsheet_commands_keep_disabled_and_dynamic_states(self):
        import build_icon_assets as icons
        rows = {r["hash"]: r for r in icons.patches() if r["width"] == r["height"] == 16}
        for key, name in {
            "e9259837631945c6": "sheet-align-left-disabled",
            "82c63cfa197a784b": "sheet-align-center-disabled",
            "f06e4957990721a4": "sheet-comment-remove-disabled",
            "cee0ae048dcbe846": "sheet-group",
            "d799d13013194d32": "sheet-ungroup",
            "d714d49884a25d85": "sheet-grid",
            "248ec37121f10a82": "sheet-headers",
            "3f37a943dfd66b2e": "sheet-freeze",
        }.items():
            self.assertEqual(rows[key]["asset"], f"{name}-16.png")
        for key in ("30a513341a4b129d", "06c64d90629bb34d", "2c6d9e8d113a951d"):
            self.assertEqual(rows[key]["preserve_bottom"], 4)
        caption = next(r for r in icons.patches() if r["hash"] == "537e4a09857aaa7c")
        self.assertEqual(caption["asset"], "sheet-document-14.png")
        self.assertEqual((caption["width"], caption["height"]), (14, 14))
        names = sorted(p.stem for p in (icons.ASSETS / "dark").glob("sheet-*.svg"))
        for style in ("light", "dark"):
            # Cairo masks use only alpha, so commands must remain distinguishable
            # even when the platform supplies its own foreground color.
            alpha = {tuple(p >> 24 for p in icons.pixels(f"{name}-16.png", style)) for name in names}
            self.assertEqual(len(alpha), len(names))
            active = icons.pixels("sheet-align-left-16.png", style)
            disabled = icons.pixels("sheet-align-left-disabled-16.png", style)
            self.assertGreater(sum(p >> 24 for p in active), sum(p >> 24 for p in disabled))


if __name__ == "__main__":
    unittest.main()
