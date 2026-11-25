#!/bin/bash
# a8studio (Project Beryllium) - one menu for the Galaxy A8 iOS port (Beryllium 10 = iPhone X builds, Beryllium 11 = iPhone 11 builds): pick an iOS copy (IPSW), extract it, measure and translate its
# arm64e code (a8xlate), build an A8 root image, install it, build + flash a boot image, go back to iOS 16, and
# pull logs. Run it in Terminal:  a8studio
#
# Safety rules it keeps (same as the hand-run tools):
#   - only the BOOT partition is ever flashed (flash-when-twrp.sh: device check, SHA check, readback, auto-restore)
#   - the phone must report ro.product.device == jackpotlte before anything touches it
#   - the iOS root is only ever written into /data/sweet/root.img in place, then SHA-compared with the Mac copy
#   - installing another iOS overwrites iOS 16 on the phone: it asks you to type YES, and "Back to iOS 16"
#     restores it from the Mac backup (~/iOS/a8-root-backups/a8-root-2026-11-23.img)
set -u
export PATH="$HOME/platform-tools:$HOME/bin:/usr/local/bin:$PATH"
REPO=$HOME/iOS/ios-sweet-a8; T=$HOME/iOS/_tmp; STUDIO=$HOME/iOS/studio; IPSWDIR=$HOME/iOS/ipsw
BK=$HOME/iOS/a8-root-backups
XL="python3 -I $REPO/tools/a8xlate/a8xlate.py"
ROOT_SIZE_DEFAULT=17716740096          # /data/sweet/root.img on the A8 (20 GB USERDATA, 0.9 GB free)
FALLBACK_IMG=$T/a8-ios121-boot.img     # the proven everyday iOS 16 image
mkdir -p "$STUDIO"
STATE=$STUDIO/.state; [ -f "$STATE" ] && . "$STATE"
IPSW=${IPSW:-}; NAME=${NAME:-}

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
ask() { local r; read -r -p "$1 " r || { echo; exit 0; }; echo "$r"; }   # end of input (Ctrl-D): quit
save() { printf 'IPSW=%q\nNAME=%q\n' "$IPSW" "$NAME" > "$STATE"; }
pause() { read -r -p "Press Return to go back to the menu. " _ || exit 0; }

need_a8() {
	say "Waiting for the A8 in TWRP (USB)..."
	local n=0
	until [ "$(adb get-state 2>/dev/null)" = recovery ]; do sleep 5; n=$((n + 1)); [ $n -gt 360 ] && { echo "No A8 in TWRP after 30 minutes."; return 1; }; done
	local dev; dev=$(adb shell getprop ro.product.device | tr -d '\r')
	[ "$dev" = jackpotlte ] || { echo "That device is '$dev', not the A8 (jackpotlte). Stopping."; return 1; }
	echo "A8 found (jackpotlte)."
}

# ---------------------------------------------------------------------------------------------------------- 1
pick_ipsw() {
	say "iOS copies in $IPSWDIR"
	local files=()
	while IFS= read -r f; do [ -e "$f.aria2" ] || files+=("$f"); done < <(find "$IPSWDIR" -name '*.ipsw' | sort)   # skip ones still downloading
	[ ${#files[@]} -gt 0 ] || { echo "No IPSWs yet (overnight downloads may still be running)."; return; }
	local i=1
	for f in "${files[@]}"; do printf '  %d) %s (%s)\n' $i "${f#"$IPSWDIR"/}" "$(du -h "$f" | cut -f1)"; i=$((i + 1)); done
	local c; c=$(ask "Which one? (number)")
	[[ $c =~ ^[0-9]+$ ]] && [ "$c" -ge 1 ] && [ "$c" -le ${#files[@]} ] || { echo "Not a valid choice."; return; }
	IPSW=${files[$((c - 1))]}
	NAME=$(basename "$IPSW" .ipsw | sed 's/_Restore//; s/,/-/g')
	save
	echo "Selected: $NAME"
}

# ---------------------------------------------------------------------------------------------------------- 2
manifest_path() {	# key -> path inside the IPSW (first build identity)
	python3 -I - "$STUDIO/$NAME/BuildManifest.plist" "$1" <<'EOF'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], "rb"))
for bi in m["BuildIdentities"]:
    e = bi.get("Manifest", {}).get(sys.argv[2])
    if e and e.get("Info", {}).get("Path"):
        print(e["Info"]["Path"]); break
EOF
}

extract() {
	[ -n "$IPSW" ] || { echo "Pick an iOS copy first (1)."; return; }
	local W=$STUDIO/$NAME; mkdir -p "$W"
	say "Extracting $NAME into $W"
	unzip -o -q "$IPSW" BuildManifest.plist -d "$W" || { echo "Couldn't read the IPSW."; return; }
	local key out p
	for key in OS Cryptex1,SystemOS Cryptex1,AppOS; do
		case $key in OS) out=OS.dmg ;; Cryptex1,SystemOS) out=SystemOS.dmg ;; Cryptex1,AppOS) out=AppOS.dmg ;; esac
		p=$(manifest_path "$key")
		[ -n "$p" ] || { echo "  $key: not in this IPSW (older iOS, no cryptexes)"; continue; }
		[ -s "$W/$out" ] && { echo "  $out already extracted"; continue; }
		echo "  $key: $p"
		unzip -o -q "$IPSW" "$p" -d "$W/raw" || { echo "  unzip failed"; return; }
		if [[ $p == *.aea ]]; then
			if ! command -v ipsw-bt >/dev/null; then
				echo "  $p is encrypted (AEA, iOS 18 and later). It needs blacktop's ipsw tool, installed as ~/bin/ipsw-bt."
				echo "  See 'Help' in the menu for the one-time install. Stopping here."
				return
			fi
			ipsw-bt fw aea "$W/raw/$p" --output "$W/raw" || { echo "  AEA decryption failed (see above)."; return; }
			mv "$W/raw/${p%.aea}" "$W/$out" 2>/dev/null || mv "$W/raw/$(basename "${p%.aea}")" "$W/$out"
		else
			mv "$W/raw/$p" "$W/$out"
		fi
		echo "  -> $out ($(du -h "$W/$out" | cut -f1))"
	done
	rm -rf "$W/raw"
	echo "Done."
}

# ---------------------------------------------------------------------------------------------------------- 3
scan() {
	[ -n "$NAME" ] && [ -s "$STUDIO/$NAME/OS.dmg" ] || { echo "Extract first (2)."; return; }
	local W=$STUDIO/$NAME M; M=$(mktemp -d /tmp/a8scan.XXXX)
	say "Measuring how much of $NAME needs translating (read-only)"
	local devs=() d
	for img in SystemOS OS; do
		[ -s "$W/$img.dmg" ] || continue
		mkdir -p "$M/$img"
		d=$(hdiutil attach -readonly -nobrowse -noverify -mountpoint "$M/$img" "$W/$img.dmg" 2>/dev/null | awk 'NR==1{print $1}') ||
			d=$(hdiutil attach -readonly -nobrowse -noverify -imagekey diskimage-class=CRawDiskImage -mountpoint "$M/$img" "$W/$img.dmg" | awk 'NR==1{print $1}')
		devs+=("$d")
	done
	local dsc; dsc=$(find "$M" -path '*com.apple.dyld/dyld_shared_cache_arm64*' ! -name '*.*' 2>/dev/null | head -1)
	{
		echo "== $NAME  $(date)"
		[ -n "$dsc" ] && { echo "-- shared cache: $dsc"; $XL scan "$dsc"; }
		echo "-- standalone binaries in the OS root (/usr, /sbin, /bin, /System/Library/Frameworks...)"
		$XL scan "$M/OS/usr" "$M/OS/sbin" "$M/OS/bin" "$M/OS/System/Library/CoreServices" 2>/dev/null
	} 2>&1 | tee "$W/scan.txt"
	for d in "${devs[@]}"; do hdiutil detach "$d" -force >/dev/null 2>&1; done
	rmdir "$M"/* "$M" 2>/dev/null
	echo "Saved to $W/scan.txt"
}

# ---------------------------------------------------------------------------------------------------------- 4
build_root() {
	[ -n "$NAME" ] && [ -s "$STUDIO/$NAME/OS.dmg" ] || { echo "Extract first (2)."; return; }
	local W=$STUDIO/$NAME
	[ -e "$W/root.img" ] && { [ "$(ask "$W/root.img exists. Rebuild it? (y/n)")" = y ] || return; rm -f "$W/root.img"; }
	local tr=--translate tw=--tweaks
	[ "$(ask "Translate arm64e code for the A8? (Y/n)")" = n ] && tr=
	[ "$(ask "Apply the A8 root tweaks (setup skip, crash fixes; written for iOS 16)? (Y/n)")" = n ] && tw=
	say "Building the A8 root image (asks for your Mac password: system files keep their owners)"
	bash "$REPO/tools/mkiosroot-mac.sh" "$W/OS.dmg" "$( [ -s "$W/SystemOS.dmg" ] && echo "$W/SystemOS.dmg" || echo -)" \
		"$( [ -s "$W/AppOS.dmg" ] && echo "$W/AppOS.dmg" || echo -)" "$W/root.img" $ROOT_SIZE_DEFAULT $tr $tw 2>&1 | tee "$W/build-root.log"
	grep -q ROOT_OK "$W/build-root.log" && { shasum -a 256 "$W/root.img" | tee "$W/root.sha"; } ||
		echo "The root build did not finish (log: $W/build-root.log). If it ran out of space, this iOS is bigger than the A8's 16.5 GB slot."
}

# ---------------------------------------------------------------------------------------------------------- 5
stream_root() {	# image sha256 -> writes it into /data/sweet/root.img in place and verifies
	local img=$1 want=$2
	local psize; psize=$(adb shell stat -c %s /data/sweet/root.img | tr -d '\r')
	[ "$(stat -f %z "$img")" -le "$psize" ] || { echo "Image is bigger than the phone's root file ($psize bytes). Stopping."; return 1; }
	local ino0; ino0=$(adb shell stat -c %i /data/sweet/root.img | tr -d '\r')
	echo "Writing $(basename "$img") into /data/sweet/root.img (about 14 minutes; inode $ino0 is kept)..."
	dd if="$img" bs=4194304 2>/dev/null | adb exec-in "dd of=/data/sweet/root.img bs=4194304 conv=notrunc 2>/dev/null"
	adb shell sync
	local ino1; ino1=$(adb shell stat -c %i /data/sweet/root.img | tr -d '\r')
	[ "$ino0" = "$ino1" ] || { echo "WARNING: the root file's inode changed ($ino0 -> $ino1); boot images need sweetrootino=$ino1."; }
	echo "Verifying (phone-side SHA-256 of 17 GB takes a few minutes)..."
	local got
	if [ "$(stat -f %z "$img")" = "$psize" ]; then
		got=$(adb shell sha256sum /data/sweet/root.img | tr -d '\r' | cut -d' ' -f1)
	else
		got=$(adb shell "head -c $(stat -f %z "$img") /data/sweet/root.img | sha256sum" | tr -d '\r' | cut -d' ' -f1)
	fi
	[ "$got" = "$want" ] && { echo "MATCH: the phone has exactly this root."; return 0; }
	echo "MISMATCH ($got). Run 'Back to iOS 16' to restore."; return 1
}

install_root() {
	[ -n "$NAME" ] && [ -s "$STUDIO/$NAME/root.img" ] || { echo "Build the root image first (4)."; return; }
	say "This REPLACES iOS 16 on the phone with $NAME."
	echo "iOS 16 comes back with 'Back to iOS 16' (restores $BK/a8-root-2026-11-23.img, ~15 minutes)."
	[ -s "$BK/a8-root-2026-11-23.img" ] || { echo "No iOS 16 backup at $BK/a8-root-2026-11-23.img. Stopping."; return; }
	echo "Backup: $BK/a8-root-2026-11-23.img from $(stat -f %Sm "$BK/a8-root-2026-11-23.img")."
	[ "$(ask "Type YES to replace iOS 16 on the phone:")" = YES ] || { echo "Cancelled."; return; }
	need_a8 || return
	stream_root "$STUDIO/$NAME/root.img" "$(cut -d' ' -f1 "$STUDIO/$NAME/root.sha")" && echo "$NAME" > "$STUDIO/.on-phone"
}

# ---------------------------------------------------------------------------------------------------------- 6
pick_kernel() {
	local ks=() names=() d
	d=$(ls -d "$HOME"/iOS/art1[0-9][0-9]/xnu-kernel-jackpotlte 2>/dev/null | sort -V | tail -1)
	[ -n "$d" ] && { ks+=("$d"); names+=("iOS 16 era - current A8 kernel ($(basename "$(dirname "$d")")): Wi-Fi work, fast storage, logging"); }
	[ -f "$HOME/iOS/art-xnu17e/xnu-kernel-jackpotlte/kernel.development.sweet" ] && { ks+=("$HOME/iOS/art-xnu17e/xnu-kernel-jackpotlte"); names+=("iOS/iPadOS 17 era + storage + arm64e emulator + sandbox fixes (xnu17, 0139-0143) - recommended for 17"); }
	[ -f "$HOME/iOS/art-xnu17s/xnu-kernel-jackpotlte/kernel.development.sweet" ] && { ks+=("$HOME/iOS/art-xnu17s/xnu-kernel-jackpotlte"); names+=("iOS/iPadOS 17 era + fast storage only (xnu17, no sandbox fixes: launchd stops at _container_init)"); }
	[ -d "$T/ipad17/art97" ] && { ks+=("$T/ipad17/art97"); names+=("iPadOS 17 era, older (xnu17 #97, patches up to 0138, no Wi-Fi)"); }
	[ -f "$HOME/iOS/art-xnu18f/xnu-kernel-jackpotlte-15.6/kernel.development.sweet" ] && { ks+=("$HOME/iOS/art-xnu18f/xnu-kernel-jackpotlte-15.6"); names+=("iOS/iPadOS 18 era, newest (xnu18 + CPU start fix 11-24; still freezes after 'boot heartbeat armed')"); }
	[ -f "$HOME/iOS/art-xnu18s/xnu-kernel-jackpotlte/kernel.development.sweet" ] && { ks+=("$HOME/iOS/art-xnu18s/xnu-kernel-jackpotlte"); names+=("iOS/iPadOS 18 era + fast storage (xnu18, patches to 0138 + storage backport) - recommended for 18"); }
	[ -d "$HOME/iOS/art-xnu18/xnu-kernel-jackpotlte" ] && { ks+=("$HOME/iOS/art-xnu18/xnu-kernel-jackpotlte"); names+=("iOS/iPadOS 18 era, older (xnu18, patches up to 0138, no Wi-Fi)"); }
	[ -f "$HOME/iOS/xnu-local/out-DEVELOPMENT/kernel.development.sweet" ] && { ks+=("$HOME/iOS/xnu-local/out-DEVELOPMENT"); names+=("Local Mac build (~/iOS/xnu-local)"); }
	local i=1; for n in "${names[@]}"; do printf '  %d) %s\n' $i "$n"; i=$((i + 1)); done
	local c; c=$(ask "Kernel? (number)")
	[[ $c =~ ^[0-9]+$ ]] && [ "$c" -ge 1 ] && [ "$c" -le ${#ks[@]} ] && KDIR=${ks[$((c - 1))]} || KDIR=
}

flash_boot() {
	say "Build and flash a boot image"
	pick_kernel; [ -n "${KDIR:-}" ] || { echo "No kernel chosen."; return; }
	echo "  1) iPhone device tree (Dynamic Island) - for iPhone iOS copies"
	echo "  2) iPad 7 device tree - for iPadOS copies"
	local adt base
	case $(ask "Device tree? (1/2)") in
	2) adt=$T/ipad17/jackpotlte-ipad7.adt; base=$T/ipad17/ipad17a.cmdline ;;
	*) adt=$T/a8art12/jackpotlte-island-radio.adt; base=$T/ios143w.cmdline ;;   # current kernel's settings
	esac
	[ -s "$base" ] || base=$T/ios121.cmdline
	local cmd tag; cmd=$(tr -d '\r\n' < "$base")
	if [ "$adt" = "$T/ipad17/jackpotlte-ipad7.adt" ]; then
		# iPadOS: full screen (the A8's real 1080x2220 panel) unless the iPad 7's own 4:3 canvas is wanted
		if [ "$(ask "Full screen (fill the phone's whole panel)? (Y/n; n = iPad 4:3, letterboxed)")" != n ]; then
			cmd=$(echo "$cmd" | sed 's/ *sweetfb_ipad=1//; s/ *sweetfb_fit=1//')
			echo "$cmd" | grep -q 'sweetfb_native=' || cmd="$cmd sweetfb_native=1"
		fi
		echo "$cmd" | grep -q 'sweetmmcdma=' || cmd="$cmd sweetmmcdma=1"
	fi
	cmd=$(echo "$cmd" | sed "s/sweettime=[0-9]*/sweettime=$(date +%s)/")
	if [ "$(ask "Test mode: return to TWRP after 5 minutes so logs can be pulled? (Y/n)")" != n ]; then
		echo "$cmd" | grep -q 'sweetreboot=' && cmd=$(echo "$cmd" | sed 's/sweetreboot=[0-9]*/sweetreboot=300/') || cmd="$cmd sweetreboot=300"
		tag=t
	else
		cmd=$(echo "$cmd" | sed 's/ *sweetreboot=[0-9]*//'); tag=e
	fi
	echo "$cmd" | grep -q 'sweetlog=' || cmd="$cmd sweetlog=2"
	local ino; ino=$(adb shell stat -c %i /data/sweet/root.img 2>/dev/null | tr -d '\r')
	[ -n "$ino" ] && cmd=$(echo "$cmd" | sed "s/sweetrootino=[0-9]*/sweetrootino=$ino/")
	[ ${#cmd} -le 607 ] || { echo "Command line is ${#cmd} characters (limit 607). Stopping."; return; }
	local nm="studio$(date +%m%d%H%M)$tag"
	echo "$cmd" > "$T/$nm.cmdline"
	(cd "$T" && ADT=$adt bash mkios-blobs.sh "$KDIR" "$nm" "$nm.cmdline" 2>&1 | tail -1)
	[ -s "$T/a8-$nm-boot.img" ] || { echo "Boot image build failed."; return; }
	local h; h=$(shasum -a 256 "$T/a8-$nm-boot.img" | cut -d' ' -f1)
	echo "Boot image a8-$nm-boot.img ($h)"
	[ "$(ask "Flash it to the A8 now? (y/n)")" = y ] || return
	need_a8 || return
	(cd "$T" && bash flash-when-twrp.sh "a8-$nm-boot.img" "$h" "ramoops-before-$nm.txt" 2>&1 | tail -3)
}

# ---------------------------------------------------------------------------------------------------------- 7
back_to_ios16() {
	say "Back to iOS 16"
	[ -s "$BK/a8-root-2026-11-23.img" ] || { echo "No backup at $BK/a8-root-2026-11-23.img."; return; }
	need_a8 || return
	local on; on=$(cat "$STUDIO/.on-phone" 2>/dev/null || echo "iOS 16")
	if [ "$on" != "iOS 16" ]; then
		echo "The phone has $on. Restoring the iOS 16 root from the Mac backup."
		echo "Checksumming the Mac backup first (about a minute)..."
		local h; h=$(shasum -a 256 "$BK/a8-root-2026-11-23.img" | cut -d' ' -f1)
		stream_root "$BK/a8-root-2026-11-23.img" "$h" || return
		echo "iOS 16" > "$STUDIO/.on-phone"
	else
		echo "The phone's root is already iOS 16; only the boot image is restored."
	fi
	[ -s "$FALLBACK_IMG" ] || { echo "Missing $FALLBACK_IMG"; return; }
	local h2; h2=$(shasum -a 256 "$FALLBACK_IMG" | cut -d' ' -f1)
	(cd "$T" && bash flash-when-twrp.sh "$FALLBACK_IMG" "$h2" "ramoops-before-back16.txt" 2>&1 | tail -3)
}

# ---------------------------------------------------------------------------------------------------------- 8
pull_logs() {
	need_a8 || return
	local L=$STUDIO/logs/$(date +%Y%m%d-%H%M); mkdir -p "$L"
	say "Pulling logs into $L"
	adb shell cat /sys/fs/pstore/dmesg-ramoops-0 > "$L/ramoops.txt" 2>/dev/null
	python3 -I "$REPO/tools/a8-log-pull.py" "$L/console.txt" 2>&1 | tail -1
	tr -cd '\11\12\15\40-\176' < "$L/console.txt" > "$L/console-clean.txt"
	python3 -I "$REPO/tools/a8-fwtrace.py" "$L/console-clean.txt" > "$L/wifi-firmware-trace.txt" 2>/dev/null
	python3 -I "$REPO/tools/a8-mxlog.py" "$L/console-clean.txt" > "$L/wifi-firmware-log.txt" 2>/dev/null
	grep -a -E 'panic|SWEET ROOT|launchd|exec |Bad CPU|EXC_|killed' "$L/console-clean.txt" | tail -25
	echo "Saved. Claude can read everything in $L next session."
}

help_text() {
	cat <<EOF

a8studio, step by step for a new iOS:
  1  pick an IPSW from ~/iOS/ipsw         4  build the A8 root image (+ translate, + tweaks)
  2  extract its system images            5  install that root on the phone (replaces iOS 16)
  3  measure how much needs translating   6  build + flash a boot image (pick the kernel era)
  7  back to iOS 16 whenever you want     8  pull logs after a test boot (for Claude)

Kernel eras: iOS 16 roots use the current A8 kernel. iPadOS 17 / iOS 17 roots the xnu17 kernel, iOS 18 roots
the xnu18 kernel (both without the newer Wi-Fi and storage work yet). There is no iOS 26/27 kernel yet.

Translation status: stage 1 (in place). The newer atomic instructions (LSE) used by arm64e system code are only
counted, not yet translated, so an arm64e iOS (XS 17/18, iPhone 11 26/27) will most likely stop early in boot.
Pull the logs (8) and Claude can take it from there. iPad 7 iPadOS 17/18 roots are plain arm64 and need no
translation at all.

One-time install for iOS 18+ IPSWs (encrypted system images): blacktop's ipsw, as ~/bin/ipsw-bt
  https://github.com/blacktop/ipsw/releases  (macOS x86_64 tar.gz; v3.1.743 was checked by Claude)
  then: tar -xzf ipsw_*_macOS_x86_64.tar.gz && mv ipsw ~/bin/ipsw-bt
  (macOS may ask you to allow it in System Settings > Privacy & Security the first time.)
EOF
}

while true; do
	say "Project Beryllium (a8studio)  -  selected: ${NAME:-(none)}   on phone: $(cat "$STUDIO/.on-phone" 2>/dev/null || echo 'iOS 16')"
	echo "  1) Pick an iOS copy            5) Install the root on the phone"
	echo "  2) Extract its system images   6) Build + flash a boot image"
	echo "  3) Measure (scan) translation  7) Back to iOS 16"
	echo "  4) Build the A8 root image     8) Pull logs after a test boot"
	echo "  h) Help                        q) Quit"
	case $(ask ">") in
	1) pick_ipsw ;; 2) extract ;; 3) scan ;; 4) build_root ;; 5) install_root ;; 6) flash_boot ;;
	7) back_to_ios16 ;; 8) pull_logs ;; h | H) help_text ;; q | Q) exit 0 ;; *) echo "Pick 1-8, h or q." ;;
	esac
	pause
done
