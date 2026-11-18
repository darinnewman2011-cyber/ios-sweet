#!/bin/bash
# Builds SweetAudio.driver (the CoreAudio HAL plug-in that gives iOS its speaker, writing to /dev/sweet-audio, xnu 0141)
# for iOS 16 arm64 on a Mac (CI: .github/workflows/apps.yml): out/SweetAudio.driver, ad-hoc signed, plus
# out/SweetAudio.tar for copying into the root image's /System/Library/Audio/Plug-Ins/HAL.
# audio_plugin.c and sweet_audio.h are Scion/HakiOS's (MIT, LICENSE-HakiOS.txt).
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
rm -rf out && mkdir -p out/SweetAudio.driver
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O2 -Wall -Wno-unused-parameter \
	-bundle -framework CoreFoundation -o out/SweetAudio.driver/SweetAudio audio_plugin.c
cp Info.plist out/SweetAudio.driver/Info.plist
plutil -convert binary1 out/SweetAudio.driver/Info.plist
cp LICENSE-HakiOS.txt out/SweetAudio.driver/
codesign -s - --force out/SweetAudio.driver
codesign -dv out/SweetAudio.driver 2>&1 | head -5
tar -C out -cf out/SweetAudio.tar SweetAudio.driver
ls -l out
