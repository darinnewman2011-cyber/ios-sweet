#!/bin/bash
# Port the patch series to a new xnu one patch at a time (uses the tree made by xnu-replay.sh).
#   xnu-port.sh start VERSION       reset the replay tree to "staged" and apply patches until the first failure
#   xnu-port.sh continue VERSION    after fixing the failed patch by hand (rejects resolved, *.rej deleted):
#                                   commit it, write its new diff to xnu/patches-VERSION/NAME, apply onward
# FUZZ patches (apply with offsets) are rewritten into patches-VERSION automatically so git apply works on CI.
set -u
cmd=$1 V=$2
DXBP=${3:-$HOME/iOS/_tmp/dxb26/patches/26.0}
REPO=$(cd "$(dirname "$0")/.." && pwd); W=$HOME/iOS/_tmp/replay-$V; P=$W/patches; X=$W/xnu
cd "$X" || exit 1
c() { git -c user.email=x -c user.name=x commit -qm "$1" --allow-empty; }
save() { git diff HEAD~1 HEAD > "$REPO/xnu/patches-$V/$1"; echo "SAVED $1 ($(wc -l < "$REPO/xnu/patches-$V/$1") lines)"; }
if [ "$cmd" = start ]; then
	# fresh patch list from the repo (base series + patches-VERSION overrides + dxb's own) and a staged base tree
	rm -rf "$P"; mkdir -p "$P"; cp "$DXBP"/*.patch "$P"/
	cp "$REPO"/xnu/patches/*.patch "$REPO"/xnu/patches/*.sh "$P"/
	for f in "$REPO"/xnu/patches-"$V"/*; do b=$(basename "$f"); case $b in *.drop) rm -f "$P/${b%.drop}" ;; *) cp "$f" "$P/$b" ;; esac; done
	git reset -q --hard "$(git rev-list --max-parents=0 HEAD)"; git clean -qfdx
	for s in "$P"/*.sh; do MACHINE_CONFIG=SWEET SWEET_BOARD=jackpotlte MACOS_VERSION=$V bash "$s" >/dev/null 2>&1 || { echo "staging $s failed"; exit 1; }; done
	git add -A; c staged; echo "staged: $(git show --stat HEAD | tail -1)"
	: > "$W/done.txt"
elif [ "$cmd" = continue ]; then
	n=$(cat "$W/current"); [ -z "$(find . -name '*.rej' -o -name '*.orig' | head -1)" ] || { echo "rejects left"; exit 1; }
	git add -A; c "$n"; save "$n"; echo "$n" >> "$W/done.txt"
fi
for p in "$P"/*.patch; do
	n=$(basename "$p"); grep -qxF "$n" "$W/done.txt" && continue
	if git apply --check "$p" 2>/dev/null; then git apply "$p"; git add -A; c "$n"
	elif patch -p1 -F3 --dry-run -s < "$p" >/dev/null 2>&1; then
		patch -p1 -F3 -s --no-backup-if-mismatch < "$p" >/dev/null; git add -A; c "$n"; save "$n"
	else
		echo "$n" > "$W/current"
		git apply --reject --whitespace=nowarn "$p" 2>&1 | grep -E "Rejected|error" | head -5
		echo "STOPPED at $n; rejects:"; find . -name '*.rej'; exit 2
	fi
	echo "$n" >> "$W/done.txt"
done
echo "ALL APPLIED"
