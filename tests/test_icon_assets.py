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
        self.assertEqual(len(rows), 816)
        self.assertEqual(icons.build(), icons.build())
        for row in rows:
            for style in ("light", "dark"):
                pixels = icons.pixels(row["asset"], style)
                self.assertEqual(len(pixels), row["size"] ** 2)
                for p in pixels:
                    self.assertTrue(all((p >> shift & 255) <= p >> 24 for shift in (0, 8, 16)))


if __name__ == "__main__":
    unittest.main()
