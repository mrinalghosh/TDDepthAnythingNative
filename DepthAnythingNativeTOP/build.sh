#!/bin/zsh
# Builds build/DepthAnythingNativeTOP.plugin, the bundle TouchDesigner loads (see install.sh to install it).
set -euo pipefail
cd "$(dirname "$0")"
NAME=DepthAnythingNativeTOP
BUNDLE="build/$NAME.plugin"
MODELC="../build/DepthAnythingV2SmallF16.mlmodelc"  # matches DA_MODEL_VARIANT in ../cmake/DepthAnythingModel.cmake

# Fetch + compile the Core ML model with CMake if it isn't there yet (first run downloads ~50 MB).
if [[ ! -f "$MODELC/coremldata.bin" ]]; then
  echo "Model not built yet; running CMake in ../build"
  cmake -S .. -B ../build
  cmake --build ../build --target da_model
fi

rm -rf "$BUNDLE" && mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

# Compile + link as a loadable bundle (TouchDesigner is arm64-only).
# macOS 14 minimum: the Core ML model itself requires it (see its metadata.json "availability").
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=14.0 -Wno-invalid-offsetof \
  -bundle -o "$BUNDLE/Contents/MacOS/$NAME" DepthAnythingNativeTOP.cpp FrameQueue.cpp DepthModel.mm \
  -fobjc-arc -framework Foundation -framework CoreML -framework CoreVideo -framework Accelerate

# Fill in Xcode's placeholders in Info.plist
sed -e "s/\$(EXECUTABLE_NAME)/$NAME/" \
    -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/ca.derivative.cpp.$NAME/" \
    -e "s/\$(PRODUCT_NAME)/$NAME/" Info.plist > "$BUNDLE/Contents/Info.plist"

# Bundle the model so the plugin is self-contained; depth_bundled_model_path() finds it at runtime.
cp -R "$MODELC" "$BUNDLE/Contents/Resources/"

# Ad-hoc sign (arm64 code must be signed to load); this also seals the bundled model.
codesign --force --sign - "$BUNDLE"
echo "Built $PWD/$BUNDLE"
