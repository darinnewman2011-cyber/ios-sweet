#!/bin/bash
# One-time (and after tool updates): install Project Beryllium's root tools root-owned and allow ONLY
# /usr/local/libexec/beryllium/beryllium-root to run without a password.
#
#   sudo bash ~/iOS/ios-sweet-a8/tools/setup-beryllium-sudo.sh          (install / update)
#   sudo bash ~/iOS/ios-sweet-a8/tools/setup-beryllium-sudo.sh remove   (take it all away again)
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
LIB=/usr/local/libexec/beryllium
RULE=/etc/sudoers.d/beryllium
if [ "${1:-}" = remove ]; then
	rm -f "$RULE"; rm -rf "$LIB"; echo "removed $RULE and $LIB"; exit 0
fi
rm -rf "$LIB"; install -d -o root -g wheel -m 0755 "$LIB" "$LIB/tools"
install -o root -g wheel -m 0755 "$REPO/tools/beryllium-root" "$LIB/beryllium-root"
# the root builder and everything it calls, as root-owned copies (edit the repo, then rerun this script)
for f in mkiosroot-mac.sh ios-minimal.filter sweet-root-tweaks.py; do
	[ -e "$REPO/tools/$f" ] && install -o root -g wheel -m 0755 "$REPO/tools/$f" "$LIB/tools/$f"
done
for d in a8xlate stubs; do
	[ -d "$REPO/tools/$d" ] && { cp -R "$REPO/tools/$d" "$LIB/tools/$d"; chown -R root:wheel "$LIB/tools/$d"; chmod -R go-w "$LIB/tools/$d"; }
done
mkdir -p /Users/darin/iOS/rootpatch && chown darin:staff /Users/darin/iOS/rootpatch
TMP=$(mktemp)
echo "darin ALL=(root) NOPASSWD: $LIB/beryllium-root" > "$TMP"
visudo -cf "$TMP" >/dev/null || { rm -f "$TMP"; echo "sudoers check failed, nothing changed"; exit 1; }
install -o root -g wheel -m 0440 "$TMP" "$RULE"; rm -f "$TMP"
echo "OK: sudo -n $LIB/beryllium-root <build|mount|put|unmount> ... works without a password; nothing else does."
