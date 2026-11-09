"""Embed the pinned TextMate engine, BSL/SDBL grammars and local scope mapping."""
from pathlib import Path
import sys


def main():
    destination, *sources = map(Path, sys.argv[1:])
    names = ("engine", "mapping", "bsl", "sdbl")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="ascii", newline="\n") as output:
        output.write("#pragma once\nnamespace embedded {\n")
        for name, source in zip(names, sources, strict=True):
            data = source.read_text(encoding="utf-8-sig").encode("utf-8")
            output.write(f"inline constexpr unsigned char {name}[] = {{\n")
            for offset in range(0, len(data), 32):
                output.write(",".join(map(str, data[offset:offset + 32])) + ",\n")
            output.write("0};\n")
        output.write("}\n")


if __name__ == "__main__":
    main()
