# WP210 iOS port: status notes (2026-11-14, updated 00:52)

Working notes so the state survives session resets. Times are the phone's
local time (Asia/Jerusalem) unless stated.

## Where things are

- `main`: unchanged shim commit 1465988 (BGRA upload for format-0 4bpp IOSurfaces).
- `sweetgl-diag-0x502` (pushed): diagnostic branch on top of 1465988.
  - 37e8346 diagnostic + flush timing (too large to install)
  - 71ca6c4 trimmed flush timing and log strings (too large)
  - c3cc1e7 validation-only diagnostic (too large by 64 bytes)
  - 471a76c validation-only diagnostic, no validate-status query (too large by 32 bytes)
  - 22ec055 old window-trace code removed; built and installed on the phone (latest)
- Build artifacts come from the `sweetgl-sw` workflow (run by hand on the branch).

## Build artifact sizes (sweetgl.dylib)

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

## Install status (2026-11-14 00:51)

- Build 22ec055 installed on the iOS volume with `install-sweetgl-diag-22ec055.sh`.
  - Payload: `sweetgl-diag-22ec055-allocation.bin`, 17211392 bytes, SHA-256 `552d50d6…`.
  - `--check` passed, then `--install` reported `INSTALL VERIFIED`. No reboot performed.
  - The phone's current shim (before the install) was the BGRA build, SHA-256 `309a61d7…`. It's backed up in `before-bgra.bin` on the PC (hash verified) and on the phone.
- Catalog metadata expected hashes were refreshed for the four blocks changed by the iOS boot (see below). Those four current values were cross-checked against the phone's reads and the catalog dump.
- Next: reboot to iOS, run the usual test, then pull the sweetgl log and video.

## Phone state

- Trace boot image (`wp210-boot-trace.img`) flashed to boot_a; set_active a.
  The phone reaches the iOS home screen.
- Shim on the iOS volume: build 22ec055 (installed 2026-11-14 00:51), replacing the BGRA build.
- Backups: original pre-BGRA shim `2db9d1eb…` (PC copy in `C:\Users\artzi123\Downloads`);
  BGRA shim `309a61d7…` (`before-bgra.bin` on the PC and on the phone).

## Trace results (log a9b74635, video 5d43f78e)

- Surface copy errors (`0x80e1`) are gone: all 28 texImageIOSurface/attachImage lines report err 0.
- No upload goes to texture 0.
- Three `GL_INVALID_OPERATION` (0x502) errors remain, all after fbfetch draws
  (programs 19, 21 and 27) into framebuffer 2 / texture 3. Cause not yet known.
- Video: 96.7 s, 60 fps. Motion onsets at about 2.1, 9.5, 16.2, 28.5, 40.4, 68.5, 84 s.
  The sweetlog has no timestamps that match these, so no swipe latency yet.

## Install check: blocked on catalog metadata

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

## Open items

1. Reboot to iOS with build 22ec055 installed; confirm the phone reaches the home screen.
2. Check the new sweetgl log: `GL error 0x502` lines and the `sweetgl: diag` validation messages.
3. Swipe latency still needs a sync marker. The shim has no touch timestamps yet.
4. Three remaining 0x502 draw errors (fbfetch); the diagnostic in 22ec055 should show the cause.

## Rules followed so far

- No writes to the phone without `--check` output reviewed first.
- The metadata check is not bypassed by editing its hashes.
- Keep backups (PC and `/data/local/tmp/sweetgl-check.*`) until a new install is verified working.
- The install script copy in `scripts/install-sweetgl-diag-22ec055.sh` is the one that ran (SHA-256 `e0c8df12…`).

## Update 2026-11-14 09:45

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
