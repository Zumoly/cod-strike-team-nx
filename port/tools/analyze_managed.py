#!/usr/bin/env python3
"""Stage 3: read CLI metadata from the game's managed DLLs.

Lists P/Invoke targets (ImplMap + ModuleRef) and the Android/Java surface reached
from C# (user strings naming Java classes).  Stdlib only.

usage: analyze_managed.py <Managed_dir>
"""
import collections
import pathlib
import re
import struct
import sys

# Row layout per table, using codes: 2/4 fixed, S/G/B heap, tN simple index, cX coded index.
CODED = {
    "TypeDefOrRef": ([2, 1, 27], 2),
    "HasConstant": ([4, 8, 23], 2),
    "HasCustomAttribute": ([6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17, 26, 27, 32, 35, 38, 39, 40], 5),
    "HasFieldMarshal": ([4, 8], 1),
    "HasDeclSecurity": ([2, 6, 32], 2),
    "MemberRefParent": ([2, 1, 26, 6, 27], 3),
    "HasSemantics": ([20, 23], 1),
    "MethodDefOrRef": ([6, 10], 1),
    "MemberForwarded": ([4, 6], 1),
    "Implementation": ([38, 35, 39], 2),
    "CustomAttributeType": ([0, 0, 6, 10, 0], 3),
    "ResolutionScope": ([0, 26, 35, 1], 2),
    "TypeOrMethodDef": ([2, 6], 1),
}
SCHEMA = {
    0: "2 S G G G", 1: "cResolutionScope S S", 2: "4 S S cTypeDefOrRef t4 t6", 3: "t4", 4: "2 S B", 5: "t6",
    6: "4 2 2 S B t8", 7: "t8", 8: "2 2 S", 9: "t2 cTypeDefOrRef", 10: "cMemberRefParent S B",
    11: "2 cHasConstant B", 12: "cHasCustomAttribute cCustomAttributeType B", 13: "cHasFieldMarshal B",
    14: "2 cHasDeclSecurity B", 15: "2 4 t2", 16: "4 t4", 17: "B", 18: "t2 t20", 19: "t20", 20: "2 S cTypeDefOrRef",
    21: "t2 t23", 22: "t23", 23: "2 S B", 24: "2 t6 cHasSemantics", 25: "t2 cMethodDefOrRef cMethodDefOrRef",
    26: "S", 27: "B", 28: "2 cMemberForwarded S t26", 29: "4 t4", 30: "4 4", 31: "4", 32: "4 2 2 2 2 4 B S S",
    33: "4", 34: "4 4 4", 35: "2 2 2 2 4 B S S B", 36: "4 t35", 37: "4 4 4 t35", 38: "4 S B",
    39: "4 4 S S cImplementation", 40: "4 4 S cImplementation", 41: "t2 t2", 42: "2 2 cTypeOrMethodDef S",
    43: "cMethodDefOrRef B", 44: "t42 cTypeDefOrRef",
}


def parse(path):
    d = path.read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", d, pe + 24 + opt + i * 40 + 8) for i in range(nsec)]

    def rva(v):
        for vs, va, rs, rp in secs:
            if va <= v < va + max(vs, rs):
                return v - va + rp
        raise ValueError(v)

    magic = struct.unpack_from("<H", d, pe + 24)[0]
    dd = pe + 24 + (96 if magic == 0x10B else 112) + 14 * 8
    cli = rva(struct.unpack_from("<I", d, dd)[0])
    meta = rva(struct.unpack_from("<I", d, cli + 8)[0])
    vlen = struct.unpack_from("<I", d, meta + 12)[0]
    p = meta + 16 + vlen + 2
    n = struct.unpack_from("<H", d, p)[0]
    p += 2
    heaps = {}
    for _ in range(n):
        o, s = struct.unpack_from("<II", d, p)
        p += 8
        e = d.index(b"\0", p)
        name = d[p:e].decode()
        p = (e + 4) & ~3
        heaps[name] = (meta + o, s)

    t = heaps["#~"][0]
    hs = d[t + 6]
    S, G, B = (4 if hs & f else 2 for f in (1, 2, 4))
    valid = struct.unpack_from("<Q", d, t + 8)[0]
    p = t + 24
    rows = {}
    for i in range(64):
        if valid >> i & 1:
            rows[i] = struct.unpack_from("<I", d, p)[0]
            p += 4

    def width(code):
        if code in "24":
            return int(code)
        if code in "SGB":
            return {"S": S, "G": G, "B": B}[code]
        if code[0] == "t":
            return 2 if rows.get(int(code[1:]), 0) < 1 << 16 else 4
        tables, bits = CODED[code[1:]]
        return 2 if max(rows.get(x, 0) for x in tables) < 1 << (16 - bits) else 4

    offs = {}
    for i in sorted(rows):
        w = [width(c) for c in SCHEMA[i].split()]
        offs[i] = (p, w)
        p += sum(w) * rows[i]

    def row(i, k):
        o, w = offs[i]
        o += sum(w) * k
        out = []
        for x in w:
            out.append(int.from_bytes(d[o:o + x], "little"))
            o += x
        return out

    def s(i):
        o = heaps["#Strings"][0] + i
        return d[o:d.index(b"\0", o)].decode("utf-8", "replace")

    mods = [s(row(26, k)[0]) for k in range(rows.get(26, 0))]
    pinv = collections.defaultdict(list)
    for k in range(rows.get(28, 0)):
        _, _, name, scope = row(28, k)
        pinv[mods[scope - 1]].append(s(name))

    us_off, us_size = heaps.get("#US", (0, 0))
    us = []
    q = us_off + 1
    end = us_off + us_size
    while q < end:
        b = d[q]
        if b == 0:
            break
        if b < 0x80:
            ln, q = b, q + 1
        elif b < 0xC0:
            ln, q = ((b & 0x3F) << 8) | d[q + 1], q + 2
        else:
            ln, q = ((b & 0x1F) << 24) | d[q + 1] << 16 | d[q + 2] << 8 | d[q + 3], q + 4
        us.append(d[q:q + max(ln - 1, 0)].decode("utf-16le", "replace"))
        q += ln
    return pinv, us


def main():
    total = collections.defaultdict(set)
    java = set()
    urls = set()
    for p in sorted(pathlib.Path(sys.argv[1]).glob("*.dll")):
        pinv, us = parse(p)
        if p.name in ("mscorlib.dll", "System.dll", "System.Core.dll", "System.Xml.dll", "Mono.Security.dll"):
            continue
        for m, f in pinv.items():
            total[m].update(f)
        for s in us:
            if re.match(r"^(com|android|org|java)[./][a-zA-Z0-9_./$]+$", s):
                java.add(s)
            if re.match(r"^https?://", s):
                urls.add(s)
    print("P/Invoke modules (game assemblies):")
    for m, f in sorted(total.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(f):>5}  {m}: {sorted(f)[:14]}")
    print("Java classes named from C#:", len(java))
    for j in sorted(java)[:50]:
        print("  ", j)
    print("URLs:", sorted(urls)[:20])


if __name__ == "__main__":
    main()
