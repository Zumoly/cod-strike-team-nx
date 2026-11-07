#!/usr/bin/env python3
"""Pulls every GLES2 program out of the game's files, for tools/translate_shaders.py.

Unity stores programs as quoted text inside serialized assets.  Files split into .splitN
parts are rejoined first, because programs cross the boundaries.  Programs seen at runtime
(the harness's dist/test/seen) are added too: Unity generates some, and only they are certain.

Unity also patches fog into a program when a scene turns fog on, so those variants are in no game
file.  fog_variant repeats the patch for each of the three modes.  Its text must match Unity's,
because programs are found by a hash of it: with --seen, it prints how many captured ones it matches.

usage: extract_shaders.py <out_dir> <game_dir>... [--seen <dir>]
Writes <out_dir>/<hash>.glsl, each holding #ifdef VERTEX and #ifdef FRAGMENT stages.
"""
import hashlib
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import translate_shaders as ts

PROGRAM = re.compile(rb'"(!!GLES(?!3).*?)"', re.S)
SPLIT = re.compile(r"^(.*)\.split(\d+)$")


FOG_DECL_VS = ("uniform highp vec4 _unity_FogParams;\nuniform lowp vec4 _unity_FogColor;\n"
               "varying lowp vec4 _unity_FogColorPreMul;\nvarying lowp vec4 _unity_FogVar;\n")
FOG_DECL_FS = "varying lowp vec4 _unity_FogColorPreMul;\nvarying lowp vec4 _unity_FogVar;\n"
FOG_TAIL = "_unity_FogColorPreMul = _unity_FogColor * (vec4(1.0)-_unity_FogVar);\n"
FOG_MODES = {
    "linear": "_unity_FogVar = vec4(clamp (_unity_FogParams.z * gl_Position.z + _unity_FogParams.w, 0.0, 1.0)); "
              "_unity_FogVar.a = 1.0;\n",
    "exp": "float _patchFog = _unity_FogParams.y * gl_Position.z;\n"
           "_unity_FogVar = vec4(clamp (exp2(-_patchFog), 0.0, 1.0)); _unity_FogVar.a = 1.0;\n",
    "exp2": "float _patchFog = _unity_FogParams.x * gl_Position.z;\n_patchFog = _patchFog * _patchFog;\n"
            "_unity_FogVar = vec4(clamp (exp2(-_patchFog), 0.0, 1.0)); _unity_FogVar.a = 1.0;\n",
}
POSITION = re.compile(r"^[ \t]*gl_Position\s*=[^;]*;[ \t]*\r?$", re.M)
COLOR = re.compile(r"^[ \t]*gl_Frag(?:Data\s*\[\s*0\s*\]|Color)\s*=\s*([^;]*);[ \t]*\r?$", re.M)


def fog_variant(vs, fs, mode):
    """What Unity's fog patch makes of a program, or None if it does not have the expected shape."""
    positions, colors = list(POSITION.finditer(vs)), list(COLOR.finditer(fs))
    if not positions or not colors:
        return None
    p, c = positions[-1], colors[-1]
    v = FOG_DECL_VS + vs[:p.end()] + "\n" + FOG_MODES[mode] + FOG_TAIL + vs[p.end():]
    wrapped = (f"lowp vec4 _patchFragColor;\n_patchFragColor = {c.group(1)};\n"
               "_patchFragColor = _patchFragColor * _unity_FogVar + _unity_FogColorPreMul; "
               "gl_FragData[0] = _patchFragColor;")
    f = FOG_DECL_FS + fs[:c.start()] + wrapped + fs[c.end():]
    return v, f


def wrap(v, f):
    return f"!!GLES\n#ifdef VERTEX\n{v}\n#endif\n#ifdef FRAGMENT\n{f}\n#endif\n".encode("latin1")


def add_fog(unique):
    base = list(unique.values())
    for text in base:
        stages = ts.split_stages(text.decode("latin1"))
        if not stages or "_unity_Fog" in stages[0]:
            continue
        for mode in FOG_MODES:
            out = fog_variant(stages[0], stages[1], mode)
            if out:
                data = wrap(*out)
                unique[hashlib.sha1(data).hexdigest()[:16]] = data


def blobs(roots):
    groups = {}
    for root in roots:
        for p in root.rglob("*"):
            if not p.is_file() or p.suffix in (".dex", ".xml", ".png"):
                continue
            m = SPLIT.match(str(p))
            if m:
                groups.setdefault(m.group(1), {})[int(m.group(2))] = p
            else:
                yield p.name, p.read_bytes()
    for name, parts in sorted(groups.items()):
        yield pathlib.Path(name).name, b"".join(parts[i].read_bytes() for i in sorted(parts))


def strip_fog(v, f):
    """Undoes the patch on a captured program, to test fog_variant against the original."""
    mode = "exp2" if "_patchFog * _patchFog" in v else "exp" if "_patchFog" in v else "linear"
    for line in FOG_DECL_VS.splitlines():
        v = v.replace(line, "")
    for line in (FOG_MODES[mode] + FOG_TAIL).splitlines():
        v = v.replace(line, "")
    for line in FOG_DECL_FS.splitlines():
        f = f.replace(line, "")
    m = re.search(r"lowp vec4 _patchFragColor;\s*_patchFragColor = ([^;]*);\s*_patchFragColor = [^;]*;[^;]*;", f)
    if not m:
        return None
    return v, f[:m.start()] + f"gl_FragData[0] = {m.group(1)};" + f[m.end():], mode


def generated(stripped, keys):
    v, f, mode = stripped
    out = fog_variant(v, f, mode)
    return ts.program_key(*out) if out else None


def main():
    args = sys.argv[1:]
    seen = None
    if "--seen" in args:
        i = args.index("--seen")
        seen = pathlib.Path(args[i + 1])
        del args[i:i + 2]
    out = pathlib.Path(args[0])
    out.mkdir(parents=True, exist_ok=True)
    roots = [pathlib.Path(a) for a in args[1:]]

    unique = {}
    for name, data in blobs(roots):
        if b"#ifdef VERTEX" not in data:
            continue
        found = 0
        for m in PROGRAM.finditer(data):
            text = m.group(1)
            if b"#ifdef VERTEX" in text and b"#ifdef FRAGMENT" in text:
                unique[hashlib.sha1(text).hexdigest()[:16]] = text
                found += 1
        print(f"{name}: {found} programs")

    if seen:
        for p in sorted(seen.glob("*.txt")):
            v, _, f = p.read_bytes().partition(b"//=====FS=====")
            text = b"!!GLES\n#ifdef VERTEX\n" + v + b"\n#endif\n#ifdef FRAGMENT\n" + f + b"\n#endif\n"
            unique[hashlib.sha1(text).hexdigest()[:16]] = text

    add_fog(unique)

    if seen:
        keys = set()
        for text in unique.values():
            stages = ts.split_stages(text.decode("latin1"))
            if stages:
                keys.add(ts.program_key(*stages))
        hit = total = 0
        for p in sorted(seen.glob("*.txt")):
            v, _, f = p.read_text(encoding="latin1").partition("//=====FS=====")
            if "_unity_FogVar" not in v:
                continue
            total += 1
            stripped = strip_fog(v, f)
            hit += stripped is not None and generated(stripped, keys) == ts.program_key(v, f)
        print(f"fog check: {hit} of {total} captured programs reproduced exactly")

    for old in out.glob("*.glsl"):
        old.unlink()
    for h, text in unique.items():
        (out / f"{h}.glsl").write_bytes(text)
    print("unique programs", len(unique))


if __name__ == "__main__":
    main()
