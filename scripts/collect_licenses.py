"""Assemble release notices from locked Windows Rust/npm dependencies.

Run npm ci and cargo fetch first. This collects license texts; it does not
maintain an inventory or decide whether a new dependency's license is suitable.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGET = "x86_64-pc-windows-msvc"


def license_files(directory):
    return sorted(p for p in directory.iterdir() if p.is_file() and
                  p.name.lower().startswith(("license", "licence", "copying", "notice", "copyright", "unlicense")))


def normal_packages(metadata):
    """Follow normal dependency edges, excluding build/dev-only subtrees."""
    nodes = {node["id"]: node for node in metadata["resolve"]["nodes"]}
    pending = list(metadata["workspace_members"])
    visited = set()
    while pending:
        package = pending.pop()
        if package in visited:
            continue
        visited.add(package)
        pending.extend(dep["pkg"] for dep in nodes[package]["deps"]
                       if any(kind["kind"] is None for kind in dep["dep_kinds"]))
    return sorted((p for p in metadata["packages"] if p["id"] in visited and p.get("source")),
                  key=lambda p: (p["name"], p["version"]))


def dependency_notice(ecosystem, name, version, expression, directory, source, overrides):
    files = license_files(directory)
    if not files:
        files = [ROOT / entry["file"] for entry in overrides
                 if (entry["ecosystem"], entry["name"], entry["version"]) == (ecosystem, name, version)]
    if not files or not expression:
        raise ValueError(f"Missing license information: {ecosystem} {name}@{version}")
    text = f"\n{'=' * 72}\n{ecosystem}: {name} {version}\nLicense: {expression}\nSource: {source}\n"
    if "MPL-2.0" in expression:
        text += "The unmodified source code of this dependency is available at the source URL above under MPL-2.0.\n"
    for path in files:
        text += f"\n--- {path.name} ---\n" + path.read_text(encoding="utf-8-sig")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--toolchain")
    parser.add_argument("--output", type=Path, default=ROOT / "build/licenses/THIRD-PARTY-NOTICES.txt")
    args = parser.parse_args()
    overrides = json.loads((ROOT / "licenses/overrides/sources.json").read_text(encoding="utf-8"))
    command = [args.cargo]
    if args.toolchain:
        command.append("+" + args.toolchain)
    command += ["metadata", "--locked", "--offline", "--filter-platform", TARGET,
                "--features", "v8-chroma-app/custom-protocol", "--format-version", "1"]
    metadata = json.loads(subprocess.check_output(command, cwd=ROOT))
    texts = []
    rust_packages = normal_packages(metadata)
    for package in rust_packages:
        name, version = package["name"], package["version"]
        texts.append(dependency_notice("rust", name, version, package.get("license"),
                     Path(package["manifest_path"]).parent,
                     f"https://crates.io/api/v1/crates/{name}/{version}/download", overrides))
    lock = json.loads((ROOT / "ui/package-lock.json").read_text(encoding="utf-8"))
    npm_count = 0
    for location, locked in sorted(lock["packages"].items()):
        if not location or locked.get("dev"):
            continue
        directory = ROOT / "ui" / location
        if not (directory / "package.json").exists():
            if locked.get("optional"):
                continue
            raise ValueError(f"Missing npm package; run npm ci: {location}")
        package = json.loads((directory / "package.json").read_text(encoding="utf-8"))
        if package["version"] != locked["version"]:
            raise ValueError(f"Installed npm version differs from lock: {location}")
        texts.append(dependency_notice("npm", package["name"], package["version"],
                     package.get("license"), directory, locked["resolved"], overrides))
        npm_count += 1
    static_files = [ROOT / "LICENSE", ROOT / "assets/THIRD-PARTY-NOTICES.txt"]
    static_files += sorted(p for p in (ROOT / "assets/palettes").rglob("*LICENSE*") if p.is_file())
    static_files += [ROOT / "assets/palettes/edt/third-party-notices.txt"]
    static_files += sorted((ROOT / "assets/licenses").glob("*.txt"))
    static_files += sorted((ROOT / "licenses/webview2-sdk").glob("*.txt"))
    static_files += sorted(p for p in (ROOT / "licenses/rust-stdlib").glob("*") if p.is_file())
    preface = ["v8-chroma — third-party notices\n\nThis file accompanies both executable files. Third-party code retains its own licenses.\n"]
    for path in static_files:
        preface.append(f"\n{'=' * 72}\n{path.relative_to(ROOT).as_posix()}\n\n" + path.read_text(encoding="utf-8-sig"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(preface + texts), encoding="utf-8")
    print(f"Collected notices: {len(rust_packages)} Rust, {npm_count} npm packages; {args.output}")


if __name__ == "__main__":
    main()
