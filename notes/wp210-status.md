# WP210 iOS port: status notes (updated 2026-11-14 16:55)

Working notes so the state survives session resets. Times are local
(Asia/Jerusalem) unless stated. The newest state is at the top; the older
notes are kept under "History" at the end.

## Summary

- **Hardware:** Oukitel WP210, SoC MT6896Z/CZA (Dimensity 8200) per its device tree. The repo calls the platform "MT6895".
- **Software:** iOS 16.7.16 (20H392), iPhone 8 (D20) userland. The UI is laid out at 750x1334.
- **Display:** drawn by the `sweetgl.dylib` shim (Mesa softpipe, all on the CPU), injected into backboardd.
- **Status:** iOS boots to the home screen with correct colours and rounded corners. It is usable but very slow: taps, Home presses and swipes lag by seconds, and heavy screens take seconds per frame.
- **Cause found:** CoreAnimation draws each frame as about 85-110 mostly full-screen layers, and softpipe interprets every pixel of every layer (see "Why the UI is slow").
- **Work in progress:** CPU fast paths in the shim for CA's simple draws (`SGLFAST`, commit e98dfef plus the fix bb529ae). Waiting for the CI build.

## Phone state now

- **Shim installed:** build **75b34d3**, allocation SHA-256 `da79097dd34d6bab4f1bc407ff0f92fb44ce62aa00fe5c2d371035115202a03a`. It replaced 41be079 (`13e97388…`).
- **Boot image:** the last one used had the k-watch kernel (wp210-auto-input 6c51db1, `kernel.development.sweet` `1ca9bd9b…`), the normal `wp210.adt` with 8 cores, and the boot-args in `notes/bootargs/args-freq.txt` (copied from the boot-args line of log cbf8881a). That means `sweetperf=2`, `wp210auto=90`, `wp210cpumhz=2400` and `sweetprof=backboardd,SpringBoard`.
- **Settings on the phone:** Reduce Transparency and Reduce Motion are on.
- **Backups:**
  - original pre-BGRA shim `2db9d1eb…`: PC copy in `C:\Users\artzi123\Downloads`
  - BGRA shim `309a61d7…`: `before-bgra.bin`, on the PC and on the phone
  - the allocation files (`sweetgl-diag-<commit>-allocation.bin`) for each install were pushed from `C:\Users\artzi123\Downloads\wp210-shim`; keep them there

## Branches

| Branch | Head | What |
|---|---|---|
| `main` | 1465988 | BGRA upload for format-0 4bpp IOSurfaces (the copy-error fix) |
| `sweetgl-diag-0x502` | bb529ae | shim work on top of 1465988 (table below) |
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
| bb529ae | forward declaration of `fast_shader` | pending | | next to build |

The install scripts that were sent for each build are in `notes/scripts/install-sweetgl-diag-<commit>.sh`. Their metadata hashes are the values at the time of that install.

## Installing a new shim build (the procedure that works)

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

## Fast paths (SGLFAST), e98dfef + bb529ae

- **What qualifies:**
  - CA's standard vertex shader
  - one of six fragment shaders, compared as source text with whitespace removed: fill, copy, tint, alpha-tint, opaque copy, alpha copy
  - an axis-aligned rectangle of 6 triangle vertices or a 4-vertex strip, from client arrays
  - no blend, or CA's premultiplied ONE / ONE_MINUS_SRC_ALPHA
  - a 2D texture with clamp-to-edge, level 0, NEAREST or LINEAR
  - drawn into a texture-backed framebuffer

  Anything else goes to softpipe as before.
- **How:** `fastpath.c` maps the textures with `pipe_texture_map_3d`, which makes softpipe flush its tile cache first and mark the texture as changed afterwards. It then runs a per-pixel loop with separable texel tables.
- **Modes** (`SGLFAST` in sweetenv):
  - 0 or unset: only counts how many draws would qualify, why the others don't, and their softpipe time
  - 1: the CPU path draws them
  - 2: verify mode. softpipe still draws; for the first 400 qualifying draws the CPU result is compared with softpipe's, including a 1-pixel border, and up to 60 `fastverify` mismatch lines are logged.
- **Log lines:**
  - `sweetgl: fast mode M: N draws; fill … copy … : X ms in softpipe, Y ms here; not: …; verify ok A bad B`, every 10 s
  - `sweetgl: fastverify kind … rect … px differ …`
- **Checked locally** (bb529ae): all of `eagl.c` and `fastpath.c` compile with stand-in Apple headers. The two Mesa calls (`_mesa_get_current_context`, `_mesa_lookup_texture`) exist in Mesa e24dc5bd, and the 7 new GL calls are GL 2.0 core functions.

## Next steps

1. **Build.** CI build of bb529ae, then check:
   - the size is at most 17208704
   - the signature ends at EOF
   - the strings `fast mode`, `fastverify` and `slow frame` are there
2. **Install.** Install with `OLD=da79097d…`. Expect a metadata refresh round, because iOS has booted since the 75b34d3 install.
3. **Verify run.** Boot with `args-fast2.txt`. Check the qualifying counts, the reasons draws don't qualify, and whether there are any `fastverify` mismatches.
4. **Speed run.** Only if the verify run is clean, boot with `args-fast1.txt` and compare the `slow frame` and `WP210 AUTO` numbers with cbf8881a.
5. **Later:**
   - run the fast path on several A78 cores
   - support REPEAT wrap and vertices from buffer objects, if the reason counts show they matter
   - fbfetch programs on the CPU
6. **Lower priority:**
   - the 0x502 setup errors
   - the launchd and mediaserverd spin
   - the A55 MHz reading
   - llvmpipe as the long-term renderer

## Rules followed

- **No blind writes:** nothing is written to the phone before the `--check` output has been reviewed and the write confirmed.
- **No bypassing the metadata check:** its hashes are refreshed only after the changed blocks are identified (and node 19204 is confirmed unchanged).
- **Keep backups** until a new install is verified working.
- **Commit after cause:** shim behaviour changes only once a cause is known. The fast path is behind `SGLFAST`, and the default is count-only.

## Files in this branch

- `notes/scripts/`:
  - install scripts for BGRA, 0x502, 22ec055, d17061a, fedeca3, 41be079 and 75b34d3
  - `check-metadata-readonly.sh` (latest version: hashes from the last check before the 75b34d3 install)
- `notes/tools/`:
  - `pull-sweetlog.bat`
  - `resolve-callers.sh`
  - `prof.py`
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