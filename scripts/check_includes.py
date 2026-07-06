#!/usr/bin/env python3
"""Enforce code-design rule 9: module includes point one way.

- engine/** never includes game/** (the engine has no game knowledge)
- game/shared/** (the sim) never includes engine/gpu, engine/audio,
  or engine/platform (sim code must stay headless-linkable)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".inl"}

# (directory scanned, forbidden include-path prefixes)
RULES = [
    ("engine", ("game/",)),
    ("game/shared", ("engine/gpu", "engine/audio", "engine/platform")),
]


def main() -> int:
    violations = []
    for scan_dir, banned in RULES:
        base = ROOT / scan_dir
        if not base.is_dir():  # rule encoded before the dir exists
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for lineno, line in enumerate(text.splitlines(), 1):
                m = INCLUDE_RE.match(line)
                if m and m.group(1).startswith(banned):
                    rel = path.relative_to(ROOT)
                    violations.append(f'{rel}:{lineno}: forbidden include "{m.group(1)}"')
    for v in violations:
        print(v)
    if violations:
        print(f"{len(violations)} include-direction violation(s); see code-design-rules.md rule 9",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
