# WP210: iOS 16 on the Oukitel WP210 Pro (MediaTek MT6895)

A port of this project's XNU kernel and iOS 16.7.16 userland to the
**Oukitel WP210 Pro**: MediaTek Dimensity 8200 class SoC (platform MT6895),
4x Cortex-A55 + 4x Cortex-A78, Mali-G610 GPU, 12 GB RAM, 512 GB UFS 3.1,
1080x2412 Novatek NT37705 AMOLED (DSI command mode, dual display pipe).

## Status

| Area | State |
|---|---|
| Boot | Own loader (`loader/`) in a boot v4 image on slot A; LK requires a **gzip** kernel |
| Watchdog | MediaTek TOPRGU at `0x1C007000` disabled by the loader (a watchdog reset puts LK into dump mode) |
| Display | Boot framebuffer `0xFE1AF000` (3 LK buffers of 1088x2432, panel stride 1080, ARGB8888). Frames go out after ungating the MMSYS clocks and pulsing **both** pipes' mutex EN (`0x14001020`, `0x14401020`) and DSI_START (`0x14017000`) |
| CPUs | All 8 cores via PSCI (`smc`), as `qcom,kryo` for the SweetCPU driver |
| PAN | ARMv8.1+ cores: SCTLR_EL1.SPAN = 1 and PAN cleared at boot (patch 0187), else `copyin` faults forever |
| Storage | MediaTek UFSHCI at `0x112B0000`, partitions on **LUN 2**. LK leaves the link down: XNU re-enables the host, resets the device, does link startup + fDeviceInit, then HS-Gear 4 (~1.35 GB/s reads) |
| iOS root | 20 GiB unpartitioned region after a shrunk `userdata` (the GPT has exactly 71 entries, all used) |
| Userland | Real `launchd` and dozens of iOS daemons run; `backboardd` starts |
| Graphics | No GPU driver. Work in progress: `sweetgl` on Mesa **softpipe** (CPU rendering) for `backboardd` |

## Kernel patches (`xnu/patches/`)

| Patch | What |
|---|---|
| 0184 | Early console on the MediaTek panel: clock ungate, dual-pipe mutex + DSI kick, draws into all 3 boot buffers |
| 0185, 0186 | Debug: `wp210hb=1` heartbeat printing PID 1's threads, user state and a kernel backtrace every 5 s |
| 0187 | PAN on ARMv8.1+ (SPAN = 1, PAN cleared at start) |
| 0188 | `ufslun=` (LUN 2 here) and `ufsprobe=1` (bring up the UFS driver without rooting on it) |
| 0189 | `ufsmtk=1`: MediaTek refclk request, Hibern8 exit, register diagnostics |
| 0190 | `ufsreinit=1`: host re-enable, device reset (SiP call), pre-link settings, DME_LINKSTARTUP, NOP OUT, fDeviceInit |
| 0191 | `ufsgear=1..4` high-speed power mode; `ufsbench=1` data peek + read benchmark |
| 0192 | `ufsfirst=`/`ufscount=`: root on a raw block range, refused if it overlaps any GPT partition |
| 0193 | AppleCLCD frames copied into all LK buffers, then pushed to the command-mode panel |

Builds with the existing `xnu` workflow (`runs-on: macos-15-intel`: the macos-14 image was retired on 2 Nov 2026 and macOS arm64 runners were often unavailable).

## Device tree

`tools/mkadt.py ADT --wp210`: CPUs, GICv3 (`0x0C000000` / `0x0C040000`, stride `0x20000`), virtual timer INTID 27, 13 MHz timebase, RAM `0x1C0000000` + 4 GB (a carve-out-free block of the 12 GB), the framebuffer and MediaTek display registers (`mtk,*` properties in `sweet-fbcon`), the UFS host, and the iPhone 8 identity (`/product` from `research/ipsw-8/d20ap.adt`, extracted from the IPSW's `DeviceTree.d20ap.im4p`).

## Building a boot image

Needs the kernel (`xnu` workflow artifact), the ADT and the stock `boot.img` of the same firmware (`OUKITEL_WP210_Pro_EEA_V11_20251201`, its ramdisk is reused). Linux or WSL with `gcc-aarch64-linux-gnu`.

```sh
python3 tools/mkadt.py wp210.adt --wp210
python3 tools/mkpayload.py kernel.development.sweet wp210.adt payload --cmdline "$ARGS"
cd wp210/loader && ./build.sh STOCK_BOOT.img ../../payload wp210-boot.img
fastboot flash boot_a wp210-boot.img && fastboot set_active a && fastboot reboot
```

Back to Android: `fastboot set_active b`, `fastboot flash boot_a boot.img` (stock). Android on slot B is never touched.

Boot-args used for the iOS root:

```
debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 rd=ufsroot0 ufsroot=ios ufslun=2
ufsfirst=119713536 ufscount=5243128 ufsmtk=1 ufsreinit=1 ufsgear=4 ufsrw=1 rootrw=1
unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1
cs_enforcement_disable=1 sweetusb=0 sweetkeys=0 sweetperf=0 sweetclk=0 sweettouch=0 sweetgpu=0
sailfishwlan=0 sweetlog=0 sweetbattery=0 sweetsample=- sweetprof=-
```

The Qualcomm-only drivers of the Redmi/Pixel ports are switched off (`sweet*=0`).

## The iOS root

1. **Image** (Linux/WSL): iPhone 8 IPSW `iPhone_4.7_P3_16.7.16_20H392`; OS = `087-86684-044.dmg`, SystemOS = `087-86580-047.dmg`, AppOS = `087-86715-047.dmg` (from `BuildManifest.plist`, `d20ap`, Erase). `tools/mkiosroot.sh OS SYSTEMOS APPOS ios16-root.img` (16 GB), then move `private/var/staged_system_apps/*.app` to `/Applications`. WSL has no HFS+ driver: build `fs/hfsplus` as a module from Microsoft's WSL2-Linux-Kernel at the running version (`make vmlinux`, then `cp vmlinux.symvers Module.symvers`, `make M=fs/hfsplus modules`, `insmod`).
2. **Space**: `userdata` (GPT entry 70 on LUN 2, 4 KiB sectors) shrunk from 3553280-124956663 to **3553280-119713535**, same name, type and unique GUID, by editing only that entry's last LBA and the CRCs of the primary (LBA 1-4) and backup (LBA 124960760-124960767) tables. `sgdisk` refuses to write this table (its header's first-usable LBA, 34, is a 512-byte-era value past `misc` at LBA 8). Then a factory reset. Keep backups of the original GPT.
3. **Write** (rooted Android): `dd if=ios16-root.img of=/dev/block/sdc bs=1M seek=467631 conv=fsync` (block 119713536 = MiB 467631), and check it by reading it back.

## Graphics (in progress)

`tools/sweetgl/build-sw.sh` builds `sweetgl.dylib` with `-DSWEETGL_SOFTPIPE`: Mesa softpipe instead of freedreno, and `sweet_sw.c` instead of the msm emulation (IOSurfaces become softpipe display targets over their own memory). Built by the `sweetgl-sw` workflow against Mesa from `github.com/mirror/mesa` (commit `e24dc5bd`, 26.2-devel). It is injected into `backboardd` with `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`.

## Bring-up tests (`shim-tests/`)

The bare-metal shim used to find the boot format, watchdog, framebuffer, panel kick and buffer layout, with a small framebuffer console. `make STOCK_BOOT=boot.img` (needs `-DUNGATE_KICK -DDUALPIPE -DCONSOLE`, set in the Makefile).

## Not in this repository

Vendor firmware (`boot.img`, `vendor_boot.img`, DA/preloader files), Apple's IPSW and anything built from it (`ios16-root.img`, `d20ap.adt`), and the boot images (they carry Oukitel's ramdisk).
