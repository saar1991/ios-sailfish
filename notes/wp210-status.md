# WP210 iOS port: status notes (updated 2026-11-14 22:30)

Working notes so the state survives session resets. Times are local
(Asia/Jerusalem) unless stated. The newest state is at the top; the older
notes are kept under "History" at the end.

## Summary

- **Hardware:** Oukitel WP210, SoC MT6896Z/CZA (Dimensity 8200) per its device tree. The repo calls the platform "MT6895".
- **Software:** iOS 16.7.16 (20H392), iPhone 8 (D20) userland. The UI is laid out at 750x1334.
- **Display:** drawn by the `sweetgl.dylib` shim (Mesa softpipe, all on the CPU), injected into backboardd.
- **Status:** iOS boots to the home screen with correct colours and rounded corners. It is usable but very slow: taps, Home presses and swipes lag by seconds, and heavy screens take seconds per frame.
- **Cause found:** CoreAnimation draws each frame as about 85-110 mostly full-screen layers, and softpipe interprets every pixel of every layer (see "Why the UI is slow").
- **Work in progress:** CPU fast paths in the shim for CA's simple draws (`SGLFAST`).
  - The first version (bb529ae) matched softpipe exactly but did not raise the frame rate: it covered too little, and its loop was slow.
  - **eed85c5** widens it (half-float colours, straight-alpha blending) and makes the loop much faster. It is pushed and waits for the CI build.
- **Also open:** Control Center draws without its background (lead: CA texture function 0x3d is missing).

## Phone state now

- **Shim installed:** build **e0ea1da** (`f2f7b4b4edfd025d6c1802bf9a744e893a4a2baa08bfc731bf26c719466d3604`), `INSTALL VERIFIED` at 19:46 (field-level check passed).
  - It replaced eed85c5 (`141eb8a7…`), whose backup is `/data/local/tmp/sweetgl-check.9BMeTY/before.bin`.
  - Check what's installed, read-only, from Android:
    `adb shell "su -c '/data/adb/magisk/busybox dd if=/dev/block/sdc bs=4096 skip=122729397 count=4202 iflag=direct 2>/dev/null | sha256sum'"`
- **boot_a:** `wp210-boot-fast1.img` (`SGLFAST=1`, which self-checks since e0ea1da), flashed again at 19:46 for the e0ea1da run.
- **Boot images in `C:\Users\artzi123\Downloads\wp210-shim`:**
  - `wp210-boot-fast2.img`: verify mode, 7442432 bytes, built 17:17
  - `wp210-boot-fast1.img`: speed run

  Both use the k-watch kernel (wp210-auto-input 6c51db1, `kernel.development.sweet` `1ca9bd9b…`), `wp210.adt` with 8 cores, the stock `boot.img` (`74d48f32…`) and `notes/bootargs/args-fast2.txt` or `args-fast1.txt`. Those are `args-freq.txt` plus `SGLFAST` in sweetenv, with `sweetperf=2`, `wp210auto=90`, `wp210cpumhz=2400` and `sweetprof=backboardd,SpringBoard`.
- **Settings on the phone:** Reduce Transparency and Reduce Motion are on.
- **Backups:**
  - original pre-BGRA shim `2db9d1eb…`: PC copy in `C:\Users\artzi123\Downloads`
  - BGRA shim `309a61d7…`: `before-bgra.bin`, on the PC and on the phone
  - the allocation files (`sweetgl-diag-<commit>-allocation.bin`) for each install were pushed from `C:\Users\artzi123\Downloads\wp210-shim`; keep them there
  - every install keeps the replaced shim as `before.bin` in its `/data/local/tmp/sweetgl-check.*` folder

## Branches

| Branch | Head | What |
|---|---|---|
| `main` | 1465988 | BGRA upload for format-0 4bpp IOSurfaces (the copy-error fix) |
| `sweetgl-diag-0x502` | eed85c5 | shim work on top of 1465988 (table below) |
| `wp210-auto-input` | 6c51db1 | kernel patches 0204-0207, `mkadt.py --bigfirst` |
| `wp210-status-notes` | this file | notes, install scripts, tools, boot-args |

## Shim builds (sweetgl-diag-0x502)

The installed file's catalog size is 17208704 bytes, and a build must fit that. Since e98dfef, `build-sw.sh` strips local symbols (`strip -x`, about 950 KB) and the artifact also carries `sweetgl.unstripped.dylib` for reading profiles.

| Commit | What | Size | Allocation SHA-256 | Installed |
|---|---|---|---|---|
| 22ec055 | trace-window code removed | 17208160 | `552d50d6…` | yes, then replaced |
| d17061a | fbfetch trace 12 -> 200 per program | 17208160 | `08cc1bf0…` | yes, then replaced |
| 3d67b58 | 200 errors per call; fbfetch setup error check | 17208240 | - | no (fedeca3 went in instead) |
| fedeca3 | **glFlush before the fbfetch barrier: fixes the square corners** | 17208272 | `22a39989…` | yes, then replaced |
| 41be079 | per-frame stats (`slow frame` lines); per-draw tracing removed | 17208272 | `13e97388…` | yes, then replaced |
| 75b34d3 | per-draw list for the first 4 frames of 2 s or more | 17208480 | `da79097d…` | **yes, current** |
| e98dfef | CPU fast paths (`SGLFAST`), stripped build | - | - | CI failed: implicit declaration of `fast_shader` |
| bb529ae | forward declaration of `fast_shader`; stripped build | 16274576 | `f7c8d02b…` | yes, then replaced |
| 14f179d | fast-path diagnostics: time per reject reason, top 6 programs, reject details (logging only) | 16275200 | `b1059f33…` | yes, then replaced |
| eed85c5 | half-float colours (fill, tint, alpha-tint), straight and mixed alpha blending, fast rows (memcpy for 1:1 copies), dispatch_apply over cores, map/pixel timing, program log at link | 16275584 | `141eb8a7…` | yes, then replaced |
| 57394b7, e0ea1da | copy+noise and tint+noise kinds, atop blend, whole-pixel edges, clamped blends; SGLFAST=1 self-checks first | 16275808 | `f2f7b4b4…` | yes, then replaced |
| a09c1a7 | softpipe's column coverage rule instead of whole-pixel edges | 16275792 | `b8bf661b…` | yes, then replaced |
| 27eb0f3 | turned textures (u along y), shape reject reasons | 16275872 | `da8d58c3…` | yes, then replaced |
| 92e026d | map failure reasons; RGBA copy for sources in other formats | 16275936 | `577d72ae…` | yes, then replaced |
| 3a1708a | darken-over-backdrop kinds, clamped texcoords | 16276064 | `a6f61661…` | yes, then replaced |
| f7ea6b1 | multi-rect draws, mask kind, check budget 10, summary in two lines | 16292624 | `142cccbf…` | yes, then replaced |
| 9a67231 | nine-slice pairing, mipmaps when not minified, luminanceCurve, dst-out | 16292736 | `14dd9a0c…` | yes (about 21:57) |
| e5f492c | flat nine-slice cells, ONE/ONE blend, colour matrix over the backdrop | 16292800 | `e18828d6…` | **yes, current** (about 22:17) |
| 5956cb2 | diagnostics: draws over 300 ms, unrecognised fragment shaders logged whole | 16292912 | `cb25b600…` | yes (about 22:40) |
| 9bfed51 | supercircle / circle shapes, masked supercircle icons, mix over the backdrop; unrecognised vertex shaders logged | 16292992 | `3d2abd4a…` | yes (about 07:52) |
| d8a55df | CA's blur passes; per-kind detail budget, mesh lines for pairing failures | 16310032 | `e0f2c5c9…` | yes (about 08:25) |
| 0e13161 | self-check: 3 draws covering 1M pixels is enough; nine-slices checked as a whole; CPU time by kind | | | pushed, to build |

bb529ae was checked before sending: signature page hashes all valid and the signature ends at EOF. The FILL and COPY shader patterns don't appear as strings because clang turned those two strcmp calls into inline 8-byte compares; the constants were confirmed in the disassembly. The padded file has 936816 zero bytes after the Mach-O.

The install scripts that were sent for each build are in `notes/scripts/install-sweetgl-diag-<commit>.sh`. Their metadata hashes are the values at the time of that install.

## Installing a new shim build (the procedure that works)

**From eed85c5 on, the install script checks the catalog by field** (`notes/tools/catalog-check`). It checks:
- the volume geometry and the catalog's location
- that node 19204 is in use
- the shim's live record: file 318814, 17208704 bytes, 122729397 x 4202
- the four index nodes, by hash

So `--check` passes after an ordinary iOS boot without refreshing hashes. If it prints `CHECK PASSED`, `--install` can follow directly; a `STOP` line names the field that changed. The steps below are the old procedure, needed only with scripts before eed85c5.


1. **Download the artifact.** Run the `sweetgl-sw` workflow on `sweetgl-diag-0x502` and download its zip.
2. **Check and pad the dylib.** It must be at most 17208704 bytes, with the code signature ending at EOF. Pad it with zeros to 17211392 bytes (4202 blocks of 4 KiB); that is the allocation file.
3. **Write the install script.** It needs:
   - `OLD` = the allocation hash of the build that is currently installed
   - `NEW` = the new allocation hash
   - `INPUT=/data/local/tmp/sweetgl-diag-<commit>-allocation.bin`
   - the current list of 9 metadata block hashes
4. **Push the files** to `/data/local/tmp`: the allocation file, the script and `check-metadata-readonly.sh`.
5. **Run `--check`.** Every iOS boot changes the volume header and the first catalog nodes, so `--check` usually stops on `metadata.bin` after a boot. When it does:
   - Run `check-metadata-readonly.sh`. It is read-only and lists which blocks differ.
   - If block 121448308 (catalog node 19204, which holds the `sweetgl.dylib` record) **matches**, refresh only the changed hashes in the script.
   - If 121448308 differs, dump node 19204, diff it against the earlier dump, and confirm that the `sweetgl.dylib` record is unchanged. That record is file ID 318814, parent `lib` 313920, size 17208704, one extent at 122729397 x 4202. Only then refresh the hashes.
6. **Install.** Run `--check` until it says PASSED, then `--install`, which must report `INSTALL VERIFIED`.

The layout is the same as in the History section: the volume starts at device block 119713536 on `/dev/block/sdc`, and the catalog extents are 183682x16384, 211717x8192, 1133760x8192 and 1729132x8192 (volume blocks).

## Boot images

```
python3 tools/mkpayload.py KERNEL wp210.adt payload.bin --cmdline "$(cat args-X.txt)"
bash wp210/loader/build.sh STOCK_BOOT payload.bin wp210-boot-X.img
fastboot flash boot_a wp210-boot-X.img && fastboot set_active a && fastboot reboot
```

Exact paths, from WSL (`build.sh` must run from `wp210/loader`):

```
ADT=/home/saar/ios-sailfish/wp210.adt
STOCK=/mnt/c/Users/artzi123/Downloads/OUKITEL_WP210_EEA_V11_20251201/boot.img
cd ~/ios-sailfish
python3 tools/mkpayload.py ~/wp210-build/k-watch/kernel.development.sweet "$ADT" ~/ios-sailfish/payload-X.bin --cmdline "$(cat args-X.txt)"
cd wp210/loader
bash build.sh "$STOCK" ~/ios-sailfish/payload-X.bin ~/ios-sailfish/wp210-boot-X.img
cp ~/ios-sailfish/wp210-boot-X.img /mnt/c/Users/artzi123/Downloads/wp210-shim/
```

- **Stock boot image:** `boot.img` from the firmware package. Its SHA-256 is `74d48f32800d8c3a8e63919bd8383681ab8fd0e949611e231aa7e6842d215aa9`, and `wp210-shim\boot.img` is the same file. `mkboot.py` takes only its ramdisk and OS version.
- **k-watch kernel:** `~/wp210-build/k-watch/kernel.development.sweet`, SHA-256 `1ca9bd9b0d96b133…`.
- Back to Android: `fastboot set_active b` then `fastboot reboot`.
- The boot-args limit is 608 bytes. Check the length before building, and check that the file isn't empty (an empty cmdline still builds an image).
- Copies of the boot-args:
  - `notes/bootargs/args-freq.txt`: the last boot, 594 characters
  - `notes/bootargs/args-fast2.txt`: the same plus `SGLFAST=2` in sweetenv, 604 characters (next boot, verify mode)
  - `notes/bootargs/args-fast1.txt`: the same plus `SGLFAST=1`, 604 characters (the speed run, only after a clean verify)
- **Never use `sweetperf=1`.** It reads Qualcomm clock registers that the WP210 doesn't have, and the boot hangs at the loader. `sweetperf=2` is the WP210-safe probe.

## Logs

- **Pull** (phone booted to Android): `notes/tools/pull-sweetlog.bat` (one command; keeps a time-stamped copy). It does
  `adb shell "su -c 'dd if=/dev/block/sdc of=/sdcard/sweetlog.bin bs=4096 skip=124955639 count=1025'" && adb pull /sdcard/sweetlog.bin`.
  In cmd, chain commands with `&&`, not `;`.
- **Decode:** `python3 tools/sweet-log.py sweetlog.bin` (in the main repo).
- **Profiles:** `sweetprof=PROCS` samples PCs; odd PCs are callers.
  - Shared-cache addresses resolve with `notes/tools/resolve-callers.sh` (needs `ipsw`; the cache is at `/home/saar/ios/dyld/20H392__iPhone10,1_4/dyld_shared_cache_arm64`).
  - Addresses inside the shim resolve with `notes/tools/prof.py` and the unstripped dylib.

## Kernel patches (wp210-auto-input)

- **0204 `wp210auto=SECS`:** scripted input, starting SECS after boot. Three rounds of:
  - idle 4 s
  - home
  - swipe left
  - swipe right
  - Spotlight down
  - home
  - tap dock icon 1, then 6 s
  - home
  - Control Center up
  - home

  Each step logs `WP210 AUTO r= s= name first_ms= frames= last_ms=`, measured from the framebuffer swap count.
- **0205 `sweetperf=2`:** logs MIDR, MHz and memcpy speed per CPU once, then every 10 s the MHz of all 8 CPUs, swaps, gaps, touch moves and `SWEET TOP`.
- **0206 `wp210cpumhz=MHZ`:** MediaTek cpufreq-hw through CSRAM at 0x11b000. Domains are at offsets 0xc10, 0xd30 and 0xe50. It enables each domain and writes the highest table index at or below MHZ.
- **0207:** every 10 s it logs the cpufreq state, hw, limit and cap words, and rewrites the wanted index if it changed.
- **`tools/mkadt.py --bigfirst`:** orders the cores 0,4,5,6,7,1,2,3. The default output is unchanged. Not used, because the 5-core layout halved the clocks.

## CPU findings

- **Cores:** cpu0-3 are Cortex-A55 (part 0xd05), cpu4-6 Cortex-A78 (0xd41), and cpu7 is the top A78.
- **Bootloader clocks:** about 1000 / 1390 / 493 MHz.
- **With `wp210cpumhz=2400` and 8 cores:** the A78s run at 2350 MHz and cpu7 at 2400 MHz, holding steady for the whole run. The phone stays cool. The firmware limit words are 0; the cap words read `0xcb00ce 0xc300c7 0xba00bf 0xad00b4` and never change.
- **Open:** the A55s read about 999 MHz although their domain register says index 0 (2000 MHz). This may be an artefact of the probe.
- **Keep 8 cores** (`cpus=8`, default ADT). The 5-core big-first layout halved the clocks.

## Why the UI is slow

- backboardd composes every frame with about 85-110 GL draws. Most are a single full-screen quad (750x1334) with a trivial shader:
  - colour fill
  - texture copy
  - tinted copy
  - the fbfetch programs
- softpipe interprets each pixel, so draw costs are:
  - full-screen fill: about 58 ms
  - full-screen texture copy: 70-256 ms
  - fbfetch program 9: about 490 ms
  - worst single draw: 1.18 s (program 5 copying a 1271x1823 texture over the whole screen)
- Most of the time goes into offscreen framebuffers, not the one that is presented.
- **Log cbf8881a** (75b34d3, auto test):
  - 36 frames took 250 ms or more: median 2.3 s, maximum 25 s, about 85 draws on average
  - backboardd is pegged at 100% of one core
  - in the auto test, round 1 Home showed its first frame after 97 ms, but most later steps took 0.7-2.7 s, and in round 3 most steps showed no frame at all within their window
- **Also seen, not yet investigated:** launchd at about 70% CPU and mediaserverd at about 48%, both spinning constantly.
- **0x502 errors:** they come from the shim's own fbfetch setup (`fbfetch setup err`), not from CA's draws. Low priority.

## Fast paths (SGLFAST): e98dfef, bb529ae, 14f179d, eed85c5

- **What qualifies:**
  - CA's standard vertex shader, which every program uses
  - one of six fragment shaders, compared as source text with whitespace removed: fill, copy, tint, alpha-tint, opaque copy, alpha copy
  - an axis-aligned rectangle of 6 triangle vertices or a 4-vertex strip, from client arrays of float, unsigned byte or (since eed85c5) half-float, which is CA's colour format
  - blending: none, premultiplied (ONE / ONE_MINUS_SRC_ALPHA), or since eed85c5 straight (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) and mixed (straight colour, premultiplied alpha)
  - a 2D texture with clamp-to-edge, level 0, NEAREST or LINEAR, identity swizzle
  - drawn into a texture-backed framebuffer

  Anything else goes to softpipe as before.
- **How:** `fastpath.c` maps the textures with `pipe_texture_map_3d`, which makes softpipe rasterize its queued draws and flush its tile cache first, and mark the texture as changed afterwards. Then it runs the pixel loop.
- **The loop since eed85c5:**
  - same-order 1:1 copies are memcpy rows, or a direct premultiplied blend
  - unfiltered sampling reads one texel
  - otherwise bilinear with separable tables
  - rectangles of 64K pixels or more run in 16-row chunks with `dispatch_apply_f`
- **Modes** (`SGLFAST` in sweetenv):
  - 0 or unset: only counts how many draws qualify and why the others don't
  - 1: the CPU path draws them
  - 2: verify mode. softpipe still draws; the CPU result is compared with softpipe's, including a 1-pixel border, and up to 60 `fastverify` mismatch lines are logged. Up to bb529ae the first 400 draws were checked; since eed85c5 it's the first 60 of each kind and blend mode.
  - A trailing `s` (`1s`, `2s`) keeps the loop on one thread (eed85c5).
- **Log lines** (every 10 s unless noted):
  - `sweetgl: fast mode M: N draws; fill … copy …: X ms in softpipe, Y ms here; not: …; verify ok A bad B`
  - `sweetgl: fast here: map … ms, pixels … ms; blend none … premul … straight … mixed …` (eed85c5)
  - `sweetgl: fast not, ms: kind … mode … blend … array …` (14f179d)
  - `sweetgl: fast prog P kind K: T ms, D draws, Q qualify, not 0x…` for the 6 costliest programs. The reason bits are 1 kind, 2 mode, 4 state, 8 blend, 16 array, 32 shape, 64 tex, 128 fb, 256 map (14f179d).
  - `sweetgl: fast no <reason> prog …`: the details of the first rejects of each reason (14f179d)
  - `sweetgl: fast link prog P fs F kind K vs V: <shader body>`, at link (eed85c5)
  - `sweetgl: fastverify kind … blend … rect … px differ …`
- **Checked locally:**
  - all of `eagl.c` and `fastpath.c` compile with stand-in Apple headers
  - the new loop is tested on the host with `notes/tools/fptest`: bit-identical to 14f179d's loop for replace and premultiplied blending, and exact against a float model for straight and mixed
  - the half-float conversion is checked on all 65536 values

## Verify run (log fde81b55, bb529ae, `SGLFAST=2`, 17:20)

- **Verification:** all 400 checked draws match softpipe (within 2 per channel, 1-pixel border included), 0 bad.
- **Qualifying draws:** 3224 of 12140 (27%): copy 2785, alpha 420, opaque 19. They took **57.7 s** of softpipe time, about 30% of the slow-frame draw time (188 s).
- **Fill, tint and alpha-tint: 0 draws qualify.** The shaders are classified correctly (programs 3, 7, 17), so the colour or index arrays are the likely cause. They are probably among the 19% `array` rejects; build 14f179d logs the details.
- **Reject counts:** kind 6017 (50%), array 2342, blend 475, mode 62, tex 20, map 5. State, shape and fb: 0.
- **Costliest programs in the per-draw lists:**
  - program 5 (copy): 47%
  - program 9 (darken blend with backdrop fetch, plus noise): 12%
  - program 19 (backdrop mix): 8%
  - programs 7 (tint) and 3 (fill): 7% each
  - program 21 (tint plus noise): 6%
  - program 15 (copy plus noise): 4%
- **Programs:** all of them share vertex shader 1. Programs 15 and 21 are the copy and tint shaders plus CA's noise dither (`texture2DProj` of a noise texture). Programs 9, 11 and 19 read the backdrop.
- **Frames:** 30 slow frames, median 2.4 s, maximum 32 s (verify adds work for the first 400 draws). The previous run had a median of 2.3 s.

## Speed run (log 0ded9c98, bb529ae, `SGLFAST=1`, 17:34, video 17:38)

- **No visible corruption** from the fast path's own pixel writes; the only glitch was Control Center, which was the same before.
- **Per draw:** a qualifying draw cost 5.5 ms on average on the CPU path, against 17.9 ms in softpipe in the verify run. Across the log: 7585 qualifying of 31475 draws, 42 s on the CPU path.
- **Frame rate did not change.** Inside the automated-test window it was 1.28 / 1.25 / 1.30 frames per second for t40 (75b34d3) / verify / speed. In-window slow frames total 88 s over 19 frames, 73 s over 12 and 94 s over 24.
- **Why:**
  - **Coverage:** only about 25% of draws and time qualify. Fill (program 3), tint (program 7), the noise variants (15, 21) and the backdrop programs (9, 19, 25) all stay in softpipe, and program 5 (copy) is still often the costliest program in a frame.
  - **Switching cost:** each CPU draw maps the framebuffer texture. That makes softpipe rasterize what it has queued and write back its colour tile cache, which the next softpipe draw must reload. Part of the saving moves into the neighbouring softpipe draws.
- **Steps (first-frame ms)** vary a lot from run to run and are not clearly better.
- **Next:** the 14f179d run (booted 17:50) shows time per reject reason and per program, and why fill and tint are rejected.

## Diagnostics run (log 22868134, 14f179d, `SGLFAST=1`, 17:50)

- **Why fill, tint and alpha-tint never qualified:** CA's colour attribute is **GL_HALF_FLOAT** (`0x140b`), 4 components with a 48-byte stride, and the fast path accepted only float and byte arrays.
- **Reject time by reason:** kind 97.7 s, array 28.7 s, blend 13.5 s, mode 1.0 s, tex 0.14 s.
- **Blend rejects:** program 15 (opaque copy) blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA for both colour and alpha.
- **Mode rejects:** draws of 4 or 8 rectangles (24 or 48 indices).
- **Tex rejects:** an ALPHA-kind texture with the A,B,G,R swizzle.
- **CPU path:** 30.5 s for 8287 draws. Program 5 (copy) now qualifies 7474 of its 7641 draws but still takes 21% of all time, so the loop needed to get faster.
- **Most expensive programs** (share of time in the top-6 lists):
  - program 9 (darken with backdrop read plus noise, about 640 ms a full-screen draw): 23%
  - program 5: 21%
  - program 3 (fill): 12%
  - program 13 (kind 0, 8-rectangle draws): 10%
  - program 15: 9%
  - program 7 (tint): 6%
  - programs 47, 39, 51 and 19: about 15% together, in very few draws (program 47: 8.6 s in 2 draws), probably blurs
- **Program numbers change between boots** (alpha copy is 23 here, 13 in the shader-dump boot), so eed85c5 logs each program's shader at link.
- **Fixes in eed85c5 (host-tested):**
  - half-float attributes
  - straight and mixed blending
  - memcpy rows for 1:1 copies (15.7 to 0.4 ms for a full screen on one host core)
  - single-texel reads when there is no filtering
  - dispatch_apply over cores
  - verify budget of 60 per kind and blend
  - map vs pixel-loop timing

## d8a55df run (log 62a4a7ac, `SGLFAST=1`, about 08:30): blur on the CPU

- **Blur:** recognised (5 programs) and drawn here. Up to 40 blur draws per 10 s, no differences.
- **Steps:** 30/30 drew a frame (first time), median first frame 0.32 s.
- **No 10 s frames any more:** worst slow frame 3.1 s (11.3 s before), mean 1.0 s.
  - Swap rate 2.65/s. The last runs were 3.72, 2.45, 2.58, 2.65: the swap count is a noisy measure, and the latencies show the change better.
  - tap_dock1, first frame: 0.20/0.08/0.32 s. It was 0.69/5.4/4.4 s in e5f492c.
- **Kinds not handled are almost gone from softpipe:** "kind" was 20-60 ms per 10 s, against about 2 s before.
- **What remains:**
  - **The CPU path:** about 3 s per 10 s; pixels 2.1-3.1 s, map 0.3-0.5 s.
  - **Qualifying draws softpipe drew:** self-checks, and draws waiting for their kind's checks. About 15 s per run.
  - **Shape rejects:** 0.5-0.85 s per 10 s.
    - **Masked nine-slices (prog 13):** rotated (the meshes are logged), so not rectangles.
    - **Mix (prog 27) and masked supercircle (prog 65):** corner cells are single triangles (the cell's other triangle has zero area).
  - **Texture rejects:** 0.3-0.45 s per 10 s.
- **CPUs:** cpu0-3 (A55) read about 999 MHz, cpu4-7 (A78) about 2.36 GHz.
- **Next build (0e13161):**
  - checked enough after 3 draws covering 1M pixels
  - nine-slices are verified as a whole (they could only wait before)
  - per-kind CPU time and pixels

## 9bfed51 run (log 86650232, `SGLFAST=1`, about 07:55)

- **Self-check:** no draw differed. 2.58 frames/s, 27/30 steps, median first frame 0.41 s. It ended on the home screen (10/s).
- **Recognised:** all four new kinds (prog 27 mix, 45 supercircle, 69 masked supercircle, 89 circle).
- **Circle:** drawn on the CPU (130 draws, verified).
- **Masked supercircle (prog 69, about 5 s per run) and mix (prog 27):** almost every draw was rejected as a shape. For prog 27, 8 triangles don't pair into rectangles. The detail lines for prog 69 weren't printed, because the shared budget was used up early in the boot.
- **Blur vertex shaders:**
  - 4 taps at `t0 + offset[i]`, or 14 at `t0 ± offset[0..6]`.
  - The fragment shaders take 4, 8, 10, 12 or 14 of them, in symmetric pairs, weights `weight[0..1]`, and divide by alpha when `divide` is set.
  - `notes/tools/srcs.py` pulls the sources out of a decoded log.
- **Next build (d8a55df):**
  - the blur kind, generic: parsed from both shaders; float bilinear sampling as softpipe does it, clamp to edge
    - host test `blurtest.c`: 4.3M pixels, max difference 1
    - a full-screen 12-tap pass takes 137 ms on one host core (softpipe 2.1-2.6 s on the phone)
  - detail budget per reason per kind
  - meshes of failed pairings logged

## 5956cb2 run (log e556f5f9, `SGLFAST=1`, about 22:42): diagnostics

- **Self-check:** no draw differed. 2.45 frames/s, 26/30 steps (a slower run; this build changed no drawing). It ended on the home screen this time (10 swaps/s after the test).
- **Draws over 300 ms (34):** the blur passes are the big ones. Programs 45, 95 and 39 read 752x1336 and write 756x1340 or 750x1334, at 2.1-2.6 s per draw.
- **Blur shaders** (unrecognised `fs 0 … vs 0` programs): CA's separable Gaussian.
  - Each pass reads 4-12 taps of texture0 at texcoord0..13, with symmetric weights `weight[0..1]`.
  - With `divide`, the result is divided by its alpha.
  - The tap offsets are computed in their own vertex shader, so 9bfed51 logs unrecognised vertex shaders.
  - About 10.5 s per test run in softpipe.
- **Shape helpers, now in the log verbatim:** `supercircleImage` (pow 2.68 superellipse) and `circleImage`, both antialiased with `fwidth`.
- **Time per test run:** softpipe (kinds not handled) about 23 s; the CPU path about 18 s (prog 13, masked nine-slices, 9.6 s in all), so the CPU loops are worth speeding up too.
- **Next build (9bfed51):**
  - supercircle and circle times colour
  - texture times supercircle mask (program 69, 2.3 s)
  - mix(texture1, framebuffer, texture0.a) (program 27, 2.5 s)
  - host test `notes/tools/fptest/shapetest.c`: 17.6M pixels against a double model of softpipe's coarse derivatives, max difference 1

## e5f492c run (log 104d951b, `SGLFAST=1`, about 22:20): 3.72 frames/s

- **Self-check:** no draw differed (the colour-matrix and ONE/ONE kinds included).
- **Test window:** 3.72 frames/s (3.39 for 9a67231; same measure, the mean of the swap rate between start and done). **28/30 steps** drew a frame (best). Median slow frame 0.76 s (0.87 s).
- **Two 10-11 s frames,** both in tap_dock1 (rounds 2 and 3): one unrecognised program (45 this boot, `fs 0`) took 9.1 s for 2 draws. In other frames the same program is the small blur chain (288x139, 721x715, about 10 ms a draw). Without those two frames the mean slow frame would be about 1.0 s.
- **Stuck on Control Center at the end:** in round 3 SpringBoard laid out Control Center again while the swipe was in progress (42 layout warnings between touch down and up). It drew 2 frames in 3 s, and the Home press at 211 s came while it was still opening and changed nothing. Afterwards nothing animates (0 swaps/s; earlier runs went back to the home screen's 10/s), and SpringBoard and backboardd are idle, not spinning. It looks like a missed Home press, not a hang. To check: press Home or swipe it down.
- **Still in softpipe:** supercircle mask (63, 1.9 s per 10 s), circleImage (77), the blur programs, ARGB-swizzled textures (tex rejects).
- **Next build (5956cb2):** diagnostics only. It logs every draw of 300 ms or more, and the whole source of unrecognised fragment shaders, so the blur and circle-mask kinds can be written next.

## 9a67231 run (log 18eee082, `SGLFAST=1`, about 21:58): 3.39 frames/s

- **Self-check:** no draw differed.
- **Test window:** 3.39 frames/s (2.65x the 1.28 before the fast path). Slow frames 1.37 s on average, median of all 0.85 s, worst 6.5 s; the first frame after a step took **0.76 s** on average (best); 26/30 steps drew a frame.
- **Masked nine-slices:** 6027 of 10487 draws now run here. Some nine-slices failed to pair (18 triangles): cells of zero width or height.
- **Other kinds:**
  - luminanceCurve: 45 draws
  - ONE/ONE (additive) blends: rejected (programs 25, 7)
  - colour matrix over the backdrop: programs 31 (4 rows, 5%) and 51 (3 rows, 4%)
  - also left: supercircle masks (63) and blur programs (71, 91, 59)
- **Next build:** flat cells skipped in pairing, the ONE/ONE blend, and the colour-matrix kinds.

## Display: black bars (question at 21:56)

- **Cause:** the panel is 1080 x 2412 (NT37705 FHD+, `goodix,panel-max-x/y` and `tpd-resolution` in the device tree), about 20:9. iOS runs as an iPhone 8 at 750 x 1334 (16:9), scaled to the panel's width (1080 x 1921). That leaves about 245 pixels black at the top and bottom.
- **Options:**
  - render iOS taller: the native way, but costs more pixels and needs a size the iPhone 8 doesn't have
  - stretch vertically: distorts the image
  - zoom: crops the sides
- **Other iPhones:**
  - **iPhone X** (A11 like the iPhone 8, iOS 16, 1125 x 2436): about 37-pixel bars, but 2.7x the pixels, no Home button (gestures), and a different IPSW and device tree
  - XS/XR and later: arm64e userland (pointer authentication), which the WP210's Cortex-A78/A55 cores lack (only the A78C variant adds it). Not practical.
- **Decision for now:** keep the iPhone 8 and its bars until rendering is several times faster.

## f7ea6b1 run (log aebf997c, `SGLFAST=1`, about 21:36): fastest frames so far

- **Self-check:** no draw differed.
- **Test window:** slow frames 1.43 s on average (1.9-2.2 s before), median of all 0.85 s (1.6-1.7 s), worst 4.9 s (9.5-25 s before); **29/30 steps** drew a frame. Frames/s 1.86; the count follows how much is animating, so frame times are the better measure.
- **The mask kind** (program 13, 27% of the time) qualified for 14 of 10289 draws:
  - its mask texture is LINEAR_MIPMAP_LINEAR (tex rejects, 13.5 s)
  - its nine-slices' 6-index groups are not one rectangle each (shape rejects, 11.3 s)
- **Also:**
  - program 5 copies sometimes blend ZERO / ONE_MINUS_SRC_ALPHA (dst-out)
  - program 29 (luminanceCurve over the backdrop) is 14%
- **Next build:**
  - nine-slice triangle pairing
  - mipmapped textures taken when not minified
  - luminanceCurve kind
  - dst-out blend

## 3a1708a run (log 518ec7c1, `SGLFAST=1`, about 21:14)

- **Self-check:** no draw differed, including the new darken kinds (alpha-tint+darken 1486 draws, copy+darken+noise 46).
- **Test window:** 2.44 frames/s. Slow frames averaged 1.9 s (median of all 1.7 s, worst 9.5 s); **27/30 steps** drew a frame (best so far); the first frame after a step took 1.11 s on average.
- **The check budget of 60 was too high for rare full-screen kinds:** the copy+darken shader came only 46 times, so every one was drawn twice (softpipe and CPU, 14 s for 41 draws).
- **The 10 s summary line passed the 512-byte log limit** and was cut.
- **Left in softpipe:**
  - program 13, the masked nine-slice: 16%
  - program 29, luminanceCurve over the backdrop (34 draws, 0.3 s each): 10.5%
  - programs 31 and 51 (colour matrix over the backdrop), 89 (no fast vertex shader), 65 (clamped supercircle mask) and 95 (circleImage tint)
- **Next build:**
  - multi-rect draws (up to 16)
  - the mask kind (`color*s0*s1.a`)
  - check budget 10
  - the summary in two lines (`fast mode` and `fast not`)

## 92e026d run (log bdf90282, `SGLFAST=1`, about 20:55): 2.75 frames/s

- **Self-check:** 519 draws matched, none differed.
- **Test window:** 2.75 frames/s (2.15x the 1.28 before the fast path). Slow frames averaged 2.2 s (median of all 1.6 s, worst 9.7 s); 25/30 steps drew a frame; the first frame after a step took 0.79 s on average.
- **The noise copy now runs on the CPU:** 1212/1212 draws. Its gradient source needed the format copy. Map rejects: 0.
- **Fast path:** 18638 draws, 21.8 s (map 4.6 s, pixels 17.2 s). Rejects: kind 81 s.
- **Left in softpipe:**

  | Program | Shader | Share |
  |---|---|---|
  | 13 | masked nine-slice | 17% |
  | 21 | alpha-tint + darkenSover over the backdrop | 15% |
  | 39 | clamped copy + noise, 29 draws of about 350 ms | 10% |
  | 19 | copy + darkenSover + noise | 9% |

  Then a circleImage tint (program 77) and programs 89 and 29.
- **Next build (commit after 92e026d):** the two darken kinds and clamped variants of every kind.

## 27eb0f3 run (log ec68489c, `SGLFAST=1`, about 20:32): 2.09 frames/s

- **Self-check:** 455 draws matched, none differed.
- **Test window:** 2.09 frames/s (1.28 before the fast path, 1.60 with a09c1a7). Slow frames averaged 2.7 s; 24/30 steps drew a frame.
- **Turned textures fixed every shape reject:** 0, was 41 s.
- **Fast path:** 21248 draws, 28.2 s (map/flush 11.9 s, pixels 16.3 s).
- **Rejects:** kind 75.8 s, **map 39.8 s**.
- **The map rejects are the noise copy** (program 17 in this boot, 29% of the time, kind 7). Mapping its 256x1 gradient source failed. The commit after 27eb0f3 logs why and converts other plain formats to an RGBA copy.
- **Next costs:**
  - program 5 (copy, almost all fast): 18%
  - **program 23** (`color*s0*s1.a`, nine-slices of 54 indices): 13%. The next fast-path target: needs several rects per draw, a second texture and a second texcoord.
  - program 13 (alpha-tint with darken over the backdrop): 11%
  - program 11 (copy with darken plus noise over the backdrop): 6%
  - programs 45, 35 and 25

## a09c1a7 run (log 215a7147, `SGLFAST=1`, about 20:10): best so far

- **Self-check:** 453 draws matched, none differed.
- **Atop blending works:** opaque copies with atop now qualify (1291 draws).

  | Run | Frames/s in the test window | Average slow frame | Steps with a frame | Mean first frame |
  |---|---|---|---|---|
  | t40 (no fast path) | 1.28 | 4.6 s | 14/30 | 1.16 s |
  | eed85c5 | 1.49 | 3.2 s | 23/30 | 1.30 s |
  | e0ea1da (edge rule too strict) | 1.35 | 4.0 s | 22/30 | 1.84 s |
  | **a09c1a7** | **1.60** | 3.2 s | **24/30** | **1.11 s** |

- **Fast path:** 15117 draws, 20.5 s (map 5.2 s, pixels 15.3 s). Rejected draws cost 117 s: kind 75 s, shape 41 s.
- **The noise copy (program 9) is now the costliest program: 32%.** It is classified correctly, but all its draws failed the shape check. Its texture is 256x1, a gradient along the window's y axis (u follows y).
  - **Next build:** turned textures (`fast_uv`) and a reason logged for every shape reject.
- **Next costs:** program 5 (copy) 15%; programs 21, 13 (mask nine-slice), 45, 39, 29 and 19 between 3% and 9% each.

## e0ea1da run (log a685589a, `SGLFAST=1` self-checking, 19:46)

- **Self-check:** 365 draws checked, none differed.
- **The whole-pixel edge rule was far too strict:** 81 s of draws rejected as shape. These were scaled and moving layers with edges like x 2.11-831.44.
  - Copies qualified for only 2942 of 6667 draws; the noise copy (program 9, classified correctly as kind 7) and atop qualified for 0.
  - Test window: 1.35 frames/s, slow frames 4.0 s on average, worst 7.3 s. This is not comparable with 4b54b84f because of the rejects.
- **Cause and fix (a09c1a7):** softpipe (`sp_setup.c`, half-pixel centres) covers rows from `ceil(y - 0.5)` but columns from `(int)(x + 0.5)`, each up to the far edge's value, exclusive. That differs from `ceil(x - 0.5)` only when an edge is exactly on a half pixel, which was the sliver (right edge 739.5). The fast path now uses softpipe's column rule, without the whole-pixel restriction.

## Speed run (log 4b54b84f, eed85c5, `SGLFAST=1`, about 19:05): first real gain

- **Frame rate in the test window:** 1.49 frames/s, against 1.28-1.30 in all earlier runs (+15%).
- **Slow frames in the test window:** 29, totalling 92.6 s, so 3.2 s on average (4.6 s before). The worst frame was 10.7 s (earlier runs: 14-25 s).
- **Responsiveness:** in test round 3, 9 of 10 steps produced a frame (earlier runs: 1-3).
- **Fast path:** 13612 draws in 14 s (map/flush 3.0 s, pixels 10.8 s), about 1 ms a draw.
- **softpipe for the rest:** 124 s (kind 111 s, blend 11 s).
- **Next build (commit after eed85c5):**
  - copy + noise and tint + noise
  - atop blending
  - whole-pixel edges only
  - clamped blends

## Verify run (log d6cfddf3, eed85c5, `SGLFAST=2`, 18:23)

- **Verification:** 383 draws match and 4 do not. All 4 are the same draw: alpha-tint, premultiplied blend, rect x 737-739, y 15-24, a 2-pixel sliver.
  - softpipe colours the column just past the right edge (x 739) and the fast path doesn't
  - so the two use different coverage rules for **non-integer rectangle edges**
  - fix: fast-path only rects with integer edges
- **Qualifying:** 20137 of 46873 draws (43%, was 27%): fill 6248, copy 10640, tint 2067, alpha-tint 277, alpha 903, opaque 2. They took 84.1 s of softpipe time; draws rejected as kind took 104.7 s.
- **No straight or mixed blends seen.** Program 15's blend is **DST_ALPHA / ONE_MINUS_SRC_ALPHA** (`0x304` = GL_DST_ALPHA): source atop destination. Earlier notes misread it as SRC_ALPHA.
- **Time by program:**

  | Program | Shader | Share | Qualifying draws |
  |---|---|---|---|
  | 5 | copy | 27% | 10638 / 10728 |
  | 9 | copy + noise | 18% | 0 |
  | 13 | `color*s0*s1.a`: a second texture's alpha as a mask, 8-9 rects a draw | 13% | 0 |
  | 3 | fill | 12% | 6247 / 6413 |
  | 7 | tint | 8% | 2015 / 2026 |
  | 15 | opaque copy, atop | 5% | 0 |

  Then a few huge draws: program 45 (no recognised shaders, 2 draws, 5.7 s), 19 (darken over backdrop), 39 (clamped copy + noise) and 51 (colour matrix over backdrop).
- **Program list** (`fast link` lines): the noise variants (copy, tint, clamped copy, mask), the backdrop blends (darken, plus-darker `R=S+D...`, mix, luminance curve, colour matrix, vibrantDark), `supercircleImage` and `circleImage` shapes, and `sgl_vcm`, `sgl_nop` and `sgl_vblur`.

## Control Center rendering (video 17:38, seen in every run)

- **What happens:** Control Center opens about 12 s after the swipe-up. Its background is missing, normally an opaque dark or blurred layer (Reduce Transparency is on). The home screen shows through, undimmed and unblurred, and the modules look translucent over the icons.
- **It stays open to the end of the test.** The final Home press probably arrived while it was still animating in.
- **Not caused by the fast path:** it looked the same in earlier runs.
- **Lead:** in the shader-dump boot (log 8e3714c1), CA's `create_shader` hook logged **texture function 0x3d MISSING**. CA drops draws whose texture function its GL renderer lacks; that is how 0x2d, 0x14, 0x26 and 0x27 were found and supplied (`sgl_texfns`). 0x3d is not identified yet.
- **Next:** log the caller address for MISSING functions and symbolicate it with ipsw. The hooks currently install only with `SWEETGL_TRACE=1`.
- **Also seen:** opening the Phone app showed a white screen for about 20 s.

## Next steps

1. **eed85c5:** built and checked (16275584 bytes, signature valid). Install with `install-sweetgl-diag-eed85c5.sh` (`OLD=b1059f33…`, `NEW=141eb8a7…`, field-level catalog check, no hash refresh).
2. **Verify run:** boot `wp210-boot-fast2.img`. This is the first check of fill, tint, alpha-tint and the straight and mixed blends against softpipe, and of the threaded loop.
3. **Speed run:** only if verification is clean, boot `wp210-boot-fast1.img`. Compare against the bb529ae and 14f179d runs:
   - frames per second inside the automated-test window (1.25-1.30 so far)
   - map vs pixel-loop time
   - the program table
4. **Then:**
   - identify the expensive unknown programs from the `fast link` lines (program 9: darken with backdrop read plus noise; programs 47, 39 and 51: few, huge draws)
   - the noise-dither variants of copy and tint
   - draws of several rectangles at once
   - the ARGB-swizzled textures
5. **Control Center background:** log where CA asks for the missing texture function 0x3d, then symbolicate the caller with ipsw.
6. **Lower priority:**
   - the 0x502 setup errors
   - the launchd and mediaserverd spin (about 70% and 47% CPU all the time)
   - the A55 MHz reading
   - llvmpipe or a multi-threaded rasterizer as the long-term renderer (the 17.2 MB installed size limit applies)

## Rules followed

- **No blind writes:** nothing is written to the phone before the `--check` output has been reviewed and the write confirmed.
- **No bypassing the metadata check:** its hashes are refreshed only after the changed blocks are identified (and node 19204 is confirmed unchanged).
- **Keep backups** until a new install is verified working.
- **Commit after cause:** shim behaviour changes only once a cause is known. The fast path is behind `SGLFAST`, and the default is count-only.
- **Verify before speed:** from e0ea1da, `SGLFAST=1` checks the first 60 draws of each kind and blend against softpipe before drawing them itself, and hands back any kind and blend that differs. One fast1 boot gives both the check and the speed numbers; `SGLFAST=2` (verify only) is no longer needed.

## Files in this branch

- `notes/scripts/`:
  - install scripts for BGRA, 0x502, 22ec055, d17061a, fedeca3, 41be079, 75b34d3, bb529ae and 14f179d (each with the hashes it was last run with), and eed85c5 (field-level catalog check)
  - `check-metadata-readonly.sh` (latest version: the same 9 hashes as the 14f179d install)
- `notes/tools/`:
  - `pull-sweetlog.bat`
  - `resolve-callers.sh`
  - `prof.py`
  - `fptest/` (host test and benchmark for `fastpath.c`)
  - `catalog-check/` (the field-level catalog check and its test)
- `notes/bootargs/`:
  - `args-freq.txt`
  - `args-fast2.txt`
  - `args-fast1.txt`

# History (earlier notes)

### Where things are

- `main`: unchanged shim commit 1465988 (BGRA upload for format-0 4bpp IOSurfaces).
- `sweetgl-diag-0x502` (pushed): diagnostic branch on top of 1465988.
  - 37e8346 diagnostic + flush timing (too large to install)
  - 71ca6c4 trimmed flush timing and log strings (too large)
  - c3cc1e7 validation-only diagnostic (too large by 64 bytes)
  - 471a76c validation-only diagnostic, no validate-status query (too large by 32 bytes)
  - 22ec055 old window-trace code removed; built and installed on the phone (latest)
- Build artifacts come from the `sweetgl-sw` workflow (run by hand on the branch).

### Build artifact sizes (sweetgl.dylib)

| Commit | Size (bytes) | SHA-256 (prefix) | Fits 17208704? |
|---|---|---|---|
| 37e8346 | 17208896 | 74b69df0… | no (+192) |
| 71ca6c4 | 17208768 | 60c6886c… | no (+64) |
| c3cc1e7 | 17208736 | 71205f7a… | no (+32) |
| 471a76c | 17208736 | 08df3a47… | no (+32) |
| 22ec055 | 17208160 | (pad 552d50d6…) | yes (-544) |

The installed file's catalog size is 17208704 bytes. The build is ad-hoc
signed and its signature grows 32 bytes per 4 KiB page, so small trims often
don't change the size.

### Install status (2026-11-14 00:51)

- Build 22ec055 installed on the iOS volume with `install-sweetgl-diag-22ec055.sh`.
  - Payload: `sweetgl-diag-22ec055-allocation.bin`, 17211392 bytes, SHA-256 `552d50d6…`.
  - `--check` passed, then `--install` reported `INSTALL VERIFIED`. No reboot performed.
  - The phone's current shim (before the install) was the BGRA build, SHA-256 `309a61d7…`. It's backed up in `before-bgra.bin` on the PC (hash verified) and on the phone.
- Catalog metadata expected hashes were refreshed for the four blocks changed by the iOS boot (see below). Those four current values were cross-checked against the phone's reads and the catalog dump.
- Next: reboot to iOS, run the usual test, then pull the sweetgl log and video.

### Phone state

- Trace boot image (`wp210-boot-trace.img`) flashed to boot_a; set_active a.
  The phone reaches the iOS home screen.
- Shim on the iOS volume: build 22ec055 (installed 2026-11-14 00:51), replacing the BGRA build.
- Backups: original pre-BGRA shim `2db9d1eb…` (PC copy in `C:\Users\artzi123\Downloads`);
  BGRA shim `309a61d7…` (`before-bgra.bin` on the PC and on the phone).

### Trace results (log a9b74635, video 5d43f78e)

- Surface copy errors (`0x80e1`) are gone: all 28 texImageIOSurface/attachImage lines report err 0.
- No upload goes to texture 0.
- Three `GL_INVALID_OPERATION` (0x502) errors remain, all after fbfetch draws
  (programs 19, 21 and 27) into framebuffer 2 / texture 3. Cause not yet known.
- Video: 96.7 s, 60 fps. Motion onsets at about 2.1, 9.5, 16.2, 28.5, 40.4, 68.5, 84 s.
  The sweetlog has no timestamps that match these, so no swipe latency yet.

### Install check: blocked on catalog metadata

The install script (`scripts/install-sweetgl-diag-0x502.sh`) refuses to write
unless the nine catalog blocks match the hashes it expects. They no longer do.
The changes come from the iOS boot in the trace image, not from our install.

- Device layout: iOS volume starts at device block 119713536 (4 KiB units) on /dev/block/sdc.
- Volume header (block 119713536): lastMountedVersion HFSJ; modifyDate
  1970-01-01 00:02:27 UTC, about 2 minutes after the trace boot's clock started.
- Catalog file extents (volume blocks → device blocks):
  - 183682 × 16384 → 119897218
  - 211717 × 8192 → 119925253
  - 1133760 × 8192 → 120847296
  - 1729132 × 8192 → 121442668
- Changed blocks (all inside catalog extent 1): 119713536, 119897218, 119897220 (6 blocks), 119897768.
- Unchanged blocks: 121450324, 121450322, 121448304, 121448452, 121448308.
- The catalog record for `/usr/lib/sweetgl.dylib` (file ID 318814, parent folder `lib` ID 313920):
  - logical size 17208704
  - one extent: device block 122729397 × 4202 blocks
  - this matches the install script's target. The file hasn't moved; only its size limit matters.
- Current SHA-256 of the four changed blocks (after the trace boot):
  - 119713536: 0c8892a2…
  - 119897218: 9493e6ef…
  - 119897220: 6b9dfaed…
  - 119897768: 5bb364e7…
- The expected (old) hashes are in `scripts/install-sweetgl-diag-0x502.sh`.
  Replacing them is a decision to make only after the size problem is solved.

### Open items

1. Reboot to iOS with build 22ec055 installed; confirm the phone reaches the home screen.
2. Check the new sweetgl log: `GL error 0x502` lines and the `sweetgl: diag` validation messages.
3. Swipe latency still needs a sync marker. The shim has no touch timestamps yet.
4. Three remaining 0x502 draw errors (fbfetch); the diagnostic in 22ec055 should show the cause.

### Rules followed so far

- No writes to the phone without `--check` output reviewed first.
- The metadata check is not bypassed by editing its hashes.
- Keep backups (PC and `/data/local/tmp/sweetgl-check.*`) until a new install is verified working.
- The install script copy in `scripts/install-sweetgl-diag-22ec055.sh` is the one that ran (SHA-256 `e0c8df12…`).

### Update 2026-11-14 09:45

- Log cfa49be2 (first boot with 22ec055): no copy errors (91 upload/attach lines, all err 0).
  Three 0x502 errors, each on the FIRST fbfetch draw of programs 19, 21, 27 (fb 1 / texture 1);
  draws 2-9 of the same programs have no error. glValidateProgram log is empty for all three.
  User reports the UI is even slower with tracing on; video testing dropped for now.
- Build d17061a (fbfetch trace limit 12 -> 200 per program): 17208160 bytes, fits.
  Allocation SHA-256 08cc1bf0…; installed with `install-sweetgl-diag-d17061a.sh` (bbccaa85…): INSTALL VERIFIED.
  Replaced 22ec055 (552d50d6…).
- After the 22ec055 boot, five catalog blocks changed: the usual four plus 121448308 (catalog node 19204,
  which holds the sweetgl.dylib record). Diffing that node against the earlier dump: only /usr/lib/dyld's
  access date changed. sweetgl.dylib record unchanged (17208704 bytes, 122729397 x 4202).
- Every iOS boot changes the catalog blocks, so the check needs refreshing each time. Idea: make the
  script verify the shim's catalog record directly instead of whole-block hashes.