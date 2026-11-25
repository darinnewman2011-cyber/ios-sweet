#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build Mesa's panfrost (Bifrost v6) for arm64 iOS, against this port's GPU
# user client instead of libdrm.
#
#   tools/mesa-jackpotlte/mk-mesa-jackpotlte.sh [WORKDIR]
#
# Needs macOS: the target is arm64 iOS (the iPhoneOS SDK), and the driver talks
# to IOKit at load time.  Nothing is written into the repository; WORKDIR
# (default ${TMPDIR:-/tmp}/jackpotlte-mesa) holds the tarball, the Mesa tree and
# the build directory.
#
# What this is for: the M4 userspace below the Metal plug-in.  Mesa's panfrost
# expects libdrm and mmap() on a DRM device; this port has neither, so
# userspace/libjackpotlte (the drm face of the JackpotlteMali user client,
# xnu/patches/0154) is built into Mesa as its "libdrm" (src/jackpotlte-drm),
# and the series in this directory points panfrost at it.  See README.md.
#
# Products, in WORKDIR/out:
#   libOSMesa.8.dylib   Mesa's OSMesa with panfrost v6 and the drm face in it
#   built-from          the sha256 of the patches and the shim it was built from
#   meson-setup.log, build.log
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:-${TMPDIR:-/tmp}/jackpotlte-mesa}
version=25.0.7
tarball=mesa-$version.tar.xz
sha256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
patchset=$repo/tools/mesa-jackpotlte
shim=$repo/userspace/libjackpotlte

[ "$(uname)" = Darwin ] || { echo "error: $0 needs macOS (the iPhoneOS SDK and Mach-O)" >&2; exit 1; }

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
cxx=$(xcrun --sdk iphoneos --find clang++)

mkdir -p "$work"
cd "$work"

# ---- the tarball, checked ---------------------------------------------------
if [ ! -f "$tarball" ]; then
	echo "--- fetching Mesa $version ---"
	curl -sSL --fail -o "$tarball" "https://archive.mesa3d.org/$tarball"
fi
echo "$sha256  $tarball" | shasum -a 256 -c --quiet

# ---- the tree, patched once per patchset ------------------------------------
# The stamp is the patchset's own hash, so editing a patch rebuilds the tree and
# leaving them alone does not.
stamp=$(cat "$patchset"/*.patch | shasum -a 256 | awk '{print $1}')
src=mesa-$version
if [ "$(cat "$src/.jackpotlte-patches" 2>/dev/null || true)" != "$stamp" ]; then
	echo "--- extracting and patching Mesa $version ---"
	rm -rf "$src"
	tar xJf "$tarball"
	for p in "$patchset"/*.patch; do
		patch -s --no-backup-if-mismatch -d "$src" -p1 < "$p"
	done
	echo "$stamp" > "$src/.jackpotlte-patches"
fi

# ---- the drm face, always the current one -----------------------------------
# It is the live copy in userspace/libjackpotlte; jackpotlte_drm.h is installed
# as xf86drm.h because that is the header panfrost includes, and kept under its
# own name because the shim's sources include it that way.
rm -rf "$src/src/jackpotlte-drm"
mkdir -p "$src/src/jackpotlte-drm"
for f in jackpotlte_drm.c jackpotlte_drm.h jackpotlte_gpu.c jackpotlte_gpu.h \
	jackpotlte_iokit.c; do
	cp "$shim/$f" "$src/src/jackpotlte-drm/$f"
done
cp "$src/src/jackpotlte-drm/jackpotlte_drm.h" "$src/src/jackpotlte-drm/xf86drm.h"

# ---- the build tools Mesa's generator scripts need --------------------------
if [ ! -x "$work/venv/bin/meson" ]; then
	echo "--- creating the Python environment (meson, ninja, mako) ---"
	python3 -m venv "$work/venv"
	"$work/venv/bin/pip" -q install meson ninja mako pyyaml packaging
fi
export PATH=$work/venv/bin:$PATH

ar=llvm-ar ranlib=llvm-ranlib strip=llvm-strip
if ! command -v llvm-ar >/dev/null; then
	ar=ar ranlib=ranlib strip=strip
fi

# ---- the cross file ---------------------------------------------------------
cat > cross.ini <<INI
[binaries]
c = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
cpp = ['$cxx', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk', '-fuse-cxa-atexit']
objc = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
ar = '$ar'
ranlib = '$ranlib'
strip = '$strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

# ---- configure and build ----------------------------------------------------
# Size matters: this library travels in the iOS root.  softpipe is here so the
# library still draws when the GPU is not up (OSMESA_SOFTWARE=1).
build=build
if [ ! -f "$build/build.ninja" ]; then
	echo "--- meson setup ---"
	meson setup "$build" "$src" --cross-file cross.ini --wrap-mode=nofallback \
		--prefix=/jackpotlte --libdir=lib --buildtype=release \
		-Doptimization=s -Db_ndebug=true \
		-Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true \
		-Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled \
		-Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
		-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
		-Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
		-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled \
		-Dbuild-tests=false > meson-setup.log 2>&1 || {
		tail -n 60 meson-setup.log; exit 1; }
fi

echo "--- ninja (this takes a while) ---"
if ! ninja -C "$build" > build.log 2>&1; then
	tail -n 80 build.log
	exit 1
fi

# ---- what was built ---------------------------------------------------------
lib=$build/src/gallium/targets/osmesa/libOSMesa.8.dylib
[ -f "$lib" ] || { echo "error: $lib was not built" >&2; exit 1; }

mkdir -p out
cp "$lib" out/libOSMesa.8.dylib
cat "$patchset"/*.patch "$shim"/*.c "$shim"/*.h | shasum -a 256 | awk '{print $1}' > out/built-from

echo
echo "built: $work/out/libOSMesa.8.dylib ($(wc -c < out/libOSMesa.8.dylib | tr -d ' ') bytes)"
file out/libOSMesa.8.dylib
lipo -info out/libOSMesa.8.dylib 2>/dev/null || true
echo "exported gl* functions: $(nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ {print substr($3, 2)}' | wc -l | tr -d ' ')"