# Audio on the Galaxy A8 (2018), `jackpotlte` — plan and facts

Status (2026-11-12): **research only, nothing built.** User asked for audio before Wi-Fi.

## Why audio before Wi-Fi

- Wi-Fi is Samsung's own SCSC "wifibt" block inside the Exynos 7885 (`samsung,scsc_wifibt@120c0000`, mailboxes to
  its own CM4/CR4 cores), not Broadcom. It needs Samsung's closed firmware, its HIP protocol, a network interface iOS
  accepts and a host-side WPA supplicant. Nobody has done it for iOS. Weeks of work.
- Audio has a precedent: Scion (github.com/Scion-Project/scion-kernel, `drivers/audio/abox/hakios.exynos_abox.c`,
  **APSL 2.0** like XNU, so reusable with its notices kept) drives the S9's ABOX from the AP **with ABOX's own CPU
  off, no firmware**: RDMA0 (memory) -> SPUS -> SIFS1 -> UAIF (I2S master) -> amplifier, in 8 boot-arg steps
  (`abox=N`: 1 looks, 7 plays a test tone, 8 serves iOS through `/dev/hakios-audio`). It names its fact sources in
  Samsung's GPL kernel, read for facts and not copied.

## A8 facts (from the stock BOOT.img device tree, rev07)

| Part | Fact |
| --- | --- |
| ABOX | `samsung,abox` SFR `0x14A50000` (64 KB), SYSREG `0x14820000`, SRAM `0x14B00000` (160 KB); SysMMU `0x14A30000`; ABOX's own GIC `0x14AF1000`/`0x14AF2000` (SPI 0x77); RDMA0..7 `0x14A51000 + 0x100*n`, WDMA0..4 `0x14A52000 + 0x100*n` |
| Power | ABOX is in **pd-dispaud** (`0x11C84020`, cal_id `0xB1380000`) — the **display's** domain, so it is already on while the panel works (unlike the S9's separate AUD domain) |
| Memory | `abox_rmem` `0xE9400000`, 4 MB (below 4 GB: ABOX's DMA ring can live here) |
| Clocks | `pll, ca7, audif, bclk0, bclk2, bclk3` (+ gates) from CMU `0x12060000` (ACPM channel 0) |
| Headphones / earpiece / mics | **UAIF0 <-> COD3035** (`codec,cod3035x`), which is on the **SPEEDY bus** `0x11CE0000` at address 3, **shared with the PMIC** (`s2mpu08`, address 0, also used by ACPM): codec writes must not collide with ACPM's PMIC traffic |
| Codec pins | MCLK `gpb0-0` fn 2; bus `gpb0-1..4` fn 2 (pinctrl `0x148F0000`, needs SMC per pd's `need_smc`) |
| Speaker | **UAIF3 -> NXP TFA98xx** at I2C `0x34` on `i2c@13860000`; its speaker-protection DSP code (`tfadsp.bin`) runs **on ABOX's CPU** in stock. Without it there is no speaker protection: a Scion-style digital guard (high-pass + limiter, low volume) is required |
| Speaker pins | `gpb2-0..3` fn 2 |
| Stock firmware names | `tfadsp.bin`, `AP_AUDIO_SLSI.bin`, `APBargeIn_AUDIO_SLSI.bin` (ABOX ext_bin); calliope images. Not needed for the bypass path |

## Plan

1. **Read the facts** the A8 differs in from the S9: the 7885 ABOX register layout (UAIF0 CTRL, SPUS/route, RDMA),
   CMU_DISPAUD clock registers and PLL_AUD, pinctrl `0x148F0000` bank layout, SPEEDY protocol and the COD3035
   headphone power-up sequence. Source: Samsung's SM-A530F kernel (GPL-2.0), facts only.
2. **Kernel driver, steps like Scion's** behind a boot-arg, each reversible and logged: look (read only) -> clocks ->
   pins -> UAIF0 -> route -> stream a quiet test tone to the **headphone jack** via COD3035.
3. **Serve iOS**: a `/dev` sink plus a CoreAudio HAL plug-in in userland (Scion has one to learn from).
4. **Speaker** after that, through UAIF3 and the TFA98xx with a digital guard.
5. Microphone later.
