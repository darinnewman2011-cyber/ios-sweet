#!/bin/bash
# Builds the SweetWiFi tweak (Settings + status-bar Wi-Fi/Bluetooth for the A8) for iOS 16 arm64:
# out/SweetWiFi.dylib (ad-hoc signed) + out/SweetWiFi.plist, for /var/jb/usr/lib/TweakInject in the root image.
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
rm -rf out && mkdir -p out
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -fobjc-arc -O2 -Wall -Wextra \
	-Wno-unused-parameter -dynamiclib -install_name /var/jb/usr/lib/TweakInject/SweetWiFi.dylib \
	-framework UIKit -framework Foundation -framework SystemConfiguration -o out/SweetWiFi.dylib Tweak.m
codesign -s - --force out/SweetWiFi.dylib
cp SweetWiFi.plist out/
ls -l out
