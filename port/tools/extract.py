#!/usr/bin/env python3
"""Stage 1: extract the user's APK and OBB into a game-data tree.

Writes a manifest (path, size, crc32) of everything extracted, so later stages
can prove nothing was lost.  CRCs are verified by zipfile on read.

usage: extract.py <apk> <obb> <out_dir>
"""
import csv
import pathlib
import shutil
import sys
import zipfile


def extract(archive: pathlib.Path, dest: pathlib.Path, rows: list, origin: str) -> None:
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = pathlib.PurePosixPath(info.filename).parts
            if info.filename.startswith("/") or ".." in parts:
                raise SystemExit(f"unsafe path in {archive.name}: {info.filename}")
            target = dest.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(target, "wb") as dst:  # CRC checked at EOF
                shutil.copyfileobj(src, dst, 1 << 20)
            if target.stat().st_size != info.file_size:
                raise SystemExit(f"size mismatch: {info.filename}")
            rows.append((origin, info.filename, info.file_size, f"{info.CRC:08x}"))


def main() -> None:
    apk, obb, out = map(pathlib.Path, sys.argv[1:4])
    rows: list = []
    extract(apk, out / "apk", rows, "apk")
    print("apk entries", len(rows))
    extract(obb, out / "obb", rows, "obb")
    print("total entries", len(rows))
    with open(out / "extract_manifest.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["origin", "path", "size", "crc32"])
        w.writerows(rows)
    print("bytes", sum(r[2] for r in rows))


if __name__ == "__main__":
    main()
