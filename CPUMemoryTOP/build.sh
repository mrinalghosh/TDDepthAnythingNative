#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"
NAME=CPUMemoryTOP
BUNDLE="build/$NAME.plugin"
rm -rf "$BUNDLE" && mkdir -p "$BUNDLE/Contents/MacOS"

# Compile + link as a loadable bundle (TouchDesigner is arm64-only)
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=12.0 -Wno-invalid-offsetof \
  -bundle -o "$BUNDLE/Contents/MacOS/$NAME" CPUMemoryTOP.cpp FrameQueue.cpp

# Fill in Xcode's placeholders in Info.plist
sed -e "s/\$(EXECUTABLE_NAME)/$NAME/" \
    -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/ca.derivative.cpp.$NAME/" \
    -e "s/\$(PRODUCT_NAME)/$NAME/" Info.plist > "$BUNDLE/Contents/Info.plist"

# Ad-hoc sign (arm64 code must be signed to load)
codesign --force --sign - "$BUNDLE"
echo "Built $PWD/$BUNDLE"
