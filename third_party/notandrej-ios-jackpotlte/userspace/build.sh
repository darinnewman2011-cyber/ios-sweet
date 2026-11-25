#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the jackpotlte userspace.  Needs macOS: the phone's payloads are arm64
# iOS, which come from the iPhoneOS SDK, and the ABI self-test is a Mach-O
# binary too.  CI runs this on the macos-15 runner (.github/workflows/userspace.yml).
#
#   userspace/build.sh [DEST_DIR]         (default userspace/out)
#
# Produces:
#   out/maliprobe.host       host copy, for `--abi` (the check CI greps)
#   out/maliprobe            arm64 iOS, for the phone
#   out/test_gpu_host        host copy of the M4 request layer's test, for CI to run
#   out/libjackpotlte.a      arm64 iOS: the M4 shim (request layer + iOS backend + libdrm face)
#   out/libjackpotlte-gl.dylib  arm64 iOS: the EAGL shim (OpenGL ES behind EAGLContext)
#   out/jackpotlte_gpu.h     the headers that go with the archive
#   out/jackpotlte_drm.h     (jackpotlte_drm.h is installed as Mesa's xf86drm.h)
#
# The phone copies are not signed.  They do not have to be: the root they run in
# boots with cs_enforcement_disable=1 and launchd_unsecure_cache=1, so an
# unsigned arm64 Mach-O runs (the kernel carries built-in entitlements for
# this).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$repo/userspace/out}
probe_src=$repo/userspace/jackpotlte-maliprobe/maliprobe.c
lib=$repo/userspace/libjackpotlte

[ "$(uname)" = Darwin ] || { echo "error: $0 needs macOS (the iPhoneOS SDK and Mach-O)" >&2; exit 1; }

mkdir -p "$out"

# -Wall -Wextra -Werror on everything: the request layer's structures are an
# ABI, and a warning there is a bug.  No -Wl flags here: the -c steps below
# cannot take them, and clang counts an unused one as an error.
common=(-O2 -Wall -Wextra -Werror -Wpedantic)

# -Wl,-undefined,dynamic_lookup: IOKit's user entry points are declared in the
# sources, not linked (neither the iOS SDK nor the macOS one ships IOKit
# headers for these), so the platform's own IOKit resolves them at load time.
# Link time only - an archive is not linked.
lookup=(-Wl,-undefined,dynamic_lookup)

echo "--- host copy of the probe (the ABI self-test) ---"
clang "${common[@]}" "${lookup[@]}" -o "$out/maliprobe.host" "$probe_src"

echo "--- host copy of the M4 request layer's test ---"
# The sanitizers are the reason this is a host build and not an arm64 one: they
# catch a wrong offset or a structure written past its size, which is exactly
# what a mistake in this layer is.  jackpotlte_drm.c is in it too: its request
# translation is the part Mesa will depend on, and it runs against the same
# fake here.
clang "${common[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	-o "$out/test_gpu_host" "$lib/jackpotlte_gpu.c" "$lib/jackpotlte_drm.c" \
	"$lib/test_gpu_host.c"

echo "--- the iOS backend parses without a Darwin header ---"
# It declares its own IOKit externs on purpose (the SDK ships no headers for
# them); this is what keeps that true, and it costs one compile.
clang "${common[@]}" -c "$lib/jackpotlte_iokit.c" -o "$out/jackpotlte_iokit.host.o"

echo "--- arm64 iOS probe ---"
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang "${common[@]}" "${lookup[@]}" -target arm64-apple-ios16.0 \
	-isysroot "$sdk" -o "$out/maliprobe" "$probe_src"

echo "--- arm64 iOS libjackpotlte-gl.dylib: the EAGL shim ---"
# A dylib, not an archive: it is loaded into another process (the display server)
# and calls the OpenGL ES library put beside it on the phone (libOSMesa.8.dylib,
# tools/mesa-jackpotlte/), which this build does not have.  dynamic_lookup is
# the same arrangement the iOS backend uses for IOKit: the process that loads
# this one has the library, libobjc and IOSurface.
# gl_slots.c is generated (tools/jackpotlte/eagl-glue.py); the others are
# the OSMesa backend, the EAGLContext methods (eagl.c) and the crash trap
# (trap.c) that names the images a crash's addresses are in.
gl=$repo/userspace/jackpotlte-gl
xcrun --sdk iphoneos clang "${common[@]}" "${lookup[@]}" -target arm64-apple-ios16.0 \
	-isysroot "$sdk" -I "$gl" -dynamiclib -o "$out/libjackpotlte-gl.dylib" \
	"$gl/gl_slots.c" "$gl/jackpotlte_gl_osmesa.c" "$gl/eagl.c" "$gl/trap.c"

# The point of the generated table: no gl* function is called by name.  iOS's own
# OpenGLES.framework is in the dyld shared cache and defines every one of them,
# so a by-name reference would bind Apple's driver, which knows nothing of these
# contexts; everything goes through jackpotlte_gl_proc (OSMesaGetProcAddress).
#
# nm's own failure is checked first, and separately: in `if nm ... | grep`, a
# failing nm makes the pipeline fail and the branch is simply not taken, so the
# one check that matters could pass by never running.  nm writes the undefined
# names to stdout and exits 0; no output from it means it did not run.
gl_undefined=$(nm -u "$out/libjackpotlte-gl.dylib")
if [ -z "$gl_undefined" ]; then
	echo "error: nm -u says nothing about the shim: is it a Mach-O dylib?" >&2
	exit 1
fi
if printf '%s\n' "$gl_undefined" | grep -E ' _gl[A-Z]'; then
	echo "error: the shim refers to an OpenGL ES function by name" >&2
	exit 1
fi

echo "--- host copy of the surface-copy test ---"
# Which IOSurfaces -texImageIOSurface: turns into textures, and in what format,
# is a decision made in eagl.c and is what the lock screen's clock and camera
# button failed on (jt64: surface 6 is '..', a 256x1 float ramp, and surface 5
# is 'L008', a luminance mask; both were refused, and a refused texture is not
# drawn).  The fake IOSurface and fake GL are in the test itself, so this is a
# host build.
# -lobjc: eagl.c installs its methods with the ObjC runtime, and the host
# link does not bring it in on its own.
clang "${common[@]}" -o "$out/test_surface_copy" "$gl/test_surface_copy.c" -lobjc

echo "--- arm64 iOS libjackpotlte.a ---"
obj=$out/.obj
rm -rf "$obj"
mkdir -p "$obj"
for source in jackpotlte_gpu.c jackpotlte_iokit.c; do
	xcrun --sdk iphoneos clang "${common[@]}" -target arm64-apple-ios16.0 \
		-isysroot "$sdk" -c "$lib/$source" -o "$obj/${source%.c}.o"
done
# JACKPOTLTE_DRM_IOS: the drm face opens the service itself on the phone.  The
# host test attaches its fake instead, and must not link IOKit (above).
xcrun --sdk iphoneos clang "${common[@]}" -DJACKPOTLTE_DRM_IOS=1 \
	-target arm64-apple-ios16.0 -isysroot "$sdk" \
	-c "$lib/jackpotlte_drm.c" -o "$obj/jackpotlte_drm.o"
rm -f "$out/libjackpotlte.a"
libtool -static -o "$out/libjackpotlte.a" "$obj"/*.o
rm -rf "$obj"
cp "$lib/jackpotlte_gpu.h" "$lib/jackpotlte_drm.h" "$out/"

echo
file "$out/maliprobe" "$out/libjackpotlte-gl.dylib"
lipo -info "$out/libjackpotlte.a"
echo
echo "built:"
ls -l "$out/maliprobe" "$out/maliprobe.host" "$out/test_gpu_host" \
	"$out/test_surface_copy" "$out/libjackpotlte.a" \
	"$out/libjackpotlte-gl.dylib" \
	"$out/jackpotlte_gpu.h" "$out/jackpotlte_drm.h"
