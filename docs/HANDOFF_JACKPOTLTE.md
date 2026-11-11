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
- **Flashed 2026-11-01 (user approved):** pushed to `/sdcard/a8-xnu1-boot.img` (hash OK on the phone), `dd` to
  `/dev/block/by-name/BOOT` (= `mmcblk0p12`), readback SHA-256 matches `270f5102…4772`, `adb reboot` from TWRP.
  `/sdcard/a8bak/BOOT.img` re-checked as `c87a1336…` before writing.
- **Observed (user photos):** the shim's blue screen is complete (PAYLOAD size `F7D520`, 10 segments, JUMP
  `0x881429488`; S-Boot placed the shim at `0x800CC000`; con_hex2 shows only the low 32 bits of the 64-bit
  addresses). XNU's start.s stage-0 white block (bottom left) appeared, then nothing: no `arm_init` text.
- **Cause (from the binary):** the boot TCR has T1SZ 26 (4K + `__ARM64_PMAP_SUBPAGE_L1__`: 38-bit kernel VA from
  `0xffffffc000000000`), but the macOS platform links the kernel at `0xfffffe0007004000` (ARM_LARGE_MEMORY,
  −2 TB). The branch to the kernel VA after the MMU is enabled faults, and so does the exception vector.
- **Fix: 0087-jackpotlte-ios-kernel-va.patch** (commit 7525da0): for jackpotlte, `-UARM_LARGE_MEMORY` and link
  at `0xfffffff007004000` (iOS 4K layout).
- **Run 41381049607 (commit 7525da0): success.** 0087 applied, `-image_base,0xfffffff007004000`, ISA 0.
  - Payload: 16,127,264 bytes. virt_base `0xfffffff006000000`, entry `0xfffffff007410488`, kernel_end
    `0xfffffff008450000`.
  - Kernel SHA-256 `eaac4048…6a87` (matches `SHA256SUMS`). Payload `ae790553…9b0c`, reproduced byte-identical
    locally.
  - Static check: boot TCR `0x22a51a251a` (T1SZ 26, TTBR1 from `0xffffffc000000000`) now covers the kernel text
    (L1 index `0xc0`), the vectors (`0xfffffff00740c000`) and `arm_init`.
- **`a8-xnu2-boot.img` built, NOT flashed:** SHA-256
  **`f7f4eb8f2ef985efa1da034cbde50ea4162d91f6e3a5143249dd974f83a299f6`** (Image 16,228,864 bytes, payload at
  `0x40e0`; DTBH and cmdline identical to stock).
- **Flashed xnu2 (user approved):** hash OK on `/sdcard`, `dd` to BOOT (`mmcblk0p12`), readback matches
  `f7f4eb8f…99f6`, backup re-checked `c87a1336…`, `adb reboot`.
- **Observed — XNU RUNS WITH THE MMU ON (user photo, xnu2):**
  - `SWEET EARLY 1` (arm_init entered, MMU on) through `7` (entering arm_vm_init) all printed. Progress blocks
    0–7 sit along the bottom edge.
  - Then `SWEET EARLY PANIC: arm_vm_init.c:1846 Assertion failed: (l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) ==
    (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)` in `sweet_early_map_block`.
  - So 0087 fixed the start.s fault: PE_init_platform, CPU topology, cpu_bootstrap,
    kernel_startup_bootstrap and cpu_init all work on the A8.
- **Cause:** 0012's `SWEET_EARLY_FB_VA = VM_MIN_KERNEL_ADDRESS + 2 MB`. That equals `LOW_GLOBAL_BASE_ADDRESS` only
  with ARM_LARGE_MEMORY. In the iOS layout it is `0xffffffe000000000`, an L1 slot with no table.
- **Fix: 0088-jackpotlte-early-fb-va.patch** (commit afb201a): `LOW_GLOBAL_BASE_ADDRESS + 2 MB` (same value on
  sweet). On the A8 the alias uses L2 slots 1–6 of the kernel's L1 slot, below `gVirtBase` `0xfffffff006000000`.
  In this layout the physmap and dynamic memory start above the kernel (`phystokv(topOfKernelData)`), so the
  alias is never handed to the VM. No other patch assumes `VM_MIN_KERNEL_ADDRESS` = low globals.
- **Run 41387128412 (commit afb201a): success**, ISA 0.
  - Kernel `bc6846c6…b340` (matches `SHA256SUMS`). Payload `3c5cc419…c6ea` (reproduced locally).
  - **`a8-xnu3-boot.img` SHA-256 `b3a413382bab04cdc0d88182706ed31334935cfbb16bf2ad40edb45db6e22df3`.** Flashed to BOOT
    with all checks passing (readback matches), `adb reboot`.
- **Observed — DARWIN BANNER ON THE A8 (user photos, xnu3):**
  - All SWEET EARLY stages passed. The progress bar runs across the whole bottom edge and the regular console
    took over.
  - Console: `Darwin Kernel Version 22.6.0: Mon Aug 14 05:06:07 UTC 2023; runner:xnu-8796.141.3`,
    `pmap_startup() init/release time: 18345 microsec`, `vm_page_bootstrap: 128151 free pages, 2857 wired pages`,
    `VM bootstrap: 97 maps, 204 entries and 512 holes available`, `Maximum number of VM swap files: 100`.
  - Then `panic(cpu 0 caller 0xfffffff007a4a878): kmem_range_init: vm_map_locate_space failing for claim zones
    @vm_kern.c:3645`, followed by nested panics (caller `0xfffffff007a2f754`) and "Nested panic stall".
  - 0088 worked.
- **Cause:** `zalloc.c` gives `ZONE_MAP_VA_SIZE` 128 GB on `XNU_PLATFORM_MacOSX` (24 GB on iOS). The iOS 4K layout
  has only ~112 GB of kernel VA (`0xffffffe000000000`–`0xfffffffbffffffff`).
- **Fix: 0089-jackpotlte-ios-zone-map.patch** (commit 4b95d92): iOS sizes (8 GB max / 24 GB VA) when
  `SWEET_JACKPOTLTE`. Checked the other kmem claims: only `compressor` (dynamic) is large, 16 × 2 GB = 32 GB
  under the macOS rule, which fits.
- **Run 41388657761 (commit 4b95d92): success**, ISA 0. Kernel `e8dbec53…83d1`, payload `8d33ad5a…24db`
  (reproduced locally). **`a8-xnu4-boot.img` SHA-256 `1bff6defc656965acaf744032598d272fc15c8ce3734f96cca34eebc8ec2093a`**,
  flashed to BOOT with all checks passing, `adb reboot`.
- **Observed (user, xnu4):** "same as last time", i.e. still the `kmem_range_init` panic.
- **Cause:** in the iOS 4K layout, `pmap_virtual_region()` region 3 also reserves `[VM_MIN_KERNEL_ADDRESS, low
  globals)`. Every kmem claim therefore has to fit above the physmap, ~45 GB (`virtual_space_start` ≈ kernel +
  ≤1 GB slide + 2 GB physmap, up to `0xfffffffbffffffff`). Zones (24 GB) plus the macOS compressor reservation
  (16 × RAM = 32 GB) don't fit; my earlier "compressor fits" estimate was wrong.
- **Fix: 0090-jackpotlte-ios-compressor-va.patch** (commit ef5b97e):
  - `vm_compressor.c` uses the generic iOS sizing (pool = RAM) when `SWEET_JACKPOTLTE`.
  - On SWEET, `pmap_virtual_region()` keeps at least 16 MB reserved at `LOW_GLOBAL_BASE_ADDRESS`, which covers
    the early FB/DECON alias (to +14 MB). Insurance: with 16 KB VM pages (131,008 pages = 2 GB) XNU already
    reserves 32 MB.
- **Run 41389836692 (commit ef5b97e): success**, ISA 0. Kernel `632dd999…15b0`, payload `e398b202…0d3f`
  (reproduced locally). **`a8-xnu5-boot.img` SHA-256 `c0f586c329b8735aac933c66d05a2f22f5356cee453cf9f5e8cd4a5334aa604e`**,
  flashed to BOOT with all checks passing, `adb reboot`.
- **Observed (user photos, xnu5):** `kmem_range_init` passed; Zone info, compressor info and 13 threads are
  printed. New panic `write_pte_fast: PTE write is unaligned, ptep=0xfffffff03842b618,
  pte=0x4200008830024ec3 @pmap.c:623`. Backtrace (symbolized with llvm-symbolizer, kernel unslid at
  `0xfffffff007004000`): `kernel_bootstrap_thread` → `vm_commpage_init` → `commpage_populate` →
  `pmap_create_commpages` → `pmap_enter_options_addr` → `pmap_enter_options_internal`.
- **Cause:** `pmap_create_commpages()` puts the text commpage at a random leaf (4K) index. With 16K VM pages on
  a 4K granule, `write_pte_fast()` writes PAGE_RATIO=4 PTEs and requires a 32-byte-aligned ptep (index 0xc3
  is not).
- **Fix: 0091-jackpotlte-commpage-text-va.patch** (commit 00d7f75): on SWEET, pick a leaf index that is a
  multiple of PAGE_RATIO (no-op on sweet, PAGE_RATIO 1).
- **Run 41391199403 (commit 00d7f75): success**, ISA 0. Kernel `3c70084c…4884`, payload `6f667a80…4553`
  (reproduced locally). **`a8-xnu6-boot.img` SHA-256 `152519a34dd1ebca3eaee5daa976d35af851f4a329639fa38322484299a3f3d4`**,
  flashed to BOOT, readback matched, `adb reboot`.
- **Observed (user photos, xnu6):** no panic. Boot passes the commpage and reaches IOKit: platform expert, AKS, WDT,
  SURF, HID buttons/touch, battery (static fallback), `SWEET FB: AppleCLCD 1080x2220 stride 4320 at 0xec000000,
  default surface 1, presented as 1125x2436`. Nothing printed after that line for minutes (user: "frozen"), but XNU's
  macOS boot progress meter (white bar, ~1/3) is drawn and the user saw it move.
  - Interpretation (guess): the kernel is alive, so timer interrupts through GICv2 work (the meter is driven by a
    delayed thread_call that slows down by design). Likely waiting for a root device (none exists on the A8 yet).
    Unconfirmed: on the command-mode panel, text printed after the last DECON trigger may simply not be shown,
    because only the early console and panics re-arm TRIG. `sweethidtest` is not in the default cmdline, so no key
    lines were expected.
- **Diagnostic: 0092-jackpotlte-bsd-init-heartbeat.patch** (commit bae8324): on jackpotlte, bsd_init's
  `bsd_init_kprintf` steps print via printf, plus `bsd_init: start` and a 5 s `SWEET A8: alive Ns, rootdevice '...'`
  heartbeat. Each print calls `sweet_early_kick()` (re-arms the DECON trigger).
- **Observed (user, xnu6 after ~8 h):** the progress meter reached ~50% = `kProgressMeterEnd` (512/1024), the
  kernel-side maximum. Kernel alive all night, never handed off to userland.
- **Run 41460201332 (commit bae8324): success**, ISA 0. Kernel `2c9b7250…`, payload `84353b06…` (reproduced
  locally). **`a8-xnu7-boot.img` SHA-256 `d8a030e73f67405c10a0669001099a2cff5c9b920e40296619c792324d1fadcd`**,
  flashed to BOOT, readback matched, `adb reboot`.
- **Observed (user, xnu7):** "back to how it was before the bar". No photo, so it's unclear whether any `bsd_init:` or
  heartbeat line appeared. The user chose to move on to the eMMC driver. Without `rd=`, `IOFindBSDRoot` waits forever
  for an IOMedia root, which fits the earlier observations (guess).
- **eMMC root device: 0093-jackpotlte-emmc-root-device.patch** (commit 9e21588, workflow fix 1868ab1):
  - `bsd/dev/sweet_ufs.c` keeps its BSD half (bdevsw/cdevsw `ufsroot0`/`rufsroot0`, GPT lookup, strategy, ioctls) and on
    jackpotlte swaps the UFS layer for DW-MSHC PIO: CMD13 at RCA 1 must report TRAN; reads are polled CMD17
    sectors through the 64-bit FIFO, exactly the shim milestone 3 method. `UFS_BLOCK` is 512 (the eMMC GPT uses
    512-byte LBAs). The ADT node is `/sweet-mmc`, and the default partition is `USERDATA`.
  - Read-only: `ufsrw` is ignored, writes return EROFS, so the log ring is off too. sweet's PMIC keys, GENI touch
    and DWC3 probes are skipped on the A8 (they would touch Qualcomm MMIO).
  - `xnu.yml` (jackpotlte) passes `--cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 rd=ufsroot0'` to mkpayload.
  - Expected first result: `SWEET UFS: "USERDATA" on LUN 0: first block …, read-only`, then HFS fails to mount
    Android's f2fs/ext4 USERDATA (`cannot mount root, errno = …` repeating). That proves the read path without
    touching any data. The iOS root image itself needs space on the eMMC: wiping USERDATA or the dual-boot GPT
    change, which needs the user's explicit OK.
  - **Run 41462686989 (commit 1868ab1): success**, ISA 0. Kernel `27aaf291…9386`, payload `823d77c7…537a`
    (reproduced locally with the same cmdline). **`a8-xnu8-boot.img` SHA-256
    `bce40ffcf1123e3688a6ca509792743a4a3fc6dfc811e0c312ffcb966d0ce696`**, flashed to BOOT, readback matched, `adb reboot`.
    **Observed (user photo, xnu8):** identical to xnu6/xnu7. The last line is `SWEET FB: AppleCLCD … presented as
    1125x2436` (after `SWEET: platform expert registered`). No `bsd_init:` line, no 0092 heartbeat, no `SWEET UFS/MMC`
    line. So the boot thread probably never reaches `bsd_init()`; the eMMC driver can't be judged yet. (Guess, not
    verified: printing works, since the earlier lines and the progress meter keep updating.)
  - **Diagnostic: 0094-jackpotlte-boot-thread-heartbeat.patch** (commit ef5328e): `kernel_bootstrap_thread` arms a 5 s
    thread call right after `spllo()`, printing `SWEET A8: Ns stage '<last stage>' state … event 0x… hint …` for the
    boot thread. Stages: initialize_screen done, commpage done, trust caches done, kext bootstrap removed,
    machine_lockdown done, PE_lockdown_iokit returned, calling bsd_init. Symbolize the wait event with llvm-symbolizer
    (kernel unslid). CI run 41466209363 failed (block inserted between `static` and `void`); fixed in 0396369, run
    41467822450 succeeded (not flashed: superseded by 0095).
- **Hands-off test loop (2026-11-02, user asked for no button presses):**
  - **Reboot into TWRP from our code, VERIFIED:** Samsung's reset reason (Exynos 7885 kernel
    `drivers/staging/samsung/sec_reboot.c` + `include/linux/sec_ext.h`, from `Exynos7885-Dev/android_kernel_samsung_exynos7885`
    lineage-18.1, read only): PMU `0x11C80000` + INFORM2 `0x808` = `0x12345678` (SEC_POWER_RESET), INFORM3 `0x80C` =
    `0x12345674` (SEC_RESET_REASON_RECOVERY), then SWRESET `0x400` = 1. Shim test image
    (`EXTRA_CFLAGS="-DA8_PROBE_RESET=5 -DA8_REBOOT_RECOVERY"`, SHA-256 `4c94c958…f808`): after `adb reboot` the phone was
    back in TWRP over adb **34 s later, untouched**.
  - **Log channel without eMMC writes:** TWRP's kernel has ramoops (`mem_address` 0x92400000, `mem_size` 0x8000,
    `record_size` 0x4000, `pmsg_size` 0x4000, `ecc` 0; no `/dev/mem`). A 2026-11-01 Android `dmesg-ramoops-0` was still
    there after many XNU boots, so S-Boot and XNU leave that RAM alone.
  - **0095-jackpotlte-ramoops-log-twrp-reboot.patch** (commit 3c713cd): `sweet_a8_snapshot()` writes the last ~16 KB of the
    console ring as a persistent_ram_buffer (sig `0x43474244`, start 0, size n, data with a `====1.0-D` header, which Linux
    needs for dmesg records) at 0x92400000, from the 0094 heartbeat every 5 s. After `sweetreboot` s (default 90, 0 = off)
    the heartbeat calls `sweet_a8_reboot_recovery()`. `panic_spin_forever()` waits `sweetpanicsecs` (default 10), then
    does the same. Read it in TWRP: `adb shell cat /sys/fs/pstore/dmesg-ramoops-0`.
  - Not covered: a hang with interrupts off (no heartbeat). The Exynos watchdog would cover it (TODO).
  - **Run 41469279085 (commit 3c713cd): success**, ISA 0. Kernel `cb504f51…b628`, payload `2200738d…8bcd` (reproduced
    locally). **`a8-xnu10-boot.img` SHA-256 `1f60d1cdee46c19d61065cdce049dc97c7f2effe18c0285ebd6c7a0f834061b4`**, flashed, `adb reboot`.
    The old Android pstore record was saved to `_tmp/pstore-android-before.txt` (PC only) first.
  - **Observed (first hands-off run): the phone came back in TWRP by itself after 118 s** and
    `/sys/fs/pstore/dmesg-ramoops-0` held the XNU console (5738 bytes, Darwin banner through IOKit). The heartbeat says:
    `stage 'PE_lockdown_iokit returned' state 0x9 event 0xfffffff007bd9dc4` (TH_WAIT|TH_UNINT), unchanged for 90 s, then
    `rebooting into TWRP (sweetreboot)`. llvm-nm: `0xfffffff007bd9dc4` = **`_max_cpus_initialized`**: the boot thread is
    in `vm_set_restrictions(machine_info.max_cpus)`, waiting for `ml_set_max_cpus()`. No `SWEET CPU:` line at all.
  - **Cause:** SweetCPU's IOKit personality has `IONameMatch = 'qcom,kryo470'`; the A8's `/cpus` nodes are
    `arm,cortex-a53` / `arm,cortex-a73`. No CPU driver starts, so max_cpus is never set. That's also why xnu6–xnu8
    stopped after `SWEET FB`.
  - **Fix: 0096-jackpotlte-cpu-driver-match.patch** (commit c05a1e7): on jackpotlte `IONameMatch = ('arm,cortex-a53',
    'arm,cortex-a73')`. CI run 41471902922: success, ISA 0.
    **`a8-xnu11-boot.img` SHA-256 `26045fd2bb83eacb6a72ea93d241e00dc5cafa89f08be428a342da0b76831b69`**, flashed to BOOT,
    readback matched, hands-off run (back in TWRP after ~120 s, log 13802 bytes, PC only: `_tmp/xnu11-log.txt`).
  - **Observed (xnu11):** `SWEET CPU: cpu=0 mpidr=0x100 came alive; SGI1 OK`, `bsd_init: start`, all bsd_init steps
    through networking, then `SWEET UFS: "USERDATA" on LUN 0: first block 0x11ac800, 20770 MiB, read-only`,
    `BSD root: ufsroot0`, `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : a04b`,
    `cannot mount root, errno = 19`, retried every 5 s (`mount(13) failed` at 90 s), then the sweetreboot.
  - **eMMC read path verified:** USERDATA start in TWRP is sector 18532352 = 0x11ac800, and bytes 1024..1025 of
    mmcblk0p29 are `a0 4b` (ext4 superblock), exactly the signature HFS reported. **0093 works, byte-exact.**
  - **Next blocker: an HFS+ iOS root on the eMMC.** Decision pending with the user: GPT split (plan below, wipes
    Android data) or an image file inside the ext4 USERDATA (no wipe, the driver needs an extent map; the file must
    live outside FBE-encrypted dirs, e.g. `/data/sweet/`, so it is plaintext on disk; 14 GB free on 2026-11-02).
  - **User chose the image file (no wipe), 2026-11-02.** Verified in TWRP: `/data/sweet/t.bin` (4 MiB, inode 1044512,
    flags `0x80000` = extents, no encrypt flag) maps to one extent at partition block 4228096, and the raw eMMC
    bytes there are the plaintext test pattern. `tools/ext4map.py DEV INODE` does this walk over adb.
  - **0097-jackpotlte-file-backed-root.patch** (commit 4238c0d): boot-arg `sweetrootino=N` makes the root device the
    ext4 file with inode N in USERDATA (superblock → group descriptor → inode → extent tree, up to 8192 extents;
    holes/unwritten read as zeros; read-only). Prints `SWEET ROOT: "USERDATA" inode N: … MiB, … extents`.
    Without the boot-arg nothing changes. CI run 41477553892: success, ISA 0, CI payload reproduced locally. Test payload rebuilt with
    `sweetrootino=1044512`: **`a8-xnu12t-boot.img` SHA-256 `58f9420211787b3b48f5d9aae94c301040f90b7f8959ee2a10f517523b7e52db`**,
    flashed, readback matched, hands-off run (TWRP after ~120 s).
  - **Observed (xnu12t): `SWEET ROOT: "USERDATA" inode 1044512: 4 MiB, 1 extents, 4 MiB mapped`, then
    `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 5454`** = "TT" at file offset 1024 of the test
    pattern, as predicted. **0097 works: XNU reads a file inside ext4 USERDATA.** Remaining: the HFS+ image itself.
  - Local tree note: the Windows checkout has CRLF patch files; strip the CRs before `git apply` in `_tmp/` trees.
  - **iOS image build (WSL Ubuntu 26.04):** the WSL kernel 6.18.54.1 has no hfsplus (`# CONFIG_HFSPLUS_FS is not set`,
    modules allowed, no signing), apfs-fuse is not packaged, TWRP's kernel has no HFS+ either. Plan: build `hfsplus.ko`
    from Microsoft's `linux-msft-wsl-6.18.54.1` tag, build apfs-fuse from source, apt hfsprogs. Blocked on the user's OK
    to download/build/run that code as root in WSL. PC: C: freed to 35 GB (WSL swap 32→4 GB in `.wslconfig`).
  - **iOS root image on the phone (2026-11-02):** built in WSL (`_tmp/wsl/mkroot.sh` + `mkroot2.sh`: mkiosroot.sh 16G,
    sweet-root-tweaks.py applied, launchd/dyld/shared cache present; 12 GB used). `fsck.hfsplus -fn` still reports
    "Volume header needs minor repair" after `-fy` (Linux hfsprogs quirk? unverified). Image SHA-256
    `b094e826b5b84c748780e6e6f1f3c701cd2302c7b5432817cd0f6ee436c6f307`, copied with a8push.py to
    `/data/sweet/root.img` (inode **1044513**, 113 extents, 11811 MiB allocated, hash matched on the phone; 2.6 GB
    left free). Android booted fine afterwards with the file in place.
  - Local ADT with the real D22AP `/product` (`research/ipsw-x/d22ap.adt` from the IPSW DeviceTree, LZFSE-decoded):
    `_tmp/a8art12/jackpotlte-product.adt`, 12884 bytes (CI's has none, 10200 bytes).
  - **Ready, not flashed:** `a8-ios1-boot.img` SHA-256 `1870593347a52ffabea9547f44b99252eb2306ffc149b826b7b4fffee5e62842`
    = run 41477553892 kernel + product ADT, cmdline `debug=0xc serial=3 cpus=1 keepsyms=1 rd=ufsroot0
    sweetrootino=1044513 sweetreboot=240`. (User later: no need to restore Android between tests.)
  - **Observed (ios1, flashed, hands-off):** `SWEET ROOT: … inode 1044513`, then HFS `journal_create` failed with
    EROFS. Cause: Linux `mkfs.hfsplus -J` leaves the journal info block flagged NeedInit (4) and the journal header
    zeroed, so XNU tries to create the journal at mount. Fix: `tools/hfs-jnl-init.py IMAGE` writes the header the way
    XNU's `journal_create()` does (JNLx, little-endian, start = end = 512, size 0x800000) and clears NeedInit. Applied
    to the WSL image and to the phone copy in place (bytes `[0x81000,+4)` and `[0x82000,+512)` only).
  - **Observed (second run, after the journal fix; log `_tmp/ios2-log.txt`):** **HFS root mounts** (`hfs: mounted
    iOSRoot`, `jnl: journal start/end pointers reset!`, `bsd_init: done`), then launchd is killed with exit reason
    namespace 9 (EXEC) code 14 = `EXEC_EXIT_REASON_WRONG_PLATFORM`: the kernel is built for the macOS platform.
    Fix: boot-arg `unentitled_ios_sim_launch=1` (as in sweet's README cmdline).
  - **Observed (`a8-ios2-boot.img` SHA-256 `345340da…b598`, cmdline `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress
    rd=ufsroot0 sweetrootino=1044513 sweetreboot=300 unentitled_ios_sim_launch=1 ignition_level=1
    launchd_unsecure_cache=1 wdt=-1 wdt_test=no_user_checkin`; log `_tmp/ios3-log.txt`): **launchd runs**
    ("Darwin Bootstrapper Version 7.0.0", ondemand mode, fsck boot task). Then mount-phase-1: `GetMasterBlock: Error
    16 opening /dev/ufsroot0`, `mount_hfs: Read-only file system`, `mount: / failed with 1`, and a panic when
    `mount[3]` exits 1. launchd needs `/` remounted read-write.
  - **0098-jackpotlte-ram-write-overlay.patch** (commit f172c7e): boot-arg `sweetramrw=1` (cap `sweetrammb`, default
    256 MiB) makes the root device writable; every write goes to 4 KiB pages in RAM (copied from the image on first
    touch), reads are patched with them. The eMMC is never written (writing into the sparse image file could hit
    holes that ext4 never allocated). The eMMC log ring is forced off on the A8. CI run 41504404196: success, ISA 0,
    kernel hash matches `SHA256SUMS`. **`a8-ios4-boot.img` SHA-256 `86605144cf568e681987c606c19f62992d6e8ee7a6f12d8150424befa3bd4102`**
    (= ios2 cmdline with `sweetramrw=1`, `sweetreboot=360`; built by `_tmp/mkios.sh`), flashed, readback matched.
  - **Observed (ios4, hands-off, back in TWRP after ~6.5 min; log `_tmp/ios4-log.txt`, last 16 KB only):** the
    mount-phase-1 failure is gone. Userland runs: 118 `exec` lines in the last 16 KB alone (xpcproxy, bluetoothd,
    contactsd, homed, healthd, watchdogd, …), configd sets up lo0. The 0096 sampler shows **SpringBoard (pid 27)
    and backboardd (pid 57) running**, and `SWEET FB: swap 8 layer 0 surface 3 1125x2436` at ~280 s, i.e.
    backboardd presented a full frame. No panic; the run ended with the planned sweetreboot.
  - Not seen (unverified): what the frame shows (lock screen, boot logo or blank). Nobody was watching the screen
    and the panel content is lost on reboot. securityd keeps getting `0xe00002c7` from AKS sel 10 (keybag; guess:
    harmless for now).
  - **User watched ios5 (`sweetreboot=0`):** the screen showed the **Apple logo** (iOS's own boot frame), then the kernel
    console text scrolled over it. 0 ⇒ `sweetfb_noconsole=1` from ios7 on: clean Apple logo, the screen still refreshes.
  - **0099 (commit 1586691, CI 41509829383): GPIO keys + sec_ts touch work** (ios6s, 1 core): `SWEET HID: touch 0 down at
    447,1332`, right-edge touches at x≈1050 tagged `edge`, `SWEET A8 KEYS: key=2 down=1 gpa1=0xae` →
    `SWEET HID: key 0xea down (1 clients)` (volume down), volume up likewise; power (0x30) seen in ios7.
  - **ios6 (cpus=6) panicked:** `cpu=1 PSCI CPU_ON rc=0`, then `NOT alive after 5s`. **0100 (commit 376afb0, CI
    41512421744):** trampoline at PA 0x80080000 (`ldr x16,=entry; br x16`). **ios7 (cpus=6, noconsole, SHA-256
    `e8cd02c5…b8da`) ran 460+ s with no panic: 6 A53 cores up.**
  - **Why it stays on the Apple logo (ios7, symbolicated with `ipsw` v3.1.739 + `tools/sweet-crash.py`):** SpringBoard's
    main thread waits in `_LSContextInitCommon` ← `-[LSApplicationRecord initWithBundleIdentifier:…]` ←
    `-[CCSModuleRepository _queue_filterModuleMetadataByAssociatedBundleAvailability:]`; another thread is in
    `__LAUNCH_SERVICES_IS_RESEEDING_ITS_DATABASE_AND_MAY_BLOCK__` (`_LSCopyServerStore`, sync XPC to lsd). lsd is rebuilding
    the app database. Guess: with the RAM overlay **every boot is a first boot** (nothing persists), and the reseed is slow
    on PIO eMMC reads. bluetoothd and fairplayd.H2 also relaunch every ~10 s. Fixes: a persistent data volume
    (`/private/var` in a fully allocated file), faster eMMC (IDMAC/DMA), CPU clock, and the S7's BTAudioHALPlugin removal.
  - Symbolication setup (WSL): image mounted read-only at `/mnt/iosroot` (`_tmp/wsl/mountro.sh`); ipsw needs the cache in
    a writable dir, so `/opt/sweet/dsc` holds **relative** symlinks to the cache files (absolute ones fail, overlayfs on
    hfsplus fails).
  - **GPU step 2 (0101, commit aeff637, CI 41517565494; `a8-ios9b`, `sweetgpu=2`):** Mali-G71 MP2 identified and powered,
    details in `docs/GPU_JACKPOTLTE.md`.
  - **Observed (ios8, `sweetreboot=1800`, log `_tmp/ios8-log.txt`): panic at ~1150 s** `hfs_swap_BTNode: offset #0 invalid
    (0x0000) … node=19731 fileID=4`, then `hfs_UNswap_BTNode: invalid forward link`. Guess (not verified): the 256 MiB RAM
    overlay filled up and refused writes, leaving the catalog B-tree inconsistent in memory. 0102 (commit f02cca2) logs
    overlay use every 50 MiB and repeats the "full" line every 500 refused writes.
  - **Persistent root: 0103-jackpotlte-emmc-write-root-in-place.patch** (commit c38eb29, CI 41521759537 success, kernel
    hash matches `SHA256SUMS`, artifact in `_tmp/a8art17`). Boot-arg `sweetrootrw=1`: if the root file is fully written
    (extents contiguous from 0, none unwritten, covering the size), writes go to the eMMC through the file's extent map
    (polled CMD24 through the FIFO, DAT0 busy wait, CMD13 until TRAN; CMD6 FLUSH_CACHE on sync). Every write is checked
    against the USERDATA bounds (`mmc_wr_lo/hi`). Otherwise the RAM overlay is used as before.
  - **13 GiB image (`_tmp/wsl/mkroot3.sh`):** the root is rebuilt at 13 GiB so it fits /data fully allocated (12.46 GB used,
    1.49 GB free inside, fsck OK, journal header written). SHA-256
    `6afd5ba18a61678e5e25219c166e39fe6a2d625cb93aad2163742bba06e18d16`. BTAudioHALPlugin.bundle was **not found** under
    `/System/Library` (depth 6), so nothing was moved. The old sparse image and `t.bin` were deleted from the phone
    (/data then had 14.5 GB free). Pushed with `tools/a8push.py --dense` (every block written, so the file has no holes).
  - **GPU step 3: 0104-jackpotlte-mali-g71-mmu-write-value-job.patch** (commit 7b8456a, CI 41524145769). `sweetgpu=3`:
    after powering the cores, AS0 gets Mali LPAE page tables (G71 has no AArch64-format GPU MMU, Panfrost
    `hw_features_g71`): table = PA|3, leaf = PA|1 at every level, HAP read+write (bits 6-7), outer shareable,
    TRANSTAB = L0 PA | READ_INNER | ADRMODE_TABLE, MEMATTR 0x88/0x8D/0x88. A chain of two WRITE_VALUE jobs (Zero, then
    Immediate 32 `0x5157ee70`; layout from Mesa genxml `v6.xml`) runs on job slot 1, polled (no IRQ). Expected:
    `SWEET GPU: the GPU wrote RAM through its MMU (step 3 done)`.
  - **New root file on the phone:** `/data/sweet/root.img` inode **1044497**, 13 GiB, 121 extents, contiguous from block
    0, none unwritten (`tools/ext4map.py`), SHA-256 after push `6afd5ba1…8d16`.
  - **Observed (ios10, 0103, `sweetrootrw=1`): panic at ~52 s,** `tzinit[18] exited … exit status 22`. Every file create
    in userland returned EINVAL (HFS journal invalid after a failed write); root.img's hash was unchanged, so no write
    landed. ios12 (0105, status every 10 s) panicked the same way; the 16 KB window held only ~2 s of launchd output.
  - **0105** (commit 8ee468a) records the first write failure (step, errno, sector, rintsts/r1); **0106** (f729292) fills
    the FIFO before CMD24 and ignores HTO on writes; status line every second.
  - **Observed (ios13, 0106):** `0 sectors written, 5 failed; first failure err 5 where 3 at 0x2e50878 (0x94 0x80)` =
    RINTSTS DCRC after all 128 words: the card rejected the data CRC.
  - **Cause and fix, 0107** (commit c78fe90, CI 41533351451): the driver read CLKSEL at `0xa4`, which is BUFADDRU with
    64-bit IDMAC registers. Samsung's `dw_mmc-exynos.h` (Exynos7885-Dev lineage-18.1, read only) puts CLKSEL at
    BUFADDRU + 4 = **`0xa8`**, and its driver sets `CMD_USE_HOLD_REG` on every command when the drive phase is non-zero
    (live FDT DDR timing `<3 0 4 2>`: divider 3, fine drive 0, drive 4, sample 2). Reads tolerate the missing hold
    register; writes do not. Init now logs CLKSEL and UHS_REG. (The shim's milestone-3 "CLKSEL 0" came from the same
    wrong offset.)
  - **Observed (ios14, 0107, `a8-ios14` SHA-256 `f592189d…9f4d`, 900 s run): root writes work.** 1,163,980 sectors
    (~570 MB) written, 0 failed, no panic; Apple logo at ~470 s; `com.apple.datamigrator` ran; backboardd looks for
    `IOAcceleratorES` (found the stub) and `IOAcceleratorESLegacy` (none). SpringBoard's main thread still waits in
    `_LSContextInitCommon` ← `LSApplicationRecord` ← `CCSModuleRepository`, a second thread in
    `__LAUNCH_SERVICES_IS_RESEEDING_ITS_DATABASE_AND_MAY_BLOCK__` (symbolicated `_tmp/wsl/smp14.sym.txt`). bluetoothd and
    fairplayd.H2 still crash-loop (~every 10 s). The reseed now persists between boots (guess: it needs one long run).
  - **0108** (commit 51dae2a, CI 41536600658): status every 30 s. **`a8-ios15`** (`c294fa66…bc16`): verbose console,
    `sweetreboot=0` (runs until stopped), in-place root writes. Flashed 2026-11-02 16:33 for a long watched run.
  - **a8-ios15 result:** mount failed, "unknown Volume Signature : 0". Sector 2 (HFS volume header) and 6 more had
    been zeroed by earlier writes with no error reported. Root image restored from `/opt/sweet/ios16-root.img`
    (WSL) by per-64 MiB-chunk md5 compare (strip CRLF from adb output) and resending bad chunks. `tools/a8push.py`
    once dropped a chunk silently: always check the final SHA-256.
  - **0109** (commit 2391c31, CI 41539477993): read back every written sector, re-read on mismatch (read glitch vs
    write fault), rewrite up to 3 times, then fail with where 11. **`a8-ios16`** (`d3a8434f…`): verify refused 4
    sectors ("still wrong after 3 writes"); the read-back showed stale data (0xFF, old content, foreign bytes).
  - **0110** (commit a38f5aa, CI 41545697431): fill the FIFO only after CMD24 is accepted (Linux order, not pre-fill),
    and require STATUS FIFO empty after the FIFO reset (where 12 otherwise). **`a8-ios17`**
    (`5181c357acc60a21a80a27584cb0f636c65eff2722ad8e5cb2d48e37316947af`), flashed 2026-11-02 ~17:40 CST.
  - **MILESTONE 2026-11-02 (observed, user photos): a8-ios17 reached the iOS 16 lock screen** (red clock "4:35",
    "Wednesday, December 31", camera button) after ~2030 s of uptime: 2.37M sectors written, 0 failed, 0 rewrites,
    0 read glitches, 0 dirty FIFO, no panic. The LaunchServices reseed that blocked SpringBoard finished on its own
    (guess: it was just slow PIO I/O, ~1 MB/s). bluetoothd and fairplayd.H2 still crash-loop; touch and keys deliver
    HID events. The verbose console faded and the Apple logo came back right before the lock screen.
  - **0111** (multi-block CMD18/CMD25, 64 KiB per command, run read back with one CMD18; `sweetrootverify=0` skips)
    + **0112** (`sweetfb_noconsole=N`: console text hidden from GUI swap N on), commit 625e42c, CI 41552897282.
    **ios18** (`7b891a4c…`, `sweetfb_noconsole=30 sweetfb_hz=30`): into iOS in under 5 min (was ~34), 0 failures.
    **ios19** (`514a6db4…`, same + **`cpus=8`**): boots with the two A73s too (user: "it boots"); log: 1095 s, no
    panic, 288958 sectors / 7506 multi-sector writes, 0 failed. Still laggy (CPU rendering). Guess (from y2sios): CPUs
    may be at S-Boot clocks; check DVFS.
  - **GPU step 4 (0113, commit 32f8b68, CI 41556352389):** Mali service (`sweetgpu=4`, docs/GPU_JACKPOTLTE.md);
    **ios20** (`f9ad1c64…`, ios19 + `sweetgpu=4 sweetgputrace=4`) flashed 2026-11-02 ~19:15; result pending
    (`SWEET MALI: self test:` line, early in the boot).
  - ios20 log: no crash at boot; the 0113 self-test line had scrolled out of the 16 KB window. The run ended in
    `Halt/Restart Timed Out @IOPlatformExpert.cpp:884` at ~390 s (a restart from inside iOS that hung; not investigated).
  - **GPU step 5 (commit 2e9b397, gpu.yml run 41558053357): success.** Mesa 25.0.7 panfrost built for iOS 16
    (`out/libOSMesa.8.dylib`, `mali_test`, `gl_test`, PC copy `_tmp/gpu1`). The outputs were **unsigned** (ld only
    signs macOS output); signed ad-hoc locally with `ipsw macho sign --ad-hoc`, and build.sh now runs `codesign -s -`
    (commit 32c4849). **0114** (status line with the Mali result every 30 s) built in xnu run 41558123439.
  - **Screen cut off** (iPhone X 1125x2436 shown as a centred crop): **0115** (commit 5ab05a8, xnu run 41559227460,
    artifact `_tmp/a8art27`): boot-arg `sweetfb_fit=1` scales the frame by 2220/2436 to 1025x2220 at x=27 (black bars),
    nearest-pixel, and SweetHID maps touches back through the same ratio.
  - **Missing apps (only Safari, Messages, Camera, Photos, Settings, App Store, Phone):** iOS 16 ships the removable
    system apps in `/private/var/staged_system_apps` and DataMigration installs them, which the `migration` tweak
    skips. Fixed on the phone's image and in `tools/sweet-root-tweaks.py` (`staged_apps`: 32 apps moved to
    `/Applications`). The LaunchServices csstore was deleted so lsd reseeds (expect a slow first boot).
  - **Editing the phone's root image** (TWRP has no hfsplus): check the HFS journal is empty (JIB at block 105, header
    at 0x6a000: start == end) and the volume header has the unmounted bit; `adb pull` the image into WSL
    `/opt/sweet/phone-root.img` (20 MB/s, 11 min); `_tmp`-side edit script mounts it `-o loop,force`, edits, and lists
    the changed 64 MiB chunks; `_tmp/pushchunks.sh` writes only those back with `dd conv=notrunc` and checks each md5,
    then the full SHA-256 (2026-11-02: 31 chunks, `f0f40979…a7ba` on both sides; `ext4map.py` still shows the file fully
    written). fsck after the edit: "Incorrect number of file hard links" + "Volume header needs minor repair" (not
    repaired; probably from before the edit).
  - The same edit added **G6** files: `/sweet/bin/{mali_test,gl_test}`, `/sweet/lib/libOSMesa.8.dylib`, and a launchd
    job `com.sweet.malitest` (in `/System/Library/xpc/launchd.plist`) that runs `mali_test 1000` once at boot with
    output to `/dev/console`.
  - **ios21** (`f8901cca5273f9e960a1d79027dfba413e603065d76107625baa516d6568ec57`) = 0115 kernel, ios20 cmdline +
    `sweetfb_fit=1`. Flashed 2026-11-02 19:53.
  - **ios21 panicked** when mali_test made its first buffer: `phystokv_range: illegal PA: 0xa31b4000` from
    `pmap_sync_page_attributes_phys` (write-combined IOBufferMemoryDescriptor): `(vm_offset_t)(pp << PAGE_SHIFT)` shifts
    the 32-bit ppnum_t before widening, dropping bit 35 of `0x8a31b4000` (code that Apple's coherent SoCs never build).
    **0116** (commit 2538631, xnu run 41563096789, `_tmp/a8art28`) shifts in 64 bits (checked in the disassembly).
  - **ios21n** (`b826dad0…7fe3`, 0115 kernel, no `sweetgpu`): user confirmed the restored apps on the home screen and the
    fit mode (whole screen, home bar and battery icon visible). First boot took ~20 min (LaunchServices reseed).
  - **ios22** (`7ab935a706029e6243fc7786081f192dbf87a91571e26ff0e87d107963be7e08`, 0116 kernel, ios21 cmdline): no panic.
    First run: `2001 jobs, 1001 faults, 1001 resets; self test PASSED`: every mali_test job faulted. Cause: y2sios wrote
    mali_test for the Valhall G77; on Bifrost the job header needs bit 0 (64-bit descriptor), as the kernel self test has.
    Fixed in source (commit d9d7a86) and on the phone by patching the two `mov w8,#4` to `#5` in the installed binary,
    re-signing at the same size (81920 bytes) and writing only the 2 changed 4 KiB blocks into root.img (found by content
    in the WSL copy; old contents checked on the phone before writing; WSL copy updated the same way).
  - **GPU step 6 part 1 DONE (ios22, second run): `SWEET MALI: up; 2001 jobs, 0 faults, 0 resets; self test PASSED`.**
    An iOS process drives the Mali through the panfrost UAPI (CREATE_BO, MMAP, SUBMIT, syncobj and WAIT_BO). Counters are
    per boot.
  - **Test runner** (commit 18f9c08): `gpu/src/sweet_run.c` = `/sweet/bin/sweet-run`, launchd job `com.sweet.run` (replaces
    com.sweet.malitest), runs `/sweet/run.cmd` (64 KiB, text up to the first NUL) once per boot, output into
    `/sweet/run.log` (1 MiB). `tools/a8run.py cmd|log MAP.json` writes the commands / reads the log from TWRP through the
    files' blocks in root.img (map: `_tmp/gpu1/out/run-map.json`, run.cmd at block 3049987, run.log 3050003..3051200;
    `ADB` must be a Windows path). Built locally: LLVM clang + ld64.lld against theos' iPhoneOS16.5.sdk in `C:\sw\sdks`
    (symlinks come out as text on Windows: link `libSystem.B.tbd` directly with `-nostdlib`), signed with `ipsw macho sign`.
  - **Image edits now need a clean iOS shutdown first** (Settings > General > Shut Down; the Halt/Restart panic at the very
    end comes after `hfs: unmount` and leaves the journal empty). A forced power-off left journal start != end and
    `_tmp/runner-install.sh` refused to edit (as designed). `_tmp/pullchunks.sh` syncs the WSL copy by 64 MiB chunk
    hashes (20-22 chunks after a session) instead of a full pull. Bug noted: `diff` exits 1 under `pipefail`, which ended
    the edit scripts before their CHANGED line (finished by hand).
  - **0117** (commit d2a6fb5, xnu run 41568100167): CPU clocks measured with the PMU cycle counter, **ios23**
    (`76bbabbc…0df`): **A53 1013 MHz (DT max 1690), A73 1351 MHz (DT max 2184)**, so S-Boot leaves ~60 % of the top
    clock. DT cpufreq domains: cal-id 0xB040003 (CPUs 0-5, 208-1690 MHz), 0xB040002 (6-7, 520-2184 MHz), voltages via
    ACPM. DVFS is a later project (Samsung cal-if + ACPM sequence).
  - ios23 runner: mali_test 100 PASS; **gl_test ran on softpipe**: Mesa's `panfrost_get_model()` matches the whole
    GPU_PROD_ID, Mesa lists the G71 as 0x6000, this one reads 0x60a0. **0118** (commit cf0ec97, xnu run 41571825655)
    reports `(gpu_id >> 16) & 0xf00f` (kbase's model mask).
  - **GPU step 6 DONE (ios24, `613dffc2df2bbf93dfa938dc27121f016c14a2b97f24128ba206083306a6e71d`):**
    `gl_test: Mesa; Mali-G71 (Panfrost); OpenGL ES 3.1 Mesa 25.0.7`, the triangle correct (26 % of pixels inside, as
    geometry says), 479 ms with shader compiles. `gl_test fill` at 1125x2436: clear 3.3 ms, clear + 1 full-screen rect
    4.2 ms, + 1 textured blended 6.2 ms, 4 blended 24 ms, 8 blended 45 ms; 1310 GPU jobs, 0 faults, 0 resets.
    gl_test's "on the panel" step returns 0xe00002c7 (show is not implemented: G8). Next: G7 (EAGL / CoreAnimation). The 16 KB ramoops window is flooded by backboardd's
    `IOAcceleratorES` matching trace at ~80 s; the 30 s status line is the reliable place for results.
  - User priorities (2026-11-02): GPU (G6, then G7 EAGL for smoothness, G8, G9 Metal for wallpapers), then Wi-Fi
    (quick path: an Ethernet-style interface first), then the AppleKeyStore stub (keychain), Bluetooth, camera last.
  - PC: the user's old Android build trees in WSL `/home/Darin` were deleted on request (200 GB inside WSL);
    `gsi/lin23.2gsi-libfix.img` kept. C: still nearly full until the WSL disk is compacted.
  - `_tmp/flash-when-twrp.sh IMG SHA LOG`: waits for TWRP, saves ramoops, flashes BOOT with all checks, reboots.
  - Next for speed: IDMAC (DMA) instead of PIO.
  - Live log plan (user chose 2026-11-02): DWC3 device mode at `0x13600000` + Exynos USB PHY init, presenting
    CDC-ACM (Windows `usbser` COM port, no driver needed) instead of sweet's CDC-ECM. Not started beyond reading
    sweet's `sweet_usb.c` (patches 0072–0077, 0081).
  - **Design note (from reading HakiOS docs, oosexgang/hakios-xnu-kernel, read only):** they keep the iOS system on one
    volume and `/private/var` on a separate small HFS+ data volume. For the A8: keep the system image sparse and
    read-only, and add a fully allocated data-volume file in `/data/sweet` (1-2 GB) so eMMC writes only ever land in
    blocks that file already owns. Home on the A8 (no Bixby key, no Home button): planned as Volume Up + Volume Down
    together = HID Home; the swipe-up gesture is the long-term fix.
  - **WSL image toolchain (2026-11-02):** hfsplus.ko built from `linux-msft-wsl-6.18.54.1` (gcc 15 vs Microsoft's gcc 13:
    `module_layout` CRC differs, loads with `modprobe --force-modversion`, taints WSL's kernel only), apfs-fuse @66b86bd
    (needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` and `-include cstdint`). Scripts: `_tmp/wsl/{setup,fetch,mkroot}.sh`.
    IPSW parts in `/opt/sweet/ipsw` (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047). Repo shell scripts
    have CRLF in the Windows checkout: strip before running in WSL. DeviceTree.d22ap.im4p payload is LZFSE (`bvx2`);
    decoded with lzfse from apfs-fuse's 3rdparty to `research/ipsw-x/d22ap.adt` (186084 bytes, git-ignored).
    `tools/a8push.py` copies an image sparsely (verified: hash match, holes kept).
  - The public GitHub API allows 60 requests/hour without a token: poll every 90 s, or read the Actions page in the browser.
  - PC disk: C: was full (0 MB free) on 2026-11-02. With the user's OK, the old CI downloads (`_tmp/a8art`–`a8art6`)
    and the xnu1–xnu5 images were deleted (hashes above; rebuildable from CI). Delete `a.zip` right after unzipping.
  - Note: `shim/a8/build.sh` needs `/c/Program Files/LLVM/bin` on PATH (otherwise it silently keeps the old
    `out/Image`), and `adb push` from Git Bash needs `MSYS_NO_PATHCONV=1`.
  - **G7/G8 (2026-11-03): CoreAnimation on the Mali.**
    - 0119 (AMFI dyld policy, IOSurface import 0x7e, CPU-side fence + flush), 0120 (panel kick after each present,
      request 0x7a "shown by the GPU", AppleCLCD SweetPanelSurface/SweetFit* properties), 0121 (boot-arg
      `sweetinject=NAME`: the kernel adds `DYLD_INSERT_LIBRARIES=/sweet/lib/libsweet-gl-loader.dylib` to that program's
      environment, because launchd strips DYLD_* from plists), 0122 ('b3a8' surfaces described to Mesa as their BGRA plane).
    - `gpu/src/eagl.c` + `sweet_gl_loader.c` (from y2sios, MIT) build `libredwood-gl.dylib` locally (LLVM + ld64.lld,
      iPhoneOS16.5 SDK, `ipsw macho sign`); log in `/sweet/gl.log`, read with `tools/a8run.py gllog _tmp/gpu1/out/gl-map.json`.
    - ios26 (0120, no inject): CA still on the CPU (loader never loaded). **ios27** (0121 + `sweetinject=backboardd`): the
      GL library loads into backboardd (all 8 EAGLContext methods replaced, Mesa on Mali-G71) but CA's display target is a
      **'b3a8'** IOSurface (2 planes: BGRA + opacity), attach refused, `glDrawElements: error 0x506`, backboardd SIGSEGV
      (far 0x30) in a loop, 0 GPU submits.
    - Fix: eagl.c attaches plane 0 of 'b3a8'; 0122 (commit dde9a9d, xnu run 41596238901). The new dylib (same size,
      557056 bytes, SHA-256 `3971a8f5...ff26`) was written into root.img **in place** over the old one's blocks (HFS FIBMAP
      of the WSL copy: runs 1627813+5, 1951777+15, 3047062, 3049970, 3049983+4, 3050583+3, 3050877+88, 3051201+19; old
      content checked on the phone first, new content verified after; `_tmp/gl-inplace.sh`). File data only, so no clean
      shutdown was needed. **ios28** (`6508f79b...c1e5`, ios27 cmdline + `sweetreboot=1200`) flashed 2026-11-03 ~01:45.
    - Fallbacks if backboardd still crashes: drop `sweetinject`; boot-args `sweetgl=0` (loader off) or `glpanel=0`.
    - ios28/ios29 (0122/0123): backboardd ran 2 GPU jobs, then SIGSEGV in `IOSurfaceGetBulkAttachments` (far 0x30) from
      `CA::Render::iosurface_get_edr_factor` <- `WindowServer::IOSurface::wrap_buffer` <- `GLESContext::retain_image`:
      `CA::Render::Image::retain_iosurface` calls `IOSurfaceWrapClientImage` (IOSurfaceRoot **selector 6**; WrapClientMemory
      is 7), and SweetSurface's default case answered success with a zeroed lock result (client without a shared state
      page). **0123** refuses 7, **0124** (commit ddbb8b5, xnu run 41604655783) refuses 6 too; CA then uploads those
      pictures as textures.
    - **MILESTONE (ios30, `bdfca020…1eaa`, 2026-11-03 ~03:00): CoreAnimation draws on the Mali and the GPU puts the frame
      on the panel.** gl.log: `panel: IOSurface 1, the 1125 x 2436 picture into 27,0 1025 x 2220 by the GPU: yes`,
      `frame N (IOSurface 2/3) put on the panel by the GPU (0)`; frames 21–38 ms typical (≈20 ms of it waiting for the
      GPU, polled, no IRQ), first frames 176/539 ms with shader compiles. No crash in 1200 s; backboardd stayed up.
      Next for speed: GPU interrupts, GPU DVFS, CPU DVFS. **ios31** (`ec811190…8968`, same, `sweetreboot=0`) left booted.
  - **G9 Metal, M0/M1 (2026-11-03):** gpu run 41638836728 (commit 370653a) builds `air_run`, `libredwoodair.dylib`
    (install name /sweet/lib) and `air_metal_test` against panfrost; installed in `/sweet/bin`, `/sweet/lib`
    (`_tmp/g9-m1.sh`, `%TEMP%/edit4.sh`). Runner commands are limited to one 4 KiB block (a8run.py `block()`), so
    `_tmp/m1.cmd` holds the first 35 kernels of y2sios' rc.jobs.airmali. **M1 result (ios32, log `_tmp/m1-run.log`):
    35/35 translated, compiled by Bifrost and dispatched, none crashed; 26 without a GPU fault, 9 with one (reset,
    recovered): clearFragment01 (the draw), fbs_bistochastize_final, fbs_pcg_init, MetalFaceMask_{ConnectSpans,
    DrawEye,DrawSpans,PropagateParents,PropogateAssociations,ResolveAssociations}.** Buffer writes seen from 7 kernels
    (CalcCubeInputScaling, GenerateMask, InitMinMax, InitSpanTable, MinMax, fbs_bistochastize_init, ...); every
    AV1SW kernel's written image reads back all zero (one -1): image stores / texture reads on Bifrost are the next
    thing to check. Runner reset to `noop.cmd`; ios31 back on BOOT.
  - **M1 runs 2-6 (2026-11-03, test images ios32-34 with `sweetreboot`, normal image ios35 = 0125 kernel):**
    - 0125 (commit 09befcc, xnu run 41652928396): `hw_init()` hard-resets the G71 (soft only as fallback). **Did not
      change anything** (run 4): after any process's GPU fault the AV1SW Blit kernels still read back all zeros
      instead of (0,0,0,1), even the image's pre-filled pattern is gone, and it lasts across processes.
    - Run 5 (`_tmp/m1w.cmd`, `--zero` constants): **compute texture reads are correct.** MetalFaceMask_MinMax gives the
      same texture-derived words as softpipe (47135, 17563, 0xffffc51a) before and after a fault; its one extra
      word (128) is an out-of-bounds texelFetch (Mali 0, softpipe clamps; Metal leaves it undefined). The earlier
      MinMax mismatch was the harness's dummy constants pushing coordinates off the 4x4 texture.
    - Run 6 (`_tmp/m1v.cmd`, `PAN_MESA_DEBUG=trace,sync PANDECODE_DUMP_FILE=stderr`): the job trace of BlitLuma before
      and after a fault is **identical** (same GPU VAs, descriptors, shader) apart from CPU pointers. Every AV1SW Blit
      kernel uses scratch (TLS at 0x7fc0000, TLS size 0 = 16 B/thread) and branches on a scratch load; every
      kernel that matched softpipe is scratch-free (softpipe cannot run scratch). Guess: scratch/TLS memory goes bad
      after a fault + reset; root cause in the kernel not found (hw_init redoes everything bring-up does).
    - Fix on the translator side (commit 28a191e, gpu.yml run 41663781057): `nir_lower_scratch_to_var` +
      `nir_lower_indirect_derefs` (≤16) + `nir_lower_vars_to_scratch` for the rest, so small private arrays never
      touch scratch. Large indirect arrays would still hit the scratch problem.
    - **Run 7 (`_tmp/m1u.cmd`, 28a191e installed by a full image edit): scratch is gone from BlitLuma's Bifrost code
      (no `.tl` access, `uint[2] scratch` promoted), but the results are unchanged**: (0,0,0,1) before the fault,
      zeros after. So scratch was **not** the cause. New observation: with `--zero` BlitLuma's bounds check
      (`gid < ubo0.xy`) fails, so it stores nothing, yet its image does not read back as the upload pattern. The
      Blit images are 16-bit (`R16G16B16A16_UINT`, image_shapes), and air_run's texel printout reads every image as
      four 32-bit channels, so those numbers were never the texels. 32-bit images (DrawSpans, DrawEye) do show the
      pattern. Commit 2a9b6f3: `AIR_RUN_DUMP=1` prints raw bytes after upload and after dispatch, plus constants.
    - **Run 8 (`_tmp/m1t.cmd`, dumps): the cause.** Uploads land; the kernels write every texel before and after a
      fault; only channels 2-3 differ (1 before, 0 after). The translator's `air.write_texture_*` took its NIR
      `src_type` from the intrinsic's return type (void), so the store was a sizeless `uint`: Bifrost read four
      32-bit registers for a 16-bit vector and channels 2-3 were stale register contents (the reset zeroes them).
    - **Fix 783e17e (gpu run 41678797957): the store's type comes from the stored value** (base and bit size).
      **Run 9 (`_tmp/m1s.cmd`, installed in place with `_tmp/air-inplace.sh`, same size): every image byte is
      identical before and after a fault**; images are now `r16g16b16a16_*` and BlitKernel_Unorm copies the
      texture's texel exactly (half 1100, 200, 300, 400). BlitLuma's 0 and BlitChroma's +inf follow from the
      harness's dummy constants (unverified: softpipe cannot run 16-bit code). The post-fault problem is closed.
    - In-place installs: `_tmp/wsl/fibmap2.sh PATH` maps a file's blocks in the WSL copy (works with a dirty
      journal, read-only mount); `_tmp/air-inplace.sh` checks the old bytes on the phone, writes, verifies (compares
      only the file's own bytes in a partial last block). No clean shutdown needed when the size is unchanged.
    - One test boot (ios34) hung at `bsd_init: calling IOKitInitializeTime` and rebooted after ~25 s (first time
      seen; next boot of the same image was fine).
  - **GPU clock (patch 0126, commit d20b3d6, xnu run 41682398236, kernel `b840c334…`):** boot-arg
    `sweetgpumhz=550|747` reprograms PLL_G3D (CMU_G3D `0x11400000`, CON0 `+0x120`) with Samsung's PMS values,
    voltage left at the 0.8 V boot default; falls back to the old values if the PLL does not lock.
    - **550 MHz: verified.** 11,278 jobs and later 95,902 jobs with 0 faults from GL; GPU-bound frames ~23 → 15.8 ms
      (light) and 67.7 → 39.2 ms (heavy).
    - **747 MHz: not stable at 0.8 V** (ios38: 2860 jobs, 652 faults/resets; the self test still passed). Would need
      the voltage raised (DVFS), not attempted.
    - **ios37/ios39 were built WITHOUT `sweetreboot=0`**, and 0095's default is 90 s, so every boot of them went
      back to TWRP at 90 s (looked like a hang at `calling bsd_init`: the boot thread's stage stays there in a
      normal boot). Everyday images must carry `sweetreboot=0`, as ios31/ios35 did. **Everyday image now
      `_tmp/a8-ios42-boot.img`** (0126 kernel, 550 MHz, `sweetreboot=0`, `115841d8…`); ios37 is retired. Test images: ios36 (550, back to TWRP after
      600 s) and **ios40 (550, `sweetreboot=240`, `38c48c29…`) for short runner tests**.
  - **M2 (draws) run 1 (`_tmp/m2a.cmd`, PANDECODE trace):** the vertex/tiler job of `clearFragment01` does not
    complete. The trace shows the hand-built quad vertex shader's renderer state with **"Attribute count: 0"**
    while its code does `LD_ATTR_IMM` of attribute 0: air_run never called `nir_shader_gather_info`, so
    `inputs_read` was 0. **Fix 8e490f7 (gpu run 41691402136)**; GL draws were never affected (Mesa's frontend
    gathers info).
    - **Run 2 (`_tmp/m2b.cmd`, air_run 8e490f7 installed in place): `clearFragment01` is the first Metal fragment
      shader drawn correctly on the Mali** -- pixel (0,0) = 2.10170319e-18, 0.178894654, 1.48687698e+16,
      1.21394141e+33, bit-identical to softpipe; 0 faults, 2 jobs.
    - `clearFragment04` (writes only FRAG_RESULT_DATA1, 2 render targets): 0 faults but **render target 1 reads back
      0** on the Mali vs the softpipe value.
    - Run 3 (`_tmp/m2c.cmd`, first on the 4-minute ios40 image): 03 and 05 right; 02, 04, 08 read back 0. The
      pandecode trace of 04 shows both colour targets set up correctly (RAW128, bases `0x...8000`/`0x...c000`,
      tile-buffer offsets 0/2048), the shader's BLEND using blend_descriptor_1.
    - Run 4 (`_tmp/m2d.cmd`, `BIFROST_MESA_DEBUG=shaders`): 02 and 03 compile identically apart from the UBO
      offset. **02's values are >= 2^31 and the output was declared `ivec4`**: BLEND.s32 into a uint target
      clamps them to 0 (softpipe copies bits). **Fix 59a3d94**: air_to_nir declares an output unsigned when
      `air.arg_type_name` starts with `u`; air_run uses an SINT target for int outputs and sets every blend
      target with independent blending.
    - **Run 5 (`_tmp/m2e.cmd`): clear shaders 01, 02, 03, 05, 06, 07 all bit-identical to softpipe, 0 faults.**
      Still 0: **04 and 08, the two that write only target 1** (float and int). Run 6 (`_tmp/m2f.cmd`, air_run
      f8e3fc6 `AIR_RUN_ALL_RT=1`) prints every target to see whether the output lands in target 0 or is dropped.
    - Run 6: target 1 is never written on the Mali, by any shader (05/06 too). Run 7 (air_run 9052e25,
      `AIR_RUN_RT8=1` rgba8 targets): same at 32 bpp, so not the tile buffer -- and only pixel 0 of target 0's
      row was written.
    - **Run 8 (air_run 108e30b, every pixel printed): even single-target 01 writes only pixel (0,0).** Every
      "match" in runs 2-5 compared pixel (0,0) alone. Cause: Panfrost's `bifrost_preprocess_nir` runs
      `nir_lower_viewport_transform`, which rewrites only a `store_deref` of gl_Position; air_run lowered I/O
      first, so positions reached the tiler in clip space (a 2x2-pixel quad at the origin). **air_metal.c's
      finalize_for_gallium has done the transform itself since 25 October; air_run's copy never got it. Fix
      6cfc55a.**
    - **Run 9 (`_tmp/m2i.cmd`, air_run 6cfc55a): render target 0 is bit-identical to softpipe on all 16 pixels for
      all 8 clear shaders (01-08), 0 faults. M2's single-target draw path works.** Render target 1 is still all
      zero on the Mali for every shader that has one (04-08), so that is a separate bug, still open (MRT).
  - **G9 M3 run 1 (2026-11-03): Metal has a device on the A8.** Kernel **0127** (commit 3e4cab1, xnu run 41707992044,
    `_tmp/a8art39`) publishes IOAcceleratorES "sgx" under RedwoodMali without the plug-in properties; with
    `sweetmetal=1` user space may set them. gpu run 41708355388 (commit 43f0a64) builds y2sios' RedwoodMetal bundle,
    metal_props, metal_test (`gpu/src`, paths /redwood -> /sweet). Installed by full image edit (`_tmp/g9-m3a.sh`,
    `%TEMP%/edit5.sh`, after Settings > General > Shut Down): `/sweet/lib/RedwoodMetal.bundle`, symlink
    `/System/Library/Extensions/RedwoodMetal.bundle`, `/sweet/bin/metal_{props,test}`, libredwoodair from that run.
    Test image ios41 (`cadf838a…`, 0127, 550 MHz, `sweetmetal=1 sweetreboot=240`), runner `_tmp/m3a.cmd`, log
    `_tmp/m3a-run.log`: before the switch `MTLCreateSystemDefaultDevice() = nil`; after `metal_props on`
    **`MTLCreateSystemDefaultDevice() = RedwoodMetalDevice`**, `RedwoodMali client opened`, `the compiler library
    Mali-G71 (Panfrost)`; switch off again at the end. backboardd stays on GL (it starts before the switch).
    - Cosmetic: the device calls itself "Mali-G77 MP11 (product 0x6000, Redwood)" (y2sios' name string); its
      `targetDeviceInfo` asks for `pipelines.Mali-G77MP11(...)`. To fix: name from GPU_ID (G71 MP2).
    - Not Metal: CoreImage's default and software CIContexts SIGSEGV in this process before and after the switch;
      CIContext over EAGL works (filter result correct).
    - Next: y2sios' `metal_drv_test.m` (compute + render through real Metal calls), then a Metal consumer
      (wallpaper / PosterBoard) with the properties set before launchd.
    - CI caveat: gpu.yml reports success when air_run fails to compile (c5f4c8b: two printf strings split across
      lines); the artifact then simply has no `air_run`. Check the file list, not the run's colour.
  - **G9 wallpaper, then the whole system on Metal (2026-11-03).**
    - Why the wallpaper is missing (crash reports, GL boots): PosterBoard aborts in
      `-[PBFHomeScreenPreviewImageBuilder _imageForView:]`. It makes a CARenderer over an MTLTexture.
      `MTLCreateSystemDefaultDevice()` is nil, so the nil command queue goes into an NSDictionary and throws.
      PosterBoard needs Metal.
    - xnu 0128: boot-arg `sweetmetalfor=NAME,...` publishes IOAcceleratorES with both properties set, but
      `copyProperty`/`serializeProperties` hide them from processes not on the list (p_comm, 16 chars).
      `sweetmetalfor=*` means every process.
      - Verified on runs 3 and 5: "Metal device hidden from backboardd / SpringBoard / chronod".
      - With PosterBoard listed it opened the GPU (run 1) but died without submitting. In idle boots it never
        asked for Metal within 6 min: it draws only on demand.
    - Logging, on this kernel:
      - Console writes from user space do NOT reach the kernel message buffer, and memorystatus fills that
        buffer in under a minute.
      - New: `sweet-dmesg [TEXT...]`, plus `-s BYTES` to grow kern.msgbuf.
      - xnu 0129: RedwoodMali request 0x79 writes one client line into the kernel log as `m2m NAME[PID]: ...`.
        m2m.h uses it when there is no /dev/m2m.
    - **Boot-arg `sweetgpumhz=550` is required** for the 550 MHz clock; without it the GPU stays at 343
      (ios43/44/45 were missing it at first).
    - ios46 (`sweetmetalfor=*`): SpringBoard crash-loops on "Failed to get MPSDevice"
      (MPSImageGaussianBlur). Fix: `metal_profile=1` (claims Apple2, the lowest rung MPS asks about).
    - ios47 (`*` + `metal_profile=1`): no crash loop, but the **screen stays black**. The GPU is nearly idle
      (8 jobs per 30 s after its self-test), so backboardd submits almost nothing. Suspect: the compositor
      waits on a pipeline that never compiles, as ios-a5xelte found (`fixed_frag_lph_cph`; orhunnx/ios-a5xelte,
      MIT, A5XELTE_PROGRESS.md, Mali-T720 port).
    - ios48 (`a7d0114e…`, kernel 0129, RedwoodMetal with kernel-log records, `sweetreboot=0`): flashed 18:08,
      runner `_tmp/w8.cmd`. **Darin keeps test images flashed: no flash-back to ios42.**
    - Run 9 (ios48 + 49b6591, XR formats 550/551/554/555 served as BGRA8 in libredwoodair and the surface
      import): no format errors and no Metal errors at all, 14 CA pipelines compile, but backboardd still makes
      **0 command buffers**. Thread samples: CA's render-server and IOMFBServer threads idle in mach_msg; one
      backboardd thread blocks forever in `dispatch_group_wait(..., FOREVER)` at backboardd `0x100018a74`
      (func_100018a58, a block). Something it waits for never finishes on the Metal path (not yet traced).
    - Run 10 (ios49 `5df45a4b...`, kernel a8art42): **hybrid**. backboardd/SpringBoard on GL, `sweetmetalfor=`
      PosterBoard plus the poster extensions (CollectionsPoster, PhotosPosterProvider, Emoji/Gradient/Unity/Pride
      PosterExtension, WeatherPoster, AegirPoster, ExtragalacticPoster, LegacyPoster), `metal_profile=1`,
      `sweetreboot=0`; runner `_tmp/w10.cmd` (10 min). mkpayload's boot-args limit is 607 bytes, silently cut:
      this line is 566.
    - Working files moved to `C:/Users/Darin/iOS` (`ios-sweet/`, `_tmp/`) on 2026-11-03.
    - **Why there is no wallpaper and no widget (2026-11-04, runs 10-15).** Not the GPU, not GL, not the clock.
      - PosterBoard runs, never asks for Metal, and its extension store is empty. `sweet-ext` (gpu/src/sweet_ext.m,
        commit 43abbb2): NSExtension and ExtensionKit find 0 extensions for `com.apple.posterkit.provider` and for
        `com.apple.widgetkit-extension`; LaunchServices has 1 plugin registered of 628 `.appex` on the root.
      - Setting aside the LaunchServices store (`*.csstore` in the lsd container) so lsd rebuilds it, three times,
        gave the same 1 plugin. With the clock right (xnu 0130, boot-arg `sweettime=`, commit 481d898, ios51) too: the
        1970 clock was not the cause.
      - **Cause, from the unified log of the ios51 boot** (read on the PC with Mandiant `unifiedlog_iterator` v0.7.0,
        Apache-2.0, in `/opt/sweet/uli` in WSL): installd/lsd `MICopyExtensionCacheEntryWithPlatform: Extension cache
        does not have an entry for com.apple.widgetkit-extension` (also intents-service, diagnostics-service,
        usernotifications.content-extension, ...); `-[MIPluginKitBundle extensionPointOverlay]: Failed to fetch
        extension cache entry`; `+[MILaunchServicesDatabaseGatherer enumerateAppExtensionsInBundle...]: Ignoring app
        extension ... due to validation issue(s)`; pkd (PlugInKit): `Candidate plugin count from LaunchServices: 0`,
        `could not create extension point record for com.apple.widget-extension: ... Code=-10814`.
        So iOS's cache of extension POINT definitions is missing/empty on this root, every extension is rejected as
        invalid, and no poster or widget is ever registered.
      - **The cache file is `/System/Library/xpc/extensions.plist`** (104731 B, plus `extensions.plist.sig`, 1852 B), and it
        DOES hold the entries (`com.apple.widgetkit-extension`, `com.apple.posterkit.provider`,
        `com.apple.intents-service` are all keys). The code that reads it is in ExtensionFoundation
        (`extensionCacheFileURLs`; the string `System/Library/xpc/extensions.plist` is at dsc 0x19375445c); the lookup
        that fails is `MICopyExtensionCacheEntryWithPlatform` in InstalledContentLibrary (log string at dsc
        0x1b124c485), reached from `-[MIAppExtensionBundle extensionCacheEntryWithError:]`. Both cryptex roots
        (`/private/preboot/Cryptexes/{OS,App}`, filled by mkiosroot, 3.2 GB) have no `xpc/extensions.plist`.
      - So the data is there and iOS still reports no entry. Not yet determined why. Candidates, cheapest first:
        (1) lsd/installd never open or cannot read the file (sandbox, or the ignition_level=1 path): log opens of
        `extensions.plist` from the kernel or run `sweet-ext` through ExtensionFoundation's own loader;
        (2) the `.sig` is checked (launchd.plist.sig sits next to the patched launchd.plist and launchd still works, so
        signature checks are lax elsewhere, but this one may not be); (3) the lookup is keyed by a platform value that
        this ADT/MobileGestalt gives differently (`MICopyExtensionCacheEntryWithPlatform` takes a platform).
        Tools: `ipsw dyld extract DSC IMAGE --output DIR` then `ipsw macho disass --vaddr`; symbols are stripped, find
        code by string xrefs (`ipsw dyld str DSC --pattern ...` gives the image and address).
    - ios50 (`6b827b62...`, no GL: no `sweetinject`, `sweetgl=0`): icons round (GL gives square icons: our GL path
      misses the icon mask), choppy, SpringBoard respawns often, PosterBoard crashes repeatedly. ios51 (`08a6713b...`,
      kernel a8art43 = 0130, GL hybrid, `sweettime=` baked in at build) is on the phone, `sweetreboot=0`.
    - TWRP zip: `tools/mka8flashzip.py` (f847203). `gpu/src/sweet_settime.c` (f8c4739) is superseded by xnu 0130.
    - **2026-11-05..10: boot menu, reinstall, first true Metal-only run.**
      - `shim/a8/menu.c` (`EXTRA_CFLAGS=-DA8_MENU`, commits 54447e0, 30e0a39): menu on the S-Boot framebuffer, 5 s
        countdown then the payload's own line; presets GL+Metal posters / GL only / Metal only / neither, toggles, reset.
        Buttons: register `0x11CB0064`, bits 5 (vol up), 6 (vol down), 7 (power), low when pressed (probe build
        `-DA8_BUTTON_PROBE`, verified on the phone). Works (user).
      - The earlier ScreenTimeAgent/calaccessd/cloudd/mobileactivationd crash loop was damaged state in the old root
        (not the clock): a fresh root has none of it.
      - Darin formatted Data. **Fresh root** `_tmp/wsl/mkfreshroot.sh` + `_tmp/freshfix.sh`: mkiosroot 13G + tweaks, then
        only `/sweet`, the RedwoodMetal link and the `com.sweet.run` job from the old root; pristine run.cmd/run.log/gl.log
        (`a8run.py prepare`), maps in `_tmp/fresh-maps/` (cmd block 3043267). WSL: `/opt/sweet/fresh/ios16-root-fresh.img`
        (`8085eb4f...`), base without our layer `ios16-root-base.img`. On the phone: `/data/sweet/root.img` **inode 913922**
        (108 extents, none unwritten), pushed by `_tmp/g9-inst.sh` (a8push --dense from `//wsl.localhost/Ubuntu/...`).
        Boot image **ios55m** (`0ed00454...`, kernel a8art43, menu, `sweetrootino=913922`). `/sdcard`: flash-ios55m.zip,
        flash-android.zip, a8bak/BOOT.img restored. First boot fine: only driverkitd crashes.
      - WSL lost hfsplus after a restart: `insmod -f /opt/sweet/linux/fs/hfsplus/hfsplus.ko`.
      - gpu.yml moved to macos-15 (macos-14 runners refused, 1aa1ccc); xnu.yml still says macos-14 (needs Xcode 15's iig).
      - `sweet-ext --cache`: ExtensionFoundation's signed load of extensions.plist works (138 keys), so the cache is
        fine; `sweet-ext --platform` (e26848c) is written, not yet run.
      - The "stuck thread" of run 9 (backboardd 0x100018a58) is `-[BKButtonHapticsController _configureAsyncWithBlock:]`
        waiting for haptics: unrelated to drawing.
      - **Run 24, true Metal-only (menu preset: no sweetinject, sweetgl=0, sweetmetalfor=*, metal_profile=1): black.**
        backboardd opens the device at 107 s, compiles 14 CA pipelines, makes/frees 23 small textures (776x24: a
        warm-up), then makes no Metal call at all (0 command buffers, no IOSurface texture, no refusal logged) while
        it swaps frames from 115 s on (surfaces 2/3, 'b3a8'). With Metal hidden and no GL (ios50) CA draws on the CPU
        and the picture shows; with Metal visible it shows nothing. So CA takes its Metal path and stops before the
        first Metal call of a frame. Next: log every device/queue method CA calls after the warm-up (a call counter
        that prints even when idle), or read `CA::OGL::MetalContext::set_destination_iosurface` / `update_surface`
        (0x188302550, 0x1883163e0) for the check that fails.
      - **Why no extension registers (found 2026-11-10 in lsd's unified log of the fresh root):** `Failed to read
        extension point cache at URL ... Error Domain=com.apple.extensionKit.errorDomain Code=12 "Unable to find AMFI"`.
        `-[EXExtensionPointEnumerator initWithSDKDictionary:urls:config:]` (ExtensionFoundation, dsc 0x19373f038) reads
        `/System/Library/xpc/extensions.plist` with `_EX_dictionaryWithSignedURL:` when `enforceXPCCacheCodeSigning`
        is on, which needs the AMFI kernel service this kernel lacks; only the 6 `.appexpt` points get registered, so
        every appex fails "Extension cache does not have an entry" during the LaunchServices gather.
        (`sweet-ext --platform`: only platform 2 finds an entry once the DB is seeded; a patch forcing platform 2 in
        `MICopyExtensionCacheEntryWithPlatform` changed nothing and was reverted.)
      - **CONFIRMED by the user 2026-11-10 (run 28, default GL + Metal posters):** with the patch below the wallpaper
        picker lists wallpapers and the widget picker shows more widgets. New problem: the wallpaper previews are
        black or black with white garbage, and AegirPoster crashed at least once. Run 28's log ended (520 s) before
        the picker was opened, so the poster processes' `m2m` lines are not captured yet: run 29 (`w29.cmd`) dumps
        `CRASH Poster m2m` at 8 and 15 min.
      - **Patch on the phone (in place):** dsc file `.05` offset 0x5b5b328 (VA
        0x19373f328) `tbz w0,#0,+0xc0` (00060036) -> `b +0xc0` (30000014): always the plain read. Image block 2301174,
        in-block 0x328; originals in `_tmp/efblk.orig`, list in `_tmp/dscpatch.txt`. To make lsd reseed without an HFS
        edit: zero the first 4 KiB of both `*.csstore` files (find block 0 with FIBMAP on the WSL copy; they move at
        each rebuild). A rebuilt root image must get the same patch (or a kernel AMFI stub).
      - `sweet-run` keeps 8 boots in `/sweet/runs.log` (252c573, gpu run 42947899394), `a8run.py runslog`; built, not
        yet installed. All boots share the baked `sweettime`, so unified-log timestamps of different boots overlap.
    - Process lessons:
      - Flash scripts take a lock (`/tmp/g9-run.lock`). Two copies once raced: one replaced the test commands
        with noop.
      - A forced power-off leaves the HFS+ journal dirty. Before editing the root again, boot ios42 once and
        shut down from Settings.
- **Nested panics:** the caller `0xfffffff007a2f754` is `_panic_with_options` itself, i.e. the panic path
  re-entering this early in boot. They're a side effect of the first panic, not a separate bug.
- **User's storage plan (2026-11-01, not started):** dual boot, Android keeps ~8 GB of USERDATA and the rest
  (~12 GB) becomes a new iOS root partition.
  - This means a GPT change: back up the GPT/PIT first and change only the USERDATA entry. Android's USERDATA
    must be reformatted once (FBE, can't be shrunk in place).
  - Samsung's PIT may make later Odin stock flashes fail or restore the stock layout.
  - Needs the user's explicit OK when the time comes; the BOOT pre-approval doesn't cover it.
  - Switching could be `dd` of BOOT from TWRP, or later the shim chainloading Android's kernel from a spare
    partition (e.g. hold Volume Down). Only after the XNU eMMC root driver exists.
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
  **Exception (user, 2026-11-01):** new test images may be flashed to `BOOT` without asking each time. Keep the
  checks (hash on /sdcard, BOOT = `mmcblk0p12`, backup hash, readback). Anything else (USERDATA etc.) still needs
  the user's OK.
- Keep dumps, serial numbers and IMEI-bearing partitions out of git. The live FDT and `bootargs` contain the
  serial; use the redacted copy under `research/a8/`. The user's partition backups stay on their PC.
- Recovery path for a bad BOOT: force off (Power + Volume Down ~8 s), TWRP (Power + Volume Up; the A8 has no Bixby key, user 2026-11-02),
  `dd` the saved `BOOT.img` (SHA-256 `c87a1336e8e6ca031b402cce84add38c9ec24172e3293de868cd7b05d6c9a59d`) back.
  Download mode: combo unverified (no Bixby key on the A8; ask the user). Stock firmware for Odin can come from SamFw (user has the tool);
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

### TWRP flash zip (2026-11-03)

`python tools/mka8flashzip.py <image>.img flash-<name>.zip` makes a zip Darin can flash from TWRP (Install → zip → swipe)
without a PC: it checks BOOT = mmcblk0p12, the Android backup's hash and the image's hash, writes BOOT, reads it back
(restores the Android backup on a mismatch) and does not reboot. `flash-ios42.zip` (the daily GL build, image
115841d8…) is on the phone's /sdcard. Images and zips stay out of git.
