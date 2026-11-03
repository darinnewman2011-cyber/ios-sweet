# Where gpu/ comes from

- `mesa/` (Mesa patches, the `redwood-drm` libdrm stand-in, the AIR translator) and `src/mali_test.c`,
  `src/gl_test.c` are copied unchanged from [stormtroopxyz/y2sios](https://github.com/stormtroopxyz/y2sios)
  (snapshot 68fe2e9), the Galaxy S20+ (Exynos 990, Mali-G77) XNU port, MIT licensed: `LICENSE.y2sios`.
  y2sios builds on HackiOS by OOSEX GANG (MIT).
- `src/eagl.c`, `src/gl_osmesa.c`, `src/redwood_gl.h`, `src/eagl_slots.txt` and `mkeagl.py` (GPU step 7) are
  copied unchanged from the same snapshot (`userland/`). `src/sweet_gl_loader.c` is this port's, after the
  `opengl_es()` part of y2sios' `src/inject.c` (its address hooks for iOS 16.5 are not used).
- Mesa itself (MIT) is downloaded by `build.sh` at build time and is not in this repository.
- `build.sh` is this port's adaptation of y2sios' `tools/mk-mesa.sh` and `userland/build-gpu-y2s.sh`.
- The kernel half they talk to is `xnu/patches/0113-jackpotlte-mali-g71-service.patch`, ported from y2sios'
  `redwood_mali.c` and `RedwoodMali.cpp`; it keeps their service name and request numbers so these work as is.
