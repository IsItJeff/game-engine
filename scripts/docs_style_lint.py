#!/usr/bin/env python3
"""Docs style lint. Handbook pages: full rule set. Engine pages: heading
depth only. docs/design/** is a frozen archive and fully exempt."""
import re
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"
MAX_WORDS, MAX_DEPTH, MAX_ADMONITIONS = 900, 3, 3


def lint(path: Path, full: bool) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    words, admonitions, title, in_fence = 0, 0, None, False
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if re.match(r"\s*(```|~~~)", line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        heading = re.match(r"(#{1,6})\s+(.*)", line)
        if heading:
            depth = len(heading.group(1))
            if depth > MAX_DEPTH:
                errors.append((n, f"heading depth {depth} exceeds {MAX_DEPTH}"))
            if title is None and depth == 1:
                title = (n, heading.group(2))
            continue
        if re.match(r"\s*(!!!|\?\?\?)", line):
            admonitions += 1
            if admonitions == MAX_ADMONITIONS + 1 and full:
                errors.append((n, f"more than {MAX_ADMONITIONS} admonitions"))
            continue
        words += len(line.split())
    if full:
        if words > MAX_WORDS:
            errors.append((1, f"{words} words of prose exceeds {MAX_WORDS}"))
        if title and " and " in title[1].lower():
            errors.append((title[0], "title contains ' and ' — one concept per page"))
    return errors


def main() -> int:
    status = 0
    for pattern, full in (("handbook/**/*.md", True), ("engine/**/*.md", False)):
        for path in sorted(DOCS.glob(pattern)):
            for n, message in lint(path, full):
                print(f"{path.relative_to(DOCS.parent)}:{n}: {message}")
                status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
