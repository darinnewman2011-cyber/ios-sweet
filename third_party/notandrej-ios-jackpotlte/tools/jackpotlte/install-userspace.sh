#!/bin/bash
# SPDX-License-Identifier: MIT
# Install the built jackpotlte userspace into the iOS root image.
#
#   sudo tools/jackpotlte/install-userspace.sh [IMAGE] [OUT]
#   (pkexec works the same way; the script itself needs root only to mount)
#
# IMAGE defaults to /var/tmp/IOSRoot.img, the raw master the sparse image
# flashed to USERDATA is derived from; OUT defaults to userspace/out, the
# directory CI's `userspace` artifact unpacks to (or userspace/build.sh's
# output).  Nothing is installed that is not there, a file whose sha256 already
# matches is not rewritten, and every file is read back from the mounted volume
# afterwards, so a half-written image is not left to be flashed.
#
# Why a mount: the phone's iOS root lives inside USERDATA as HFS+, and the two
# libraries that make the GL path are files in it (/usr/local/lib).  The
# kernel's hfsplus driver is the only writer available here (7z reads HFS+,
# hfsutils writes only classic HFS, hfsprogs ships no writer), and it refuses a
# journaled volume unless told `force`:
#
#   hfsplus: write access to a journaled filesystem is not supported, use the
#            force option at your own risk, mounting read-only.
#
# The volume this writes is journaled (kHFSVolumeJournaledBit), so the write is
# made with `force` and is not recorded in the journal.  That is acceptable
# here and only here: the volume is unmounted on both sides of it, so the
# journal is empty and a later mount - on this host or on the phone - replays
# nothing.  What makes it safe to spend on a 12 GB flash is fsck.hfsplus: the
# volume is checked before the write and after it, and any problem the write
# introduces (a line the first check did not print) fails the script.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
image=${1:-/var/tmp/IOSRoot.img}
out=${2:-$repo/userspace/out}

[ "$(id -u)" = 0 ] || { echo "error: needs root to mount $image (sudo or pkexec)" >&2; exit 1; }

files=(
	libjackpotlte-gl.dylib
	libOSMesa.8.dylib
)

for name in "${files[@]}"; do
	[ -f "$out/$name" ] || { echo "error: no $out/$name" >&2; exit 1; }
done

# An image that is already attached to a loop device is reused by mount(8), and
# if that attachment is read-only the new mount is read-only too, so the install
# dies deep inside `install` with "cannot remove ...: Read-only file system"
# after nothing at all has been written.  Say so here instead.  The comparison
# is against the loop device's backing file, not the mount entry: /proc/mounts
# and /proc/self/mountinfo name a loop mount's source as /dev/loopN, never the
# image.  -O BACK-FILE prints the name as it was attached, so the argument is
# resolved first.
image=$(readlink -f "$image")
for l in $(losetup -l -n -O NAME,BACK-FILE | awk -v img="$image" '$2 == img { print $1 }'); do
	where=$(findmnt -n -o TARGET --source "$l" 2>/dev/null | tr '\n' ' ')
	cat >&2 <<EOF
error: $image is already attached to $l${where:+ (mounted at $where)}.

       A second mount of it would come up read-only and the install would fail
       after changing nothing.  Clear it, unmounting before detaching - losetup
       -d on a mounted autoclear device only marks it:

         umount ${where:-<mountpoint>}
         losetup -d $l
EOF
	exit 1
done

# The problem lines of a read-only check, without the two lines every run
# prints and without the "(1, 0)" counters, which are not a problem.
problems() {
	fsck.hfsplus -fn "$image" 2>&1 |
	    grep -iE 'corrupt|needs to be repaired|error|invalid|bad |mismatch|orphan|overlap' || true
}

before=$(sha256sum "$image" | cut -d' ' -f1)
echo "image  $image"
echo "before $before"

echo "--- fsck.hfsplus before the write (background: this image already reports"
echo "    its alternate volume header as stale; that is what mkiosroot left) ---"
problems | tee /tmp/install-userspace-fsck-before.txt

mountpoint=$(mktemp -d /tmp/jackpotlte-root.XXXXXX)
cleanup() {
	umount "$mountpoint" 2>/dev/null || true
	rmdir "$mountpoint" 2>/dev/null || true
}
trap cleanup EXIT

mount -t hfsplus -o loop,rw,force "$image" "$mountpoint"
grep -F "$mountpoint" /proc/mounts || true
echo "mounted $mountpoint"

install -d -m 0755 "$mountpoint/usr/local/lib"
for name in "${files[@]}"; do
	want=$(sha256sum "$out/$name" | cut -d' ' -f1)
	there=$mountpoint/usr/local/lib/$name
	if [ -f "$there" ] && [ "$(sha256sum "$there" | cut -d' ' -f1)" = "$want" ]; then
		echo "same  $name $want (already installed, not rewritten)"
		continue
	fi
	echo "install $name ($(stat -c %s "$out/$name") bytes)"
	install -m 0755 "$out/$name" "$there"
done
sync

status=0
for name in "${files[@]}"; do
	want=$(sha256sum "$out/$name" | cut -d' ' -f1)
	got=$(sha256sum "$mountpoint/usr/local/lib/$name" | cut -d' ' -f1)
	if [ "$want" = "$got" ]; then
		echo "ok    $name $got"
	else
		echo "BROKEN $name: wrote $want, read $got" >&2
		status=1
	fi
	ls -l "$mountpoint/usr/local/lib/$name"
done

umount "$mountpoint"
rmdir "$mountpoint"
trap - EXIT

after=$(sha256sum "$image" | cut -d' ' -f1)
echo "after  $after"
[ "$before" != "$after" ] || { echo "error: the image did not change" >&2; exit 1; }

echo "--- fsck.hfsplus after the write ---"
problems | tee /tmp/install-userspace-fsck-after.txt
if new=$(diff /tmp/install-userspace-fsck-before.txt /tmp/install-userspace-fsck-after.txt | grep '^>' || true)
	[ -z "$new" ]; then
	echo "fsck: no new problem"
else
	echo "error: the write introduced a filesystem problem:" >&2
	echo "$new" >&2
	status=1
fi

echo
echo "next: df -h /   (the sparse needs ~12.15 GB free)"
echo "      img2simg $image /var/tmp/IOSRoot.sparse.img"
echo "      tools/jackpotlte/verify-sparse.py $image /var/tmp/IOSRoot.sparse.img"
echo "      fastboot flash USERDATA /var/tmp/IOSRoot.sparse.img"
exit $status