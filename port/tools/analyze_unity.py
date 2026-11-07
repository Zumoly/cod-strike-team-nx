#!/usr/bin/env python3
"""Stage 3: parse Unity 4.3 serialized files (format v9) from the extracted data.

Reports the class histogram, texture formats and shader program mix, and writes
each unique GLES subprogram to <out>/gles/<hash>.glsl for the shader pipeline.
The output is game data and stays local.

usage: analyze_unity.py <gamedata_dir> <out_dir>
"""
import collections
import hashlib
import pathlib
import re
import struct
import sys

CLASS = {1: "GameObject", 4: "Transform", 20: "Camera", 21: "Material", 23: "MeshRenderer", 28: "Texture2D",
         33: "MeshFilter", 43: "Mesh", 48: "Shader", 49: "TextAsset", 54: "Rigidbody", 64: "MeshCollider",
         65: "BoxCollider", 74: "AnimationClip", 82: "AudioSource", 83: "AudioClip", 84: "RenderTexture",
         89: "Cubemap", 90: "Avatar", 91: "AnimatorController", 95: "Animator", 96: "TrailRenderer",
         102: "TextMesh", 108: "Light", 111: "Animation", 114: "MonoBehaviour", 115: "MonoScript",
         128: "Font", 136: "CharacterController", 137: "SkinnedMeshRenderer", 196: "NavMeshSettings",
         198: "ParticleSystem", 199: "ParticleSystemRenderer", 205: "LODGroup", 212: "SpriteRenderer",
         213: "Sprite", 218: "Terrain", 221: "AnimatorOverrideController", 238: "NavMeshData"}
TEXFMT = {1: "Alpha8", 2: "ARGB4444", 3: "RGB24", 4: "RGBA32", 5: "ARGB32", 7: "RGB565", 10: "DXT1", 12: "DXT5",
          13: "RGBA4444", 30: "PVRTC_RGB2", 31: "PVRTC_RGBA2", 32: "PVRTC_RGB4", 33: "PVRTC_RGBA4",
          34: "ETC_RGB4", 35: "ATC_RGB4", 36: "ATC_RGBA8", 45: "ETC2_RGB", 46: "ETC2_RGBA1", 47: "ETC2_RGBA8"}


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def cstr(self):
        e = self.b.index(b"\0", self.p)
        s = self.b[self.p:e]
        self.p = e + 1
        return s.decode("latin1")

    def lstr(self):
        n = self.i32()
        s = self.b[self.p:self.p + n]
        self.p += n
        self.p = (self.p + 3) & ~3
        return s

    def align(self):
        self.p = (self.p + 3) & ~3


def skip_tree(r):
    r.cstr(); r.cstr()
    r.p += 7 * 4 - 8 + 8  # size, index, isArray, version, flags (5 ints) + 0
    r.p = r.p  # kept explicit: 5 ints were consumed above with the +20 below
    kids = r.i32()
    for _ in range(kids):
        skip_tree(r)


def objects(blob):
    meta, size, ver, off = struct.unpack_from(">IIII", blob, 0)
    r = R(blob, 20)
    r.cstr()
    r.u32()  # platform
    types = r.i32()
    for _ in range(types):
        r.i32()
        r.cstr(); r.cstr()
        r.p += 20
        kids = r.i32()

        def rec(n):
            for _ in range(n):
                r.cstr(); r.cstr()
                r.p += 20
                rec(r.i32())
        rec(kids)
    r.i32()  # bigIDEnabled
    count = r.i32()
    r.align() if False else None
    out = []
    for _ in range(count):
        pid, start, sz, tid = r.i32(), r.u32(), r.u32(), r.i32()
        cid = struct.unpack_from("<h", blob, r.p)[0]
        r.p += 4
        out.append((pid, start + off, sz, cid))
    return ver, types, out


def main():
    root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    data_dirs = [root / "obb/assets/bin/Data", root / "apk/assets/bin/Data"]
    (out / "gles").mkdir(parents=True, exist_ok=True)

    files = {}
    for d in data_dirs:
        for p in d.iterdir():
            if not p.is_file() or p.suffix in (".resS", ".xml", ".png") or p.name == "PlayerConnectionConfigFile":
                continue
            m = re.match(r"(.*)\.split(\d+)$", p.name)
            if m:
                files.setdefault(m.group(1), []).append((int(m.group(2)), p))
            else:
                files.setdefault(p.name, []).append((0, p))
    print("serialized files", len(files))

    classes = collections.Counter()
    class_bytes = collections.Counter()
    fmts = collections.Counter()
    fmt_bytes = collections.Counter()
    sizes = collections.Counter()
    shader_names = set()
    subprog = collections.Counter()
    unique = {}
    trees = 0
    errors = []
    for name, parts in files.items():
        blob = b"".join(p.read_bytes() for _, p in sorted(parts))
        try:
            ver, types, objs = objects(blob)
        except Exception as e:  # parsing must not abort the survey; failures are reported
            errors.append((name, repr(e)))
            continue
        trees += types > 0
        for pid, start, sz, cid in objs:
            classes[cid] += 1
            class_bytes[cid] += sz
            try:
                if cid == 28:
                    r = R(blob, start)
                    r.lstr()
                    w, h, comp, fmt = r.i32(), r.i32(), r.i32(), r.i32()
                    fmts[fmt] += 1
                    fmt_bytes[fmt] += comp
                    sizes[max(w, h)] += 1
                elif cid == 48:
                    r = R(blob, start)
                    r.lstr()
                    text = r.lstr()
                    m = re.search(rb'Shader "([^"]+)"', text)
                    shader_names.add(m.group(1).decode("latin1") if m else "?")
                    for sm in re.finditer(rb'SubProgram "([a-z0-9_ ]+)"', text):
                        subprog[sm.group(1).decode().strip()] += 1
                    for gm in re.finditer(rb'SubProgram "gles[^"]*"[^{]*\{(.*?)"(!!GLES.*?)"\s*\}', text, re.S):
                        src = gm.group(2).replace(b'\\"', b'"')
                        h = hashlib.sha1(src).hexdigest()[:16]
                        unique.setdefault(h, src)
            except Exception as e:
                errors.append((name, cid, repr(e)))

    print("files with embedded type trees", trees)
    print("parse errors", len(errors), errors[:5])
    print("\nclass histogram (by object count)")
    for c, n in classes.most_common(34):
        print(f"  {n:>8}  {class_bytes[c]:>12}  {CLASS.get(c, c)}")
    print("\ntexture formats")
    for f, n in fmts.most_common():
        print(f"  {n:>6}  {fmt_bytes[f]:>12}  {TEXFMT.get(f, f)}")
    print("  max dimension histogram", sorted(sizes.items())[-6:])
    print("\nshaders", len(shader_names), "subprograms by API", subprog.most_common(12))
    print("unique GLES programs", len(unique))
    for h, s in unique.items():
        (out / "gles" / f"{h}.glsl").write_bytes(s)


if __name__ == "__main__":
    main()
