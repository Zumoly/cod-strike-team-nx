#!/usr/bin/env python3
"""Checks the 32-bit NRO's structure, since no NRO loader exists here to run it.

usage: check_nro.py <nro> <elf>
"""
import struct
import subprocess
import sys

nro_path, elf = sys.argv[1:3]
d = open(nro_path, "rb").read()


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        sys.exit(1)


check(d[0x10:0x14] == b"NRO0", "NRO magic")
total = struct.unpack_from("<I", d, 0x18)[0]
text_off, text_size, ro_off, ro_size, data_off, data_size, bss = struct.unpack_from("<7I", d, 0x20)
check(total <= len(d), f"image size {total:#x} fits the file ({len(d):#x})")
check(all(v % 0x1000 == 0 for v in (text_off, ro_off, data_off)), "segments are page aligned")
check(text_off == 0 and ro_off == text_off + text_size, "text then rodata")
check(data_off == ro_off + ro_size, "data follows rodata")

# crt0's first instruction jumps over the header; AArch64 code would differ.
first = struct.unpack_from("<I", d, 0)[0]
check(first == 0xEA00001E, f"entry is the AArch32 branch ({first:#010x})")
check(d[8:16] == b"HOMEBREW", "homebrew marker")
mod0 = struct.unpack_from("<I", d, 4)[0]
check(d[mod0:mod0 + 4] == b"MOD0", f"MOD0 at {mod0:#x}")

syms = dict((p[2], int(p[0], 16)) for p in (l.split() for l in subprocess.check_output(
    ["arm-none-eabi-nm", elf], text=True).splitlines()) if len(p) == 3)
check(syms["__nx_dynamic"] < 0x1000, "relocator is on page 0")

asset = total
check(d[asset:asset + 4] == b"ASET", "asset section with icon and NACP")
print(f"info text={text_size:#x} rodata={ro_size:#x} data={data_size:#x} bss={bss:#x}")
