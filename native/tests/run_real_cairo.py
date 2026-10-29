"""Test the production DLL's scoped IAT hooks with the installed real Cairo, without UI."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--cairo', type=Path, default=Path(r'C:\Program Files\1cv8\8.3.27.2325\bin\cairo.dll'))
args = parser.parse_args()
fixtures = (ROOT/'native/bin/real').resolve()
evidence = ROOT/'evidence/selection'
evidence.mkdir(parents=True, exist_ok=True)

def wide(path, capacity):
    value = str(path).encode('utf-16le')
    if len(value) >= capacity*2:
        raise ValueError('Path exceeds native protocol capacity')
    return value.ljust(capacity*2, b'\0')

with tempfile.TemporaryDirectory(prefix='theme-real-cairo-') as temporary:
    directory = Path(temporary)
    log = directory/'colors.csv'
    configs = []
    for source in sorted((ROOT/'tests/golden').glob('*.bin')):
        cfg = bytearray(source.read_bytes())
        assert len(cfg) == 4760 and struct.unpack_from('<II',cfg) == (4760,3)
        cfg[120:640] = wide(fixtures,260)
        cfg[640:1680] = wide(log,520)
        path = directory/source.name
        path.write_bytes(cfg)
        configs.append(path)
    cfg = bytearray(4760)
    struct.pack_into('<II',cfg,0,4760,3)
    cfg[16:26] = b'diagnostic'
    cfg[120:640] = wide(fixtures,260)
    cfg[640:1680] = wide(log,520)
    path = directory/'diagnostic.bin'
    path.write_bytes(cfg)
    configs.append(path)
    result = subprocess.run([str(fixtures/'real_selection_host.exe'),str(args.cairo.resolve(strict=True)),
        str((ROOT/'native/bin/ThemeProbe.dll').resolve(strict=True)),*map(str,configs)],capture_output=True,text=True)
    report = result.stdout+result.stderr+f'\nExit code: {result.returncode}\n'
    (evidence/'real-cairo-production-iat.txt').write_text(report,encoding='utf-8')
    if log.exists():
        (evidence/'real-cairo-production-iat.csv').write_bytes(log.read_bytes())
    print(report)
    raise SystemExit(result.returncode)
