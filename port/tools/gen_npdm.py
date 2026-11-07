#!/usr/bin/env python3
"""Generate the NPDM description for the AArch32 port process.

Matches the android32 runtime's npdm.json.in, which runs 32-bit Android games on real
hardware.  Do not take values from nx-hbloader's hbl.json: it is 64-bit.

usage: gen_npdm.py <out.json>
"""
import json
import sys

SYSCALLS = list(range(0x00, 0x53)) + [0x55] + list(range(0x5F, 0x6E)) + [0x6F] + \
    list(range(0x72, 0x79)) + [0x7F]

doc = {
    "name": "codst",
    "version": "0.0.1",
    "program_id": "0x010000000000100D",
    "program_id_range_min": "0x010000000000100D",
    "program_id_range_max": "0x010000000000100D",
    "main_thread_stack_size": "0x800000",
    "main_thread_priority": 44,
    "default_cpu_id": 0,
    "pool_partition": 0,
    "is_64_bit": False,
    "address_space_type": 0,
    "is_retail": True,
    "system_resource_size": "0",
    "disable_device_address_space_merge": False,
    "optimize_memory_allocation": False,
    "signature_key_generation": 0,
    "filesystem_access": {"permissions": "0xFFFFFFFFFFFFFFFF"},
    "service_access": ["*"],
    "service_host": ["*"],
    "kernel_capabilities": [
        {"type": "kernel_flags", "value": {
            "highest_thread_priority": 59, "lowest_thread_priority": 28,
            "lowest_cpu_id": 0, "highest_cpu_id": 2}},
        {"type": "syscalls", "value": {f"svc{n:02X}": f"0x{n:02x}" for n in SYSCALLS}},
        {"type": "application_type", "value": 1},
        {"type": "min_kernel_version", "value": "0x30"},
        {"type": "handle_table_size", "value": 1023},
        {"type": "debug_flags", "value": {"allow_debug": True}},
    ],
}

with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
