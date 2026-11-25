#!/bin/bash
# Replay the CI patch series for one macOS version against a local xnu checkout, as dxb build.sh does
# (scripts first, then *.patch in name order with git apply), and report each patch:
#   OK = git apply works, FUZZ = only applies with patch(1) fuzz/offsets (regenerate it), FAIL = rejects.
#   xnu-replay.sh XNU_SRC MACOS_VERSION DXB_PATCH_DIR [STOP_AT_FIRST_FAIL=0]
set -u
SRC=$1 V=$2 DXBP=$3 STOP=${4:-0}
REPO=$(cd "$(dirname "$0")/.." && pwd)
W=$HOME/iOS/_tmp/replay-$V; P=$W/patches; X=$W/xnu
rm -rf "$W"; mkdir -p "$P"
cp "$DXBP"/*.patch "$P"/ 2>/dev/null
cp "$REPO"/xnu/patches/*.patch "$REPO"/xnu/patches/*.sh "$P"/
for f in "$REPO"/xnu/patches-"$V"/*; do b=$(basename "$f"); case $b in *.drop) rm -f "$P/${b%.drop}" ;; *) cp "$f" "$P/$b" ;; esac; done
cp -R "$SRC" "$X"; cd "$X"; rm -rf .git; git init -q; git add -A; git -c user.email=x -c user.name=x commit -qm base
for s in "$P"/*.sh; do MACOS_VERSION=$V bash "$s" >/dev/null 2>&1 && echo "SCRIPT_OK  $(basename "$s")" || echo "SCRIPT_ERR $(basename "$s")"; done
git add -A; git -c user.email=x -c user.name=x commit -qm staged
for p in "$P"/*.patch; do
	n=$(basename "$p")
	if git apply --check "$p" 2>/dev/null; then git apply "$p"; echo "OK    $n"
	elif patch -p1 -F3 --dry-run -s < "$p" >/dev/null 2>&1; then patch -p1 -F3 -s --no-backup-if-mismatch < "$p" >/dev/null; echo "FUZZ  $n"
	else echo "FAIL  $n  $(patch -p1 -F3 --dry-run < "$p" 2>&1 | grep -c FAILED) hunk(s): $(patch -p1 -F3 --dry-run < "$p" 2>&1 | grep -B3 FAILED | grep -oE 'patching file [^ ]+' | sed 's/patching file //' | sort -u | tr '\n' ' ')"
		[ "$STOP" = 1 ] && break
	fi
done
