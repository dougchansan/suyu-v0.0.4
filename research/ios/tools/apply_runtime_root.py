#!/usr/bin/env python3
"""Add an emitted runtime-discovered block to a generated module source unit."""
import argparse
from pathlib import Path
import re


def apply(project: Path, module: str, offset: int, function: str) -> Path:
    name = f"blk_{module}_{offset:016x}"
    entries = []
    pattern = re.compile(rf"^  \{{0x([0-9a-f]+)ULL, (blk_{re.escape(module)}_[0-9a-f]+)\}},$",
                         re.MULTILINE)
    for path in (project / "src").glob(f"recompiled_{module}*.c"):
        source = path.read_text()
        entries.extend((int(match.group(1), 16), path, match.group(0))
                       for match in pattern.finditer(source))
    entries.sort(key=lambda item: item[0])
    if any(value == offset for value, _, _ in entries):
        raise ValueError(f"root 0x{offset:x} already exists")
    successor = next((item for item in entries if item[0] > offset), None)
    if successor is None:
        raise ValueError("root lies beyond generated dispatch range")
    _, path, successor_line = successor
    source = path.read_text()
    table_marker = re.search(rf"^const struct _recomp_ent _seg_{re.escape(module)}_\d+\[\] = \{{",
                             source, re.MULTILINE)
    if not table_marker:
        raise ValueError(f"dispatch table missing from {path}")
    if f"void {name}" in source:
        raise ValueError(f"function {name} already exists without a dispatch entry")
    source = source[:table_marker.start()] + function.rstrip() + "\n\n" + source[table_marker.start():]
    entry = f"  {{0x{offset:x}ULL, {name}}},"
    if source.count(successor_line) != 1:
        raise ValueError("successor dispatch entry is ambiguous")
    source = source.replace(successor_line, entry + "\n" + successor_line)
    count_pattern = re.compile(
        rf"(const unsigned _segn_{re.escape(module)}_\d+ = )(\d+)(U;)")
    count_match = count_pattern.search(source)
    if not count_match:
        raise ValueError("dispatch entry count is missing")
    source = (source[:count_match.start()] + count_match.group(1) +
              str(int(count_match.group(2)) + 1) + count_match.group(3) +
              source[count_match.end():])
    path.write_text(source)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("module")
    parser.add_argument("offset", type=lambda value: int(value, 0))
    parser.add_argument("function", type=Path)
    args = parser.parse_args()
    print(apply(args.project, args.module, args.offset, args.function.read_text()))
