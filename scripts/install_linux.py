"""Install a built Linux preview and a desktop menu entry for the current user."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILES = ("v8-chroma", "v8-chroma-injector", "libv8-chroma-linux.so", "icon.png")


def desktop_string(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def exec_argument(value):
    # Desktop Entry Exec has its own quoting, followed by string-value escaping.
    text = str(value).replace("%", "%%")
    text = "".join("\\" + char if char in '\\"`$' else char for char in text)
    return desktop_string('"' + text + '"')


def atomic_write(path, content, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".v8-chroma-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def install(source, data_home):
    source = source.resolve(strict=True)
    contents = {name: (source / name).read_bytes() for name in FILES}
    digest = hashlib.sha256()
    for name, content in contents.items():
        digest.update(name.encode() + b"\0" + hashlib.sha256(content).digest())
    app = data_home.resolve() / "v8-chroma/app"
    releases = app / "versions"
    releases.mkdir(parents=True, exist_ok=True)
    version = releases / digest.hexdigest()
    if not version.exists():
        temporary = Path(tempfile.mkdtemp(prefix=".install-", dir=releases))
        try:
            for name, content in contents.items():
                target = temporary / name
                target.write_bytes(content)
                target.chmod(0o755 if name in FILES[:2] else 0o644)
            temporary.rename(version)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
    else:
        for name, content in contents.items():
            if (version / name).read_bytes() != content:
                raise ValueError(f"Damaged installation: {version / name}")
    # Never overwrite an executable or library mapped by a running process.
    link = app / f".current-{os.getpid()}"
    try:
        link.symlink_to(version.relative_to(app), target_is_directory=True)
        link.replace(app / "current")
    finally:
        link.unlink(missing_ok=True)
    executable = app / "current/v8-chroma"
    desktop = data_home.resolve() / "applications/v8-chroma.desktop"
    atomic_write(desktop, "\n".join((
        "[Desktop Entry]", "Type=Application", "Name=V8 Chroma",
        "Comment=Themes for 1C Designer", "Comment[ru]=Темы для Конфигуратора 1С",
        "Exec=" + exec_argument(executable),
        "Icon=" + desktop_string(app / "current/icon.png"),
        "Terminal=false", "Categories=Development;Utility;", "StartupNotify=true", "",
    )))
    return {"executable": str(executable), "desktop_entry": str(desktop)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "dist/linux-preview")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run this installer inside Linux/WSL")
    data_home = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    if not data_home.is_absolute():
        parser.error("XDG_DATA_HOME must be absolute")
    print(json.dumps(install(args.source, data_home), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
