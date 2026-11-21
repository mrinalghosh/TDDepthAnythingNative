// DepthModel.h: plain C API for the Core ML depth model (implemented in DepthModel.mm).
// DepthAnythingNativeTOP.cpp includes this, so nothing here may use Objective-C types.
// The extern "C" block keeps the names unmangled; without it the .cpp and .mm sides mangle them differently and the
// link fails. Declarations here must match the definitions in DepthModel.mm exactly.

#pragma once
#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle: the fields live only in DepthModel.mm, so callers just pass the pointer around.
typedef struct DepthModel DepthModel;

// Loads a compiled Core ML model.
//  - modelcPath: path to the compiled .mlmodelc folder (e.g. build/DepthAnythingV2SmallF16.mlmodelc).
//  - Returns NULL on failure, with a readable message copied into errBuf (errBuf may be NULL or errLen 0).
//  - Slow (~10 s on first load: Core ML prepares the model for the Neural Engine): call it once, never per frame,
//    and preferably off TouchDesigner's main thread.
DepthModel *depth_create(const char *modelcPath, char *errBuf, size_t errLen);

// Reports the depth map size (518x392) so the caller can allocate depthOut before the first depth_run.
// Writes 0x0 if model is NULL; either out-pointer may be NULL.
void depth_output_size(const DepthModel *model, int *outW, int *outH);

// Runs the model on one frame.
//  - bgra: input pixels, BGRA8Fixed with rows top-down (download from TD with verticalFlip = true). Any size: it is
//    scaled to the model's input size internally.
//  - stride: bytes per row of bgra. Pass it explicitly; don't assume width * 4.
//  - depthOut: caller-owned float buffer of outW * outH, filled with relative inverse depth (bigger = closer).
//    The model scales its output so the nearest point is about 1.0 (0.9995-1.0 in tests); the far end varies.
//  - Returns 0 on success, nonzero on failure (codes are listed above depth_run in DepthModel.mm).
//  - Not thread-safe per model (the input buffer is reused): call it from one thread at a time.
int depth_run(DepthModel *model, const uint8_t *bgra, int width, int height, size_t stride, float *depthOut);

// Frees everything; safe to call with NULL.
void depth_destroy(DepthModel *model);

#ifdef __cplusplus
}
#endif
