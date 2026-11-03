"""CLI lifecycle test: real detached process, Unicode paths and stale PID guards."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "engine", "host", "cairo", "binder", "injector", "graphics"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    pid = None
    with tempfile.TemporaryDirectory(prefix="v8-chroma CLI ") as temporary:
        root = Path(temporary)
        platform = root / "Платформа с пробелами"
        platform.mkdir()
        shutil.copy2(args.host, platform / "1cv8")
        shutil.copy2(args.cairo, platform / "libcairo-v8.so")
        for library in ("core83.so", "frame.so", "grphcs.so"):
            shutil.copy2(args.binder, platform / library)
        profile = root / "Профиль с пробелами"

        def cli(*command, success=True):
            result = subprocess.run([str(args.cli), "--data-dir", str(profile), *map(str, command)],
                                    capture_output=True, text=True, timeout=20)
            assert (result.returncode == 0) == success, (result.stdout, result.stderr)
            return json.loads(result.stdout) if success else result.stderr

        try:
            session = cli("launch", "--platform", platform / "1cv8", "--engine", args.engine,
                          "--theme", "dracula", "--", platform / "libcairo-v8.so", "--serve")
            pid = session["identity"]["pid"]
            assert session["engine_sha256"] in session["engine"]
            assert Path(session["engine"]).read_bytes() == args.engine.read_bytes()
            assert cli("status", "--pid", pid)["pid"] == pid
            assert next(row for row in cli("list") if row["pid"] == pid)["managed"]
            assert os.stat(profile).st_mode & 0o777 == 0o700
            for theme in ("github-light-default", "standard", "dracula"):
                result = cli("apply", "--pid", pid, "--theme", theme)
                assert result["confirmed"] and result["status"]["theme"] == theme, result
            cli("apply", "--pid", pid, "--theme", "nonexistent", success=False)
            assert cli("status", "--pid", pid)["theme"] == "dracula"
            # Model reuse of the same PID with another start time.
            record = profile / "sessions" / f"{pid}.json"
            original = record.read_bytes()
            stale = json.loads(original)
            stale["identity"]["created_time"] += 1
            record.write_text(json.dumps(stale))
            cli("apply", "--pid", pid, "--theme", "standard", success=False)
            record.write_bytes(original)
            os.kill(pid, signal.SIGTERM)
            for _ in range(40):
                if not Path(f"/proc/{pid}/exe").exists():
                    break
                time.sleep(.05)
            cli("status", "--pid", pid, success=False)
            # Exercise the Rust live-attachment transaction, not only the helper.
            shutil.copy2(args.graphics, platform / "grphcs.so")
            helper = profile / "runtime" / (hashlib.sha256(args.injector.read_bytes()).hexdigest() + ".injector")
            helper.write_bytes(args.injector.read_bytes())
            helper.chmod(0o600)  # Interrupted staging before chmod must repair itself.
            with subprocess.Popen([str(platform / "1cv8"), "DESIGNER", str(platform / "libcairo-v8.so"), "--live"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as live:
                pid = live.pid
                def sample():
                    live.stdin.write("vendor 5 0 1 1 1 1\n"); live.stdin.flush()
                    assert select.select([live.stdout], [], [], 5)[0], "Live fixture is unresponsive"
                    return tuple(map(float, live.stdout.readline().split()))
                try:
                    assert sample() == (1, 1, 1, 1)
                    for theme in ("dracula", "github-light-default", "standard"):
                        applied = cli("attach", "--pid", pid, "--theme", theme,
                                      "--engine", args.engine, "--injector", args.injector)
                        assert applied["confirmed"] and applied["status"]["theme"] == theme, applied
                    assert helper.stat().st_mode & 0o777 == 0o700
                    record = json.loads((profile / "sessions" / f"{pid}.json").read_text())
                    config = Path(record["config"])
                    goldens = Path(__file__).resolve().parents[2] / "tests/golden"
                    config.write_bytes((goldens / "icons-dark.bin").read_bytes())
                    time.sleep(.25)
                    for action, theme, mode in (("apply", "github-light-default", 1), ("attach", "dracula", 2)):
                        extra = ("--engine", args.engine, "--injector", args.injector) if action == "attach" else ()
                        applied = cli(action, "--pid", pid, "--theme", theme, *extra)
                        assert applied["confirmed"] and applied["status"]["icon_mode"] == mode, applied
                    cli("apply", "--pid", pid, "--theme", "standard")
                    assert sample() == (1, 1, 1, 1)
                finally:
                    live.stdin.close()
                    try: live.wait(timeout=5)
                    except subprocess.TimeoutExpired: live.terminate(); live.wait(timeout=5)
        finally:
            if pid and Path(f"/proc/{pid}/exe").exists():
                os.kill(pid, signal.SIGTERM)
    print("Linux CLI: launch, live attach, detached lifetime, Unicode paths, palette switch, reset, invalid input and stale identity: PASS")


if __name__ == "__main__":
    main()
