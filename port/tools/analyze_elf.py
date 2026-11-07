#!/usr/bin/env python3
"""Stage 3: stdlib-only ELF32 dynamic analysis of the Android .so files.

For each library: arch, DT_NEEDED, imports grouped by library family, JNI
exports, relocation types, TLS and init_array use.  Full import lists go to
<out_dir>/<lib>.imports.txt.

usage: analyze_elf.py <lib_dir> <out_dir>
"""
import collections
import pathlib
import re
import struct
import sys

PT_LOAD, PT_DYNAMIC, PT_TLS, PT_INTERP = 1, 2, 7, 3
DT = dict(NEEDED=1, PLTRELSZ=2, STRTAB=5, SYMTAB=6, RELA=7, REL=17, RELSZ=18, JMPREL=23,
          INIT=12, INIT_ARRAY=25, INIT_ARRAYSZ=27, HASH=4, SONAME=14, TEXTREL=22, FLAGS=30)
RELNAME = {2: "ABS32", 21: "GLOB_DAT", 22: "JUMP_SLOT", 23: "RELATIVE", 17: "TLS_DTPMOD32",
           18: "TLS_DTPOFF32", 19: "TLS_TPOFF32"}


def family(n: str) -> str:
    if n.startswith("gl") or n.startswith("egl"):
        return "gles/egl"
    if n.startswith("pthread_") or n.startswith("sem_") or n.startswith("__pthread"):
        return "pthread/sem"
    if n.startswith(("AL", "al")) or n.startswith("SL"):
        return "audio"
    if n.startswith("__android") or n.startswith("AAsset") or n.startswith("ANative"):
        return "android"
    if n.startswith("__aeabi") or n.startswith("__gnu") or n.startswith("__cxa") or n.startswith("__"):
        return "abi/runtime"
    if n.startswith(("mono_", "il2cpp")):
        return "mono"
    return "libc/other"


def analyze(path: pathlib.Path, outdir: pathlib.Path):
    d = path.read_bytes()
    assert d[:4] == b"\x7fELF"
    cls, en = d[4], d[5]
    e_type, e_machine = struct.unpack_from("<HH", d, 16)
    e_entry, e_phoff, e_shoff, e_flags = struct.unpack_from("<IIII", d, 24)
    phsize, phnum = struct.unpack_from("<HH", d, 42)
    print(f"\n=== {path.name} size={len(d)} class={cls} machine={e_machine} flags={e_flags:#x}")
    loads = []
    dyn = None
    tls = None
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, e_phoff + i * phsize)
        if t == PT_LOAD:
            loads.append((va, off, fsz, msz, fl))
        elif t == PT_DYNAMIC:
            dyn = (off, fsz)
        elif t == PT_TLS:
            tls = (va, fsz, msz)
    print("  LOAD", [(hex(v), hex(m), "RWX"[0] * bool(f & 4) + "W" * bool(f & 2) + "X" * bool(f & 1)) for v, _, _, m, f in loads])
    print("  TLS", tls)

    def v2o(v):
        for va, off, fsz, msz, fl in loads:
            if va <= v < va + fsz:
                return v - va + off
        raise ValueError(hex(v))

    ents = []
    for i in range(dyn[1] // 8):
        tag, val = struct.unpack_from("<II", d, dyn[0] + i * 8)
        if tag == 0:
            break
        ents.append((tag, val))
    D = collections.defaultdict(list)
    for t, v in ents:
        D[t].append(v)
    strtab = v2o(D[DT["STRTAB"]][0])

    def s(o):
        e = d.index(b"\0", strtab + o)
        return d[strtab + o:e].decode("latin1")

    print("  NEEDED", [s(x) for x in D[DT["NEEDED"]]])
    print("  TEXTREL", DT["TEXTREL"] in D, "INIT", D.get(DT["INIT"]), "INIT_ARRAYSZ", D.get(DT["INIT_ARRAYSZ"]))
    symtab = v2o(D[DT["SYMTAB"]][0])
    nsym = struct.unpack_from("<II", d, v2o(D[DT["HASH"]][0]))[1] if DT["HASH"] in D else 0
    imports, exports = [], []
    for i in range(1, nsym):
        nm, val, sz, info, oth, shndx = struct.unpack_from("<IIIBBH", d, symtab + i * 16)
        name = s(nm)
        (imports if shndx == 0 else exports).append(name)
    print("  dynsym", nsym, "imports", len(imports), "exports", len(exports))
    fam = collections.Counter(family(n) for n in imports)
    print("  import families", fam.most_common())
    rel = collections.Counter()
    for tagoff, tagsz in ((DT["REL"], DT["RELSZ"]), (DT["JMPREL"], DT["PLTRELSZ"])):
        if tagoff in D:
            o = v2o(D[tagoff][0])
            for i in range(D[tagsz][0] // 8):
                _, info = struct.unpack_from("<II", d, o + i * 8)
                rel[RELNAME.get(info & 0xff, info & 0xff)] += 1
    print("  relocs", rel.most_common())
    jni = [n for n in exports if n.startswith("Java_") or n in ("JNI_OnLoad", "JNI_OnUnload")]
    print("  JNI exports", len(jni), jni[:40])
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / f"{path.name}.imports.txt").write_text("\n".join(sorted(imports)), encoding="utf-8")
    (outdir / f"{path.name}.exports.txt").write_text("\n".join(sorted(exports)), encoding="utf-8")
    return d


unity = None
libdir, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
for p in sorted(libdir.glob("*.so")):
    data = analyze(p, out)
    if p.name == "libunity.so":
        unity = data

print("\n=== libunity strings ===")
strs = set(m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{4,}", unity))
print("  version-like", sorted(s for s in strs if re.fullmatch(r"\d\.\d\.\d+[a-z]\d*", s))[:10])
for pat in (r"^GL_[A-Z_a-z0-9]+$", r"(?i)glsl|vertex shader|#version", r"(?i)getprocaddress", r"(?i)dlopen|dlsym|/proc/|\.so$",
            r"(?i)split[0-9%]|\.obb|\.apk|mainobb|expansion", r"(?i)android/|com/unity3d|Lcom/"):
    hits = sorted(s for s in strs if re.search(pat, s))
    print(f"  /{pat}/ n={len(hits)}", hits[:30])
