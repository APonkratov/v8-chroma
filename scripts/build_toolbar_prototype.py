"""Build an opt-in icon pack from original fingerprints and project-owned vectors.

The pack contains only new artwork and fingerprints. Original 1C pixels stay in
the installed platform and in ignored, local QA evidence.
"""
import argparse
import ctypes as C
import io
import json
from pathlib import Path
import struct
import tempfile
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont

from inspect_platform_icons import zip_entries
from metadata_icons import GEOMETRY as METADATA_GEOMETRY, ICON_ROLES as METADATA_ROLES
from menu_icons import GEOMETRY as MENU_GEOMETRY, ICON_ROLES as MENU_ROLES
from editor_icons import GEOMETRY as EDITOR_GEOMETRY, ICON_ROLES as EDITOR_ROLES

ROOT = Path(__file__).resolve().parents[1]
SIZES = (14, 16, 20, 24, 28, 32, 48, 64)
RASTER_SIZES = tuple(sorted(set(SIZES) | {8,9,12,15}))
FILE_ICONS = {0: "new", 1: "open", 3: "save", 4: "save-all", 5: "print", 7: "preview", 8: "global-search"}
EDIT_ICONS = {0: "search", 1:"search-next", 2:"search-previous", 5: "cut", 6: "copy", 7: "paste", 8: "undo", 9: "redo", 17: "search"}
GEOMETRY = {
    "new": '<polyline points="9.5,1.8 3.2,1.8 3.2,14.2 12.8,14.2 12.8,5.2 9.5,1.8 9.5,5.2 12.8,5.2"/><line x1="5.5" y1="8" x2="10.5" y2="8"/><line x1="5.5" y1="10.5" x2="9" y2="10.5"/>',
    "open": '<polyline points="1.8,6 1.8,3.5 6.2,3.5 7.6,5 13.8,5 13.8,7"/><polygon points="3.2,7 14.2,7 12.5,13 1.8,13"/>',
    "save": '<polyline points="11.5,2 2.5,2 2.5,14 13.5,14 13.5,4 11.5,2"/><rect x="5" y="2" width="5.5" height="4"/><rect x="5" y="9" width="6" height="5"/>',
    "save-all": '<polyline points="1.5,5 1.5,14.5 10.5,14.5"/><polyline points="11.5,1.5 4,1.5 4,12 14,12 14,4 11.5,1.5"/><rect x="6.5" y="1.5" width="4.5" height="3.5"/><rect x="6.5" y="8" width="5" height="4"/>',
    "print": '<polyline points="4,5 4,2 12,2 12,5"/><polyline points="4,11.5 1.8,11.5 1.8,5.8 14.2,5.8 14.2,11.5 12,11.5"/><rect x="4" y="9" width="8" height="5"/><line x1="11.5" y1="7.5" x2="12" y2="7.5"/>',
    "search": '<ellipse cx="6.8" cy="6.8" rx="4.4" ry="4.4"/><line x1="10.2" y1="10.2" x2="14" y2="14"/>',
    "copy": '<polyline points="5.5,10.5 2,10.5 2,2 10.5,2 10.5,5.5"/><rect x="5.5" y="5.5" width="8.5" height="8.5"/>',
    "paste": '<polyline points="5,3 2.8,3 2.8,14 13.2,14 13.2,3 11,3"/><rect x="5" y="1.8" width="6" height="3"/><line x1="5.5" y1="8" x2="10.5" y2="8"/><line x1="5.5" y1="10.5" x2="9.5" y2="10.5"/>',
    "cut": '<ellipse cx="4" cy="11.7" rx="2.1" ry="2.1"/><ellipse cx="12" cy="11.7" rx="2.1" ry="2.1"/><line x1="5.4" y1="10.1" x2="12.8" y2="2"/><line x1="10.6" y1="10.1" x2="3.2" y2="2"/>',
    "undo": '<polyline points="5.5,2.8 2,6.3 5.5,9.8"/><polyline points="2,6.3 9,6.3 12,7.3 13.5,9.5 13.5,13"/>',
    "redo": '<polyline points="10.5,2.8 14,6.3 10.5,9.8"/><polyline points="14,6.3 7,6.3 4,7.3 2.5,9.5 2.5,13"/>',
}
GEOMETRY.update({
    "preview": '<polyline points="8,1.8 2.3,1.8 2.3,14 8,14"/><polyline points="8,1.8 11,4.8 8,4.8 8,1.8"/><ellipse cx="10" cy="9" rx="3.2" ry="3.2"/><line x1="12.3" y1="11.3" x2="14.5" y2="14"/>',
    "global-search": '<polyline points="1.8,13 1.8,3.5 6,3.5 7.5,5 14,5 14,7"/><ellipse cx="8.8" cy="10" rx="3.2" ry="3.2"/><line x1="11.3" y1="12.3" x2="14" y2="14.5"/>',
    "search-next": '<ellipse cx="6" cy="6" rx="3.8" ry="3.8"/><line x1="9" y1="9" x2="11" y2="11"/><polyline points="10.5,10 14,10 14,14"/><polyline points="11.5,12 14,14 16,12"/>',
    "search-previous": '<ellipse cx="6" cy="8.5" rx="3.8" ry="3.8"/><line x1="9" y1="11.5" x2="11" y2="14"/><polyline points="10.5,5 14,5 14,1"/><polyline points="11.5,3 14,1 16,3"/>',
    "windows": '<rect x="1.5" y="1.8" width="10" height="9"/><polyline points="5,12.5 5,14 14.5,14 14.5,5 13,5"/><line x1="2" y1="4.2" x2="11" y2="4.2"/>',
    "syntax-help": '<polyline points="7.5,3 4.3,2 1.5,3 1.5,13 4.3,12 7.5,13 7.5,3 10.7,2 14.5,3 14.5,13 10.7,12 7.5,13"/><polyline points="5.3,5.5 3.5,7.3 5.3,9.1"/><polyline points="10.5,5.5 12.3,7.3 10.5,9.1"/>',
    "syntax-search": '<polyline points="6,3 3.7,2.2 1.5,3 1.5,12.8 3.7,12 6,12.8 6,3 9,2.2 12,3 12,5"/><ellipse cx="10.5" cy="9.5" rx="3.2" ry="3.2"/><line x1="12.7" y1="11.7" x2="14.8" y2="14.5"/>',
    "templates": '<rect x="2.2" y="1.8" width="11.5" height="12.4"/><line x1="4.6" y1="5" x2="7" y2="5"/><line x1="9" y1="5" x2="11.3" y2="5"/><polyline points="6,8 4.3,9.8 6,11.5"/><polyline points="10,8 11.7,9.8 10,11.5"/>',
    "about": '<ellipse cx="8" cy="8" rx="6" ry="6"/><line x1="8" y1="7.2" x2="8" y2="11.7"/><line x1="8" y1="4.6" x2="8" y2="4.8"/>',
    "repository": '<ellipse cx="8" cy="3.5" rx="5.5" ry="2"/><polyline points="2.5,3.5 2.5,12 5,14 11,14 13.5,12 13.5,3.5"/><polyline points="2.5,7 5,9 11,9 13.5,7"/>',
    "repository-history": '<polyline points="2.5,10 2.5,3 5,1.5 10,1.5 12.5,3 12.5,5 10,6.2 5,6.2 2.5,5"/><ellipse cx="10" cy="10.5" rx="4.2" ry="4.2"/><polyline points="10,8 10,10.5 12,11.5"/>',
    "repository-refresh": '<polyline points="12.5,5.5 10.5,2.5 5.5,2.5 2.5,5.5 2.5,8"/><polyline points="12.5,1.8 12.5,5.5 8.8,5.5"/><polyline points="3.5,10.5 5.5,13.5 10.5,13.5 13.5,10.5 13.5,8"/><polyline points="3.5,14.2 3.5,10.5 7.2,10.5"/>',
    "repository-update": '<polyline points="2,3 4,1.5 9.5,1.5 11.5,3 11.5,6 8.5,7.2 4,7.2 2,6 2,11.5 4,13 7,13"/><polyline points="12.2,7.5 12.2,14.5 9.7,12"/><line x1="12.2" y1="14.5" x2="14.7" y2="12"/>',
    "repository-settings": '<polyline points="2,12.8 2,3 4.5,1.5 9.5,1.5 12,3 12,5"/><polyline points="2,5 4.5,6.5 8,6.5"/><ellipse cx="10" cy="10.5" rx="3.2" ry="3.2"/><ellipse cx="10" cy="10.5" rx="1" ry="1"/><line x1="10" y1="6" x2="10" y2="7.3"/><line x1="10" y1="13.7" x2="10" y2="15"/><line x1="5.5" y1="10.5" x2="6.8" y2="10.5"/><line x1="13.2" y1="10.5" x2="14.5" y2="10.5"/>',
    "configuration-open": '<polyline points="1.8,6 1.8,3.5 6,3.5 7.5,5 13.8,5 13.8,7"/><polygon points="3,7 14.5,7 12.5,13.5 1.5,13.5"/><line x1="8" y1="8.7" x2="8" y2="11.7"/><line x1="6.5" y1="10.2" x2="9.5" y2="10.2"/>',
    "configuration-close": '<polyline points="2,13.5 2,3.5 6.5,3.5 8,5 14,5 14,13.5 2,13.5"/><line x1="6.5" y1="7.5" x2="10.5" y2="11.5"/><line x1="10.5" y1="7.5" x2="6.5" y2="11.5"/>',
    "database-update": '<ellipse cx="6" cy="3.5" rx="4.2" ry="2"/><polyline points="1.8,3.5 1.8,12 4,13.5 7.5,13.5"/><line x1="10.2" y1="3.5" x2="10.2" y2="6.5"/><polyline points="13,7.3 13,14.5 10.5,12"/><line x1="13" y1="14.5" x2="15.2" y2="12"/>',
    "configuration-window": '<rect x="1.8" y="1.8" width="12.4" height="12.4"/><line x1="2" y1="5" x2="14" y2="5"/><line x1="6" y1="5" x2="6" y2="14"/><line x1="8.5" y1="8" x2="12" y2="8"/><line x1="8.5" y1="11" x2="11" y2="11"/>',
    "debug": '<rect x="5" y="4" width="6" height="9"/><polyline points="6.5,3.5 5.2,1.8"/><polyline points="9.5,3.5 10.8,1.8"/><line x1="2" y1="6" x2="5" y2="6"/><line x1="11" y1="6" x2="14" y2="6"/><line x1="2" y1="10" x2="5" y2="10"/><line x1="11" y1="10" x2="14" y2="10"/><line x1="8" y1="4.5" x2="8" y2="12.5"/>',
    "run": '<polygon points="5,2.2 13.5,8 5,13.8"/>',
    "clear": '<line x1="4.5" y1="4.5" x2="11.5" y2="11.5"/><line x1="11.5" y1="4.5" x2="4.5" y2="11.5"/>',
    "chevron-down": '<polyline points="4,6 8,10 12,6"/>',
    "chevron-right": '<polyline points="6,4 10,8 6,12"/>',
    "add": '<ellipse cx="8" cy="8" rx="6" ry="6"/><line x1="8" y1="4.8" x2="8" y2="11.2"/><line x1="4.8" y1="8" x2="11.2" y2="8"/>',
    "edit": '<polygon points="3,10 10.5,2.5 13.5,5.5 6,13 2,14"/><line x1="8.8" y1="4.2" x2="11.8" y2="7.2"/>',
    "duplicate": '<polyline points="5.5,10.5 2,10.5 2,2 10.5,2 10.5,5.5"/><rect x="5.5" y="5.5" width="8.5" height="8.5"/><line x1="9.7" y1="7.5" x2="9.7" y2="12"/><line x1="7.5" y1="9.7" x2="12" y2="9.7"/>',
    "delete": '<polyline points="2.5,4.5 13.5,4.5"/><polyline points="4,4.5 4.5,14 11.5,14 12,4.5"/><polyline points="6,4 6,2 10,2 10,4"/><line x1="6.5" y1="7" x2="6.8" y2="11.5"/><line x1="9.5" y1="7" x2="9.2" y2="11.5"/>',
    "move-up": '<polyline points="3,8 8,3 13,8"/><line x1="8" y1="3" x2="8" y2="14"/>',
    "move-down": '<polyline points="3,8 8,13 13,8"/><line x1="8" y1="2" x2="8" y2="13"/>',
    "sort": '<line x1="2" y1="3" x2="10" y2="3"/><line x1="2" y1="6" x2="8" y2="6"/><line x1="2" y1="9" x2="6" y2="9"/><line x1="2" y1="12" x2="4" y2="12"/><polyline points="12,5 12,14 9.5,11.5"/><line x1="12" y1="14" x2="14.5" y2="11.5"/>',
    "subsystems": '<polygon points="1.5,2.5 14.5,2.5 9.5,8 9.5,13.5 6.5,12 6.5,8"/><line x1="4.2" y1="5" x2="11.8" y2="5"/>',
    "subsystems-support": '<polyline points="4.5,10 4.5,7 1.5,2.5 11,2.5 8,7 8,12.5 6.5,12"/><line x1="11.5" y1="6" x2="14.5" y2="6"/><line x1="11.5" y1="9.5" x2="14.5" y2="9.5"/><line x1="11.5" y1="13" x2="14.5" y2="13"/><rect x="1.5" y="11" width="4" height="3.5"/><polyline points="2.3,11 2.3,9.3 4.7,9.3 4.7,11"/>',
})
GEOMETRY.update(METADATA_GEOMETRY)
GEOMETRY.update(MENU_GEOMETRY)
GEOMETRY.update(EDITOR_GEOMETRY)


def svg(name, color, stroke=1.25):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="{color}" stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round">{GEOMETRY[name]}</svg>\n'


def raster(vector, size):
    scale = size * 4 / 16
    root = ET.fromstring(vector)
    image = Image.new("RGBA", (size*4, size*4))
    draw = ImageDraw.Draw(image)
    color = root.attrib["stroke"]
    stroke = max(1, round(float(root.attrib["stroke-width"]) * scale))
    for element in root:
        kind = element.tag.split("}")[-1]
        values = element.attrib
        def number(key):
            return float(values[key]) * scale
        if kind in ("polyline", "polygon"):
            points = [tuple(float(n)*scale for n in point.split(",")) for point in values["points"].split()]
            if kind == "polygon":
                points.append(points[0])
            draw.line(points, fill=color, width=stroke, joint="curve")
        elif kind == "line":
            points = [(number("x1"), number("y1")), (number("x2"), number("y2"))]
            draw.line(points, fill=color, width=stroke)
        elif kind == "rect":
            x, y = number("x"), number("y")
            draw.rectangle((x, y, x+number("width"), y+number("height")), outline=color, width=stroke)
            points = []
        elif kind == "ellipse":
            x, y, rx, ry = number("cx"), number("cy"), number("rx"), number("ry")
            draw.ellipse((x-rx,y-ry,x+rx,y+ry), outline=color, width=stroke)
            points = []
        else:
            raise ValueError(kind)
        for x, y in points:
            r = stroke/2
            draw.ellipse((x-r,y-r,x+r,y+r), fill=color)
    return image.resize((size,size), Image.Resampling.LANCZOS)


def fingerprint(pixels, width, height):
    value = 14695981039346656037
    for byte in struct.pack("<II",width,height) + pixels:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def cairo_pixels(cairo, path):
    surface = cairo.cairo_image_surface_create_from_png(str(path).encode("utf-8"))
    if cairo.cairo_surface_status(surface):
        raise ValueError(f"Cairo cannot read {path}")
    try:
        w, h = cairo.cairo_image_surface_get_width(surface), cairo.cairo_image_surface_get_height(surface)
        pitch = cairo.cairo_image_surface_get_stride(surface)
        cairo.cairo_surface_flush(surface)
        pointer = cairo.cairo_image_surface_get_data(surface)
        raw = C.string_at(pointer,pitch*h)
        pixels = b"".join(raw[y*pitch:y*pitch+w*4] for y in range(h))
        return w,h,pixels
    finally:
        cairo.cairo_surface_destroy(surface)


def load_cairo(path):
    import os
    global dll_directory
    dll_directory = os.add_dll_directory(str(path.parent))
    cairo = C.CDLL(str(path))
    cairo.cairo_image_surface_create_from_png.argtypes = [C.c_char_p]
    cairo.cairo_image_surface_create_from_png.restype = C.c_void_p
    for name in ("cairo_surface_status", "cairo_image_surface_get_width", "cairo_image_surface_get_height", "cairo_image_surface_get_stride"):
        getattr(cairo,name).argtypes = [C.c_void_p]
        getattr(cairo,name).restype = C.c_int
    for name in ("cairo_surface_flush", "cairo_surface_destroy"):
        getattr(cairo,name).argtypes = [C.c_void_p]
        getattr(cairo,name).restype = None
    cairo.cairo_image_surface_get_data.argtypes = [C.c_void_p]
    cairo.cairo_image_surface_get_data.restype = C.c_void_p
    return cairo


def icon_sources():
    sources={}
    for name in ("platform-8.3.27.json","tree-8.3.27.json","menus-8.3.27.json","editor-8.3.27.json"):
        profile=json.loads((ROOT/"assets/toolbar-icons"/name).read_text(encoding="utf-8"))
        for entry in profile["sources"]:
            key=entry["hash"]
            size=entry.get("size",16)
            if len(key)!=16 or any(c not in "0123456789abcdef" for c in key):
                raise ValueError(f"Invalid image fingerprint in {name}: {key}")
            if entry["icon"] not in GEOMETRY or size not in RASTER_SIZES:
                raise ValueError(f"Missing icon artwork or unsupported size: {entry}")
            identity=(entry["icon"],bool(entry.get("disabled")),size)
            if key in sources:
                previous=sources[key]
                if identity!=(previous["icon"],bool(previous.get("disabled")),previous.get("size",16)):
                    raise ValueError(f"Conflicting replacements for {key}")
            sources[key]=entry
    return list(sources.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", type=Path, required=True, help="Installed platform bin directory")
    parser.add_argument("--output", type=Path, default=ROOT/"work/toolbar-icons/dark")
    parser.add_argument("--style", choices=("dark", "light"), default="dark")
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    masters = ROOT/"assets/toolbar-icons"/args.style
    masters.mkdir(parents=True,exist_ok=True)
    colors = {"dark": {"ink":"#dce4f2", "blue":"#85b6ff", "yellow":"#f2ca74", "green":"#98d8a8", "teal":"#74d4c2", "purple":"#c5a4fa", "pink":"#e7a4cc", "red":"#f28b82"},
              "light": {"ink":"#364254", "blue":"#2867b7", "yellow":"#986b13", "green":"#26783e", "teal":"#15766d", "purple":"#7750ad", "pink":"#a84f83", "red":"#b83232"}}[args.style]
    for name in GEOMETRY:
        role = "yellow" if name in ("open","configuration-open","global-search") else "blue" if name in ("save","save-all") or name.startswith("repository") else "green" if name in ("undo","redo","debug","run") else "ink"
        role=METADATA_ROLES.get(name,role)
        role=MENU_ROLES.get(name,role)
        role=EDITOR_ROLES.get(name,role)
        vector = svg(name,colors[role])
        (masters/f"{name}.svg").write_text(vector,encoding="utf-8")
        for size in RASTER_SIZES:
            image=raster(vector,size)
            image.save(args.output/f"{name}-{size}.png")
            opacity=.62 if args.style=="light" else .42
            image.putalpha(image.getchannel("A").point(lambda a:round(a*opacity)))
            image.save(args.output/f"{name}-disabled-{size}.png")
    cairo = load_cairo(args.platform/"cairo.dll")
    lines = set()
    resource = args.platform/"core83_root.res"
    if not resource.exists():
        resource = args.platform/"core85_root.res"
    original = {}
    small_file_index = 0
    # Legacy single-image ZIPs precede the multi-DPI atlas in core83/core85.
    small_file_icons = ("save", "open", "new", "preview", "global-search", "print", "print")
    with tempfile.TemporaryDirectory(prefix="v8-icons-") as temporary:
        for _,name,blob in zip_entries(resource.read_bytes()):
            mapping = FILE_ICONS if name.startswith("tbglfile") else EDIT_ICONS if name.startswith("tbgledit") and "ltr" in name else None
            if mapping is None or "old" in name or not name.endswith(".png"):
                continue
            path = Path(temporary)/"source.png"
            path.write_bytes(blob)
            w,h,pixels = cairo_pixels(cairo,path)
            if name=="tbglfile.png" and (w,h)==(16,16):
                replacement=small_file_icons[small_file_index]
                small_file_index+=1
                mapping={0:replacement}
            elif h not in SIZES or w != h * (9 if mapping is FILE_ICONS else 18):
                continue
            if h==16 and name in ("tbglfile_100.png", "tbgledit_100_ltr.png"):
                original[name] = Image.open(io.BytesIO(blob)).convert("RGBA")
            straight = Image.open(io.BytesIO(blob)).convert("RGBA").tobytes()
            # 1C truncates premultiplication; Cairo's own PNG decoder rounds it.
            floor = b"".join(bytes((b*a//255,g*a//255,r*a//255,a)) for r,g,b,a in zip(*[iter(straight)]*4))
            for variant in (pixels,floor):
                for cell, replacement in mapping.items():
                    lines.add(f"{fingerprint(variant,w,h)} {w} {h} {cell*h} 0 {h} {replacement}-{h}.png")
                    crop = b"".join(variant[(y*w+cell*h)*4:(y*w+(cell+1)*h)*4] for y in range(h))
                    lines.add(f"{fingerprint(crop,h,h)} {h} {h} 0 0 {h} {replacement}-{h}.png")
    overrides=[]
    for entry in icon_sources():
        size=entry.get("size",16)
        variant="-disabled" if entry.get("disabled") else ""
        overrides.append(f'{entry["hash"]} {size} {size} 0 0 {size} {entry["icon"]}{variant}-{size}.png')
    overridden={line.split()[0] for line in overrides}
    lines={line for line in lines if line.split()[0] not in overridden} | set(overrides)
    (args.output/"pack.tsv").write_text("# fnv1a64 width height x y size replacement-png\n"+"\n".join(sorted(lines))+"\n",encoding="ascii")
    preview = Image.new("RGB",(1200,260),"#303036" if args.style=="dark" else "#f1f3f7")
    draw = ImageDraw.Draw(preview)
    font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf",18)
    text = "#dce4f2" if args.style=="dark" else "#364254"
    for row, (name,mapping) in enumerate((("tbglfile_100.png",FILE_ICONS),("tbgledit_100_ltr.png",EDIT_ICONS))):
        old = original[name]
        new = old.copy()
        for cell, replacement in mapping.items():
            new.paste(Image.open(args.output/f"{replacement}-16.png"),(cell*16,0))
        draw.text((20,row*125+8),"Штатные",font=font,fill=text)
        draw.text((20,row*125+64),"Прототип",font=font,fill=text)
        for y,strip in ((row*125+8,old),(row*125+64,new)):
            enlarged = strip.resize((strip.width*3,48),Image.Resampling.NEAREST)
            preview.paste(enlarged,(160,y),enlarged)
    evidence = ROOT/"evidence/toolbar-icons"
    evidence.mkdir(parents=True,exist_ok=True)
    preview.save(evidence/f"preview-{args.style}.png")
    print(f"Generated {len(lines)} exact patches, {len(GEOMETRY)} icons, {len(SIZES)} sizes: {args.output}")


if __name__ == "__main__":
    main()
