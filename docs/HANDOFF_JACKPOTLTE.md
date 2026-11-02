# Handoff: ios-sweet → Galaxy A8 (2018), `jackpotlte` (SM-A530F, Exynos 7885)

Written 2026-11-01 at the end of a claude.ai session. Read this, then `docs/PORTING_JACKPOTLTE.md` (hardware facts,
all verified on the user's phone) and `AGENTS.md`. Upstream base commit: `45263be` (dipa-studio/ios-sweet).

## Goal
Boot Apple's XNU (iOS 16.7.16 iPhone X userland) on the user's own Galaxy A8 (2018). The user has the phone,
rooted debugging, TWRP, an unlocked bootloader and a full partition backup. The Redmi Note 10 Pro (`sweet`) port
in this repo is the reference; a Snapdragon S9 (SM-G9600) port exists in `PatoKuaKua2711/ios-s9`
(Samsung command-mode AMOLED display fix = its patch 0095 + `sweetfb_hz=60`; boots by `dd` of the shim into `boot`
from TWRP). Treat third-party forks as untrusted reading material; do not run their code unreviewed.

## Update 2026-11-01 (Claude Code session on the user's PC)
- **Repo:** the user's fork is `darinnewman2011-cyber/ios-sweet` (remote `origin`; `upstream` = dipa-studio), work on
  branch `jackpotlte`. GitHub Actions is enabled on the fork. **Baseline `xnu.yml` run 41369167845** (manual,
  `jackpotlte` @ `5c7809b`, defaults: macOS 13.5 xnu, DEVELOPMENT, MACHINE_CONFIG SWEET, make -k): **success**, all
  steps green in ~6 min, artifacts `xnu-kernel` (~60 MB) and `xnu-buildtree` (~23 MB). The fork's pipeline works;
  no jackpotlte XNU patches exist yet (the run built sweet's kernel). No `gh` CLI on this PC: start runs from the
  Actions page, read status from the public REST API. Local toolchain: LLVM 23.1.4 (`C:\Program Files\LLVM\bin`, add to PATH) builds the shim; Python is
  `python` (Espressif's), not the WindowsApps `python3` stub. Keep Git Bash paths short (MAX_PATH) and use
  `MSYS_NO_PATHCONV=1` for `adb shell` with `/dev/...` arguments.
- **Observed:** the diagnostic image (SHA-256 `19d11df…0396e`) was `dd`'d to BOOT from TWRP; BOOT read back with the
  same hash. On reboot the screen **kept showing the S-Boot splash** ("SAMSUNG Galaxy A8 / powered by android"); no
  blue or red screen. **Guess:** the command-mode panel keeps its last frame (the splash) in panel RAM, so our
  framebuffer writes never reach the glass without a DECON trigger. It could also mean S-Boot never jumped to us.
- To tell the two apart, `diag.c` gained a proof-of-entry probe (`EXTRA_CFLAGS=-DA8_PROBE_RESET=4 shim/a8/build.sh`):
  after drawing it waits 4 s, then writes PMU SWRESET (`0x11C80400` = 1, the mainline exynos7 syscon-reboot value,
  unverified on this SoC). Reboot loop ⇒ the shim runs and the display trigger is the blocker. Splash that stays ⇒
  S-Boot did not start us, or something before the reset hung.
- **Observed (probe, boot image SHA-256 `5b02e66e…22bc8`, built with LLVM 23.1.4 on Windows):** the phone
  **reboot-loops** every few seconds. So: S-Boot accepts and enters our arm64 Image; the shim runs through all
  register reads, FDT parsing and framebuffer drawing without an exception (an exception paints red and halts, no
  reset); the generic timer counts; **PMU SWRESET `0x11C80400` = 1 resets the SoC** (now verified). The display
  is the only reason nothing was visible: the command-mode panel keeps the S-Boot splash until DECON sends a frame.
  Next: DECON trigger (step 1 below). The reset is also a usable 1-bit signal for later blind experiments.
- **Observed — DISPLAY WORKS (commit `608430b`, boot image `459ddbe7…eeed`, `-DA8_PROBE_RESET=10`):** blue text
  screen, colour bars in the right order (red, green, blue, white, so a8r8g8b8 is right), then the PMU reset. Step 1 is
  done. `a8_present()` = Samsung `decon_reg_start()`: GC |= EN|EN_F, SHADOW |= bit31, wait RUN, TRIG: clear
  MASK_DECON, set HW_TRIG_EN. Values read on the phone (S-Boot hand-over state):
  EL 1; MPIDR `0x80000100` (boot CPU = A53 `0x100`); MIDR `0x410FD034` (Cortex-A53 r0p4); SCTLR_EL1 `0x30C5183E`
  (MMU off, **alignment checking on**: keep `-mstrict-align`); PFR0 `0x2222` (GIC sysreg field 0: GICv2 only);
  MMFR0 `0x1122` (40-bit PA, 16-bit ASID); CNTFRQ `0x18CBA80`; DTB `0x8A000000` (magic OK, 0x2B082 bytes);
  initrd `0x89000000`; GICD_CTLR 0, GICD_TYPER `0xFCEF` (512 IRQ lines, 8 CPUs, security extensions),
  GICD_IIDR `0x0200143B` / GICC_IIDR `0x0202143B` (GIC-400), GICC_CTLR 1; DECON GLOBAL_CONTROL `0x133` (enabled,
  running, idle, I80/command mode), HW_SW_TRIG_CONTROL `0x1290` (**MASK_DECON set**: why the splash stayed;
  TRIG_AUTO_MASK_EN set; bit 9 set, undocumented), SHADOW 0, DPP0 IDMA_IN_BASE_ADDR_Y `0xEC000000` (buffer 0; the
  copy into the second buffer in `fb_mirror()` is not needed for this S-Boot). PSCI_VERSION was not called (probe build).
- **Observed — MILESTONE 2 PASSES (commit `138c504`, boot image `fe6d56aa…9a27`):** GICv2 interrupts and SMP work.
  GICD_CTLR 0 → 1 (NS EnableGrp1). GICD_ISENABLER0 was `0x7EFF` (S-Boot left SGIs 0-7, 9-14 enabled), now
  `0x08007EFF`. IPRIORITYR[27] reads back `0xA0`, GICC_PMR `0xF0`. **Virtual timer: 30 IRQs in 300 ms at a 10 ms
  period, all INTID 27 (0x1B), none stray**; before unmasking, ISPENDR0 = `0x08000000` and CNTV_CTL = 5 (ISTATUS).
  **Self-SGI 1: 1 IRQ**, ISPENDR0 `0x2` before unmask. **PSCI_VERSION = -1** (not supported: v0.1 firmware, use DT
  IDs only). **CPU_ON (`0xC4000003`, x1 = MPIDR, x2 = entry, x3 = index) returns 0 for all seven** (0x101-0x103,
  0x200, 0x201, 0x0, 0x1), and all seven ran `secondary_entry` (alive mask `0x3E3`, MPIDR-derived stacks) and
  called CPU_OFF (`0x84000002`). Both A73s and the second A53 cluster come up without extra cluster power-up.
  Step 2 is done; next is step 3 (DW-MSHC eMMC, read-only first).
- **Observed — MILESTONE 3 (eMMC read) PASSES (commit `059403b`, boot image `1237728a…40e7`):** S-Boot leaves the
  DW-MSHC usable as is; no re-init needed. State at hand-over: CTRL `0x10` (PIO, INT_ENABLE; no IDMAC), VERID
  `0x5342270A` (v2.70a), HCON `0x08E42541` (64-bit FIFO entries, 64-bit addressing), CLKSEL `0` (no phase shift, so
  no USE_HOLD_REG), CTYPE `0x10000` (8-bit), UHS_REG `0x10000` (**DDR**: S-Boot runs DDR52, not HS400), BMOD `0x282`,
  STATUS `0x8906`. The card answers CMD13 at **RCA 1** in TRAN state (R1 `0x900`). PIO CMD17 reads work: LBA0 has
  `0xAA55`, LBA1 "EFI PART" (entries at LBA 2, 128 entries), GPT "BOOT" starts at **LBA `0x23800`** (= Linux sysfs
  145408), and BOOT's first sector is our own boot.img ("ANDROID!", kernel_size `0x18400`). Samsung protector/SMU:
  MPSCTRL0 `0xF1` (= BYPASS: secure + non-secure read/write, valid), MPSEND0 `0xFFFFFFFF`, readable from NS EL1, so
  it does not block us (nor would it block writes: be careful). The shim only ever sends CMD13/CMD7/CMD17 (checked in
  the disassembly). Step 3's shim half is done; DDR52 PIO is slow but enough to load a payload. The XNU root-device
  driver is still to do.
- **Verified:** `mkadt.py` sweet output is byte-identical to `45263be` (seed `\0`*64, highmem off and on);
  `--device jackpotlte` builds (10200 bytes). Open question: every jackpotlte CPU node has `cluster-id` 0, although
  the A53s sit in two clusters (`0x1xx`, `0x2xx`) and the A73s in a third.
- **XNU step 4a, GICv2 (commit `2a044b2`, patch `0082-jackpotlte-gicv2.patch`, CI run `41370797532`).**
  - **Observed (shim milestone 1 ID dump):** ID_AA64MMFR0_EL1 = `0x1122`, so TGran16 = 0: **the cores have no 16K
    translation granule.** Sweet's XNU (`__ARM_16K_PG__`) cannot run here. The A8 kernel uses 4K pages like XNU's
    BCM2837 (Raspberry Pi 3, also Cortex-A53) config.
  - Design: no new MACHINE_CONFIG (dozens of patches test `SWEET` / `ARM64_BOARD_CONFIG_SWEET` /
    `CURRENT_MACHINE_CONFIG`). Instead, `SWEET_BOARD=jackpotlte` in the environment (xnu.yml input `board`) switches
    `MakeInc.def` to `-DARM64_BOARD_CONFIG_SWEET -DSWEET_JACKPOTLTE -march=armv8-a`. `SWEET.h` then drops
    `__ARM_16K_PG__`, PAN and the crypto define (not verified on this SoC), adds `__ARM64_PMAP_SUBPAGE_L1__` (as in
    BCM2837), sets `SWEET_GICV2` instead of `HAS_GIC_V3`, and sets vtimer INTID 27. `board=sweet` (the default)
    builds exactly what it did before.
  - The GICv2 driver is in `pe_fiq.c`: ADT `/arm-io/gic` reg = GICD, GICC (GICH/GICV unused). The boot CPU sets
    GICD_CTLR=1. Every CPU sets its banked SGI/PPI priority to 0xA0, enables the vtimer and SGI1, sets PMR 0xF0 and
    GICC_CTLR=1, and records its CPU-interface mask from its banked GICD_ITARGETSR0. `sleh_irq` acks and EOIs
    through GICC_IAR/EOIR, using the full IAR because GICv2 SGIs carry the source CPU. `SweetCPU::signalCPU` writes
    GICD_SGIR. `sweet_secondary_entry`'s EL2 path skips `ICC_SRE_EL2` (UNDEFINED without GICv3 sysregs).
  - **CI result (observed):**
    - Run `41370797532` compiled every 0082 file, with only XNU's usual warnings. It failed in one place: a
      `pmap.c` static assert. With 4K pages, XNU requires the commpage (`0xFFFFFC000`) to sit above
      MACH_VM_MAX_ADDRESS, and blacktop builds the macOS platform, whose user VA ends at `0x7FFFFE000000`.
    - Patch `0083-jackpotlte-ios-user-va.patch` (commit `2d79fa0`) uses the iOS value `0xFC0000000` for
      SWEET_JACKPOTLTE, the same as 4K-page iOS devices, and the layout the iPhone userland expects.
    - **Run `41371924237` succeeds.** It links `kernel.development.sweet` (16,799,808 bytes; artifact
      `xnu-kernel-jackpotlte`, with the build tree in `xnu-buildtree-jackpotlte`). All 1,108 compile commands carry
      `-DSWEET_JACKPOTLTE -march=armv8-a`, and none carries `armv8.2-a`. The file name is still `.sweet` because
      MACHINE_CONFIG stays SWEET.
    - Not run on hardware yet. The kernel would not boot as is (see below).
    - Reading the logs: the API needs a token and `gh` is not installed. Open
      `github.com/<repo>/commit/<full sha>/checks/<job id>/logs` in the signed-in browser, take the
      redirected blob URL (it expires after about 10 minutes) and `curl` it. The log is about 30 MB, so grep for
      ` error: `.
- **XNU step 4b, SMP + 4K early framebuffer (commit `2d64023`, patches 0084/0085, CI run `41373667993`).**
  - 0084 (SMP): `SWEET.h` gives `SWEET_BOOT_MPIDR` (A8 `0x100`), `SWEET_MPIDR_VALID()` (A53 `0x100–0x103`,
    `0x200/0x201`, A73 `0x0/0x1`) and `SWEET_CPU_NAME`. SweetCPU uses these instead of sweet's checks
    ("Aff0 must be 0", "boot MPIDR 0"). No other change was needed: the ADT marks cpu0 (`0x100`) "running",
    XNU's reset vector already matches full Aff1:Aff0, and PSCI CPU_ON is the same `0xC4000003`. SMP still
    needs `cpus=N` (default 1). A73 + A53 run as one homogeneous cluster.
  - 0085 (early console, 4K):
    - `SWEET.h` defines `SWEET_FB_PA`, `SWEET_FB_WIDTH`/`HEIGHT`/`STRIDE`/`BYTES`. A8: `0xEC000000`, 1080×2220.
    - `start.s` stage 0 uses them. The bootstrap framebuffer mapping is the new `sweet_bootstrap_blocks` macro: it
      reuses the L2 table of an occupied L1 slot or allocates one. sweet's result is unchanged (one 32 MB block in
      L1 slot 0). The A8 needs 5 × 2 MB blocks in L1 slot 3, plus one Device block for DECON in slot 0.
    - `arm_vm_init` maps the same blocks for the kernel alias above low globals.
    - Command-mode panel: the early console re-arms DECON's TE trigger (`HW_SW_TRIG_CONTROL` 0x70: clear bit 4,
      set bit 0) after every line, after a panic, and in start.s stage 0. Samsung's DECON driver says running with
      `TRIG_AUTO_MASK_EN=0` is forbidden, so every update has to be presented.
    - Assumption: the A8 boot shim leaves DECON enabled and DPP0 scanning `SWEET_FB_PA` (the diag shim's
      `a8_present` and `fb_mirror` do this).
    - `BOOTSTRAP_TABLE_SIZE` is 12 pages on the A8 (8 upstream).
  - `xnu.yml` has a new "ISA check" step for `board=jackpotlte`: it disassembles the kernel and counts ARMv8.1+
    instructions (LSE atomics, RDM, RCpc, non-hint PAC, dot product) per function. Output is
    `isa-v81-functions.txt` in the kernel artifact and in the log. This covers the closed `libcc_kext`.
  - Not covered yet: the regular framebuffer console and the IOMFB driver (after early-console stage 20) still
    need a present path, which is step "panel present path".
  - **Still blocking the first A8 XNU boot:** the A8 boot shim and payload placement (boot args with phys
    base and size, ADT, DPP0 on `SWEET_FB_PA`). (ARMv8.1+ hits: resolved in step 4c, count is 0. Shim: built in
    step 5; only the flash remains.)
  - **Was blocking before 0084/0085 (now addressed):**
    - `SweetCPU::start`/`startAll` require boot MPIDR 0 and MPIDRs `0xN00` (A8 boots on `0x100`; MPIDRs use Aff0).
    - `start.s` hard-codes sweet's framebuffer `0x9c000000` (stage-0 white band, MMU off) and a 32 MB L2 block that
      is only valid with 16K pages. On the A8, `0x9c000000` is plain RAM.
    - The bootstrap code assumes all RAM is below 4 GB.
    - Apple's closed KDK libraries linked into the kernel may contain ARMv8.1+ instructions (LSE atomics), which
      are UNDEFINED on A53/A73. Disassemble them before the first boot.

### Step 4c: ARMv8.0-clean kernel (2026-11-01)
- **Result (observed): run 41377484152 (commit f1bcf44, board=jackpotlte) built the A8 kernel and the ISA check
  reports 0 ARMv8.1+ instructions** in the whole linked kernel (including the closed libraries). Artifact
  `xnu-kernel-jackpotlte`. Not run on hardware.
- How it got there:
  - Run 41373667993: 1082 LSE instructions. Apple clang with only `-march=armv8-a` keeps the macOS default CPU's
    features, so 0082 now passes `-mcpu=generic -march=armv8-a` (commit f4d90a0).
  - Run 41375031655 then failed in the assembler (good: it now rejects v8.1+ asm):
    `commpage_asm.s` `casa` (×2) and `sha256_compress_arm64.s` SHA2 instructions (80 errors).
  - **0086-jackpotlte-armv80-asm.patch** (commit 3e29312):
    - The commpage `pfz_trylock_and_enqueue`/`dequeue` use an LDAXR/STXR acquire trylock (CLREX on failure; only
      x9–x15 are used) instead of `casa`.
    - `cc_config.h` sets `CCSHA2_VNG_ARM 0` on jackpotlte, so `ccsha256_di()` returns the C `ccsha256_ltc_di`.
      The SHA2 asm is compiled out. The Exynos 7885 crypto extension is unverified, so it isn't relied on.
  - Run 41376104920 built successfully, with 12 LSE hits, all in `firehose_*`. That code is libdispatch's
    `libfirehose_kernel.a`, which blacktop's `build.sh` builds with xcodebuild at the default Apple arm64 CPU.
  - `xnu.yml` (jackpotlte only) now adds
    `'OTHER_CFLAGS=$(inherited) $(SWEET_ARCH_CFLAGS_$(CURRENT_ARCH))' 'SWEET_ARCH_CFLAGS_arm64=-mcpu=generic -march=armv8-a'`
    to that xcodebuild line. `OTHER_CFLAGS[arch=arm64]=` on the command line is misparsed: run 41376911349
    failed with `clang: no such file 'arm64]=-mkernel'`.
- Caveat (ISA check): the ISA regex covers LSE, RDM, RCpc (`ldapr`), non-hint PAC and dot product. It does not cover every
  v8.2+ extension (e.g. FP16 arithmetic, `dc cvap`), and kexts in the kernelcache/userland are a separate question.

### Step 5: A8 XNU boot shim, milestone 4 (2026-11-01, built, NOT flashed)
- **Design:** the SWEETXNU payload (`tools/mkpayload.py`) is linked into the shim Image itself
  (`PAYLOAD=... shim/a8/build.sh`, `.incbin` in the generated `out/payload.S`), not put in the ramdisk.
  - S-Boot already loads the 22 MB stock kernel (template `kernel_size` `0x1539520`) at `0x80080000`, so a
    ~16 MB Image is normal for it.
  - A >16 MB ramdisk at `0x89000000` would run into the DTB S-Boot places at `0x8A000000`.
  - With a payload linked in, `a8_main` boots XNU through `shim/loader.c`; without one, the milestone 1–3
    diagnostics run unchanged.
- **Code:**
  - `loader.c`: `loader_boot_payload(start, end)` is split out of `loader_boot(dtb)`, plus two weak board hooks.
    sweet behaves the same.
  - `diag.c`: the A8 hooks `dc civac` each destination range before the MMU-off copy (S-Boot ran with
    caches on; no stale dirty line may be written back over the kernel once XNU enables caches), and call
    `a8_present()` before the jump so DECON is presenting.
  - `start.S`: `adrp` for `.bss` and the stacks (`adr`'s ±1 MB range is too small past a 17 MB payload).
- **boot_args** (same as sweet): physBase/memSize from the ADT `/chosen dram-base/size` = `0x880000000` /
  `0x7FF00000` (the panic log takes the top 1 MiB). The video base is `0xEC000000` 1080×2220, stride 4320; x0
  is the boot_args PA. The shim enters at EL1 with the MMU off and the I-cache invalidated.
- **CI:** `xnu.yml` (jackpotlte) now also runs `mkadt.py --device jackpotlte` and `mkpayload.py` on the fresh
  kernel. Results:
  - Run 41378975010 (commit 1942adc): ISA 0. `out/xnu-jackpotlte.payload` 16,241,952 bytes; no chained
    fixups.
  - `virt_base 0xfffffe0006000000`, entry `0xfffffe0007429488`, kernel_end `0xfffffe000846c000`. So text is at
    PA `0x881004000` and the kernel ends at `0x88246c000`. Both are in the artifact `xnu-kernel-jackpotlte`.
  - CI has no `research/ipsw-x/d22ap.adt`, so that ADT has **no `/product` node**. That doesn't matter for
    kernel entry; userland may need it.
- **Local check (synthetic 17 MB payload):** Image 17.9 MB, payload at offset `0x40e0`, embedded size word
  correct. `mka8boot.py` output fits the 32 MiB BOOT with ~14 MB to spare. The diag build (no payload) is
  unchanged at 101,600 bytes.
- **To build the first XNU boot image** (on the PC, artifact downloaded, LLVM on PATH):
  `PAYLOAD=<artifact>/xnu-jackpotlte.payload shim/a8/build.sh`, then
  `python tools/mka8boot.py <BOOT backup> shim/a8/out/Image <out>.img`. Flash from TWRP as before. Get the
  user's confirmation; recovery is unchanged.
- **First XNU boot image built (2026-11-01, not flashed yet).** The artifact from run 41378975010 was
  downloaded to the PC (kernel SHA-256 `e7b04f6a…fb7a3` matches CI's `SHA256SUMS`). A local `mkpayload.py`
  rebuild is byte-identical to CI's payload (`360008e1…aed1`).
  - Image: 16,343,552 bytes, payload at `0x40e0`.
  - `a8-xnu1-boot.img` (template = BOOT backup `c87a1336…`): SHA-256
    **`270f5102b890c89365427fc563780a528b43d56bcbca93c8787bcb2409034772`**.
  - DTBH and cmdline are byte-identical to stock; Image header text_offset `0x80000`, flags `0xa`.
  - The image stays on the PC (`_tmp/`, not in git).
- **Expected on the phone:**
  - First the blue "milestone 4: xnu loader" screen with PAYLOAD/PHYS/SEGS/BOOTARG/JUMP lines.
  - Then XNU's early console (0085: stage-0 white band, then text).
  - A red EXC screen means the shim faulted. Text that stops after JUMP means XNU died before its console.

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
1. **DONE 2026-11-01** (see Update above; XNU's present path must do the same re-arm per frame). Original note: if black, build the DECON trigger / TE-paced present path. Reference: S9
   port patch 0095 and the 4.4 Samsung DECON driver (`decon_f`, `psr_mode=2`, `trig_mode=0`, TE pinctrl `0x139B0A38`).
   S-Boot leaves DSIM running ("PLL is stabled in bootloader") and the framebuffer holds two 1080×2220 32 bpp buffers.
2. **DONE 2026-11-01** (see Update above). Original note: GICv2 in the shim: port `shim/shim.c`'s GICv3 timer IRQ self-test to GICD `0x12301000` / GICC `0x12302000`,
   virtual-timer INTID 27 (PPI 11), 26 MHz. Then PSCI `cpu_on` (`0xc4000003`, `smc`, v0.1 IDs from DT) for the 7
   other cores with the MPIDR table in `mkadt.py` (`JP_CORES`).
3. **Shim read path DONE 2026-11-01** (see Update above; XNU driver still to do). DW-MSHC eMMC driver (`0x13500000`, v270a, 64-bit IDMAC, HS400/enhanced strobe/CMDQ were enabled by Android;
   check whether S-Boot leaves the controller usable, otherwise full init with the `samsung,dw-mshc-*` timings).
   First read-only in the shim, then the XNU root-device driver replacing `bsd/dev/sweet_ufs.c` (patches 0028–0034, 0069).
4. XNU patches: **GICv2 interrupt controller and ARMv8.0/4K board variant written (0082, see Update above)**;
   **SMP MPIDR map (0084) and 4K early framebuffer/DECON present (0085) written; ARMv8.0-clean build with ISA
   count 0 (0086 + firehose CFLAGS, run 41377484152); A8 XNU boot shim built (step 5, not flashed)**; next: first
   XNU boot on the phone, then GPIO keys
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
