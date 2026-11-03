#!/bin/bash
# GPU step 5 (docs/GPU_JACKPOTLTE.md): the userland half of the Mali stack for the Galaxy A8, built on
# macOS (GitHub's runners, .github/workflows/gpu.yml) for arm64 iOS 16.
#
#   out/mali_test          the kernel's GPU service (RedwoodMali, sweetgpu=4) through its panfrost interface
#   out/libOSMesa.8.dylib  Mesa 25.0.7 with panfrost (and softpipe), install name /sweet/lib/libOSMesa.8.dylib
#   out/gl_test            one triangle through that library on the Mali, read back and checked
#
# Everything under gpu/mesa and gpu/src comes from stormtroopxyz/y2sios (MIT, gpu/LICENSE.y2sios), which
# does the same for the Galaxy S20+'s Mali-G77: tools/mk-mesa.sh and userland/build-gpu-y2s.sh, adapted
# to Xcode's clang and Theos's iPhoneOS 16.5 SDK (it has the IOKit headers Apple's SDK leaves out).
#
# Needs: Xcode, Homebrew bison, python3. SDK=path to iPhoneOS16.5.sdk (default: theos/sdks checkout in ./sdks).
set -euo pipefail
cd "$(dirname "$0")"
G=$(pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
SDK=${SDK:-$G/sdks/iPhoneOS16.5.sdk}
CLANG=$(xcrun -f clang)
TARGET="-target arm64-apple-ios16.0 -isysroot $SDK"
B=$G/build
mkdir -p "$B" out

if [ ! -d "$SDK" ]; then
	git clone -q --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks "$G/sdks"
	git -C "$G/sdks" sparse-checkout set iPhoneOS16.5.sdk
fi

# The test of the kernel's interface needs nothing else: first, so it is there even if Mesa fails.
"$CLANG" $TARGET -O1 -framework IOKit -o out/mali_test src/mali_test.c
codesign -s - -f out/mali_test
echo "build.sh: out/mali_test ($(wc -c < out/mali_test) bytes)"

cd "$B"
if [ ! -d mesa-$VERSION ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	tar xJf mesa-$VERSION.tar.xz
	for p in "$G"/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	cp -r "$G/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm
	cp -r "$G/mesa/air" mesa-$VERSION/src/compiler/air
fi
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging meson ninja
fi
export PATH=$B/venv/bin:$(brew --prefix bison)/bin:$PATH

cat > cross.ini <<INI
[binaries]
c = ['$CLANG', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['$CLANG++', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CLANG', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '$(xcrun -f ar)'
ranlib = '$(xcrun -f ranlib)'
strip = '$(xcrun -f strip)'
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
DRIVERS=softpipe,panfrost
BUILD=build-softpipe-panfrost
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/sweet --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false | tee meson-setup.log | tail -30
ninja -C $BUILD
cd "$G"
cp "$B/$BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib" out/libOSMesa.8.dylib
xcrun strip -x out/libOSMesa.8.dylib
install_name_tool -id /sweet/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
"$CLANG" $TARGET -O1 -framework CoreFoundation -framework IOKit out/libOSMesa.8.dylib -o out/gl_test src/gl_test.c
# ld only signs macOS arm64 output by itself; iOS kills unsigned code, and the kernel's AMFI takes ad-hoc.
codesign -s - -f out/mali_test out/gl_test out/libOSMesa.8.dylib
echo "build.sh: out/libOSMesa.8.dylib ($(wc -c < out/libOSMesa.8.dylib) bytes), out/gl_test"
