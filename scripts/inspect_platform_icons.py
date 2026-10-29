"""Read PNG resources from installed 1C files into a local, ignored evidence folder.

Only complete PNG streams with valid chunk CRCs are accepted. Platform resources
are diagnostic input, never checked in or included in the application package.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zlib

from PIL import Image, ImageDraw, ImageFont


def zip_entries(data):
    """Inspect embedded ZIP local entries without executing or unpacking paths."""
    position = 0
    while (start := data.find(b"PK\x03\x04", position)) >= 0:
        position = start + 4
        try:
            _, flags, method, _, _, crc, packed, size, names, extra = struct.unpack_from("<5H3I2H", data, start + 4)
            begin = start + 30 + names + extra
            if flags & 9 or size > 64 * 1024 * 1024 or begin + packed > len(data):
                continue
            name = data[start + 30:start + 30 + names].decode("utf-8", errors="replace")
            blob = data[begin:begin + packed]
            if method == 8:
                blob = zlib.decompress(blob, -15)
            elif method != 0:
                continue
            if len(blob) == size and zlib.crc32(blob) & 0xffffffff == crc:
                yield start, name, blob
                position = begin + packed
        except (struct.error, zlib.error):
            continue


def pngs(data):
    position = 0
    while (start := data.find(b"\x89PNG\r\n\x1a\n", position)) >= 0:
        position = start + 8
        cursor = position
        try:
            while True:
                length = struct.unpack_from(">I", data, cursor)[0]
                end = cursor + length + 12
                if end > len(data) or length > 64 * 1024 * 1024:
                    break
                kind = data[cursor + 4:cursor + 8]
                expected = struct.unpack_from(">I", data, end - 4)[0]
                if zlib.crc32(data[cursor + 4:end - 4]) & 0xffffffff != expected:
                    break
                cursor = end
                if kind == b"IEND":
                    yield start, data[start:end]
                    position = end
                    break
        except struct.error:
            pass


def inspect(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    data = source.read_bytes()
    records = []
    thumbnails = []
    strips = []
    position = 0
    while (start := data.find(b"BM", position)) >= 0:
        position = start + 2
        if start + 54 > len(data) or data[start+6:start+10] != b"\0" * 4:
            continue
        size = struct.unpack_from("<I", data, start+2)[0]
        if size < 54 or start + size > len(data):
            continue
        try:
            with Image.open(io.BytesIO(data[start:start+size])) as opened:
                image = opened.convert("RGBA")
            if image.width >= 128 and image.height <= 64:
                name = f"bmp-{start:08x}-{image.width}x{image.height}.png"
                image.save(destination / name)
                strips.append((name, image))
        except (OSError, ValueError):
            continue
    embedded = []
    for offset, name, blob in zip_entries(data):
        if name.lower().endswith((".png", ".svg", ".xml")):
            filename = f"zip-{offset:08x}-{Path(name).name}"
            (destination / filename).write_bytes(blob)
            embedded.append(dict(file=filename, name=name, offset=offset))
    (destination / "zip-entries.json").write_text(json.dumps(embedded, indent=2), encoding="utf-8")
    for index, (offset, png) in enumerate(pngs(data)):
        with Image.open(io.BytesIO(png)) as opened:
            image = opened.convert("RGBA")
        name = f"{index:04d}-{image.width}x{image.height}.png"
        (destination / name).write_bytes(png)
        records.append(dict(file=name, offset=offset, width=image.width, height=image.height,
                            sha256=hashlib.sha256(png).hexdigest()))
        if image.width >= 128 and image.height <= 64:
            strips.append((name, image))
        if image.width <= 128 and image.height <= 128:
            thumbnails.append((index, image))
    (destination / "manifest.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    font = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 12)
    for start in range(0, len(thumbnails), 180):
        batch = thumbnails[start:start + 180]
        sheet = Image.new("RGB", (1000, ((len(batch) + 9) // 10) * 72), "#303036")
        draw = ImageDraw.Draw(sheet)
        for cell, (index, image) in enumerate(batch):
            x, y = cell % 10 * 100, cell // 10 * 72
            preview = image.copy()
            preview.thumbnail((60, 42))
            if image.width <= 24 and image.height <= 24:
                preview = image.resize((image.width * 2, image.height * 2), Image.Resampling.NEAREST)
            sheet.paste(preview, (x + (100-preview.width)//2, y + 2), preview)
            draw.text((x + 8, y + 51), f"{index:04d} {image.width}x{image.height}", fill="#dddde8", font=font)
        sheet.save(destination / f"sheet-{start // 180}.png")
    strip_sheet = Image.new("RGB", (1800, max(1, len(strips)) * 90), "#303036")
    draw = ImageDraw.Draw(strip_sheet)
    for row, (name, image) in enumerate(strips):
        draw.text((10, row * 90), name, fill="white", font=font)
        preview = image.copy()
        preview.thumbnail((1780, 60))
        if image.height <= 24 and image.width <= 880:
            preview = image.resize((image.width*2, image.height*2), Image.Resampling.NEAREST)
        strip_sheet.paste(preview, (10, row*90+22), preview)
    strip_sheet.save(destination / "strips.png")
    print(f"{source.name}: {len(records)} raw PNGs, {len(embedded)} ZIP entries -> {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("evidence/platform-icons"))
    args = parser.parse_args()
    for source in args.source:
        inspect(source, args.output / source.stem)


if __name__ == "__main__":
    main()
