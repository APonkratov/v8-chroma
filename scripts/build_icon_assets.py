"""Build embedded, owned icon pixels without an installed 1C or Cairo runtime."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re

from icon_raster import raster

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/toolbar-icons"


def patches():
    rows = json.loads((ASSETS / "atlas-8.3.27.json").read_text(encoding="utf-8"))["sources"]
    manual = {}
    for name in ("platform", "tree", "menus", "editor", "formatting", "comparison", "sheet"):
        for e in json.loads((ASSETS / f"{name}-8.3.27.json").read_text(encoding="utf-8"))["sources"]:
            size = e.get("size", 16)
            row = dict(hash=e["hash"], width=size, height=size, x=0, y=0, size=size,
                       asset=f'{e["icon"]}{"-disabled" if e.get("disabled") else ""}-{size}.png',
                       preserve_bottom=e.get("preserve_bottom", 0))
            if e["hash"] in manual and row != manual[e["hash"]]:
                raise ValueError("Conflicting manual fingerprint")
            manual[e["hash"]] = row
    rows = [r for r in rows if r["hash"] not in manual] + list(manual.values())
    seen = {}
    for row in rows:
        if not re.fullmatch(r"[0-9a-f]{16}", row["hash"]):
            raise ValueError("Invalid fingerprint")
        w, h, x, y, size = (row[k] for k in ("width", "height", "x", "y", "size"))
        keep = row.setdefault("preserve_bottom", 0)
        if type(keep) is not int or keep not in (0, 4) or (keep and (w,h,x,y,size) != (16,16,0,0,16)):
            raise ValueError("Indicator preservation is limited to the bottom four rows of a 16px glyph")
        if any(type(n) is not int for n in (w, h, x, y, size)) or not (
                1 <= w <= 2048 and 1 <= h <= 64 and 1 <= size <= 64
                and 0 <= x <= w-size and 0 <= y <= h-size):
            raise ValueError("Invalid patch bounds")
        if not re.fullmatch(r"[a-z0-9-]+-\d+\.png", row["asset"]):
            raise ValueError("Invalid asset name")
        if int(row["asset"].removesuffix(".png").rsplit("-", 1)[1]) != size:
            raise ValueError("Artwork size differs from patch")
        if keep and row["asset"].removesuffix("-16.png").removesuffix("-disabled") not in (
                "format-fill-color", "format-font-color", "format-line-color"):
            raise ValueError("Only known formatting color controls have a dynamic swatch")
        key = (row["hash"], w, h, x, y)
        if key in seen and seen[key] != row:
            raise ValueError("Conflicting patch")
        seen[key] = row
    return sorted(seen.values(), key=lambda r: (r["hash"], r["x"], r["y"]))


def pixels(asset, style):
    stem, size = asset.removesuffix(".png").rsplit("-", 1)
    disabled = stem.endswith("-disabled")
    name = stem.removesuffix("-disabled")
    image = raster((ASSETS / style / f"{name}.svg").read_text(encoding="utf-8"), int(size))
    if disabled:
        opacity = .62 if style == "light" else .42
        image.putalpha(image.getchannel("A").point(lambda a: round(a * opacity)))
    return [(a << 24) | ((r*a+127)//255 << 16) | ((g*a+127)//255 << 8) | ((b*a+127)//255)
            for r, g, b, a in image.get_flattened_data()]


def build():
    rows = patches()
    names = sorted({r["asset"] for r in rows})
    out = ["// Generated from project-owned artwork; no platform pixels.\n#pragma once\n#include <stdint.h>\n",
           "namespace IconAssets {\nstruct Run { uint32_t count, pixel; };\n",
           "struct Asset { const Run* runs; unsigned count, size; };\n",
           "struct Patch { uint64_t hash; unsigned w,h,x,y,size,asset,preserveBottom; };\n"]
    for style in ("light", "dark"):
        for i, name in enumerate(names):
            runs = [(sum(1 for _ in group), p) for p, group in itertools.groupby(pixels(name, style))]
            out.append(f"inline constexpr Run {style}{i}[]={{" +
                       ",".join(f"{{{n},0x{p:08x}u}}" for n, p in runs) + "};\n")
        out.append(f"inline constexpr Asset {style}[]={{\n")
        for i, name in enumerate(names):
            size = int(name.removesuffix(".png").rsplit("-", 1)[1])
            out.append(f"{{{style}{i},sizeof({style}{i})/sizeof(Run),{size}}},\n")
        out.append("};\n")
    ids = {name: i for i, name in enumerate(names)}
    out.append("inline constexpr Patch patches[]={\n")
    for r in rows:
        out.append("{0x" + r["hash"] + "ull," + ",".join(str(r[k]) for k in ("width", "height", "x", "y", "size")) + f',{ids[r["asset"]]},{r["preserve_bottom"]}' + "},\n")
    out.append("};\ninline constexpr unsigned version=1;\n}\n")
    return "".join(out).encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--fixture", action="store_true", help="Owned synthetic pixels for native tests only")
    args = parser.parse_args()
    content = fixture() if args.fixture else build()
    if args.check:
        if args.output.read_bytes() != content:
            raise ValueError("Embedded icon assets differ")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(content)
    label = "Synthetic native fixture" if args.fixture else f"Owned icon assets: {len(patches())} patches"
    print(f"{label}, sha256={hashlib.sha256(content).hexdigest()}")


def fixture():
    from icon_raster import fingerprint, SIZES
    out = ["#pragma once\n#include <stdint.h>\nnamespace IconAssets {\n",
           "struct Run { uint32_t count,pixel; }; struct Asset { const Run* runs; unsigned count,size; };\n",
           "struct Patch { uint64_t hash; unsigned w,h,x,y,size,asset,preserveBottom; };\n"]
    for size in SIZES:
        out.append(f"inline constexpr Run l{size}[]={{{{{size*size},0xff44cc88u}}}};\n")
        out.append(f"inline constexpr Run d{size}[]={{{{{size*size},0xff4488ccu}}}};\n")
    for name, prefix in [("light", "l"), ("dark", "d")]:
        out.append(f"inline constexpr Asset {name}[]={{" + ",".join(f"{{{prefix}{s},1,{s}}}" for s in SIZES) + "};\n")
    out.append("inline constexpr Patch patches[]={\n")
    for i, size in enumerate(SIZES):
        for pixel in (bytes((68,34,204,255)), bytes((255,255,255,255))):
            source = pixel*(size*size*3-1)+bytes(4)
            out.append(f"{{0x{fingerprint(source,size*3,size)}ull,{size*3},{size},{size},0,{size},{i},0}},\n")
    # Synthetic dynamic-color control: match the glyph exactly, retain its swatch.
    normalized = bytes((68,34,204,255))*(16*12)+bytes(16*4*4)
    out.append(f"{{0x{fingerprint(normalized,16,16)}ull,16,16,0,0,16,1,4}},\n")
    out.append("};\n}\n")
    return "".join(out).encode("ascii")


if __name__ == "__main__":
    main()
