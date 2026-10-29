"""Development only: produce protocol fixtures using the local independent Python reference."""
import hashlib
import json
from pathlib import Path
import sys
import argparse
from dataclasses import replace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/reference"))
from protocol_reference import make_config
from theme_reference import load_theme, load_profile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true', help='Verify committed fixtures without changing them')
args = parser.parse_args()

report = []
cases = {}
for path in sorted((ROOT / 'assets/themes').glob('*.json')):
    theme = load_theme(path)
    # Independent reference for the internal compare/merge profile roles.
    def luminance(rgb):
        channels = [(rgb >> shift & 255) / 255 for shift in (16, 8, 0)]
        return sum(weight * (c/12.92 if c <= .04045 else ((c+.055)/1.055)**2.4)
                   for weight,c in zip((.2126,.7152,.0722),channels))
    def contrast(a,b):
        a,b=luminance(a),luminance(b)
        return (max(a,b)+.05)/(min(a,b)+.05)
    colors = dict(theme.colors)
    for role,tint in [('compare.changed',0x00B8C4),('compare.local',0xE99A49),('compare.file',0x55AA66)]:
        for amount in range(25,-1,-1):
            color = sum((((colors['editor.background'] >> shift & 255)*(100-amount)
                         +(tint >> shift & 255)*amount+50)//100) << shift for shift in (16,8,0))
            if contrast(color,colors['text']) >= 4.5 or amount==0:
                colors[role]=color
                break
    theme = replace(theme, colors=colors)
    mappings = load_profile(ROOT / 'assets/profile.properties', theme)
    cfg = make_config(theme, mappings, Path(r'C:\Platform'), Path(r'C:\Logs\colors.csv'))
    data = bytes(cfg)
    cases[theme.id] = data
    destination = ROOT / 'tests/golden' / (theme.id + '.bin')
    if args.check:
        if destination.read_bytes() != data:
            raise SystemExit(f'Protocol fixture differs: {theme.id}; regenerate intentionally')
    else:
        destination.write_bytes(data)
    report.append({'theme': theme.id, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
for name, base, mode in [('icons-dark','dracula',2),('icons-light','github-light-default',1),('icons-standard',None,1),('diagnostic',None,0)]:
    import struct
    data = bytearray(cases[base]) if base else make_config(None,[],Path(r'C:\Platform'),Path(r'C:\Logs\colors.csv'), identifier='standard' if mode else 'diagnostic')
    struct.pack_into('<II',data,4752,mode,int(mode!=0))
    data=bytes(data)
    destination=ROOT/'tests/golden'/f'{name}.bin'
    if args.check:
        if destination.read_bytes()!=data: raise SystemExit(f'Protocol fixture differs: {name}')
    else: destination.write_bytes(data)
    report.append({'theme':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
manifest = ROOT / 'tests/golden/manifest.json'
if args.check:
    if json.loads(manifest.read_text(encoding='utf-8')) != report:
        raise SystemExit('Protocol fixture manifest differs')
else:
    manifest.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'{"Verified" if args.check else "Generated"} {len(report)} independent protocol fixtures')
