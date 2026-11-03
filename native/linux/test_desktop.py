"""Exercise the embedded desktop UI through the native WebKitGTK WebDriver."""
import argparse
import base64
import json
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def until(predicate, message, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.25)
    raise AssertionError(message)


def stop_process(process, *, terminate=False, timeout=5):
    if process.stdin:
        try:
            process.stdin.close()
        except OSError:
            pass
    try:
        if terminate:
            process.terminate()
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired) as error:
        # Cleanup of one owned process must not skip the others or replace
        # the original test failure.
        print(f"Cleanup failed for test process {process.pid}: {error}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, default=Path("build/linux-desktop-test"))
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="v8 desktop Профиль ") as temporary:
        root = Path(temporary)
        platform = root / "Платформа с пробелами"
        platform.mkdir()
        for name in ("1cv8", "libcairo-v8.so", "grphcs.so", "core83.so"):
            shutil.copy2(args.fixtures / name, platform / name)
        shutil.copy2(args.fixtures / "core83.so", platform / "frame.so")
        profile = root / "profile"
        profile.mkdir()
        settings = {"schema_version": 4, "selected_theme": "dracula", "auto_apply": False,
                    "custom_icons": False, "editor_font": {"enabled": False, "family": "JetBrains Mono Medium", "points10": 130},
                    "ui_font": {"enabled": False, "family": "Segoe UI Variable Small"},
                    "window": {"width": 1000, "height": 700, "maximized": False}}
        (profile / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        fixtures = []

        def start_fixture(mode="DESIGNER"):
            process = subprocess.Popen([str(platform / "1cv8"), mode, str(platform / "libcairo-v8.so"), "--live"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            fixtures.append(process)
            assert sample(process) == (1, 1, 1, 1)
            return process

        def sample(process, which="vendor", kind=5):
            process.stdin.write(f"{which} {kind} 0 1 1 1 1\n")
            process.stdin.flush()
            assert select.select([process.stdout], [], [], 5)[0], "Fixture stopped responding"
            return tuple(map(float, process.stdout.readline().split()))

        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = os.environ.copy()
        env.update(TAURI_AUTOMATION="true", TAURI_WEBVIEW_AUTOMATION="true")
        session = None
        log = (args.evidence / "webdriver.log").open("w")
        driver = subprocess.Popen(["WebKitWebDriver", "--host=127.0.0.1", f"--port={port}"],
                                  env=env, stdout=log, stderr=log)

        def request(method, path, body=None):
            data = json.dumps(body).encode() if body is not None else None
            req = urllib.request.Request(f"http://127.0.0.1:{port}" + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=45) as response:
                    return json.load(response)["value"]
            except urllib.error.HTTPError as error:
                raise AssertionError(error.read().decode()) from error

        def ready():
            try:
                return request("GET", "/status")["ready"]
            except urllib.error.URLError:
                return False

        def execute(script, *values):
            return request("POST", f"/session/{session}/execute/sync", {"script": script, "args": list(values)})

        def invoke(command, arguments=None):
            result = request("POST", f"/session/{session}/execute/async", {
                "script": "const done=arguments[arguments.length-1]; window.__TAURI_INTERNALS__.invoke(arguments[0], arguments[1]).then(value=>done({value})).catch(error=>done({error:String(error)}));",
                "args": [command, arguments or {}]})
            assert "error" not in result, result
            return result.get("value")

        def state():
            return invoke("get_state")

        def click(selector):
            until(lambda: execute("const e=document.querySelector(arguments[0]); return e && !e.disabled && e.getClientRects().length>0", selector),
                  f"Control stayed disabled: {selector}")
            element = request("POST", f"/session/{session}/element", {"using": "css selector", "value": selector})
            key = element["element-6066-11e4-a52e-4f735466cecf"]
            request("POST", f"/session/{session}/element/{key}/click", {})

        def idle():
            return until(lambda: not state()["busy"] and execute("return !document.querySelector('button.theme-card:disabled')"), "UI stayed busy")

        def themed(process, theme):
            snapshot = state()
            return any(row["pid"] == process.pid and row["theme"] == theme and row["status"] == "applied" for row in snapshot["processes"])
        def choose(selector, value):
            # A font sample can change before the watcher finishes applying
            # the rest of the appearance. The DOM polls busy once per second,
            # so wait for the backend as well before sending the next change.
            idle()
            until(lambda: execute("const e=document.querySelector(arguments[0]);return e&&!e.disabled",selector),"Select stayed disabled")
            execute("const e=document.querySelector(arguments[0]);e.value=arguments[1];e.dispatchEvent(new Event('change',{bubbles:true}));",selector,value)
            idle()
        def name_hash(name):
            value=2166136261
            for char in name:value=((value^ord(char))*16777619)&0xffffffff
            return value

        try:
            first = start_fixture()
            enterprise = start_fixture("ENTERPRISE")
            until(ready, "WebKitWebDriver did not start")
            created = request("POST", "/session", {"capabilities": {"alwaysMatch": {
                "webkitgtk:browserOptions": {"binary": str(args.app.resolve()), "args": ["--config-dir", str(profile)]}}}})
            session = created["sessionId"]
            until(lambda: execute("return document.querySelectorAll('.theme-card').length===14"), "Theme catalog did not render")
            snapshot = state()
            assert snapshot["capabilities"]["platform"] == "linux", snapshot
            assert snapshot["capabilities"]["fonts"] and snapshot["capabilities"]["custom_icons"]
            assert execute("return document.querySelector('.appearance-controls')!==null")
            assert not snapshot["settings"]["settings"]["auto_apply"]
            mono=subprocess.check_output(["fc-match","--format=%{family}","monospace"],text=True).split(',')[0]
            sans=subprocess.check_output(["fc-match","--format=%{family}","sans"],text=True).split(',')[0]
            until(lambda: execute("return document.querySelectorAll('#font-family option').length>1"),"Font catalog missing")
            choose("#font-family",mono)
            choose("#font-size","19.5")
            choose("#ui-font-family",sans)
            click("#custom-icons")
            idle()
            saved=state()["settings"]["settings"]
            assert saved["custom_icons"] and saved["editor_font"]["family"]==mono and saved["editor_font"]["points10"]==195 and saved["ui_font"]["family"]==sans
            assert sample(first,"font",5)==(name_hash("Courier New"),-17,7,400),"Font selection applied with automation off"
            # A UI selection while automation is off only saves the preference.
            click('button.theme-card[title="GitHub Light Default"]')
            idle()
            assert state()["settings"]["settings"]["selected_theme"] == "github-light-default"
            assert sample(first) == (1, 1, 1, 1)
            click('button[title="Конфигураторы"]')
            until(lambda: execute("return document.querySelectorAll('.process-card').length>0"), "Running Designer missing")
            # Apply exactly the fixture row through its real button.
            execute("const card=[...document.querySelectorAll('.process-card')].find(c=>c.textContent.includes('PID '+arguments[0])); card.querySelector('button').id='fixture-apply';", first.pid)
            click("#fixture-apply")
            until(lambda: themed(first, "github-light-default"), "Manual application failed")
            assert sample(first,"font",5)==(name_hash(mono),-26,0,400),"Manual font/size not applied"
            assert sample(first,"font",7)==(name_hash(sans),-17,0,400),"Manual interface font not applied"
            # Enable automation from Settings, then select Dracula from the catalog.
            click('button[title="Настройки"]')
            click('button[aria-label="Автоматическое применение оформления"]')
            until(lambda: state()["settings"]["settings"]["auto_apply"], "Automation switch failed")
            click('button[title="Темы"]')
            idle()
            click('button.theme-card[title="Dracula"]')
            until(lambda: themed(first, "dracula"), "Theme selection did not propagate")
            second = start_fixture()
            until(lambda: themed(second, "dracula"), "New Designer not attached automatically")
            assert sample(enterprise) == (1, 1, 1, 1), "Enterprise mode was modified"
            assert any(row["pid"] == enterprise.pid and not row["eligible"] for row in state()["processes"])
            expected = (0x28 / 255, 0x2a / 255, 0x36 / 255, 1)
            assert all(abs(a-b) < 1e-12 for a, b in zip(sample(second), expected))
            assert sample(second,"font",5)==(name_hash(mono),-26,0,400),"New Designer font not applied"
            choose("#font-family","")
            until(lambda: sample(first,"font",5)==(name_hash("Courier New"),-17,7,400),"Stock editor font not restored")
            choose("#ui-font-family","")
            until(lambda: sample(first,"font",7)==(name_hash("Arial"),-17,7,400),"Stock interface font not restored")
            choose("#font-family",mono)
            choose("#ui-font-family",sans)
            click('button[title="Настройки"]')
            click('button[aria-label="Автоматическое применение оформления"]')
            until(lambda: not state()["settings"]["settings"]["auto_apply"], "Automation did not stop")
            third = start_fixture()
            time.sleep(2)
            assert sample(third) == (1, 1, 1, 1), "Automation ran while disabled"
            click('button[aria-label="Автоматическое применение оформления"]')
            until(lambda: themed(third, "dracula"), "Re-enabling did not attach waiting Designer")
            click('button[title="Темы"]')
            (args.evidence / "desktop.png").write_bytes(base64.b64decode(request("GET", f"/session/{session}/screenshot")))
            (args.evidence / "state.json").write_text(json.dumps(state(), ensure_ascii=False, indent=2), encoding="utf-8")
            print("Linux desktop: themes/icons/fonts/size, stock restore, manual apply, auto-off, existing/new Designer, Enterprise isolation and resume: PASS", flush=True)
        except Exception:
            if session:
                try:
                    (args.evidence / "failure-state.json").write_text(json.dumps(state(), ensure_ascii=False, indent=2), encoding="utf-8")
                    (args.evidence / "failure-dom.txt").write_text(execute("return document.body.innerText"), encoding="utf-8")
                    (args.evidence / "failure.png").write_bytes(base64.b64decode(request("GET", f"/session/{session}/screenshot")))
                except (AssertionError, OSError):
                    pass
            raise
        finally:
            if session:
                try:
                    invoke("quit_app")
                except (AssertionError, OSError):
                    pass
                try:
                    request("DELETE", f"/session/{session}")
                except (AssertionError, OSError):
                    pass
            try:
                stop_process(driver, terminate=True, timeout=10)
            finally:
                try:
                    log.close()
                finally:
                    for process in fixtures:
                        stop_process(process)


if __name__ == "__main__":
    main()
