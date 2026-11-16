#!/bin/bash
# Stage Apple's open source HFS for the SWEET built-in filesystem (bsd/sweet_hfs).
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
case "${MACOS_VERSION:-}" in
    13.5) hfs_tag=hfs-627.100.6 hfs_sha=31d5ec238dfab485e78a9a30b8b92d13e8914f03 ;;  # paired with xnu-8796.141.3 in macos-135
    14.6) hfs_tag=hfs-650.140.2 hfs_sha=8804cd8e786c267e2610a8e05060de4abad20b1e ;;  # paired with xnu-10063.141.1 in macos-146
    15.6) hfs_tag=hfs-683.120.3 hfs_sha=0d90b5c1924ce74cf865ff3e92f94c0cc98cd1bc ;;  # paired with xnu-11417.140.69 in macos-156
    *) echo "SWEET hfs integration: no pinned hfs for macOS ${MACOS_VERSION:-}" >&2; exit 1 ;;
esac
[[ -f bsd/kern/bsd_init.c ]] || exit 1

# Apple release.json pairing; fetch the immutable commit, never a moving branch/tag.
hfs_tmp=$(mktemp -d)
trap 'rm -rf "$hfs_tmp"' EXIT
git init -q "$hfs_tmp"
git -C "$hfs_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/hfs "$hfs_sha"
git -C "$hfs_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$hfs_tmp" rev-parse HEAD)" == "$hfs_sha" ]]

python3 - "$hfs_tmp" <<'PY'
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
target = Path('bsd/sweet_hfs')
if target.exists():
    raise SystemExit(f'refusing to overwrite existing HFS sources: {target}')
# Preserve upstream copyright/license headers; no downloaded sources in git.
for directory in ('core', 'hfs_encodings'):
    shutil.copytree(source / directory, target / directory)

# SWEET has no AppleKeyStore (content protection is not built, see
# sweet_hfs_compat.h); its header is only needed for the key-wrapping API.
path = target / 'core' / 'hfs_iokit.h'
text = path.read_text()
include = '#include <AppleKeyStore/AppleKeyStoreFSServices.h>\n'
first = 'int hfs_unwrap_key('
end = '__END_DECLS'
if text.count(include) != 1 or text.count(first) != 1 or text.count(end) != 1:
    raise SystemExit(f'HFS source changed: {path}')
text = text.replace(include, '#if CONFIG_PROTECT\n' + include + '#endif\n')
i, j = text.index(first), text.index(end)
text = text[:i] + '#if CONFIG_PROTECT\n' + text[i:j].rstrip('\n') + '\n#endif\n\n' + text[j:]
path.write_text(text)
PY

# Kernel builds use -nostdinc; HFS's kext-config.h needs the SDK's target macros.
hfs_sdk=$(xcrun --sdk macosx --show-sdk-path)
cp "$hfs_sdk/usr/include/TargetConditionals.h" bsd/sweet_hfs/
echo "Staged ${hfs_tag} ($hfs_sha) for SWEET"
