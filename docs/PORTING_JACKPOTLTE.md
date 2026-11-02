# Porting ios-sweet to the Galaxy A8 (2018) — `jackpotlte` (SM-A530F, Exynos 7885)

Status: **scaffold only, never booted.** `tools/mkadt.py --device jackpotlte` generates a device tree from
facts read out of a real `boot.img` (below). Nothing has run on hardware yet.

Fact sources, best first:
1. **Samsung downstream DTBs** inside the user's `boot.img` (5 trees, `JACKPOTLTE EUR rev04…rev07`, `samsung,Universal7885`).
   They agree on everything listed under "Verified". S-Boot patches the tree at boot (RAM size, MAC, reserved ranges),
   so the phone's *live* FDT remains the final word.
2. Mainline Linux `exynos7885.dtsi` / `exynos7885-jackpotlte.dts`.

## Verified from the boot.img DTBs

| Area | Value |
| --- | --- |
| Boot image | Android boot image, legacy header (page 2048), raw arm64 `Image` (text_offset `0x80000`, flags `0xa`), gzip ramdisk, Samsung **`DTBH`** bundle of 5 DTBs after it. Kernel is the community `4.4.302-lts-Nameless-jackpotlte` build |
| CPUs | PSCI over `smc` (`cpu_on 0xc4000003`). A53 MPIDR `0x100–0x103`, `0x200–0x201`; A73 `0x0`, `0x1`. Logical IDs 0–5 are the A53s, 6–7 the A73s |
| Interrupts | **GICv2** (`cortex-a15-gic`): GICD `0x12301000`, GICC `0x12302000` (0x1000), GICH `0x12304000`, GICV `0x12306000` |
| Timer | `arm,armv8-timer`, 26 MHz (`0x18cba80`). PPIs 13/14/11/10 → virtual timer INTID **27** |
| RAM | 4 GB unit (user-confirmed): `0x80000000`+`0x3da00000`, `0xc0000000`+1 GiB, `0x8_80000000`+**2 GiB**. The boot.img DTB lists only 1 GiB in the upper bank; S-Boot extends it at boot (mainline also says 2 GiB). XNU gets the 2 GiB upper bank; the iPhone X userland was built for 3 GB, so expect memory pressure |
| Reserved | Framebuffer `0xEC000000` (0x1400000), modem `0xF5400000`, cp log `0xFDC00000`, gnss `0xFFC00000`, TIMA `0xB8000000`, seclog `0xC0000000`, wifibt `0xE9000000`, abox `0xE9400000`. **The upper bank (`0x880000000`) has no carve-outs in any revision** |
| Storage | **eMMC**: DW-MSHC `0x13500000` (+ CMDQ window `0x13508000`), IRQ SPI 0x92, 8-bit, HS400 + enhanced strobe, CMDQ. Android's `SYSTEM`/`VENDOR` are on `13500000.dwmmc0/by-name/` |
| Display | Panel **`s6e3fa7_fhd`**: 1080×2220, 60 Hz, 4 lanes, no DSC, **DSI command mode**. DSIM `0x14870000`, DECON `0x14860000` (`psr_mode=2`, `trig_mode=0` = hardware TE trigger, TE pin via pinctrl `0x139B0A38`). Panel power/reset are GPIO sequences in `decon_board` |
| Touch | Samsung `sec,sec_ts` at I²C address `0x48` on `i2c@13870000` (~400 kHz), IRQ gpio `gpa0-0`, firmware `tsp_sec/y661_a5y18.fw`, 4096×4096 coordinates |
| Buttons | Plain GPIO on `gpa1`: volume-down 6, volume-up 5, power 7 (`0x5C` = gpa1, pinctrl block `0x11CB0000`) |
| PMIC | `samsung,s2mpu08mfd` |
| USB | DWC3 at `0x13600000` |
| Kernel cmdline | `console=ram … androidboot.hardware=samsungexynos7885 … ess_setup=0x86000000`; initrd at `0x84000000` |
| Kernel EL | No RKP/`uh_call` code in this kernel; it supports both EL1 and EL2 entry |

## sweet vs jackpotlte — what has to change

| Area | sweet | jackpotlte | Work |
| --- | --- | --- | --- |
| Interrupts | GICv3, `ICC_*`/`GICR_*` | GICv2 (above) | **New GICv2 driver** in shim and XNU (patches 0016–0018 area) |
| Timer | 19.2 MHz, INTID 19 | 26 MHz, INTID 27 | constants (ADT already updated; shim has `VTIMER_INTID`) |
| SMP | MPIDR = core<<8 | table above | patch 0065 logical→MPIDR map |
| Storage | UFS | eMMC DW-MSHC | **New root-device driver** replacing 0028–0034/0069; new read path in the shim |
| Display | video mode, continuous splash | command mode, HW TE trigger | Samsung-AMOLED present path: see the S9 (SDM845) port, which solved the same "frame every second TE pulse" problem (its patch 0095, `sweetfb_hz=60`) |
| Touch | Goodix over GENI | `sec_ts` over Exynos HSI2C-class I²C | new driver (Samsung's protocol; source is in the 4.4 kernel tree) |
| Buttons | PMIC/SPMI | GPIO | simple; rewrite 0060's key half |
| Boot | `fastboot boot` | S-Boot: flash `boot` from TWRP (`dd`) or Odin | see below |

## What exists in this tree

`tools/mkadt.py --device jackpotlte [--fb ADDR[,WxH]] [--mem-size N]` emits the GIC-400 node (4 reg pairs,
virtual-timer-intid 27), 8 CPU nodes with the real MPIDRs and 26 MHz timebase, a `sweet-mmc` node, the upper
DRAM bank, the S-Boot framebuffer (`0xEC000000`, 1080×2220) and `samsung,jackpotlte` / `samsung,exynos7885` in
the compatible list. `xiaomi,sweet` stays last so the existing platform expert matches until it is renamed.
`--device sweet` (default) is unchanged.

## Next steps, in order

1. Shim: GICv2 init + timer IRQ self-test; Exynos MPIDR table; arm64 `Image` header with text_offset `0x80000`
   so S-Boot's loader accepts it; DW-MSHC read-only driver to fetch the payload/root.
2. XNU: GICv2 interrupt controller, eMMC root device, SMP MPIDR map.
3. Display: keep the S-Boot framebuffer alive and drive DECON's TE trigger (borrow from the S9 port).
4. Input: GPIO keys, then `sec_ts`.
5. Logging: keep the framebuffer console plus a persistent log ring on eMMC, read back from TWRP.

## Verified on the phone (live FDT + dmesg + /proc, SM-A530F EUR, hardware revision 7)

- The unit is an SM-A530F (`androidboot.em.model`). `ro.product.model` reads SM-A515F because the ROM spoofs it.
- **Kernel entered at EL1** (`CPU: All CPU(s) started at EL1`). PSCI is v0.1 with function IDs from the DT (cpu_on `0xc4000003`, `smc` conduit).
- Live FDT = static rev07 DTB with three changes: **upper RAM bank is 2 GiB** (`0x880000000`–`0x8ffffffff`), `chosen` is filled in, and `linux,initrd-start` = **`0x89000000`** (end `0x890d6933` for that boot's ramdisk). So S-Boot loads the boot.img ramdisk at `0x89000000`, in the low bank.
- RAM total 4155392 KiB. `/proc/iomem`: System RAM `80000000-bd9fffff`, `c0000000-ffffffff`, `880000000-8ffffffff`.
- Framebuffer handover confirmed: `s3cfb.bootloaderfb=0xec000000`, size `0x1400000` (two 1080×2220 buffers, 32 bpp). **DSIM is left running** ("PLL is stabled in bootloader, so skip DSIM link/DPHY init"); panel `s6e3fa7`, command mode.
- eMMC: Samsung `BJTD4R`, 29.1 GiB, HS400 + enhanced strobe + CMDQ, DW-MSHC v270a, 64-bit IDMAC, 64-deep FIFO. A **second** DW-MSHC at `0x13550000` drives the SD slot (no SMU).
- Partition table (eMMC `mmcblk0`): BOOT `p12` (32 MiB, **the whole boot.img incl. kernel, ramdisk and DTBs must fit**), RECOVERY `p13` (38 MiB), EFS `p3`, USERDATA `p29` (~20.3 GiB), SYSTEM `p19`, VENDOR `p9`. The bootloader lives in `mmcblk0boot0/1` (4 MiB each): never write them.
- Bootloader unlocked (`verifiedbootstate=orange`) and Knox warranty bit already set (`warranty_bit=1`), so there is nothing further to trip.
- Touch is `sec_ts` firmware v0.0.2.20 on `i2c@13870000`; GIC distributor/CPU interface are not listed in `/proc/iomem` but the DT addresses were used by the running kernel.

## Design consequences

- Payload loading: S-Boot places the ramdisk at `0x89000000` and reports it in the FDT, so the shim can read its payload there as ios-sweet does — but BOOT is only 32 MiB. If kernel + ADT + shim exceeds that, load the payload from eMMC with the DW-MSHC driver (e.g. from RECOVERY or a `userdata` offset).
- Kernel image goes at `0x80080000` (text_offset `0x80000` from the 2 MiB-aligned DRAM base).
- Keep the S-Boot framebuffer and DSIM/DECON untouched; only add the command-mode TE-paced present path.

## Still UNVERIFIED

- Whether DW-MSHC is left in a state the shim can reuse (HS400 timing, CMDQ) or must be re-initialised from scratch.
- GICv2 register state at handover (the kernel enables it itself, so assume clean).
- ~~Whether S-Boot accepts a non-Linux `Image`~~ **Verified 2026-11-01:** it enters the shim (arm64 Image header,
  text_offset `0x80000`, flags `0xa`). PMU SWRESET (`0x11C80000 + 0x400` = 1) reboots the SoC.

## Boot and recovery (read before flashing anything)

- Back up first: stock firmware for Odin, plus the `EFS`/modem partitions (IMEI/radio data; not restorable from firmware).
- Flash only `boot` (and `userdata` for the iOS root). Never `sboot`/`param`/modem/EFS.
- As with sweet, putting the iOS root in `userdata` erases Android's user data.

## Milestone 1: diagnostic boot image (`shim/a8/`)

`shim/a8/build.sh` builds a tiny position-independent arm64 Image (clang + lld; no GNU cross toolchain) that
prints, on the S-Boot framebuffer (`0xEC000000`, 1080×2220): exception level, MPIDR/MIDR, SCTLR, ID registers,
CNTFRQ, the FDT pointer/magic/size, the ramdisk range S-Boot reports, the GIC-400 distributor/CPU-interface
registers, colour bars, and finally a PSCI_VERSION `smc` (last on purpose: if it hangs everything above is
already on screen). Any unexpected exception paints a red screen with ESR/ELR/FAR. It touches no storage.

`tools/mka8boot.py TEMPLATE_BOOT.img Image OUT.img` wraps it in a Samsung boot image, reusing the phone's own
`BOOT` backup as template (legacy v0 header, page 2048, cmdline and the `DTBH` bundle byte for byte), padded
to the 32 MiB `BOOT` partition. Its id formula reproduces the template's own SHA-1 id.

Flash from TWRP (backup first!): `dd` the image to `/dev/block/by-name/BOOT`, read it back and compare SHA-256, reboot.
Recover: force power off (Power + Volume Down ~8 s), boot TWRP (Power + Bixby + Volume Up), `dd` the saved `BOOT.img` back.
If the screen stays black the display path needs the DECON trigger (see "Display" above) and that is the next thing to build.
