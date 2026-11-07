# COD Strike Team NX

A Switch wrapper for the 32-bit Android version of Call of Duty: Strike Team (Unity 4.3, Mono). The game's own
ARM libraries run directly on the Switch CPU as a 32-bit Horizon program, with OpenGL ES translated to deko3d.
This repository and its releases hold no game files: you supply your own APK and OBB.

## Install

You need Atmosphère with signature patches, DBI, and Docker on a PC for the one-time game-file setup below.

1. Download `cod_st_nx-v0.1.0.zip` from the Releases page and extract it.
2. Copy the `switch/cod_st/` folder to the root of your SD card.
3. Add your game files to `sdmc:/switch/cod_st/` (see **Game files**).
4. Install `COD Strike Team Loader [0100C0D57E4A0000].nsp` with DBI.
5. Start **COD Strike Team** from the HOME menu.

The installed title is a small 32-bit loader for `sdmc:/switch/cod_st/cod_st.nro`. Install it once; to update
the port, replace `cod_st.nro`. sphaira and hbmenu cannot start this NRO, whatever address-space setting they
use: their loader is 64-bit and the game is 32-bit ARM code.

The release also has `cod_st.nsp`, which Atmosphère can start in place of another game, held with a button
(`override_config.ini`, see `port/docs/PORTING.md`, "Hardware probe"). The installed title is easier.

## Game files

Everything is built from your own copy of the game; nothing here is taken from it. On a PC with Docker, from
`port/`:

1. `python tools/extract.py <your.apk> <your.obb> ../gamedata`
2. `python tools/package.py ../gamedata <your.apk> <your.obb> dist/sd`, which gives `game.apk`, `game.obb`
   and `lib/`. Add `--store` to trade about 1 GB of SD space for faster level loads.
3. Copy the managed DLLs from `../gamedata/apk/assets/bin/Data/Managed/` to `dist/sd/data/app/managed/`, and
   `mscorlib.dll` also to `dist/sd/data/app/managed/mono/2.0/`.
4. Build the ARM harness and compile the DLLs ahead of time. This is slow: a compiler on an emulated CPU,
   for hours.
   ```powershell
   docker run --rm -v "${PWD}:/work" -w /work ghcr.io/vita2hos/devcontainer/vita2hos:latest sh tests/qemu/build.sh
   docker build --platform linux/arm/v7 -t cod-harness tests/qemu
   powershell -File tests/qemu/aot_all.ps1
   ```
5. Link the results into `.dll.so` files:
   ```powershell
   docker run --rm -v "${PWD}:/work" -w /work ghcr.io/vita2hos/devcontainer/vita2hos:latest sh tests/qemu/aot_link.sh dist/sd/data/app/managed dist/sd/data/app/managed
   ```
6. Build the shaders, in the toolchain image (it has `uam`, the shader compiler). Run from the folder that
   holds both `port/` and `gamedata/`:
   ```powershell
   docker run --rm -v "${PWD}:/w" -w /w ghcr.io/vita2hos/devcontainer/vita2hos:latest sh -c 'export PATH=$PATH:/opt/devkitpro/tools/bin; python3 port/tools/extract_shaders.py gamedata/shaders/gles gamedata/apk gamedata/obb && python3 port/tools/translate_shaders.py gamedata/shaders/gles gamedata/shaders/out port/dist/sd/shaders.pak'
   ```
7. Copy the contents of `dist/sd/` to `sdmc:/switch/cod_st/`.

Unity builds a few shaders at runtime. Any it uses that `shaders.pak` lacks are written to
`sdmc:/switch/cod_st/shader_miss/`; copy that folder next to `gamedata/`, add `--seen shader_miss` to the
`extract_shaders.py` command in step 6, run it again and copy the new `shaders.pak`.

Your game data stays yours: do not share `dist/` or `gamedata/`.

## Controls

| Input | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Look |
| ZR | Fire |
| ZL | Aim down sights |
| L | Switch between the drone view and first person |
| R | Throw a grenade |
| L3 | Target to the left; on a purchase or revive prompt, its left button (No) |
| R3 | Target to the right; on a prompt, confirm (buy or revive) |
| A | Interact, hold breath; on a prompt, confirm |
| B | Crouch |
| X | Tap: grenade; hold: mine |
| Y | Tap: reload; hold: next weapon |
| D-pad | Left and right switch targets, up changes unit, down is follow me |
| Plus | Pause |
| Minus | Virtual mouse on/off: the left stick moves the blue arrow, A taps, hold A to drag |
| Touchscreen | Touch directly |
| Capture | Screenshots and video |

## What this port fixed

- **Graphics**: the ground and sky were missing (front-face winding inverted by deko3d's Y-down origin), and
  render-to-texture crashed the GPU (Unity's packed depth/stencil textures were created as colour images).
  The sniper scope, the drone view and the campaign's camera effects work.
- **Stability**: survival used to crash at wave 3-5. Causes found and fixed: GL object ids reused while still
  live, a leak in the fake Java layer (hundreds of objects a second), heap fragmentation from devkitARM's
  aligned `malloc`, texture data kept twice in memory, and an out-of-bounds `glReadPixels`.
- **Missions**: the campaign loads, cutscenes included. Shaders Unity generates at runtime are added to the
  pak instead of drawn with a stand-in that faulted the GPU.
- **Prompts and menus**: purchase and revive prompts answer to the pad; a virtual mouse reaches the rest.
- **Launching**: a 32-bit NRO loader title, so the port installs once and updates as an NRO.
- **Diagnostics**: crash, GPU-error, memory and per-call-site allocation reports in `sdmc:/switch/cod_st/`.

## If something goes wrong

`boot.log`, `game.log`, `watch.log`, `loader.log` and, after a crash, `crash.log` and `gpu_error.txt` are in
`sdmc:/switch/cod_st/`, and Atmosphère's report is in `sdmc:/atmosphere/crash_reports/`. Send them with a
description of what happened. `port/docs/PORTING.md` lists the switch files (`heap_guard`, `thread_dumps`,
`unlock_missions`, `scene_report` and others) that turn on extra checks.

## Limits

- Tested with one APK (Unity 4.3.0f4, versionName 1.0.40) on firmware 21.2.0. The libraries must match it.
- Online features, the store and leaderboards have no servers behind them; the game runs offline.
- The long load before the campaign is the briefing video, which the game waits for.
- HOME and sleep handling are basic.

## Build

Needs Docker with `ghcr.io/vita2hos/devcontainer/vita2hos:latest`, and
[aks796/libnx32](https://github.com/aks796/libnx32) built into `port/third_party/libnx32/prefix`.

```powershell
cd port
.\build.ps1 all                    # build/cod_st.nro, build/cod_st.nsp
.\build.ps1 -f Makefile.loader     # build_loader/loader.nso, the installed title's program
```

`port/docs/PORTING.md` explains the design, every fix with the evidence behind it, and how the loader title is
packaged with hacBrewPack.

## Credits

- [android32](https://github.com/aks796/android32) and [libnx32](https://github.com/aks796/libnx32) by aks796:
  the 32-bit start-up code in `port/src/rt/` (MIT, see `port/src/rt/LICENSE.android32`) and the libnx fork the
  port links against (ISC, libnx authors).
- [mc3_nx](https://github.com/Kaays/mc3_nx) by Kaays and contributors: the working 32-bit Android port this one
  was checked against, and the source of the exception handler and audio output.
- [deko3d](https://github.com/devkitPro/deko3d) by fincs, [libnx](https://github.com/switchbrew/libnx), and
  devkitPro's toolchain; the vita2hos devcontainer image that bundles them.
- The `.so` loader approach of TheFloW's and fgsfds' so_util ports.
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere), and
  [hacBrewPack](https://github.com/The-4n/hacBrewPack) by The-4n for the installable title.

Call of Duty: Strike Team is by Activision and The Blast Furnace. This project isn't affiliated with Activision,
Unity Technologies or Nintendo, and contains none of their code or assets.
