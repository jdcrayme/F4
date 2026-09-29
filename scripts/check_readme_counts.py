#!/usr/bin/env python3
"""Diff README.md per-library test counts against actual TEST macro counts.

The README documents every library with a `**Tests**: N` prose row and a
quick-reference table row. Both drift the moment a TEST lands without a
re-pin — this script is the two-minute drift check: it counts the TEST(
/TEST_F(/TEST_P( macros under each library's tests/ directory and prints
every row that disagrees. Run from the repo root; exit 1 when any row is
stale (CI-friendly), 0 when the counts are honest.
"""
import os
import re
import sys

# The libraries the README documents (### headers + table rows).
LIBS = ["f4-simulation", "f4-world-viewer", "f4-ai", "f4-campaign",
        "f4-renderer", "f4-world-convert", "f4-flight-model", "f4-models",
        "f4-data", "f4-world", "f4-convert", "f4-weapons", "f4-campaign-api",
        "f4-import", "f4-entities", "f4-sensors", "f4-recorder", "f4-terrain",
        "f4-math", "f4-anim", "f4-assets", "f4-units", "f4-install",
        "f4-world-types", "f4-gltf", "f4-io", "f4-json", "f4-lzss",
        "f4-messaging", "f4-state-machine", "f4-geo", "f4-flight-api"]

TEST_MACRO = re.compile(r"^TEST")


def actual(lib):
    """Count TEST macros across every .cpp under <lib>/tests/."""
    root = os.path.join(lib, "tests")
    count = 0
    if not os.path.isdir(root):
        return count
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not name.endswith(".cpp"):
                continue
            with open(os.path.join(dirpath, name), encoding="utf-8",
                      errors="replace") as f:
                for line in f:
                    if TEST_MACRO.match(line):
                        count += 1
    return count


def main():
    with open("README.md", encoding="utf-8") as f:
        lines = f.read().splitlines()

    # Prose rows: "**Tests**: N" tracked against the current ### header.
    section_counts = {}
    current = None
    for ln in lines:
        m = re.match(r"###\s+(f4-[a-z-]+)", ln)
        if m:
            current = m.group(1)
        m2 = re.search(r"\*\*Tests\*\*: (\d+)", ln)
        if m2 and current:
            section_counts.setdefault(current, int(m2.group(1)))

    # Table rows: | `f4-xxx` | ... | N |
    table_counts = {}
    for ln in lines:
        m = re.match(r"\|\s*`(f4-[a-z-]+)`\s*\|.*\|\s*(\d+)\s*\|\s*$", ln)
        if m:
            table_counts[m.group(1)] = int(m.group(2))

    stale = 0
    for lib in LIBS:
        a = actual(lib)
        s = section_counts.get(lib)
        t = table_counts.get(lib)
        flags = []
        if s is not None and s != a:
            flags.append(f"section {s}!={a}")
        if t is not None and t != a:
            flags.append(f"table {t}!={a}")
        note = "  <<< " + ", ".join(flags) if flags else ""
        if flags:
            stale += 1
        print(f"{lib:20s} actual={a:4d} section={s} table={t}{note}")

    if stale:
        print(f"\n{stale} library row(s) stale — re-pin README.md")
        return 1
    print("\nall README test counts honest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
