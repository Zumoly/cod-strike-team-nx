#!/usr/bin/env python3
"""Stage 1/2: classify the extracted Unity data tree.

Reports Unity version, serialized-file headers, split-file groups (and whether
each group is complete), and path problems relevant to Horizon: case-folding
collisions, overlong names, non-ASCII names, APK/OBB overlaps.

usage: analyze_assets.py <gamedata_dir>
"""
import collections
import pathlib
import re
import struct
import sys

root = pathlib.Path(sys.argv[1])
apk_data = root / "apk" / "assets" / "bin" / "Data"
obb_data = root / "obb" / "assets" / "bin" / "Data"


def serialized_header(p: pathlib.Path):
    """Unity serialized files start with big-endian metadataSize, fileSize, version, dataOffset."""
    with open(p, "rb") as f:
        h = f.read(64)
    if len(h) < 20:
        return None
    meta, size, ver, off = struct.unpack(">IIII", h[:16])
    if not (5 <= ver <= 30) or off > size or meta > size:
        return None
    pos = 20 if ver >= 9 else 16
    end = h.find(b"\0", pos)
    vs = h[pos:end].decode("ascii", "replace") if end > 0 else ""
    if not re.match(r"^\d+\.\d+", vs):
        vs = ""
    return dict(meta=meta, size=size, ver=ver, off=off, unity=vs)


files = [p for p in root.rglob("*") if p.is_file() and p.name != "extract_manifest.csv"]
print("files", len(files), "bytes", sum(p.stat().st_size for p in files))

print("\n== settings / small text ==")
for name in ("settings.xml", "PlayerConnectionConfigFile"):
    p = apk_data / name
    print(name, repr(p.read_bytes()[:300]))

print("\n== named (non-GUID) entries in OBB Data ==")
guid = re.compile(r"^[0-9a-f]{32}")
named = collections.Counter()
for p in obb_data.rglob("*"):
    if p.is_file() and not guid.match(p.name):
        named[re.sub(r"\.split\d+$", ".splitN", str(p.relative_to(obb_data)))] += 1
for k, v in sorted(named.items())[:60]:
    print(f"  {v:>4}  {k}")
print("  distinct", len(named))
print("\n== OBB top-level dirs ==")
print(collections.Counter(str(p.relative_to(root / "obb")).split("\\")[0:3].__str__() for p in (root / "obb").rglob("*") if p.is_file()).most_common(12))

print("\n== split groups ==")
groups = collections.defaultdict(dict)
for p in obb_data.rglob("*"):
    m = re.match(r"^(.*)\.split(\d+)$", p.name)
    if m and p.is_file():
        groups[p.parent / m.group(1)][int(m.group(2))] = p
gaps = 0
total = 0
for base, parts in groups.items():
    total += sum(q.stat().st_size for q in parts.values())
    if sorted(parts) != list(range(len(parts))):
        gaps += 1
        print("  GAP", base.name, sorted(parts))
print("  groups", len(groups), "bytes", total, "incomplete", gaps)
print("  count histogram (top)", collections.Counter(len(v) for v in groups.values()).most_common(8))

print("\n== serialized headers ==")
versions = collections.Counter()
kinds = collections.Counter()
bad_size = []
samples = []
for p in sorted(obb_data.iterdir()) + sorted(apk_data.iterdir()):
    if not p.is_file() or re.search(r"\.split[1-9]\d*$", p.name):
        continue
    h = serialized_header(p)
    if h is None:
        kinds["not-serialized:" + (p.suffix or "(none)")] += 1
        continue
    kinds["serialized"] += 1
    versions[(h["ver"], h["unity"])] += 1
    if p.name.endswith(".split0"):
        parts = groups[p.parent / p.name[: -len(".split0")]]
        real = sum(q.stat().st_size for q in parts.values())
    else:
        real = p.stat().st_size
    if real != h["size"]:
        bad_size.append((p.name, h["size"], real))
    if len(samples) < 6:
        samples.append((p.name, h))
print(" ", kinds.most_common(12))
print(" ", versions.most_common(8))
print("  header-size mismatches", len(bad_size), bad_size[:8])
for s in samples:
    print("  ", s)

print("\n== magic of non-serialized ==")
magics = collections.Counter()
for p in obb_data.iterdir():
    if p.is_file() and serialized_header(p) is None and not re.search(r"\.split[1-9]\d*$", p.name):
        with open(p, "rb") as f:
            magics[(p.suffix, f.read(8).hex())] += 1
print(" ", magics.most_common(15))

print("\n== Horizon path checks ==")
rels = [str(p.relative_to(root)).replace("\\", "/") for p in files]
print("  longest path", max(map(len, rels)), "longest component", max(len(c) for r in rels for c in r.split("/")))
print("  non-ascii", [r for r in rels if not r.isascii()][:10])
print("  with spaces", [r for r in rels if " " in r][:10])
print("  uppercase in GUID names", sum(1 for p in files if re.match(r"^[0-9A-F]*[A-F][0-9A-F]*$", p.stem)))
fold = collections.defaultdict(list)
for r in rels:
    fold[r.lower()].append(r)
print("  case-fold collisions", [v for v in fold.values() if len(v) > 1][:10])
apk_rel = {str(p.relative_to(root / "apk")).lower() for p in files if p.is_relative_to(root / "apk")}
obb_rel = {str(p.relative_to(root / "obb")).lower() for p in files if p.is_relative_to(root / "obb")}
print("  apk/obb overlaps", sorted(apk_rel & obb_rel)[:20], len(apk_rel & obb_rel))
print("  files in dir Data (obb)", sum(1 for _ in obb_data.iterdir()))
