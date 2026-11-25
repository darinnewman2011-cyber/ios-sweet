# Mesa for the A8: panfrost (Bifrost v6) on arm64 iOS

Mesa 25.0.7, built for iOS, with panfrost driving the phone's Mali-G71 through
this port's kernel user client (`JackpotlteMali`, `xnu/patches/0154`) instead of
Linux's DRM.  It is the userspace half of M4: what a graphics stack above it
(the Metal plug-in `0155` names, or an EAGL/GLES one) draws through.

Mesa stays upstream.  Nothing in this directory is a copy of Mesa; the patches
change thirteen files of the release tarball, and the drm face is the live copy in
`userspace/libjackpotlte/`, staged into the tree at build time.

## Run it

```
tools/mesa-jackpotlte/mk-mesa-jackpotlte.sh [WORKDIR]
```

Needs macOS (the iPhoneOS SDK).  `WORKDIR` defaults to
`${TMPDIR:-/tmp}/jackpotlte-mesa` and holds the tarball, the patched tree and
the build directory; nothing is written into the repository.  The product is
`WORKDIR/out/libOSMesa.8.dylib`.

The tarball is verified against
`sha256 592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6`
(`mesa-25.0.7.tar.xz` from `archive.mesa3d.org`).  The same build runs in CI
(`.github/workflows/mesa.yml`).

## What the patches do

| Patch | Files | Why |
|---|---|---|
| `0001-darwin-drm-standin.patch` | `meson.build`, `src/panfrost/lib/kmod/{meson.build,pan_kmod.h,pan_kmod.c}`, `src/gallium/drivers/panfrost/pan_bo.c` | There is no libdrm and no `mmap()` on a device: `dep_libdrm` becomes a dependency on `src/jackpotlte-drm` (our `userspace/libjackpotlte`, installed as `xf86drm.h`); a buffer is mapped/unmapped by its handle; a SweetSurface's size comes from the kernel's describe request (`0154`'s `0x7e`), not from `lseek()`. |
| `0002-panfrost-v6-darwin.patch` | `src/panfrost/lib/meson.build`, `src/gallium/drivers/panfrost/meson.build`, `src/panfrost/lib/pan_shader.c`, `src/gallium/drivers/panfrost/pan_screen.c` | The A8's G71 is Bifrost **v6**; the Darwin build compiles that architecture alone, so the other architectures' entry points are neither built nor named, and `pan_raw_format_mask_midgard` is compiled for v6 too. |
| `0003-osmesa-panfrost-screen.patch` | `src/gallium/targets/osmesa/{osmesa_target.c,meson.build}`, `src/gallium/frontends/osmesa/osmesa.c` | OSMesa asks for the GPU's screen before the processor's (`OSMESA_SOFTWARE=1` still forces softpipe), and turns a SweetSurface's pixels into a texture for the display's own memory. The frontend (`osmesa.c`) is where `glEGLImageTargetTexture2DOES` is answered: without the `get_egl_image`/`validate_egl_image` hook there is no EGL here, so the call could never resolve an image to a texture and the zero-copy path would be dead code. |
| `0004-osmesa-es-profile.patch` | `include/GL/osmesa.h`, `src/gallium/frontends/osmesa/osmesa.c` | OSMesa has no OpenGL ES: it makes a desktop GL compat or core context. The EAGL shim is handed `api` 2 or 3 by CoreAnimation and needs an ES context, so `OSMESA_PROFILE` gains `OSMESA_ES_PROFILE` (0x38), accepted and mapped to `API_OPENGLES2` (the version asked for is the ES version then). Without it `OSMesaCreateContextAttribs` refuses the attribute and no context exists at all. |
The v6 change is the one specific to this board.  The reference this port
started from (HakiOS, Mali-G52) builds **v7** alone; `docs/jackpotlte-mali.md`
§4 records the gap and why it is a build-configuration change rather than a
missing backend.

## The drm face

`userspace/libjackpotlte/` is built into Mesa's panfrost kmod library, with
`-DJACKPOTLTE_DRM_IOS=1`, as `src/jackpotlte-drm/`:

- `jackpotlte_drm.c` is `xf86drm.h`'s functions (`drmOpenWithType`, `drmIoctl`,
  `drmPrimeFDToHandle`, the `drmSyncobj*` family, ...) over the user client.
  The structures are translated field by field: the kernel reads exactly its
  own 40-byte structures and refuses a larger submit (`0154`).
- `jackpotlte_gpu.c` is the request layer and `jackpotlte_iokit.c` the iOS
  backend; both are covered by `userspace/`'s host test and ABI check.

`jackpotlte_drm.h` is installed twice in the tree, as `xf86drm.h` (the name
panfrost includes) and under its own name (what the shim's sources include).

It includes Mesa's own `drm-uapi/drm.h` where that header is available, as real
libdrm's `xf86drm.h` includes `drm.h`.  Not for panfrost, which includes Mesa's
copy itself, but for Mesa's `renderonly.c` (`src/gallium/auxiliary/meson.build`
adds it whenever `dep_libdrm` is found, and panfrost calls the `renderonly_*`
functions from `pan_resource.c`, so it has to be built): it includes only
`<xf86drm.h>` and then uses `DRM_IOCTL_MODE_CREATE_DUMB` and
`struct drm_mode_create_dumb`.  Without the definitions the build stops there
instead of in the driver, which is what the first CI run did.  Outside Mesa
(`userspace/build.sh`, for the phone) there is no `drm-uapi` copy and nothing
asks for those codes, so the include is conditional (`__has_include`).

## What this is not

- Not the Metal driver.  `docs/mali-userland-provenance.md` is where that
  decision and its licence evidence live.
- Not the AIR (Metal shader) frontend.  A Metal plug-in needs one; this library
  does not.
- Not built or booted on the phone yet.  A green build here is a library, not a
  screen: the first claim that panfrost draws on this GPU needs a photo.

## Verification

- The four patches apply with `patch -p1` to a pristine `mesa-25.0.7.tar.xz`
  at the sha256 above (checked 2026-11-13 on the extracted tree).
- CI builds the library and fails if it is missing or not arm64.
- The device-side claim is separate and still open (`JACKPOTLTE_PROGRESS.md`).