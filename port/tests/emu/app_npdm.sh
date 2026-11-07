#!/bin/sh
# Makes an exefs copy with a game-style program id.  Ryujinx treats the hbloader id as a
# system applet with 1 GB; a console game slot, reached through hbl_config, has far more.
# Testing with this id is what exposes memory-size bugs before they reach hardware.
set -e
cd /work
python3 tools/gen_npdm.py /tmp/n.json
sed -i 's/0x010000000000100D/0x0100000000C0D000/g' /tmp/n.json
npdmtool /tmp/n.json /tmp/n.npdm >/dev/null 2>&1
rm -rf build/appid_exefs
mkdir -p build/appid_exefs
cp build/exefs/main build/appid_exefs/main
cp /tmp/n.npdm build/appid_exefs/main.npdm
