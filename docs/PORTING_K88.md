# Porting ios-sweet to the ZTE K88 tablet (`jasmine`, MSM8952 / Snapdragon 617)

Status (2026-11-13): **facts gathered, diagnostic shim built, not run yet.** Nothing has been written to the tablet.

The plan reuses the Galaxy A8 work (`docs/PORTING_JACKPOTLTE.md`, `docs/HANDOFF_JACKPOTLTE.md`): the 4K-page,
ARMv8.0, GICv2 kernel variant (patches 0082-0091), the early framebuffer console and the shim/loader layout.
Target: iOS 16 first (all tooling reused); iOS 13 if 2 GB of RAM turns out to be the wall (Darin agreed).

## Sources and how each value is marked

- **live**: read from the tablet over adb on 2026-11-13, in TWRP (kernel `3.10.84-perf`, built 2017-12-20):
  `/proc/iomem`, `/proc/cpuinfo`, `/proc/cmdline`, `dmesg`, `/proc/partitions`, by-name links, fb0 sysfs, i2c/input
  lists. `tools/k88-facts.sh` collects them into the git-ignored `backup/k88/`.
- **dtb**: the board's DTB inside the `boot` partition (`backup/k88/boot.img`, SHA-256 `d7102cbe…a115`, 48 MiB).
  The kernel is `Image.gz` with **32 DTBs appended** (no `dt.img`, header `dt_size` 0); the one for this board is
  `model = "Qualcomm Technologies, Inc. MSM8952 JASMINE"`, `qcom,msm-id <0x108 0>`, `qcom,board-id <0x69 0>`
  (number 30 in file order, saved as `backup/k88/jasmine.dtb`). dmesg's `Machine: … MSM8952 JASMINE` confirms
  LK picked that tree.
- **guess**: not yet seen on this tablet; the diagnostic shim is meant to settle these.

The TWRP kernel has no `/proc/device-tree` or `/sys/firmware/fdt`, so the live (LK-patched) tree has not been
read. Reading it from booted Android (it runs, adb works) is a cheap follow-up.

## Facts

| Area | Value | Mark |
| --- | --- | --- |
| Device | ZTE K88, product `K88`, device `jasmine`, board `MSM8952`, Adreno 405. Boot partition header `os_version` 8.1.0 (the custom Oreo ROM); TWRP's build props still say 7.1.1 | live |
| CPU | 8 × Cortex-A53 r0p4 (MIDR `0x410FD034`), features fp asimd aes pmull sha1 sha2 crc32 (**ARMv8.0 crypto present**, unlike the unverified A8 case) | live |
| MPIDRs | `0x100–0x103` (DT `cluster1`) and `0x000–0x003` (`cluster0`), `enable-method = "psci"` | dtb |
| Boot CPU | Linux logical CPU 0 = first DT node, so very likely MPIDR `0x100`. Stock cmdline `boot_cpus=0,4,5,6,7` brings up only 5 cores ("SMP: Total of 5 processors activated"): a Qualcomm HMP choice, not a fault | guess / live |
| PSCI | `arm,psci-1.0`, `smc`; dmesg "PSCIv1.0 detected in firmware", "standard PSCI v0.2 function IDs" (CPU_ON `0xC4000003`, CPU_OFF `0x84000002`, VERSION `0x84000000`) | live |
| Exception level | Qualcomm LK enters the kernel at EL1 (a `hyp` partition holds the hypervisor) | guess |
| Interrupts | **GICv2** `qcom,msm-qgic2`: GICD `0x0B000000` (0x1000), GICC `0x0B002000` (0x1000). No GICH/GICV listed | dtb |
| Timer | `arm,armv8-timer`, **19.2 MHz** (`0x124F800`, dmesg "cp15 and mmio timer(s) running at 19.20MHz (virt/virt)"). PPIs in DT order 2, 3, 4, 1 → virtual timer = PPI 4 = **INTID 20** (A8: 27). MMIO timer frame at `0x0B120000` | dtb (INTID: guess) |
| RAM | 2 GB part: MemTotal 1894240 kB. System RAM `0x80000000–0x85AFFFFF`, `0x8B600000–0x8C1FFFFF`, `0x8D900000–0x8DCFFFFF`, `0x8F600000–0x9FCFFFFF`, **`0x9FF00000–0xFFFFFFFF`** (the big clean bank: 1.5 GiB, minus CMA pools Linux put at `0xF6C00000+`) | live |
| Carve-outs | other_ext `0x85B00000` (13 MiB), modem `0x86800000` (78 MiB), reloc `0x8C200000` (23 MiB), tzapp `0x8DD00000` (25 MiB), **splash `0x83000000` (20 MiB)**, ramoops `0x9FD00000` (2 MiB). Dynamic (CMA, Linux's choice, free for us): venus, secure, qseecom, adsp | dtb + live |
| Boot image | Android boot image v0, page `0x1000`, kernel_addr `0x80008000`, ramdisk_addr `0x81000000`, tags `0x80000100`. Kernel = gzip arm64 `Image` (text_offset `0x80000`, 24 MB inflated) + appended DTBs. Cmdline in the header: `console=ttyHSL0,115200,n8 … androidboot.bootdevice=7824900.sdhci … ramoops.mem_address=0x9fd00000 …` | dtb |
| LK cmdline additions | `boot_cpus=0,4,5,6,7`, `androidboot.verifiedbootstate=green`, `mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_boe_jd9366_boe_8p0_800p_video:1:none:cfg:single_dsi`, `sdlog.addr=0x90000000` | live |
| Display | Panel **BOE JD9366, 800 × 1280, 8", DSI video mode** ("mipi dsi video panel"); LK leaves **continuous splash** on ("Continuous splash enabled"), fb0 virtual 800 × 2560, 32 bpp, stride 3200 (Linux's buffer, not LK's). MDSS `0x01A00000` (0x90000), mdp-reg-offset 0x1000, SSPP RGB0/RGB1/DMA0/VIG0 at +0x15000/+0x17000/+0x25000/+0x5000, DSI0 `0x01A98000`. Video mode = the MDP scans continuously, **no trigger needed** (simpler than the A8) | live + dtb |
| LK framebuffer | Somewhere in the 20 MiB splash region at `0x83000000`; LK's own pixel format (usually 24 bpp) is not known yet: the shim reads it from the pipe registers | guess |
| Storage | **eMMC on sdhci-msm** (SDHCI 3.0 + Qualcomm core): hc_mem `0x07824900`, core_mem `0x07824000`, cmdq `0x07824E00`, IRQ SPI 0x7B (+ pwr 0x8A), 8-bit, HS400/HS200/DDR52, ICE crypto engine at `0x07803000`. `mmcblk0` = 15267840 KiB (14.6 GiB). SD slot `sdhci@7864000` | live + dtb |
| Partitions | `boot` = p20 (48 MiB), `recovery` = p21 (48 MiB, TWRP), `aboot` p18 (LK), `splash` p11, `cache` p36 (1 GiB), `system` p37 (3 GiB), `userdata` p38 (~10 GiB) | live |
| Touch | **Goodix** at I²C `0x5D` on `i2c@78B7000` (BLSP QUP, `qcom,i2c-msm-v2`, IRQ SPI 0x61), irq GPIO 65, reset GPIO 64 (input device "goodix-touchscreen"). Synaptics `0x20` and NXP NFC `0x28` nodes exist too (shared DT; Synaptics bound but appears unused) | live + dtb |
| Buttons | volume up = TLMM GPIO 85, volume down = GPIO 90 (active low, `gpio-keys`); power = PMIC PON (`qpnp_pon`) | dtb + live |
| PMIC / charger | PM8950 (SPMI arbiter `0x0200F000`, v2), PMI8950 class charger (`SMBCHG`), parallel SMB1351 at I²C `0x1D` on `i2c@78B6000`; fuel gauge in PMI8950 (FG) | live + dtb |
| USB | ChipIdea HS OTG `qcom,hsusb-otg` at `0x078DB000`, PHY CSR `0x0006C000` (not DWC3: a different USB device-mode driver than the A8 plan) | dtb |
| Reset | PS_HOLD `0x004AB000`; restart reason in IMEM `0x08600000 + 0x65C` (`qcom,msm-imem-restart_reason`), same scheme as sweet (`0x77665500` = bootloader) | dtb |
| Logging | Linux ramoops at `0x9FD00000`, 2 MiB, console 1 MiB: the same channel the A8 uses from TWRP | live |
| Fastboot | **Not usable on this tablet (Darin, 2026-11-13).** `adb reboot bootloader` just boots Android. So test images have to be written to `boot` from TWRP, like on the A8 | live |

## What carries over from the A8, what is new

| Area | A8 (jackpotlte) | K88 | Work |
| --- | --- | --- | --- |
| Pages / ISA | 4K, ARMv8.0 | same (A53) | `board=k88` variant reusing 0082-0091's `SWEET_JACKPOTLTE` paths; probably split that define into "4K/ARMv8.0/GICv2 class" and "board" |
| Interrupts | GIC-400 at `0x12301000` | GICv2 at `0x0B000000`/`0x0B002000` | ADT `/arm-io/gic` addresses; vtimer INTID 20 |
| Timer | 26 MHz, INTID 27 | 19.2 MHz, INTID 20 | constants |
| SMP | PSCI v0.1, MPIDRs `0x1xx/0x2xx/0x0x` | PSCI 1.0, `0x10x` + `0x00x` | `SWEET_MPIDR_VALID()` / `SWEET_BOOT_MPIDR` per board |
| RAM | 2 GiB bank at `0x880000000` | 1.5 GiB at `0x9FF00000–0xFFFFFFFF` (plus scattered low pieces) | XNU gets the top bank; userland built for 3 GB (iPhone X) will be squeezed harder than on the A8 |
| Display | command mode, DECON TE trigger | **video mode**, continuous splash | simpler: no present/kick; early console only needs FB address/stride/format |
| Storage | DW-MSHC PIO | **SDHCI** (standard spec) | new PIO driver behind 0093/0097/0103's BSD half; a well-documented controller |
| Root | ext4 file in USERDATA (0097) | same idea | `tools/ext4map.py` etc. reused |
| Touch | sec_ts | Goodix GT-series | sweet's Goodix code may be partly reusable (protocol differs from sweet's SPI part: check) |
| Keys | GPIO | TLMM GPIO 85/90 + PMIC PON | small |
| Boot | `dd` to BOOT from TWRP, `tools/mka8boot.py` | `dd` to boot (p20) from TWRP, **`tools/mkqcomboot.py`** | done (below) |
| GPU | Mali-G71 (panfrost) | Adreno 405 (freedreno, a4xx) | separate, later project |

## Diagnostic shim (`shim/k88/`, milestone 1+2 in one boot)

`shim/k88/build.sh` builds a position-independent arm64 Image (clang + lld, same as `shim/a8`), and
`python tools/mkqcomboot.py backup/k88/boot.img shim/k88/out/Image backup/k88/jasmine.dtb OUT.img` wraps it the way
the stock kernel is packed: gzip(Image) + the JASMINE DTB, v0 header with the stock addresses, page size, os_version
and cmdline, one zero page as ramdisk. LK inflates the kernel, finds the DTB after the gzip stream, patches it and
passes it in x0.

On screen (blue, 16 × 32 font, 47 columns):
- EL, MPIDR, MIDR, SCTLR, ID_AA64PFR0/MMFR0/ISAR0, CNTFRQ, the FDT pointer/magic/size and initrd LK reports;
- every MDSS pipe with a source address (both possible register bases), the chosen scanout address/stride and its
  format/unpack words; colour bars at the bottom, which should read red, green, blue, white from left to right;
- GICD TYPER/IIDR, GICC CTLR/IIDR, then a 300 ms virtual-timer IRQ window at 10 ms (expect ~30 IRQs, INTID `0x14`;
  if the INTID is wrong, the `pend=` word shows which PPI is pending) and a self-SGI;
- PSCI_VERSION (expect `0x00010000`), CPU_ON for the seven other cores, and the alive mask (bit = Aff1·4 + Aff0;
  `want=` is the expected mask);
- read-only SDHCI register dump (version, present state, host/clock control, capabilities, sdhci-msm core mode).
  No eMMC command is sent.

Drawing: the console draws 32 bpp into a shadow at `0xA0000000`. `k88_present()` packs it into LK's 24 bpp buffer
(or the shim draws directly if the pipe is 32 bpp). A red screen = unexpected exception with ESR/ELR/FAR.

Built 2026-11-13: Image 96,688 bytes; `k88-diag-boot.img` 233,472 bytes, SHA-256
`dbb5f23f4d553b84b6666f5c3e72e677778eda92b79d031b477749dee488ca2e` (out of git).

### Running it (needs Darin's OK: it writes the boot partition)

1. The PC already has `backup/k88/boot.img` (SHA-256 `d7102cbe4aff184b20192f10781b8b357eb12e317cf3604c494e448dd1c2a115`).
   Also keep a copy on the tablet (`/sdcard/k88bak/boot.img`).
2. In TWRP: push the image, check its hash, `dd` it to `/dev/block/bootdevice/by-name/boot` (= `mmcblk0p20`), read
   the first bytes back and compare, reboot.
3. Expected: the blue screen within a few seconds of the LK splash. Black or the splash staying = the shim did not
   start or drew somewhere else (photo either way).
4. Back to Android: force off (hold power ~10 s), boot TWRP with the button combination, `dd` the saved boot.img back.
   `recovery` (TWRP) is never touched, so this path always works as long as the button combination is known.

## Step list

1. ~~Facts over adb~~ (done, this file). Follow-up: dump the live FDT from booted Android (`/proc/device-tree` there)
   to see LK's memory/reserved-memory patches.
2. Run the diagnostic shim (above). It settles: EL, boot MPIDR, LK's framebuffer address and format, vtimer INTID,
   PSCI/SMP, SDHCI state.
3. `tools/mkadt.py --device k88`: GICv2 at `0xB000000`, 8 CPUs, 19.2 MHz, vtimer 20, DRAM bank, framebuffer, `sweet-mmc`
   (SDHCI) node, `zte,k88`/`qcom,msm8952` compatibles.
4. XNU: `board=k88` in `xnu.yml` (same 4K/ARMv8.0 paths as jackpotlte, new board constants: FB, MPIDRs, vtimer, RAM
   bank). Check the ISA count stays 0. The A53 here has the crypto extension, but the A8 settings (C SHA2) are safe.
5. Shim milestone 3: SDHCI read-only PIO (CMD13/CMD17), find GPT `userdata`; then milestone 4, the XNU loader with
   the payload linked in (`boot` is 48 MiB, plenty).
6. XNU eMMC driver (SDHCI PIO) behind 0093/0097's BSD half; HFS+ root as a file in `userdata` (ext4), RAM overlay first.
7. Keys (TLMM GPIO 85/90 + PON), Goodix touch on BLSP I²C `0x78B7000`.
8. Later: Adreno 405 GPU (freedreno), audio, Wi-Fi (WCNSS/pronto), USB device log (ChipIdea).

## Rules for this device

- Never write `aboot`, `sbl1`, `rpm`, `tz`, `hyp`, `modem*`, `fsg`, `persist`, `DDR`, `devinfo`, `recovery`, or the GPT.
  Only `boot` (and later a file inside `userdata`), and only with Darin's OK.
- Keep dumps (`backup/k88/`), the boot image and serials out of git. The kernel cmdline in dmesg carries the serial
  number; `tools/k88-facts.sh` redacts it in `cmdline.txt`, and `dmesg.txt` was redacted by hand.
