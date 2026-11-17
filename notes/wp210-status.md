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
| `sweetgl-diag-0x502` | af43d32 | shim work on top of 1465988 (table below); softpipe builds with workflow `sweetgl-sw` |
| `llvmpipe-test` | c5543ad | llvmpipe testing, made 2026-11-16 from `sweetgl-diag-0x502`: build with workflow `sweetgl-llvmpipe` (LLVM + Mesa with llvmpipe); llvmpipe-only shim changes go here, softpipe work stays on `sweetgl-diag-0x502` |
| `wp210-auto-input` | 6c51db1 | kernel patches 0204-0207, `mkadt.py --bigfirst` |
| `wp210-a55-step` | f370f40 | 0208 cpufreq step; 0209 once-a-second clocks, domain 1 re-requested when halved; 0210: no more domain 0 re-requests, the DSI waited for (DSI_INTSTA bit 31) before writing the panel buffer and before START, planar surface layouts logged (`SWEET PANEL:` / `SWEET SURF: id N fmt … plane …` lines); 0211: frames started on the panel's TE when found (`SWEET TE:`), wallpaper image bytes logged (`SWEET PIX:`); 0212: large images sampled once a second while booting; 0213: their bytes at lookup and free; 0214: wallpaper file opens (`SWEET OPEN:`); 0215: without poster descriptors; 0216: posters' RuntimeSnapshot .atx hidden from readers (`sweetsnap=0` keeps them); 0217: 0216's use-after-free fixed; 0218: launchd/mediaserverd syscall counts (`SWEET SYS:`), black-rectangle scan of presented frames (`SWEET BLACK:`); 0219: launchd's received messages, mediaserverd's port flags; 0220: `sweetjit=NAME` (code-signing enforcement off for NAME, for llvmpipe); 0221: the presenter copies the swapped surface before the slow panel copy, swaps wait for that copy (`SWEET COPY:`, `sweetfb_stage=0` = off), first messages between launchd and mediaserverd with body strings (`SWEET MSG:`); 0222: `sweetdeny` (Bluetooth audio HAL plug-in not loaded); 0223: 4 spare rows after kernel-allocated surfaces (`sweetsurfpad`) for llvmpipe; 0224: the same for the display's own buffer; 0225: profiler samples by CPU and thread; 0226: CPU PLLs and clock dividers (`SWEET CLKTREE`, `wp210lldiv`); 0227: A55 divider written back by default (A55s at 2 GHz); 0228: clear bit 12 whenever set |
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
| 0e13161 | self-check: 3 draws covering 1M pixels is enough; nine-slices checked as a whole; CPU time by kind | 16310160 | `f9d8ab6d…` | yes (about 08:55) |
| 47ab084 | threads from 4096 pixels; draw time by CPU | 16310432 | `a313176c…` | yes (about 09:16) |
| e38a725 | spot checks the whole run (1 in 50 draws, 1 in 400 over 256K px); program in mismatch lines | 16310544 | `6eda66af…` | yes (about 10:40) |
| 2e46631 | IOSurfaces written into immutable (glTexStorage) textures instead of failing with 0x502 | 16310592 | `4d5a9d63…` | yes (about 11:12, 2026-11-15) |
| af43d32 | `SGLFRAMES=A-B` frame listings (draws, clears, scissors, IOSurfaces); llvmpipe code is not in this softpipe build | 16310928 | `ec73133a…` | **script ready** (`install-sweetgl-diag-af43d32.sh`, OLD `4d5a9d63…`; simulated: check, install verified, refuses a second time) |

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

## llvmpipe (started 2026-11-16): plan and state

Goal: compare softpipe + fast path (today) with llvmpipe (shaders compiled to AArch64 by LLVM, rasterization on all CPUs).

- **Feasibility checked:**
  - *Space:* the llvmpipe dylib (LLVM inside) will be far over sweetgl.dylib's 17 MB. It goes into **`/usr/standalone/update/ramdisk/arm64SURamDisk.dmg`** (software update ramdisk, never used here): file ID 315534, catalog node 19291, owner 0:0 mode 0644, not compressed, data fork 113246235 bytes = 27649 blocks in **one extent at volume block 2164422 = device block 121877958** (catalog dump of 2026-11-14). Same in-place write as the shim, loaded by `DYLD_INSERT_LIBRARIES` from that path. Other big files seen: PingFang.ttc 74 MB, AppleColorEmoji-160px.ttc 130 MB (in use).
  - *Trust:* ad-hoc signatures pass the built-in AMFI (0025/0035), as the shim's does.
  - *JIT:* `MAP_JIT` is allowed (the built-in AMFI has no map_anon hook). Executing unsigned anonymous pages is decided by the map's code-signing enforcement (vm_fault: no violation without it). `cs_enforcement_disable=1` is already in the boot args; kernel patch **0220** (`sweetjit=NAME`) turns enforcement off for NAME explicitly and logs `SWEET JIT:`.
- **Build:** workflow `sweetgl-llvmpipe` (branch `sweetgl-diag-0x502`, af43d32): LLVM 19.1.7 for iOS arm64 (AArch64 only, static, no RTTI), built once and cached; host `llvm-config` of the same configuration in the iOS prefix behind a wrapper; Mesa with softpipe and llvmpipe; `build-llvmpipe.sh`. One dylib, both drivers: llvmpipe by default, `SGLDRIVER=softpipe` for softpipe; the fast path is off under llvmpipe.
- **Boot args:** `notes/bootargs/args-llvmpipe.txt` (597 characters): args-big5 without `sweetprof`, `keepsyms=1` and `SGLFAST=1`, with the ramdisk path and `sweetjit=backboardd`.
- **Runs:** the upload of 2026-11-16 08:38 (`386d9e3d-sweetgl-sw.zip`) was the softpipe workflow again: its dylib is byte-identical to af43d32, no llvmpipe in it. The first `sweetgl-llvmpipe` run: 43818233580 on `llvmpipe-test` (started 07:00 UTC, LLVM cache empty, so LLVM is built first). Kernel 0221: xnu run 43818237078, success.
- **Run 1 failed** (43818233580, log zip 37b80f06): LLVM's host tools and almost all iOS libraries built in ~25 min, then linking an iOS `llvm-tblgen` failed (`ld: unknown options: --gc-sections`; utils/TableGen is always configured and CMake's iOS is not "Darwin" to LLVM). Fix 950232f on `llvmpipe-test`: install only `install-llvm-headers install-llvm-libraries`, plus `LLVM_NO_DEAD_STRIP=ON`. Run 2 (43822232731) installed LLVM's 95 iOS libraries and headers, then failed copying llvm-config into the missing `llvm-ios/bin` (fixed, 0df5959). 894eeac saves the LLVM cache right after the LLVM check, so later failures don't rebuild LLVM. Run 3 (43826028095, 894eeac): **success**, LLVM cached. `sweetgl.dylib` with llvmpipe + softpipe: 51,286,880 bytes (fits the ramdisk allocation, 113,246,235 bytes); artifact `sweetgl-llvmpipe` (zip SHA-256 `7292f67e…`).
- **Checked** (artifact received 10:54): `sweetgl.dylib` SHA-256 `c4d8d4c1…`, 51,286,880 bytes, arm64 iOS 16.0 minimum, ad-hoc signature valid (12,420 pages, 0 bad), llvmpipe and softpipe in it; imports only libSystem / libc++ / frameworks that iOS 16 has (no `pthread_jit_write_protect_np`).
- **Install script** `tools/install/install-sweetgl-llvmpipe-894eeac.sh`: writes into the space of `/usr/standalone/update/ramdisk/arm64SURamDisk.dmg` (device block 121877958 x 27649), `/usr/lib/sweetgl.dylib` untouched. The phone gets the artifact's dylib at `/data/local/tmp/sweetgl-llvmpipe.dylib`; the script pads it with zeros to 113,250,304 bytes (SHA-256 `5bd83463…`). Field-level catalog check: volume header, catalog header (depth 4, root 20212, node 19291 in use), index path 20212 → 275 → 19391 → 19291 by child pointers, and the leaf record (file 315534, mode 0644, size, one extent). The current content is unknown, so `--check` prints its SHA-256 (and keeps a backup) and `--install HASH` writes only if the content still has that hash. Simulated: check, install verified, wrong/missing hash refused, a second run refuses, a changed extent byte stops the check.
- **Boot:** kernel 0221 with `args-llvmpipe.txt` (598 characters; `sweetjit=backboardd`, backboardd inserts the ramdisk path). Softpipe stays one boot image away (`args-frames.txt`).
- Kernel zip `xnu-kernel_19c085.zip` is 0221 (`SWEET COPY`, `SWEET MSG`, `sweetfb_stage` present), SHA-256 `ffb108b6…`.
- **Comparison:** the same scripted test, kernel 0221 in both; baseline = today's shim with `args-big5` minus `sweetprof`. Compare `SWEET PERF` swaps/s, slow frames (count, mean, worst), the `WP210 AUTO` step times (first_ms, last_ms; Phone launch), CPU per core (`SWEET TOP`), and the video.
- **Still to do:** install script for the ramdisk allocation (field-level catalog check on node 19291, backup of the 108 MB region first), once the dylib exists.

## Shader-cache shim: install script (2026-11-17 07:45)

- **First boot with it (log 2fbde5ff; the user rebooted twice by accident, the log holds only the last boot):** `sweetgl: shader cache in /private/var/mobile/Library/Caches/sweetgl/run43962007014 (uid 501)` but `disk shader cache off`. Mesa's `disk_cache_create` returns a cache even when the directory fails, so the cache was never created: `lp_disk_cache_create` returns early when `disk_cache_get_function_identifier` (dladdr + stat of the dylib, for its mtime) fails for `lp_disk_cache_create` or `LLVMLinkInMCJIT`. Frames as before (733; rounds 2-3 523), 19 slow frames, 20.1 s, 5 over 1 s. A55 fix at 41 s, cpu0 1999 to the end.
- **Shim c5543ad (run 44027538880, macos-15-intel, success):** build-llvmpipe.sh patches Mesa's `lp_screen.c` so that the cache id falls back to `SWEETGL_CACHE_ID` (`sweetgl-<build>`, from the cross file's c_args) instead of giving up, and sets `sweetgl_lp_cache_id_fallback`; sweetgl.c logs the cache state, where the id came from, and dladdr/stat (errno, mtime) and uid/gid, to see which part failed.

- Artifact `sweetgl-llvmpipe_1.zip` (run 43962007014, b6214f5): `sweetgl.dylib` 51,323,952 bytes, SHA-256 `ec59bc2a…`, signature valid (12,429 pages), contains `run43962007014` and the cache strings; new imports only libz (deflate/inflate/crc32) and libSystem file calls (flock, rename, fstatat, rmdir, utime, ...).
- `tools/install/install-sweetgl-llvmpipe-b6214f5.sh`: same ramdisk space; current content must be the 894eeac payload (OLD `5bd83463…`), NEW `26f658c5…` (dylib padded to 27,649 blocks); field-level catalog checks as before; the dylib goes to `/data/local/tmp/sweetgl-llvmpipe-cache.dylib`. Simulated: check, install verified, a second run refuses.
- Boot: kernel 0228 + `args-default.txt`, twice: the first boot fills the cache, the second should have no compile stalls.

## 8-core layout with the A55s at 2 GHz (prepared 2026-11-17 07:15)

- **Result (log 03157eac, video 07:36:23, kernel 0228 + `args-8core.txt`): 8 cores lose.** The first sample (19 s) already had bit 12 set (0x030012ff, MCUPM's init ran early with 8 cores); 0228 cleared it at 19 s and **all four A55s ran at 1999 MHz from 21 s**, A78s 3000, prime 3100, to the end. But frames: all steps 790 → **685**, rounds 2-3 524 → 492 (-6%); compile stalls 25.3 → 29.0 s. `PROF`: CA's render thread on the A55s 35% of its samples (16% with 5 cores), llvmpipe's 8 threads 37% on A55s. The A55s add throughput to the rasterizer but the single-threaded work lands on them too often. **Stay on 5 cores** (`args-default.txt`); 0228 is fine for both.

- Idea: with the A55s at 2 GHz, 8 cores (4x A55 2 GHz + 3x A78 3 GHz + prime 3.1 GHz) may beat 5; llvmpipe's rasterizer gets 8 threads. Risk: single-threaded work (CA's render thread, LLVM compiles) lands on an A55 more often (4 of 8 cores instead of 1 of 5); an A55 at 2 GHz does much less per clock than an A78.
- **Kernel 0228 (f370f40, xnu run 44022472551):** the A55 fix clears bit 12 whenever it is set, instead of restoring the first sample: with 8 cores online MCUPM's init (which waits for 8 cores ready) runs before our first sample at ~19 s.
- **Boot args `notes/bootargs/args-8core.txt`** = `args-default.txt` with `cpus=8` (598 characters), same big-first ADT (logical 0 = boot A55, 1-3 A78, 4 prime, 5-7 A55s).
- Compare with the 0227 run (log cab9e7a4): frames per step, slow frames, `PROF` by CPU/thread (where CA's render thread runs), clocks of all 8.

## The A55 halving: fixed (log cff94a30, kernel 0226 + `args-lldiv.txt`)

- At 41 s MCUPM's init set bit 12 (all three words 0x030012ff), cpu0 999, cpu1-3 1499. Kernel 0226 wrote 0x030002ff back to 0x0c53a2a0: it read 0x030102ff (bit 16 set for a moment), wrote again at 42 s, then 0x030002ff stayed. **cpu0 1999 MHz from 42 s to the end of the log (98 s)**, cpu1-3 3000 (domain 1 re-request as before), prime 3100. No panic; crashes the same as in earlier runs (MusicWidgets 8, assetsd 8, driverkitd 4). The log ended before the scripted test (90 s), so no frame numbers yet.
- **Kernel 0227 (473007e, xnu run 44019151975):** the write-back is on by default (`wp210lldiv=0` turns it off), and bit 16 is ignored in the compare.
- **Kernel 0227 run (log cab9e7a4, video 07:06:39, `args-default.txt`):** one write-back at 41 s, then cpu0 1999-2000 MHz to the end (210 s), cpu1-3 3000, prime 3100. No panic, no glitch in the video. Against the 3 GHz run with the A55 at 999 (log 16ff718b): all steps 701 → **790 frames (+13%)**, rounds 2-3 480 → 524 (+9%); swipe back 61-62 → 61-66, home after Spotlight 29-32 → 32-47, home after Phone 31-44 → 43-44; shader-compile stalls 30.4 s → 25.3 s (23 slow frames, 8 over 1 s, as before). Presenter copies 6.3 / 5.2 ms.
- Next: the scripted test with 0227 + `args-default.txt`, frames against 0226/3 GHz; then maybe the 8-core layout again, now that the four A55s would run at 2 GHz (llvmpipe could use 8 threads).

## The A55 halving, again (2026-11-16 night): the clock tree

- **Kernel 0226 run (log 10298a6d, `args-default.txt`):** the PLLs don't move. ARMPLL_LL stays at 1999 MHz (CON1 0x80133b13) through the drop; BL 2999, B 3099, CCI 1599 from 22 s. The only word that changes: at 40 s the three "divider" words (0x0c53a2a0/a4/a8) go from **0x030002ff to 0x030012ff** (bit 12 set), the same second cpu0 reads 999 and cpu1-3 1500 MHz. CCI's (0x0c53a2e0) stays 0x83010aff. The three words always read the same, so they may be one register; bits 21:17 (the assumed ckdiv1) are 0, so the layout isn't the one assumed. Domain 1 came back to 3000 MHz the next second (re-request) with the bit still set, and the prime (B) never halved, so bit 12 is not simply "divide this cluster by 2", but it is set at the moment of the drop, by something other than our kernel.
- **MCUPM firmware (read-only copies of `mcupm_a/b`, `sspm_a/b`, 1 MiB each, SHA-256 in `notes/firmware/SHA256SUMS`):** MTK image `tinysys-mcupm-RV33_A`, 628,353 bytes, RISC-V 32-bit with compressed instructions (plus MediaTek custom opcodes), FreeRTOS 10.1.0, `project/RV33_A/mt6895/...`; slots a and b differ only in the signature (cert2). Feature strings: EEMSN (voltage aging), PTP3 (`brisket3_control`, `fll_control`, `adcc`), COBRA (power budget per cluster/OPP), mcdi (deep idle), FH (frequency hopping), DVCS, timers `CPUDVFSTimer`, `SNTimer`, `CMTimer`. Disassembled with llvm-objdump (riscv32 +c,+m,+a,+zicsr; raw payload from file offset 0x200, wrapped as ELF).
- **Who sets bit 12:** only two functions use the address 0x0c53a2a0 (`lui 0xc53a` at payload 0x12f36 and 0x12f58). 0x12f2c writes a 1-bit field at bit 12 of `0x0c53a2a0 + 4*cluster`, 0x12f4e the same at bit 13 (via the field-write helper 0x10e6a). Their only callers are one init routine at 0x12be4: it sets bit 16 of 0x0c53a814, then polls **0x0c53a840 until its low byte is 0xff** (up to 30,000 tries), and **on success or timeout** (both paths are the same) does, for clusters 0, 1 and 2: clear bit 13, set bit 12. Then per cluster 0x12006, 0x116fe, 0x1018c → 0x10210, 0x12f70 (cluster 7 too), 0x10748, 0x10b90 (writes 0x0c532xxx), and finally a ready flag. So the drop is MCUPM's CPU DVFS init, run once. A low byte of 0xff looks like 8 per-core bits: with 8 cores online the init runs early (A55s already halved at 18 s), with cpus=5 it waits for the timeout (~40 s). That explains the different timing of the two layouts.
- **Next:** `notes/bootargs/args-lldiv.txt` (= args-default minus `sailfishwlan=0` and `sweetgpu=0`, plus `wp210lldiv=1`, 584 characters): kernel 0226 writes 0x030002ff back to 0x0c53a2a0 when it changes, logs what it reads after, at most 20 times. If cpu0 returns to ~2000 MHz, bit 12 is the switch; if the bit comes back, the log shows who fights. The PLL and the cpufreq index stay at 2000 MHz throughout, so the voltage should already be the 2 GHz one.

- **The event:** at 41 s in every recent run (`SWEET MHZ: 41 s 999/1499/1499/1499/3095` at 3 GHz; `999/1175/1175/1175/2392` at 2.4 GHz), logical cpu0-3 drop to exactly half, cpu4 (prime) not; the 10 s watch's re-request brings domain 1 back the next second, domain 0 never. In those seconds the log shows backboardd's first swaps (11-16); in the 8-core runs the A55s were already halved at the 18 s probe.
- **Exactly half suggests a divider, not the PLL.** The phone's DT (`cpuhvfs@00114400`, "mediatek,cpufreq-hybrid") names the clock registers: `apmixedsys = mcusys_pll1u_top@1000c000`, `pll-con = <0x20c 0x21c 0x22c 0x23c>` (ARMPLL_LL = A55s, ARMPLL_BL = A78s, ARMPLL_B = prime, CCIPLL), `clk-div-base = mcusys_ao_cfg@0c530000`, `clk-div = <0xa2a0 0xa2a4 0xa2a8 0xa2e0>`. fhctl's map0 says these PLLs hop via `fhctl-mcupm` (the MCUPM firmware drives them).
- **Kernel 0226 (786f62e, xnu run 43959255883):** `SWEET CLKTREE:` each PLL's CON0/CON1 (MHz from PCW and posdiv) and each divider (ckdiv1 = bits 21:17; 8 = /1, 10 = /2, 11 = /4), printed when any word changes, next to `SWEET MHZ`. `wp210lldiv=1` (off by default) writes the A55 divider back when it leaves its first value; for a second boot, only after the first shows which word moves.
- **Shim build done:** run 43962007014 (macos-15-intel) succeeded at 22:44 UTC: shader cache enabled, zlib 1.2.12, `sweetgl.dylib` 51,323,952 bytes (fits the ramdisk space); artifact `sweetgl-llvmpipe` (digest `a4876126…`).
- **Shim build:** arm64 Mac runners unavailable (3 runs cancelled, "capacity constraints"); b6214f5 adds a `runner` input, run 43962007014 on `macos-15-intel` (LLVM built again for an Intel host, own cache key; host LLVM with `LLVM_TARGET_ARCH=AArch64`).

## Shader cache, llvmpipe as default (2026-11-16 evening)

- **Shim 27de1ce + cd22e90 on `llvmpipe-test`:** Mesa built with `-Dshader-cache=enabled -Dzlib=enabled` (the cache needs compression). Mesa keys llvmpipe's cache on the dylib's mtime, which a raw install never changes, so `sweetgl_cache_setup()` gives each build its own directory, `SWEETGL_BUILD_ID` = `run<GITHUB_RUN_ID>`: `/private/var/mobile/Library/Caches/sweetgl/<build>`, else `/private/var/tmp/sweetgl/<build>`. `MESA_SHADER_CACHE_DIR` in the environment wins, `SGLCACHE=0` = off. Log lines: `sweetgl: shader cache in …`, `sweetgl: build …, disk shader cache on/off`.
- cd22e90: `-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0` for the cross build (run 43949620268 stopped in `program_lex.yy.c`: u_endian.h's `<endian.h>` branch found no byte order).
- Run 43951940034: never started ("not acquired by Runner ... capacity"). Retried as 43955146631.
- **Boot args:** `notes/bootargs/args-default.txt` = llvmpipe at 3100 MHz (= `args-llvmpipe-3g.txt`, 598 characters); `args-softpipe.txt` = the softpipe fallback, `/usr/lib/sweetgl.dylib` + `SGLFAST=1`, 3100 MHz, no SGLFRAMES (572).
- Test: boot the same image twice; the first boot fills the cache (stalls as before), the second should have none.

## llvmpipe, 3 GHz, 4 threads, kernel 0225 profiler (log 3cb6b869)

- **4 threads is worse:** rounds 2-3 480 → 445 frames (swipe back 61-62 → 55-62, home after Spotlight 29-32 → 14-25). Keep the default 5 (`args-llvmpipe-3g.txt`).
- **Where backboardd's CPU goes** (0225 per-thread samples, all rounds): llvmpipe's rasterizer threads `llvmpipe-0..3` **90.8%**, CA's render thread `com.apple.coreanimation.display.primary` 5.5%, unnamed 3%, `render-server` 0.6%.
- **The slow core is not the bottleneck:** 15% of the rasterizer samples and 16% of CA's land on cpu0 (the 999 MHz A55), less than its 1-in-5 share. Busy rounds: cpu0 1353-1769, cpu1-4 2000-4600 each.
- **Inside those samples** (shim symbolized: load address 0x107aa8000, found from the `thread_function`/`impl_thrd_routine`/`rasterize_scene` return addresses): self time JIT shader code 57%, kernel 33%, shared cache 7%, shim C code 4%. Of the kernel part, 83% is one pc, `ml_set_interrupts_enabled_with_debug` called from `Call_continuation` (kernel 0225 = 0221's symbols + 0x1000 here): a timer tick that came while the CPU was idle or switching, counted when the woken thread enables interrupts. Mostly idle time charged to backboardd, not real work. The rest (~6% of all): psynch condition variables (llvmpipe's thread hand-offs), `gate_handoff` 10% of kernel.
- **So:** frames cost pixel shading in llvmpipe's JIT code. More clock helps only in proportion. Next: which CA passes cost most (per-framebuffer timing in the shim with llvmpipe), Mesa's shader cache (the multi-second first-use stalls), llvmpipe's linear path for plain textured quads.

## llvmpipe at 3 GHz (log 16ff718b, video 21:40:53, kernel 0224, `args-llvmpipe-3g.txt`)

- **Clocks held:** domain 1 index 0 (3000 MHz), domain 2 index 0 (3100); `SWEET MHZ` 2985-3015 / 3088-3112 for the whole run, state 0/0/0, never rewritten, limit words 0, cap words unchanged. The A55 still 999 after ~40 s.
- **Gain smaller than the clock (+28%):** rounds 2-3 463 → 480 frames (+4%); swipe back 56-57 → 61-62 (+9%); Spotlight 16-19 → 18-27; whole run 616 → 701 (round 1 depends on compile timing). Compile stalls unchanged (23 slow frames, 30.4 s against 30.1 s), although LLVM compiles on one thread and should be 28% faster on an A78. Suspect: backboardd's render thread (CA + compile + llvmpipe setup) often on cpu0, the 999 MHz A55; or frames paced on the 60 Hz grid.
- Presenter copies 3.5 / 4.2 ms.
- **Next (kernel 0225, 8325306, xnu run 43942677067):** each `PROF` round also prints samples by CPU and, for the busiest 10 backboardd threads, total and by CPU with the thread name. Boot args `args-llvmpipe-3g-t4.txt` (589 characters): 3 GHz, `LP_NUM_THREADS=4` (llvmpipe's rasterizer threads off the A55), `sailfishwlan=0` and `sweetgpu=0` dropped (both default off) to make room.

## llvmpipe works: kernel 0224 run (log 4db271f5, video 21:24:07), compared with softpipe on 0223 (log bfac827b)

- No backboardd crash; the display buffer is now mapped with its spare rows (`physical with managed page 0x1c41a8000:0x3d8000`). Home screen, Spotlight, Phone, Control Centre all draw correctly; no glitch in the video (the only transients are the boot console scrolling).
- **Frames per scripted step** (rounds 2 and 3, ~3 s each; softpipe → llvmpipe): swipe back 23 → 56–57 (~8 → ~19 fps); home after Phone 17–18 → 33–37; home after Spotlight 17–18 → 18–51; Spotlight 4–11 → 16–19; Control Centre 0–1 → 8. All steps over the whole run: 414 → 616 frames.
- **CPU:** idle `SWEET TOP` ~40% → ~20%; during animations backboardd uses about the same CPU (65–140%) for 2–3x the frames. The presenter's copies averaged 3.7 / 4.9 ms (softpipe 0223 run: 10 / 11.5 ms; 0221: 5 / 6 ms), so that spread is not the driver.
- **The cost: compiling shaders.** Each new shader/state combination is compiled by LLVM on first use, inside the frame: 21 slow frames, 8 over 1 s, up to 6.1 s (first Spotlight, first Phone, first Control Centre in round 1). Rounds 2 and 3 are smooth. Mesa was built with `-Dshader-cache=disabled`, so this repeats every boot.
- **CPU first (user's choice, 21:30).** Clocks in this run: A55 (cpu0) 999 MHz, A78s 2340-2360, prime 2380-2396 MHz, held by `wp210cpumhz=2400`. The cpufreq tables go to **3000 MHz** (domain 1, index 0) and **3100 MHz** (domain 2, index 0); domain 0 tops at 2000. Test 1: `notes/bootargs/args-llvmpipe-3g.txt` (= args-llvmpipe with `wp210cpumhz=3100`, 598 characters), kernel 0224, same shim: +28% on the A78s if it holds. The 10 s watch rewrites a domain's state when it differs from the wanted index, so a firmware throttle by state would be overridden: check `SWEET CPUFREQ` limit/cap words and the phone's temperature. Then: llvmpipe threads (`LP_NUM_THREADS=4`, off the 999 MHz A55) and the presenter's copies kept off cpu0.
- **Next:** Mesa's disk shader cache (build with `-Dshader-cache=enabled`, cache directory writable by backboardd, `MESA_SHADER_CACHE_DIR` in sweetenv) so compiles happen once; possibly fewer LLVM optimisation passes for the first compile.

## Kernel 0223 runs (softpipe log bfac827b + video `softpipe.mp4`; llvmpipe log 0a58836c)

- **launchd spin: fixed by 0222.** `SWEET DENY: 2 read opens refused`; launchd's syscalls per 10 s fell from ~246,000 BSD + 98,000 Mach (0221, 30 s) to 429 + 245 (50 s) and lower. Idle `SWEET TOP` went from ~140% (launchd 56%, mediaserverd 51%) to ~40% (kernel_task ~25%, backboardd ~11%); launchd and mediaserverd no longer show.
- **Softpipe:** no glitch in the 192 s video (`transient.py`: nothing). Step times about the same as 0221, some animations a few frames fewer (one run each). The presenter's copies took twice as long as in 0221 (staged ~10 ms, panel ~11.5 ms, against ~5 and ~6 ms): to check, possibly the presenter thread now running on the slow core 0 (999 MHz, memcpy 4.6 GB/s against 9.4) now that the other cores are idle.
- **llvmpipe: same crash.** The overrun buffer is not a kernel-allocated IOSurface but the display's own 750x1334 buffer (`fShadow`, bpr 3008, published by `sweet_surface_create_physical`: "physical with managed page 0x1c41a8000:0x3d4000"), so 0223's spare rows didn't reach it.
- **Fix (0224, 90a38ef, xnu run 43930146252):** `fShadow` gets 4 spare rows and the framebuffer surface maps them, without counting them in its size.

## First llvmpipe boot (log 61934921, kernel 0221, `args-llvmpipe.txt`, video 19:48:06): crashes on the first frame

- Install: `--check` passed; the old content was an `IM4P` (the original update ramdisk, no `koly` trailer), SHA-256 `8a75ac07…`, backup in `/data/local/tmp/sweetgl-lp-check.oOOy5q/before.bin`. Installed.
- `SWEET JIT: backboardd may run code it generates`, then `sweetgl: screen llvmpipe (LLVM 19.1.7, 128 bits)`: LLVM compiles and runs shaders on the phone. The first frame took 398 ms.
- Then backboardd crashed four times the same way (Apple logo, then a dim screen in the video): SIGSEGV at an address 0x640 past the end of an IOKit mapping of 0x3D4000 bytes (a 752x1334 surface, bpr 3008: the fault is row 1335, column 0). Symbolized with the unstripped dylib (load address 0x10737c000 in pid 204): JIT code ← `lp_rast_triangle_32_1` ← `rasterize_scene` ← `thread_function` ← `impl_thrd_routine`. llvmpipe draws in 4x4 blocks and reads whole blocks at the bottom edge (rows 1332–1335 of 1334); the page slack after the surface was 1408 bytes, less than a row.
- **Fix (0223, ddeb2a7, xnu run 43922945507):** kernel-allocated surfaces get 4 spare rows (of the widest plane) mapped with them but not counted in their size; `sweetsurfpad=N` (0 = off). Surfaces that wrap a client's own memory can't be padded; if llvmpipe draws into one of those, the same crash would show there.
- Next: softpipe and llvmpipe both on 0223 (which also has 0222's `sweetdeny`), so the comparison stays fair.

## Kernel 0221 run (log fdc99a88, softpipe af43d32, `args-frames.txt`, video 19:35:47): glitch gone, the spin named

- **Glitch:** none. A new detector (`vidtools/transient.py`: a frame that differs from both neighbours while they agree) finds nothing in the 214 s video; on the 0219 video it finds the 111.72 s glitch (7 blocks). No `SWEET BLACK` lines.
- **Cost of the copy:** the presenter's copy of the surface takes 5.1–5.5 ms on average (max 40 ms), not the ~0.5 ms expected: the surface reads at ~0.8 GB/s against 4.5 GB/s for cached kernel memory, so the kernel's mapping of the surface is probably uncached. The panel copy from the staged copy averages ~6 ms. Swaps waited only 10 times in 210 s (avg 11 ms). Step times and frame counts match the 0219 run; nothing got slower.
- **The launchd spin:** mediaserverd sends `0x40000324` = XPC `lookup-handle` of **`com.apple.BTAudioHALPlugin.xpc`** (keys handle, flags, name, type); launchd answers `0x20000000` with `error 3` (ESRCH: no such service). The Bluetooth audio HAL plug-in (`/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver`, only its executable on disk, no XPCServices) retries at once, ~4,700 times a second.
- **Fix (0222, aede029, xnu run 43920560411):** read opens of anything named in boot-arg `sweetdeny` (default `BTAudioHALPlugin.driver`), or directly inside it, fail with ENOENT, so mediaserverd doesn't load the plug-in. `sweetdeny=-` = off. `SWEET DENY:` every 30 s counts the refusals; the `SWEET SYS` tables show whether launchd calms down.

## Kernel 0219 run (log 92a99cec, shim af43d32, `args-frames.txt`, video 08:28:55): the glitch explained

- **Who sends launchd the messages:** mediaserverd. At 40 s launchd had received id `0x40000324` from mediaserverd[28] 46,977 times in 10 s, and mediaserverd had received `0x20000000` from launchd 47,045 times; mediaserverd made a reply port (flags `0x202b`) 46,976 times. So mediaserverd asks launchd for something ~4,700 times a second and gets an answer it doesn't accept (likely a lookup of a service that isn't there). 0221 logs the body strings to name it.
- **The glitch:** video 111.73 s (60 fps frame 6704), about 120.4 s uptime, at the end of `swipe_right` (round 1). For one video frame the rectangle at x 352–736, y 160–672 (750x1334 coordinates; the Calendar widget body, Photos, Mail, Clock, News) shows only wallpaper, with the bottoms of Clock and News already drawn; the frames before and after show it half drawn.
- That rectangle is exactly the scissor of CA's partial redraws in frames F 761–767 (120.37–120.72 s). Each such frame first draws the wallpaper into the rectangle, then the icons ~10–15 ms later. backboardd alternates surfaces 1 and 2, and a partial frame took only ~25–30 ms.
- **Cause:** the kernel presenter reads the swapped surface up to ~12 ms after the swap (it keeps presents 3/4 of a frame apart), then waits for a busy DSI, then copies to the panel by CPU for several ms. The code assumed the GPU quad path (~2 ms), which the wp210 doesn't use. With frames this short, backboardd was already drawing the frame after next into the same surface while the copy was reading it, so the panel got a half-drawn frame. The earlier black rectangles and thin black lines fit the same cause (a clear or a full-screen pass caught mid-way).
- **Fix (0221):** the presenter first copies the surface into a kernel buffer (4 MB, ~0.5 ms at the measured ~9 GB/s), then works from the copy; a swap of the other surface waits while that copy runs. `sweetfb_stage=0` turns it off. New `SWEET COPY:` line every 30 s: presents, staged copies (avg/max), panel copy avg, swap waits (count, avg, max, timeouts).

## Kernel 0218 run (log 7e696cb0, video 07:31): the launchd/mediaserverd loop; the glitch again

- **launchd** runs one cycle about **4,700 times a second**, idle or not: `kevent_id` x2, `__mac_syscall` x2, `proc_info`, `mach_msg2`, `mach_port_deallocate`, `bsdthread_ctl` — receive a request, check the sender, reply. Plus `sendto`/`lstat64` pairs (logging; at 30 s 1000 `sendto` failed with ENOBUFS).
- **mediaserverd** constructs and destructs a port at the same rate, window by window (46,810 vs 46,851; 47,422 vs 47,666; 50,111 vs 50,319), but sends only ~610 messages a second, ~6% of its receives timing out (`0x10004003`). The two loops are one.
- **Glitch** at video 107.6 s ≈ 119.2 s uptime, again in the scripted swipe from page 2 back to page 1 (r1 s3): a rectangle (right half, from the middle of the Calendar widget to the Clock/News row) shows only wallpaper, icons and widget missing, a thin black line on its top edge, for one frame. No `SWEET BLACK` (mostly wallpaper, not black), no odd swaps.
- **Next:** kernel 0219 (198b359): launchd's received messages by id and sender, mediaserverd's `mach_port_construct` flags and results, in the `SWEET SYS` lines. Shim 5549066: `SGLFRAMES=A-B` lists every frame in that uptime window draw by draw with clears, scissors and IOSurfaces. Boot args `notes/bootargs/args-frames.txt` (args-big5 without `sweetprof`, with `SGLFRAMES=119-121`, 589 characters).

## Kernel 0217 run (log 89cc705b, video 22:57): magenta gone

- **Magenta fixed:** `SWEET OPEN: SpringBoard denied r …RuntimeSnapshot-…atx` twice, no panic; the wallpaper goes black (video 18-20 s) → blue (21 s). The black is the time with no snapshot, until the live poster draws.
- **One black rectangle** at video 102.87 s (≈ 120 s uptime, end of the scripted swipe back to page 1, step r1 s3): a band across the Clock and News icons, dark for one frame, solid black for one, then gone. Not the panel noise (that is gone: strip detector found nothing else); looks like a region of one frame left undrawn. No error in the shim's log. Watch whether it recurs.
- **Next kernel (b92fc4b, patch 0218):** `SWEET SYS:` system-call counts of launchd and mediaserverd every 10 s (boot-arg `sweetsys=NAME,NAME`, `-` = off), and `SWEET BLACK:` when a presented frame holds a black rectangle (runs of 140+ black pixels bounded by non-black on 3+ sampled rows, from 45 s, at most 60).
- **Phone app launch:** tap at 128.08 s, first frame after 81 ms, 5 frames over 1722 ms; frame 833 alone took 963 ms of drawing (150 draws, 7 off-screen framebuffers: the zoom over the blurred home screen). First-round launches since 0210: 1678, 1170, 982, 1265, 975, 1722 ms — the usual range, not a new slowdown. At the same time launchd 60% and mediaserverd 35% (the known spin).

## Kernel 0216 runs (videos 22:30:15 and 22:30:24): panic, and magenta still there

- **Panic (my bug):** first boot reached the home screen, then `Kernel data abort` at pc …74b32a8, lr …7575f24, x0 0xfffffe100d3450d3, x1 a kernel constant. 0216 compared `ni_cnd.cn_nameptr` with "RuntimeSnapshot-", but `vn_open_auth` frees that path buffer (`nameidone`, line 695) before the VNOP_OPEN block. Fixed in **0217 (5c0a12b)**: the name comes from `vnode_getname`.
- **Second boot (log 7a3d258a) ran 0215, not 0216:** its open log has `/Library/Wallpaper/…` paths, which 0216 skips, and no `denied` lines; so the magenta in that video is expected.
- **That log shows the reader:** PosterBoard writes `RuntimeSnapshot-home/lock.atx` at about 35 s; at about 36.5 s **SpringBoard reads `RuntimeSnapshot-lock.atx` and `-home.atx`** (and `RuntimeSnapshotMetadata-home.plist`); the very next frame is the magenta one (prog 35's single full-screen draw, swap 14). 0217 refuses exactly that read.

## Kernel 0215 run (log e9bb670e): the snapshot is rewritten at each boot

- At about 35 s PosterBoard writes (write-only) the active poster's snapshots: `PRBPosterExtensionDataStore/59/Extensions/com.apple.WallpaperKit.CollectionsPoster/configurations/C4DB7640-A7AE-481B-8DD7-10A7FABBF8F0/versions/0/RuntimeSnapshot-home.atx` and `-lock.atx` (plus `RuntimeSnapshotMetadata-*.plist`). The magenta shows about 2 s later (swap 10).
- So the snapshot is made anew each boot. Likely mechanism: an .atx holds ASTC blocks; ASTC decoders give **magenta, the error colour,** for invalid blocks, and an all-zero block is invalid. The poster's conversion images are all zero (0213), so the snapshot is encoded from nothing; the dark specks are the few blocks that decode.
- The 300 lines ran out again before any reader (extension binaries, `/Library/Wallpaper/Collections/Bokeh…` plists; the active poster is the Bokeh collection).
- **Next kernel (7cb354c, patch 0216):** open log for any process, only snapshot / .atx / .ktx / .heic / .cpbitmap paths outside system folders and bundles; **read-only opens of `RuntimeSnapshot-*.atx` fail with ENOENT** (logged `denied`; boot-arg `sweetsnap=0` allows them). Expected: black instead of magenta until the live poster.

## Kernel 0214 run (log 6c8df7e3): open log used up by poster descriptors

- PosterBoard rewrites every poster descriptor in `PRBPosterExtensionDataStore/59` at each boot (Emoji 140, Gradient 120, Aegir 20 opens), which used the 300-line budget before the magenta frame; long paths were cut by the log's line limit.
- Seen otherwise: SpringBoard reads widget snapshots (`Caches/com.apple.chrono/snapshot-cache/…`) and `Caches/MappedImageCache/Persistent/highlight-….cpbitmap`.
- **Next kernel (c8a78bf, patch 0215):** skip paths with `descriptors/` or `.app/`, print the last 200 characters of the path.

## Trace run (log b763735b, 0213 kernel + SWEETGL_TRACE=1): the magenta is a cached snapshot picture

- Just before the magenta frame backboardd uploads **one 750x1334 BGRA picture from memory** (`TexImage2D tex 4 … ifmt 0x1908 750x1334 fmt 0x80e1`, row 750); the frame's single full-screen opaque draw (prog 35) shows it.
- No byte order of a blue pixel gives red = blue with little green, so the picture itself is magenta: a ready-made snapshot of the wallpaper (iOS shows one while the live poster loads; the live poster, the blue gradient, replaces it 2 s later).
- That is why no kernel change touched it and why its dark specks never move. Whether the snapshot is a file from an earlier boot or made anew each boot is open.
- iOS 16 keeps poster data under PosterBoard's container, `Library/Application Support/PRBPosterExtensionDataStore/<n>/Extensions/com.apple.WallpaperKit.CollectionsPoster/…`.
- **Next kernel (a8e9079, patch 0214):** `SWEET OPEN: <proc> <r/w/c> <path>` for files with poster / snapshot / wallpaper / .ktx / .heic / .cpbitmap in the path, opened by PosterBoard, SpringBoard, backboardd or a *Poster* process (up to 300 lines).

## Kernel 0213 run (log 809f7331): the wallpaper app's YUV/BGRA images are never filled

- CollectionsPoster's 1105x1160 420f and BGRA images (and the 512x512 420f ones) are **all zero in every plane when freed**, and no other process ever looks them up. They are scratch images for a hardware conversion this phone does not have; the magenta does not travel through them, and the lock result's plane fields are not involved.
- The magenta frame (frame 6 here) is again one full-screen draw of prog 35, the general opaque-image shader (later used ~100 times per sample), 271 ms: just under the 300 ms big-draw log, so its texture is not named.
- **Next run, no new kernel or shim:** the 0213 kernel with `notes/bootargs/args-trace.txt` (args-big5 without `sweetprof`, plus `SWEETGL_TRACE=1` in sweetenv, 587 characters): the shim traces its first 20000 texture uploads and IOSurface binds (sizes, formats, centre pixel), interleaved with the swaps.

## Kernel 0212 run (log 1698ab27, about 18:20): wallpaper images too short-lived to sample

- Magenta still at startup (user).
- The magenta frame (swap 10, frame 7) is one full-screen opaque copy, prog 35, the plain `vec4(t0.rgb,1)` shader, 253 ms: its texture already holds the magenta bytes. The fast path drew it as softpipe would.
- CollectionsPoster's 1105x1160 420f (id 51) and BGRA (52/54/56) images are created between the 37 s and 38 s samples and gone at 38 s; the once-a-second sampler never saw them. backboardd's 1105x1160 copy (id 64, created after the magenta frame) stayed all zero.
- **Next kernel (patch 0213):** large images' bytes when another process looks one up and when the last user frees it (`SWEET PIX: <proc> looked up / freed id …`).
- Suspect if the BGRA image holds the CbCr bytes: the plane fields of the lock result (0x68 + plane * 0x80, from patch 0046), never checked against IOSurface.framework.

## Kernel 0211 run (log ec3478a8, video 17:49): TE fixes the line strips

- **Image checked:** `wp210-boot-0211.img` (`268374d6…`) holds the 0211 kernel (SWEET TE / SWEET PIX strings) and the 5-core command line.
- **Panel TE found:** DSI mode 0 (command), TXRX 0x1023c (EXT_TE_EN off), INTEN 0. `TE_RDY` (INTSTA bit 2) ticks every 16.47 ms (60.7 Hz); `EXT_TE_RDY` only twice in 100 ms at 20 s and not at all at 60 s, nor with EXT_TE_EN. Frames start on TE_RDY: 1112 waits in 210 s, none hit 20 ms.
- **Line strips: fixed.** Same detector, still home screen: 1 strip in 72 s (33.7 s), against about 13 a minute with 0210 and 15 before.
- **No speed cost:** swaps/s through the test match 0210 sample for sample; slow-frame times within run-to-run noise.
- **Wallpaper:** still black, magenta, blue. CollectionsPoster's 420f and BGRA images never pass the kernel's lock/unlock traps, so 0211 logged only backboardd's 1105x1160 copy (id 63): all zero at its first unlock (the black phase).
- **Next kernel (5872b22, patch 0212):** 10-75 s after boot, once a second, the kernel reads 4 bytes at three points of each plane of every large 420f / BGRA / format-0 image and logs them when they change (`SWEET PIX: <s> s id …`).

## Kernel 0210 run (log 24b1a1a9, video 16:45): strips unchanged, wallpaper layouts right

- **Domain 0 left alone, domain 1 restored at 41 s** on the first try. The domain-0 lines at 231-241 s in this log are the previous boot's, left in the ring (identical in e13c888d); this boot ends at 211 s.
- **Thin noise strips: not fixed by 0210.**
  - Video, all frames, row by row against the neighbours: about 20 strips on the still home screen (35.1, 39.8, 43.8, 48.0, 57.5, 58.0, 59.4, 62.2, 65.3, 67.9, 68.3, 75.8, 77.6, 81.1, 82.0, 88.0, 97.1, 97.3, 100.3, 102.6 s) and some later. About 13 a minute; 15 a minute in the 14:37 video with the same detector.
  - Each is a full-width band a few pixels high of **coloured noise** (not a shifted picture), in exactly one video frame (1/60 s).
  - `SWEET PANEL`: 1321 kicks in 210 s, about 8-10 a second on the still screen (backboardd presents about 10 frames/s); the DSI was busy before a START only 23 times.
  - So garbage written into the panel's memory would last about 100 ms, not one frame. More likely the panel scans rows a transfer is writing at that moment: frames are not started on the panel's TE.
- **Wallpaper (black 23.6 s, magenta 24.96-26.97 s, blue after, in the video):**
  - Matches the log: swap 8 shows the home screen, swap 10 the magenta, and frame 8 took 1997 ms before swap 11 (blue; that frame draws the gradient, prog 15 with a 256x1 texture).
  - The planar layouts are right and as CollectionsPoster asked: 420f 1105x1160, Y bpr 1536 at offset 0, CbCr 553x580 bpr 1536 at 2359296.
  - backboardd never binds a 420f image. CollectionsPoster makes a BGRA 1105x1160 image after each 420f one; backboardd makes a 32-bit 1105x1160 one (id 64).
  - In the video's magenta, **red = blue at every spot** (245/245, 244/245, 246/248) and green falls down the screen. A BGRA image read in the wrong byte order cannot do that; Cb,Cr,Cb,Cr bytes shown as B,G,R,A can.
  - The magenta has fixed dark specks (same places for 2 s): part of the wrongly made image.
- **Test run itself:** no rendering glitches (no black bars, no blue widget, no frame that differs from both neighbours); the script did not drift to the Today view.
- **Next kernel (e528e6a, patch 0211):**
  - panel TE: at 20 s and 60 s log DSI MODE/TXRX/INTEN/INTSTA and count TE_RDY / EXT_TE_RDY (INTSTA bits 2/4) for 100 ms, also with EXT_TE_EN briefly if neither ticks; if one ticks like a refresh, every frame waits for it (at most 20 ms) before START (`sweette=0` turns it off). `SWEET TE:` lines; `SWEET PANEL` adds TE waits.
  - wallpaper: `SWEET PIX:` 4 bytes at three points of every plane, at the first unlock of each 512x512+ 420f / BGRA / format-0 image, to find which image first holds the wrong bytes.

## Kernel 6a61c4f run (log e13c888d, about 15:00): the A55 domain does not respond

- **Kernel 6a61c4f ran:** its lines name the method. Domain 1 came back at 41 s on the first try, as before.
- **Domain 0 (A55) stayed at 999-1000 MHz through 19+ tries,** every 5 s, cycling the next index, the far index (1000 MHz), enable 0x301, and enable off/on.
  - Writing index 13 (1000 MHz) did not even move it.
  - **After the 40 s event the A55 clock does not follow this register at all.** Something else (firmware or another block) owns it.
  - With cpus=5 the A55 is only cpu0 (about 3 s of draw time per run), so this is set aside.
- **Next kernel (56e2dff, patch 0210):**
  - stop re-requesting domain 0
  - wait for the DSI to finish a frame before writing and kicking (the line bands)
  - log the planar (420f) surface layouts (the wallpaper)
  - Written against a reconstructed tree: xnu-8796.141.3 from apple-oss-distributions with patches 0001-0209 applied by git apply (0031 and 0041 need blacktop's sweet_hfs).
- **Video (from the user):** the black, magenta, blue wallpaper at startup is still there, plus minor glitches.
  - The wallpaper comes from CollectionsPoster. It creates '420f' surfaces (1105x1160, 512x512) that the kernel's IOSurface reports as `bpe 4 bpr 4480 planes 2` (4 bytes a pixel for a 4:2:0 YUV surface), plus BGRA 1105x1160 surfaces.
  - backboardd's shim never binds a 420f surface (none fails in the log). So the magenta is most likely made inside CollectionsPoster, from YUV planes laid out differently from what it expects.
  - To check: the kernel's plane offsets and row lengths for 420f.

## Run with video (log 895ce656, about 14:35): still kernel 4367afb; display glitches

- **Kernel:** the log's re-request lines are 4367afb's (no "by <method>", three tries), so 6a61c4f's A55 methods were not in this boot.
  - The A78s came back at 41 s again.
  - No draw differed.
- **Boot wallpaper, video 0:39-0:41:** black, then magenta for about 0.8 s, then the right blue.
  - The wallpaper (CollectionsPoster) is a YUV image: surfaces '420f' (bi-planar 4:2:0, full range) and 'L008'.
  - Magenta is what YUV gives with the chroma bytes all high (about 0xFF); zero chroma would be green.
  - So the first frames show the wallpaper's luma with a chroma plane not yet written, or read from the wrong place. To check in the shim: how the 420f planes are bound (plane offsets) at first use.
- **Thin black lines, video 0:41, 0:49, 1:10:**
  - On a still home screen, one camera frame (1/60 s) shows a band a few rows high, across the whole width. In it the picture is cut and shifted sideways (0:49.05 through the Notes/Reminders/Clock/News row, 0:49.55 through the TV/Podcasts row).
  - Not drawing: the frames are the same before and after, and the fast path checks are clean.
  - It is the path from the frame to the panel. AppleCLCD scales each 750x1334 frame row by row into the panel buffer (0197). backboardd alternates two surfaces with different row lengths (surface 1 bpr 3008, surface 2 bpr 3072). The panel is command mode, kicked once per present (0195, `sweet_fbcon_kick(false)`).
  - A transfer that reads the buffer while rows are being rewritten, or without the panel's tearing-effect sync, can show such a band.
  - The earlier one-frame "lower half black" (e38a725 video 1:49) is probably the same family.
  - To look at: present timing against the transfer (TE), or a second panel buffer.

## Re-request run (log 47c1078b, kernel 4367afb, 5-core big-first, about 12:30): A78s restored

- **At 41 s** the halving came again: cpu0 999, cpu1-3 1174.
- **Domain 1 (A78s):** one re-request (neighbour index, then the wanted one) brought it back to 2349-2364 MHz, and it held to the end of the run.
- **Domain 0 (A55):** stayed at 999 through all three tries.
- **The best run so far:**

  | Measure | This run | Previous 5-core runs |
  |---|---|---|
  | Swap rate | **4.51/s** | 3.36-3.49 |
  | Slow frames, mean | **0.59 s** | 0.70 s |
  | Slow frames, median | **0.46 s** | 0.51-0.57 s |
  | Worst frame | 1.4 s | 1.6-2.4 s |
  | Mean first frame | 0.52 s | |

  29/30 steps drew a frame; no draw differed. Draw time 38.4 s (45 s before).
- **Next kernel:** keeps re-requesting for the whole run, and tries three more ways for domain 0.

## Once-a-second clocks (log 189a8195, kernel f007825, 5-core big-first, about 11:55)

- **18-20 s:** the bootloader clocks (cpu0 1999, domain 1 1400, prime 499), state 0/19/29.
- **22 s:** wp210cpumhz has run: 1998 / 2349 / 2399, state 0/6/6.
- **40 to 41 s:** cpu0 1998 -> **999**, cpu1-3 2349 -> **1174**; the prime stays 2380-2404. The state stays 0/6/6 to the end.
- **The first 5-core run** dropped between its 10 s samples around 31 and 41 s, so the same moment. In the 8-core runs the A55s were already at 999 at the boot probe (about 18 s).
- **In the log that second:** a dasd (pid 68) crash report, `exec webbookmarksd`, the first screen frames (swaps 11-15). Nothing that obviously touches clocks.
- **Next (kernel 4367afb):** after a drop, request the index again (neighbour, then wanted) and see whether the clock comes back.

## 5-core big-first run (log f7cd692a, about 11:26): faster despite halved clocks

- **Boot:** `mkadt.py --bigfirst` ADT (from wp210-auto-input, identical to wp210.adt apart from the random-seed), `args-big5.txt` (args-fast1 with `cpus=5`, 604 characters), kernel `1b095684…` (a55-step), shim 2e46631. Boot image `wp210-boot-big5.img`.
  - Logical cpu0 is the boot A55, cpu1-3 the A78s of domain 1, cpu4 the prime A78 (domain 2).
- **Better on every measure:**

  | Measure | 5 cores | Last 8-core runs |
  |---|---|---|
  | Slow frames, mean | **0.70 s** | about 1.0 s |
  | Slow frames, median | **0.51 s** | 0.64-0.70 s |
  | Worst frame | 2.4 s | 2.9-3.7 s |
  | Swap rate | **3.49/s** | 2.9-3.1/s |
  | Mean first frame | 0.58 s | |

  29/30 steps drew a frame; no draw differed.
- **Draw time:** 45 s in the test window, about 62 s before.
  - cpu0 (A55): 5.3 s
  - cpu1-3: 32.5 s
  - cpu4: 7.2 s
- **The clocks halve:**
  - At boot, cpu0 (A55) read **1998 MHz** and cpu1-3 2349. So the A55s can run at 2 GHz.
  - From the second 10 s sample on (as userland starts), and for the rest of the run: cpu0 **999**, cpu1-3 **1174** (exactly half), cpu4 2390-2400 (not halved).
  - The cpufreq state registers do not change (d0 0, d1 6).
  - In the 8-core runs the A55s (logical 0-3) were already at 999 at the boot probe, and the A78s (logical 4-7) never halved.
  - So whatever halves the clock hits the CPUs XNU numbers 0-3, whatever their type, outside cpufreq-hw. Possibly a clock divider, or a kernel path tied to the first four CPUs. **Finding it would give back half the speed of logical 0-3.**
- **launchd (64%) and mediaserverd (56%) still spin;** with 5 cores they take A78 time.

## 2e46631 run (log 89095bc7, about 11:13): the 0x502 errors are gone

- **No 0x502 at all:** no `EGLImageTargetTexture2D error 0x502` and no `copy error 0x502` in the whole log, at boot included (there were 6-20 per run before). Also not a single `texture N immutable` line.
  - So the textures were not immutable: the errors were **stale**. glGetError returns the first error recorded since the last call, and CA's own earlier GL calls had left one pending. The shim read it after its upload and took a good bind for a failure.
  - The fix works because it calls glGetError right before the upload, which clears the stale error. To do properly: clear the pending error before each upload (it does now), and log what was pending, to find the CA call that raises it.
- **Video** (WhatsApp 11:14): the weather widget stays "No weather data" on the Today view the whole time.
  - A scan for one-frame dark bands (15 fps, rows suddenly dark against the frames on either side) finds nothing but an app-launch zoom. The same scan on the previous video finds its glitches (1:49, 2:34, and 2:27, 3:01, 3:06).
  - The News app did not open this run (the script's path differed), so its launch is not retested directly.
- **Run:** 28/30 steps drew a frame. Swipe rate etc. as before.

## e38a725 run (log aba34a49, a55-step kernel, about 10:45): spot checks and video

- **Checks:** 402 spot checks and the first checks, **553 compared draws, 0 differed**. The fast path drew exactly what softpipe draws everywhere it was checked.
- **Run:** 30/30 steps. No panic or crash, only the usual boot messages.
- **Video** (WhatsApp 10:49):
  - **Today view, weather widget as a plain blue square:** shows again when the Today view opens, then turns into "No weather data" on glass. With no differences in any check, this is most likely the widget's placeholder look while it loads (the weather widget's background is a blue sky gradient), not a drawing error.
  - **Top Stories widget:** once showed its grey placeholder boxes without its titles. Also a loading state.
  - **One frame (video 1:49, the round 1 swipe right back to page 1):** the lower half of the screen is black and the home screen top is drawn. It is the only one in the run. The fast path is checked and clean, so this is more likely the display path: the single-buffered framebuffer caught mid-copy. To watch for.
  - **Correction (you pointed at 2:32 and 2:34):**
    - The weather widget turns from "No weather data" to plain blue at 2:32.5, after showing correctly: not a loading look.
    - Frame by frame at 2:34, the News app's launch shows black bars over its title and text lines.
    - Both line up with texImageIOSurface failures: surface 114 642x1000 at `exec News`, surface 112 750x184 during the Today view. Both are `EGLImageTargetTexture2D error 0x502`, which Mesa raises only for immutable (glTexStorage) textures.
    - **2e46631 copies into such textures with glTexSubImage2D.**
  - **Not glitches, but not what the script means:** in rounds 2 and 3 swipe right ends on the Today view, and the following taps open Apple News and Spotlight instead of the Phone app.

## wp210-a55-step kernel run (log 7268fa74, kernel `1b095684…`, shim 47ab084, about 10:15)

- **Kernel 0208 ran:** `domain 0 already at index 0: stepped to 1 first`, then index 0 again. **The A55s still read 999 MHz.** Writing the CSRAM state register does not move the A55 clock; something else sets it. Still open.
- **Run:** comparable to the last one. 2.84 swaps/s, 30/30 steps, slow frames mean 0.94 s, median 0.60 s, worst 3.1 s. No draw differed.
- **Video** (WhatsApp 10:16):
  - **Control Center:** its background is now the blurred home screen, as on a real iPhone (it was missing before).
  - **Spotlight:** its blur looks right.
  - **Today view:** for about 3 s (video 2:23-2:26) the weather widget shows as a plain bright blue square, without its glass material and text. It is back to normal at 2:31. Then the scripted tap opens Apple News. Not yet explained: it may be a widget reload or a drawing bug.

## 47ab084 run (log 27f57680, `SGLFAST=1`, about 09:18): the A55s

- **Self-check:** no draw differed.
- **Steps:** 30/30 drew a frame. Median first frame 0.29 s.
- **Slow frames:** mean 0.94 s, median 0.62 s, worst 3.9 s. The mean and median are the best so far.
- **Swap rate:** 2.85/s.
- **Threads from 4096 pixels:** the CPU path went from 20.1 s to 12.7 s per test run.
  - darken tint: 272 to 58 ns/px
  - alpha copy: 166 to 76 ns/px
  - alpha tint: 182 to 69 ns/px
- **Draw time by CPU** (softpipe and the fast path, test window): **60% on cpu0-3** (the A55s at 999 MHz). The A55s total 37.2 s, the A78s 24.5 s. backboardd's drawing thread spends most of its time on the slow cores, because the ADT presents one SMP cluster.
- **The A55 clock:** the domain 0 register read index 0 (2000 MHz) before wp210cpumhz wrote it, and enable 0x301. The A78 domains, left at index 19 and 29, moved when written. Writing the index a domain already holds probably does nothing.
  - **Kernel branch `wp210-a55-step` (cbb5e10, patch 0208):** if a domain already holds the wanted index, write the next index first, wait 2 ms, then the wanted one. It logs `SWEET CPUFREQ: domain N already at index I: stepped to J first`.
  - Built by the xnu workflow (workflow_dispatch on that branch); boot image as above with `args-fast1.txt`.
  - If the A55s then read about 2000 MHz, all softpipe and CPU-path time on them should about halve.

## 0e13161 run (log 0ca183a3, `SGLFAST=1`, about 08:58)

- **Self-check:** no draw differed.
- **Steps:** 30/30 drew a frame. Median first frame 0.25 s (the best so far), mean 0.73 s.
- **Swap rate:** 2.28/s (a noisy measure).
- **Qualifying draws softpipe drew** (self-checks, or waiting for them): 9.6 s per test run, down from 14.8 s.
- **CPU path by kind,** over the test (ms / Mpx, then ns per pixel):

  | Kind | ms | Mpx | ns/px |
  |---|---|---|---|
  | copy | 6603 | 212 | 31 |
  | darken tint | 4108 | 15 | 272 |
  | tint | 1488 | 34 | 44 |
  | noisy copy | 1329 | 85 | 16 |
  | alpha copy | 1278 | 7.7 | 166 |
  | fill | 1269 | 233 | 5 |
  | alpha tint | 1109 | 6.1 | 182 |
  | circle | 891 | 3.6 | 247 |
  | blur | 833 | 7.5 | 111 |

  On one host core: tint 34, alpha tint 17-32, darken tint 34, circle 41 ns/px (`notes/tools/fptest/bench.c`). The phone is 6-8 times slower, as if the draws ran on an A55 at 1 GHz.
- **Next build:**
  - dispatch_apply from 4096 pixels (was 65536)
  - draw time by CPU number, to see where backboardd's render thread runs
  - If it is the A55s: raise their clock (they read 999 MHz), or mark the clusters E/P in the ADT so XNU's AMP scheduler puts UI threads on the A78s.

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