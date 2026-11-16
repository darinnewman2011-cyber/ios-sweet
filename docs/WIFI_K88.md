# Wi-Fi on the ZTE K88 (MSM8952), as a real Wi-Fi interface in iPadOS Settings

Research notes, 2026-11-15. Nothing here is built yet. "Verified" means checked on the K88 or in a primary source;
everything else is a plan or a guess.

## Two halves

1. **Hardware**: make the Qualcomm Wi-Fi core send and receive 802.11 frames.
2. **iPadOS**: make Settings > Wi-Fi list networks, join one, show signal bars, and route traffic over it.

## 1. Hardware: WCNSS (Pronto) + WCN36xx

- The digital Wi-Fi/Bluetooth core (WCNSS, "Pronto") is inside the SoC; the RF part is a separate WCN36xx chip (wcn36xx
  docs; the K88 chip variant is still to be read from its DTB, `qcom,wcnss-wlan` / `qcom,pronto` nodes).
- **Boot the core (PIL):** Pronto runs its own firmware (`wcnss.mdt` + `wcnss.b00..bNN`, or a squashed `wcnss.mbn`).
  The application CPU cannot load it directly: TrustZone checks and starts it through SCM calls (PAS: init image, memory
  setup, auth-and-reset, peripheral id 6). Linux: `qcom_wcnss` remoteproc (GPL, facts only). The firmware files come
  from the K88's own Android partitions (`modem` FAT image / `/system/etc/firmware`), not from git.
- **Talk to it (SMD):** shared-memory channels (SMEM) with an IPC interrupt. Channel `WCNSS_CTRL` first: the host pushes
  the calibration file `WCNSS_qcom_wlan_nv.bin` (from `persist`/vendor). Linux: `qcom_smd` (GPL, facts only).
- **Drive it (HAL):** channel `WLAN_CTRL` carries the WCN36xx HAL messages (start, scan, join, config BSS, add station,
  set keys, ...); frames go through the DXE DMA rings. Linux `drivers/net/wireless/ath/wcn36xx` carries an ISC-style
  permission notice (Copyright 2013 Eugene Krasnikov): reusable with credit, unlike the GPL parts. Confirm the exact
  header of each file before borrowing.
- wcn36xx is a "soft" design: the host builds authentication/association frames and runs the WPA handshake (Linux uses
  mac80211 + wpa_supplicant). So the K88 needs an 802.11 stack.

## 2. iPadOS: what Settings talks to

- Settings > Wi-Fi uses the private MobileWiFi framework, which talks to `wifid`; the Apple80211 code is compiled into
  `wifid` (Levin, newosxbook). `wifid` opens a socket and drives the interface with ioctls (`Apple80211Open` issues
  ioctl `0xc02869c9`, an `_IOWR('i', 201, 40-byte struct)`; very likely SIOCGA80211 with `struct apple80211req`;
  SIOCSA80211 is the set side). Request IDs are in Apple's old 10.5-era `apple80211_ioctl.h` / `apple80211_var.h`.
- Since iOS 13 the kernel side is IO80211FamilyV2 (Black Hat 2020, "Dive into Apple IO80211FamilyV2"): `wifid` also
  finds the interface through IOKit (IO80211 controller/interface classes) and Skywalk. Exactly which classes, properties
  and request IDs iPadOS 16.6.1's `wifid` needs is the main unknown: read it from the iPad dsc / `wifid` binary.
- Apple's real Wi-Fi kexts (IO80211Family, AppleBCMWLANCore) are closed and built for Apple's kernel (the iPad 7
  kernelcache is 16K pages): they cannot be loaded into our kernel. Our kernel must provide the pieces `wifid` expects.
- Precedent on macOS: OpenIntelWireless **AirportItlwm** makes Intel Wi-Fi show as native Wi-Fi by plugging into
  IO80211Family and answering the Apple80211 requests (scan results, SSID, RSSI, association, keys); it uses Apple's RSN
  supplicant for WPA/WPA2-Enterprise. Its sources are the best reference for the request handling (check its license per
  file before borrowing anything).

## Plan

**Stage A, internet first, "wired" style (fast).** A kernel driver brings up Pronto, joins one network given by boot-arg
or a config file, does WPA2-PSK in the kernel, and attaches a plain Ethernet-type `en` interface. iPadOS sees a wired
network: Safari and apps work, Settings shows no Wi-Fi list. 802.11 + WPA2-PSK: OpenBSD `net80211`
(BSD/ISC licensed) does the 4-way handshake in the kernel (`ieee80211_pae_input.c`); the HAL side follows wcn36xx.

**Stage B, native Wi-Fi in Settings.** Same driver, plus:
- a kernel object named and shaped like the IO80211 controller/interface `wifid` looks for (IOKit class names and
  properties taken from iPadOS 16.6.1's `wifid`),
- handlers for the Apple80211 get/set requests on the interface (scan, scan results, SSID, BSSID, RSSI, channel, state,
  associate/disassociate, set keys / let `wifid`'s supplicant drive the handshake),
- link-state and scan-done events back to `wifid` (kernel events / IOKit notifications; to be read from `wifid`).
Then Settings lists networks, asks for the password and shows the bars.

**Order:** after the Adreno GPU (Darin's priority, 2026-11-15). Hardware bring-up first in the shim (PIL + SMD + HAL
version reply), as with every K88 block.

## Open questions
- K88 WCN chip variant, Pronto version, and where its firmware and NV files live on the tablet (read-only from TWRP).
- Which SCM/PAS calls this TrustZone accepts from a non-Linux kernel (the shim can probe them safely: PAS "supported"
  query first).
- iPadOS 16.6.1 `wifid`: IOKit matching, request IDs used at startup, event delivery.

## Sources
- Levin, "Apple 80211 - 28 Days Later": https://newosxbook.com/articles/11208ellpA-II.html
- Black Hat USA 2020, "Dive into Apple IO80211FamilyV2": https://i.blackhat.com/USA-20/Thursday/us-20-Wang-Dive-into-Apple-IO80211FamilyV2.pdf
- OpenIntelWireless itlwm / AirportItlwm releases: https://newreleases.io/project/github/OpenIntelWireless/itlwm/release/v1.2.0
- Linux WCNSS PIL binding: https://www.kernel.org/doc/Documentation/devicetree/bindings/remoteproc/qcom%2Cwcnss-pil.txt
- wcn36xx source (header/license): https://code.woboq.org/linux/linux/drivers/net/wireless/ath/wcn36xx/wcn36xx.h.html
- OpenBSD net80211 in-kernel WPA handshake: https://src.rcs.uwaterloo.ca/xref/openbsd-src/sys/net80211/ieee80211_pae_input.c
- MSM8916 DB410c Wi-Fi firmware threads: https://lore.kernel.org/all/YLX4TfuxEnjQ3fhi@gerhold.net/

## K88 facts (step 1, 2026-11-15)

Source: the JASMINE DTB (`backup/k88/jasmine.dtb`, decompiled with a small FDT dumper; no dtc on the PC). "dtb" = read
there, not yet seen running. The TWRP dmesg shows `wcnss_wlan probed in built-in mode` and the pronto PIL driver deferring:
TWRP never boots the Wi-Fi core, so nothing below is verified live yet.

| Item | Value | Mark |
| --- | --- | --- |
| PIL node | `qcom,pronto@a21b000`, `qcom,pil-tz-generic`, reg `0x0A21B000` (0x3000), IRQ SPI 0x95 (watchdog bite), **PAS id 6**, firmware name `wcnss` (`wcnss.mdt` + `.bNN`), smem-id 0x1A6 (SSR crash reason), sysmon id 6 | dtb |
| PIL memory | `linux,contiguous-region` = `reloc_mem` **`0x8C200000`, 0x1700000 (23 MiB)**, shared in the DT with an ION heap and one more node; the firmware's ELF program headers decide where inside | dtb |
| PIL clocks (TZ auth) | proxy clocks `xo`, `scm_core_clk`, `scm_iface_clk`, `scm_bus_clk`, `scm_core_clk_src` at 80 MHz (`0x4C4B400`): the crypto engine (CE1) clocks TZ needs to check the signature | dtb |
| PIL proxy supply | `vdd_pronto_pll` = **PM8950 L7** 1.8 V, 18 mA, held until the core reports ready | dtb |
| PIL handshake (SMP2P from wcnss) | err-fatal = bit 0, err-ready = bit 1, proxy-unvote = bit 2, stop-ack = bit 3 (incoming, `smp2p` entry, remote pid 4); force-stop = outgoing bit 0 | dtb |
| WLAN node | `qcom,wcnss-wlan@0a000000`, `qcom,wcnss_wlan`: `wcnss_mmio` `0x0A000000` (0x280000), `pronto_phy_base` `0x0A21B000`, `pronto_a2xb_base` `0x0A100400`, `pronto_ccpu_base` `0x0A205050`, `pronto_saw2_base` `0x0A219000`, `pronto_mcu_base` `0x0A080C00`; tx/rx IRQs SPI 0x91 / 0x92 (the DXE rings); flags `is-pronto-v3`, `has-pronto-hw`, `has-autodetect-xo`, `has-vsys-adc-channel` | dtb |
| Pronto rails | vddmx = PM8950 L3 (corner, `0x180` = turbo level), vddcx = PM8950 S2 (corner 0x100..0x180), vddpx = **L5** 1.8 V | dtb |
| Iris (RF chip) rails | vddxo = **L7** 1.8 V 10 mA, vddrfa = **L19** 1.3 V 100 mA, vddpa = **L9** 3.3 V 515 mA, vdddig = **L5** 1.8 V | dtb |
| Rail control | all of these are `qcom,rpm-smd-regulator` (RPM owns them; K88 notes: a direct SPMI write to L8 was ignored). L3/S2/L5 are system rails that are on anyway; **L7, L9 and L19 may be off** and would need RPM requests over the `rpm_requests` SMD channel. To check: read their SPMI status (SID 1, `0x4000 + (n-1)*0x100`, read is allowed) | dtb / plan |
| Iris clock | `rf_clk` (an RPM clock, the PMIC RF clock buffer that feeds the RF chip's XO), `xo` | dtb |
| Iris pins | 5-wire bus to the RF chip on TLMM GPIOs **76-80** (function 1, 6 mA, pull-up; sleep = 2 mA pull-down) | dtb |
| SMEM | `0x86300000`, 1 MiB; aux-mem1 `0x00060000` (0x8000, RPM message RAM), target-info reg `0x0193D000`. HW spinlocks `qcom,ipc-spinlock-sfpb` at `0x01905000` (8 locks) | dtb |
| Outgoing IPC register | **`0x0B011008`** (APCS IPC) for every edge; WCNSS bits: **SMD 17 (`0x20000`)**, SMSM 19 (`0x80000`), SMP2P 18 (`0x40000`). RPM SMD = bit 0 | dtb |
| Incoming IPC IRQs (from WCNSS) | SMD SPI **0x8E**, SMP2P SPI 0x8F, SMSM SPI 0x90 (edge-triggered) | dtb |
| SMD edge | `qcom,smd-wcnss`, edge **6**, label `wcnss` (channels: `WCNSS_CTRL`, `WLAN_CTRL`, `APPS_RIVA_BT_*`, `APPS_FM`, IPC router) | dtb |
| IOMMU | context `pronto_buf` at `0x01E38000` (SIDs `0x1C06/0x1C08/0x1C0C`, secure bank): the Pronto's DMA goes through an SMMU context set up by TZ/hyp, unknown whether it is bypass after a PAS boot | dtb |
| RF chip variant | not in the DT (the driver reads it at run time). MSM8952 boards pair with WCN3620/3660B/3680B: to be read from the firmware's version reply | open |
| SCM convention | Linux probes SMC64 first (`INFO/IS_CALL_AVAIL` fast call `0xC2000601`, x1 = 1 arg, x2 = the queried id with owner bits), then SMC32, then the legacy convention. PAS calls: PIL service 2: INIT_IMAGE 1 (pas id, PA of a 4K-aligned copy of the `.mdt` header+hashes), MEM_SETUP 2 (pas id, PA, size), AUTH_AND_RESET 5, SHUTDOWN 6, IS_SUPPORTED 7. x0 = 1 means "interrupted, call again with x0 = 1 and the returned x6" | Linux facts |

### Firmware and calibration files (step 2)

- From the Android system backup (WSL `/opt/sweet/k88-system.img`, `vendor/firmware/wlan/prima/`), copied to the
  git-ignored `backup/k88/wifi/`: `WCNSS_qcom_wlan_nv.bin` (31723 bytes, SHA-256 `5ff2bfb9…8393`), `WCNSS_cfg.dat`
  (11514, `66d8aa04…c1ae`), `WCNSS_wlan_dictionary.dat`, and `vendor/etc/wifi/WCNSS_qcom_cfg.ini` (7794, `d49d9160…5ea1`).
  The `WCNSS_qcom_cfg.ini` in the firmware dir is only a link to `/data/misc/wifi/`.
- `wcnss.mdt`/`wcnss.bNN` are not in the system image: they are in the `modem` partition (FAT, `image/`) and must be
  read from the tablet. `persist` may hold a per-device NV file and the MAC (`wlan_mac.bin`).
