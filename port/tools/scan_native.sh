#!/bin/bash
# Stage 3/4: instruction-level scan of the Android libraries for things a loader
# cannot intercept through imports.   usage: scan_native.sh <lib_dir>
for so in "$1"/*.so; do
    n=$(basename "$so")
    d=/tmp/$n.dis
    arm-none-eabi-objdump -d --no-show-raw-insn "$so" > "$d" 2>/dev/null
    echo "=== $n ($(wc -l < "$d") lines)"
    printf "  svc instructions:      %s\n" "$(grep -cE '\ssvc\s' "$d")"
    printf "  TLS reg reads (mrc c13): %s\n" "$(grep -cE 'mrc.*c13, c0' "$d")"
    printf "  kuser helpers (ffff0f): %s\n" "$(grep -ciE '0xffff0f[0-9a-f]{2}|ffff0fa0|ffff0fc0|ffff0fe0' "$d")"
    printf "  ldrex/strex:           %s\n" "$(grep -cE '\s(ldrex|strex)' "$d")"
    printf "  dmb/dsb/isb:           %s\n" "$(grep -cE '\s(dmb|dsb|isb)\b' "$d")"
    printf "  swp (deprecated):      %s\n" "$(grep -cE '\sswpb?\s' "$d")"
    printf "  mcr p15 barriers:      %s\n" "$(grep -cE 'mcr.*p15' "$d")"
    printf "  NEON (vld/vst/q regs): %s\n" "$(grep -cE '\sv(ld|st)[1-4]|\sv[a-z]+\.[0-9a-z]+\s+q[0-9]' "$d")"
    printf "  VFP (d regs):          %s\n" "$(grep -cE '\sv(add|mul|ldr|str|mov)\.?[a-z0-9]*\s+[ds][0-9]' "$d")"
    printf "  thumb bx/blx interwork: %s\n" "$(grep -cE '\sblx\s' "$d")"
    printf "  udf/bkpt:              %s\n" "$(grep -cE '\s(bkpt|udf)\s' "$d")"
done
echo "=== libmono GC / suspend / JIT exports"
tr '\n' ' ' < /work/docs/imports/libmono.so.exports.txt | tr ' ' '\n' | grep -E '^(GC_|mono_jit_|mono_thread_(attach|set_main)|mono_aot|mono_arch_flush|mono_code_manager|mono_gc_|mono_set_signal|mono_runtime_set|mono_signal|mono_debugger)' | head -60 | tr '\n' ' '; echo
echo "=== libmono: syscall call sites"
grep -B6 -E 'bl.*<syscall@plt>' /tmp/libmono.so.dis | grep -E 'mov[wt]?\s+r0|ldr\s+r0|syscall' | head -20
echo "=== libmono strings"
strings -n 5 "$1/libmono.so" | grep -iE 'sigsegv|SIGPWR|SIGXCPU|signal|cacheflush|hard.?float|armv7|thumb|MONO_|mono_aot|aot|mmap|xdebug|2\.[0-9]\.[0-9]' | sort -u | head -60
echo "=== libunity: skin symbol binding"
arm-none-eabi-nm -D "$1/libunity.so" | grep -E 'SkinVertices|ANativeActivity|GC_' | head
echo "=== libmono imports not yet bridged, file-related"
grep -E '^(lseek|lseek64|open|stat|fstat|lstat|fopen|read|write|pread|pwrite|mmap|ftruncate|fsync|statfs|readlink|realpath|access|dup|dup2|utime|utimes|chmod|getcwd|getdents|sendfile|mkstemp|mkdtemp|symlink|link|fcntl|ioctl)$' /work/docs/imports/libmono.so.imports.txt | tr '\n' ' '; echo
