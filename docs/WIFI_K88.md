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
