#!/bin/sh
# Builds the two diagnostic NSPs.  They share the port's NPDM, so run `make` first.
set -e
cd /work
O=build/canary
rm -rf $O
mkdir -p $O
build() { # name, extra flags
    arm-none-eabi-gcc -march=armv8-a -marm -nostdlib -nostartfiles -static $2 -Wl,--build-id=sha1 \
        -Wl,-T,tests/canary/canary.ld tests/canary/canary.S -o $O/$1.elf
    elf2nso $O/$1.elf $O/$1.nso
    mkdir -p $O/$1_exefs
    cp $O/$1.nso $O/$1_exefs/main
    cp build/cod_st.npdm $O/$1_exefs/main.npdm
    build_pfs0 $O/$1_exefs $O/$1.nsp
    python3 -I - "$O/$1.nsp" "$O/$1.elf" <<'E'
import struct, subprocess, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'PFS0'
out = subprocess.check_output(['arm-none-eabi-readelf', '-lW', sys.argv[2]], text=True)
print(sys.argv[1], len(d), 'bytes,', out.count('LOAD'), 'load segments')
E
}
build canary_npdm ""

L=$DEVKITPRO/libnx32
F=third_party/libnx32/prefix
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mtp=soft -fPIE -ftls-model=local-exec -marm"
arm-none-eabi-gcc $ARCH -O2 -D__SWITCH__ -I$F/include -c tests/canary/libnx_main.c -o $O/libnx_main.o
arm-none-eabi-gcc -specs=config/dcr32.specs -T config/dcr32.ld $ARCH build/src/rt/crt0_reloc.o build/src/rt/nx_init.o \
    build/src/rt/nx32_virtmem.o $O/libnx_main.o -L$F/lib -lnx -lm -o $O/canary_libnx.elf
elf2nso $O/canary_libnx.elf $O/canary_libnx.nso
mkdir -p $O/canary_libnx_exefs
cp $O/canary_libnx.nso $O/canary_libnx_exefs/main
cp build/cod_st.npdm $O/canary_libnx_exefs/main.npdm
build_pfs0 $O/canary_libnx_exefs $O/canary_libnx.nsp
ls -la $O/*.nsp
