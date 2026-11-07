#!/usr/bin/env python3
"""Lists ARM instructions in the game's code that can behave differently on real hardware.

Emulators run all of these.  An ARMv8 core at EL0 can trap the deprecated ones, depending on
how the kernel configures SCTLR.  Literal pools sit inside text, so a hit is a candidate.
usage: scan_hw_insns.py <file.so> ...
"""
import collections
import struct
import sys


def exec_ranges(data):
    phoff = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    for i in range(count):
        kind, off, _, _, filesz, _, flags, _ = struct.unpack_from("<8I", data, phoff + i * size)
        if kind == 1 and flags & 1:
            yield off, filesz


ARM = [
    ("cp15 dmb", 0x0FFF0FFF, 0x0E070FBA),
    ("cp15 dsb", 0x0FFF0FFF, 0x0E070F9A),
    ("cp15 isb", 0x0FFF0FFF, 0x0E070F95),
    ("swp/swpb", 0x0FB00FF0, 0x01000090),
    ("setend", 0xFFFFFDFF, 0xF1010000),
    ("tls read", 0x0FFF0FFF, 0x0E1D0F70),
]


def scan(path):
    data = open(path, "rb").read()
    hits = collections.defaultdict(list)
    other = collections.Counter()
    for off, size in exec_ranges(data):
        for pos in range(off, off + size - 3, 4):
            w = struct.unpack_from("<I", data, pos)[0]
            if w >> 28 not in (0xE, 0xF):
                continue
            for name, mask, value in ARM:
                if w & mask == value:
                    hits[name].append(pos)
            if w & 0x0F000F10 == 0x0E000F10:
                other[f"p15 {'mrc' if w & 0x100000 else 'mcr'} op1={w >> 21 & 7} c{w >> 16 & 15} c{w & 15} {w >> 5 & 7}"] += 1
        for pos in range(off, off + size - 3, 2):
            a, b = struct.unpack_from("<HH", data, pos)
            if a == 0xEE07 and b & 0x0FFF in (0x0FBA, 0x0F9A, 0x0F95):
                hits["thumb cp15 barrier"].append(pos)
    print(path.replace("\\", "/").rsplit("/", 1)[-1], len(data))
    for name, where in sorted(hits.items()):
        print(f"  {name}: {len(where)} first {[hex(x) for x in where[:6]]}")
    for name, n in other.most_common(10):
        print(f"  {name}: {n}")


for p in sys.argv[1:]:
    scan(p)
