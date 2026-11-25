# userspace

The phone's side of the GPU work. Everything here is arm64 iOS, so it builds on
macOS with the iPhoneOS SDK and nowhere else; CI does it (`.github/workflows/userspace.yml`).
All of it is MIT (see `LICENSE`).

## Why this exists

M2 and M3 put the GPU in the kernel: power, clocks, the MMU, address space 0,
one job at a time, interrupts. That is where `xnu/patches/0152`-`0154` stop and
where the reference (HakiOS's `redwood_mali.c`) also stops — its own notes say a
Bifrost Mali needs no firmware and it runs one AS0 and one job at a time. The
remaining distance to a fast home screen is userspace, in three pieces:

1. `jackpotlte-maliprobe` (here): prove a Darwin client can reach the GPU
   through `JackpotlteMali` and share memory with it.
2. `libjackpotlte` (here): the shim. Not the reference's `redwood_drm.c` copied
   over — a small library of its own that speaks `0154` and hands the GPU to a
   Linux-shaped userspace (Mesa's panfrost) or to Metal, with the requests it
   builds checked on the build machine instead of on the phone.
3. Mesa with Bifrost **v6** and, above it, `jackpotlte-gl` — an `EAGLContext`
   implementation that puts CoreAnimation's renderer on the GPU. That is the
   chosen route (`docs/jackpotlte-mali.md` §6, §9); the `JackpotlteMetal`
   bundle (`0155` publishes the service it plugs into) is the later, separate
   Metal milestone.

## jackpotlte-maliprobe

The first tool, and the smallest thing that answers "does any of this work from
userspace?". It finds the `JackpotlteMali` service, opens it, makes a 16 KB
buffer, maps it, checks it reads back, asks `get_param` for the GPU's identity
and features, and closes the buffer.

```
userspace/jackpotlte-maliprobe/maliprobe.c
userspace/jackpotlte-maliprobe/com.jackpotlte.maliprobe.plist
```

- `maliprobe` — on the phone. Needs the GPU up, so boot with the engine args
  (`jtmalipwr=1 jtmalijob=1 jtmalimetal=1`); without them the service is still
  published but the user client answers `kIOReturnNotReady`.
- `maliprobe --abi` — prints the ABI (selectors, structure sizes and offsets)
  and touches no IOKit, so it also runs on the build machine. **This is the
  point of the CI job:** the numbers are grepped against
  `xnu/patches/0154`, and a structure or selector that drifts on either side
  turns the build red instead of failing on the phone.
- `maliprobe --help`.

It prints to stdout *and* to `/dev/console`: a launchd job's stdout is a file
nobody can read back on this phone, and the panel is photographable.

Its IOKit entry points are declared in the source rather than included — the iOS
SDK ships no IOKit headers — and it links with `-Wl,-undefined,dynamic_lookup`,
so the device's own IOKit resolves them when the tool loads.

The phone's copy is unsigned. It does not have to be signed: the root boots with
`cs_enforcement_disable=1` and `launchd_unsecure_cache=1`, and the kernel
carries the built-in entitlements this needs.

## libjackpotlte

The shim. Our kernel exposes the GPU as one IOKit service whose user client
answers panfrost's requests (`xnu/patches/0154`); this turns that into the small
surface a GPU userspace actually wants — a buffer with a GPU address, its memory
in this process, a job chain submitted and waited for, and the GPU's parameters:

```
userspace/libjackpotlte/jackpotlte_gpu.h     the API and the ABI it mirrors
userspace/libjackpotlte/jackpotlte_gpu.c     the requests (no IOKit in here)
userspace/libjackpotlte/jackpotlte_iokit.c   the iOS backend (IOKit)
userspace/libjackpotlte/jackpotlte_drm.h     the libdrm face (installed as Mesa's xf86drm.h)
userspace/libjackpotlte/jackpotlte_drm.c     and the requests it translates
userspace/libjackpotlte/test_gpu_host.c      the host test
```

`jackpotlte_drm.*` is the layer above that, and the one Mesa is built against.
Mesa's panfrost calls libdrm: it opens a device, hands it Linux's DRM request
codes, and maps buffers as if it had `mmap()` on the device. There is no libdrm
on iOS, so this supplies the names panfrost uses with their libdrm meaning and
adds `jackpotlte_drm_map`/`_unmap`/`_surface` for what libdrm has no
equivalent for. Its request numbers are Linux's, and 0154's selectors are those
numbers' low bytes, so nothing is renumbered — but the structures are
translated field by field, because the kernel reads exactly its own 40-byte
submit and refuses anything larger.

`jackpotlte_gpu.c` knows nothing about Darwin. Everything it does with the
kernel goes through a four-entry vtable (`call`, `map`, `unmap`, `close`), so
the whole layer runs on the build machine against a fake IOKit and what it would
send to the phone is asserted there (`test_gpu_host`, under ASan/UBSan). That is
deliberate: the phone is the only place this can run for real, and every mistake
costs a flash. `jackpotlte_iokit.c` is the backend that does the same calls
`maliprobe` makes, and releases what a tool may leak.

Two things keep the ABI honest, and both run in CI:

- `test_gpu_host` proves the layer **asks correctly** — the right
  selector, the right structure size, the right fields, and the kernel's
  refusals turned into the right `errno`.
- `tools/jackpotlte/check-mali-abi.py` proves the layer and the kernel **agree**.
  It lifts the selectors, struct bodies and constants out of
  `xnu/patches/0154`, puts them beside our header, and compiles a unit of
  `_Static_assert`s that every value, size and field offset is the same. A moved
  field or a renumbered request stops the build rather than the phone.

Neither of those is a test of the GPU. "Green" means the layer is not the
problem; whether the GPU answers is a question only the phone can answer.

The archive is built for arm64 iOS as `out/libjackpotlte.a` with
`out/jackpotlte_gpu.h`. It resolves IOKit at load time, so whatever links it
needs `-Wl,-undefined,dynamic_lookup` the way `maliprobe` does.

## jackpotlte-gl

The GLES side (`userspace/jackpotlte-gl/`): the `EAGLContext` implementation
that CoreAnimation asks for when there is no Metal, backed by Mesa's OSMesa with
panfrost on the phone's Mali. It is written and builds as
`out/libjackpotlte-gl.dylib`; `userspace/jackpotlte-gl/README.md` is what each
file is and how it works. Two things it rests on came first, because nothing
else can start without them: the OpenGL ES dispatch table for this phone's build
(`eagl_slots.txt`, 409 entries), generated from the 20H392 dyld shared cache by
`tools/jackpotlte/eagl-slots.py`, and the C for it (`gl_slots.c`, generated by
`tools/jackpotlte/eagl-glue.py`).

The loader, from the reference's own kernel: it is not launchd and not a dyld
insert from a job, because launchd refuses `DYLD_*` in a job description. The
kernel adds `DYLD_INSERT_LIBRARIES` itself, on exec, for a named program
(`xnu/redwood.patch`'s `kern.exec_inject`). For this port that is
`0156-jackpotlte-exec-inject.patch`: a boot-arg (`jtexecinject`) naming the
library and `backboardd`, parsed with `TUNABLE_STR` and applied in
`exec_extract_strings()`. A second boot-arg, `jtgllog=1`
(`0158-jackpotlte-gl-path-log.patch`), puts the loader's decision and the
shim's own log on the kernel console: the kernel prints one `JT INJECT:` line
per exec, and the shim reads `kern.bootargs` and writes its `say()` lines to
`/dev/console` as well as stderr. Both are off unless the switch is there.
A third boot-arg, `jtexecenv=NAME=VALUE,...`
(`0159-jackpotlte-exec-inject-env.patch`), puts extra variables into the
environment of those same programs. It is what hands dyld
`DYLD_AMFI_FAKE=0x2`: dyld4 only reads `DYLD_*` when AMFI says it may
(`allowEnvVarsPath`), and no policy on this port answers that query, so the
kernel-set variable is dropped without it. It needs `dyld_flags=2` on the
cmdline as well; `docs/jackpotlte-mali.md` §9 lists both conditions. All three
are off unless they are on the cmdline.
Only putting the library into the root is a `userdata`
change, and it needs the owner's word first. The reference also turns off
`cs_require_lv()`, but that function has no caller in xnu-8796 (our built-in
AMFI never calls it), so it is not the gate here; if the phone refuses the
library it is dyld's check, and `docs/jackpotlte-mali.md` §9 has the scope and
the citations. Nothing here has drawn a frame.

## Build

```
userspace/build.sh [DEST]      # DEST defaults to userspace/out
```

Needs macOS: the phone's copies are arm64 iOS (iPhoneOS SDK). `build.sh` writes

```
out/maliprobe.host     host copy of the probe, for `--abi`
out/maliprobe          arm64 iOS probe
out/test_gpu_host      host copy of the M4 layer's test (CI runs it)
out/libjackpotlte.a    arm64 iOS: the M4 shim
out/libjackpotlte-gl.dylib  arm64 iOS: the EAGL shim (OpenGL ES behind EAGLContext)
out/jackpotlte_gpu.h   the headers the archive goes with
out/jackpotlte_drm.h   (installed into Mesa's tree as xf86drm.h)
```

CI runs the same script on `macos-15`, runs the two checks above, and uploads
`out/` as the `userspace` artifact, so a phone copy can be pulled from a run
without a Mac in the loop.

## Getting a tool into the iOS root

The root is `/var/tmp/IOSRoot.img` (raw master; the sparse copy flashed to
`USERDATA` is derived from it). It is edited on the host, with the mount
scaffolding in `tools/jackpotlte/apply-root-tweaks.sh`; anything that goes in
belongs there, so there is one fsck/mount-rw/umount pass for the whole root.

For `maliprobe` that is:

```
/usr/local/bin/maliprobe
/System/Library/LaunchDaemons/com.jackpotlte.maliprobe.plist
```

and `tools/jackpotlte/apply-root-tweaks.sh` does both, in the mount it already
does, from `userspace/out/` (override with `MALIPROBE_OUT`). It skips them with
a note when the built copy is absent, which is the normal case on a host without
the iPhoneOS SDK.

The probe is quiet on an ordinary boot: it reads `kern.bootargs` first and
exits without touching the panel unless the boot carries the GPU arguments
(`jtmalimetal`/`jtmalipwr`), because the user client answers
`kIOReturnNotReady` otherwise and a line on the panel at every boot is noise.
On a GPU boot it prints its report to the console *before* iOS draws its first
frame, so a photo of the panel is the readback.
