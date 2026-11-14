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

## 2026-11-13: facts for the full audio build (Samsung lineage-18.1 7885 kernel, read for facts only)

- **The codec is never driven over SPEEDY by the AP.** `cod3035x.c` reads and writes it through ACPM
  (`exynos_acpm_read_reg/write_reg(ACPM_ADDR_CODECA = 0x07, reg, val)`); ACPM owns the SPEEDY bus it shares with
  the PMIC (type 0x01). Doing the same is also the safe way (no collision with ACPM's PMIC traffic).
- **ACPM IPC** (`drivers/soc/samsung/acpm/acpm_ipc.c`, `acpm_mfd.c`, `include/soc/samsung/acpm_mfd.h`,
  `fw_header/framework.h`):
  - DT `acpm_ipc`: mailbox `0x11C40000` (INTGR0 0x08, INTCR1 0x20, INTMR1 0x24, INTSR1 0x28), APM SRAM `0x02052000`
    (0x1B000), `initdata-base` 0x4B80 = `struct acpm_framework` (u32s: plugins, num_plugins, ipc_channels,
    num_ipc_channels, pid_framework, pid_max, ipc_ap_max, ...). `ipc_channels` = SRAM offset of `struct ipc_channel[ipc_ap_max]`:
    {id, field, owner, type, channel_info{rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size,
    tx_rear, tx_front, tx_base, q_len, q_elem_size, credit, mbox_addr}, ap_poll} (all u32, offsets into SRAM).
  - AP->APM queue = the channel's **rx_*** fields; APM->AP queue = its **tx_*** fields.
  - Send: wait until (front+1)%len != rear; cmd[0] |= seq (1..63) << 16; copy q_elem_size bytes to base + size*front;
    front = (front+1)%len; ring APM: INTGR0 = (1 << id) << 16. Response: poll INTSR1 bit id, scan the APM->AP queue
    from rear to front for the entry whose seq ((w0 >> 16) & 0x3f) matches, copy it, rear++; if rear == front,
    INTCR1 = 1 << id.
  - MFD channel = the PMIC node's `acpm-ipc-channel` = **2**. Read: cmd[0] = (type & 0xf) << 8 | reg, cmd[1] = 0
    (FUNC_READ); reply cmd[1]: value at bits 15:8, return code 31:24. Write: cmd[1] = 1 (FUNC_WRITE) | val << 8.
    Update: cmd[1] = 2 | val << 8 | mask << 16.
- **COD3035 driver functions** to take the sequences from: `cod3035x_codec_initialize` (4433), `cod3035x_enable`
  (5528), `vmid_ev`, `dac_ev`, `cod3035x_hp_playback_init`, `hpdrv_ev`/`post_hpdrv_normal`,
  `cod3035x_lassenA_normal`, `playback_hw_params`, `cod3035x_dai_set_fmt`; microphone: `cod3035x_mic_bias_ev`,
  `cod3035_power_on_mic1/mic3`, `cod3035x_capture_init`, `adc_ev`, `mic1_pga_ev`, `capture_hw_params`.
  Routing control names in the vendor image's `etc/mixer_paths.xml` (PC copy `_tmp/vendor-fw` has the firmware;
  the XML is in the same vendor.img).
- **iOS side: Scion's `userland/src/audio_plugin.c`** (scion-kernel @2a79ef8, **MIT**): an AudioServerPlugIn in
  /System/Library/Audio/Plug-Ins/HAL presenting "Speaker" (writes /dev/hakios-audio), "Hawking" (companion the
  iPhone X routing tables need), "Actuator" (Taptic waveforms -> /dev/hakios-haptic) and "Codec" (mic from
  /dev/hakios-mic), for the iPhone X (D22) routing tables we also run. Kernel side `drivers/audio/hakios.audio.c`,
  `hakios.mic.c`, `hakios.guard.c` (**APSL-2.0**, reusable in XNU with notices). Header `userland/src/hakios.audio.h`.
