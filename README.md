# COD Strike Team NX

A Switch wrapper for the 32-bit Android version of Call of Duty: Strike Team (Unity 4.3, Mono). The game's own
ARM libraries run directly on the Switch CPU as a 32-bit Horizon program, with OpenGL ES translated to deko3d.
This repository and its releases hold no game files: you supply your own APK and OBB.

## Install

You need Atmosphère with signature patches, DBI, your own APK and OBB of the game (version below), and Docker
Desktop on a PC for the one-time game-file setup.

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

The port was made and tested with this version of the Android game:

| | |
| --- | --- |
| Package | `com.activision.callofduty.striketeam` |
| Version | 1.0.40 (versionCode 15), Unity 4.3.0f4, armeabi-v7a |
| APK | 37,363,316 bytes, SHA-1 `e468b92901054d2fa0ac92908a97015424d17822` |
| OBB | `main.15.com.activision.callofduty.striketeam.obb`, 1,037,673,862 bytes, SHA-1 `7e7d910a8956f76d0f0d8756be853bcf6bf12b3d` |

Other versions are untested: the port patches and calls into these exact libraries.

Some of what the console needs is built from your game on a PC: the game's C# code, compiled ahead of time
(Mono's JIT cannot run there), and its shaders, compiled for the Switch GPU. One script does it all, with
**Docker Desktop** as the only requirement. From `port/`:

```powershell
powershell -ExecutionPolicy Bypass -File setup.ps1 -Apk <your.apk> -Obb <your.obb>
```

It checks your files against the table above, then extracts them, compiles the C# with the game's own Mono
on an emulated ARM CPU (**this takes hours**), links it, builds `shaders.pak`, and leaves the finished
folder in `port/dist/cod_st/`. Copy its contents to `sdmc:/switch/cod_st/`, next to `cod_st.nro`. If it is
stopped, run the same command again: finished steps are skipped. `-Store` keeps the OBB uncompressed, using
about 1 GB more SD space for faster level loads.

Unity builds a few shaders at runtime. Any that `shaders.pak` lacks are written to
`sdmc:/switch/cod_st/shader_miss/` and skipped while drawing. Copy that folder to the PC, run
`setup.ps1 -ShadersOnly -Seen <that folder>` (a few minutes), and copy the new `shaders.pak` over.

Your game data stays yours: do not share `port/dist/` or `gamedata/`.

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

- Tested with one APK (1.0.40, versionCode 15; see **Game files**) on firmware 21.2.0.
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
