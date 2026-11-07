#!/bin/sh
# Threading tests on the host: plain, ThreadSanitizer, and a negative control.
cd /work
mkdir -p build/host
CF="-Wall -Wextra -g -Iinclude -D_GNU_SOURCE"
SRC="src/bionic/futex.c src/bionic/pthread_bridge.c src/bionic/fs_bridge.c src/vfs/vfs.c tests/test_threads.c"

echo "=== plain -O2"
gcc $CF -O2 $SRC -lpthread -o build/host/threads || exit 1
./build/host/threads | tail -1 || exit 1
for i in 1 2 3 4 5; do ./build/host/threads | tail -1 | grep -q PASSED || { echo "repeat $i failed"; exit 1; }; done
echo "repeat runs passed"

echo "=== ThreadSanitizer"
gcc $CF -O1 -fsanitize=thread $SRC -lpthread -o build/host/threads_tsan || exit 1
TSAN_OPTIONS="halt_on_error=0 exitcode=66" setarch -R ./build/host/threads_tsan > /tmp/tsan.log 2>&1
rc=$?
tail -1 /tmp/tsan.log
echo "tsan exit=$rc warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/tsan.log)"
[ "$rc" = 0 ] || { grep -A14 -m2 'WARNING: ThreadSanitizer' /tmp/tsan.log; exit 1; }

echo "=== negative control: unlock weakened to relaxed must be detected"
gcc $CF -O1 -fsanitize=thread -DUNLOCK_ORDER=memory_order_relaxed $SRC -lpthread -o build/host/threads_bad || exit 1
TSAN_OPTIONS="halt_on_error=1 exitcode=66" setarch -R ./build/host/threads_bad > /tmp/bad.log 2>&1
rc=$?
echo "control exit=$rc warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/bad.log)"
grep -m1 -E 'WARNING: ThreadSanitizer' /tmp/bad.log
[ "$rc" = 66 ] || { echo "control NOT detected"; exit 1; }
echo "ALL THREAD TESTS OK"
