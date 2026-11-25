#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Which iPhone display mode will iOS see for a framebuffer canvas?

   display-mode.py                     the tree's own numbers (A8 panel + donor)
   display-mode.py --canvas 1125x2436  a candidate presented mode
   display-mode.py --root /mnt/iosroot read the mode list from the phone
   display-mode.py --check             exit 1 if the geometry is not iPhone

Why this exists
---------------
iOS draws the red/orange "rdar:45025538" strip over the status bar when UIKit
has no status-bar visual provider for the screen.  Both halves of that live in
the phone's own UIKitCore (iOS 16.7.16, offsets from the dyld shared cache):

  +0x11d3626  "No status bar visual provider for screen %@ (width: %.2f,
              scale: %.2f, idiom: %ld, cornerRadius: %.2f). Please see
              rdar://45025538."
  +0x11d35c4  "UIStatusBarVisualProviderClassName"   (debug override key)
  +0x123325a  "rdar:45025538" as UTF-16, in the localisable string table
              next to the status-bar strings ("5G", "5G-e")

and the subclasses it can pick from are named after the **native pixel width**
of the real notched iPhones: _UIStatusBarVisualProvider_Split828, _Split1080,
_Split1080_Wide, _Split1125, _Split1170, _Split1242, _Split1284; there is no
provider for a width/height pair that is not a real device.  UIKitCore's
__TEXT also holds the doubles 828.0, 1080.0, 1125.0, 1170.0, 1242.0, 1284.0,
2340.0, 2436.0, 2532.0 and 2778.0 -- and neither 2220.0 nor 2400.0 anywhere.
So a panel's own geometry is not an iPhone mode, and neither 1080x2220
(the A8) nor 1080x2400 (the S8 donor) is one; both carried the strip until the
donor presented 1125x2436.

The mode list below is what this build's UIKitCore pairs native pixels with:
the names in
/System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle/
Assets.car, e.g. "BoundingPathBitmap-1125x2436-375x812-3.00x".  `--root` reads
them from an extracted/mounted iOS root instead of this baked copy, so the
answer comes from the phone and not from here.

Two different lists decide the strip, and `--add`/--root only touch one:

  * PROVIDERS below is the set of native (w, h) sizes UIKitCore has a
    **compiled-in** status-bar visual provider for (the _UIStatusBarVisual
    Provider_Split* classes and the (native width, native height) dispatch at
    UIKitCore+0x283bfc).  This is what decides whether the rdar strip is drawn.
  * the asset catalog decides only the **bounding path** UIKit loads for the
    mode.  tools/jackpotlte/car-mode.py adds the 1080x2220 entry to a root's
    catalog, so SpringBoard can load that path (jt46 crashed on the missing
    one); it does not add a provider.  So `--check` passes only when the canvas
    is a provider size, and it says "boot" when the catalog entry exists but the
    provider does not.

Points are canvas pixels / display-scale, which is what the phone showed when
the canvas was 1125x2436: SpringBoard logged "375x812@3x".
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# (native_w, native_h, [(points_w, points_h, scale), ...])
MODES = [
    (828, 1792, [(375, 812, 2.0), (414, 896, 2.0)]),
    (1080, 2340, [(320, 693, 3.0), (375, 812, 3.0)]),
    (1125, 2436, [(320, 693, 3.0), (375, 812, 3.0)]),
    (1170, 2532, [(320, 693, 3.0), (390, 844, 3.0)]),
    (1179, 2556, [(320, 693, 3.0), (393, 852, 3.0)]),
    (1242, 2688, [(375, 812, 3.0), (414, 896, 3.0)]),
    (1284, 2778, [(375, 812, 3.0), (428, 926, 3.0)]),
    (1290, 2796, [(375, 812, 3.0), (430, 932, 3.0)]),
]

# Native (width, height) sizes UIKitCore has a compiled-in status-bar visual
# provider for, read by hand from the phone's iOS 16.7.16 UIKitCore: the
# _UIStatusBarVisualProvider_Split* class names (828, 1080, 1080_Wide, 1125,
# 1170, 1170_Wide, 1242, 1284, 1284_Wide) and the fixed sizes in the
# (native w, h) dispatch at UIKitCore+0x283bfc (its doubles include 320x480,
# 640x960, 750x1334, 1080x1920, 1080x2340, 1125x2436, 1170x2532, 1242x2688,
# 1284x2778).  1080x2220 is deliberately not here, and no asset catalog can add
# one.
PROVIDERS = {
    (320, 480), (640, 960), (750, 1334), (1080, 1920),
    (828, 1792), (1080, 2340), (1125, 2436), (1170, 2532),
    (1242, 2688), (1284, 2778),
}

ASSET_CAR = ("System/Library/PrivateFrameworks/UIKitCore.framework/"
             "BoundingPathData.bundle/Assets.car")
ASSET_RE = re.compile(
    rb"BoundingPathBitmap-(\d+)x(\d+)-(\d+)x(\d+)-(\d+)\.(\d+)x")


def tree_numbers():
    """FB_W/FB_H from the A8 ADT builder and display-scale from mkadt.py."""
    panel, scale = None, None
    try:
        with open(os.path.join(REPO, "tools", "mkadt_jackpotlte.py"),
                  "r", errors="replace") as f:
            m = re.search(r"^FB_W,\s*FB_H\s*=\s*(\d+),\s*(\d+)", f.read(), re.M)
            if m:
                panel = (int(m.group(1)), int(m.group(2)))
    except OSError:
        pass
    try:
        with open(os.path.join(REPO, "tools", "mkadt.py"),
                  "r", errors="replace") as f:
            m = re.search(r"\('display-scale',\s*\w+\((\d+)\)\)", f.read())
            if m:
                scale = int(m.group(1))
    except OSError:
        pass
    return panel, scale


def read_modes(root):
    """The mode list out of the root's own BoundingPathData asset catalog."""
    path = os.path.join(root, ASSET_CAR)
    data = open(path, "rb").read()
    seen, modes = set(), []
    for m in ASSET_RE.finditer(data):
        w, h, pw, ph, s0, s1 = (int(x) for x in m.groups())
        key = (w, h, pw, ph, "%s.%s" % (s0, s1))
        if key in seen:
            continue
        seen.add(key)
        for mode in modes:
            if (mode[0], mode[1]) == (w, h):
                mode[2].append((pw, ph, float("%s.%s" % (s0, s1))))
                break
        else:
            modes.append((w, h, [(pw, ph, float("%s.%s" % (s0, s1)))]))
    return sorted(modes), path


def fit(panel, canvas):
    """How a presented canvas lands on the panel, in words."""
    pw, ph = panel
    cw, ch = canvas
    if (cw, ch) == (pw, ph):
        return "1:1, the whole panel"
    if cw <= pw and ch <= ph:
        return ("1:1, letterboxed (%.0f px bars left/right, %.0f top/bottom)"
                % ((pw - cw) / 2.0, (ph - ch) / 2.0))
    if cw == pw:
        return ("width exact, %d rows %s (%.1f%% of source)"
                % (abs(ch - ph), "squeezed" if ch > ph else "padded",
                   100.0 * abs(ch - ph) / ch))
    if ch == ph:
        return "height exact, %d columns cropped (%.1f%%)" % (
            abs(cw - pw), 100.0 * abs(cw - pw) / cw)
    if cw > pw and ch > ph:
        return ("%d columns cropped, %d rows squeezed (%.1f%% of source)"
                % (cw - pw, ch - ph, 100.0 * (ch - ph) / ch))
    return "scaled on both axes (%.3f x %.3f)" % (1.0 * pw / cw, 1.0 * ph / ch)


def report(panel, canvas, scale, modes, source, providers):
    print("panel        %dx%d" % panel)
    print("canvas       %dx%d (what IOMobileFramebuffer/ADT report)"
          % canvas)
    print("display-scale %d" % scale)
    ptw, pth = canvas[0] / float(scale), canvas[1] / float(scale)
    print("iOS sees     %gx%g points (canvas / display-scale)" % (ptw, pth))
    print("modes from   %s" % source)
    print()

    known = next((m for m in modes if (m[0], m[1]) == canvas), None)
    prov = canvas in providers

    # The two facts the strip depends on, kept apart on purpose.
    if known:
        pairs = ", ".join("%dx%d @%g" % (p[0], p[1], p[2]) for p in known[2])
        print("bounding path  present: the catalog has %dx%d (%s)"
              % (canvas[0], canvas[1], pairs))
    else:
        print("bounding path  MISSING: the catalog has no %dx%d rendition, so "
              "UIKit" % canvas)
        print("               cannot load a bounding path for it")
    if prov:
        print("status bar     a provider size: UIKitCore has a compiled-in "
              "status-bar")
        print("               visual provider for %dx%d" % canvas)
    else:
        print("status bar     NOT a provider size: UIKitCore has no "
              "compiled-in")
        print("               status-bar visual provider for %dx%d" % canvas)
    print()

    if prov and known and (ptw, pth) in [(p[0], p[1]) for p in known[2]]:
        print("VERDICT  %dx%d is a known iPhone mode: a status-bar provider and "
              "a" % canvas)
        print("         bounding path for %gx%g points, so no rdar strip is "
              "expected." % (ptw, pth))
        return 0
    if prov and known:
        print("VERDICT  %dx%d has a provider and a bounding path, but %gx%g "
              "points is" % (canvas + (ptw, pth)))
        print("         not one of its point sizes (%s)."
              % ", ".join("%dx%d" % (p[0], p[1]) for p in known[2]))
        return 1
    if prov and not known:
        print("VERDICT  %dx%d has a status-bar provider but no bounding path in "
              "the" % canvas)
        print("         root's catalog.  A mode needs both: loading a provider "
              "size")
        print("         whose path is missing is what crashed SpringBoard at "
              "1125x2436")
        print("         (jt46).  Run tools/jackpotlte/car-mode.py --add, or use "
              "a root")
        print("         that has it.")
        return 1
    if known and not prov:
        print("VERDICT  %dx%d is in the root's BoundingPathData catalog, so "
              "SpringBoard" % canvas)
        print("         can load its bounding path (%gx%g points).  But it is "
              "not one" % (ptw, pth))
        print("         of the sizes UIKitCore has a compiled-in status-bar "
              "provider")
        print("         for, and an asset catalog cannot add one.  Whether the "
              "rdar")
        print("         strip is gone is a boot question, not an offline one.")
        return 1
    if any(m[0] == canvas[0] for m in modes):
        print("VERDICT  %dx%d is NOT an iPhone mode.  Width %d is supported "
              "(native " % (canvas[0], canvas[1], canvas[0])
              + "height %s), so it is the height that misses."
              % " or ".join(str(m[1]) for m in modes if m[0] == canvas[0]))
    else:
        print("VERDICT  %dx%d is NOT an iPhone mode (no iPhone is %d px wide)."
              % (canvas[0], canvas[1], canvas[0]))
    print("         UIKitCore has no status-bar visual provider for it, so the")
    print("         status bar falls back to the rdar strip.  Present a real "
          "mode:")
    print()
    cands = sorted(modes, key=lambda m: abs(m[0] - panel[0]) + abs(m[1] - panel[1]))
    shown = 0
    for w, h, pairs in cands:
        for pw, ph, s in pairs:
            if shown >= 4:
                return 1
            print("  %4dx%-4d @%-3g %3dx%-3d  %s"
                  % (w, h, s, pw, ph, fit(panel, (w, h))))
            shown += 1
    return 1


def main(argv):
    panel, scale = tree_numbers()
    canvas = None
    root = None
    check = False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--panel" and i + 1 < len(argv):
            panel = tuple(int(x) for x in argv[i + 1].split("x"))
            i += 2
        elif a == "--canvas" and i + 1 < len(argv):
            canvas = tuple(int(x) for x in argv[i + 1].split("x"))
            i += 2
        elif a == "--scale" and i + 1 < len(argv):
            scale = int(argv[i + 1])
            i += 2
        elif a == "--root" and i + 1 < len(argv):
            root = argv[i + 1]
            i += 2
        elif a == "--check":
            check = True
            i += 1
        elif a in ("-h", "--help"):
            return __doc__
        else:
            return "display-mode.py: unknown argument %r\n%s" % (a, __doc__)
    if panel is None:
        return "display-mode.py: no panel (pass --panel WxH)"
    if scale is None:
        return "display-mode.py: no display-scale (pass --scale N)"

    if root:
        try:
            modes, source = read_modes(root)
        except OSError as e:
            return "display-mode.py: cannot read the mode table: %s" % e
    else:
        modes, source = MODES, "this file (iOS 16.7.16 asset names; --root to re-read)"

    rc = 0
    if canvas is None:
        # the tree's canvas (the panel) and the donor's panel, for contrast
        for c in ((panel[0], panel[1]), (1080, 2400)):
            rc |= report(panel, c, scale, modes, source, PROVIDERS)
            print()
        return rc if check else 0
    rc = report(panel, canvas, scale, modes, source, PROVIDERS)
    return rc if check else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)