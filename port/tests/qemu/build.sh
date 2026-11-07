#!/bin/sh
# Builds the ARM Linux harness: the port's real code, with tests/qemu/sys.c in place of libnx.
set -e
cd /work
L=/opt/devkitpro/libnx32
O=build/qemu
mkdir -p $O
ARCH="-march=armv8-a+crc+simd -mtune=cortex-a57 -mfpu=neon-fp-armv8 -mfloat-abi=softfp -mtp=soft -marm"
CF="$ARCH -g -O1 -ffunction-sections -fdata-sections -Wall -Wextra -DPORT_QEMU -D_GNU_SOURCE -Iinclude -I$L/include"
SRC="src/loader/so_loader.c src/loader/imports.c src/bionic/libc_bridge.c src/bionic/fs_bridge.c
     src/bionic/pthread_bridge.c src/bionic/futex.c src/bionic/sha1.c src/bionic/gc_bridge.c src/bionic/heap_guard.c src/vfs/vfs.c
     src/gl/gl_imports.c src/gl/gles.c src/jni/fake_jni.c src/jni/android_input.c src/audio.c
     src/profiler.c src/loader/pad_dialog.c
     tests/qemu/sys.c tests/qemu/main.c"
ASM="src/loader/so_thunks.S src/bionic/runtime.S src/bionic/fastmem.S src/jni/jni_thunks.S tests/qemu/crt.S"
OBJ=""
for f in $SRC; do o=$O/$(echo $f | tr / _).o; arm-none-eabi-gcc $CF -c $f -o $o; OBJ="$OBJ $o"; done
for f in $ASM; do o=$O/$(echo $f | tr / _).o; arm-none-eabi-gcc $ARCH -c $f -o $o; OBJ="$OBJ $o"; done
arm-none-eabi-gcc $ARCH -nostartfiles -static -Wl,-T,tests/qemu/qemu.ld -Wl,--gc-sections \
    $OBJ -L$L/lib -lminiz -lm -lc -lsysbase -lc -lgcc -o $O/harness
arm-none-eabi-size $O/harness
cp $O/harness tests/qemu/harness
