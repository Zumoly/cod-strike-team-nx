#!/bin/sh
# Assembles Mono's AOT output into shared objects for src/loader.  Mono drives `as` and
# `ld` itself when it can; here they run separately, on the host-side ARM toolchain.
# usage: aot_link.sh <dir holding *.dll.s> <out dir>
SRC=$1
OUT=$2
mkdir -p "$OUT" /tmp/aot
for s in "$SRC"/*.dll.s; do
    n=$(basename "$s" .s)
    [ "$OUT/$n.so" -nt "$s" ] && continue
    (
        arm-none-eabi-as -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp "$s" -o "/tmp/aot/$n.o" &&
        arm-none-eabi-ld -shared --hash-style=sysv -z notext -o "$OUT/$n.so" "/tmp/aot/$n.o" &&
        echo "linked $n $(stat -c %s "$OUT/$n.so")"
    ) || echo "FAILED $n"
done
