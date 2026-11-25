#!/bin/bash
# SPDX-License-Identifier: MIT
# Apply the SWEET iOS-root tweaks *and* the jackpotlte app fix to the iOS
# root image.
#
#   pkexec tools/jackpotlte/apply-root-tweaks.sh [IMAGE]
#
# IMAGE defaults to /var/tmp/IOSRoot.img (the raw master; the sparse copy
# flashed to USERDATA is derived from it). The script is idempotent: the
# tweaks are set-style and an app already in /Applications is left alone.
#
# Why this exists: tools/mkiosroot.sh builds the plain root, and the donor's
# tools/sweet-root-tweaks.py is what turned "plain mkiosroot output" into the
# images that reach the SpringBoard UI. The image flashed on 2026-11-08 was
# checked with `7z l`: it has NO /private/var/mobile/Library/Preferences/
# com.apple.migration.plist, no purplebuddy Setup flags, no mobileactivationd
# data ark, no containers/ dirs, and VirtualAudio still installed. That is the
# pre-tweak state, which is exactly the endless-DataMigration boot: the bar
# runs, and SpringBoard is held in -[BKSSystemShellService _checkIn] until
# backboardd's DMPerformMigrationIfNeeded returns.
#
# What gets written:
#   (via tools/sweet-root-tweaks.py)
#   com.apple.migration.plist  LastSystemVersion=20H392 so DataMigration stops
#                              running on every boot (the proven 100% gate),
#                              + DMUserDataDisposition + DMLastMigrationResults
#   containers/                /private/var/containers/{Bundle,Data/System,
#                              Shared/SystemGroup,Temp}
#   purplebuddy                SetupDone / SetupFinishedAllSteps / SetupVersion,
#                              mini-buddy flags off
#   springboard                BKNoWatchdogs, no cloud-configuration UI
#   mobileactivationd          activation data ark (-ActivationState Activated)
#   launchd.plist              _PanicOnCrash -> _PanicOnCrasX, CommCenter jobs off
#   mediaserverd               VirtualAudio HAL plug-in disabled,
#                              StartRecorderServer=false
#   mobilegestalt              .../containers/Shared/SystemGroup/
#                              systemgroup.com.apple.mobilegestaltcache/
#                              Library/Caches/com.apple.MobileGestalt.plist
#                              with CacheExtra: productType=iPhone10,3,
#                              hwModelStr=D22AP, productName and
#                              marketingNameString=iPhone X, and
#                              artworkTraits={ArtworkDeviceSubType:2556} --
#                              the last one is SpringBoard's Dynamic Island
#                              gate on 16.7.16 (Jindo flag + subtype 2556/2796).
#                              CacheVersion/CacheUUID are set too, because
#                              MobileGestaltMigrator rebuilds (and would drop
#                              CacheExtra) on any cache that fails
#                              __MGCacheValid.  Read back and refused if the
#                              write did not take.
#   (via tools/jackpotlte/car-mode.py)
#   BoundingPathData Assets.car  the missing 1080x2220 (360x740 at 3x)
#                              bounding-path rendition UIKitCore builds the
#                              name for and loads by hand (the A8 canvas is
#                              not one of Apple's shipped modes)
#   (then, by this script, after the Python step)
#   /Applications              the 32 bundles from
#                              /private/var/staged_system_apps moved in
#   /usr/local/bin/maliprobe   the userspace GPU probe, and
#   /System/Library/LaunchDaemons/com.jackpotlte.maliprobe.plist
#                              to run it once at boot (userspace/README.md).
#                              Skipped when the built copy is absent: it is
#                              arm64 iOS, so it comes from the userspace CI
#                              job, not from this host.
#   /usr/local/lib/libjackpotlte-gl.dylib + libOSMesa.8.dylib
#                              the EAGL shim and the Mesa it sits on, for the
#                              kernel's exec-time injection into backboardd
#                              (0156; docs/jackpotlte-mali.md sec. 9).  Same
#                              CI-origin note as the probe.
#
# Identity has a kernel half this script cannot write.  sysctl hw.machine /
# hw.model come from the device tree (IODTPlatformExpert::getMachineName /
# getModelName), and until xnu/patches/0169 the A5XELTE config took the macOS
# kludge branch in bsd/kern/kern_mib.c that hardcodes hw.machine to "iPad8,6"
# -- the one place this tree calls itself an iPad, and what uname().machine
# returns, because libsystem_c's _uname reads CTL_HW/HW_MACHINE into
# utsname.machine.  0169 makes A5XELTE take the device-tree branch
# (hw.machine = root model = iPhone10,3, hw.model = compatible[0] = D22AP),
# and tools/mkadt_jackpotlte.py now puts D22AP first in root compatible for
# the same reason.  The mobilegestalt step below is the userspace half: it is
# enough on its own for every MobileGestalt consumer (SpringBoard, UIKit,
# Settings/General, AuthKit), so an image whose kernel predates 0169 still
# reports iPhone X to those.  Note that UIUserInterfaceIdiom follows this
# identity, so the red "no status bar visual provider" strip (rdar://45025538)
# can move once the device stops describing itself as an iPad; that is
# expected, and the display lane owns what the status bar does with it.
#
# Why the app move: SpringBoard showed only ~7 icons because iOS 16 ships the
# 32 user apps (Maps, Music, Weather, Clock, Calculator, Files, Books, Notes,
# Mail, ...) in /private/var/staged_system_apps and installs them onto the App
# cryptex on the first boot (SystemAppMigrator). This root is one plain HFS+
# volume and its App cryptex is a 10 MB skeleton (the IPSW's `Cryptex1,AppOS`)
# that holds no apps, so the migration installs nothing and LaunchServices
# never registers them. /Applications is the directory it already scans for the
# icons that work. Same volume, so the move is a rename and costs no space.
# tools/mkiosroot.sh does the same during a from-scratch build.
#
# The volume is journaled. Linux refuses it read-write until fsck.hfsplus has
# repaired the volume header, hence `-o force`; and it must be unmounted
# cleanly or XNU has to replay the journal on first boot.
#
# Host quirk (2026-11-08): the ext4 directory index for this image is damaged,
# so a fresh lookup of the *absolute* path intermittently returns ENOENT while
# readdir still lists the entry (the inode is fine: debugfs shows Links 1 and
# the full block list). Running from inside the directory with a relative name,
# and priming with a readdir of that directory before each attempt, makes it
# resolve. Hence the cd + retries below.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
img=${1:-/var/tmp/IOSRoot.img}
imgabs=$img

[ "$(id -u)" -eq 0 ] || { echo "error: run as root (sudo/pkexec $0 $img)" >&2; exit 1; }
[ -d "$(dirname "$img")" ] || { echo "error: no directory for $img" >&2; exit 1; }

# Work from inside the image's directory with a relative name (see the host
# quirk note above), and prime the dentry with a readdir each time.
imgdir=$(cd "$(dirname "$img")" && pwd)
imgbase=$(basename "$img")
cd "$imgdir"
img=./$imgbase

prime() { ls -la . >/dev/null 2>&1 || true; }

n=0
until [ -f "$img" ]; do
	prime
	n=$((n + 1))
	if [ "$n" -ge 300 ]; then
		echo "error: no image at $imgabs (after $n tries)" >&2
		exit 1
	fi
	sleep 1
done
echo "image: $imgabs ($(stat -c %s "$img" 2>/dev/null || echo '?') bytes)"

echo "--- fsck before (repairs the header Linux objects to) ---"
for n in $(seq 1 60); do
	prime
	if fsck.hfsplus -fy "$img"; then break; fi
	echo "  ... fsck attempt $n failed (transient lookup?), retrying"
	sleep 1
done

mnt=$(mktemp -d /tmp/iosroot-XXXXXX)
cleanup() {
	mountpoint -q "$mnt" && umount "$mnt" || true
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT

echo "--- mount read-write ---"
mounted=0
for n in $(seq 1 300); do
	prime
	if mount -t hfsplus -o loop,rw,force "$img" "$mnt" 2>/tmp/iosroot-mount.err; then
		mounted=1
		break
	fi
	[ $((n % 30)) -eq 0 ] && echo "  ... mount attempt $n still failing"
	sleep 1
done
if [ "$mounted" != 1 ]; then
	echo "error: cannot mount $imgabs after $n tries:" >&2
	cat /tmp/iosroot-mount.err >&2
	exit 1
fi

if ! touch "$mnt/.rwcheck" 2>/dev/null; then
	echo "error: $mnt is not writable (journal not repaired?)" >&2
	exit 1
fi
rm -f "$mnt/.rwcheck"

python3 "$repo/tools/sweet-root-tweaks.py" "$mnt"

# The 1080x2220 display mode's bounding path.  The catalog only ships the modes
# for the panels Apple built; the A8's own canvas is not one of them, so add it
# to the list UIKitCore consults (see tools/jackpotlte/display-mode.py for how
# the name is formed and where it is read).  car-mode.py writes atomically, is
# idempotent, and re-reads the file it just wrote; the explicit --verify below
# is a second read of the mounted volume, and the script stops if it fails.
bpcatalog="$mnt/System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle/Assets.car"
if [ -f "$bpcatalog" ]; then
	if python3 "$repo/tools/jackpotlte/car-mode.py" --catalog "$bpcatalog" \
	    --add 1080x2220 --scale 3 --points 360x740 --verbose; then
		python3 "$repo/tools/jackpotlte/car-mode.py" --catalog "$bpcatalog" \
		    --verify 1080x2220 --scale 3
	else
		echo "error: failed to add the 1080x2220 bounding path to $bpcatalog" >&2
		exit 1
	fi
else
	echo "warning: no BoundingPathData Assets.car in $imgabs; UIKit will keep" >&2
	echo "         its fallback status bar for the 1080x2220 mode" >&2
fi

# Present the apps iOS 16 leaves staged for the App cryptex (see the header).
if [ -d "$mnt/private/var/staged_system_apps" ]; then
	promoted=0
	for app in "$mnt/private/var/staged_system_apps"/*.app; do
		[ -e "$app" ] || continue
		[ -e "$mnt/Applications/${app##*/}" ] && continue
		mv "$app" "$mnt/Applications/"
		promoted=$((promoted + 1))
	done
	echo "promoted $promoted staged system apps into /Applications"
else
	echo "warning: no /private/var/staged_system_apps in $imgabs" >&2
fi

# The phone's userspace tool: the probe and the LaunchDaemon that runs it once
# at boot.  It reports to /dev/console only on a GPU boot, so it is quiet on an
# ordinary one (see userspace/README.md).
probe=${MALIPROBE_OUT:-$repo/userspace/out}
if [ -f "$probe/maliprobe" ]; then
	install -d -m 0755 "$mnt/usr/local/bin"
	install -m 0755 "$probe/maliprobe" "$mnt/usr/local/bin/maliprobe"
	install -m 0644 "$repo/userspace/jackpotlte-maliprobe/com.jackpotlte.maliprobe.plist" \
	    "$mnt/System/Library/LaunchDaemons/com.jackpotlte.maliprobe.plist"
	echo "installed maliprobe + its LaunchDaemon"
else
	echo "note: no $probe/maliprobe (the userspace CI job builds it); skipping the probe"
fi

# The GLES shim and the OSMesa it sits on, for the kernel's exec-time injection
# (0156; docs/jackpotlte-mali.md sec. 9).  They share one directory: the shim
# opens Mesa by @loader_path first, so /usr/local/lib holding both is enough and
# the injection names the shim by absolute path (boot-arg jtexecinject).  Both
# are arm64 iOS dylibs, so like the probe they come from CI, not this host.
shim=${GL_OUT:-$repo/userspace/out}
if [ -f "$shim/libjackpotlte-gl.dylib" ] && [ -f "$shim/libOSMesa.8.dylib" ]; then
	install -d -m 0755 "$mnt/usr/local/lib"
	install -m 0755 "$shim/libjackpotlte-gl.dylib" "$shim/libOSMesa.8.dylib" \
	    "$mnt/usr/local/lib/"
	echo "installed libjackpotlte-gl.dylib + libOSMesa.8.dylib into /usr/local/lib"
else
	echo "note: no GLES shim/Mesa in $shim (both from CI); skipping the loader payload"
fi

umount "$mnt"
echo "--- fsck after ---"
fsck.hfsplus -fy "$img" || true

echo
echo "OK: tweaks + app fix applied to $imgabs"
echo
sparse=${imgabs%.img}.sparse.img
echo "Next (host): free the old sparse copy first, there is not room for two."
echo "  rm -f $sparse"
echo "  img2simg $imgabs $sparse"
echo "Then, with the phone in fastboot:"
echo "  fastboot flash USERDATA $sparse"
