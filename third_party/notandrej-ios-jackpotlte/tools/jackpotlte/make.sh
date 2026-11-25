#!/bin/bash
# SPDX-License-Identifier: MIT
# One-shot provisioning pipeline for booting iOS 16.7.16 (20H392) on the
# Galaxy A8 2018 (SM-A530F, jackpotlte, Exynos 7885).
#
# This is a *skeleton*: every stage is a function, every external command is
# printed before it runs, and nothing destructive runs without a typed
# confirmation. It is meant to be read and reviewed before it is run.
#
#   make.sh [--work DIR] [--run] [--yes] [--install-deps] [--stage N] ...
#
# Stages, in order:
#   0  check_system   host/arch, required commands (table), `gh auth status`,
#                     free-space arithmetic. Read-only; refuses if space is short.
#   1  fetch_deps     all inputs land in the work dir (never in the repo):
#                     Android platform-tools (only if adb/fastboot absent),
#                     the iPhone10,3 20H392 IPSW, the latest green `xnu.yml`
#                     `xnu-kernel` artifact, the Lineage recovery image, the
#                     stock Samsung BOOT image from the Google Drive link, and
#                     (default on) the `userspace` CI artifact. Network only.
#   2  build_artifacts
#                     BOOT  = tools/jackpotlte/make-boot.sh KERNEL STOCK OUT <cmdline>
#                     ROOT  = tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg
#                             IOSRoot.img 16G, then
#                             tools/jackpotlte/apply-root-tweaks.sh IOSRoot.img
#                     SPARSE= img2simg IOSRoot.img IOSRoot.sparse.img, checked by
#                             tools/jackpotlte/verify-sparse.py.
#                     Host work only; slow and disk-hungry, not destructive.
#   3  flash          GUIDED and DESTRUCTIVE, hardware step by hardware step:
#                     Samsung download mode -> heimdall RECOVERY flash ->
#                     Lineage recovery -> Advanced -> Enter Fastboot ->
#                     `fastboot flash USERDATA <SPARSE>` -> `fastboot flash BOOT`
#                     -> boot.
#
# DESTRUCTIVE, read before proceeding:
#   * `heimdall flash --RECOVERY` OVERWRITES the recovery partition (p11).
#     AGENTS.md says to preserve recovery; the owner has explicitly asked for
#     this flash and accepts the overwrite. If you do not, stop here.
#   * `fastboot flash USERDATA` destroys the current contents of USERDATA.
#     The image MUST be the Android sparse copy, never the raw IOSRoot.img:
#     a raw 16 GiB write OOM-kills the host. Both writes ask for a typed phrase.
#   * Nothing here ever touches bootloaders, EFS (p3), persist, modem or the GPT.
#
# Disk: stage 2 needs the IPSW plus the three extracted .dmg members plus the
# 16 GiB IOSRoot.img (~12 GiB written) resident at once; the img2simg step then
# needs the raw master *and* the sparse copy at once. The script measures what
# it can and refuses below ${NEED_GIB} GiB free (see --help).
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Constants. Everything a fresh clone needs is either fetched here or built
# here; no path outside the work dir is referenced.
# ---------------------------------------------------------------------------
repo=$(cd "$(dirname "$0")/../.." && pwd)

NEED_GIB=${JACKPOTLTE_NEED_GIB:-28}   # peak free space the pipeline is refused below

IPSW_DEVICE=${JACKPOTLTE_IPSW_DEVICE:-iPhone10,3}
IPSW_BUILD=${JACKPOTLTE_IPSW_BUILD:-20H392}
# Resolved from https://api.ipsw.me/v4/device/iPhone10,3 on 2026-11-14; Apple
# may re-sign later, so fetch_deps re-queries the API when it can.
IPSW_SHA256=${JACKPOTLTE_IPSW_SHA256:-ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb}
IPSW_SIZE=${JACKPOTLTE_IPSW_SIZE:-6171035648}
IPSW_URL_FALLBACK=${JACKPOTLTE_IPSW_URL:-}
PLATFORM_TOOLS_URL=${JACKPOTLTE_PLATFORM_TOOLS_URL:-https://dl.google.com/android/repository/platform-tools-latest-linux.zip}

# github.com/A2L5E0X1/lineage release jackpotlte-20261108 (asset digest).
RECOVERY_URL=${JACKPOTLTE_RECOVERY_URL:-https://github.com/A2L5E0X1/lineage/releases/download/jackpotlte-20261108/lineage-18.1-20261108-recovery-A2L5E0X1-jackpotlte.img}
RECOVERY_SHA256=${JACKPOTLTE_RECOVERY_SHA256:-4ef5af234713b5ee3e265bb514107e1aeda6bd6f5758ee10964e1e9343c88b44}

# Google Drive id the owner verified serves boot_a530f_q.img (27,133,952 B,
# a Samsung boot image). The confirm token is re-derived if this one expires.
DRIVE_BOOT_ID=${JACKPOTLTE_DRIVE_BOOT_ID:-14uXa-Q13wr-tobRMQzTWCtizMX-Wl_4A}
BOOT_Q_SIZE=${JACKPOTLTE_BOOT_Q_SIZE:-27133952}
BOOT_Q_SHA256=${JACKPOTLTE_BOOT_Q_SHA256:-a81415f5387ebe0f4c836fdf952681bfe0272c0576d6b1c58cec3d5d3abff403}

# The BOOT cmdline of the current bring-up image (jt51). Must keep
# jtexecinject, dyld_flags=2 and jtexecenv=DYLD_AMFI_FAKE=0x2 (0159).
CMDLINE=${JACKPOTLTE_CMDLINE:-'debug=0xc serial=3 cpus=8 keepsyms=1 rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 unentitled_ios_sim_launch=1 cs_enforcement_disable=1 launchd_unsecure_cache=1 ignition_level=1 a8touchlog=1 a8touchpwr=1 -enable_kprintf_spam -noprogress jtmaliprobe=1 jtmalipwr=1 jtmalijob=1 jtmalimetal=1 jtexecinject=/usr/local/lib/libjackpotlte-gl.dylib,backboardd dyld_flags=2 jtexecenv=DYLD_AMFI_FAKE=0x2'}

ROOT_SIZE=${JACKPOTLTE_ROOT_SIZE:-16G}   # IOSRoot.img volume size (mkiosroot.sh default)
WORK=${JACKPOTLTE_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/jackpotlte/work}

DRY_RUN=1
ASSUME_YES=0
INSTALL_DEPS=0
WITH_USERSPACE=1
IGNORE_SPACE=0
RUN_STAGE=all
STOCK_IMAGE=${JACKPOTLTE_STOCK_IMAGE:-}

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
stage() { printf '\n==== stage: %s ====\n' "$*"; }
say()  { printf '%s\n' "$*"; }
info() { printf '  %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# Print a command and (unless dry-run) run it. %q keeps paths with spaces safe.
run() {
	printf '+'
	printf ' %q' "$@"
	printf '\n'
	if [ "$DRY_RUN" = 0 ]; then
		"$@"
	fi
}

usage() {
	cat <<'EOF'
Usage: make.sh [options]

A one-shot (reviewable) provisioning pipeline for iOS 16.7.16 on the
Galaxy A8 2018 (jackpotlte). By default it is DRY-RUN: it prints every
command it would run and executes nothing. Add --run to execute.

Options:
  --work DIR        work directory (default $JACKPOTLTE_WORK or
                    $XDG_CACHE_HOME/jackpotlte/work, i.e. NOT in the repo)
  --run             actually execute (default: print only)
  --yes             do not ask y/N questions (the two destructive flashes
                    still require a typed confirmation phrase)
  --stage N         only run stage N (0..3), or `all` (default)
  --stock-image F   use an existing stock Samsung boot image instead of the
                    one downloaded from the Drive link
  --with-userspace  also fetch the CI userspace artifact (maliprobe + the GL
                    shim; default on)   --no-userspace to skip
  --install-deps    opt-in: install missing commands with the detected package
                    manager, via sudo/doas/pkexec; prints every command first
  --ignore-space    proceed even if free space is below the requirement
  -h, --help        this text

What is downloaded (stage 1, into the work dir):
  platform-tools-latest-linux.zip   ~9 MB   (only if adb/fastboot are absent)
  iPhone10,3 16.7.16 20H392 IPSW    ~6.2 GB
  xnu.yml `xnu-kernel` artifact     ~20 MB  (latest successful run, via gh)
  Lineage recovery img              ~34 MB
  boot_a530f_q.img (Google Drive)   ~27 MB
  `userspace` artifact (optional)   small    maliprobe + libjackpotlte-gl/Mesa

Space (stage 2, in the work dir):
  IPSW + 3 extracted .dmg members + a 16 GiB IOSRoot.img (~12 GiB written),
  then img2simg needs the raw master (~12 GiB allocated) and the ~12 GiB
  sparse copy at the same time. The script refuses below 28 GiB free
  (override with --ignore-space or JACKPOTLTE_NEED_GIB).

Destructive (stage 3):
  heimdall flash --RECOVERY <img>   overwrites recovery (p11); AGENTS.md says
                                    preserve it, the owner asked for this flash
  fastboot flash USERDATA <sparse>  erases USERDATA; must be the sparse image
  fastboot flash BOOT <img>         overwrites BOOT (p10) only
  Never touched: bootloaders, EFS (p3), persist, modem, GPT.
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
	case $1 in
	--work)       [ $# -ge 2 ] || die "--work needs a directory"; WORK=$2; shift 2 ;;
	--run)        DRY_RUN=0; shift ;;
	--dry-run)    DRY_RUN=1; shift ;;
	--yes|-y)     ASSUME_YES=1; shift ;;
	--stage)      [ $# -ge 2 ] || die "--stage needs a number"; RUN_STAGE=$2; shift 2 ;;
	--stock-image) [ $# -ge 2 ] || die "--stock-image needs a file"; STOCK_IMAGE=$2; shift 2 ;;
	--with-userspace)    WITH_USERSPACE=1; shift ;;
	--no-userspace)      WITH_USERSPACE=0; shift ;;
	--install-deps) INSTALL_DEPS=1; shift ;;
	--ignore-space) IGNORE_SPACE=1; shift ;;
	-h|--help)    usage; exit 0 ;;
	*)            die "unknown option: $1 (try --help)" ;;
	esac
done

case $RUN_STAGE in 0|1|2|3|all) ;; *) die "--stage must be 0..3 or all" ;; esac
[ -n "$WORK" ] || die "empty work directory"
# Absolute work dir so the repo tools and children agree on it.
case $WORK in /*) ;; *) WORK=$PWD/$WORK ;; esac

# ---------------------------------------------------------------------------
# Portable helpers
# ---------------------------------------------------------------------------
sha256_of() { # FILE -> hash, sha256sum or shasum
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		die "no sha256sum or shasum to verify $1"
	fi
}

size_of() { # FILE -> bytes
	if [ -e "$1" ]; then
		if command -v stat >/dev/null 2>&1; then
			if stat -c %s "$1" >/dev/null 2>&1; then stat -c %s "$1"; else stat -f %z "$1"; fi
		else
			wc -c <"$1" | awk '{print $1}'
		fi
	else
		echo 0
	fi
}

verify_size() { # FILE EXPECTED_BYTES LABEL
	local got
	got=$(size_of "$1")
	if [ "$got" != "$2" ]; then
		warn "$3: size $got, expected $2 ($1)"
		return 1
	fi
	return 0
}

verify_sha256() { # FILE EXPECTED_HEX LABEL
	local got
	[ -s "$1" ] || { warn "$3: $1 is missing or empty"; return 1; }
	got=$(sha256_of "$1")
	if [ -n "$2" ] && [ "$got" != "$2" ]; then
		warn "$3: sha256 $got, expected $2"
		return 1
	fi
	info "$3: sha256 $got"
	return 0
}

have() { command -v "$1" >/dev/null 2>&1; }

# Missing-input guard: fatal when running, a note in dry-run so the plan prints.
need_file() { # FILE WHAT
	if [ -s "$1" ]; then return 0; fi
	if [ "$DRY_RUN" = 1 ]; then
		info "[dry-run] not present yet (an earlier stage produces it): $1"
		return 1
	fi
	die "$2: $1"
}

# ---------------------------------------------------------------------------
# Privilege escalation. mkiosroot.sh / apply-root-tweaks.sh need root to
# mount HFS+ and to run apfs-fuse; heimdall usually wants root for USB.
# Accept sudo, doas or pkexec and shim `sudo` for the tools that hardcode it.
# ---------------------------------------------------------------------------
SUDO=""
shimdir=""
detect_sudo() {
	if have sudo; then SUDO=sudo
	elif have doas; then SUDO=doas
	elif have pkexec; then SUDO=pkexec
	else SUDO=""
	fi
}

setup_sudo_shim() { # mkiosroot.sh calls `sudo` internally; provide one if needed
	[ "$SUDO" = sudo ] && return 0
	[ -n "$SUDO" ] || return 0
	shimdir=$(mktemp -d "${TMPDIR:-/tmp}/jackpotlte-sudo-XXXXXX")
	printf '#!/bin/sh\nexec %s "$@"\n' "$SUDO" >"$shimdir/sudo"
	chmod 0755 "$shimdir/sudo"
	PATH="$shimdir:$PATH"
	export PATH
	info "shimmed \`sudo\` -> $SUDO for the tools that hardcode it ($shimdir)"
}

cleanup() {
	[ -n "$shimdir" ] && rm -rf "$shimdir"
	return 0
}
trap cleanup EXIT

run_root() {
	if [ -z "$SUDO" ] && [ "$DRY_RUN" = 0 ]; then
		die "this step needs root: install sudo, doas or pkexec"
	fi
	if [ -n "$SUDO" ]; then
		run "$SUDO" "$@"
	else
		run "$@"
	fi
}

# ---------------------------------------------------------------------------
# Package-manager detection and dependency hints (never assume one name fits
# every distro).
# ---------------------------------------------------------------------------
PM=""
PM_FAMILY=none
detect_pm() {
	local m
	for m in apt-get dnf yum pacman zypper apk xbps-install; do
		if have "$m"; then PM=$m; break; fi
	done
	case $PM in
	apt-get)       PM_FAMILY=apt ;;
	dnf|yum)       PM_FAMILY=rpm ;;
	pacman)        PM_FAMILY=pacman ;;
	zypper)        PM_FAMILY=suse ;;
	apk)           PM_FAMILY=alpine ;;
	xbps-install)  PM_FAMILY=void ;;
	*)             PM_FAMILY=none ;;
	esac
}

# suggest_pkg CMD -> package name for the detected family (best effort)
suggest_pkg() {
	case $1 in
	apfs-fuse)
		printf '%s' 'NOT PACKAGED (build upstream: https://github.com/sgan81/apfs-fuse, see docs/upstream-README.md)'
		return ;;
	heimdall|haimdall)
		case $PM_FAMILY in apt) echo heimdall-flash ;; rpm) echo heimdall ;; pacman) echo heimdall ;; *) echo heimdall ;; esac
		return ;;
	fastboot|adb)
		case $PM_FAMILY in
		apt) echo android-sdk-platform-tools-common ;;
		rpm) echo android-tools ;;
		pacman) echo android-tools ;;
		suse) echo android-tools ;;
		alpine) echo android-tools ;;
		*) echo android-tools ;;
		esac
		return ;;
	img2simg|simg2img)
		case $PM_FAMILY in
		apt) echo android-sdk-libsparse-utils ;;
		*) echo android-tools ;;
		esac
		return ;;
	aarch64-linux-gnu-gcc|aarch64-linux-gnu-objcopy|aarch64-linux-gnu-readelf|aarch64-linux-gnu-nm)
		case $PM_FAMILY in apt) echo gcc-aarch64-linux-gnu ;; rpm) echo gcc-aarch64-linux-gnu ;; pacman) echo aarch64-linux-gnu-gcc ;; *) echo gcc-aarch64-linux-gnu ;; esac
		return ;;
	mkfs.hfsplus|fsck.hfsplus)
		case $PM_FAMILY in
		apt) echo hfsprogs ;;
		rpm) echo hfsplus-tools ;;
		pacman) echo hfsprogs ;;
		alpine) echo hfsprogs ;;
		*) echo hfsprogs ;;
		esac
		return ;;
	7z)  echo p7zip ;;
	gh)  echo "gh (GitHub CLI; see https://cli.github.com)" ;;
	*)
		case $1 in
		bash) echo bash ;;
		curl) echo curl ;;
		unzip) echo unzip ;;
		git) echo git ;;
		jq) echo jq ;;
		python3) echo python3 ;;
		rsync) echo rsync ;;
		dd|sha256sum|truncate|realpath|stat|mktemp|df) echo coreutils ;;
		mount|umount|mountpoint) echo util-linux ;;
		awk) echo gawk ;;
		*) echo "$1" ;;
		esac
		;;
	esac
}

# ---------------------------------------------------------------------------
# Device commands: discovered, never hardcoded, serials never echoed.
# ---------------------------------------------------------------------------
fastboot_device_present() {
	fastboot devices 2>/dev/null | awk 'NF{found=1} END{exit !found}'
}

pause_for_hardware() { # MSG
	if [ "$DRY_RUN" = 1 ]; then
		info "[dry-run] would wait for you to: $1"
		return 0
	fi
	printf '\n>>> %s\n' "$1"
	printf '    Press Enter here when it is done (Ctrl-C aborts): '
	read -r _ </dev/tty || true
}

confirm_yes() { # QUESTION
	[ "$ASSUME_YES" = 1 ] && return 0
	if [ "$DRY_RUN" = 1 ]; then return 0; fi
	local a
	printf '%s [y/N] ' "$1" >/dev/tty
	read -r a </dev/tty || a=n
	case $a in y|Y|yes|YES) return 0 ;; *) die "aborted by user" ;; esac
}

confirm_typed() { # PHRASE REASON
	if [ "$DRY_RUN" = 1 ]; then
		info "[dry-run] would require typing '$1' ($2)"
		return 0
	fi
	local a
	printf '\n!! %s\n' "$2" >&2
	printf "Type '%s' to proceed: " "$1" >/dev/tty
	read -r a </dev/tty || a=""
	[ "$a" = "$1" ] || die "aborted (phrase did not match)"
}

# ===========================================================================
# Stage 0: check_system
# ===========================================================================
MISSING_CMDS=()

check_one() { # CMD STATUS_NOTE
	local cmd=$1 note=${2:-}
	if have "$cmd"; then
		printf '  %-24s %-8s %s\n' "$cmd" "found" "$note"
	else
		printf '  %-24s %-8s %s\n' "$cmd" "MISSING" "$note"
		MISSING_CMDS+=("$cmd")
	fi
}

check_system() {
	stage "0 check_system"

	local arch
	arch=$(uname -m 2>/dev/null || echo unknown)
	case $arch in
	x86_64|aarch64|arm64) printf 'host: %s %s\n' "$arch" "$(uname -sr)" ;;
	*) warn "host arch '$arch' is untested; x86_64 or aarch64 expected" ;;
	esac

	printf '\ncommand table:\n'
	printf '  %-24s %-8s %s\n' "COMMAND" "STATUS" "NOTE"
	# Core download/verify/tooling.
	check_one bash ""
	check_one curl ""
	check_one unzip ""
	check_one git ""
	check_one gh "for the kernel/userspace artifacts"
	check_one jq "optional"
	check_one python3 "repo tools and plist parsing"
	check_one 7z "optional (image listings)"
	check_one rsync "mkiosroot.sh copies the root"
	check_one sha256sum ""
	check_one dd "optional diagnostic reads"
	check_one fastboot "stage 1 fetches platform-tools if absent"
	check_one adb "stage 1 fetches platform-tools if absent"
	check_one img2simg "sparse conversion (android-sdk-libsparse-utils)"
	check_one simg2img "reverse conversion"
	# heimdall or haimdall (the owner spelled it 'haimdall'; the real tool is
	# heimdall). Accept either name.
	if have heimdall || have haimdall; then
		printf '  %-24s %-8s %s\n' "heimdall/haimdall" "found" "$(command -v heimdall 2>/dev/null || command -v haimdall)"
	else
		printf '  %-24s %-8s %s\n' "heimdall/haimdall" "MISSING" "needed to flash recovery from download mode"
		MISSING_CMDS+=(heimdall)
	fi
	# What the tools this script calls need.
	check_one aarch64-linux-gnu-gcc "shim build (make-boot.sh)"
	check_one aarch64-linux-gnu-objcopy "shim build"
	check_one aarch64-linux-gnu-readelf "shim build"
	check_one aarch64-linux-gnu-nm "shim build"
	check_one truncate "payload/root sizing"
	check_one realpath "repo tools use it"
	check_one awk "parsing"
	check_one mountpoint "mkiosroot.sh"
	if have apfs-fuse; then
		printf '  %-24s %-8s %s\n' "apfs-fuse" "found" "$(command -v apfs-fuse)"
	else
		printf '  %-24s %-8s %s\n' "apfs-fuse" "MISSING" "required to read the IPSW .dmg members"
		MISSING_CMDS+=(apfs-fuse)
		cat >&2 <<'EOF'
  !! apfs-fuse is NOT in most distro repositories and the distro 0.33 build
     returns EIO on the LZBITMAP-compressed files in this IPSW. Build upstream
     master instead (docs/upstream-README.md, NOTES.md):
       git clone https://github.com/sgan81/apfs-fuse
       cmake -S apfs-fuse -B apfs-fuse/build -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_CXX_FLAGS="-include cstdint" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
       cmake --build apfs-fuse/build -j
EOF
	fi
	# hfsprogs: mkfs.hfsplus / fsck.hfsplus
	for c in mkfs.hfsplus fsck.hfsplus; do check_one "$c" "hfsprogs / hfsplus-tools"; done

	if [ ${#MISSING_CMDS[@]} -gt 0 ]; then
		detect_pm
		printf '\nmissing commands and a best-effort package name for this host'
		printf ' (%s):\n' "${PM:-no package manager detected}"
		local c
		for c in "${MISSING_CMDS[@]}"; do
			printf '  %-24s %s\n' "$c" "$(suggest_pkg "$c")"
		done
		printf '\n'
		if [ "$INSTALL_DEPS" = 1 ]; then
			[ -n "$PM" ] || die "--install-deps: no supported package manager found"
			[ -n "$SUDO" ] || die "--install-deps needs sudo/doas/pkexec"
			local pkgs=() p
			for c in "${MISSING_CMDS[@]}"; do
				p=$(suggest_pkg "$c")
				case $p in
				*"NOT PACKAGED"*|"gh (GitHub CLI;"*) warn "skipping $c: $p" ;;
				*) pkgs+=("$p") ;;
				esac
			done
			if [ ${#pkgs[@]} -gt 0 ]; then
				case $PM_FAMILY in
				apt)    run_root apt-get install -y "${pkgs[@]}" ;;
				rpm)    run_root dnf install -y "${pkgs[@]}" ;;
				pacman) run_root pacman -S --needed --noconfirm "${pkgs[@]}" ;;
				suse)   run_root zypper install -y "${pkgs[@]}" ;;
				alpine) run_root apk add "${pkgs[@]}" ;;
				void)   run_root xbps-install -y "${pkgs[@]}" ;;
				esac
			fi
		else
			warn "re-run with --install-deps to install the installable ones"
		fi
	fi

	# gh auth
	printf '\ngh auth:\n'
	if have gh; then
		gh auth status 2>&1 || warn "gh is not authenticated: run 'gh auth login' (artifacts need it)"
	else
		warn "gh missing: the kernel/userspace artifacts cannot be downloaded"
	fi

	# Work dir + space.
	printf '\nwork directory: %s\n' "$WORK"
	if [ "$DRY_RUN" = 0 ]; then
		mkdir -p "$WORK" || die "cannot create $WORK"
		: >"$WORK/.write-test" || die "work directory $WORK is not writable"
		rm -f "$WORK/.write-test"
	elif [ ! -d "$WORK" ]; then
		info "(does not exist yet; --run will create it)"
	fi
	space_report "$WORK"
}

space_report() { # DIR
	local checkdir=$1
	while [ ! -d "$checkdir" ]; do checkdir=$(dirname "$checkdir"); done
	local free_kib free_gib
	free_kib=$(df -P -k "$checkdir" | awk 'NR==2 {print $4}')
	[ -n "$free_kib" ] || free_kib=0
	free_gib=$(( free_kib / 1048576 ))

	local ipsw=$WORK/iPhone10,3_${IPSW_BUILD}_Restore.ipsw
	local ipsw_b now dmg_b=0
	now=$(size_of "$ipsw")
	if [ "$now" -gt 0 ]; then ipsw_b=$now; else ipsw_b=$IPSW_SIZE; fi
	if [ -f "$ipsw" ] && have python3; then
		dmg_b=$(ipsw_dmg_bytes "$ipsw" 2>/dev/null || echo 0)
	fi

	printf '  %s\n' "$(df -P -h "$checkdir" | awk 'NR==1 || NR==2')"
	printf '  free: %s GiB (%s KiB) on %s\n' "$free_gib" "$free_kib" "$checkdir"
	printf '\n  stage 2 arithmetic (worst case, all in the work dir):\n'
	printf '    IPSW (compressed) ................ %s B\n' "$ipsw_b"
	printf '    3 extracted .dmg members ........ %s B%s\n' "$dmg_b" \
		"$([ "$dmg_b" = 0 ] && echo '  (unknown until the IPSW is present)' || true)"
	printf '    IOSRoot.img, %s volume, ~12 GiB written\n' "$ROOT_SIZE"
	printf '    ipsw2root peak ................... %s GiB\n' \
		"$(( (ipsw_b + dmg_b) / 1073741824 + 12 ))"
	printf '    img2simg peak (raw ~12 GiB + sparse ~12 GiB) ... 24 GiB\n'
	printf '    enforced threshold ............... %s GiB\n' "$NEED_GIB"

	if [ "$free_gib" -lt "$NEED_GIB" ] && [ "$IGNORE_SPACE" = 0 ]; then
		die "only ${free_gib} GiB free in $checkdir, need ${NEED_GIB} GiB (override with --ignore-space)"
	fi
}

# ===========================================================================
# Stage 1: fetch_deps
# ===========================================================================
fetch_url() { # URL OUT
	local url=$1 out=$2
	if [ -s "$out" ]; then
		info "already present: $out"
		return 0
	fi
	[ "$DRY_RUN" = 1 ] || mkdir -p "$(dirname "$out")"
	if [ -s "$out.part" ]; then
		run curl -fL --retry 3 --retry-delay 2 -C - -o "$out.part" "$url"
	else
		run curl -fL --retry 3 --retry-delay 2 -o "$out.part" "$url"
	fi
	if [ "$DRY_RUN" = 0 ]; then
		mv -f "$out.part" "$out"
	else
		info "(would rename $out.part -> $out)"
	fi
}

fetch_platform_tools() {
	if have adb && have fastboot; then
		info "adb and fastboot present; skipping platform-tools"
		return 0
	fi
	case $(uname -m) in
	aarch64|arm64)
		warn "host is aarch64: Google's Linux platform-tools are x86_64-only."
		warn "install your distro's android-tools instead (--install-deps)"
		;;
	esac
	local zip=$WORK/downloads/platform-tools-latest-linux.zip
	fetch_url "$PLATFORM_TOOLS_URL" "$zip"
	if [ "$DRY_RUN" = 0 ]; then
		mkdir -p "$WORK/platform-tools"
		unzip -o -q "$zip" -d "$WORK/platform-tools"
		PATH="$WORK/platform-tools/platform-tools:$PATH"
		export PATH
		have adb && have fastboot || die "platform-tools zip has no adb/fastboot"
		info "using $WORK/platform-tools/platform-tools"
	fi
}

resolve_ipsw_url() { # -> "URL SHA256"
	local url=$IPSW_URL_FALLBACK sha=$IPSW_SHA256 api row
	if [ -n "$url" ]; then printf '%s %s\n' "$url" "$sha"; return 0; fi
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] would query https://api.ipsw.me/v4/device/%s?type=ipsw for build %s\n' \
			"$IPSW_DEVICE" "$IPSW_BUILD" >&2
		printf '%s %s\n' "<resolved-via-ipsw.me>" "$sha"
		return 0
	fi
	have curl && have python3 || die "need curl and python3 to resolve the IPSW URL"
	api=https://api.ipsw.me/v4/device/$IPSW_DEVICE?type=ipsw
	run curl -fsSL "$api" -o "$WORK/ipswme.json"
	row=$(python3 - "$WORK/ipswme.json" "$IPSW_BUILD" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for f in d.get("firmwares", []):
    if f.get("buildid") == sys.argv[2]:
        print(f["url"], f.get("sha256sum", ""))
        break
PY
)
	if [ -z "$row" ]; then
		die "no $IPSW_BUILD firmware for $IPSW_DEVICE in the ipsw.me API; set JACKPOTLTE_IPSW_URL"
	fi
	printf '%s\n' "$row"
}

fetch_ipsw() {
	local ipsw=$WORK/iPhone10,3_${IPSW_BUILD}_Restore.ipsw
	if [ -s "$ipsw" ]; then
		if verify_sha256 "$ipsw" "$IPSW_SHA256" "IPSW" 2>/dev/null; then
			info "reusing verified IPSW $ipsw"
			return 0
		fi
		warn "existing IPSW does not match the recorded sha256; re-downloading"
		[ "$DRY_RUN" = 1 ] || mv -f "$ipsw" "$ipsw.bad"
	fi
	local url sha row
	row=$(resolve_ipsw_url)
	url=${row%% *}; sha=${row##* }
	[ -n "$sha" ] || sha=$IPSW_SHA256
	fetch_url "$url" "$ipsw"
	if [ "$DRY_RUN" = 0 ]; then
		verify_size "$ipsw" "$IPSW_SIZE" "IPSW" || warn "IPSW size differs from the recorded ${IPSW_SIZE} B"
		verify_sha256 "$ipsw" "$sha" "IPSW" || die "IPSW failed sha256 verification"
	fi
}

# Resolve the OS/SystemOS/AppOS .dmg members from the IPSW's BuildManifest.
ipsw_dmg_lines() { # IPSW -> "LABEL path size" per line
	python3 - "$1" <<'PY'
import plistlib, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
m = plistlib.loads(z.read("BuildManifest.plist"))
want = {"OS": ("OS",), "SystemOS": ("SystemOS", "Cryptex1,SystemOS"),
        "AppOS": ("AppOS", "Cryptex1,AppOS")}
ids = m.get("BuildIdentities", [])
ident = next((i for i in ids
              if i.get("Info", {}).get("DeviceClass") in ("D22AP", "D221AP")), None) or ids[0]
man = ident.get("Manifest", {})
for label, keys in want.items():
    for k in keys:
        e = man.get(k)
        if isinstance(e, dict) and e.get("Info", {}).get("Path"):
            p = e["Info"]["Path"]
            print(label, p, z.getinfo(p).file_size)
            break
    else:
        sys.exit("no IPSW member for " + label)
PY
}

ipsw_dmg_bytes() { # IPSW -> sum of the three members' uncompressed size
	ipsw_dmg_lines "$1" | awk '{s+=$3} END{print s+0}'
}

fetch_kernel_artifact() {
	local dir=$WORK/kernel
	local kernel=$dir/kernel.development.a5xelte
	if [ -s "$kernel" ]; then
		info "kernel artifact already present: $kernel"
		return 0
	fi
	have gh || die "gh is required for the kernel artifact"
	local repo_slug id
	repo_slug=$(gh_repo_slug)
	info "workflow xnu.yml, latest successful run on ${repo_slug:-this repo}"
	id=$(gh run list --workflow xnu.yml --status success --limit 1 \
		--json databaseId --jq '.[0].databaseId' 2>/dev/null | tr -d '\r')
	if [ "$DRY_RUN" = 0 ]; then
		[ -n "$id" ] || die "no successful xnu.yml run found (run 'gh auth login'?)"
		mkdir -p "$dir"
		if [ -n "$repo_slug" ]; then
			run gh run download "$id" -n xnu-kernel -D "$dir" -R "$repo_slug"
		else
			run gh run download "$id" -n xnu-kernel -D "$dir"
		fi
		[ -s "$kernel" ] || die "artifact xnu-kernel has no kernel.development.a5xelte"
		kernel_sha_check "$dir" "$kernel"
	else
		run gh run download "<latest-successful-xnu.yml-run>" -n xnu-kernel -D "$dir"
	fi
}

gh_repo_slug() {
	if have gh; then
		gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null | tr -d '\r' || true
	fi
}

kernel_sha_check() { # DIR KERNEL
	local sums=$1/SHA256SUMS want got
	if [ -f "$sums" ]; then
		want=$(awk '$2=="kernel.development.a5xelte"{print $1; exit}' "$sums")
		got=$(sha256_of "$2")
		if [ -n "$want" ] && [ "$want" != "$got" ]; then
			die "kernel sha256 $got does not match SHA256SUMS ($want)"
		fi
		info "kernel sha256 verified against SHA256SUMS: $got"
	else
		warn "artifact has no SHA256SUMS; kernel sha256 is $(sha256_of "$2")"
	fi
}

fetch_recovery() {
	local out=$WORK/downloads/lineage-recovery-jackpotlte.img
	if [ -s "$out" ] && verify_sha256 "$out" "$RECOVERY_SHA256" "recovery" 2>/dev/null; then
		info "reusing verified recovery image"
		return 0
	fi
	[ -s "$out" ] && { warn "existing recovery image failed its sha256; re-downloading"; [ "$DRY_RUN" = 1 ] || rm -f "$out"; }
	fetch_url "$RECOVERY_URL" "$out"
	if [ "$DRY_RUN" = 0 ]; then
		verify_sha256 "$out" "$RECOVERY_SHA256" "recovery" || die "recovery image failed sha256 verification"
	fi
}

# Google Drive direct download. The confirm token expires; re-derive it from
# the HTML if the plain URL returns a page instead of the image.
drive_url_for() { # ID -> URL
	local id=$1 page token
	page=$(curl -fsSL -c "$WORK/downloads/gdrive.cookies" \
		"https://drive.usercontent.google.com/download?id=${id}&export=download" 2>/dev/null || true)
	token=$(printf '%s' "$page" | sed -n 's/.*name="confirm" value="\([^"]*\)".*/\1/p' | head -1)
	if [ -n "$token" ]; then
		printf 'https://drive.usercontent.google.com/download?id=%s&export=download&confirm=%s\n' "$id" "$token"
	else
		printf 'https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t\n' "$id"
	fi
}

fetch_stock_boot() {
	if [ -n "$STOCK_IMAGE" ]; then
		[ -s "$STOCK_IMAGE" ] || die "--stock-image $STOCK_IMAGE is missing"
		info "using provided stock boot image: $STOCK_IMAGE"
		return 0
	fi
	STOCK_IMAGE=$WORK/downloads/boot_a530f_q.img
	local out=$STOCK_IMAGE
	if [ -s "$out" ] && verify_size "$out" "$BOOT_Q_SIZE" "stock boot" 2>/dev/null; then
		stock_boot_check "$out" || true
		info "reusing stock boot image"
		return 0
	fi
	[ "$DRY_RUN" = 1 ] || mkdir -p "$(dirname "$out")"
	if [ "$DRY_RUN" = 1 ]; then
		run curl -fL -o "$out.part" \
			"https://drive.usercontent.google.com/download?id=${DRIVE_BOOT_ID}&export=download&confirm=t"
		info "(would rename $out.part -> $out; the confirm token is re-derived if it expired)"
		return 0
	fi
	local url
	url=$(drive_url_for "$DRIVE_BOOT_ID")
	if ! run curl -fL -o "$out.part" "$url" || ! head -c 8 "$out.part" | grep -q 'ANDROID!'; then
		warn "Drive URL returned no boot image; trying gdown"
		rm -f "$out.part"
		if have gdown || have pipx; then
			run gdown --id "$DRIVE_BOOT_ID" -O "$out.part"
		else
			die "Google Drive download failed and gdown is absent (pip install gdown)"
		fi
	fi
	head -c 8 "$out.part" | grep -q 'ANDROID!' || die "downloaded file is not a Samsung boot image"
	mv -f "$out.part" "$out"
	verify_size "$out" "$BOOT_Q_SIZE" "stock boot" || die "stock boot image size check failed"
	stock_boot_check "$out" || die "stock boot image is not usable as a container"
}

stock_boot_check() { # FILE
	head -c 8 "$1" | grep -q 'ANDROID!' || { warn "$1: no ANDROID! magic"; return 1; }
	local got
	got=$(sha256_of "$1")
	if [ -n "$BOOT_Q_SHA256" ] && [ "$got" != "$BOOT_Q_SHA256" ]; then
		warn "stock boot sha256 $got differs from the recorded $BOOT_Q_SHA256"
		warn "(a different firmware revision is fine if its header/DTBH match the A8)"
	fi
	# TODO(owner): the currently-flashed jt images were packed on
	# boot_a530f_p.img, not the Q image this script can download. The two
	# headers agree (name SRPRG03A001KU, dt_size 0xd7800, page 2048) but the
	# embedded DTB differs. Pass --stock-image to use another container.
	return 0
}

fetch_userspace_artifact() {
	local dir=$WORK/userspace
	if [ "$WITH_USERSPACE" = 0 ]; then
		info "userspace artifact skipped (--no-userspace)"
		return 0
	fi
	if [ -d "$dir" ] && find "$dir" -name maliprobe -type f | grep -q .; then
		info "userspace artifact already present"
		return 0
	fi
	have gh || { warn "gh missing; skipping the userspace artifact"; return 0; }
	local id repo_slug
	repo_slug=$(gh_repo_slug)
	id=$(gh run list --workflow userspace.yml --status success --limit 1 \
		--json databaseId --jq '.[0].databaseId' 2>/dev/null | tr -d '\r')
	if [ "$DRY_RUN" = 0 ]; then
		[ -n "$id" ] || { warn "no successful userspace.yml run; skipping"; return 0; }
		mkdir -p "$dir"
		if [ -n "$repo_slug" ]; then
			run gh run download "$id" -n userspace -D "$dir" -R "$repo_slug"
		else
			run gh run download "$id" -n userspace -D "$dir"
		fi
		USR_MALIPROBE=$(find "$dir" -name maliprobe -type f 2>/dev/null | head -1 || true)
		USR_GL=$(find "$dir" -name libjackpotlte-gl.dylib -type f 2>/dev/null | head -1 || true)
		info "maliprobe: ${USR_MALIPROBE:-absent}"
		info "GL shim:   ${USR_GL:-absent}"
	else
		run gh run download "<latest-successful-userspace.yml-run>" -n userspace -D "$dir"
	fi
}

fetch_deps() {
	stage "1 fetch_deps"
	if [ "$DRY_RUN" = 0 ]; then
		mkdir -p "$WORK/downloads"
	else
		run mkdir -p "$WORK/downloads"
	fi

	fetch_platform_tools
	fetch_ipsw
	fetch_kernel_artifact
	fetch_recovery
	fetch_stock_boot
	fetch_userspace_artifact

	if [ "$DRY_RUN" = 0 ]; then
		# Cross-check the IPSW's members with the repo's own inspector.
		if [ -x "$repo/tools/a5xelte-inspect-ipsw.py" ]; then
			run python3 "$repo/tools/a5xelte-inspect-ipsw.py" \
				"$WORK/iPhone10,3_${IPSW_BUILD}_Restore.ipsw" --device-class D22AP || true
		fi
	fi
}

# ===========================================================================
# Stage 2: build_artifacts
# ===========================================================================
kernel_path()   { printf '%s/kernel/kernel.development.a5xelte\n' "$WORK"; }
boot_out()      { printf '%s/IOSBoot.img\n' "$WORK"; }
raw_root()      { printf '%s/IOSRoot.img\n' "$WORK"; }
sparse_root()   { printf '%s/IOSRoot.sparse.img\n' "$WORK"; }

check_cmdline() {
	local missing=""
	case $CMDLINE in *jtexecinject=*) ;; *) missing="$missing jtexecinject";; esac
	case $CMDLINE in *dyld_flags=2*) ;; *) missing="$missing dyld_flags=2";; esac
	case $CMDLINE in *jtexecenv=DYLD_AMFI_FAKE=0x2*) ;; *) missing="$missing jtexecenv=DYLD_AMFI_FAKE=0x2";; esac
	[ -z "$missing" ] || die "BOOT cmdline lost required arguments:$missing"
}

build_boot() {
	local kernel stock out
	kernel=$(kernel_path)
	stock=${STOCK_IMAGE:-$WORK/downloads/boot_a530f_q.img}
	out=$(boot_out)
	need_file "$kernel" "kernel missing (run stage 1)" || true
	need_file "$stock" "stock boot image missing (run stage 1 or pass --stock-image)" || true
	check_cmdline
	[ "$DRY_RUN" = 1 ] || mkdir -p "$WORK/shim-out"
	info "cmdline: $CMDLINE"
	# make-boot.sh KERNEL STOCK OUT [boot-args...]; one quoted argument keeps
	# the whole cmdline. Its own arg-count default is unused because we pass it.
	run bash "$repo/tools/jackpotlte/make-boot.sh" "$kernel" "$stock" "$out" "$CMDLINE"
}

extract_ipsw_dmgs() { # -> writes $WORK/ipsw/{OS,SystemOS,AppOS}.dmg
	local ipsw=$WORK/iPhone10,3_${IPSW_BUILD}_Restore.ipsw
	local dir=$WORK/ipsw lines label member size
	need_file "$ipsw" "IPSW missing (run stage 1)" || return 0
	[ "$DRY_RUN" = 1 ] || mkdir -p "$dir"
	lines=$(ipsw_dmg_lines "$ipsw")
	[ -n "$lines" ] || die "could not read the .dmg members from $ipsw"
	while read -r label member size; do
		[ -n "$label" ] || continue
		if [ -s "$dir/$label.dmg" ]; then
			info "already extracted: $dir/$label.dmg"
			continue
		fi
		run unzip -o -q "$ipsw" "$member" -d "$dir"
		if [ ! -e "$dir/$label.dmg" ]; then
			run mv -f "$dir/$member" "$dir/$label.dmg"
		fi
	done <<EOF
$lines
EOF
}

build_root() {
	local raw dmgs os sysos appos
	raw=$(raw_root)
	extract_ipsw_dmgs
	dmgs=$WORK/ipsw
	os=$dmgs/OS.dmg; sysos=$dmgs/SystemOS.dmg; appos=$dmgs/AppOS.dmg
	if [ -s "$raw" ]; then
		info "IOSRoot.img already exists ($raw); delete it to rebuild"
	else
		run bash "$repo/tools/mkiosroot.sh" "$os" "$sysos" "$appos" "$raw" "$ROOT_SIZE"
	fi
	# apply-root-tweaks.sh needs root, is idempotent, and defaults its path.
	run_root bash "$repo/tools/jackpotlte/apply-root-tweaks.sh" "$raw"
}

build_sparse() {
	local raw sparse
	raw=$(raw_root); sparse=$(sparse_root)
	need_file "$raw" "raw root missing (run stage 2 up to build_root)" || true
	# Rebuild when the sparse is missing or older than the raw master.
	if [ -s "$sparse" ] && [ "$sparse" -nt "$raw" ]; then
		info "sparse copy is newer than the raw master; rebuilding anyway is optional"
	else
		run img2simg "$raw" "$sparse"
	fi
	run python3 "$repo/tools/jackpotlte/verify-sparse.py" "$raw" "$sparse"
}

build_artifacts() {
	stage "2 build_artifacts"
	# mkiosroot.sh shells out to `sudo`; shim it when only doas/pkexec exists.
	if [ "$DRY_RUN" = 0 ]; then setup_sudo_shim; fi
	build_boot
	build_root
	build_sparse
}

# ===========================================================================
# Stage 3: flash  (guided, destructive)
# ===========================================================================

enter_download_mode() {
	say 'Put the phone into Samsung download mode:'
	say '  1. Power the phone fully off.'
	say '  2. Hold Power + Volume Down + Volume Up together.'
	say '  3. On the warning screen, press Volume Up to confirm.'
	pause_for_hardware "the phone shows the download-mode screen"
}

heimdall_wait() {
	if [ -z "$SUDO" ] && [ "$DRY_RUN" = 0 ]; then
		die "heimdall needs root; install sudo/doas/pkexec"
	fi
	local tool n=0
	if have heimdall; then tool=heimdall; else tool=haimdall; fi
	while [ "$n" -lt 60 ]; do
		if [ "$DRY_RUN" = 0 ] && run_root "$tool" detect >/dev/null 2>&1; then
			info "download-mode device detected"
			return 0
		fi
		n=$((n + 1)); [ "$DRY_RUN" = 1 ] && return 0
		sleep 5
	done
	die "no download-mode device after 5 minutes"
}

flash_recovery() {
	local tool recovery
	recovery=$WORK/downloads/lineage-recovery-jackpotlte.img
	need_file "$recovery" "recovery image missing (run stage 1)" || true
	if have heimdall; then tool=heimdall; else tool=haimdall; fi
	enter_download_mode
	heimdall_wait
	warn "DESTRUCTIVE: this overwrites the recovery partition (p11)."
	warn "AGENTS.md says to preserve recovery p11; the owner explicitly asked for"
	warn "this flash. Nothing else on the device is written by this step."
	confirm_yes "Flash the Lineage recovery image to recovery?"
	confirm_typed "FLASH-RECOVERY" "Heimdall will write the RECOVERY partition."
	# Classic syntax (heimdall 2.2.2 help, verified on this host):
	#   heimdall flash --RECOVERY <img>
	# TODO(owner): the owner wrote "haimdall"; there is no such tool. `heimdall`
	# is what exists (v2.2.2 here), and its `flash` action takes
	# `--<PARTITION NAME> <file>`. Confirm the PIT's recovery partition name
	# with `heimdall print-pit` if `--RECOVERY` is rejected.
	run_root "$tool" flash --RECOVERY "$recovery"
}

reboot_to_recovery() {
	say 'Reboot into the Lineage recovery just flashed:'
	say '  1. Power the phone off (from download mode: hold Power + Volume Down).'
	say '  2. Hold Power + Volume Down, then release and immediately hold'
	say '     Power + Volume Up.'
	pause_for_hardware "the phone shows the Lineage recovery screen"
	say 'In the recovery menu: choose  Advanced  ->  Enter Fastboot.'
	pause_for_hardware "the phone is in fastboot mode"
}

fastboot_wait() {
	local n=0
	while [ "$n" -lt 60 ]; do
		if fastboot_device_present; then
			info "fastboot device detected"
			return 0
		fi
		n=$((n + 1)); [ "$DRY_RUN" = 1 ] && return 0
		sleep 5
	done
	die "no fastboot device after 5 minutes (check the USB cable/mode)"
}

flash_userdata() {
	local sparse
	sparse=$(sparse_root)
	need_file "$sparse" "sparse USERDATA image missing (run stage 2)" || true
	# Never the raw image: a raw 16 GiB write OOM-kills the host.
	case $sparse in *.sparse.img) ;; *) die "refusing: $sparse is not the sparse copy" ;; esac
	fastboot_wait
	warn "DESTRUCTIVE: this erases the current USERDATA (the iOS root)."
	warn "The image being written is the verified sparse copy, never the raw master."
	confirm_typed "ERASE-USERDATA" "fastboot will overwrite the USERDATA partition with $sparse"
	# TODO(owner): reference the actual partition name discovered, do not hardcode.
	run fastboot flash USERDATA "$sparse"
}

flash_boot() {
	local boot
	boot=$(boot_out)
	need_file "$boot" "BOOT image missing (run stage 2)" || true
	warn "DESTRUCTIVE: this overwrites BOOT (p10) only."
	confirm_yes "Flash the XNU BOOT image?"
	run fastboot flash BOOT "$boot"
}

observe_after_boot() {
	say 'Boot the phone and watch the console. Expected, in order:'
	say '  JT ENV: backboardd += DYLD_AMFI_FAKE=0x2      (kernel half, 0159)'
	say '  |backboardd| jackpotlte-gl: library loaded into the display server'
	say '  EAGLContext is ours: N of 10 methods'
	say '  the context/surface lines, then the wallpaper.'
	say 'If it stalls pre-SpringBoard (xpcproxy/bluetoothd/fairplayd repeating),'
	say 'the console ring at the tail of USERDATA holds the log: a temporarily'
	say 'booted recovery can read it with tools/sweet-log-pull.sh.'
	# TODO(owner): decide whether to automate the log pull here.
}

flash() {
	stage "3 flash (DESTRUCTIVE, guided)"
	confirm_yes "Start the guided, destructive flash sequence?"
	flash_recovery
	reboot_to_recovery
	flash_userdata
	flash_boot
	run fastboot reboot
	observe_after_boot
}

# ===========================================================================
# main
# ===========================================================================
main() {
	detect_sudo
	detect_pm

	if [ "$DRY_RUN" = 1 ]; then
		warn "DRY-RUN (default): printing commands only. Add --run to execute."
	fi

	case $RUN_STAGE in
	0)   check_system ;;
	1)   fetch_deps ;;
	2)   build_artifacts ;;
	3)   flash ;;
	all) check_system; fetch_deps; build_artifacts; flash ;;
	esac

	say ''
	if [ "$DRY_RUN" = 1 ]; then
		say "dry-run finished; re-run with --run to execute."
	else
		say "done."
	fi
}

main "$@"
