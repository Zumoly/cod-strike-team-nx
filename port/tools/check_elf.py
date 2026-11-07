#!/usr/bin/env python3
"""Post-link checks.  A failure in any of these would otherwise be a silent
boot crash on hardware, so the build fails instead.

usage: check_elf.py <elf> <npdm> <nsp>
"""
import re
import struct
import subprocess
import sys

elf, npdm, nsp = sys.argv[1:4]


def run(*a):
    return subprocess.check_output(a, text=True)


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        sys.exit(1)


syms = {}
for line in run("arm-none-eabi-nm", elf).splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

# config/dcr32.ld keeps page 0 for crt0 and the relocator.  The PROVIDEd symbols vanish
# when unreferenced, so the code end comes from the second LOAD segment.
guard_end = 0x1000
loads = re.findall(r"^\s*LOAD\s+\S+\s+(0x[0-9a-f]+)", run("arm-none-eabi-readelf", "-lW", elf), re.M)
code_end = int(loads[1], 16)
check(syms["_start"] == 0, "_start is at image base")
check(syms["__nx_dynamic"] < guard_end, f"__nx_dynamic lies in page 0 ({syms['__nx_dynamic']:#x})")
check(code_end % 0x1000 == 0, f"code segment end is page aligned ({code_end:#x})")

relocs = re.findall(r"^([0-9a-f]{8})\s+[0-9a-f]+\s+(R_ARM_\S+)", run("arm-none-eabi-readelf", "-rW", elf), re.M)
types = {t for _, t in relocs}
check(types <= {"R_ARM_RELATIVE"}, f"only RELATIVE relocations ({len(relocs)} total, {types})")
in_guard = [o for o, _ in relocs if int(o, 16) < guard_end]
check(not in_guard, f"no relocation targets the guard {in_guard[:4]}")
in_code = sum(1 for o, _ in relocs if guard_end <= int(o, 16) < code_end)
print(f"info {in_code} text relocations will be applied through a writable alias")

nm = open(npdm, "rb").read()
check(nm[:4] == b"META", "NPDM magic")
flags = nm[0xC]
check((flags & 1) == 0, f"NPDM is 32-bit (flags {flags:#x})")
check(((flags >> 1) & 7) == 0, "NPDM address space is 32-bit")
aci = struct.unpack_from("<I", nm, 0x70)[0]
tid = struct.unpack_from("<Q", nm, aci + 0x10)[0]
check(tid == 0x010000000000100D, f"program id in ACI ({tid:#x})")

ps = open(nsp, "rb").read()
check(ps[:4] == b"PFS0", "NSP is PFS0")
count, strsize = struct.unpack_from("<II", ps, 4)
strtab = ps[0x10 + 24 * count:0x10 + 24 * count + strsize]
names = sorted(s.decode() for s in strtab.split(b"\0") if s)
check(names == ["main", "main.npdm"], f"exefs contents {names}")
