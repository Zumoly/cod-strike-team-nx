#!/bin/sh
# Host-side tests.  $1 = packaged SD layout, mounted at any path.
set -e
cd /work
mkdir -p build/host
CF="-Wall -Wextra -g -O1 -Iinclude"
gcc $CF -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    src/vfs/vfs.c src/bionic/fs_bridge.c tests/test_vfs.c -o build/host/test_vfs
rm -rf /tmp/sd
mkdir -p /tmp/sd
ln -s "$1/game.apk" /tmp/sd/game.apk
ln -s "$1/game.obb" /tmp/sd/game.obb
./build/host/test_vfs /tmp/sd "${2:-5168}"
