# GPU on the Galaxy A8 (2018): plan

Status 2026-11-02: **steps 2 and 3 done on the phone** (step 2: patch 0101, `sweetgpu=2`, boot image `a8-ios9b`, SHA-256
`94a30287…eea1`). iOS itself still draws everything on the CPU (CoreAnimation in software, frames copied into the
S-Boot framebuffer), like sweet.

Observed (ramoops log, 6 cores, 45 s run):
```
SWEET GPU: pd-g3d config 0xf status 0x3000f; CMU_G3D pll_con0 0xa1570d11 switch_user 0 busd_mux 0 busp_div 0x3
SWEET GPU: GPU_ID 0x60a00002 ... L2 0x7120206 core 0 tiler 0x809 mem 0x1 mmu 0x2830 as 0xff js 0x7 status 0
SWEET GPU: present shader 0x3 tiler 0x1 l2 0x1; ready shader 0 tiler 0 l2 0
SWEET GPU: L2 ready 0x1 after ~0 ms / tiler ready 0x1 after ~0 ms / shader ready 0x3 after ~10 ms
```
- S-Boot leaves the G3D domain **on** (config/status `0xF`) and PLL_G3D enabled and locked (bits 31 and 29 of
  `0xa1570d11`), so no PMU or clock write was needed.
- `GPU_ID 0x60a00002` = arch 6.0 (Bifrost), product major 0 = **TMIx (Mali-G71)**, version r0p0, status 2.
  (The probe's own "product/rNpN" printout uses the old Midgard layout; ignore it.)
- 2 shader cores, 1 tiler, 1 L2, 3 job slots (`JS_PRESENT 0x7`), 8 address spaces (`AS_PRESENT 0xff`),
  `MMU_FEATURES 0x2830` (VA 48 bits, PA 40 bits), coherency features `MEM 0x1`.
- Powering L2, tiler and shaders through PWRON works; nothing hung.

**Step 3 done on the phone (2026-11-02)** (patch 0104, `sweetgpu=3`, boot image `a8-ios11g`, SHA-256 `4e7a4c68…19ee`,
CI run 41524145769, log `_tmp/ios11g-log.txt`):
```
SWEET GPU: AS0 transtab 0x883df4007 memattr 0x888d88, buffer PA 0x883df4000, jobs at GPU VA 0x1004000
SWEET GPU: job rawstat 0x2 after ~1 ms; JS1 status 0 head 0x1004080; mmu rawstat 0 AS0 fault 0 at 000000000
SWEET GPU: job0 exception 0x1 incomplete 0, job1 exception 0x1 incomplete 0; target 0 0 0x5157ee70 0xdeadbeef
SWEET GPU: the GPU wrote RAM through its MMU (step 3 done)
```
- The Mali LPAE page-table format works as Panfrost uses it for G71: table = PA|3, leaf = PA|1 at every level,
  HAP read+write, outer shareable, TRANSTAB = L0 PA | READ_INNER | ADRMODE_TABLE, MEMATTR `0x888d88`, 4 levels.
- A chain of two WRITE_VALUE jobs (Zero, Immediate 32) on job slot 1 finished in ~1 ms (JOB_IRQ_RAWSTAT bit 1 = slot 1
  done, no error bit). Both job headers came back with exception status 1 (DONE). No MMU fault.
- The CPU side is non-coherent: the buffer is cleaned/invalidated with `dc civac` before submit and before reading,
  and the job config flushes (clean+invalidate) GPU caches at job start and end. `COHERENCY_ENABLE` = 31 (none).
- Next: step 4 (kernel job service for userland).

## The hardware (from the phone's live FDT and Samsung's Exynos 7885 kernel)

| Item | Value | Source |
| --- | --- | --- |
| GPU | ARM **Mali-G71 MP2** (Bifrost v6, Job Manager), 2 shader cores | Exynos 7885 spec; GPU_ID still to read on the phone |
| Registers | `0x11500000`, size `0x5000` | live FDT `mali@11500000` |
| Interrupts | SPI 0x9F JOB, 0xA0 MMU, 0x9E GPU (INTID +32) | live FDT |
| Power domain | `pd-g3d`, PMU `0x11C840A0` (+0x20), status at PMU + `0x40A4`, mask `0xF` | live FDT |
| Clock | CMU_G3D `0x11400000` (`PLL_CON0_PLL_G3D` +0x120, `CLKCMU_G3D_SWITCH_USER` +0x100), CAL id `0xB040004` | Samsung `cmucal-sfr.c` |
| Rail | `vdd_g3d` = S2MPU08 **BUCK5**, `regulator-always-on`, 0.5–1.3 V, default 0.8 V | live FDT |
| Clocks | DVFS table 343–1300 MHz; Samsung caps at 1100 MHz (`gpu_max_clock`), starts at 343 MHz | live FDT |

The rail is always on, so the first steps need **no PMIC writes** (the riskiest part on the Galaxy S7 port).

## Is there a userland driver? Yes

Mesa's **Panfrost** officially supports **G71/G72 (Bifrost v6)**: OpenGL ES 3.1, Vulkan 1.3 (PanVK), OpenCL 3.0
(Mesa `docs/drivers/panfrost.rst`, checked 2026-11-02). It is MIT licensed. The Galaxy S7 port
(`wardevsa/ios-a5xelte`, branch `herolte`, read only) already runs CoreAnimation on a Mali through Panfrost.
Their GPU is a Mali-T880 (Midgard), not Bifrost, so their kernel side is a close reference (same Job Manager
design, same register families), but the shader compiler and descriptors are Bifrost ones from Mesa.

There is **no Metal**. iOS 16 CoreAnimation normally renders with Metal; the S7 port reaches the GPU through an
**EAGL bridge** (OpenGL ES under CoreAnimation), credited to HakiOS and Bergamota.

## Steps, in order (each one testable hands-off with the ramoops log)

1. **Prerequisites (CPU side):**
   - SMP (patch 0100, in test) so the CPU has room for the driver threads.
   - A real present path: DECON trigger on every framebuffer swap, not only on console prints.
   - The smaller iPhone 8 canvas (750×1334 @2x, scaled to 1080×2220), as on the S7: 72% fewer pixels to render.
2. **Power and identify** (kernel, read-mostly): power the `pd-g3d` domain if S-Boot left it off, ungate the
   G3D clock at the lowest DVFS step (343 MHz), read `GPU_ID` (expect `0x6000xxxx`), `SHADER_PRESENT` (expect
   `0x3`), `L2_PRESENT`, `TILER_PRESENT`, then power the cores on through `SHADER_PWRON`/`L2_PWRON` and wait for
   `*_READY`. Log everything. Done when the IDs read sane and the cores report ready.
3. **MMU and one job** (kernel): one address space (AS0) with 4K page tables, a buffer allocator, and a single
   hand-built job (a WRITE_VALUE or fragment clear), polled with no IRQ. The S7 port did exactly this (their
   patches 0225–0231). Done when the GPU writes a known value into RAM.
4. **Kernel job service**: an IOKit user client (`IOAccelerator`-style) that offers what Panfrost's DRM
   interface gives Linux: create/mmap/free buffer, submit job chain, wait. Back the buffers with IOSurface memory
   so finished frames can be scanned out without a copy.
5. **Mesa Panfrost for iOS**: cross-build Mesa's Panfrost Gallium driver (Bifrost compiler) as an arm64 iOS
   dylib, with a small winsys that talks to step 4's user client instead of `/dev/dri`. First test: a command-line
   program that clears a surface and draws a triangle (the S7's 0232–0234 milestones).
6. **EAGL bridge**: make CoreAnimation/backboardd render through OpenGL ES instead of software. This is the
   biggest and least documented step; the S7 port's `gpu/eagl` and HakiOS's `hakios-gl` are the references
   (read their licenses and NOTICE files before reusing anything).
7. **Present, then tune**: GPU scales the 750×1334 frame to the panel, then a vsync-paced DECON trigger. Then
   DVFS (raise the clock step by step with Samsung's own table and voltages, never above `gpu_max_clock`),
   thermal limits, fault recovery (the S7 still sees occasional GPU read faults).

## Risks and unknowns

- `pd-g3d` state at S-Boot hand-over is unknown (read it first; turning a domain on is a PMU write).
- Changing the GPU clock goes through Samsung's CAL/ACPM (APM firmware at `0x11C00000`); stay at the boot or
  lowest rate until that is understood.
- G71 has known hardware errata that Panfrost works around; use Mesa's G71 paths unmodified.
- Effort: the S7 port needed about 40 kernel patches plus the userland bridge. Expect weeks, not days.
