# Call of Duty: Strike Team on Horizon: porting notes

Status: the game's real startup and C# run in Ryujinx on the console code path, for 3,000+
frames, in full-AOT mode with the GC bridge. **No pixels are drawn yet**: GLES tracks state and
counts draws, and each frame presents a flat clear.

On hardware (firmware 21.2.0), the mc3_nx startup works: the process reached `main()`, loaded and
initialized the libraries, ran Mono and Unity, and crashed between frames 4 and 120. That crash is
diagnosed and fixed below, and the fix is confirmed on hardware: the game reached frame 360+ with no
crash and no `sqlite:` errors, and wrote `amp.bin` (24,576 bytes) and `userdb_0.bin` (3,072 bytes). That
run was short, and `boot.log` logs every 120 frames, so long-run behaviour is unknown.

**deko3D draws on hardware.** `src/gl/gles.c` tracks GL state and `src/gl/renderer_deko3d.c` draws it; first
console run reached the title screen with no shader missing. Facts that shaped it:

- Shaders: `tools/extract_shaders.py` rejoins split assets and adds programs seen at runtime (Unity generates
  fixed-function ones), `tools/translate_shaders.py` builds `shaders.pak`. A program is found by a hash of its
  two stages. A miss skips the draw and saves the source to `sd:/switch/cod_st/shader_miss/`; add those with
  `--seen` and rebuild. The pack must be copied to the SD card with the NSP.
- Orientation: rendering is GL-style, row 0 at the bottom, into an offscreen back buffer flipped at present.
- Screen size: Unity stays at 128x128 until `nativeResize` arrives after it calls `triggerResizeCall`.

Input: `src/jni/android_input.c` is a fake `AInputQueue`, `src/input_pad.c` reads the Switch. Unity builds a
device through JNI (`fake_jni.c`, `InputDevice`), and numbers axes from a table in libunity. With no `AXIS_RX`:
X Y Z RZ HAT_X HAT_Y are 0 to 5. The game reads 0-3 as sticks (`AndAxis1-4`) and 4-5 as triggers, so triggers
are sent on the HAT axes, and the D-pad as keys, which the game reads as arrow keys. Buttons map by position:
Cross B, Circle A, Square Y, Triangle X. Plus also sends Enter, Minus also sends Back.

Sound: Java's `FMODAudioDevice` thread is replaced by `src/audio.c`. It polls `fmodGetInfo` until FMOD's
output exists (24 kHz, 512 frames x 4 buffers), loops on `fmodProcess`, resamples linearly to 48 kHz and plays
through `src/rt/rt_audout.c` from mc3_nx. `setScreenSize(-1, -1)` means "keep the size" and must be ignored.

Scheduling: Unity relies on preemption, and spins waiting for other threads. Horizon time-slices only priority
59 on cores 0-2 (mc3_nx `dcr_sched.c`), so guest threads and the main thread run at 59 with mask 0x7, and the
audio pump at 0x2A. They ran at 0x2C before, which is the likely cause of the whole-console freeze when a map
loaded: a spinning thread at 44 outranks system services on those cores. Priority 59 was tested on hardware: it
did not cure the map-load hang. That hang was `/dev/urandom`: a 4 KB stand-in file reached end of file, Mono's
reader loops until it has every byte, so it spun for ever, flooding the FS service until the console died.
`fs_bridge.c` now generates the bytes. QEMU then ran the Arctic survival map to the end of its test.
`src/watchdog.c` writes `sd:/switch/cod_st/watch.log` every
second with flushes: if lines stop, the system died, and if they continue, the game hung and thread dumps show where.
QEMU cannot show this, because Linux preempts. `tests/qemu/run.ps1 520 -Level 04_GO_Arc_Survival_S1_Baked`
loads that scene directly; it stalled in the wait at libunity 0x280874, and a watchdog dumps every thread.
`game.log` is now flushed every 60 frames. Runtime-captured shaders are kept in `gamedata/shaders/seen`; pass
that folder to `extract_shaders.py --seen`.

## First hardware crash

`libbedrock`'s `brDeviceCache::clearCache` passed an uninitialized error pointer to `sqlite3_free`, which
ended up in newlib's `free` with a stack address. It did so because an earlier SQL failure had closed the
database and cleared its handle. Bedrock has that bug on any database failure; the failure itself was ours.

Horizon's filesystem has rules that Linux, and so Ryujinx in Docker, does not enforce:

| Horizon rule | Effect | Fix in `src/bionic/fs_bridge.c` |
|---|---|---|
| One handle at a time may write a file, and then no other handle can open it | libnx's `stat()` opens the file, so stat, access and truncate by name fail on a file open for writing | Tracked open files; these go through the open handle |
| libnx reports inode 0 for every file | SQLite keys file state on the inode, so it saw journal and database as one file | Inode from a case-blind path hash |
| An open file cannot be deleted or renamed | A journal SQLite kept open could not be removed | Follows from the inode fix |
| Rename does not replace an existing target | POSIX code expects it to | `b_rename` removes the target first |
| Written data can stay unflushed until close or flush | `b_fsync` was a no-op, so a crash lost commits | `b_fsync` flushes |

`tests/qemu/sys.c` now enforces these rules, reports inode 0 and uses the console's heap address. With the
old bridge it reproduced the crash through the same `libbedrock` frames. With the fix it runs 600 frames, on
a first launch and on a second launch with existing databases. **Use the QEMU harness, not Ryujinx, for
anything touching files.** SQLite failures now appear in `game.log` as `sqlite:` lines, and refused path
operations as `fs:` lines with the Horizon result.

Other leads from this crash, none of which caused it:

| Lead | Status |
|---|---|
| CP15 barriers in `libmono` (14, in Boehm) | Rewritten to `dmb ish` by `so_patch_kuser`. 141 GC thread stops ran on hardware with this in place |
| `swp` in `libunity` at `0x7a73c0` | Removed from ARMv8; emulators run it. `src/loader/hw_fixups.c` emulates it on fault. That path is untested |
| Heap at `0xbf800000`, so pointers at or above `0xC0000000` | Ruled out: the QEMU harness uses that heap and ran 600 frames |
| `r8`-`r11` read as zero for a thread blocked in an SVC | Bridge calls spill them; a thread blocked inside libnx does not, so a value held only there is invisible to Boehm |
| Unaligned `ldm`, `ldrd` or `vldm` | Linux fixes these up and Horizon does not. Not handled yet |

A crash writes `sd:/switch/cod_st/crash.log` (`src/rt/exc_handler.c`, from mc3_nx) with the fault type,
registers, module offsets and a stack scan, and flushes `game.log`. Delivery is confirmed on hardware.
`tools/scan_hw_insns.py` lists instructions that behave differently on real hardware.

## Boot path, current

1. `src/rt/` holds mc3_nx's android32 startup, copied unchanged (see `src/rt/README.md`), and links
   against `aks796/libnx32` in `third_party/libnx32`. It fixes two faults that each stop the console
   before `main()`:
   - Heap: stock libnx32 requests about 3 GB in a 1 GiB region and aborts. Ryujinx hides this because
     it caps the reported total memory. `nx_init.c` clamps it.
   - Relocation: on Atmosphere a Code page made writable can never become executable again.
     `crt0_reloc.c` writes text relocations through a `svcMapProcessMemory` alias, one kernel memory
     block per call, using a real process handle (`selfproc.c`). Ryujinx accepts the old way too.
2. `src/loader/so_backend_nx.c` follows the same rule. Each library is built in heap staging memory,
   then `svcMapProcessCodeMemory` maps it, RX first and RW second. Code stays inside the
   `[0x200000, 0x40000000)` code region. `boot.log` line 1 reports the relocation path.
3. `src/loader/game.c` loads the libraries, then acts as `UnityPlayer` through `src/jni/fake_jni.c`.
4. Mono runs **full AOT**, compiled by the game's own libmono under ARM emulation
   (`tests/qemu/aot_all.ps1`, then `aot_link.sh`), so no JIT or writable code is needed. `MONO_DEBUG=
   explicit-null-checks` is set while compiling, so ordinary managed code does not need fault delivery.
5. `src/bionic/gc_bridge.c` does Boehm's signal handler work from outside by pausing threads.

Horizon differences found by running it: directories cannot be opened as files, reads past end of file
fail instead of returning 0, and the kernel's thread context is larger than libnx32's struct. miniz
consumes input eagerly, so `port_inflate` holds back a byte to match zlib, or Unity loses file tails.

## mc3_nx

`github.com/Kaays/mc3_nx` is a working AArch32 Android port on real hardware, using the same toolchain
image, and its fork `aks796/libnx32` is the hardware reference for this port. Check both before theorizing
about console behaviour. It also launches through a 64-bit NRO that installs the NSP as an ExeFS override
for a sphaira forwarder icon, which is the way to get an NRO entry point.

## What the game is

Unity 4.3.0f4 on Mono 2.6.5, ARMv7 only (`armeabi-v7a`). There is no engine source: gameplay
is C# in `Assembly-CSharp.dll`, and the engine is the closed `libunity.so`.

| Library | Role | Plan |
|---|---|---|
| `libunity.so` | Engine, FMOD, PhysX | Load unmodified except kernel-helper literals |
| `libmono.so` | Mono JIT + Boehm GC | Load, hook icache flush, bridge GC suspend |
| `libmain.so` | Unwinder, `ANativeActivity_*` | Load for the unwinder exports |
| `libbedrock.so` | Activision online services | Stub (106 P/Invokes); see open question 2 |
| `libBlueDoveMediaRender.so` | Video playback | Skip at first; intro/cutscene video needs a replacement |

The APK is a modified build: signed with the public AOSP test key and contains MT Manager's
`cc.binmt.signature.PmsHookApplication`. That class is Java and never runs here, but whether the
native libraries were changed cannot be determined without an unmodified copy to compare.

## Architecture decision: a 32-bit Horizon process

The libraries are ARMv7, and mainline libnx/deko3D are AArch64 only. The local
`ghcr.io/vita2hos/devcontainer/vita2hos` image has 32-bit libnx and `libdeko3d.a`, so the code
runs natively in an AArch32 process. The alternative, a dynarmic JIT in a 64-bit process, would
need a thunk for every import and cost CPU on a game that is already CPU-bound.

Consequences:
- Output is an exefs NSP launched through Atmosphere's `hbl_config`; hbmenu `.nro` cannot start it.
- Address space is 4 GB, shared with the Android layout assumptions the libraries already make.
- Android's softfp calling convention matches, so `float` imports need no thunks.

### Toolchain gaps worked around

| Gap in the image | Fix |
|---|---|
| newlib is non-PIC ARMv4T, so its literal pools need text relocations | `src/rt/crt0_reloc.c` applies them through a writable alias. `tools/check_elf.py` proves none lands in page 0, where it runs |
| `svcWaitForAddress`, `svcSignalToAddress`, `svcGetThreadContext3` missing from the image's libnx32 | Provided by `aks796/libnx32` |
| `__libnx_exception_entry` returns "unhandled" | Weak; replaced by `src/rt/exc32.S` |
| `armICacheInvalidate` is a no-op | Mono's flush is routed to `port_icache_flush`; AOT code does not change after mapping |

newlib is also soft-float-only ARMv4T, so `sin`/`pow` from `-lm` are slow. Hot math imports
should get VFP/NEON versions when the loader binds them.

## Stage results

### 1. Extraction and source setup
5,508 entries extracted with CRCs checked. 3,961 serialized files; 23 split groups, all complete.
`tools/check_elf.py` runs on every build.

### 2. Packaging and VFS
Unity reads through the zip containers (`.apk/`, `.obb/` paths), so the archives stay intact.
`src/vfs/vfs.c` maps guest paths to `sdmc:/switch/cod_st/`, refuses everything else, blocks `..`
escapes, and falls back to case-insensitive lookup. No case-fold collisions exist in the data, and
the longest path is 76 characters.

`src/bionic/fs_bridge.c` translates what differs from newlib: `struct stat` (104 bytes),
`struct dirent`, `O_*` flags, errno numbers, 32-bit `off_t`.

`tools/package.py --store` trades 1 GB of SD space for no inflate work during level loads.

### 3. Engine dependencies and renderer mapping
There is no software renderer. All 104 GL/EGL imports are classified in `docs/GL_MAPPING.md`;
`src/gl/gl_imports.c` is generated from the real import table and binds unimplemented functions to
a stub that stops. `include/port/renderer.h` keeps deko3D behind one file.

The driver should report **OpenGL ES 2.0**, because every shader has a GLES2 variant and it selects
Unity's simpler device path.

Shaders: all 192 real GLES2 programs translate and compile with `uam` (464 KB of DKSH, largest
uniform block 2,448 bytes). `tools/translate_shaders.py` moves loose uniforms to std140 blocks and
records offsets in `shader_db.json`. Compiling proves the rewrite is valid GLSL, not that it renders
correctly.

Texture formats (bytes include mips):

| Format | Textures | MB | deko3D |
|---|---|---|---|
| ETC1 | 687 | 484 | `RGB_ETC2` (compatible) |
| RGBA4444 | 559 | 466 | `RGBA4_Unorm`; channel order to confirm |
| PVRTC RGBA4 | 92 | 209 | **None: decode on the CPU** |
| RGB24 | 32 | 88 | Expand to RGBA8 |
| RGBA32/ARGB32 | 40 | 45 | `RGBA8_Unorm` plus swizzle |
| DXT1/DXT5 | 113 | 18 | `BC1`/`BC3` |

Non-import dependencies found by disassembly:

| Item | Fix in the loader |
|---|---|
| `libmono` `svc 0` at `0x1157ac` (cacheflush) | Patch to a host call; Linux `svc 0` would be fatal |
| `0xffff0fc0` x43, `0xffff0fa0` x4 in `libunity` (`__sync_*` via kernel helpers) | Rewrite literals to host cmpxchg/barrier |
| `syscall(316-318)` in `libmono` (inotify) | Return ENOSYS |
| `_s_SkinVertices*` undefined | Harmless: no relocation references them |
| Java layer | About 30 `native*` entry points plus a fake JNI; 30 Java classes named from C# |
| FMOD audio | `org.fmod.FMODAudioDevice` over JNI, or OpenSL by `dlopen` |

### 4. Threading
`src/bionic/pthread_bridge.c` implements bionic's 4-byte mutex, cond, and semaphore directly over
a futex, because guests allocate them inline and zero-initialize them statically.

Horizon-specific adaptations:
- Threads get affinity mask 0-2. Horizon does not balance like Linux, so an unset mask would
  leave every thread on core 0. Core 3 belongs to the system.
- All threads share one priority: a higher one would starve lock holders on its core.
- `sched_yield` uses `YieldType_ToAnyThread`, since a plain yield never leaves the core.
- Default stack is 1 MB, matching Android.

Verified with 8 threads at 100k iterations on all mutex types, bounded-queue and semaphore
handshakes. ThreadSanitizer reports 0 warnings, and weakening one unlock to `relaxed` is detected.
This validates the algorithms and memory orders, on x86. ARM behaviour needs the probe.

### Still to solve: Boehm stop-the-world
`libmono` installs handlers for signals 30 and 24 (`SIGPWR`, `SIGXCPU`), and `pthread_kill` currently
returns ENOSYS. Horizon has no signals, and the first GC will hang or fail without this.

Proposed design: in `b_pthread_kill`, pause the target with `svcSetThreadActivity`, read its registers
with `svcGetThreadContext3`, store them below its SP, set the GC thread's `stack_ptr` (found through
the exported `GC_lookup_thread`), then post the ack semaphore. The restart signal resumes it.

The header warns the kernel may not dump GPRs for a thread inside a syscall. If the probe confirms
that, blocking bridge calls must spill callee-saved registers to the stack and publish SP before
entering the kernel.

Mono's null checks also use SIGSEGV. `src/rt/exc32.S` is the delivery path; `libmono` lists an
`explicit-null-checks` option that may reduce the need.

## Loader

`src/loader/so_loader.c` maps the libraries, applies relocations, patches the kernel-helper
literals and Mono's icache flush, protects segments, and runs initializers.
`src/loader/imports.c` binds each import. An import with no implementation gets a named thunk
that stops with its name, so a gap never becomes a jump to address 0.

Symbol order: host table, then libraries already loaded. `data_start` is special-cased to the
library's own data segment because Boehm uses it as the base of its static roots.

### Emulated test harness

`tests/qemu/run.ps1` runs the port's real ARM code, and the game's real libraries, on Docker's
user-mode ARM emulation. `tests/qemu/sys.c` stands in for libnx over Linux syscalls. Add `-Strace`
for a syscall trace.

| Proven under emulation | Result |
|---|---|
| Load and relocate libmain, libmono, libunity, libbedrock | All relocations applied |
| Imports | 535 external; 59 unbound, all Android input/sensor APIs |
| Initializers | All ran, including libunity's 201 |
| Mono boot | `mono_jit_init_version` succeeds on the game's `mscorlib.dll` |
| JIT | `Int32.Parse("1234")` compiled and returned 1234 |
| Exceptions | Managed throw caught; a write to address 0 in generated code became an exception |
| Game code | `Assembly-CSharp.dll` loads |
| GC collection | **Fails**: `pthread_kill failed`, as predicted (build with `TEST_GC`) |

What emulation cannot show:
- Emulated user mode has signals, real page permissions and RWX memory. Faults here reach Mono through
  real Linux signals. On Horizon they must come through `src/rt/exc32.S`, not yet connected.
- The Horizon backend refuses executable mappings, so **Mono's JIT will not run on a Switch yet**.
- Timing and memory ordering are not representative of Cortex-A57.

Bugs this found before hardware: a bootstrap cycle between thread adoption and malloc, and `mmap`
reading a file into a read-only mapping. Mono also needs `/proc/self/maps` to exist, and Boehm
rejects a `/proc/stat` under 100 bytes, so `vfs_init` generates both.

## Hardware probe

`dist/` holds your game data; do not share it. The NSP contains none.

1. Back up `sd:/atmosphere/config/override_config.ini` if it exists: this replaces it.
2. Copy `port/build/cod_st.nsp` to `sd:/switch/cod_st/cod_st.nsp`, and `port/dist/sd/lib/` to
   `sd:/switch/cod_st/lib/`.
3. Create `sd:/atmosphere/config/override_config.ini`:
   ```ini
   [hbl_config]
   override_any_app=true
   override_any_app_key=R
   override_any_app_address_space=32_bit
   path=switch/cod_st/cod_st.nsp
   ```
4. Reboot, then start any game while holding R. Do not use the album button.
5. Expect 3 seconds of green. Then read `sd:/switch/cod_st/probe.log` and `boot.log`.
6. Remove or rename the ini to get hbmenu back.

| Log line | Pass means |
|---|---|
| process reaches `boot.log` at all | `crt0_reloc.c`, the heap clamp and service startup work |
| `relocation path 1` | Text relocations went through the alias. 2 is the emulator fallback, not expected on a console |
| `futex: equal value` | WaitForAddress ABI is right: blocks about 20,000 us |
| `threads:` | Counter exact, `cores` shows several cores |
| `suspend[spinning]` / `[blocked-in-svc]` | What GC suspend can read from each state |
| `exception:` | AArch32 fault delivery, frame offset, and resume work |
| `mem:` | Real heap and address space available |
| `game_load:` lines in `boot.log` | The real libraries load and initialize on hardware |

The log stops at the failing step, so the last line identifies it.

## Ryujinx

`tests/emu/run.ps1 <exefs folder> [seconds]` runs a build in the Ryujinx kept in
`Sims3-NX/third_party/chansey`, inside the `sims3-emu` image, using `Homebrew Games/prod.keys`. All three
are mounted read-only. Results go to `build/emu/<folder>/`. Pass `build/exefs` for the port and
`build/canary/canary_npdm_exefs` for the canary.

| Reached in Ryujinx | Result |
|---|---|
| 32-bit process, `crt0_reloc.c` alias path, fork libnx startup, `main()` | Works |
| VFS, deko3D init, 180 presents | Work (calls return; pixels were not inspected) |
| Futex wait, 6 threads across 3 cores, thread pause and context dump | Work |
| Load libmain, libmono, libunity through staging memory and run all initializers | Works |
| Unity startup and C# | 3,240 frames in a 170-second run, stopped by the time limit |

Ryujinx cannot test these, so a pass there says nothing about them:
- User exception delivery: it ends the process at the probe's deliberate null read.
- The console crash: the old heap and the old relocator both run here, so Ryujinx does not enforce what
  the console rejected.

Bugs it found: libnx32 declares a 32-bit mask for `svcSetThreadCoreMask`, but the kernel reads 64 bits
(`port_svcSetThreadCoreMask` clears `r3`); `fflush` alone left `boot.log` empty, so logs now `fsync`; and
the loader asked for RW on memory that was already RW.

Its 32-bit process gets a 1 GB heap region (`mem:` lines). If the console reports the same, address space
type 2 may give more. Check this before finalizing the memory plan.

## Boot failure triage

First hardware run: instant crash with no logs, on both NSP and NRO. The NPDM was then rebuilt to
match vita2hos instead of 64-bit hbloader (`application_type` 1, no `map_region`, 512 handles, its
SVC list). That fix is not confirmed on hardware, and those early logs may have been lost to the
missing `fsync`, so the crash point is not certain either.

The later cause is now known from mc3_nx: the heap abort and the RX to RW to RX relocator would each have
stopped the process before `main()`, which fits the missing logs.

`tests/canary/` builds two NSPs. Point `path=` at each in turn if `cod_st` still fails:

| Result | Meaning |
|---|---|
| `canary_npdm` crashes (no libnx, no game code) | The NPDM or launch setup is still wrong |
| `canary_npdm` runs, `canary_libnx` crashes (real startup, no game code) | Relocation, heap or service startup |
| Both run, `cod_st` crashes | Game loading or early `main()`; read `boot.log` |

`canary_npdm` shows a black screen for 5 seconds, then exits cleanly.

## 32-bit NRO

`build/cod_st.nro` is an AArch32 NRO. hbmenu's 64-bit hbloader cannot run it: its first instruction
is ARM32, so a 64-bit process would crash at entry. It needs a forwarder that is itself a 32-bit
process and follows hbloader's contract:

| Requirement | Why |
|---|---|
| NPDM with `is_64_bit: false` and address space type 0 | The process mode is fixed at creation |
| Application title with full memory | Provides the full RAM |
| Map the NRO and enter at offset 0 with `r0` = homebrew config, `r1` = -1 | crt0 treats any other non-zero pair as an exception |
| Config entries for main thread, heap override, syscall hints | libnx reads these at startup |
| Forward kernel exceptions to NRO offset 0 with `r0`/`r1` unchanged | The kernel enters the process's own crt0, not the NRO's |
| Do not relocate the NRO | `src/rt/crt0_reloc.c` does that itself |
| Allow SVCs 0x32-0x35, 0x73, 0x77, 0x78, 0x5F, 0x28 | Thread pause, futex, code memory, cache, exception return |

Exception forwarding matters most: without it, Mono's null checks and the probe's fault test kill
the process. The relocator's alias path was written for an NSP, and this NRO path is untested. Paths stay `sdmc:/switch/cod_st/`, so `lib/` and the logs do not depend on where the NRO lives.

## Gamepad

In a mission `CommonHudController.UpdatePadInput` picks one of two paths. `UpdateAndroidPadInput` runs when
`Controller.GetState().connected` is true, and the Xbox-style `PadControl` path is used otherwise.
`Moga_Input.GetControllerConnected` is true as soon as Unity lists a joystick, so the Android path runs.

| Switch | Game reads | Action |
| --- | --- | --- |
| R or ZR | `rightShoulder` / `rightTrigger` pressed | fire |
| L or ZL | `leftShoulder` / `leftTrigger` pressed | aim down sights |
| X | `y`: tap / hold 0.5 s | grenade / mine |
| Y | `x`: tap / hold 0.5 s | reload / next weapon |
| B | `b` down | crouch |
| A | `a` down | interact, hold breath |
| D-pad | arrow keys | left and right snap to targets, up changes unit, down is follow me |

`Controller` reads keys through `Moga_Input`, which trusts the MOGA SDK's `getKeyCode` over Unity. That call
returns `ACTION_DOWN`, which is 0, for a held button, so it must answer 1. The default answer of 0 reported
every button held, for the whole session, while Unity's own `Input` read them correctly. A held `y` also
made the mine the only result of X, since the tap is the release of a button that never releases.

`tests/qemu/main.c` presses buttons in a mission and logs Unity's and the game's view. Read Unity's one-frame
flags from inside a frame, which is why that probe runs on a frame's first draw: between frames they are 0.

## Frame time

`watch.log` prints a `ms/frame` line each second: `game` is the time before the frame's first GL call,
`gpu_wait` the frame fence, `draw_calls` recording, `swap_wait` the swapchain image. In a mission the
console ran about 11 fps with `gpu=idle` in nearly every sample, which points at the CPU, not deko3D.
`appletSetCpuBoostMode` lifts the CPU to 1785 MHz but drops the GPU to its minimum, so it only suits loads.
`src/cpu_clock.c` sets 1785 MHz directly through clkrst instead, leaving the GPU alone, and puts it back when
the game exits. It is opt-in: creating `sd:/switch/cod_st/boost` turns it on, since the port is meant to be playable
at stock clocks; `watch.log` shows the real clock either way. Ryujinx refuses the call, which is harmless.
On 2026-11-06 a console run with the boost and the quieter log ran a survival mission at 40-50 fps (from about 14),
with `game` at 11-17 ms. Which of the two changes gave how much is not measured.

Every line of `game.log` used to reach the card at once, about 150 a second, most of them JNI calls Unity
repeats each frame. JNI calls now log once per method, and from the first frame the log stays in memory until
a thread writes it out every 2 seconds. A crash flushes it too.

`watch.log` also carries a sampled profile of the main thread every 30 seconds, by module and by 1 KB of code.
Addresses without a module are the port's own: subtract the printed code anchor's distance from
`profiler_sample` in the ELF and use addr2line. In QEMU the profile came out flat, with no single hot spot,
but its costs differ from the console's. Anything else that pauses threads must use `b_gc_pause_begin`.

Do not give JNI method ids one shared record per method: it was tried, and the harness failed to load a mission
until that was removed. The cause was not established, since the runs also suffered from the fault below.

The harness pauses threads with a signal, which can interrupt a read from the Windows-mounted `/sd`. It restarts
those reads now. Before that, the fault showed up as Unity's `Inflate Error` flood and hangs on the load frame.
In QEMU the load frame alone takes several minutes, so a run that sits on frame 250 is not necessarily hung: check
that the heap in the `STALLED` lines grows, and allow 1300 seconds.

## Message boxes

The game takes message boxes only by touch, so `src/loader/pad_dialog.c` calls what a touch would. Clicking the
right stick presses the first button and the left stick the second. The first button is on the right: "Yes" on
a purchase, and "No" on destructive questions such as quitting. Share buttons are left alone.

## Missing ground and sky (fixed 2026-11-06)

Ground, sky dome, background hills and parts of vehicles were missing, showing the camera's grey. Cause: face
culling. The device uses `DkDeviceFlags_YAxisPointsDown`, which mirrors every triangle's winding, so GL's front
face is the other one; single-sided ground and the sky dome (seen from inside) were culled, and closed meshes
showed their far walls. `apply_state` in `renderer_deko3d.c` now flips the front face. A console run with
culling off (`no_cull`) showed every graphic correct; the flipped winding itself needs a console check.

Ruled out on the way, with evidence: missing shaders (all in the pack), occlusion culling, fog (`_FogRange`
values are sane), the low-memory profile, game data (the phone's APK and OBB are byte-identical to ours).
The game also classed the console as AndroidLow at start-up: `readTotalRam` now answers 3584 (MB) and a
`/proc/meminfo` reports 3.5 GB.

Diagnostics kept, all off unless a file exists in `sd:/switch/cod_st/`:
- `capture`: `src/gl/draw_capture.h` writes 6 frames (every 30 s from frame 1800) to `capture_<n>.txt`: each
  draw's state, textures, attributes, every uniform, and its vertices run through the MVP and depth band.
- `scene_report`: `src/loader/scene_report.c` writes cameras and every renderer (active, enabled, Unity's
  isVisible, layer, mesh size, materials and shaders) to `scene_<n>.txt` at 30 s intervals.
- `no_cull`, `no_flip`: culling off, or the old winding. `no_fog`: forces `_FogRange` to no fog.
- `no_occlusion`: turns occlusion culling off on every camera (`pad_dialog.c`).

## Heap corruption at wave 4-5 (in progress 2026-11-06)

Every long survival run crashed when wave 4 or 5 started, about 5-6 minutes in: data aborts inside newlib's
malloc and inside Unity's allocator (`libunity+0xb4e78`, a garbage leaf in its pointer table), always under
Unity's memory manager (`libunity+0xb4dbc`..`0xb6098`) and with the free lists already damaged. Runs that ended
before wave 4 never crashed. The main thread freezes for about a second as each wave starts; `watch.log` had
used its 10 freeze dumps on the mission load, so none show the wave start.

`src/bionic/heap_guard.c` now sits behind the game's malloc, calloc, realloc, memalign and free (not the port's
own): a header naming the allocating call site, a 16-byte guard after each block, and a 24 MB quarantine of
freed blocks filled with a pattern. Writes past a block's end, writes after free and double frees are logged
as `heap_guard:` lines in `game.log` with the guilty block's allocator, and kept away from newlib's bookkeeping.
`no_heap_guard` turns it off. The first console build faulted at start-up: libmono (`libmono+0x2b7c0c`) frees a
pointer that ends in `...008` with an unmapped page before it, and reading the guard's 16-byte header there left
mapped memory. The guard now reads a header only for 16-aligned pointers inside newlib's heap, asking the kernel
first when the header would sit on the previous page; frees of pointers outside the heap are logged and not
passed to newlib, which would have linked them into its free lists. ELFs for addr2line:
`builds_tested/cod_st_heapguard.elf` (start-up fault) and `cod_st_heapguard2.elf` (the fix).

Console run with the guard (run10): the culling fix held, the guard logged nothing, and it still crashed as wave
5 began: `memcpy` in `buffer_address` (moving an unchanged buffer to the GPU buffer heap) wrote past the end of a
heap chunk. The allocator is correct, but its `free_range` records are newlib blocks, so they were damaged too:
the corruption hits the port's own allocations, which the guard does not cover. Found and fixed since:
- `glReadPixels` zeroed `w*h*4` bytes whatever the format; an RGB, alpha or 16-bit read ran up to `w*h` bytes
  past Unity's buffer. It now writes exactly what GL defines (format, type, `GL_PACK_ALIGNMENT`), and logs each
  format the game uses (`gles: glReadPixels`).
- Bounds checks, each logging when it fires: `glBufferSubData` against the buffer, `copy_to_image` against the
  image's level size and faces (the GPU writes the rectangle as given), JNI array regions and elements.
ELF: `builds_tested/cod_st_hardened.elf`. No GC pause failures were ever logged, and the GC stop count at a crash
(about 250) only reflects play time: it grows by about 2 every 10 s.

Also suspect, not yet changed: `gc_bridge.c` spills a paused thread's registers below the SP the kernel reports.
If that SP is stale for a thread inside an SVC that did not go through `b_blocking_enter`, the spill lands on
whatever memory is there, and thread stacks come from the heap.

## Not done

1. Long console runs: follow `crash.log` for anything that appears. On 2026-11-06 the console crashed about 310 s into
   `04_GO_Arc_Survival_S1_Baked`, right after `TimeRemainingSeconds = 0`: a data abort at `libunity.so+0xb4e78`, in
   Unity's allocator walking its pointer radix table (`ldr r3,[r7,#0x40]` with r7 = 0x3fb25028, a garbage leaf). The
   table was already corrupt, so the cause is earlier heap damage. Logs are in `console_logs/2026-11-06_run/`. `readTotalRam` is unanswered, so the
   game logs `IsHighMemoryAndroidDevice: 0` while detecting `AndroidHigh`; check this affects quality.
2. PVRTC handling: check whether Unity decodes it when the extension is not advertised.
3. Video: the game currently sees each clip as ended. The clips are H.264 with AAC.
4. Networking stays absent: sockets fail cleanly and Bedrock runs offline.
5. HOME and sleep handling, which android32 has.

## Open questions

1. Does Unity 4.3 decompress PVRTC itself if the extension is not advertised? Check at bring-up.
2. Does the game reach menus with Bedrock stubbed? Its servers are likely gone, and it is a
   live-service-era title, so some offline paths may need forcing from the C# side.
3. Texture data is 1.3 GB in total, but it streams per level. Memory is a probe question.

## Commands

Run `.\build.ps1 all` from `port/`. It uses `ghcr.io/vita2hos/devcontainer/vita2hos:latest`.
Host tests run in that image: `sh tests/run_host.sh <sd_dir>` and `sh tests/run_threads.sh`. The
threading script needs `--security-opt seccomp=unconfined`. The emulated harness is
`tests/qemu/run.ps1 [seconds] [-Strace]`, and it expects the managed DLLs in
`dist/sd/data/app/managed/`, with `mscorlib.dll` also in `managed/mono/2.0/`.

### Run 11: textures missing after a second map load (heap exhaustion)

Run 11 (the hardened build) did not crash. After one survival game the player returned to the menu and started
another; during that load the heap (sbrk) peaked at 993 MB against a ~960 MB limit for a 32-bit process, and
`gb_tex_data` logged `deko3d: out of memory holding a texture level` twice; the map then drew with missing or
unstable textures. Cause: texture levels were copied into the heap and kept there until the texture's first draw,
so a level load held a second copy of every texture (uploads grew by ~120 MB during the load), and textures never
drawn kept theirs forever. Textures with pending data are now made into GPU images at the end of the frame that
filled them (`realize_queued` in `gles_backend_present`). The heap guard's quarantine was cut from 24 MB to 8 MB.

### Run 13: the survival leak and the vertex-buffer overruns

The per-call-site report (heap_guard_report, every 30 s in watch.log) showed the game's own blocks growing only
slowly while newlib's in-use total rose ~0.2 MB/s faster. Cause: `b_mem_unmap` freed nothing, so every
mmap/munmap pair (Mono, Unity file maps) leaked. Mappings are now tracked; a whole region unmapped from its start
is freed, a partial unmap (Mono trimming for alignment) still keeps the region.
The glBufferSubData check showed a rotating pool of 24 dynamic vertex buffers (ids 75-98) that the shim sized at
12 or 144 bytes being filled with up to 56 KB: that was the heap corruption behind the wave 4-5 crashes, and
clipping it drew garbage geometry. The buffer is now grown to fit. Why the shim's size is that small is open
(a glBufferData the shim misses?).

### Run 14: GL object ids reused while still live (root cause of the wave 3-5 garbage and crash)

The munmap fix changed nothing (no munmap happens in survival). The real fault: `new_id` in gles.c wrapped to 1
after 8192 objects and handed out ids still in use. Survival creates objects steadily, so around wave 3 new
small buffers (12 / 144 bytes) took the ids of Unity's dynamic vertex buffers, new textures and programs took
live ones, and the game's 56 KB vertex writes overran the 12-byte allocations: garbage geometry, heap
corruption, the wave 4-5 crash. Ids are now tracked as live until deleted and never reused while live.

### Run 15: past wave 5, clean graphics; out of memory at the revive screen

With GL ids fixed there was no garbage and wave 5 passed. The crash at the revive screen was Unity failing a
1 MB allocation: sbrk at ~997 MB, 115 MB free inside the heap but fragmented, Unity's own labels only ~120 MB,
game blocks ~150 MB. newlib in-use still rises ~0.2 MB/s beyond the game's blocks. src/bionic/alloc_track.c
(link-time --wrap of malloc/calloc/realloc/memalign/free) samples 1 in 8 port allocations by caller and
reports the top sites every 30 s in watch.log, to name that growth.

### Run 16: the heap was lost to fragmentation, from devkitARM's malloc

The sampled port tracker showed nothing of the port's own growing (top sites: the heap guard's blocks, the
library code reserve, glBufferData copies, fake JNI objects, all flat or tiny). Disassembly of the linked
`malloc` (libg.a, libc_a-malloc.o) shows devkitARM's newlib sends every request >= 32 bytes to
`_memalign_r(32)`; newlib's memalign over-allocates and splits off a leading fragment each time, and the heap
guard added a memalign(16) per game block. Result: sbrk climbs while "free in heap" grows to 100+ MB of
pieces, and a 1 MB Unity allocation fails. The game's imports (hg_*) and the port's own malloc/calloc/realloc
(through the --wrap in alloc_track.c) now call `_malloc_r`/`_calloc_r`/`_realloc_r` directly, 8-byte aligned
as Android 4's dlmalloc was. The heap guard is now opt-in (sd:/switch/cod_st/heap_guard).

### Run 17: smooth 14-minute survival; out of memory returning to the menu

Fragmentation was gone (free-in-heap ~70 MB), but in-use still rose ~0.2 MB/s and the menu load failed a
256 KB allocation. The tracker showed fake JNI objects piling up at ~360/s (make_obj, jni_object,
make_method, plus untracked strdup'd names): Unity polls the controller through JNI every frame and nothing
is ever freed (DeleteLocalRef is a no-op). Classes, strings, stateless instances and method ids are now
interned (`shared_obj`, `make_method` in fake_jni.c). The game's direct mallocs (hg_*) now also feed the
tracker, so the next report covers everything.

### Run 18: leak gone; campaign load after survival dies without a crash record

With JNI objects shared, in-use stayed flat at ~747 MB through survival and the return to the menu worked.
Starting the campaign, watch.log ends at t=386 with frames still advancing (sbrk peaked at 931 MB, images
jumping 40 -> 149 MB) and crash.log has no new entry: the process was ended from outside the handler.
Best explanation is memory: the GPU heaps grew in 64 MB pieces, each one contiguous allocation from the
nearly full process heap. CHUNK_SIZE is now 16 MB, fully free chunks are destroyed (heap_free), and the
per-frame upload ring is 32 MB instead of 40 MB (16 MB less in all).

### Run 19: campaign dies on its first frames: a shader missing from the pak

Heap peaked at only 755 MB, so not memory. The process died ~2 s after the campaign map started drawing,
with no CPU fault, and shader_miss/1d2a4b7df726340a.txt was written that same second: a Unity
fixed-function program (texture * _glesTextureEnvColor0, no _glesColor attribute). The miss path bound
PAK_FALLBACK_KEY, whose vertex shader reads _glesColor, which this program never supplies: most likely a
GPU fault. The miss was added to gamedata/shaders/seen and shaders.pak rebuilt (901 programs; every miss
on the card is now in it). The fallback is now used only when the program has _glesColor and
_glesMultiTexCoord0; otherwise the draw is skipped. Run 18's silent death at the same point was likely
the same shader.

### Run 20: campaign straight from boot, still dies; open questions

No new shader miss, heap only 667 MB, and again death ~3 s after the campaign map starts drawing, with no
CPU fault record. Before that the loading screen ran ~135 s with frames advancing, heap and uploads flat
and nothing logged but "Missing the loading tip!": a wait, not loading (the real load took ~10 s at the
end). Sockets fail at once (b_no_network), so it is not a network timeout. sd:/switch/cod_st/thread_dumps
now makes the watchdog dump every thread every 20 s, to see what the wait and the last seconds are doing.

### Run 21: the system crash report names it: deko3d aborts in dkQueueAcquireImage

"The software was closed because an error occurred" left Atmosphere reports in sd:/atmosphere/crash_reports
(one per campaign attempt since run 19). Result 0x367 (2359-0001), User Break from diagAbortWithResult,
called by dk::detail::RaiseError(DkResult_Fail) in dkQueueAcquireImage: the swapchain acquire fails,
typically because the GPU faulted on an earlier draw. Check crash_reports first for any silent death.
Added: index-buffer bounds (an indexed draw reading past its index buffer is clamped or skipped; the binding
has no size, so the GPU would read past it), a ring of the last 1024 draws written to
sd:/switch/cod_st/gpu_error.txt when deko3d reports an error or the queue is in error state, and deko3d
messages flushed at once.

### Run 22: gpu_error.txt written; the fault is not an out-of-range draw

The queue was in error state at frame 9973. The last 1024 draws were plain triangle lists and one strip,
none reading past an index or vertex buffer, and the index clamp never fired. The campaign scene drew 76
draws a frame, then two frames of ~10 draws (textures dropped and remade at the same second) before the
error: a switch to render-to-texture. Guards added on that path: a colour target with no image, or one
not made renderable, is no longer bound (it pointed the GPU at an uninitialised view); a depth buffer
smaller than the colour target is left off; glCopyTexSubImage blits are clamped to both images and
skipped for compressed or unrealized ones; mipmaps are no longer generated for images without the 2D
engine flag (compressed).

### Run 23 and the reference video: the campaign start

The Android gameplay video shows the campaign opening with a briefing (COD_INTRO.mp4 through EasyMovieTexture,
a satellite map with video panels, 0:28-1:08) while the level loads, then the opening scene with letterbox and
radio subtitles. The port reports every video finished at once, so a static loading screen stands in for the
briefing, and the GPU error comes at the hand-over to the opening scene. In the last frames program 82b48
has an enabled attribute on a 0-byte buffer (offset 12, stride 124), bound with size 0; the vertex fetch
limit is start + size - 1, which wraps, so the fetch is unbounded and faults depending on where the buffer
sits. Such attributes are now treated as disabled (constant 0). gpu_error.txt now also lists each draw's
textures and the GPU work between draws, in case this is not it.

### Run 24: the campaign's image effect; render textures reported unsupported

gpu_error.txt with events: in the frame before the error the game makes two 1280x720 colour render textures
and a 640x360 one, then each frame binds the 1280x720 one with a 1280x720 depth buffer, clears it, draws a
full-screen quad, downsamples into 640x360 (no depth), and composites to the screen: a camera image effect
that survival never uses. All sizes, buffers and descriptors are in range; deko3d logs nothing but "the
queue is in an error state". Rather than keep guessing, glCheckFramebufferStatus now reports user
framebuffers as GL_FRAMEBUFFER_UNSUPPORTED, so Unity treats render textures as unavailable and image
effects switch off (sd:/switch/cod_st/render_textures restores them). The harness boot is unchanged
(Unity probes twice at start-up and carries on).

### Run 25 and the mission unlock

Reporting framebuffers unsupported did not stop the image effect (Unity used the render textures anyway) and
the opening mission still faults. To try later missions instead: the game keeps a debug
ActStructure.UnlockAll() that nothing calls, and saves mission locks as SecureStorage bools
"Unlock.<mission>.<section>". src/loader/unlock_missions.c calls UnlockAll() and
SecureStorage.SaveActStructure() once, 30 s after the first pad poll, when sd:/switch/cod_st/unlock_missions
exists. The phone backup holds no progress (its Unity prefs are screen settings only).

### Run 26: the GPU error's cause: depth textures taken as RGBA8

Mission 2 (unlocked) played for two minutes, then the same 2359-0001 when zooming a sniper scope: the scope
makes 512x512 render textures and the queue errors two frames later, as the campaign opening's image effect
did. game.log had "unknown texture format 0x84f9 type 0x84fa": Unity's render-texture depth is a
GL_DEPTH_STENCIL_OES / UNSIGNED_INT_24_8 texture, which client_format turned into RGBA8, then bound as the
depth buffer: an invalid zeta format. 0x84F9 and DEPTH_COMPONENT now map to depth formats, a colour image
attached as depth is left off, and depth images use UsageRender | HwCompression like the screen's.
The framebuffer-status experiment from run 25 is reverted.

## Installable title (home-menu icon)

sphaira and hbmenu start NROs inside 64-bit hbloader, which cannot run this AArch32 code whatever address
space is chosen. Besides the hbl override, the port installs as its own application, title ID
0100C0D57E4A0000, built from the same exefs:

    build/title/exefs/main       <- build/cod_st.nso
    build/title/exefs/main.npdm  <- build/cod_st.npdm
    build/title/control/control.nacp            nacptool --create 'COD Strike Team' 'Activision (port)' '0.1.0'
    build/title/control/icon_AmericanEnglish.dat  256x256 JPEG of res/drawable-xxhdpi-v4/app_icon.png
    hacbrewpack -k <prod.keys> --exefsdir exefs --controldir control --noromfs --nologo
                --titleid 0100C0D57E4A0000 --titlename "COD Strike Team" --titlepublisher "Activision (port)"

hacbrewpack is in %LOCALAPPDATA%\Carafe. The result goes to dist/ and the card's root; install with DBI.
It is unsigned, so it needs sigpatches like any homebrew title. Data and logs stay in sdmc:/switch/cod_st.

## Controls added after the title install

- Boot shows one black frame instead of a second of green (main.c).
- Revive screen (ContinueScreen, not a MessageBox): R3 calls GMGRevive (survival) or CampaignReviveAll
  (missions), L3 calls FailMission. pad_dialog.c tries it when no MessageBox is up.
- Virtual mouse: Minus toggles it (Minus no longer sends Back). Left stick moves the pointer (squared
  response, 16 px a frame at full tilt), A touches the screen at the pointer as pointer id 100, hold A to
  drag. While on, the left stick and A go only to the pointer. The pointer is three scissored clears on the
  swapchain image after the blit (renderer_deko3d.c draw_cursor), so it needs no shader.

## The installed title is a 32-bit NRO loader (loader_title/)

The title 0100C0D57E4A0000 no longer carries the port: its program is loader_title/main.c (Makefile.loader,
`.\build.ps1 -f Makefile.loader`), with the port's NPDM, crt0 and crt0_reloc.c. It loads
sdmc:/switch/cod_st/cod_st.nro as hbloader would, so the title is installed once and the port is updated
by replacing the NRO:
- the process heap is set to the size nx_init.c would choose; the NRO image is read to its start and
  mapped with svcMapProcessCodeMemory into a free range of the 32-bit code region, then text RX, ro R,
  data+bss RW;
- config entries: main thread, process handle (from crt0_reloc.c), applet type Application, the rest of
  the heap as OverrideHeap (nx_init.c uses it instead of svcSetHeapSize), all syscalls, argv, loader info;
- sm/fs are closed before entering the NRO at offset 0 with r0 = config, r1 = -1, lr = svcExitProcess;
- loader_title/exc_forward.S sends kernel exceptions (which enter the loader's crt0) to the NRO's offset 0
  with r0/r1/sp unchanged, so the port's handler runs.
It writes sdmc:/switch/cod_st/loader.log; a failure breaks with 0xC0D500nn (step) in the crash report.
Packaging: build_loader/loader.nso -> build/title/exefs/main, build/cod_st.npdm -> main.npdm, then the
hacbrewpack command above; dist/COD Strike Team Loader [0100C0D57E4A0000].nsp. The hbl-override NSP
(switch/cod_st/cod_st.nsp) still starts the port directly. Untested on hardware as of 2026-11-07.

## Button changes (2026-11-07)

| Switch | Now |
| --- | --- |
| L | drone view <-> first person: CommonHudController.ZoomToggled() (what a MOGA Pro's L1 does); no longer L1/aim |
| R | grenade: StartThrowGrenadeFirst + EndThrowGrenadeFirst, in first person, unpaused, unless TPPDropGrenadeLocked; no longer R1/fire |
| L3 | D-pad left (target left) as well; on a prompt, its left button / the revive screen's "No" |
| R3 | D-pad right (target right) as well; on a prompt, its right button / revive |
| A | also presses a prompt's right button / revive; in play it keeps its own job (no prompt, no effect) |
| ZL / ZR | aim / fire, unchanged |
The virtual mouse pointer is a blue arrow (white outline), tip at the click point.
