"""Exercise the actual prototype IAT hook using owned images and installed Cairo."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"scripts"))
from build_toolbar_prototype import SIZES, load_cairo, cairo_pixels, fingerprint

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cairo",type=Path,required=True)
parser.add_argument("--build",type=Path,required=True)
args=parser.parse_args()
cairo=load_cairo(args.cairo.resolve())
with tempfile.TemporaryDirectory(prefix="v8-icons-test-") as temporary:
    pack=Path(temporary)
    lines=[]
    for size in SIZES:
        source=Image.new("RGBA",(size*3,size),(204,34,68,255))
        source.putpixel((size*3-1,size-1),(0,0,0,0))
        path=pack/f"source-{size}.png"
        source.save(path)
        target=Image.new("RGBA",(size,size),(68,204,136,255))
        target.putpixel((size-1,size-1),(0,0,0,0))
        target.save(pack/f"target-{size}.png")
        w,h,pixels=cairo_pixels(cairo,path)
        lines.append(f"{fingerprint(pixels,w,h)} {w} {h} {size} 0 {size} target-{size}.png")
        # A8 glyph masks use the same canonical premultiplied pixels as white RGBA.
        white=Image.new("RGBA",(size*3,size),(255,255,255,255))
        white.putpixel((size*3-1,size-1),(0,0,0,0))
        white.save(pack/f"mask-{size}.png")
        w,h,pixels=cairo_pixels(cairo,pack/f"mask-{size}.png")
        lines.append(f"{fingerprint(pixels,w,h)} {w} {h} {size} 0 {size} target-{size}.png")
    (pack/"pack.tsv").write_text("\n".join(lines)+"\n",encoding="ascii")
    (pack/"invalid").mkdir()
    (pack/"invalid/pack.tsv").write_text("0000000000000000 16 16 0 0 16 ../outside.png\n",encoding="ascii")
    subprocess.run([str((args.build/"bin/prototype-fixtures/toolbar_icons_test.exe").resolve()),
                    str(args.cairo.resolve()),str((args.build/"bin/prototype/ToolbarIcons.dll").resolve()),str(pack)],check=True)
