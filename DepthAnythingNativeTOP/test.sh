#!/bin/zsh
# Builds and runs test_main.mm: checks DepthModel on a photo without TouchDesigner.
# Usage: ./test.sh <image> [depth.png]
#   MODEL=/path/to/other.mlmodelc ./test.sh photo.jpg   to test a different model
set -euo pipefail
if (( $# < 1 )); then
  echo "usage: $0 <image> [depth.png]" >&2
  exit 2
fi
# Resolve the paths before changing directory (zsh :A = absolute path).
args=("${1:A}")
(( $# > 1 )) && args+=("${2:A}")
cd "$(dirname "$0")"

MODEL="${MODEL:-../build/DepthAnythingV2SmallF16.mlmodelc}"
if [[ ! -d "$MODEL" ]]; then
  echo "No model at $MODEL. Run ./build.sh first (it fetches and compiles the model)." >&2
  exit 1
fi

mkdir -p build
xcrun clang++ -std=c++17 -O2 -arch arm64 -mmacosx-version-min=14.0 -Wall -fobjc-arc \
  test_main.mm DepthModel.mm -o build/depth_test \
  -framework Foundation -framework CoreML -framework CoreVideo -framework Accelerate \
  -framework ImageIO -framework CoreGraphics
build/depth_test "$MODEL" "${args[@]}"
