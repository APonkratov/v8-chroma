"""Version/tag gates and PE dependency checks for Windows x64 releases."""
import argparse
import json
from pathlib import Path
import re
import struct
import tomllib

ROOT = Path(__file__).resolve().parents[1]
NUMBER = r"(?:0|[1-9][0-9]*)"
TAG = re.compile(rf"v({NUMBER}\.{NUMBER}\.{NUMBER}(?:-rc\.[1-9][0-9]*)?)")
SYSTEM_DLLS = set("""advapi32 avrt bcrypt bcryptprimitives cfgmgr32 comctl32 comdlg32
crypt32 d2d1 d3d11 dbghelp dnsapi dwmapi dwrite dxgi gdi32 gdiplus hid imm32
iphlpapi kernel32 kernelbase msimg32 msvcrt ncrypt netapi32 normaliz ntdll ole32
oleacc oleaut32 opengl32 powrprof propsys psapi rpcrt4 secur32 setupapi shell32
shlwapi ucrtbase user32 userenv usp10 uxtheme version windowscodecs winhttp
wininet winmm winspool wintrust ws2_32 wtsapi32""".split())


def release_version(tag):
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError("Expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-rc.N (N >= 1)")
    return match[1]


def versions(root=ROOT, tag=None):
    cargo = tomllib.loads((root / "Cargo.toml").read_text(encoding="utf-8"))
    app = tomllib.loads((root / "ui/src-tauri/Cargo.toml").read_text(encoding="utf-8"))
    lock = tomllib.loads((root / "Cargo.lock").read_text(encoding="utf-8"))
    package = json.loads((root / "ui/package.json").read_text(encoding="utf-8"))
    npm_lock = json.loads((root / "ui/package-lock.json").read_text(encoding="utf-8"))
    tauri = json.loads((root / "ui/src-tauri/tauri.conf.json").read_text(encoding="utf-8"))
    values = [cargo["package"]["version"], app["package"]["version"], package["version"],
              npm_lock["version"], npm_lock["packages"][""]["version"], tauri["version"]]
    values += [p["version"] for p in lock["package"] if p["name"] in ("v8-chroma-core", "v8-chroma-app")]
    if len(set(values)) != 1:
        raise ValueError(f"Versions differ across manifests/locks: {values}")
    version = values[0]
    if tag and release_version(tag) != version:
        raise ValueError(f"Tag {tag} does not match manifest version {version}")
    return version


def pe_imports(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Invalid DOS header: {path}")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if machine != 0x8664 or struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError(f"Expected x64 PE32+: {path}")
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    headers_size = struct.unpack_from("<I", data, opt + 60)[0]
    directories = struct.unpack_from("<I", data, opt + 108)[0]
    sections = []
    for index in range(count):
        section = opt + optional_size + 40 * index
        size, rva, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
        sections.append((rva, max(size, raw_size), raw, raw_size))

    def offset(rva, size=1):
        if rva < headers_size and rva + size <= min(headers_size, len(data)):
            return rva
        for base, length, raw, raw_size in sections:
            delta = rva - base
            if 0 <= delta and delta + size <= min(length, raw_size) and raw + delta + size <= len(data):
                return raw + delta
        raise ValueError(f"Unmapped PE RVA {rva:x}: {path}")

    def string(rva):
        start = offset(rva)
        end = data.find(b"\0", start, min(start + 512, len(data)))
        if end < 0:
            raise ValueError(f"Unterminated DLL name: {path}")
        return data[start:end].decode("ascii").lower()

    imports = set()
    for index, width in ((1, 20), (13, 32)):
        if index >= directories or 112 + (index + 1) * 8 > optional_size:
            continue
        table, size = struct.unpack_from("<II", data, opt + 112 + index * 8)
        if not table:
            continue
        terminated = False
        for pos in range(0, size, width):
            descriptor = offset(table + pos, width)
            fields = struct.unpack_from("<" + "I" * (width // 4), data, descriptor)
            if not any(fields):
                terminated = True
                break
            name_rva = fields[3] if index == 1 else fields[1]
            if index == 13 and not fields[0] & 1:
                name_rva -= image_base
            imports.add(string(name_rva))
        if not terminated:
            raise ValueError(f"Unterminated PE import table: {path}")
    return sorted(imports)


def verify_binary(path):
    imports = pe_imports(path)
    unexpected = [name for name in imports if not (
        name.endswith(".dll") and (name[:-4] in SYSTEM_DLLS or name.startswith(("api-ms-win-", "ext-ms-win-"))))]
    if unexpected:
        raise ValueError(f"Non-system DLL dependencies in {path.name}: {unexpected}")
    return imports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--binary", type=Path, action="append", default=[])
    args = parser.parse_args()
    print(f"Version gate PASS: {versions(tag=args.tag)}")
    for path in args.binary:
        print(f"PE gate PASS: {path.name}: {', '.join(verify_binary(path))}")


if __name__ == "__main__":
    main()
