#!/bin/bash
# Overnight autopilot (no phone, no sudo, no prompts; background priority). Log: ~/iOS/studio/autopilot.log
#  1. false-positive check: scan the iPad 7 iPadOS 17.7 shared cache (plain arm64: should be ~0 to translate)
#  2. iPhone XS iOS 17.7 (not encrypted): wait for the download, extract, scan, translate its shared cache copy
set -u
REPO=$HOME/iOS/ios-sweet-a8; STUDIO=$HOME/iOS/studio
XL="python3 -I $REPO/tools/a8xlate/a8xlate.py"
log() { echo "[$(date '+%H:%M:%S')] $*"; }
mnt() {	# image mountpoint -> prints device
	mkdir -p "$2"
	hdiutil attach -readonly -nobrowse -noverify -noautoopen -mountpoint "$2" "$1" 2>/dev/null | awk 'NR==1{print $1}' ||
		hdiutil attach -readonly -nobrowse -noverify -noautoopen -imagekey diskimage-class=CRawDiskImage -mountpoint "$2" "$1" | awk 'NR==1{print $1}'
}
manifest() {	# BuildManifest key
	python3 -I -c 'import plistlib,sys
m=plistlib.load(open(sys.argv[1],"rb"))
for b in m["BuildIdentities"]:
    e=b.get("Manifest",{}).get(sys.argv[2])
    if e and e.get("Info",{}).get("Path"): print(e["Info"]["Path"]); break' "$1" "$2"
}

log "=== autopilot start"
# ---- 1. iPad 7 iPadOS 17.7: false-positive check
W=$STUDIO/iPad_10.2_17.7_21H16
if [ -s "$W/SystemOS.dmg" ]; then
	M=$(mktemp -d /tmp/ap.XXXX); d=$(mnt "$W/SystemOS.dmg" "$M/s")
	log "1. scanning iPadOS 17.7 (arm64; expect ~0)"
	$XL scan "$M/s/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64" --jobs 4 > "$W/scan.txt" 2>&1
	cat "$W/scan.txt"
	hdiutil detach "$d" -force >/dev/null 2>&1; rm -rf "$M"
fi

# ---- 2. iPhone XS iOS 17.7
I=$HOME/iOS/ipsw/iphonexs/iPhone11,2,iPhone11,4,iPhone11,6_17.7_21H16_Restore.ipsw
log "2. waiting for the XS iOS 17.7 download (checks every 5 minutes, up to 10 hours)"
for n in $(seq 1 120); do
	[ -s "$I" ] && [ ! -e "$I.aria2" ] && grep -q "OK $(basename "$I")" "$HOME/iOS/ipsw/overnight-ios1718.log" 2>/dev/null && break
	sleep 300
done
if [ ! -s "$I" ] || [ -e "$I.aria2" ]; then log "XS iOS 17.7 never finished downloading; stopping."; exit 0; fi
W=$STUDIO/iPhone11-2_17.7_21H16; mkdir -p "$W"
log "extracting $(basename "$I")"
unzip -o -q "$I" BuildManifest.plist -d "$W"
for key in OS Cryptex1,SystemOS; do
	out=$([ $key = OS ] && echo OS.dmg || echo SystemOS.dmg)
	[ -s "$W/$out" ] && continue
	p=$(manifest "$W/BuildManifest.plist" "$key"); [ -n "$p" ] || continue
	unzip -o -q "$I" "$p" -d "$W/raw" && mv "$W/raw/$p" "$W/$out"
	log "  $key -> $out ($(du -h "$W/$out" | cut -f1))"
done
rm -rf "$W/raw"
M=$(mktemp -d /tmp/ap.XXXX); d=$(mnt "$W/SystemOS.dmg" "$M/s"); d2=$(mnt "$W/OS.dmg" "$M/o")
DSC=$(ls "$M"/s/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e 2>/dev/null)
log "scanning the XS iOS 17.7 shared cache and OS binaries"
{ echo "-- shared cache"; $XL scan "$DSC" --jobs 4; echo "-- OS root binaries"; $XL scan "$M/o/usr" "$M/o/sbin" "$M/o/bin" "$M/o/System/Library/CoreServices" --jobs 4; } > "$W/scan.txt" 2>&1
cat "$W/scan.txt"
log "translating a copy of the shared cache into $W/translated (stage 1, in place)"
mkdir -p "$W/translated"
$XL translate "$DSC" "$W/translated/dyld_shared_cache_arm64e" --jobs 4 > "$W/translate.txt" 2>&1
cat "$W/translate.txt"
hdiutil detach "$d" -force >/dev/null 2>&1; hdiutil detach "$d2" -force >/dev/null 2>&1; rm -rf "$M"
du -sh "$W/translated" ~/.a8xlate-cache 2>/dev/null
log "=== autopilot done"
