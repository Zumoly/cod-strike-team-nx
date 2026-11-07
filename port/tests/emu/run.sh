#!/bin/sh
# Runs inside the sims3-emu image with the Sims3-NX Ryujinx build.  Runs as root; the
# emulator refuses root, so it runs as uid 1000, like Sims3-NX/tools/docker/run_chansey.sh.
#   /work = port (read-only), /ryu = third_party/chansey (read-only),
#   /keys = the user's own prod.keys (read-only), /out = results
# usage: run.sh <path inside /work> [seconds]   env: MMODE, BACKEND
TARGET=$1
SECS=${2:-40}
C=/ryu
R=/tmp/emu
SD=$R/sdcard/switch/cod_st
mkdir -p "$SD" "$R/system" /tmp/home
cp -r /work/dist/sd/lib "$SD/lib"
mkdir -p "$SD/data/app"
cp -r /work/dist/test/data/app/managed "$SD/data/app/managed"
ln -s /work/dist/sd/game.apk "$SD/game.apk"
ln -s /work/dist/sd/game.obb "$SD/game.obb"
cp /work/dist/sd/shaders.pak "$SD/shaders.pak"
# Copied so the keys stay in this throwaway container's filesystem.
cp /keys/prod.keys "$R/system/prod.keys"
cp -r "/work/$TARGET" /tmp/app
chown -R 1000:1000 "$R" /tmp/home /tmp/app

Xvfb :9 -ac -screen 0 1280x720x24 >/dev/null 2>&1 &
export DISPLAY=:9
sleep 2
shot() { xwd -root -silent -display :9 2>/dev/null | xwdtopnm 2>/dev/null | pnmtopng > "/out/$1.png" 2>/dev/null; }
( n=0; while [ $n -lt 6 ]; do sleep $((SECS / 7)); n=$((n + 1)); shot shot$n; done ) &
setpriv --reuid=1000 --regid=1000 --clear-groups env RYUBING_INTERACTIVE=1 HOME=/tmp/home \
  DOTNET_ROOT=$C/dotnet DOTNET_gcServer=0 XDG_RUNTIME_DIR=/tmp/home \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json LIBGL_ALWAYS_SOFTWARE=1 \
  LD_LIBRARY_PATH=$C/build timeout -s KILL "$SECS" $C/build/Ryujinx --no-gui --root-data-dir "$R" \
  --memory-manager-mode ${MMODE:-SoftwarePageTable} --ignore-controller-applet --skip-user-profiles-manager \
  --ignore-missing-services --graphics-backend ${BACKEND:-Vulkan} /tmp/app > /out/stdout.txt 2>&1
echo "emu_exit=$?" >> /out/stdout.txt
cp -r "$R/Logs" /out/ 2>/dev/null
cp "$SD"/*.log /out/ 2>/dev/null
find "$SD" -type f ! -name '*.so' ! -name '*.dll' ! -path '*/managed/*' -exec ls -la {} \; > /out/files.txt
echo done
