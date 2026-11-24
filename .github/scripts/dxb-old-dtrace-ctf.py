#!/usr/bin/env python3
"""dtrace of macOS 12.x installs ctfconvert/ctfmerge/ctfdump under fakeroot/<Xcode toolchain>/usr/local/bin.
Make dxb's ctf_tool_overrides() first copy them into fakeroot/usr/local/bin, where it looks."""
import sys
p = sys.argv[1]
s = open(p).read()
old = 'ctf_tool_overrides() {\n    local tool\n'
new = '''ctf_tool_overrides() {
    local tool f
    for tool in ctfconvert ctfmerge ctfdump; do
        if [ ! -x "${FAKEROOT_DIR}/usr/local/bin/${tool}" ]; then
            f=$(find "${FAKEROOT_DIR}" -name "${tool}" -type f -perm -u+x | head -1)
            [ -n "$f" ] && mkdir -p "${FAKEROOT_DIR}/usr/local/bin" && cp "$f" "${FAKEROOT_DIR}/usr/local/bin/"
        fi
    done
'''
assert s.count(old) == 1, 'ctf_tool_overrides() not found'
open(p, 'w').write(s.replace(old, new))
print('dxb: ctf tools are copied into fakeroot/usr/local/bin')
