"""Developer/CI entry point. CMake owns C++; Cargo owns Rust; Tauri embeds the UI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = tomllib.loads((ROOT / "rust-toolchain.toml").read_text(encoding="utf-8"))["toolchain"]["channel"]


def host_target():
    architectures = {"arm64": "aarch64", "aarch64": "aarch64", "amd64": "x86_64", "x86_64": "x86_64"}
    machine = platform.machine().lower()
    if machine not in architectures:
        raise ValueError(f"Unsupported host architecture: {machine}")
    arch = architectures[machine]
    suffix = {"win32": "pc-windows-msvc", "linux": "unknown-linux-gnu", "darwin": "apple-darwin"}
    if sys.platform not in suffix:
        raise ValueError(f"Unsupported build host: {sys.platform}")
    return arch + "-" + suffix[sys.platform]


def run(*command, cwd=ROOT, env=None, capture=False):
    print("+", " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True,
                            stdout=subprocess.PIPE if capture else None, text=True, encoding="utf-8")
    return result.stdout if capture else None


def cargo(toolchain, *args, **kwargs):
    return run("cargo", "+" + toolchain, *args, **kwargs)


def npm(*args):
    # npm.cmd is a Windows command script; arguments here are fixed build commands.
    command = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "npm.cmd"] if os.name == "nt" else ["npm"]
    run(*command, *args, cwd=ROOT / "ui")


def native(preset):
    run("cmake", "--preset", preset, cwd=ROOT / "native")
    run("cmake", "--build", "--preset", preset, "--parallel", cwd=ROOT / "native")
    if preset != "linux-gcc":
        run(sys.executable, "scripts/build_icon_assets.py", "--check", "--output", ROOT / "build/native" / preset / "generated/icon_assets.h")
    run("cmake", "--install", ROOT / "build/native" / preset, "--prefix", ROOT, "--config", "Release")


def basic_checks():
    run(sys.executable, "tests/generate_golden.py", "--check")
    run(sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py")
    run(sys.executable, "scripts/release_checks.py")


def core(toolchain, target):
    basic_checks()
    cargo(toolchain, "fmt", "--all", "--check")
    flags = ["-p", "v8-chroma-core", "--lib", "--no-default-features", "--target", target, "--locked"]
    cargo(toolchain, "test", *flags)
    cargo(toolchain, "clippy", *flags, "--", "-D", "warnings")


def linux(toolchain, target, cairo):
    if sys.platform != "linux" or target != "x86_64-unknown-linux-gnu":
        raise ValueError("The experimental Linux backend requires Linux x64/glibc")
    native("linux-gcc")
    directory = ROOT / "build/native/linux-gcc"
    run("ctest", "--test-dir", directory, "--output-on-failure")
    if cairo:
        run(sys.executable, "native/linux/test_engine.py", "--engine", directory / "linux/libv8-chroma-linux.so",
            "--host", directory / "linux/1cv8", "--cairo", cairo, "--goldens", ROOT / "tests/golden")
    basic_checks()
    cargo(toolchain, "fmt", "--all", "--check")
    flags = ["-p", "v8-chroma-core", "--no-default-features", "--features", "linux-engine", "--target", target, "--locked"]
    cargo(toolchain, "test", *flags)
    cargo(toolchain, "clippy", *flags, "--all-targets", "--", "-D", "warnings")
    cargo(toolchain, "build", *flags, "--bin", "v8-chroma-linux")
    destination = ROOT / "dist/linux-preview"
    destination.mkdir(parents=True, exist_ok=True)
    build_root = Path(os.environ.get("CARGO_TARGET_DIR", ROOT / "target"))
    if not build_root.is_absolute():
        build_root = ROOT / build_root
    run(sys.executable, "native/linux/test_cli.py", "--cli", build_root / target / "debug/v8-chroma-linux",
        "--engine", directory / "linux/libv8-chroma-linux.so", "--host", directory / "linux/1cv8",
        "--cairo", directory / "linux/libcairo-v8.so", "--binder", directory / "linux/core83.so",
        "--injector", directory / "linux/v8-chroma-injector", "--graphics", directory / "linux/grphcs.so")
    shutil.copy2(build_root / target / "debug/v8-chroma-linux", destination)
    shutil.copy2(directory / "linux/libv8-chroma-linux.so", destination)


def check(toolchain, target, preset, cairo):
    if sys.platform != "win32" or target not in ("x86_64-pc-windows-msvc", "x86_64-pc-windows-gnu"):
        raise ValueError("The full drawing backend currently requires Windows x64. Use 'core' for portable checks.")
    native(preset)
    run(ROOT / "tests/fixtures/browser_lifetime_test.exe")
    run(ROOT / "tests/fixtures/icon_theme_test.exe")
    run(ROOT / "tests/fixtures/font_shaping.exe", "Consolas")
    run(ROOT / "tests/fixtures/ui_font_policy_test.exe")
    run(ROOT / "tests/fixtures/contrast_test.exe", *sorted((ROOT / "tests/golden").glob("*.bin")))
    with tempfile.TemporaryDirectory(prefix="icon-bootstrap-") as temporary:
        for mode in (1, 2):
            run(ROOT / "tests/fixtures/icon_bootstrap_test.exe", Path(temporary) / "colors.csv", mode)
            run(ROOT / "tests/fixtures/icon_bootstrap_test.exe", Path(temporary) / "available.csv", mode, "available")
    basic_checks()
    cargo(toolchain, "fmt", "--all", "--check")
    npm("ci", "--no-audit", "--no-fund")
    npm("run", "build")
    env = os.environ.copy()
    if target.endswith("-gnu"):
        metadata = json.loads(cargo(toolchain, "metadata", "--locked", "--format-version", "1", capture=True))
        binding = next(p for p in metadata["packages"] if p["name"] == "webview2-com-sys")
        loader = Path(binding["manifest_path"]).parent / "x64"
        env["PATH"] = str(loader) + os.pathsep + env.get("PATH", "")
    cargo(toolchain, "test", "--workspace", "--target", target, "--locked", env=env)
    cargo(toolchain, "clippy", "--workspace", "--all-targets", "--target", target, "--locked", "--", "-D", "warnings", env=env)
    if cairo:
        run(ROOT / "tests/fixtures/ui_font_discovery_test.exe", cairo.parent)
        for executable in ("raster_test.exe", "editor_selection_test.exe"):
            run(ROOT / "native/bin/demo" / executable, cairo)
        run(sys.executable, "native/tests/run_real_cairo.py", "--cairo", cairo)


def linux_desktop(toolchain, target, cairo):
    linux(toolchain, target, cairo)
    npm("ci", "--no-audit", "--no-fund")
    npm("run", "build")
    flags = ["-p", "v8-chroma-app", "--target", target, "--features", "custom-protocol", "--locked"]
    cargo(toolchain, "test", *flags)
    cargo(toolchain, "clippy", *flags, "--all-targets", "--", "-D", "warnings")
    cargo(toolchain, "build", *flags)
    build_root = Path(os.environ.get("CARGO_TARGET_DIR", ROOT / "target"))
    if not build_root.is_absolute():
        build_root = ROOT / build_root
    shutil.copy2(build_root / target / "debug/v8-chroma", ROOT / "dist/linux-preview/v8-chroma")
    shutil.copy2(ROOT / "ui/src-tauri/icons/icon.png", ROOT / "dist/linux-preview/icon.png")


def release_guard(tag, env):
    if env.get("GITHUB_ACTIONS") != "true" or env.get("GITHUB_REF_TYPE") != "tag" or env.get("GITHUB_REF_NAME") != tag:
        raise ValueError("Release executables are built only by GitHub Actions for the checked-out tag")
    if env.get("RUSTFLAGS") != "-C target-feature=+crt-static":
        raise ValueError("Release requires static MSVC CRT")


def release(tag, toolchain):
    release_guard(tag, os.environ)
    if sys.platform != "win32":
        raise ValueError("Only the Windows x64 backend can be released at present")
    run(sys.executable, "scripts/release_checks.py", "--tag", tag)
    cargo(toolchain, "build", "--workspace", "--release", "--locked", "--target", "x86_64-pc-windows-msvc",
          "--features", "v8-chroma-app/custom-protocol")
    target = Path(os.environ.get("CARGO_TARGET_DIR", ROOT / "target"))
    if not target.is_absolute():
        target = ROOT / target
    binaries = [target / "x86_64-pc-windows-msvc/release" / name for name in ("v8-chroma.exe", "v8-chroma-cli.exe")]
    run(sys.executable, "scripts/release_checks.py", "--tag", tag,
        "--binary", binaries[0], "--binary", binaries[1], "--binary", "assets/ThemeProbe.dll",
        "--binary", "assets/FontProbe.dll", "--binary", "assets/UiFontEngine.dll")
    run(sys.executable, "scripts/collect_licenses.py", "--toolchain", toolchain)
    destination = ROOT / "dist/release"
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise ValueError("Release destination must be empty")
    for source in [*binaries, ROOT / "LICENSE", ROOT / "build/licenses/THIRD-PARTY-NOTICES.txt"]:
        shutil.copy2(source, destination / source.name)
    with tempfile.TemporaryDirectory(prefix="v8-chroma smoke ", dir=os.environ.get("RUNNER_TEMP")) as temporary:
        directory = Path(temporary)
        cli = directory / "v8-chroma-cli.exe"
        shutil.copy2(binaries[1], cli)
        themes = json.loads(run(cli, "themes", cwd=directory, capture=True))
        if len(themes) != 13:
            raise ValueError("Copied CLI catalog failed")
        profile = directory / "profile"
        run(cli, "--config-dir", profile, "settings", "set", "--theme", "monokai", "--auto-apply", "off", cwd=directory)
        saved = json.loads(run(cli, "--config-dir", profile, "settings", "show", cwd=directory, capture=True))
        if saved["settings"]["selected_theme"] != "monokai" or saved["settings"]["auto_apply"] is not False:
            raise ValueError("Copied CLI settings persistence failed")
        run(cli, "--config-dir", profile, "watch", "--seconds", "2", cwd=directory)
    checksums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}" for p in sorted(destination.iterdir())]
    (destination / "SHA256SUMS.txt").write_text("\n".join(checksums) + "\n", encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("native", "check", "core", "linux", "linux-desktop", "release"))
    parser.add_argument("--toolchain", default=TOOLCHAIN)
    parser.add_argument("--target", default=host_target())
    parser.add_argument("--native-preset", default="windows-msvc")
    parser.add_argument("--cairo", type=Path)
    parser.add_argument("--tag")
    args = parser.parse_args()
    if args.command == "native":
        native(args.native_preset)
    elif args.command == "core":
        core(args.toolchain, args.target)
    elif args.command == "linux":
        linux(args.toolchain, args.target, args.cairo)
    elif args.command == "linux-desktop":
        linux_desktop(args.toolchain, args.target, args.cairo)
    elif args.command == "check":
        check(args.toolchain, args.target, args.native_preset, args.cairo)
    elif args.command == "release":
        if not args.tag:
            parser.error("release requires --tag")
        release(args.tag, args.toolchain)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
