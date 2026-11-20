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
- ~~**Also open:** Control Center draws without its background~~ fixed in later runs (see wp210-a55-step run).

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
| `wp210-a55-step` | 91bd138 | 0208 cpufreq step; 0209 once-a-second clocks, domain 1 re-requested when halved; 0210: no more domain 0 re-requests, the DSI waited for (DSI_INTSTA bit 31) before writing the panel buffer and before START, planar surface layouts logged (`SWEET PANEL:` / `SWEET SURF: id N fmt … plane …` lines); 0211: frames started on the panel's TE when found (`SWEET TE:`), wallpaper image bytes logged (`SWEET PIX:`); 0212: large images sampled once a second while booting; 0213: their bytes at lookup and free; 0214: wallpaper file opens (`SWEET OPEN:`); 0215: without poster descriptors; 0216: posters' RuntimeSnapshot .atx hidden from readers (`sweetsnap=0` keeps them); 0217: 0216's use-after-free fixed; 0218: launchd/mediaserverd syscall counts (`SWEET SYS:`), black-rectangle scan of presented frames (`SWEET BLACK:`); 0219: launchd's received messages, mediaserverd's port flags; 0220: `sweetjit=NAME` (code-signing enforcement off for NAME, for llvmpipe); 0221: the presenter copies the swapped surface before the slow panel copy, swaps wait for that copy (`SWEET COPY:`, `sweetfb_stage=0` = off), first messages between launchd and mediaserverd with body strings (`SWEET MSG:`); 0222: `sweetdeny` (Bluetooth audio HAL plug-in not loaded); 0223: 4 spare rows after kernel-allocated surfaces (`sweetsurfpad`) for llvmpipe; 0224: the same for the display's own buffer; 0225: profiler samples by CPU and thread; 0226: CPU PLLs and clock dividers (`SWEET CLKTREE`, `wp210lldiv`); 0227: A55 divider written back by default (A55s at 2 GHz); 0228: clear bit 12 whenever set; 0229: the framebuffer's own surface mapped cached (`sweetfb_cached`), copies timed by surface and CPU; 0230: `wp210slow=N`, A55s only for llvmpipe's workers; 0231: derecommended cores run only bound threads; 0232: DRAM PLL and DVFSRC registers logged (`SWEET DRAM`, hung: whole-page reads); 0233: only known words, from 60 s (sleep+0x50c: SError); 0234: without it (DRAM at 3068 Mbps); 0235: DVFSRC page dump; 0236: DVFSRC_INIT SMC (vendor sequence; accepted, no speed change); 0237: bandwidth test (23.6 GB/s: the 3200 step); 0238: DRAM level 8 requested (kept, no switch); 0239: SPM status (firmware parked, no DVFSRC event); 0240: DVFSRC_START (accepted, SPM never wakes); 0241: DRAM code off by default; 0242: SPM command table (= Android's after INIT); 0243: DVFSRC page after START; 0244: all 416 SPM registers; 0245: SPMI_CON + SCU_CTRL1 to Android's values (after START: nothing; SCU_CTRL1 read-only); 0246: SPMI_CON before INIT |
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

## DRAM clock (kernel 0232, read-only)

- Device tree (`fdt/fdt.dts`, the LK-passed tree): `dramc@10230000` `mediatek,mt6895-dramc`, LPDDR5 (`dram_type` 8), 4 channels, `freq_step` 6400 6400 5500 4266 3200 2667 2133 1866 800 Mbps, `fmeter_version` 1, crystal 26 MHz. Fields: `pll_id` sleep(0x10006000)+0x50c bit 8, `shu_lv` bits 16-17, `shu_of` 0x900; in DDRPHY_AO ch0 (0x10238000): `sdmpcw` 0x904/0x924 bits 16-31, `prediv` 0x908/0x928 bits 18-19, `posdiv` bits 0-2, `fbksel` 0x90c bit 6, `pll_md` 0x944 bit 8, `ckdiv4` 0xef4 bit 2, `cldiv2` 0xf34 bit 1, `dqsopen`/`dqopen` 0xef0 bits 20/21, `ckdiv4_ca` 0xe74 bit 2.
- `dvfsrc@1c00f000` `mediatek,mt6895-dvfsrc` (+ SPM page 0x1c001000): 9 OPPs, peak bandwidth 0 / 4.8 / 11.9 / 13.6 / 17.0 / 19.8 / 27.3 / 35.2 / 40.9 GB/s (opp8 → opp0). Upstream register maps for relatives (mt6873): SW_REQ 0xc, LEVEL 0xd44, TARGET_LEVEL 0xd48, SW_BW 0x26c, SW_PEAK_BW 0x278, SW_HRT_BW 0x290, VCORE_REQUEST 0x6c.
- **0232 (kwt 34e8332, xnu run 44166515030):** `SWEET DRAM:` the raw PLL words each second when they change; `SWEET DRAMDUMP:` the sleep, DVFSRC and SPM pages once (non-zero words); `SWEET DVFSRC:` DVFSRC words that change (6 changes per word at most). `wp210dram=0` turns it off. Decoded offline against `freq_step`.
- Test: kernel 0232 + `args-default.txt` (5 cores).
- **0232 hung the phone** (video 22:34:24): the boot console stops right after `SWEET CLKTREE: 19 s` (the DRAM probe's first call, in the same loop); the persistent log was never flushed (its first flush is later), so the pulled sweetlog (ea57ff31) is byte-identical to the previous boot's (9f057b7e). A whole-page read (sleep page 0x10006000, the DVFSRC page or SPM page 0x1c001000) stalled the bus.
- **0233 (kwt a31cd57, xnu run 44171302007):** only sleep+0x50c and the DDRPHY fields (what MediaTek's dramc driver reads), and 12 DVFSRC words at the mt6873 driver's offsets (0x0-0x10, 0x6c, 0x26c, 0x278, 0x290, 0xd44-0xd4c); starts at 60 s (`wp210dramat`); before each group a `SWEET DRAM: about to read …` line and a forced log flush (`sweet_log_kick` + 2 s), so a hang leaves its last line in the sweetlog.
- **0233 result (log a0696775, photo 22:54):** the phone reached the home screen, then panicked at 60 s with `Unhandled implementation specific error` (an SError) on cpu 4; the log ends with `SWEET DRAM: about to read sleep+0x50c`. **The sleep register 0x10006000+0x50c is not readable from our kernel** (most likely also what hung 0232, whose dump began with that page).
- **0234 (kwt 81438e3, xnu run 44174671075):** no sleep register; the DDRPHY fields for both shuffle slots (+0, +0x900) and both PLLs, then the DVFSRC words, each group announced and flushed first.
- **0234 result (log 2bf58c3e, full test, no crash):** `shu0/shu1 904 0x76000002 908 0x40000 90c 0x12d 944 0x11 ef4 0x8c001c81 f34 0xf006a00d/0xf0002005`, both PLLs the same, unchanged the whole run. MediaTek's fmeter (2021 dramc patch, mt6779 v0; v1 adds `<< fbksel`): `((crystal >> prediv) * (sdmpcw >> 8)) >> posdiv >> ckdiv4 >> pll_md >> cldiv2`, crystal 0x34 = 52: (52 >> 1) * 118 = **3068 Mbps**, the 3200 step (5th of 9; 6400 would be sdmpcw ~0xf600). dqsopen/dqopen (0xef0) not read yet (both set would halve it). DVFSRC at the mt6873 offsets: 000=8, 290=0x60400020, everything else 0 (0xd44/0xd48 level = 0): the mt6895 layout differs.
- **0235 (kwt 2cc1a73, xnu run 44179306754):** at 90 s, 0xef0/0xe74 of both slots, then the DVFSRC page in four announced, flushed quarters.
- **0235 result (log 6fdfe1a0, full test):** `ef0 0x13000` (dqsopen/dqopen 0: no halving), so **3068 Mbps** stands. The DVFSRC page is nearly empty: `000=8 22c=1 230=1 288=ffffffff 28c=ffffffff 290=60400020 31c=100 3ac=7`, 0x400-0xfff all 0. Whole-page reads of 0x1c00f000 are safe.
- **Vendor driver** (Xiaomi xaga MT6895 6.12 module tree, github.com/BkaNeko/android_kernel_modules_xiaomi_mt6895-6.12, `drivers/soc/mediatek/mtk-dvfsrc.c`): `mediatek,mt6895-dvfsrc` → `mt6983_data`, registers `mt6983_regs`: BASIC_CONTROL 0x0, SW_REQ 0x18 (DRAM level bits 12-15, EMI 0-3), VCORE_REQUEST 0x80, SW_BW 0x1e8, SW_PEAK_BW 0x1f4, SW_FORCE_BW 0x200, SW_HRT_BW 0x20c, DEFAULT_OPP_1/2 0x22c/0x230, TARGET_FORCE_H/FORCE 0x5dc/0x5e0, FORCE_MASK 0x5ec, LEVEL = TARGET_LEVEL 0x5f0 (current = bits 0-5 + 1, target valid bit 16). Ours: SW_REQ 0, LEVEL 0: **the DVFSRC was never started.** The probe calls `arm_smccc_smc(MTK_SIP_VCOREFS_CONTROL = 0xc2000506 (SMC64 fast SIP 0x506), MTK_SIP_DVFSRC_INIT = 0, dvfsrc-flag, dvfsrc-vmode)` (our device tree sets neither flag nor vmode nor bringup → 0, 0); a0 = 0 success, a1 = DRAM type. `mtk-dvfsrc-start.c`: the DVFSRC holds the high OPP until a late-initcall SMC `MTK_SIP_DVFSRC_START` (1). No kernel-side SPM firmware loader in that tree (the bootloader loads it).
- **0236 (kwt d369a3d, xnu run 44240006667):** at 65 s, after a flush, SMC 0xc2000506 (0, 0, 0); logs a0-a3 and the time taken, then the DVFSRC words at the mt6983 offsets and PLL changes. `wp210dvfsrc=0` skips. Default on in this kernel (the 5-core args are at 598 of 607 characters).
- **0236 result (log 47af0995, video 07:12:50, full test):** `DVFSRC_INIT returned a0 0 a1 0` in 89 us; BASIC_CONTROL 0x8 → **0x65ac02ab** (the DVFSRC is running), but SW_REQ, LEVEL (0x5f0) and the force registers stay 0 and **the PLL stays at 3068 Mbps** (no change line all run). Frames 967 (0229: 968; 5-core runs 922-968). In the vendor driver LEVEL counts down (current OPP = num_opp - (LEVEL & 0x3f) - 1), so LEVEL 0 = the top OPP {vcore 4, DRAM 8}; that contradicts 3068 unless the decode is off by 2x. MediaTek's 2025 mt8196 fmeter (`posdiv_freq = (ref * (sdmpcw >> prediv)) >> posdiv >> 1; vco << fbksel; >> clkdiv`) gives 3068 as well.
- **0237 (kwt 903a724, xnu run 44243004658):** DRAM read bandwidth, 32 MB per worker bound to big cores 1-4, 6 passes, best of 3, 1 core and 4 cores, at 62 s (before DVFSRC_INIT) and 70 s (after). 3200 Mbps x64 = 25.6 GB/s peak, 6400 = 51.2 (DT top DVFSRC OPP 40.9 GB/s): 4 cores well over ~26 GB/s would mean faster than 3200.
- **0237 result (log e2d7d881):** before DVFSRC_INIT: 1 big core **19.12 GB/s**, 4 big cores **23.64 GB/s**; after: 19.44 / 23.43. Four cores only 1.24x one: saturated at 96% of 3068 Mbps x 8 bytes = 24.5 GB/s. **The DRAM really is at the 3200 step** (the PLL decode is right; LEVEL 0 isn't "top"); DVFSRC_INIT alone requests nothing.
- **0238 (kwt 4efdba9, xnu run 44245985857):** at 82 s (after a flush) writes DVFSRC SW_REQ 0x18 bits 12-15 = 8 (the vendor's `mt6983_set_dram_level`; level 8 = top in `dvfsrc_opp_mt6983`), logs SW_REQ/LEVEL/PLL every 10 ms for 1 s when they change, then measures bandwidth again at 86 s. `wp210ddrlvl=N` (0-8), -1 skips.
- **0238 result (log 6f07c237):** `SW_REQ 0 -> 0x8000` written and kept (still 0x8000 at 88 s), no crash; but LEVEL stayed 0, the PLL stayed 0x76000002 for the whole second and after, bandwidth 19.0 / 23.6 GB/s. The DVFSRC takes requests but nothing performs the switch.
- **0239 (kwt 0794183, xnu run 44250085200):** the SPM (0x1c001000; registers from the vendor's `lpm_legacy/modules/debug/k6895/inc/k6895_spm_reg.h`): POWERON_CONFIG_EN 0x0, PCM_CON0 0x18, **MD32PCM_PC 0x194** (sampled 4 times 1 ms apart: moving = SPM firmware running), DVFSRC_EVENT_STA 0x380, SPM_DVFS_STA 0x388, SPM_DVS_DFS_LEVEL 0x38c, SPM_DVFS_LEVEL 0x390, SPM_DVFS_OPP 0x394, SPM_DVFS_CON 0x39c, SPM_SW_FLAG_0/1 0x600/0x608, SPM_SW_RSV_0 0x610; before the level request and at +10 ms, +100 ms, +500 ms, +1 s after.
- **0239 result (log 1eaef471):** SPM, identical in every sample (before, 1 ms apart, and +10 ms to +1 s after the request): `000=1 018=4 194=6b0 380=0 388=0 38c=100001 390=1 394=0 39c=50 600=2080000 608=0 610=0`. **MD32PCM_PC stuck at 0x6b0** (the firmware parked, likely waiting for an event) and **DVFSRC_EVENT_STA 0**: the DVFSRC never signals the SPM. PLL and bandwidth (23.4 GB/s) unchanged.
- **0240 (kwt 14601fe, xnu run 44252570419):** at 84 s SMC 0xc2000506 with MTK_SIP_DVFSRC_START (1) (the vendor's `mtk-dvfsrc-start.c`, "let dvfsrc free run"), then BASIC/SW_REQ/LEVEL/PLL every 10 ms for 2 s when they change, SPM lines at +10 ms, +100 ms, +500 ms, +2 s, bandwidth at 90 s. `wp210dvfsrcstart=0` skips.
- **0240 result (log 789f5235):** `DVFSRC_START returned a0 0 a1 0`; BASIC 0x65ac02ab, SW_REQ 0x8000, LEVEL 0, PLL 0x76000002 unchanged for 2 s; SPM identical in every sample (PC 0x6b0, DVFSRC_EVENT_STA 0); bandwidth 18.9 / 23.7 GB/s. **Both vendor SMCs and the level request change nothing: the SPM never wakes.**
- The vendor `lpm_legacy/inc/lpm_module.h`: `MT_SPM_SMC_UID_FW_INIT = 142857 /* call from lk load_spm */`: the SPM firmware is loaded by LK, not the Android kernel, and the Android kernel's DVFSRC path is just INIT + START + requests (all done). What differs on our boot path is below the kernel (LK/ATF state, or something Android's userspace or another driver triggers). Lead: read the same registers on stock Android (needs root) to see the working state.
- **Stock Android, rooted (dvfs.txt, `notes/firmware/android-dvfsrc-dump-idle.txt`):** `/sys/devices/platform/soc/1c00f000.dvfsrc/1c00f000.dvfsrc:dvfsrc-helper/` (`dvfsrc_dump`, `spm_cmd_dump`, `dvfsrc_opp_table` …) and `/sys/bus/platform/drivers/dramc_drv/dram_data_rate`; no `/dev/mem`. Idle: **DRAM 1866** (`dram_data_rate`), OPP 26 of 27 (Vcore 550 mV, DRAM opp 8 = the lowest; the DDR table is 800 … 6400, 27 OPPs from {787.5 mV, 6400} down). DVFSRC CONTROL 0x65ac02ab (= ours after INIT), **CURRENT/TARGET 0x1a** (ours 0), SW_REQ all 0, DVFSRC_LAST history 8 entries. SPM: PC_STA **0x6b0** (so 0x6b0 is its normal wait, not dead), **SW_FLAG_0 0x02190067** (ours 0x02080000), SPM_DVFS_LEVEL (0x38c) 2 (ours 0x100001), DVS_DFS_LEVEL (0x390) 0x10080 (ours 0x1), DVFS_STA 0, **SPM_DVFS_CMD0-23 filled** (0x024e0058, 0x024e005c, 0x024e0064, 0x024e0074, 0x024e007e, 0x04110001 …: PMIC vcore writes). No kernel driver writes the command table (ATF or LK does). Android scales DRAM with load; it doesn't hold 6400.
- **0242 (kwt e5c8568, xnu run 44353360432; 44350421638 got no runner):** the SPM command table 0x310-0x36c, STA/LEVEL/DVS_DFS, PC and SW_FLAG before INIT, after INIT, after START (offsets: the vendor debug driver's `mt6877_spm_regs`, which `mt6983_dvfsrc_config` uses). Needs `wp210dram=1`: `args-dram.txt` = `args-default.txt` without `sweetgpu=0` plus `wp210dram=1`, 599 characters.
- **0242 result (log 893e1d60):** the SPM command table is filled from boot (by LK: `24e005c 24e0060 24e0068 24e0074 24e0078 …`) and after INIT **matches Android exactly** (CMD0-4 rewritten to `24e0058 24e005c 24e0064 24e0074 24e007e`, the rest identical). SW_FLAG_0 before INIT **0x80f00** = DISABLE_VCORE_DVS | DISABLE_DDR_DFS | DISABLE_EMI_DFS | DISABLE_BUS_DFS | RUN_COMMON_SCENARIO (vendor `lpm_legacy/modules/debug/k6895/inc/lpm_spm_comm.h`), after INIT **0x2080000** (the disables cleared: INIT really enables DVFS); Android's extra bits 0x110067 are idle options (DISABLE_INFRA/DPM/MCUPM_PDN, SYSRAM/SSPM_SRAM sleep, KEEP_CSYSPWRACK_HIGH, DEEPIDLE_OPTION). SPM_DVFS_LEVEL 0x100001 / DVS_DFS 0x1 all along (Android 2 / 0x10080). The vendor probe does nothing else (INIT, the timeout IRQ, the start device). **Only difference left: the DVFSRC level stays 0 (Android 0x1a at 0x5f0, which the debug driver reads for both CURRENT and TARGET on mt6983).**
- **0243 (kwt 0896137, xnu run 44359591157):** the whole DVFSRC page after START, to compare DEBUG_STA (0x29c-), LAST (0x3ac), RECORD (0x3b8-) with Android's dump (DEBUG_STA_0 0 0 0xf00 3; _4 0 0 4; _7 0 0 0xd2; LAST 1).
- **0243 result (log 3eb7a4d8):** DVFSRC page after START, 88 non-zero words. Same as Android: CONTROL 0x65ac02ab, 0x2a4 f00, 0x2a8 3, 0x2b4 4, 0x2c0 d2, 0x290 0x60400020. Different: **DEBUG_STA_0 (0x29c) 0x20000** (Android 0), **RECORD 0x3b8- all 0** (Android 8 entries), LAST (0x3ac) 7 (Android 1), LEVEL 0x5f0 0 (Android 0x1a); also 0x00c = 6, SW_REQ (0x18) = our 0x8000. **The DVFSRC has a request in flight (bit 17, most likely waiting for the SPM) and has never completed a level change; the SPM never sees an event (DVFSRC_EVENT_STA 0, PC parked at 0x6b0).** Probable cause: the SPM's wake/event masks, which Android's low-power driver sets up through other secure calls (MTK_SIP_MTK_LPM_CONTROL, mixed with suspend/idle), closed in ATF and not visible from Android's sysfs.
- **Stock Android through kprobes (no module):** the kernel has `CONFIG_KPROBE_EVENTS=y` (`notes/firmware/android-kernel-info.txt`). `notes/tools/android/kp1b.sh`: probe `mtk_dvfsrc_helper:dvfsrc_dump_reg` (the names are duplicated, so module-qualified) → the helper struct's words 4 and 5 (+32, +40) are the DVFSRC and SPM ioremaps. `kp2.sh`: reads through them (`+0xOFF(+32(%x0)):x32`), checked first (DVFSRC CONTROL 0x65ac02ab, SPM PC 0x6b0); the whole DVFSRC page and all 416 registers of `k6895_spm_reg.h` (`notes/tools/android/mt6895-spm-regs.txt`) read without trouble: `notes/firmware/android-kp2-dvfsrc-spm-regs.txt`.
- **DVFSRC, Android vs ours (0243):** configuration identical word for word (0x0fc-0x12c, 0x258-0x2c0, 0x320-0x3a8 …). Status only: 0x29c Android 0 / ours 0x20000; LAST 0x3ac 3 / 7 and RECORD 0x3b8- filled / empty; **0x5c8 0x4000000 / 0x1** (not named in either vendor map); LEVEL 0x5f0 0x1a / 0. So the difference is on the SPM side.
- **0244 (kwt 5194dc9, xnu run 44369976591):** the same 416 SPM registers on our boot before INIT, after INIT, after START (`SWEET SPMALL:`); `notes/tools/android/spmcmp.py LOG` lists every register that differs from Android, by name.
- **0244 result (log e1f4b8fe; `notes/firmware/spm-diff-0244-vs-android.txt`):** 98 SPM registers differ before INIT, 90 after INIT/START. Explained: power-domain states (CONN/AUDIO/ADSP/DIS/MM/SCP PWR_CON, PWR_STATUS), CPU power (CPU_PWR_STATUS, SPM_CPUx_PWR_CON), timers and sleep records (SYS_TIMER_*, PCM_TIMER_OUT, SPM_BK_*, PCM_WDT_LATCH_SPARE_*), idle resource masks (SPM_SRC_MASK_0/2/4/5/6; SPM_SRC_REQ ours has REG_SPM_DDREN_REQ), the DVFS level/OPP outputs. **Candidates:** `SPM_PMIC_SPMI_CON` 0x308 ours 0x83 (CMD 3, **SLAVEID 0**, DBCNT 1), Android 0x13 (CMD 3, **SLAVEID 4**): the PMIC the SPM's vcore commands go to; `MD32PCM_SCU_CTRL1` 0x104 ours 0x3ff920, Android 0xbff820 (bit 23 set, bit 8 clear); SPM_ULTRA_REQ 0xc0000 / 0x1c0000 (ack bits).
- **0245 (kwt 33842e4, xnu run 44377707034; 44374901417 got no runner):** after START, writes SPMI_CON = 0x13, then SCU_CTRL1 = 0xbff820 (one at a time, flushed first), logging LEVEL, DEBUG_STA_0, SPM PC/event/levels and the PLL every 10 ms for 2 s when they change, then bandwidth. `wp210spmfix=0/1/2` (default 2).
- **0245 result (log 484ece15):** SPMI_CON 0x83 → 0x13 took; nothing moved in 2 s (LEVEL 0, DEBUG0 0x20000, SPM pc 0x6b0, event 0, PLL 0x76000002). **SCU_CTRL1 ignored the write** (still 0x3ff920): only the secure side or the SPM itself can set it. Bandwidth 23.6 GB/s, no crash.
- **Next (`notes/tools/android/kp3.sh`):** on a freshly booted Android with USB connected (no suspend yet): SCU_CTRL0-3, STA0, PC, SPMI_CON, DVFS regs, SW_FLAG/RSV_7, DVFSRC CTL/DEBUG0/LAST/0x5c8/LEVEL, suspend count, DRAM rate; then 60 s of SIP SMCs (`__arm_smccc_smc`, x0 in 0x82…/0xc2…, all args); registers again. Shows whether SCU_CTRL1 bit 23 comes from Android's suspend path or from a call we can replay.
- **kp3 result (fresh Android, USB on, no suspend; `notes/firmware/android-kp3-*.txt`):** at 19 s: **SCU_CTRL1 0x3ff920 and SW_FLAG_0 0x2080000 = ours**, yet DVFS already works: DRAM **5500**, LEVEL 1, LAST 5, DEBUG0 0, 0x5c8 2, lv 0x100002/0x10, **SPMI_CON already 0x13**. At 80 s: SCU_CTRL1 0xbff820, flag 0x2190067 (idle set-up, later), DRAM 1866, LEVEL 0x1a, 0x5c8 0x4000000. So SCU_CTRL1/flags aren't the blocker. 60 s of SIP SMCs: 0xc2000276 (UFS, 309), 0xc2000514/0518 (display/MMSYS), 0xc2000507 a1 0xda000001 every 5 s from idle, 0xc2000232 (sensors), 0x8200026a (hwrng); no DVFSRC call (its INIT runs at probe, before the trace). Ours: SPMI_CON 0x83 (slave 0, LK's) before and after INIT; Android's kernel sets 0x13 before its DVFSRC probe. A first transition with vcore commands to the wrong PMIC would hang → the stuck request (DEBUG0 bit 17), which a later fix can't rescue.
- **Probe kit result (Android boot e1e8eab2, `notes/firmware/android-probe-e1e8eab2/`):** Magisk's earliest hook (post-fs-data) runs at 5.2 s with all 351 modules loaded and the DVFSRC already initialised. **At 5.5 s Android's DVFSRC is already switching** (LEVEL 3/2/1/0x15/0xc/8/0x1a, DRAM 5500→1866→2133→3200…). **DEBUG_STA_0 0x20000 appears briefly on Android too: bit 17 = transition in progress** (ours is stuck on it). **0x5c8 = 1 << current level** (level 3 → 0x8, 0x1a → 0x4000000): ours 0x1 = the DVFSRC thinks it is at level 0 (the top OPP) and is trying to leave it. At 5.5 s SCU_CTRL1 0x3ff920 and SW_FLAG 0x2080000 = ours; SPMI_CON 0x13 (and it alternates 0x13/0x17 while switching: the SPM uses it every time); SPM levels 0x80004/0x10 (ours 0x100001/0x1). Secure calls from 5.3 s: VCOREFS 0x12 (= GET_VCORE_INFO, read-only, a daemon), MMSYS/display 0x514/0x518, 0x505 (CCCI), 0x517, 0x52c/0x52e (vcodec), 0x507 (LPM, every 5 s from idle), UFS 0x276. Nothing DVFS-related beyond INIT/START (`mtk-dvfsrc-start.c` also has PAUSE_ENABLE 0x21, panic only). So the remaining difference at INIT time is SPMI_CON.
- **0246 (kwt 91bd138, xnu run 44383509942, built):** SPMI_CON = 0x13 **before** DVFSRC_INIT (`wp210spmi=0` skips), then watches LEVEL/LAST/DEBUG0/0x5c8/SPM/PLL for 3 s after INIT; the 0245 post-START copy off by default.
- **0246 result (log 497312a3):** SPMI_CON 0x83 → 0x13 before INIT took; INIT a0 0; **3 us after INIT: LEVEL 0, LAST 7, DEBUG0 0x20000, 0x5c8 0x1**, unchanged for 3 s, through START and to the end; SPM pc 0x6b0, PLL 0x76000002, 23.7 GB/s. DEBUG0 was 0 before INIT: INIT's first transition hangs whatever SPMI_CON is. **SPMI_CON ruled out.** Next suspect: the PMIF (SPMI master the SPM sends its vcore commands through), whose channels Android's PMIF driver sets up before its DVFSRC probe and our kernel never touches: compare its registers Android vs ours (kit + kprobe on the PMIF driver).
- **Stopped here.** DRAM stays at 3068 Mbps (LK's speed; 23.6 GB/s on 4 big cores). Everything DRAM is off unless `wp210dram=1`. Default stays `wp210-boot-0229.img`. Next lead if picked up again: MediaTek's LPM/SPM ATF sources for mt6895/mt6983 (the SPM event mask set-up), or Android's lpm debugfs.
- **0241 (kwt e50236d, not built yet):** `wp210dram` defaults to 0: the probe, bandwidth tests and DVFSRC steps run only with `wp210dram=1`. Default image stays `wp210-boot-0229.img`.

## 8 cores with the A55s only for llvmpipe's workers (kernel 0230, `wp210slow`)

- Layout with `cpus=8` (log 03157eac MHz line): cpu0 A55, cpus 1-3 A78 3.0 GHz, cpu4 A78 3.1 GHz, cpus 5-7 A55. With `cpus=5` the one A55 is cpu0 (the 0229 copies by CPU: cpu0 0.83 ms, the others 0.4 ms).
- **0230 (kwt df0f2e6):** `wp210slow=N` takes the A55s (physical cores 0-3, from MPIDR in the 1-second clock thread) out of the scheduler's recommended set (`sched_perfcontrol_update_recommended_cores`, the performance controller's mechanism: a core out of the set runs only threads bound to it). llvmpipe's workers `llvmpipe-N`, `llvmpipe-N+1`, … bind themselves to the A55s, one each, when they name themselves (hook in `proc_info` `PROC_SELFSET_THREADNAME`). Everything else, CA's render thread included, runs on the 4 big cores. Logs `SWEET SLOW:` lines (applied mask, each bind, and every 30 s: recommended mask, failsafe, threads named/bound).
- Boot args `args-8core-slow.txt`: `args-8core.txt` without `sweetgpu=0` (off is the default) plus `wp210slow=4` (llvmpipe-4..7 on the A55s, llvmpipe-0..3 on the big cores), 598 characters.
- First build failed (the declaration isn't visible from BSD code; fixed); then GitHub had no macOS Intel runner twice (runs 44149185357). Built: **xnu run 44155246115** (success).
- **Result (log 60c5f619, video 21:33:17, kernel 0230 + `args-8core-slow.txt`):** applied at boot (A55s 0xe1 out, recommended 0x1e, no failsafe all run); llvmpipe-4..7 bound to cpus 0,5,6,7, llvmpipe-0..3 on cpus 1-4 only (PROF round 3: big workers 248-288 samples, A55 workers 147-167; CA's render thread 1 of 90 samples on an A55, was 35%). Frames 819 (old 8-core 685, 5-core 0229 968); rounds 2-3 574 (630). **But this was a cold shader cache:** llvmpipe's cache key hashes `util_cpu_caps` (`nr_cpus`, `max_cpus` in its first 5 dwords), so an 8-core boot can't use the 5-core cache. 17 slow frames 16.5 s (5 of them, 3.1 s, in rounds 2-3). Needs a second 8-core boot for a fair number. Video: no one-frame glitches.
- Leak: the presenter's staged copies ran on A55s 200 of 1762 times (0.82 ms there, 0.40 on big cores): `sched_clutch_choose_thread` on a derecommended core with bound threads queued also takes shared threads. **0231 (kwt a48a1aa, xnu run 44160395950):** derecommended cores take only bound threads (choose_thread, csw_check, queue_has_priority), and unbound threads avoid them (`thread_avoid_processor`). No effect without `wp210slow`.
- **Result 0231 (log 9f057b7e, video 22:04:41, `args-8core-slow.txt`, warm 8-core cache):** no leak left: all staged copies on cpus 1-4 (0.40 ms), CA's render thread 0 samples on A55s, A55 workers 136-172 samples each in PROF round 3 (big-core workers 277-328). Frames **939** (5-core 0229: 968), rounds 2-3 618 (630), slow frames 7 / 4.3 s (one of 1.1 s in round 3). Video clean. **8 cores with the A55s for llvmpipe only is no better than 5 cores (-3%, likely within run-to-run noise): stay on 5 cores (`wp210-boot-0229.img`).** The A55 workers add rasterizer threads but the big cores don't get faster; rasterization is probably held back by memory (the cached-surface fix and the A55 clock fix were both memory-side wins). Next lead: the DRAM clock (MediaTek DVFSRC/EMI), which nothing in our kernel requests.

## Frame copy 4-11 ms: the framebuffer's own surface was write-combined (kernel 0229)

- `SWEET COPY` staged-copy averages across runs: 3.5 ms (log3g), 3.7, 4.6, 4.9, 5.1, 5.5, 6.3, 6.7, 7.7 and 8.6 ms (8 cores). The copy is 4 MB (750x1334, 3008 bytes a row): 0.5-1.1 GB/s. Cached memory would be about 0.5 ms.
- CA swaps between surface 2 (backboardd's IOSurface, ordinary cached memory, bpr 3072) and **surface 1, the framebuffer's own** (`sweet_surface_create_physical` over fShadow, bpr 3008), which was mapped **write-combined** for the kernel and for backboardd. Every CPU read of it is uncached: the staged copy, and llvmpipe reading tiles back to blend. fShadow is ordinary RAM that only CPUs touch (the DSI sends fPanel), so it doesn't need write-combining.
- How fast a run's copy was depended on how much of the copy came from surface 1, and on which core did it (A55s and memory contention with the rasterizer).
- **0229 (kwt 47feb9f, xnu run 44033305723):** surface 1 mapped cached (`sweetfb_cached=0` = write-combined as before; `sweetgpu=1` keeps write-combined by default, since the GPU path reads it by DMA). Logs `SWEET FB: own surface mapped cached` and a second `SWEET COPY` line every 30 s: own-surface copies against other surfaces, and copies by CPU.
- Test: kernel 0229 + `args-default.txt` (the shader cache is already filled), compare with log ca95c935.
- **Result (log 100a1caf, video 20:04:09): the copy is 11x faster and frames are up 16%.** Staged copy **0.48 ms** average (was 3.5-8.6 ms), max 1.1 ms (was 38-40 ms every run), own surface 0.48 ms and other surfaces 0.50 ms alike; by CPU: cpu0 0.83 ms, cpus 1-4 0.39-0.41 ms. Swaps that waited for the copy: 3 (was 20). Frames, all 30 steps: 836 → **968**; round 1 300 → 338, rounds 2-3 536 → 630 (+18%). Slow frames 10 / 6.7 s → **4 / 2.0 s**, none over 1 s (max 856 ms). A55s 1999 MHz. The video has no one-frame glitches. The panel copy (scaling into the DSI buffer) is still ~4.9 ms.
- **Default now: kernel 0229 + `args-default.txt`** (`wp210-boot-0229.img`).

## Shader cache works (c5543ad; logs 4048b139 = boot 1, ca95c935 = boot 2, video 08:31:20)

- The `disk shader cache off` line is printed too early to mean anything: llvmpipe creates its cache in `llvmpipe_screen_late_init` (first shader), after screen creation. Mesa could name the dylib's mtime (dladdr `/usr/standalone/update/ramdisk/arm64SURamDisk.dmg`, stat 0, mtime 1777518794, uid/gid 501), so the build-id fallback isn't used.
- **Boot 1** (fills the cache): 20 slow frames, 21.9 s, 6 over 1 s (max 4.9 s). **Boot 2:** **10 slow frames, 6.7 s, 2 over 1 s (max 1.4 s)**. Round 1 233 → 300 frames: first Spotlight 2 → 15 frames, home after Spotlight 12 → 55, Control Centre 2 → 6. Whole run 788 → 836.
- llvmpipe at 3 GHz, A55 at 2 GHz, 5 cores, shader cache = the default now (`wp210-boot-0228.img` = kernel 0228 + `args-default.txt`, shim c5543ad in the ramdisk space). Softpipe fallback: `args-softpipe.txt` (`/usr/lib/sweetgl.dylib`).
- To do: print the cache state after the first frame instead of at screen creation.

## Shader-cache shim: install script (2026-11-17 07:45)

- **First boot with it (log 2fbde5ff; the user rebooted twice by accident, the log holds only the last boot):** `sweetgl: shader cache in /private/var/mobile/Library/Caches/sweetgl/run43962007014 (uid 501)` but `disk shader cache off`. Mesa's `disk_cache_create` returns a cache even when the directory fails, so the cache was never created: `lp_disk_cache_create` returns early when `disk_cache_get_function_identifier` (dladdr + stat of the dylib, for its mtime) fails for `lp_disk_cache_create` or `LLVMLinkInMCJIT`. Frames as before (733; rounds 2-3 523), 19 slow frames, 20.1 s, 5 over 1 s. A55 fix at 41 s, cpu0 1999 to the end.
- **Shim c5543ad (run 44027538880, macos-15-intel, success):** build-llvmpipe.sh patches Mesa's `lp_screen.c` so that the cache id falls back to `SWEETGL_CACHE_ID` (`sweetgl-<build>`, from the cross file's c_args) instead of giving up, and sets `sweetgl_lp_cache_id_fallback`; sweetgl.c logs the cache state, where the id came from, and dladdr/stat (errno, mtime) and uid/gid, to see which part failed.
- c5543ad artifact (`sweetgl-llvmpipe_2.zip`): dylib 51,324,032 bytes, SHA-256 `8a774274…`, signature valid, contains `run44027538880` and `sweetgl-run44027538880`. Install script `tools/install/install-sweetgl-llvmpipe-c5543ad.sh` (OLD = b6214f5 payload `26f658c5…`, NEW `4e829218…`, dylib at `/data/local/tmp/sweetgl-llvmpipe-c5543ad.dylib`), simulated: check, install verified, second run refused.

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
5. ~~Control Center background~~: drawn correctly since the wp210-a55-step run (blurred home screen) and in the llvmpipe runs; texture function 0x3d never needed supplying. Recheck on 0251.
6. **Lower priority:**
   - ~~the 0x502 setup errors~~ gone since shim 2e46631 (stale glGetError)
   - ~~the launchd and mediaserverd spin~~ fixed by 0222 (`sweetdeny`); 0251 idle TOP 14-17% (kernel_task 7-9%, backboardd 5-6%)
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
### PMIF on stock Android (kp6, 2026-11-18)
- The `spmi_mtk_pmif` driver keeps its mapped bases in its static template `mt6xxx_pmif_arb`
  (+0x00 PMIF, +0x10 SPMI master). Read through those pointers with kprobes:
  `notes/firmware/android-kp6-pmif-spmimst.txt` (PMIF 0x000-0x8fc, SPMI master 0x00-0xfc, uptime 1015 s, DRAM 1866).
- Key PMIF words (mt6xxx_regs): INIT_DONE 0x000=1, INF_EN 0x024=0x7effff, 0x028=1 (clkbuf CONN inf),
  ARB_EN 0x150=0x7affff, CMDISSUE_EN 0x3b8=1, TIMER_CTRL 0x3e4=3, SPI_MODE_CTRL 0x408=0x225d,
  IRQ_EVENT_EN_0 0x420=0x80000000.
- Patch 0247 dumps the same blocks on our kernel before/after DVFSRC_INIT (`SWEET PMIF:` / `SWEET SPMIMST:`);
  compare with `notes/tools/android/pmifcmp.py kp6.txt sweetlog.txt`.
- **0247 result (log 6ab44313, kwt c9f6d7b, run 44394786623):** PMIF vs Android (`notes/firmware/pmif-diff-0247-vs-android.txt`):
  every enable/config word matches (INIT_DONE, INF_EN 0x7effff, ARB_EN 0x7affff, CMDISSUE_EN, TIMER, SPI_MODE 0x225d,
  slp-protect 0x3f0). The 84 differing words are history/status: the command record ring 0x4f8-0x768, last-command
  words (0x054/0x13c/0x388/0x3c0/0x3d4), software channels 0x800-0x8d4 (LK's leftovers), IRQ enable 0x420 (Linux driver).
  Android's record ring holds **writes to slave 4 register 0x24e (VCORE) from a hardware channel** (0x7e40024e / 0x6640024e,
  the SPM command table's 0x024e00xx); ours holds only polling reads (0x1b45/0x1449/0x144d), the same ones Android has.
  SPMI master: identical except SLV_7_4_EINT (0x024) = 4 (slave 4 interrupt pending; Android's driver clears it at probe).
  **The PMIF is set up like Android's; our SPM simply never sends a VCORE command.** PMIF ruled out.
- SPM vs Android **at 5.57 s** (`full-5.57-first.txt`, DVFS already running), timers skipped: 36 differ. Besides power
  domains and CPU power: SPM_PWRAP_CON 0x300 = 3 on Android at 5.57 s (0 when idle: a PMIC transaction in progress),
  DVS_DFS_LEVEL/DVFS_LEVEL/OPP moving on Android, SYS_TIMER_LATCH all zero on ours: **the SPM firmware has handled no event since boot.**
- The kp3 SIP trace started after DVFS was already running (post-fs-data); the kernel's early calls (lpm/spm/dvfsrc module init,
  first ~5 s) were never seen. **kp7:** trace them from the first instruction with a boot-time kprobe
  (`kprobe_event=p:wp210b/smc,__arm_smccc_smc,...` on the command line of a copy of boot_b booted with `fastboot boot`,
  nothing flashed); a Magisk post-fs-data hook saves the trace, dmesg and /proc/bootprof.
- **Boot trace 1 (2026-11-19, boot 7cf56765):** `fastboot boot` is refused ("unknown command"), so the traced copy was
  flashed to boot_b (backup `boot_b.img`, sha256 a9dd144f…, restored after). LK drops the first 20 characters of the
  boot-image command line: pad with a 40-character dummy word first. The kprobe ran; Android's init turns tracing off
  at 3.62 s. Window 1.375-3.616 s (`notes/firmware/android-boottrace1-sip.txt`, PSCI CPU_SUSPEND removed: 655k of them).
  VCOREFS_CONTROL calls at 1.621 s (init, the dvfsrc helper's probe): a1 2 GET_OPP_TYPE, 3 GET_FW_TYPE, 4/5 GET_VCORE_UV /
  GET_DRAM_FREQ per OPP, 7; **no DVFSRC_INIT (0) in the window**. Other SIP: 0xc2000518 (display), 0xc2000276 (UFS),
  0xb2000001/0xb2005704/5 and 0xbb00ffa7 (trusted OS), 0x8200026a (rng). Every CPU's first event is at 1.375-1.376 s
  and modules start loading at 0.97 s (bootprof): something before 1.375 s is missing (buffer reset or overflow of the
  PSCI flood; CPUs 5/6 start at 1.61/1.52, overwritten). dmesg starts at 4.86 s (a warning flood filled it).
  Next: same with `cpuidle.off=1` (no PSCI flood) and the ring-buffer stats saved (kp9 hook).
- **Boot trace 2 (boot c504336a, `cpuidle.off=1`):** complete (ring-buffer overrun 0 on every CPU, 1570 entries, first
  event 0.807 s, tracing off at 3.69 s by atrace.rc): `notes/firmware/android-boottrace2-sip.txt`. Secure calls in order:
  hwrng (0.81 s); **DVFSRC_INIT 0xc2000506 a1=0 a2=0 a3=0 at 1.165 s** (identical to ours); trusted OS 0xbb00ffa4/a5/a7
  (memory sharing); display 0xc2000514/518; the dvfsrc helper's queries at 1.725 s (a1 2, 3, 4×32, 5×36, 7); UFS. Nothing
  else, no DVFSRC_START before 3.69 s. **Android's kernel sends no extra secure call before or after INIT.** Remaining
  differences: when INIT runs (Android 1.16 s of kernel time, ours 65 s) and direct register writes by kernel drivers.
- **0248 (kwt, run 44466407051):** `wp210dram=2` = INIT from SweetPlatformExpert::start (as early as our kernel can),
  2 s watch of DVFSRC/SPM/PLL (`SWEET EARLY:` lines), no INIT at 65 s; the rest of wp210dram=1 unchanged.
  Args: args-dram.txt with `wp210dram=2` (same length).
- **0248 result (log 920c3063):** INIT from the platform expert at 18.7 s (counter time): flag0 0x80f00 before
  (bits 8-11 = DVS/DFS disabled, LK's state) → 0x2080000 after (ATF's INIT enables them, as on Android); 3 us after INIT:
  DEBUG0 0x20000, 0x5c8 1, LAST 7, and nothing else for 2 s or ever; SPMI_CON 0x83. **Timing of INIT ruled out.**
- Android (boot c504336a): SPM_PMIC_SPMI_CON 0x17 (slave 5) at 5.6 s, 0x13 (slave 4) later: **the SPM firmware itself
  rewrites SPMI_CON per PMIC** while it runs DVFS; ours stays at LK's 0x83 because our SPM never runs its DVFS code.
  DVFSRC record (13 MHz SYS timer, 0x504): Android's first two transitions at ≈3.9 s kernel time, 2.7 s after INIT and
  just after trace 2 ended (3.69 s). Next: trace past 3.69 s (`androidboot.fastboot.boottrace=enabled` keeps atrace.rc
  from turning tracing off).
- **Boot trace 3 (boot 6e2a1553, + `androidboot.fastboot.boottrace=enabled`):** complete to 19.1 s, no overrun
  (`notes/firmware/android-boottrace3-sip.txt`). DVFSRC_INIT 1.105 s, helper queries, then from the second-stage modprobe:
  0xc2000232 (3.69), 0xc2000528 a1 3 (3.695), audio 0xc2000517, 0xc2000202 (4.419), 0xc200051e ×8 (4.43-4.46),
  **45 MTK_SIP_MTK_LPM_CONTROL (0xc2000507) calls at 4.638-4.646 s** (`android-lpm-calls-before-start.txt`; decoded with
  lpm_legacy `lpm_module.h`: 0xDA000000 | user<<16 | uid; user 0 CPU_PM irq-remain list + VALIDATE_PWR_STATE_CTRL + CPC_COMMAND,
  1 SPM_DBG RES_USER_NAME/DOE_RESOURCE_CTRL/COND_BLOCK/RC_BBLPM, 2 SPM SET_PENDING_IRQ_INIT 0x12e, 3 CPU_PM_LP irq-remain),
  then **DVFSRC_START at 4.653 s**. Ours never sends any 0x507 call. Android's first DVFSRC record entry ≈ 3.8-4.2 s by the
  13 MHz SYS timer (uncertain mapping; consistent with right after START).
- **0249 (kwt, run 44475059088):** `wp210dram=3` = the wp210dram=1 sequence without the SPMI_CON write, plus the 45 LPM calls
  replayed (Android's order and arguments) right before START at 90 s; START watch logs LEVEL/LAST/DEBUG0/SPM pc/SPMI_CON/PLL.
- **0249 result (log e64e673e):** all 45 LPM calls returned a0 0; nothing moved: SPM pc 0x6b0, SPMI_CON 0x83, flag0 0x2080000,
  DVFSRC LEVEL 0 / LAST 7 / DEBUG0 0x20000 before and after the replay and for 2 s after START; PLL 0x76000002; 23.4 GB/s.
  **Every secure call Android makes up to START has now been replayed: the blocker is not a secure call.**
  Remaining: direct register writes by Android drivers (modprobe 3.6-4.8 s loads vendor_dlkm modules; clk_disable_unused at 4.68 s),
  or the order of the first DVFSRC step relative to them. Next: pin Android's first DVFSRC step to kernel time in one traced
  boot (lib.sh probe() now keeps the trace timestamp: `ts=` + SPM SYS_TIMER 0x504 maps the DVFSRC record's 13 MHz stamps).
- **Boot trace 4 (boot 3748cfe5, kit probe timestamps):** SPM SYS_TIMER (0x504) runs at 13.00 MHz; mapped to kernel time
  with the sp0 probe's timestamps (6.38/20.43/84.01 s). Android's **first two DVFSRC steps: 4.4109 and 4.4112 s** (record
  entries 0x11a7f0e4/0x11a800e3, the only two until 6.4 s) — **before** the LPM calls (4.503-4.510) and DVFSRC_START (4.517).
  Secure calls just before: 0xc2000202 (4.301), 0xc200051e ×8 (4.313-4.336), trusted OS 0xbb00ffa4/0xbb000f0b/0x33000000
  (4.328-4.373); nothing between 4.373 and 4.411. So DVFS starts on Android without any secure call: a driver's register write
  (DVFSRC request, EMI bandwidth, …) from a module loaded ~4.3-4.41 s by the second-stage modprobe. Next: trace module loads
  (`trace_event=module:module_load`) in the same boot.
- **Boot trace 5 (boot ebd99ec7, + module_load events + kprobe on mtk_dvfsrc_send_request):**
  `notes/firmware/android-boottrace5-modules-dvfsrc-req.txt`. First-stage modules before mtk_dvfsrc (1.231 s): clk_mt6895*,
  mtk_dcm/mt6895_dcm, pmic_wrap, spmi, regulators, mtk_vmm_spm, mtk_scpsys*, clk/pd checkers, **mtk_dramc (1.204)**.
  INIT 1.2314; DVFSRC requests (cmd: 0 BW, 2 VCORE, 3 DRAM, 4 HRT BW, 6 PEAK BW): 1.2346 (6/0/4 = 0), 1.283 VCORE 4,
  1.481-1.483 (VCORE 4/2/0, HRT 0xceafe), 3.465 DRAM 6 (init-381, second-stage init), 4.631 (6/0/4 0xceafe, modprobe), 7.65 …
  **First DVFSRC step this boot: 4.6586 s** (13.009 MHz map), between camera modules (mtk_aie 4.647, camera_dpe_isp70 4.655)
  and mtk_lpm (4.681) / mtk_dvfsrc_start (4.693). In boot 3748cfe5 it was 4.411 s; no fixed precursor yet.
  Next: sample DVFSRC registers at each request (struct offsets guessed: driver_data +0x78, regs +0x40/+0x48, checked by
  V_MODE 0x290 = 0x60400020) to see whether Android's DVFSRC also sits at DEBUG0 0x20000 from INIT until the first step.
- **Boot trace 6 (boot 7b4a776c):** the req probe's guessed offsets were wrong (driver_data +0x78 reads 0: GKI's struct
  device layout differs), and without trace_buf_size the default buffer overran (first-stage events lost). kp10.sh finds the
  real offsets on live Android (scan the device struct for a pointer whose struct holds the DVFSRC ioremap from the helper).
- **kp10/kp11 (live Android):** GKI struct device: kobject 0x60 (4 KABI words), driver_data at **+0x98**; mtk_dvfsrc's struct
  holds its own DVFSRC ioremap at **+0x40** (0x290 reads 0x60400020 through it; the helper maps the DVFSRC separately, at a
  different address each boot). Boot probe chain: `+0xREG(+0x40(+0x98(%x0)))` on mtk_dvfsrc_send_request.
- **Boot trace 7 (boot 0b43e338, DVFSRC read at every request; `android-boottrace7-dvfsrc-at-requests.txt`):**
  **Android's DVFSRC is in exactly our state from INIT (1.199 s) to at least 4.585 s**: BASIC 0x65ac02ab, DEBUG0 0x20000,
  LAST 7, 0x5c8 1, LEVEL 0 (SW_REQ 0x40/0x20/0 from VCORE requests, 0x6000 after init's DRAM request at 3.39 s).
  First step at **4.5952 s** (record, 13.006 MHz map), 10 ms after modprobe's BW/peak/HRT requests (4.585, the camera
  modules; in boot ebd99ec7: requests 4.631 → step 4.6586 after mtk_imgsys_v4l2/mtk_aie/camera_dpe_isp70 loaded),
  **before** the LPM calls (4.634) and START (4.646). By 7.47 s LAST 4-5, LEVEL 3, 0x5c8 8.
  So INIT leaves the same stuck request on both; on Android something in the camera/imgsys module loads releases it.
  Next: clk_prepare/clk_enable/regulator events + module loads in the same boot to see what those modules switch on.
- **Boot trace 8 (boot ec7cc2df, + clk_prepare/enable, regulator, module_load):** first step **4.73723 s**; window
  (`android-boottrace8-window-4.5-4.8.txt`): mtk_imgsys_v4l2 loads 4.7164 → its BW/peak/HRT requests 4.7212 (DVFSRC still
  stuck) → **step 4.7372** → mtk_aie loaded 4.7370 then enables vbuck3, imgpll, img/ipe clocks from 4.7379 (after the step).
  No clock enable or regulator enable in the 16 ms before the step: the trigger is something not traced (clock disable,
  runtime PM / power domain, or a co-processor). Delay request→step: 10-28 ms over three boots. Next: + rpm resume/suspend,
  clk disable/unprepare, regulator disable.
- **Boot trace 9 (boot 10d9f05c, + rpm resume/suspend, clk disable/unprepare, regulator disable):** first step ≈4.6610 s
  (map error ~1 ms: the request at 4.6623 still reads LAST 7), right as mtk_imgsys_v4l2 initialises: rpm_resume
  **15000000.imgsys_fw** 4.6594, its BW requests 4.6623 (`android-boottrace9-window-4.55-4.75.txt`). No clock or regulator
  change in between (mtk_ccu at 4.6355 powered CAM clocks on/off without effect). Delay after the imgsys requests over
  four boots: -1, 10, 16, 27 ms. Working idea: the IMG power-up raises resource requests (REQ_STA) that wake the SPM,
  which then serves the DVFSRC's pending request; our SPM never sees a resource change.
- **0250 (run 44487611532):** `wp210dram=4`: at 90 s, on the stuck DVFSRC, set SPM_SRC_REQ (0x818) software request bits
  8 SW_MAILBOX, 3 DVFS, 4 F26M, 5 INFRA, 1 APSRC, 10 VRF18 one at a time for 300 ms (requests only, register restored),
  stop at the first DVFSRC/SPM change; then START as before.
- **0250 result (log 9dac1a47) — RAM SPEED FIXED IN PRINCIPLE:** SPM_SRC_REQ was 0x6360204, REQ_STA_0 0. Bit 8 (SW_MAILBOX):
  nothing in 330 ms. **Bit 3 (REG_SPM_DVFS_REQ): within 11 ms the SPM firmware ran** (pc 0x6b0 → 0x2110, SPMI_CON 0x83 → 0x13,
  DVS_DFS 0x10080, DVFSRC LEVEL 0x10000 / LAST 2 / 0x5c8 0) and **the DDR PLL changed: 904 0x76000002 → 0x8f000002 →
  0x69800002 (shu0; fbksel 1) = 5486 Mbps** (shu1 0x52000002 = 2132). After clearing bit 3 the SPM parked again with the DRAM
  at 5486; START then left the DVFSRC with a new pending request (LAST 7, DEBUG0 0x20000), DRAM unchanged.
  **Read bandwidth: 1 big core 19.5 → 23.7 GB/s, 4 big cores 23.6 → 32.7 GB/s (+38%).** Stable to 150 s+.
  So Android's DVFS starts when something raises an SPM resource request (on Android: the ISP power-up at ~4.6 s).
- **0251 (run 44491297895):** the fix as a default (`wp210ddr=1`, runs only when wp210dram=0): at the platform expert's start
  DVFSRC_INIT, DVFSRC SW_REQ DRAM floor (`wp210ddrlvl`, default 8), SPM_SRC_REQ bit 3 until the PLL changes (≤500 ms), bit 3
  restored (`wp210ddrhold=1` keeps it), `SWEET DDR:` line with Mbps before/after; a bandwidth reading at 40 s.
- **0251 result (log 73ab5d3e, args-default.txt): works on a normal boot.** At the platform expert's start:
  `INIT a0 0, floor level 8, SPM DVFS request pulsed: DRAM 3068 -> 5486 Mbps (904 0x76000002 -> 0x69800002) after 22 ms;
  SPM pc 0x2118 then 0x6b0`. Bandwidth at 40 s: 1 big core 19.7, **4 big cores 32.9 GB/s** (was 23.6). Frame copy
  0.36-0.39 ms staged. CPUs 1999/3003/3003/2999/3095 MHz. Stable to the end of the log.
- **Default now: kernel 0251 + `args-default.txt`** (`wp210-boot-0251.img`). `wp210ddr=0` turns the DRAM switch off.
- Open: 6400 Mbps (level 8 gives the 5500 step; Android also tops out at 5500 under load); keeping DVFS dynamic (we hold
  5500 after one step, no scaling down at idle: costs some power); the 1-core figure moved little (19.7, latency-bound).
- **8 cores again with the DRAM fix (kernel 0251 + `args-8core-slow.txt`, logs first/second, 2026-11-19):** DRAM switch ran
  (5486 Mbps); bandwidth 1 big core 25.0/23.9 GB/s, 4 big 33.8/33.6. Frames, 30 steps: **944 and 952** (5-core 0251: **986**);
  rounds 2-3 612/620 (643). Losses are on the big animations ("home" 27 vs 33, 54-57 vs 60, cc_up 10-13 vs 15); first-frame
  times equal. A55s at 1999 MHz. **Still ~3.5% behind 5 cores: stay on 5 cores.** Faster RAM didn't change the 5-core frame
  count either (0249 slow RAM 984, 0251 986): this test is bounded by the animations / llvmpipe work split, not by DRAM.
- **Idle CPU check (0251 log, 2026-11-19):** the launchd/mediaserverd spin is gone since 0222 (`sweetdeny=BTAudioHALPlugin.driver`,
  `SWEET DENY`); neither appears in any `SWEET TOP` line. Idle total 14-17% (kernel_task 7-9%: our own sampling and log
  threads; backboardd 5-6%). Busy: backboardd 64-137% (the renderer), Spotlight up to 41%.
- **Stale list cleaned (2026-11-19):** the 'Next steps' section predates many fixes: the launchd/mediaserverd spin (0222),
  the 0x502 errors (2e46631), Control Centre's background (a55-step run) and compile stalls (llvmpipe disk shader cache) are done.
  Really open: idle power/heat with DRAM held at 5500; Wi-Fi (`sailfishwlan=0`), USB (`sweetusb=0`), battery (`sweetbattery=0`),
  buttons via the iOS key path (`sweetkeys=0`), audio, camera, cellular; 6400 Mbps.
- **Stock firmware V11 (20251201), part 1 of the split 7z uploaded (400 MB of ~3.68 GB):** the 7z stores a .tar.gz uncompressed,
  so part 1 alone extracts up to the start of super.img (7.47 GB, the rest of the parts). Extracted in the sandbox
  (/home/claude/fw): boot.img, dtbo.img, lk.img, preloader*, **spmfw.img** (18 KB, the SPM firmware), scp/sspm/mcupm/gpueb/
  dpm/ccu/apusys/gz images, scatter, ProjectConfig.mk/HardwareConfig.txt (copied to `notes/firmware/stock-v11/`).
  Hardware from ProjectConfig: Wi-Fi/BT CONSYS (MTK_WLAN_CHIP CONSYS_MT6893, BT MTK_CONSYS_MT6895), gauge version 30,
  smart PA Richtek RT5512, 2 mics / 1 speaker. Device tree: charger/gauge/TCPC **MT6375 at I2C 0x34** (mt6375-chg, -gauge,
  -tcpc, -adc, battery profiles t0-t4: OCV tables), power key on **mt6363-keys** (hw-pwrkey 8), USB `mt6895-ssusb`.
- **All 9 firmware parts (2026-11-19):** total 3,679,564,704 bytes = the 7z's size. Decoded in the sandbox (/home/claude/fw):
  super.img (Android sparse, 12 GB, LP metadata) → vendor_a (ext4, 1.86 GB), **vendor_dlkm_a** (ext4, 208 modules),
  odm_dlkm_a; vendor_boot.img v4 → lz4-legacy ramdisk (pure-Python decoder fw/lz4leg.py) → **198 first-stage modules**
  (mtk-pmic-keys, spmi-mtk-pmif, mt6375{,-battery,-charger,-adc,-auxadc}, tcpc_mt6375, mtu3, xhci-mtk-hcd, i2c-mt65xx, …)
  and vendor_boot.dtb; also vbmeta*, tee, vcp, userdata.img. These are the exact 5.10 driver binaries of this phone.
- **0252 (run 44501037198):** `wp210pmic=1`: MT6363 (SPMI sid 4) reads through PMIF SWINF channel 2 (ACC 0x880, RDATA 0x894,
  VLD_CLR 0x8a4, STA 0x8a8; command (3<<30)|sid<<24|addr, as Android and as LK left it: 0xc400001e). At 25 s: 0x08/0x09
  (identity) and TOPSTATUS 0x1e (mtk-pmic-keys: power key bit 0, active low), then TOPSTATUS every 50 ms for 60 s.
- **0252 result (log 84fe81f3): PMIC link works.** MT6363 identity 0x09/0x08 = **0x63/0x40**, TOPSTATUS 0x1e idles at
  **0x1e**; five power-key presses seen: **bit 1 drops to 0 while pressed** (0x1e → 0x1c, ~200 ms each). (The probe's
  "bit 0" label was wrong; mtk-pmic-keys' deb_mask 0x1 is a bit number.) Device tree mt6363-keys: power (116) and home
  (115 = volume up) in TOPSTATUS.
- **0253 (run 44810001089):** keys thread reads TOPSTATUS every 20 ms (PMIF channel 2, under sweet_pmic_lock): **power key → Power**;
  new default layout `wp210keymap=1`: red side key (GPIO 108) → Home, volume down (GPIO 107) → volume down; every other
  TOPSTATUS change logged to find volume up. `wp210keymap=0` = old layout (volume down = Home, side key = Power).
- **0253 result (log 876fb952, video 22:42):** power key works: press → iOS locks (lock screen "Press home to open"),
  red side key = Home works, volume down sends key 2. TOPSTATUS bits (active low unless noted): **bit 1 power, bit 3
  volume up** (0x1a ↔ 0x12, logged only in 0253), **bit 2 charger present (1 = USB power in)**; boot value 0x1a (no cable).
  **The screen does not go dark on lock:** CoreBrightness fades the backlight, but AppleARMBacklight's DCS 0x51 path is the
  Qualcomm one (sweet_fbcon_dcs: fbcon_dsi NULL on the WP210 → "FAILED (no DSI command done)", nothing written).
- **0254 (run 44814073202):** volume up mapped (bit 3 → KEY_VOLUP); one read-only dump of the MediaTek DSI registers +0x000-0x2fc at 30 s
  (`SWEET DSI:`), to see LK's command queue before sending DCS 0x51 (brightness) to the NT37705 from the DSI CMDQ.
- **0254 result (log 990f0c8f):** volume up/down reach iOS (HID 0xe9/0xea; SpringBoard builds the volume HUD layers).
  DSI dump (MediaTek, base from mtk,dsi-start): START 1, INTSTA 0x16, MODE_CTRL 0 (command mode), TXRX 0x1023c,
  PSCTRL 0x2c050438, CMDQ_CON (0x60) 0x8001; 0x200-0x2fc all 0 (on the MT6895 the command queue is at **0xd00**,
  mtk_dsi.c mt6895_dsi_driver_data reg_cmdq0_ofs; 0x200 is the video-mode VM_CMD block).
- **Backlight command from the stock panel driver** (vendor_boot panel-nt37705-fhdp2412-dsi-cmd-oled-dphy.ko,
  lcm_setbacklight_cmdq, disassembled): DCS **0x51 + 2 bytes big-endian, clamped to 0xdee** (0xfff = HBM path), 3-byte write.
  Frame entry in the queue: 0x002c3909 (mtk_dsi.c), size 1.
- **0255 (run 44817714554):** AppleARMBacklight → sweet_fbcon_mtk_bl(level·0xdee/255): under the kick lock with the DSI idle,
  CMDQ0 = 0x00033 90a (len 3, DCS long write, LONG|HSTX), CMDQ1 = 0x51 hi lo, CMDQ_CON size 2, START 0→1, wait INTSTA bit 1,
  then CMDQ0/1 and CMDQ_CON restored as found.
- **0255 result (log 81c5ff18, video 23:29): brightness works.** Every DCS 0x51 completes ("panel level … (nt37705) set");
  Control Centre slider sends 244 → 70 in steps; power key → fade to **panel level 0: the screen goes black** (video), wake by
  power or Home → level 23 then the fade back up; lock screen shows. No FAILED, no display glitch.
  **Default now: kernel 0255 + args-default.txt** (`wp210-boot-0255.img`): RAM fix, 4 keys, brightness, screen off on lock.
