#!/usr/bin/env python3
"""Generate Data/manifest.json — provenance + content fingerprints for staleness detection.
Tranche 0a.2 (NO_BINARY_RUNTIME_PLAN.md). The manifest is the contract (spec P7).

Task 58 (Tranche 0e): entries now carry an explicit "id" (the
"@asset:<family>:<local-id>" identity) in addition to the fingerprints.
The derivation below MIRRORS the C++ legacy fallback in
f4-assets/src/manifest.cpp (derive_id_from_path) — keep the two in sync.
"""
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

def derive_asset_id(rel_posix: str, theater: str, save: str) -> str:
    """Derive the @asset: id for an exported file. MIRRORS
    f4-assets/src/manifest.cpp derive_id_from_path — keep in sync:
      Aircraft/<stem>.json       -> aircraft:<stem-lowercased>
      Classes/<name>.json        -> class:<name minus trailing .json>
      SimData/<stem>.json        -> simdata:<stem-lowercased>
      Theater/<t>/<file>         -> theater:<t>
      World/<stem>.world.json    -> campaign:<save, else stem minus .world>
      Models/koreaobj/<N>.gltf   -> koreaobj:<N>
    Returns "" when the path matches no convention (unaddressable)."""
    def lower(s): return s.lower()
    def stem(p):  # last component minus its final extension
        name = p.rsplit("/", 1)[-1]
        return name.rsplit(".", 1)[0]
    if rel_posix.startswith("Aircraft/"):
        return "aircraft:" + lower(stem(rel_posix))
    if rel_posix.startswith("Classes/"):
        name = rel_posix.rsplit("/", 1)[-1]
        if name.endswith(".json"): name = name[: -len(".json")]
        return "class:" + lower(name)
    if rel_posix.startswith("SimData/"):
        return "simdata:" + lower(stem(rel_posix))
    if rel_posix.startswith("Theater/"):
        rest = rel_posix[len("Theater/"):]
        t = lower(theater) if theater else lower(rest.split("/", 1)[0])
        return "theater:" + t
    if rel_posix.startswith("World/"):
        if save:
            return "campaign:" + lower(save)
        s = stem(rel_posix)  # "korea.world"
        if s.endswith(".world"): s = s[: -len(".world")]
        return "campaign:" + s
    if rel_posix.startswith("Models/koreaobj/"):
        return "koreaobj:" + stem(rel_posix)
    return ""

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
    # runtime readers — f4import check, doctor D9, f4-assets — can parse it.
    # Since Task 58 each entry carries an explicit "id" (the runtime's
    # @asset: handle) on top of the content fingerprints.
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
            "id": derive_asset_id(rel.as_posix(), args.theater, args.save),
            "path": rel.as_posix(),
            "size_bytes": p.stat().st_size,
            "sha256": sha256(p),
            "fnv1a_64": fnv1a_64(p.read_bytes()),
        })

    out = args.output / "manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    n_id = sum(1 for a in manifest["assets"] if a["id"])
    print(f"manifest: {len(manifest['assets'])} assets ({n_id} with ids) -> {out}")
    missing = [a["path"] for a in manifest["assets"] if not a["id"]]
    if missing:
        print(f"WARNING: {len(missing)} entries matched no id convention: {missing[:5]}")

if __name__ == "__main__":
    main()
