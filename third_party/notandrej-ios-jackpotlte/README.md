# NotAndrej / ios-jackpotlte (Galaxy A8 2018 pieces)

Imported, unmodified, from https://github.com/NotAndrej/ios-jackpotlte at commit `49fbeea`
("docs: main builds 183 patches"), MIT License (see `LICENSE`; copyright dipa-studio and the
ios-sweet / jackpotlte / a5xelte port authors). Thank you to NotAndrej and everyone credited there.

Only the jackpotlte (Galaxy A8 2018) parts are here; their a5xelte (Galaxy A5) and dreamlte (Galaxy S8) work is not.
**Nothing in this folder is built or applied.** It is reference and raw material for Project Beryllium; anything we
use gets ported into `xnu/patches` (or the userland) on purpose, with this credit kept.

| Here | What it is |
| --- | --- |
| `xnu-patches/` | 60 jackpotlte patches we don't have by name: SCSC Wi-Fi/BT transport (`scsc-*`), USB DWC3 gadget + MTP (`usb-*`), touch fixes (`touch-*`, `sec-ts-*`), charger, flashlight, sensors, Mali (`mali-*`), CPU cluster power / A73 personality (`cluster-power`, `a73-cpu-personality`, `cpu-clusters`), pmap fixes |
| `userspace/` | `jackpotlte-gl` (EAGL over OSMesa), `libjackpotlte` (DRM stand-in), `jackpotlte-maliprobe` |
| `tools/` | `jackpotlte/` (boot image, root tweaks, EAGL slot tools), `mesa-jackpotlte/` (Mesa patches for panfrost on Darwin) |
| `docs/research/` | Their research notes: SCSC Wi-Fi/BT, BT, CPU power, Exynos 7885 mainline, Mali G71 / panfrost v6, sensors, flashlight |

Their patch numbers follow their own series and will not apply to ours as-is.
