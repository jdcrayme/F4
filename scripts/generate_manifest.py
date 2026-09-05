#!/usr/bin/env python3
"""Generate Data/manifest.json — provenance + content fingerprints for staleness detection.
Tranche 0a.2 (NO_BINARY_RUNTIME_PLAN.md). The manifest is the contract (spec P7)."""
import argparse, hashlib, json, os, sys, time
from pathlib import Path

def fnv1a_64(data: bytes) -> str:
    h = 14695981039346656037
    for b in data:
        h ^= b; h *= 1099511628211
        h &= 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""): h.update(chunk)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--install", required=True, type=Path)
    ap.add_argument("--theater", default="korea")
    ap.add_argument("--save", default="save1")
    ap.add_argument("--exclude", action="append", default=[],
                    help="top-level dir under --output to skip (repeatable), "
                         "e.g. Models while it stays gitignored")
    args = ap.parse_args()

    excludes = set(args.exclude)
    # The manifest lives in the f4 envelope ({"f4": {"v": 1}, ...}) so the
    # runtime readers — f4import check, doctor D9 — can parse it. Unknown
    # top-level keys and unknown per-entry fields are skipped by the f4
    # reader; the fingerprint entries carry no "id", so family-specific
    # checks (D1) ignore them.
    manifest = {
        "f4": {"v": 1},
        "format": "f4-data-manifest",
        "version": 1,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_install": str(args.install),
        "theater": args.theater,
        "save": args.save,
        "data_dir": "Data/",
        # Declares to the doctor (D8) which top-level dirs are
        # intentionally unlisted — e.g. Models while it stays gitignored.
        "excluded_dirs": sorted(excludes),
        "assets": []
    }

    for p in sorted(args.output.rglob("*")):
        if not p.is_file() or p.name == "manifest.json": continue
        rel = p.relative_to(args.output)
        if rel.parts[0] in excludes: continue
        manifest["assets"].append({
            "path": rel.as_posix(),
            "size_bytes": p.stat().st_size,
            "sha256": sha256(p),
            "fnv1a_64": fnv1a_64(p.read_bytes()),
        })

    out = args.output / "manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manifest: {len(manifest['assets'])} assets -> {out}")

if __name__ == "__main__":
    main()
