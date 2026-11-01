# Handoff: ios-sweet → Galaxy A8 (2018), `jackpotlte` (SM-A530F, Exynos 7885)

Written 2026-11-01 at the end of a claude.ai session. Read this, then `docs/PORTING_JACKPOTLTE.md` (hardware facts,
all verified on the user's phone) and `AGENTS.md`. Upstream base commit: `45263be` (dipa-studio/ios-sweet).

## Goal
Boot Apple's XNU (iOS 16.7.16 iPhone X userland) on the user's own Galaxy A8 (2018). The user has the phone,
rooted debugging, TWRP, an unlocked bootloader and a full partition backup. The Redmi Note 10 Pro (`sweet`) port
in this repo is the reference; a Snapdragon S9 (SM-G9600) port exists in `PatoKuaKua2711/ios-s9`
(Samsung command-mode AMOLED display fix = its patch 0095 + `sweetfb_hz=60`; boots by `dd` of the shim into `boot`
from TWRP). Treat third-party forks as untrusted reading material; do not run their code unreviewed.

## State at handoff
- Done (not yet run on hardware): `tools/mkadt.py --device jackpotlte` (GICv2, 8 real MPIDRs, 26 MHz timer, 2 GiB
  upper DRAM bank, S-Boot framebuffer `0xEC000000` 1080×2220, `sweet-mmc` node); `shim/a8/` milestone-1
  diagnostic shim; `tools/mka8boot.py` (wraps an Image into a Samsung boot.img using the phone's own `BOOT`
  backup as template); `docs/PORTING_JACKPOTLTE.md`.
- **`tools/mkadt.py` has only been syntax-checked** (the claude.ai sandbox refused to execute repo Python). First
  task: run it for `--device sweet` with a fixed seed before/after the change and confirm the sweet ADT is
  byte-identical (`build(seed=b'\0'*64, highmem=...)`), then run `--device jackpotlte --dump`.
- Diagnostic image: `shim/a8/build.sh` then `python3 tools/mka8boot.py BOOT.img shim/a8/out/Image out.img`.
  The image that was handed to the user has SHA-256
  `19d11dfeac68f7cc46f87d108178b306a65ad39a4b86388d2668c3fc1480396e` (32 MiB, kernel 43328 B).
- The user had pushed that image to `/sdcard/` and verified the checksum, and was about to `dd` it to
  `/dev/block/by-name/BOOT`. **Whether it was flashed, and what the screen showed, is unknown: ask the user.**
  Expected: blue screen with text (EL, MPIDR, MIDR, FDT info, initrd range, GICD/GICC identity, colour bars,
  PSCI version). Red = unexpected exception (ESR/ELR/FAR shown). Black = display needs the DECON trigger.

## Rules (from AGENTS.md, plus device-specific)
- Never write `mmcblk0boot0/1` (S-Boot lives there) or `EFS`, `CPEFS`, `m9kefs*`, `PERSISTENT`, `PARAM`, `RADIO`, `CM`, `NA`.
  Only `BOOT` (and later `USERDATA`) are ever flashed. Confirm each destructive step with the user.
- Keep dumps, serial numbers and IMEI-bearing partitions out of git. The live FDT and `bootargs` contain the
  serial; use the redacted copy under `research/a8/`. The user's partition backups stay on their PC.
- Recovery path for a bad BOOT: force off (Power + Volume Down ~8 s), TWRP (Power + Bixby + Volume Up),
  `dd` the saved `BOOT.img` (SHA-256 `c87a1336e8e6ca031b402cce84add38c9ec24172e3293de868cd7b05d6c9a59d`) back.
  Download mode is Power + Bixby + Volume Down. Stock firmware for Odin can come from SamFw (user has the tool);
  never flash an older bootloader than `A530FXXULCUK6`.
- Update this file / `NOTES.md` after each milestone (AGENTS.md convention) and distinguish observations from guesses.

## User environment
- Windows 11 PC, ADB in `C:\Users\Darin\Downloads\ONN\Vid stuff\platform-tools-latest-windows (2)\platform-tools`.
  Use **cmd.exe, not PowerShell**, for anything with `>` on binary data; use `adb shell -n` in pasted multi-line
  blocks (plain `adb shell` swallows the following lines); `adb root` works, there is no `su`.
- Phone: SM-A530F EUR, hardware revision 7, bootloader `A530FXXULCUK6`, `verifiedbootstate=orange`, Knox
  warranty bit already set. Running a community Android 16 (LineageOS 23.2 GSI) with kernel
  `4.4.302-lts-Nameless-jackpotlte` (its source tree is not yet identified; ask the user for the link: it holds the
  DECON/DSIM/`sec_ts`/dw_mmc drivers). `ro.product.model` shows SM-A515F (ROM spoof); real model is in
  `androidboot.em.model`.
- Backups on the user's PC (from `/sdcard/a8bak`, checksums in `research/a8/SHA256SUMS`): BOOT, RECOVERY (TWRP,
  not received), EFS, CPEFS, m9kefs1-3, PERSISTENT, PARAM, NA, CM, RADIO, STEADY, MISC, mmcblk0boot0/1.

## Toolchain notes
- `clang --target=aarch64-none-elf` + `ld.lld` + `llvm-objcopy` build the A8 shim (no GNU cross gcc needed).
  `shim/console.h` now honours `-DFB_BASE/-DFB_W/-DFB_H` overrides; without them sweet's values apply. A past bug:
  the override was silently ignored until `console.h` had `#ifndef` guards — check the build for
  `macro redefined` warnings.
- The kernel itself builds only on GitHub macOS runners (`xnu.yml`, blacktop/darwin-xnu-build). The user must fork
  the repo; there is no way to build XNU in a Linux sandbox. iOS root image: README step 2 (iPhone10,3 IPSW 16.7.16 20H392).

## Next steps, in order
1. Get the diagnostic result (photo). If black: build the DECON trigger / TE-paced present path. Reference: S9
   port patch 0095 and the 4.4 Samsung DECON driver (`decon_f`, `psr_mode=2`, `trig_mode=0`, TE pinctrl `0x139B0A38`).
   S-Boot leaves DSIM running ("PLL is stabled in bootloader") and the framebuffer holds two 1080×2220 32 bpp buffers.
2. GICv2 in the shim: port `shim/shim.c`'s GICv3 timer IRQ self-test to GICD `0x12301000` / GICC `0x12302000`,
   virtual-timer INTID 27 (PPI 11), 26 MHz. Then PSCI `cpu_on` (`0xc4000003`, `smc`, v0.1 IDs from DT) for the 7
   other cores with the MPIDR table in `mkadt.py` (`JP_CORES`).
3. DW-MSHC eMMC driver (`0x13500000`, v270a, 64-bit IDMAC, HS400/enhanced strobe/CMDQ were enabled by Android;
   check whether S-Boot leaves the controller usable, otherwise full init with the `samsung,dw-mshc-*` timings).
   First read-only in the shim, then the XNU root-device driver replacing `bsd/dev/sweet_ufs.c` (patches 0028–0034, 0069).
4. XNU patches: GICv2 interrupt controller, SMP MPIDR map (0065), ARMv8.0 (A53/A73) machine config, GPIO keys
   (`gpa1` 5/6/7 at `0x11CB0000`), `sec_ts` touch on `i2c@13870000` addr `0x48` (Samsung protocol, fw v0.0.2.20),
   panel present path. Rename the `xiaomi,sweet` compatible fallback in `mkadt.py` together with the platform expert.
5. Payload placement: S-Boot puts the boot.img ramdisk at `0x89000000` and reports it in `/chosen`, but `BOOT` is only
   32 MiB; if shim + kernel + ADT do not fit, load the payload from eMMC. The upper bank `0x880000000`–`0x8ffffffff`
   (2 GiB) has no carve-outs; iPhone X userland was built for 3 GB, expect memory pressure.
6. Live debug channel: sweet's USB CDC-ECM live log (patches 0072–0077, `tools/sweet-usbnet.sh`) is the model;
   A8 USB is DWC3 at `0x13600000` (Exynos wrapper init needed). Until then: framebuffer console + persistent
   eMMC log ring read from TWRP.

## Data not in git (`research/a8/`, git-ignored)
`dtb4-rev07-static.dts` (decompiled Samsung DTB from the user's boot.img, rev07), `live-redacted.dts` (live FDT with serial
values removed), `iomem.txt`, `partitions.txt`, `byname.txt`, `dmesg_filtered.txt`, `props.txt`, `SHA256SUMS`,
`bootinfo.py` (header/section splitter used to take the boot.img apart; stdlib only), `a8-diag-boot.img`.
