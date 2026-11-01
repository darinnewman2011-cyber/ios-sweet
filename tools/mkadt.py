#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# --device jackpotlte: Samsung Galaxy A8 (2018), SM-A530F, Exynos 7885.
# Source of the facts below: mainline Linux exynos7885.dtsi and
# exynos7885-jackpotlte.dts. Anything marked UNVERIFIED has to be checked
# against the phone's own live FDT before a boot attempt
# (see docs/PORTING_JACKPOTLTE.md).
# GIC-400 is a GICv2 (distributor + CPU interface), NOT the GICv3 of sweet.
JP_GIC = [(0x12301000, 0x1000),   # GICD
          (0x12302000, 0x1000),   # GICC (Samsung's DTB; mainline says 0x2000)
          (0x12304000, 0x2000),   # GICH
          (0x12306000, 0x2000)]   # GICV
JP_TIMEBASE = 26000000            # oscclk; the arch timer runs from it
JP_VTIMER_INTID = 27              # <GIC_PPI 11 ...> -> 16 + 11
# DRAM on the 4 GB unit: 0x80000000 (0x3da00000), 0xc0000000 (1 GiB), 0x8_80000000
# (2 GiB). The boot.img DTB lists only 1 GiB in the upper bank; S-Boot extends it on
# 4 GB units (mainline agrees: 2 GiB). XNU wants one contiguous range, so the upper
# bank is used; it has no reserved-memory carve-outs in any revision.
# Verify against the live FDT and pass --mem-size if it differs.
JP_PHYS_BASE, JP_MEM_SIZE = 0x880000000, 0x80000000
# Internal storage is eMMC 5.1 (DW-MSHC), not UFS.
JP_MMC, JP_MMC_SIZE = 0x13500000, 0x2000
# Cores as (MPIDR, compatible, Hz), boot core first. UNVERIFIED: which core
# S-Boot/EL3 enters the payload on (assumed the first little core).
JP_CORES = ([(m, 'arm,cortex-a53', 1600000000) for m in (0x100, 0x101, 0x102, 0x103, 0x200, 0x201)] +
            [(m, 'arm,cortex-a73', 2200000000) for m in (0x0, 0x1)])
# Panel s6e3fa7_fhd: 1080x2220, DSI command mode (decon psr_mode=2, HW TE trigger).
# S-Boot hands over a framebuffer at 0xEC000000 (0x1400000 bytes), same in all revisions.
JP_FB_W, JP_FB_H = 1080, 2220
JP_FB_PHYS = 0xEC000000

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, device='sweet', fb=None, mem=None):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py).
    device='jackpotlte': Galaxy A8 (2018) / Exynos 7885 (GICv2, eMMC, 26 MHz timer).
    fb=(phys, width, height) overrides the framebuffer ABL/S-Boot leaves (required for jackpotlte)."""
    jp = device == 'jackpotlte'
    if jp and qemu:
        raise SystemExit('--device jackpotlte has no QEMU variant')
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    if jp:
        phys_base, mem_size = JP_PHYS_BASE, (mem or JP_MEM_SIZE)
    # Carve the panic log off the top so the kernel never allocates over it.
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    if jp:
        timebase = JP_TIMEBASE
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(len(JP_CORES) if jp else (1 if qemu else 8)):
        if jp:
            mpidr, cpu_compat, cpu_hz = JP_CORES[core]
        else:
            mpidr, cpu_compat, cpu_hz = core << 8, 'qcom,kryo470', (2304000000 if core >= 6 else 1804800000)
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(cpu_compat) + cstr('ARM,v8')),
            ('reg', u32(mpidr)),  # actual MPIDR Aff2:Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(cpu_hz)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    if jp:
        # GICv2: reg = GICD, GICC, GICH, GICV. The kernel needs a GICv2
        # driver for this (sweet's is GICv3 system-register based).
        gic = node('gic', [
            ph(),
            ('compatible', cstr('arm,gic-400')),
            ('interrupt-controller', b''),
            ('virtual-timer-intid', u32(JP_VTIMER_INTID)),
            ('reg', u64(*[v for base, size in JP_GIC for v in (base - SOC_BASE, size)])),
        ])
    else:
        gic = node('gic', [
            ph(),
            ('compatible', cstr('arm,gic-v3')),
            ('interrupt-controller', b''),
            ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
            ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
        ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    if jp:
        fb_phys, fb_w, fb_h = fb if fb else (JP_FB_PHYS, JP_FB_W, JP_FB_H)
        fb_stride = fb_w * 4
        fb_size = (fb_stride * fb_h + 0xfff) & ~0xfff
    else:
        fb_phys, fb_w, fb_h, fb_stride, fb_size = FB, FB_W, FB_H, FB_STRIDE, FB_SIZE
    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb_phys, fb_size)),
        ('width', u32(fb_w)),
        ('height', u32(fb_h)),
        ('stride', u32(fb_stride)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])
    if jp:
        # Galaxy A8 (2018) has eMMC on the DW-MSHC controller; the kernel
        # needs a new root-device driver (replaces bsd/dev/sweet_ufs.c).
        ufs = node('sweet-mmc', [
            ph(),
            ('compatible', cstr('samsung,exynos7885-dw-mshc-smu')),
            ('reg', u64(JP_MMC, JP_MMC_SIZE)),
        ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    compat = cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('qcom,sm7150') + cstr('AppleARM')
    if jp:
        # 'xiaomi,sweet' is kept only so the existing SweetPlatformExpert
        # matches during bring-up; rename it together with the kernel patch.
        compat = (cstr('D22AP') + cstr('iPhone10,3') + cstr('samsung,jackpotlte') +
                  cstr('samsung,exynos7885') + cstr('xiaomi,sweet') + cstr('AppleARM'))
    return node('device-tree', [
        ph(),
        ('compatible', compat),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [ufs]))


def _opt(name):
    """Value of '--name VALUE' or '--name=VALUE' in argv, else None."""
    for i, a in enumerate(sys.argv):
        if a == name and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if a.startswith(name + '='):
            return a.split('=', 1)[1]
    return None


if __name__ == '__main__':
    device = _opt('--device') or 'sweet'
    if device not in ('sweet', 'jackpotlte'):
        raise SystemExit(f'unknown --device {device!r} (sweet, jackpotlte)')
    fb = None
    if _opt('--fb'):   # --fb 0x9d000000[,1080x2220]
        addr, _, dims = _opt('--fb').partition(',')
        w, h = (int(x) for x in dims.split('x')) if dims else (JP_FB_W, JP_FB_H)
        fb = (int(addr, 0), w, h)
    mem = int(_opt('--mem-size'), 0) if _opt('--mem-size') else None
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv, device=device, fb=fb, mem=mem)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
