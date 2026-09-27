#!/usr/bin/env python3
"""Export a theater's campaign name table (the nameid strings) to JSON.

Falcon 4 stores the campaign's per-instance display names — the strings
ObjectivePriorityComponent::nameid and SquadronUIInfo::name_id index —
as a pair of files per campaign/theater (FreeFalcon CAMPLIB/Name.cpp
LoadNames/LoadNameStream):

    <theater>.idx   short NameEntries; short NameIndex[NameEntries];
                    (offsets, in TCHAR units, into the .wch stream)
    <theater>.wch   the raw string stream (no delimiters, no terminators —
                    a string is NameIndex[i] .. NameIndex[i+1])

Entry 0 is "Nowhere" (the F4 null name). This script rewrites the pair
as one JSON document so the no-binary runtime can resolve nameids:

    {"format": "f4.theater.names/1",
     "theater": "korea",
     "count": <NameEntries>,
     "names": ["Nowhere", "New", ...]}

Run from the repo root (vanilla install shown; point --install at the
campaign dir's PARENT — the same root export-game-data uses):

    python scripts/export_names.py --install "D:/SteamLibrary/steamapps/common/Falcon 4.0"

Writes Data/Theater/<theater>/names.json (default theater: korea).
Regenerate Data/manifest.json afterwards (generate_manifest.py), the
same as after any Data/ change — CI's --check gate covers the new file.
"""
import argparse
import json
import os
import struct
import sys


def read_names(install, theater):
    """Read <theater>.idx + <theater>.wch from the install's campaign dir.

    Returns the list of names (len == NameEntries; the last entry's
    offset == the stream length, so names has count-1 usable strings —
    kept verbatim, F4 itself indexes only 0..NameEntries-2).
    """
    campaign_dir = os.path.join(install, "campaign", "SAVE")
    idx_path = os.path.join(campaign_dir, f"{theater}.idx")
    wch_path = os.path.join(campaign_dir, f"{theater}.wch")
    for path in (idx_path, wch_path):
        if not os.path.exists(path):
            raise SystemExit(
                f"error: {path} not found — point --install at the "
                f"install root (the dir containing campaign/)")
    with open(idx_path, "rb") as f:
        idx = f.read()
    with open(wch_path, "rb") as f:
        wch = f.read()

    if len(idx) < 2:
        raise SystemExit(f"error: {idx_path} too small for a count")
    (count,) = struct.unpack_from("<h", idx, 0)
    if count <= 0 or 2 + count * 2 > len(idx):
        raise SystemExit(f"error: {idx_path} implausible count {count}")

    offsets = struct.unpack_from(f"<{count}h", idx, 2)
    stream_len = offsets[-1]
    if stream_len > len(wch):
        raise SystemExit(
            f"error: {idx_path} references offset {stream_len} but "
            f"{wch_path} is only {len(wch)} bytes")

    names = []
    for i in range(count - 1):
        raw = wch[offsets[i]:offsets[i + 1]]
        names.append(raw.decode("latin-1"))
    return names


def main():
    ap = argparse.ArgumentParser(
        description="Export a theater's F4 campaign name table to JSON.")
    ap.add_argument("--install", required=True,
                    help="install root (the dir containing campaign/)")
    ap.add_argument("--theater", default="korea")
    ap.add_argument("--out",
                    help="output path (default Data/Theater/<t>/names.json)")
    args = ap.parse_args()

    out = args.out or os.path.join("Data", "Theater", args.theater,
                                   "names.json")
    names = read_names(args.install, args.theater)

    doc = {
        "format": "f4.theater.names/1",
        "theater": args.theater,
        "count": len(names),
        "names": names,
    }
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"{out}: {len(names)} names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
