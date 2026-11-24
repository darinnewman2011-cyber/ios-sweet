# Galaxy A8: Wi-Fi through iOS's own Wi-Fi stack (the "real route")

Goal (Darin, 2026-11-23): Wi-Fi in iOS Settings, the status bar and Control Center, driven by iOS's own `wifid`,
instead of Sweet Connect or a tweak (ElleKit tweaks never loaded on the A8). The A8's radio already scans, joins open
networks and runs DHCP in the kernel (`bsd/dev/sweet_a8wlbt.c`, xnu 0139); what is missing is the Apple-facing layer.

## How iOS 16.7 finds and drives Wi-Fi (read from the A8 root's dsc, 20H392)

- `wifid` registers `IOServiceAddMatchingNotification("IO80211SkywalkInterface")` and reads the interface's BSD name.
- `IO80211.framework` (`Apple80211BindToInterface`): `_Apple80211FindService` matches the service whose
  `IOInterfaceName` equals the BSD name; `_isInfraInterface` wants `IO80211InterfaceRole = "Infrastructure"`.
- `Apple80211BindToInterfaceWithService`: `IOServiceOpen(service, task, 0)`, then the first request
  (type 43 = DRIVER_VERSION). After a successful bind every `Apple80211Get/Set` goes through
  `IOConnectCallStructMethod(conn, 0, req, 0x3c30, out, &outSize)`:

  | offset | content |
  |---|---|
  | 0x0000 | u32 command: `0xc02869c9` (SIOCGA80211, get) / `0x802869c8` (SIOCSA80211, set) |
  | 0x0004 | u32 inline data length (1..0x3c00), else 0 |
  | 0x0008 | the request's data, inline |
  | 0x3c08 | `struct apple80211req { char ifname[16]; u32 type; u32 val; u32 len; void *data; }` |

  Output = the caller's data buffer (`len` bytes) or `req_val` (4 bytes) when there is no buffer; the call's return
  value becomes the error. Without a bound user client the framework falls back to the classic socket ioctl.
- `Apple80211GetIfListCopy`: `getifaddrs()`, skips lo0/gif0/stf0/faith0, keeps interfaces whose `SIOCGIFMEDIA`
  says IEEE 802.11.
- Events: `_Apple80211IOUCResume` asks the driver "useIOUCWhenPossible" / "useIOUCEventPipe" (a mapped ring buffer
  + IOConnectCallAsyncMethod); without it the framework uses `_openEventSocket`: `socket(PF_SYSTEM, SOCK_RAW,
  SYSPROTO_EVENT)` + `SIOCSKEVFILT {vendor 1 (Apple), class 6 (KEV_IEEE80211_CLASS), subclass 1}`. So the kernel can
  post `kev_post_msg` events with APPLE80211_M_* codes (4 LINK_CHANGED, 9 ASSOC_DONE, 10 SCAN_DONE, ...); the payload
  layout is read from `__eventReadCallbackCF` in stage 2.
- Request numbers and structure layouts: AirportItlwm's reverse-engineered headers (OpenIntelWireless/itlwm,
  `include/Airport/apple80211_*.h`), read for facts; macOS-era, so iOS differences are expected.

## Stages

1. **xnu 0161 (boot-arg `a8wifi80211=1`):** when the WLAN service starts, `en2` is created at once (media
   IEEE 802.11) and the resource `SweetA8WiFi` is published; `iokit/Kernel/arm/SweetA8WiFi.cpp` then registers
   `IO80211SkywalkInterface` (en2, Infrastructure) with a user client that unpacks selector 0 and calls
   `sweet_a80211_request()` in the driver. Answers: driver/hardware version, capabilities, power, state, op/phy mode,
   locale, country, SSID/BSSID/channel/RSSI/noise when joined, supported channels, scan request/results, associate
   (open networks). Everything else: EOPNOTSUPP. A `SWEET 80211:` summary every 30 s lists each request type with its
   count and error, plus opens/selectors/notification ports. Goal: wifid binds and we learn its request sequence.
2. Events to wifid (link up/down, scan done) via whatever 1 shows it uses (notification port or kernel events);
   correct scan-result layout; Settings shows networks.
3. Join from Settings (open networks), status bar icon, IP via configd from wifid's own path.
4. WPA2: the firmware or the kernel does the 4-way handshake with the passphrase/PMK from ASSOCIATE.
5. Power off/on from Settings and Control Center, saved networks, auto-join.
