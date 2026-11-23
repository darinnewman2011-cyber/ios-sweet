#!/bin/bash
# Builds sweetnetd (A8 Wi-Fi address -> configd) for iOS 16 arm64: out/sweetnetd (ad-hoc signed, entitlements).
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
rm -rf out && mkdir -p out
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O2 -Wall -Wextra \
	-framework CoreFoundation -framework SystemConfiguration -o out/sweetnetd main.c
codesign -s - --force --entitlements entitlements.plist out/sweetnetd
ls -l out
