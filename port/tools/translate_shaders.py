#!/usr/bin/env python3
"""Stage 3: offline GLES2 -> deko3D shader pipeline.

Unity hands the driver GLSL ES source at runtime; deko3D only consumes compiled
DKSH modules and has no loose uniforms.  This rewrites each program so uam can
compile it, and records what the GL layer needs to emulate the old API:
  * loose uniforms -> one std140 block per stage (binding 0), with byte offsets
    for the glUniform* shadow buffer
  * attributes/varyings -> explicit locations, samplers -> explicit bindings
  * legacy texture functions -> GLSL 4.x equivalents

usage: translate_shaders.py <gles_dir> <out_dir> [<shaders.pak>]
Writes <out_dir>/<hash>.{vert,frag}.{glsl,dksh} and <out_dir>/shader_db.json.

shaders.pak is what the console loads.  Unity submits source at runtime, so each program is
found by a hash of its two stages (program_key must match pak_key in src/gl/gles.c):
  header  "CSP1", count, code_offset, code_size
  entries key u64, record offset u32, pad u32, sorted by key
  records vs control offset/size, vs code offset, the same for fs, vs and fs block sizes,
          attribute, sampler and uniform counts (u16), pad, then the name tables
  blobs   DKSH control sections, then the code region, each program 0x100 aligned
"""
import concurrent.futures as cf
import collections
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys

PRELUDE = """#version 450 core
#define texture2D texture
#define texture2DProj textureProj
#define textureCube texture
#define texture2DLodEXT textureLod
#define textureCubeLodEXT textureLod
#define shadow2DEXT texture
#define shadow2DProjEXT textureProj
"""

SIZE = {  # base alignment, size, array/matrix-column stride rules follow std140
    "float": (4, 4), "int": (4, 4), "bool": (4, 4),
    "vec2": (8, 8), "ivec2": (8, 8), "bvec2": (8, 8),
    "vec3": (16, 12), "ivec3": (16, 12), "bvec3": (16, 12),
    "vec4": (16, 16), "ivec4": (16, 16), "bvec4": (16, 16),
    "mat2": (16, 32), "mat3": (16, 48), "mat4": (16, 64),
}
UNIFORM = re.compile(r"^[ \t]*uniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[(\d+)\])?\s*;[ \t]*\r?$", re.M)
ATTR = re.compile(r"^[ \t]*attribute\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;[ \t]*\r?$", re.M)
VARY = re.compile(r"^[ \t]*varying\s+((?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(\[\d+\])?\s*;[ \t]*\r?$", re.M)


def split_stages(src):
    """Unity guards each stage with #ifdef VERTEX/FRAGMENT at top level."""
    text = src.partition("\n")[2] if src.startswith("!!") else src
    out = {}
    for name in ("VERTEX", "FRAGMENT"):
        m = re.search(rf"^[ \t]*#ifdef {name}[ \t]*\r?$", text, re.M)
        if not m:
            return None
        depth, pos = 1, m.end()
        for t in re.finditer(r"^[ \t]*#\s*(if|ifdef|ifndef|endif)\b.*$", text[pos:], re.M):
            depth += -1 if t.group(1) == "endif" else 1
            if depth == 0:
                out[name] = text[pos:pos + t.start()]
                break
        else:
            return None
    return out["VERTEX"], out["FRAGMENT"]


def normalize(stage):
    """Unity drops some precision lines and all layout at runtime, so neither is hashed."""
    return re.sub(r"\s", "", re.sub(r"precision\s[^;]*;", "", stage))


def fnv1a64(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def program_key(vs, fs):
    return fnv1a64((normalize(vs) + "|" + normalize(fs)).encode("latin1"))


def std140(uniforms):
    off, table = 0, []
    for typ, name, count in uniforms:
        align, size = SIZE[typ]
        if count:
            align, size = 16, max(16, (size + 15) & ~15) * count
        off = (off + align - 1) & ~(align - 1)
        table.append({"name": name, "type": typ, "count": count or 1, "offset": off})
        off += size
    return table, (off + 15) & ~15


def rewrite(stage_src, is_vertex, varyings, samplers, attrs):
    body = re.sub(r"^[ \t]*#extension.*$", "", stage_src, flags=re.M)
    body = re.sub(r"^[ \t]*precision\s+\w+\s+\w+\s*;.*$", "", body, flags=re.M)
    uniforms = []

    def on_uniform(m):
        typ, name, count = m.group(1), m.group(2), m.group(3)
        if typ == "sampler":  # invalid in every GLSL version; one shipped variant has it
            typ = "sampler2D"
        if typ.startswith("sampler"):
            b = samplers.setdefault(name, len(samplers))
            return f"layout(binding={b}) uniform {typ} {name};"
        uniforms.append((typ, name, int(count) if count else 0))
        return ""

    body = UNIFORM.sub(on_uniform, body)

    def on_attr(m):
        loc = attrs.setdefault(m.group(2), len(attrs))
        return f"layout(location={loc}) in {m.group(1)} {m.group(2)};"

    body = ATTR.sub(on_attr, body)

    def on_vary(m):
        loc = varyings[m.group(3)]
        q = "out" if is_vertex else "in"
        return f"layout(location={loc}) {q} {m.group(2)} {m.group(3)}{m.group(4) or ''};"

    body = VARY.sub(on_vary, body)

    head = PRELUDE
    if uniforms:
        table, size = std140(uniforms)
        fields = "".join(f"  {t} {n}{f'[{c}]' if c else ''};\n" for t, n, c in uniforms)
        head += f"layout(std140, binding=0) uniform StageUniforms {{\n{fields}}};\n"
    else:
        table, size = [], 0
    if not is_vertex:
        head += "layout(location=0) out vec4 port_FragColor;\n"
        body = re.sub(r"\bgl_FragData\s*\[\s*0\s*\]", "port_FragColor", body)
        body = re.sub(r"\bgl_FragColor\b", "port_FragColor", body)
    return head + body, table, size


def translate(path, out):
    src = path.read_text(encoding="latin1")
    if src.startswith("!!GLES3"):
        return path.stem, "skip-gles3", ""
    stages = split_stages(src)
    if stages is None:
        return path.stem, "skip-nostages", ""
    vs, fs = stages
    names = sorted({m.group(3) for s in stages for m in VARY.finditer(s)})
    varyings = {n: i for i, n in enumerate(names)}
    samplers, attrs = {}, {}
    vtext, vuni, vsize = rewrite(vs, True, varyings, samplers, attrs)
    ftext, funi, fsize = rewrite(fs, False, varyings, samplers, attrs)

    meta = {"attributes": attrs, "samplers": samplers, "vs_uniforms": vuni, "vs_size": vsize,
            "fs_uniforms": funi, "fs_size": fsize}
    for stage, text in (("vert", vtext), ("frag", ftext)):
        g = out / f"{path.stem}.{stage}.glsl"
        g.write_text(text, encoding="latin1", newline="\n")
        r = subprocess.run(["uam", "-s", stage, "-o", str(g.with_suffix(".dksh")), str(g)],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            return path.stem, f"fail-{stage}", (r.stderr + r.stdout).strip()
    meta["dksh_bytes"] = sum((out / f"{path.stem}.{s}.dksh").stat().st_size for s in ("vert", "frag"))
    meta["key"] = program_key(vs, fs)
    return path.stem, "ok", meta


FALLBACK_SOURCE = 0xB1A78426FC940A31
FALLBACK_KEY = 1


def split_dksh(data):
    magic, header, control, code = struct.unpack_from("<4sIII", data)
    if magic != b"DKSH":
        raise ValueError("not a DKSH file")
    return data[:control], data[control:control + code]


def name_field(name):
    raw = name.encode("latin1")
    if len(raw) > 43:
        raise ValueError(f"name too long: {name}")
    return raw.ljust(44, b"\0")


def pack(db, out, path):
    """Stages shared between programs are stored once."""
    controls, codes = {}, {}
    control_blob, code_blob = bytearray(), bytearray()

    def add(data, table, blob, align):
        h = hashlib.sha1(data).digest()
        if h not in table:
            blob.extend(b"\0" * (-len(blob) % align))
            table[h] = len(blob)
            blob.extend(data)
        return table[h]

    unique = {}
    for stem, m in db.items():
        if m["key"] in unique and unique[m["key"]][1] != m:
            print("note: same key twice", stem)
        unique.setdefault(m["key"], (stem, m))

    # The stand-in for programs Unity generates per material: one texture times vertex colour.
    if FALLBACK_SOURCE in unique:
        unique[FALLBACK_KEY] = unique[FALLBACK_SOURCE]

    records = []
    for key in sorted(unique):
        stem, m = unique[key]
        spans = []
        for s in ("vert", "frag"):
            ctrl, code = split_dksh((out / f"{stem}.{s}.dksh").read_bytes())
            spans += [add(ctrl, controls, control_blob, 16), len(ctrl), add(code, codes, code_blob, 0x100)]
        r = struct.pack("<8I4H", *spans, m["vs_size"], m["fs_size"], len(m["attributes"]), len(m["samplers"]),
                        len(m["vs_uniforms"]) + len(m["fs_uniforms"]), 0)
        for name, loc in m["attributes"].items():
            r += name_field(name) + struct.pack("<I", loc)
        for name, binding in m["samplers"].items():
            r += name_field(name) + struct.pack("<I", binding)
        for stage, table in ((0, m["vs_uniforms"]), (1, m["fs_uniforms"])):
            for u in table:
                r += name_field(u["name"]) + struct.pack("<HH", stage, u["offset"])
        records.append((key, r))

    table_size = 16 + 16 * len(records)
    at = table_size
    entries, body = bytearray(), bytearray()
    for key, r in records:
        entries += struct.pack("<QII", key, at, 0)
        body += r
        at += len(r)
    # Control offsets are relative to the file, code offsets to the code region.
    control_base = at + (-at % 16)
    fixed = bytearray()
    pos = 0
    for key, r in records:
        rr = bytearray(r)
        for i in (0, 3):
            v = struct.unpack_from("<I", rr, i * 4)[0]
            struct.pack_into("<I", rr, i * 4, v + control_base)
        fixed += rr
    code_base = control_base + len(control_blob)
    code_base += -code_base % 0x1000
    data = b"CSP1" + struct.pack("<III", len(records), code_base, len(code_blob))
    data += bytes(entries) + bytes(fixed)
    data += b"\0" * (control_base - len(data)) + bytes(control_blob)
    data += b"\0" * (code_base - len(data)) + bytes(code_blob)
    pathlib.Path(path).write_bytes(data)
    print(f"pak: {len(records)} programs, {len(control_blob)} control, {len(code_blob)} code, {len(data)} bytes")


def main():
    indir, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    with cf.ThreadPoolExecutor(8) as ex:
        results = list(ex.map(lambda p: translate(p, out), sorted(indir.glob("*.glsl"))))
    counts = collections.Counter(r[1] for r in results)
    print(counts.most_common())
    db = {h: m for h, s, m in results if s == "ok"}
    (out / "shader_db.json").write_text(json.dumps(db, indent=0), encoding="utf-8")
    fails = [(h, s, m) for h, s, m in results if s.startswith("fail")]
    reasons = collections.Counter()
    for _, _, m in fails:
        lines = [l for l in m.splitlines() if "error" in l.lower()]
        reasons[re.sub(r"\d+", "N", lines[0])[:110] if lines else m[:110]] += 1
    for r, n in reasons.most_common(15):
        print(f"  {n:>4}  {r}")
    if fails:
        print("sample failure", fails[0][0], fails[0][2][:400])
    if db:
        ok = list(db.values())
        print("max VS uniform block", max(m["vs_size"] for m in ok), "FS", max(m["fs_size"] for m in ok),
              "max samplers", max(len(m["samplers"]) for m in ok),
              "total dksh bytes", sum(m["dksh_bytes"] for m in ok))
        print("attributes", collections.Counter(a for m in ok for a in m["attributes"]).most_common())
    if len(sys.argv) > 3 and not fails:
        pack(db, out, sys.argv[3])
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
