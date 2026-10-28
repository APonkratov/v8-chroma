"""Export the selected geometric C as SVG, PNG, Windows ICO and tray RGBA.

This draws project-owned vector geometry. It does not crop or alter the generated
concept board. Pillow is used only to rasterize that same geometry for Windows.
"""

from pathlib import Path
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "assets/branding"
ICONS = ROOT / "ui/src-tauri/icons"
COLORS = {"cyan": "#00C8DB", "blue": "#167CEF", "indigo": "#2636B9", "violet": "#7637F2"}
FACES = [
    ("indigo", [(24, 76), (128, 136), (128, 240), (24, 180)]),
    ("cyan", [(24, 76), (128, 16), (232, 76), (128, 136)]),
    ("blue", [(24, 76), (128, 16), (128, 136)]),
    ("violet", [(24, 180), (128, 120), (232, 180), (128, 240)]),
    ("indigo", [(24, 180), (128, 120), (128, 240)]),
]
OUTLINE = [(128, 16), (232, 76), (142, 128), (232, 180), (128, 240), (24, 180), (24, 76)]


def svg(tile=None, monochrome=None):
    content = []
    if tile:
        content.append(f'<rect width="256" height="256" rx="56" fill="{tile}"/>')
    polygons = [(monochrome, OUTLINE)] if monochrome else [(COLORS[color], points) for color, points in FACES]
    if tile:
        content.append('<g transform="translate(35.84 35.84) scale(.72)">')
    for color, points in polygons:
        coordinates = " ".join(f"{x},{y}" for x, y in points)
        content.append(f'<polygon fill="{color}" points="{coordinates}"/>')
    if tile:
        content.append('</g>')
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">' + "".join(content) + '</svg>\n'


def raster(size, tile=None, monochrome=None):
    scale = size * 4 / 256
    image = Image.new("RGBA", (size * 4, size * 4))
    draw = ImageDraw.Draw(image)
    if tile:
        draw.rounded_rectangle((0, 0, size * 4 - 1, size * 4 - 1), radius=56 * scale, fill=tile)
    polygons = [(monochrome, OUTLINE)] if monochrome else [(COLORS[color], points) for color, points in FACES]
    for color, points in polygons:
        transformed = [((x * .72 + 35.84) * scale, (y * .72 + 35.84) * scale) for x, y in points] if tile else [(x * scale, y * scale) for x, y in points]
        draw.polygon(transformed, fill=color)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    BRAND.mkdir(parents=True, exist_ok=True)
    ICONS.mkdir(parents=True, exist_ok=True)
    (ROOT / "ui/src/assets").mkdir(parents=True, exist_ok=True)
    for name, options in {
        "mark": {},
        "app-dark": {"tile": "#111827"},
        "app-light": {"tile": "#F5F7FB"},
        "mark-mono-dark": {"monochrome": "#111827"},
        "mark-mono-light": {"monochrome": "#FFFFFF"},
    }.items():
        (BRAND / f"{name}.svg").write_text(svg(**options), encoding="utf-8")
        raster(512, **options).save(BRAND / f"{name}.png")
    (ROOT / "ui/src/assets/brand-mark.svg").write_text(svg(), encoding="utf-8")
    (ICONS / "icon.svg").write_text(svg(tile="#111827"), encoding="utf-8")
    icon = raster(512, tile="#111827")
    icon.save(ICONS / "icon.png")
    icon.save(ICONS / "icon.ico", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
    (ICONS / "tray.rgba").write_bytes(raster(32, tile="#111827").tobytes("raw", "RGBA"))
    for size in (16, 20, 24, 32, 40, 48, 64, 128, 256):
        raster(size, tile="#111827").save(ICONS / f"{size}x{size}.png")
    font_path = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/seguisb.ttf"
    font = ImageFont.truetype(str(font_path), 152) if font_path.exists() else None
    width = int(288 + (font.getlength("v8-chroma") / 2 if font else 410) + 24)
    mark = svg().split(">", 1)[1].rsplit("</svg>", 1)[0]
    for name, color in [("logo-dark", "#111827"), ("logo-light", "#FFFFFF")]:
        lockup = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 256">{mark}<text x="288" y="154" fill="{color}" font-family="Segoe UI,Arial,sans-serif" font-size="76" font-weight="600">v8-chroma</text></svg>\n'
        (BRAND / f"{name}.svg").write_text(lockup, encoding="utf-8")
        if font:
            image = Image.new("RGBA", (width * 2, 512))
            image.alpha_composite(raster(512))
            ImageDraw.Draw(image).text((576, 308), "v8-chroma", font=font, fill=color, anchor="ls")
            image.save(BRAND / f"{name}.png")
    print("Branding exported: vector masters, transparent PNGs, multi-size ICO, 32px tray RGBA")


if __name__ == "__main__":
    main()
