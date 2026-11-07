#!/usr/bin/env python3
"""Stage 2: build the SD card layout from the user's own game files.

Output (copy <out>/ to sdmc:/switch/cod_st/):
    game.apk, game.obb     the original containers, paths mapped by src/vfs/vfs.c
    lib/*.so               ARMv7 libraries for the loader (gdbserver is skipped)

--store rewrites the OBB with every entry uncompressed.  It doubles the size to
about 2 GB but removes inflate work on the CPU during level loads.  Every entry's
CRC is checked against the extraction manifest either way.

Game data stays local: the build never bundles it into the NSP, and dist/ must not
be redistributed.

usage: package.py <gamedata_dir> <apk> <obb> <out_dir> [--store]
"""
import csv
import pathlib
import shutil
import sys
import zipfile

CHUNK = 1 << 20


def expected(manifest: pathlib.Path, origin: str) -> dict:
    with open(manifest, encoding="utf-8") as f:
        return {r["path"]: (int(r["size"]), int(r["crc32"], 16))
                for r in csv.DictReader(f) if r["origin"] == origin}


def verify_zip(path: pathlib.Path, want: dict) -> None:
    with zipfile.ZipFile(path) as z:
        got = {i.filename: (i.file_size, i.CRC) for i in z.infolist() if not i.is_dir()}
        if got != want:
            raise SystemExit(f"{path.name}: entries differ from manifest "
                             f"(missing {len(want.keys() - got.keys())}, extra {len(got.keys() - want.keys())})")
        bad = z.testzip()  # recomputes every CRC from the data on disk
        if bad:
            raise SystemExit(f"{path.name}: bad CRC in {bad}")
    print(f"verified {path.name}: {len(got)} entries, data CRCs ok")


def repack_stored(src: pathlib.Path, dst: pathlib.Path) -> None:
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED, allowZip64=True) as zout:
        for info in zin.infolist():
            out = zipfile.ZipInfo(info.filename, info.date_time)
            out.compress_type = zipfile.ZIP_STORED
            out.file_size = info.file_size  # Zip64 not needed for any single entry
            with zin.open(info) as r, zout.open(out, "w") as w:
                shutil.copyfileobj(r, w, CHUNK)


def main() -> None:
    args = [a for a in sys.argv[1:] if a != "--store"]
    store = "--store" in sys.argv
    gamedata, apk, obb, out = map(pathlib.Path, args)
    out.mkdir(parents=True, exist_ok=True)
    manifest = gamedata / "extract_manifest.csv"

    shutil.copyfile(apk, out / "game.apk")
    verify_zip(out / "game.apk", expected(manifest, "apk"))

    if store:
        repack_stored(obb, out / "game.obb")
    else:
        shutil.copyfile(obb, out / "game.obb")
    verify_zip(out / "game.obb", expected(manifest, "obb"))

    lib = out / "lib"
    lib.mkdir(exist_ok=True)
    for so in sorted((gamedata / "apk/lib/armeabi-v7a").glob("*.so")):
        shutil.copyfile(so, lib / so.name)
        print("lib", so.name)
    print("size", sum(p.stat().st_size for p in out.rglob("*") if p.is_file()))


if __name__ == "__main__":
    main()
