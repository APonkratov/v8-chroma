"""Run the real glibc auditor against owned processes and real Cairo graphics state."""
import argparse
import json
import os
import select
from pathlib import Path
import struct
import subprocess
import tempfile
import time


def channels(rgb):
    return tuple((rgb >> shift & 255) / 255 for shift in (16, 8, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("engine", "host", "cairo", "goldens"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--binder", type=Path)
    args = parser.parse_args()
    cases = 0
    with tempfile.TemporaryDirectory(prefix="v8-chroma linux ") as directory:
        config = Path(directory) / "config.bin"

        def replace(data):
            pending = config.with_suffix(".pending")
            pending.write_bytes(data)
            pending.chmod(0o600)
            pending.replace(config)

        def start(mode="DESIGNER"):
            env = os.environ.copy()
            env.update(LD_AUDIT=str(args.engine.resolve()), V8_CHROMA_CONFIG=str(config),
                       V8_CHROMA_CAIRO=str(args.cairo.resolve()))
            command = [str(args.host), mode, str(args.cairo)]
            if args.binder:
                command.append(str(args.binder))
            return subprocess.Popen(command, env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def sample(proc, which, kind, rgb, alpha=.375, opaque=False):
            nonlocal cases
            values = (*channels(rgb), alpha)
            proc.stdin.write(f"{which} {kind} {int(opaque)} " + " ".join(map(str, values)) + "\n")
            proc.stdin.flush()
            assert select.select([proc.stdout], [], [], 5)[0], "fixture response timed out"
            line = proc.stdout.readline()
            assert line, f"fixture stopped: {proc.poll()}"
            cases += 1
            return tuple(map(float, line.split()))

        def expect(value, rgb, alpha):
            assert all(abs(a - b) < 1e-12 for a, b in zip(value, (*channels(rgb), alpha))), (value, rgb, alpha)

        def finish(proc):
            proc.stdin.close()
            assert proc.wait(timeout=5) == 0

        for golden in sorted(args.goldens.glob("*.bin")):
            if golden.stem.startswith("icons-"):
                continue
            data = golden.read_bytes()
            replace(data)
            proc = start()
            try:
                mode, count = struct.unpack_from("<II", data, 8)
                maps = [struct.unpack_from("<III", data, 1680 + i * 12) for i in range(count)]
                for kind, source, target in maps:
                    if kind < 5:
                        continue
                    expect(sample(proc, "vendor", kind, source), target if mode else source, 1 if kind == 5 else .375)
                    expect(sample(proc, "system", kind, source), source, 1 if kind == 5 else .375)
                    if args.binder:
                        expect(sample(proc, "platform", kind, source), target if mode else source, 1 if kind == 5 else .375)
                for kind in range(5, 9):
                    expect(sample(proc, "vendor", kind, 0x123456), 0x123456, 1 if kind == 5 else .375)
                for kind, source in ((7, 0xF4F2E6), (8, 0xF5F2DD)):
                    target = next((dst for api, src, dst in maps if api == kind and src == source), source)
                    expect(sample(proc, "vendor", kind, source, opaque=True), target if mode else source, 1)
            finally:
                finish(proc)

        # Atomic theme changes, malformed input and restoring pass-through in a
        # live process. No self-referential mock encoder: use committed goldens.
        dracula = (args.goldens / "dracula.bin").read_bytes()
        replace(dracula)
        proc = start()
        try:
            expect(sample(proc, "vendor", 5, 0xFFFFFF), 0x282A36, 1)
            for theme, expected in (("github-light-default", 0xFFFFFF), ("diagnostic", 0xFFFFFF), ("dracula", 0x282A36)):
                replace((args.goldens / f"{theme}.bin").read_bytes())
                time.sleep(.13)
                expect(sample(proc, "vendor", 5, 0xFFFFFF), expected, 1)
            for invalid in (b"short", dracula + b"x", b"\0" * 4760):
                replace(invalid)
                time.sleep(.13)
                expect(sample(proc, "vendor", 5, 0xFFFFFF), 0xFFFFFF, 1)
                assert json.loads(Path(str(config) + ".status").read_text())["error"] == "invalid-config"
            replace(dracula)
            time.sleep(.13)
            expect(sample(proc, "vendor", 5, 0xFFFFFF), 0x282A36, 1)
            config.chmod(0o666)
            time.sleep(.13)
            expect(sample(proc, "vendor", 5, 0xFFFFFF), 0xFFFFFF, 1)
        finally:
            finish(proc)

        replace(dracula)
        proc = start("ENTERPRISE")
        try:
            expect(sample(proc, "vendor", 5, 0xFFFFFF), 0xFFFFFF, 1)
        finally:
            finish(proc)
        print(f"Linux auditor: {cases} checks, 13 themes, system Cairo isolation, live reload and invalid input: PASS")


if __name__ == "__main__":
    main()
