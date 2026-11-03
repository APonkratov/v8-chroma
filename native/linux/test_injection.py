"""Inject into an already-running owned process; its existing Cairo table is cached."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import struct
import tempfile
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("injector", "engine", "host", "cairo", "graphics", "core", "goldens"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="v8 live injection ") as temporary:
        root = Path(temporary)
        host = root / "1cv8"
        shutil.copy2(args.host, host)
        shutil.copy2(args.cairo, root / "libcairo-v8.so")
        shutil.copy2(args.graphics, root / "grphcs.so")
        shutil.copy2(args.core, root / "core83.so")
        engine = root / "engine.so"
        shutil.copy2(args.engine, engine)
        config = root / "config.bin"
        def replace(theme):
            pending = root / "pending.bin"
            pending.write_bytes((args.goldens / f"{theme}.bin").read_bytes())
            pending.chmod(0o600); pending.replace(config)
        replace("dracula")
        env = {key:value for key,value in os.environ.items() if key not in ("LD_AUDIT", "LD_PRELOAD")}
        with subprocess.Popen([str(host), "DESIGNER", str(root / "libcairo-v8.so"), "--live"], env=env,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as process:
            def sample(which="vendor", kind=5):
                process.stdin.write(f"{which} {kind} 0 1 1 1 1\n"); process.stdin.flush()
                assert select.select([process.stdout], [], [], 10)[0], "No response after injection"
                result = process.stdout.readline()
                assert result, f"fixture exited ({process.poll()})"
                return tuple(map(float, result.split()))
            assert sample() == (1, 1, 1, 1)
            stat = Path(f"/proc/{process.pid}/stat").read_text().rsplit(")",1)[1].split()[19]
            try:
                for start, library in ((str(int(stat) + 1), engine), (stat, root / "missing.so")):
                    rejected = subprocess.run([str(args.injector), str(process.pid), start, str(library), str(config)],
                                              capture_output=True, text=True, timeout=20)
                    assert rejected.returncode != 0, "Invalid attachment accepted"
                    assert sample() == (1, 1, 1, 1), "Failed attachment changed renderer"
                result = subprocess.run([str(args.injector), str(process.pid), stat, str(engine), str(config)],
                                        capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                assert all(abs(a-b)<1e-12 for a,b in zip(sample(),(0x28/255,0x2a/255,0x36/255,1)))
                assert sample("system") == (1, 1, 1, 1)
                replace("diagnostic"); time.sleep(.25)
                assert sample() == (1, 1, 1, 1)
                status=json.loads(Path(str(config)+".status").read_text())
                assert status["theme"] == "diagnostic" and status["hook_mask"] & 11 == 11
                assert int(Path(f"/proc/{process.pid}/status").read_text().split("TracerPid:",1)[1].split()[0]) == 0
                mono = subprocess.check_output(["fc-match", "--format=%{family}", "monospace"], text=True).split(',')[0]
                sans = subprocess.check_output(["fc-match", "--format=%{family}", "sans"], text=True).split(',')[0]
                def name_hash(name):
                    value=2166136261
                    for char in name:
                        value=((value^ord(char))*16777619)&0xffffffff
                    return value
                font_file=Path(str(config)+".fonts")
                def write_fonts(editor_enabled,editor_family,ui_enabled,ui_family,points10=195):
                    pending=root/"fonts.pending"
                    pending.write_bytes(struct.pack('<5I',148,1,editor_enabled,points10,ui_enabled)+editor_family.encode('utf-16-le').ljust(64,b'\0')+ui_family.encode('utf-16-le').ljust(64,b'\0'))
                    pending.chmod(0o600);pending.replace(font_file);time.sleep(.25)
                def fonts(enabled):
                    write_fonts(enabled,mono,enabled,sans)
                def font_status_when(predicate):
                    deadline=time.monotonic()+5
                    while time.monotonic()<deadline:
                        value=json.loads(Path(str(config)+".status").read_text())
                        if predicate(value):return value
                        time.sleep(.1)
                    raise AssertionError(f"Font diagnostics did not converge: {value}")
                fonts(1)
                assert sample("font",5)==(name_hash(mono),-26,0,400), "Editor family/point size/metrics not replaced"
                assert sample("font",7)==(name_hash(sans),-17,0,400), "Stock interface font not replaced"
                assert sample("font",6)==(name_hash("Arial"),-17,7,400), "Explicit document font changed"
                assert sample("font",8)==(name_hash("MS Sans Serif"),-17,7,400), "Explicit alias treated as stock"
                assert sample("font",9)==(name_hash("MS Sans Serif"),-17,7,400), "Explicit face attribute replaced"
                assert sample("font",10)==(name_hash("Courier New"),-17,7,400), "Fixed stock font replaced"
                fonts(0)
                assert sample("font",5)==(name_hash("Courier New"),-17,7,400), "Stock editor not restored"
                fonts(1);font_file.chmod(0o666);time.sleep(.25)
                assert sample("font",5)==(name_hash("Courier New"),-17,7,400), "Untrusted font config accepted"
                # Disabling a rejected font can match the effective config;
                # diagnostics must still clear without requesting a refresh.
                missing="V8Chroma Missing Test Font"
                write_fonts(1,missing,0,sans)
                rejected=font_status_when(lambda value:value["font_error"]=="editor-font-unavailable")
                write_fonts(0,missing,0,sans)
                cleared=font_status_when(lambda value:not value["font_error"])
                assert cleared["font_generation"]==rejected["font_generation"] and not cleared["editor_enabled"]
                # The Windows default has an explicit, reported Linux fallback.
                write_fonts(0,mono,1,'Segoe UI Variable Small',130)
                font_status=font_status_when(lambda value:value["ui_enabled"] and not value["font_error"])
                assert font_status["ui_enabled"] and not font_status["font_error"], font_status
                assert sample("font",7)==(name_hash(font_status["ui_family"]),-17,0,400)
                if font_status["ui_fallback"] and font_status["ui_family"]!='Segoe UI Variable Small':
                    write_fonts(0,mono,1,font_status["ui_family"],130)
                    direct=font_status_when(lambda value:not value["ui_fallback"])
                    assert direct["font_generation"]==font_status["font_generation"] and direct["ui_family"]==font_status["ui_family"]
            finally:
                process.stdin.close()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5)
    print("Live injection: cached renderer, restored thread/SIMD, Cairo isolation, font family/size/stock restore, explicit-font isolation and untrusted config: PASS")

if __name__ == "__main__": main()
