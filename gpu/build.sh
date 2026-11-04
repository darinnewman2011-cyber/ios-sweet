#!/bin/bash
# GPU step 5 (docs/GPU_JACKPOTLTE.md): the userland half of the Mali stack for the Galaxy A8, built on
# macOS (GitHub's runners, .github/workflows/gpu.yml) for arm64 iOS 16.
#
#   out/mali_test          the kernel's GPU service (RedwoodMali, sweetgpu=4) through its panfrost interface
#   out/libOSMesa.8.dylib  Mesa 25.0.7 with panfrost (and softpipe), install name /sweet/lib/libOSMesa.8.dylib
#   out/gl_test            one triangle through that library on the Mali, read back and checked
#   out/libredwood-gl.dylib     OpenGL ES behind EAGLContext (step 7), for CoreAnimation in backboardd
#   out/libsweet-gl-loader.dylib  inserted into backboardd (DYLD_INSERT_LIBRARIES): loads the one above
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
"$CLANG" $TARGET -O1 -o out/sweet-run src/sweet_run.c          # the test runner (launchd job com.sweet.run)
codesign -s - -f out/mali_test out/sweet-run
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
# GPU step 7: OpenGL ES behind EAGLContext for CoreAnimation (y2sios' eagl.c; its table of functions is the
# same in iOS 16.7.16's OpenGLES as in 16.5's, all 457 entries checked) and its loader for backboardd.
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$B/exported-gl.txt"
python3 mkeagl.py src/eagl_slots.txt "$B/mesa-$VERSION/src/mapi/glapi/registry/gl.xml" "$B/exported-gl.txt" src/eagl.c 	> "$B/eagl_gen.h"
"$CLANG" $TARGET -O2 -dynamiclib -install_name /sweet/lib/libredwood-gl.dylib -I"$B" -framework CoreFoundation 	-framework IOKit -framework IOSurface -lobjc out/libOSMesa.8.dylib -o out/libredwood-gl.dylib src/eagl.c src/gl_osmesa.c
"$CLANG" $TARGET -O2 -dynamiclib -install_name /sweet/lib/libsweet-gl-loader.dylib -framework IOKit 	-o out/libsweet-gl-loader.dylib src/sweet_gl_loader.c
codesign -s - -f out/mali_test out/gl_test out/libOSMesa.8.dylib out/libredwood-gl.dylib out/libsweet-gl-loader.dylib
echo "build.sh: out/libOSMesa.8.dylib ($(wc -c < out/libOSMesa.8.dylib) bytes), out/gl_test"

# GPU step G9 (Metal), M0: the AIR translator's programs against panfrost (air-panfrost.patch). Not fatal:
# the OpenGL ES outputs above stay usable if these fail to build.
#   out/air_run              a .metallib's compute kernel (or draw) on the GPU, checked against softpipe
#   out/libredwoodair.dylib  the GPU half of the Metal driver (air_metal.h), install name /sweet/lib/libredwoodair.dylib
#   out/air_metal_test       that library driven the way the Metal runtime drives it
P=src/gallium/drivers/panfrost
if ninja -C "$B/$BUILD" $P/air_run $P/libredwoodair.dylib $P/air_metal_test > "$B/air-build.log" 2>&1; then
	cp "$B/$BUILD/$P/air_run" "$B/$BUILD/$P/libredwoodair.dylib" "$B/$BUILD/$P/air_metal_test" out/
	install_name_tool -id /sweet/lib/libredwoodair.dylib out/libredwoodair.dylib
	install_name_tool -change /redwood/lib/libredwoodair.dylib /sweet/lib/libredwoodair.dylib out/air_metal_test
	xcrun strip -x out/libredwoodair.dylib
	codesign -s - -f out/air_run out/libredwoodair.dylib out/air_metal_test
	echo "build.sh: out/air_run, out/libredwoodair.dylib ($(wc -c < out/libredwoodair.dylib) bytes), out/air_metal_test"
else
	echo "build.sh: the AIR programs did not build:"; grep -E "error|FAILED" "$B/air-build.log" | head -40
fi

# GPU step G9 (Metal), M3: the Metal runtime itself, y2sios' RedwoodMetal (userland/src/redwoodmetal.m and
# redwooddrawable.m, MIT; paths /redwood -> /sweet). Metal opens /System/Library/Extensions/<MetalPluginName>.bundle
# off the IOAcceleratorES service (xnu 0127); the bundle's executable is out/RedwoodMetal and its Info.plist
# out/RedwoodMetal.Info.plist (flat, so the checksum step sees files). It dlopens /sweet/lib/libredwoodair.dylib.
#   out/metal_props   sets / removes MetalPluginName and MetalPluginClassName (needs boot-arg sweetmetal=1)
#   out/metal_test    prints the discovery chain and what MTLCreateSystemDefaultDevice() returns
# Not fatal, like the AIR programs: each one is built on its own and a failure is printed.
M3LOG="$B/metal-build.log"; : > "$M3LOG"
m3() { local what=$1; shift; if "$CLANG" $TARGET -O2 "$@" >> "$M3LOG" 2>&1; then codesign -s - -f "out/$what" >> "$M3LOG" 2>&1; echo "build.sh: out/$what"; else echo "build.sh: $what did not build:"; grep -E "error" "$M3LOG" | tail -20 || true; fi; }
m3 RedwoodMetal -bundle -Wno-deprecated-declarations -framework Foundation -framework IOKit -framework Metal -lobjc \
	-o out/RedwoodMetal src/redwoodmetal.m src/redwooddrawable.m
cp src/metal_plugin.plist out/RedwoodMetal.Info.plist
m3 metal_props -framework CoreFoundation -framework IOKit -o out/metal_props src/metal_props.c
m3 sweet-dmesg -o out/sweet-dmesg src/sweet_dmesg.c          # the kernel message buffer, whole (no dmesg on iOS)
m3 sweet-ext -fobjc-arc -framework Foundation -o out/sweet-ext src/sweet_ext.m   # which registry knows an extension point
[ -f out/sweet-ext ] && codesign -s - -f --entitlements src/sweet_ext.entitlements out/sweet-ext >> "$M3LOG" 2>&1
m3 metal_test -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
	-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES -lMobileGestalt \
	-o out/metal_test src/metal_test.m
