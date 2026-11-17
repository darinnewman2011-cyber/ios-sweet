#!/bin/bash
# Builds Sweet Connect for iOS 16 arm64 on a Mac (CI: .github/workflows/apps.yml): out/SweetConnect.app, ad-hoc signed,
# plus out/SweetConnect.tar for copying into the root image's /Applications.
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
rm -rf out && mkdir -p out/SweetConnect.app
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter \
	-framework UIKit -framework Foundation -framework CoreGraphics -o out/SweetConnect.app/SweetConnect main.m
cp Info.plist out/SweetConnect.app/Info.plist
plutil -convert binary1 out/SweetConnect.app/Info.plist
codesign -s - --force --entitlements entitlements.plist out/SweetConnect.app
codesign -dv out/SweetConnect.app 2>&1 | head -5
tar -C out -cf out/SweetConnect.tar SweetConnect.app
ls -l out
