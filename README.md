# TDDepthAnythingNative
apple-silicon native touchdesigner implementation for depthanything

A TouchDesigner Custom TOP that runs [Depth Anything V2 Small](https://huggingface.co/apple/coreml-depth-anything-v2-small)
through Core ML, so it uses the Neural Engine directly: no Python, no PyTorch. It outputs a 518x392 mono float
depth map (brighter = closer).

## Requirements

- Apple silicon Mac, macOS 14+ (the Core ML model's minimum)
- Xcode command line tools (`xcode-select --install`)
- CMake 3.24+ (`brew install cmake`), used once to download and compile the model
- TouchDesigner (developed against 2025.32460 and the SDK headers in this repo)

## Build, test, install

```sh
cd DepthAnythingNativeTOP
./build.sh                    # first run: CMake downloads (~50 MB) and compiles the model into ../build
./test.sh photo.jpg depth.png # optional: runs the model on a photo without TouchDesigner
./install.sh                  # copies the plugin into TouchDesigner's Plugins folder
```

Then restart TouchDesigner; **Depth Anything** appears under the Custom tab of the OP Create Dialog.
`./install.sh path/to/project/Plugins` installs it for a single project instead.

The model can also be fetched on its own with `cmake -S . -B build && cmake --build build`.

## Layout

| Path | What it is |
|---|---|
| `DepthAnythingNativeTOP/DepthModel.h/.mm` | Core ML wrapper behind a plain C API: load, run on a BGRA frame, free |
| `DepthAnythingNativeTOP/DepthAnythingNativeTOP.cpp/.h` | The TOP itself (a skeleton with numbered TIPs, work in progress) |
| `DepthAnythingNativeTOP/test_main.mm` | Standalone test of DepthModel (`test.sh`) |
| `DepthAnythingNativeTOP/build.sh`, `install.sh` | Build the `.plugin` (with the model bundled inside) and install it |
| `cmake/DepthAnythingModel.cmake` | Pinned, hash-checked model download + `coremlcompiler` step |
| `CPUMemoryTOP/` | Derivative's CPU-memory TOP sample, kept for reference |

## Performance

On an M-series Mac: about 10 s to load the model each time TouchDesigner starts (Core ML prepares it for the
Neural Engine), then about 25 ms per frame.
