#!/bin/bash
# macOS port of tools/mkiosroot.sh: build an A8 iOS root (one case-sensitive journaled HFS+ raw image with the
# OS root + its cryptexes inside) from an IPSW's decrypted disk images, using only macOS tools.
#
#   mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img SIZE_BYTES [--translate] [--tweaks]
#
# SIZE_BYTES must not exceed the phone's /data/sweet/root.img (the image is written into that file in place).
# --minimal skips what ios-minimal.filter lists while copying (rsync filter rules: apps, languages, voices...),
# so a big iOS (26) fits the slot; nothing is copied and then deleted.
# --translate runs tools/a8xlate (arm64e -> ARMv8.0) on the mounted image; --tweaks runs sweet-root-tweaks.py.
# Needs sudo (system files keep their owners). Older IPSWs without cryptexes: pass "-" for SYSTEMOS/APPOS.
set -euo pipefail
[[ $# -ge 5 ]] || { sed -n '2,10p' "$0"; exit 2; }
os=$1 sysos=$2 appos=$3 out=$4 size=$5; shift 5
translate=0 tweaks=0 minimal=0
for a in "$@"; do case $a in --translate) translate=1 ;; --tweaks) tweaks=1 ;; --minimal) minimal=1 ;; esac; done
here=$(cd "$(dirname "$0")" && pwd)
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d /tmp/a8root.XXXX)
attached=()
cleanup() {
	for d in "${attached[@]}"; do hdiutil detach "$d" -force >/dev/null 2>&1 || true; done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

attach_ro() {	# image mountpoint
	mkdir -p "$2"
	local dev
	if ! dev=$(hdiutil attach -readonly -owners on -nobrowse -noverify -noautoopen -mountpoint "$2" "$1" 2>/dev/null | awk 'NR==1{print $1}'); then
		# the cryptexes are raw APFS images
		dev=$(hdiutil attach -readonly -owners on -nobrowse -noverify -noautoopen -imagekey diskimage-class=CRawDiskImage \
			-mountpoint "$2" "$1" | awk 'NR==1{print $1}')
	fi
	attached+=("$dev")
	echo "mounted $(basename "$1") at $2"
}
attach_ro "$os" "$work/os"
[[ $sysos != - ]] && attach_ro "$sysos" "$work/sysos"
[[ $appos != - ]] && attach_ro "$appos" "$work/appos"

echo "creating $out ($size bytes, case-sensitive journaled HFS+)"
mkfile -n "$size" "$out"
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
newfs_hfs -s -J -v iOSRoot "$dev" >/dev/null
hdiutil detach "$dev" >/dev/null
# re-attach mounted with ownership enabled (system files keep their root/mobile/_logd owners)
mkdir -p "$work/root"
dev=$(hdiutil attach -owners on -nobrowse -noautoopen -imagekey diskimage-class=CRawDiskImage -mountpoint "$work/root" "$out" | awk 'NR==1{print $1}')
attached=("$dev" "${attached[@]}")
diskutil info "$work/root" | grep -q 'Owners:.*Enabled' || { echo "ownership is not enabled on the new image" >&2; exit 1; }
R=$work/root

copy() {
	echo "copying $1 -> ${2#"$R"}"
	sudo mkdir -p "$2"
	if [[ $minimal == 1 ]]; then
		sudo rsync -aH --numeric-ids --filter="merge $here/ios-minimal.filter" "$1/" "$2/"
	else
		sudo rsync -aH --numeric-ids "$1/" "$2/"
	fi
}
copy "$work/os" "$R"
[[ $sysos != - ]] && copy "$work/sysos" "$R/private/preboot/Cryptexes/OS"
[[ $appos != - ]] && copy "$work/appos" "$R/private/preboot/Cryptexes/App"

if [[ $sysos != - ]]; then
	# dyld at ignition level 1 looks in /System/Library/Caches/com.apple.dyld: hard links, no extra space
	dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	sudo mkdir -p "$R/System/Library/Caches/com.apple.dyld"
	for f in "$R/$dsc"/*; do sudo ln -f "$f" "$R/System/Library/Caches/com.apple.dyld/${f##*/}"; done
	# cryptex files with no counterpart in the OS root, linked to their root paths
	for c in OS App; do
		[[ -d "$R/private/preboot/Cryptexes/$c" ]] || continue
		(cd "$R/private/preboot/Cryptexes/$c" && sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
		while IFS= read -r f; do
			f=${f#./}
			if ! sudo test -e "$R/$f" && ! sudo test -L "$R/$f"; then
				sudo mkdir -p "$R/$(dirname "$f")"
				src="$R/private/preboot/Cryptexes/$c/$f"
				if sudo test -L "$src"; then sudo cp -P "$src" "$R/$f"; else sudo ln "$src" "$R/$f"; fi
			fi
		done
	done
fi

# logd stores (owner _logd 272) and the plain-plist system keybag, as mkiosroot.sh does
db="$R/private/var/db"
sudo install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
sudo install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do sudo install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"; done
sudo install -d -m 0700 -o 0 -g 0 "$R/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo tee "$R/private/var/keybags/systembag.kb" >/dev/null
sudo chmod 0600 "$R/private/var/keybags/systembag.kb"

if [[ $tweaks == 1 ]]; then
	echo "applying sweet-root-tweaks.py (written for iOS 16; version-specific steps may warn)"
	sudo python3 "$here/sweet-root-tweaks.py" "$R" || echo "WARNING: some tweaks failed (see above)"
fi
if [[ $translate == 1 ]]; then
	echo "translating arm64e code in place (a8xlate)"
	sudo python3 "$here/a8xlate/a8xlate.py" inplace "$R" --cache "$HOME/.a8xlate-cache"
fi

df -h "$R" | tail -1
hdiutil detach "$dev" >/dev/null
attached=("${attached[@]:1}")
echo "checking the filesystem"
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
fsck_hfs -fy "$dev" | tail -3 || true
hdiutil detach "$dev" >/dev/null
echo "ROOT_OK $out"
