// DepthModel.mm: Core ML wrapper behind a plain C API (declared in DepthModel.h), so the TOP code never touches Objective-C.
// Built with -fobjc-arc (see build.sh): Objective-C objects are released automatically, like OP_SmartRef.
// Model I/O (from the compiled metadata.json): input "image" = RGB 518x392, output "depth" = Grayscale16Half 518x392.

#import "DepthModel.h"              // the plain C API this file implements (declarations wrapped in extern "C")
#import <Foundation/Foundation.h>   // NSString, NSURL, NSError
#import <CoreML/CoreML.h>           // MLModel, MLModelConfiguration, MLDictionaryFeatureProvider, MLFeatureValue
#import <CoreVideo/CoreVideo.h>     // CVPixelBuffer: the model's "image" input and "depth" output are both pixel buffers
#import <Accelerate/Accelerate.h>   // vImage: scaling + half->float (build.sh links -framework Accelerate)
#include <dlfcn.h>                  // dladdr: which binary (the plugin) this code was loaded from

// ---- 1. struct DepthModel ----
// 1a. Holds: MLModel *model (ARC releases it on delete, even inside a C++ struct in a .mm file).
// 1b. Holds: CVPixelBufferRef input, a 518x392 BGRA buffer reused every frame. It's a Core Foundation object, so ARC
//     does NOT manage it; release it yourself in depth_destroy.
// 1c. Holds: int inW, inH, outW, outH, read from the model description in depth_create instead of hard-coding 518x392.
struct DepthModel {
    MLModel *model;
    CVPixelBufferRef input; // 518x392 BGRA buffer
    int inW, inH, outW, outH;
};


// ---- 2. depth_create ----
// 2a. Wrap the body in @autoreleasepool { }.
// 2b. path -> [NSString stringWithUTF8String:] -> [NSURL fileURLWithPath:].
// 2c. MLModelConfiguration *config: set config.computeUnits = MLComputeUnitsAll so Core ML can use the Neural Engine.
// 2d. [MLModel modelWithContentsOfURL:configuration:error:]; on nil, strlcpy err.localizedDescription.UTF8String
//     into errBuf and return NULL.
// 2e. Read the sizes: model.modelDescription.inputDescriptionsByName[@"image"].imageConstraint.pixelsWide / pixelsHigh,
//     and the same for outputDescriptionsByName[@"depth"]. Log them once to confirm 518x392.
// 2f. CVPixelBufferCreate(kCFAllocatorDefault, inW, inH, kCVPixelFormatType_32BGRA, attrs, &input), where attrs is
//     @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{} } (bridge-cast to CFDictionaryRef) so the Neural Engine can
//     read the buffer without a copy. On failure, write an error and return NULL.
// 2g. new DepthModel, fill in the fields, return it.

// Copies msg into the caller's errBuf, tolerating a NULL or zero-length buffer.
static void depth_set_error(char *errBuf, size_t errLen, const char *msg)
{
    if (errBuf && errLen > 0)
        strlcpy(errBuf, msg ? msg : "unknown error", errLen);
}

DepthModel *depth_create(const char *modelcPath, char *errBuf, size_t errLen)
{
    depth_set_error(errBuf, errLen, "");  // start with an empty message

    // 2a. Drains the temporary NSString/NSURL/NSError objects below; also drains correctly on every early return.
    @autoreleasepool {
        if (!modelcPath) {  // stringWithUTF8String: throws on NULL
            depth_set_error(errBuf, errLen, "depth_create: modelcPath is NULL");
            return NULL;
        }

        // 2b. const char* -> NSString -> NSURL. stringWithUTF8String: returns nil for invalid UTF-8.
        NSString *pathStr = [NSString stringWithUTF8String:modelcPath];
        if (!pathStr) {
            depth_set_error(errBuf, errLen, "depth_create: modelcPath is not valid UTF-8");
            return NULL;
        }
        NSURL    *url     = [NSURL fileURLWithPath:pathStr];

        // 2c. Allow CPU, GPU, and Neural Engine (also the default; set here to make the intent explicit).
        MLModelConfiguration *config = [[MLModelConfiguration alloc] init];
        config.computeUnits = MLComputeUnitsAll;

        // 2d. Load the compiled model. This is the slow call (first load compiles for the Neural Engine).
        NSError *err = nil;
        MLModel *mlModel = [MLModel modelWithContentsOfURL:url configuration:config error:&err];
        if (!mlModel) {
            depth_set_error(errBuf, errLen, err.localizedDescription.UTF8String);
            return NULL;
        }

        // 2e. Read the sizes from the model instead of hard-coding them.
        MLModelDescription *desc = mlModel.modelDescription;
        MLImageConstraint *inC  = desc.inputDescriptionsByName[@"image"].imageConstraint;
        MLImageConstraint *outC = desc.outputDescriptionsByName[@"depth"].imageConstraint;
        if (!inC || !outC) {
            depth_set_error(errBuf, errLen,
                            "depth_create: model has no image input named \"image\" or image output named \"depth\"");
            return NULL;
        }
        int inW  = (int)inC.pixelsWide,  inH  = (int)inC.pixelsHigh;
        int outW = (int)outC.pixelsWide, outH = (int)outC.pixelsHigh;
        NSLog(@"DepthModel: input %dx%d, output %dx%d", inW, inH, outW, outH);

        // 2f. Reusable BGRA input buffer, IOSurface-backed so the Neural Engine can read it without a copy.
        NSDictionary *attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
        CVPixelBufferRef input = NULL;
        CVReturn cvr = CVPixelBufferCreate(kCFAllocatorDefault, inW, inH, kCVPixelFormatType_32BGRA,
                                           (__bridge CFDictionaryRef)attrs, &input);
        if (cvr != kCVReturnSuccess || !input) {
            char msg[128];
            snprintf(msg, sizeof msg, "depth_create: CVPixelBufferCreate failed (CVReturn %d)", (int)cvr);
            depth_set_error(errBuf, errLen, msg);
            return NULL;
        }

        // 2g. Hand everything to the struct. ARC retains mlModel via the strong field; input is released in depth_destroy.
        DepthModel *m = new DepthModel();
        m->model = mlModel;
        m->input = input;
        m->inW = inW;   m->inH = inH;
        m->outW = outW; m->outH = outH;
        return m;
    }
}

// ---- 3. depth_output_size ----
// 3a. Write model->outW / model->outH into the out-pointers.

void depth_output_size(const DepthModel *model, int *outW, int *outH)
{
    if (outW) *outW = model ? model->outW : 0;
    if (outH) *outH = model ? model->outH : 0;
}

// ---- 4. depth_run ----
// 4a. Wrap the whole body in @autoreleasepool { }, since TD's thread won't drain temporary Objective-C objects for you.
// 4b. Fill the input: CVPixelBufferLockBaseAddress(input, 0), then get base address + CVPixelBufferGetBytesPerRow.
//     Rows are often padded, so use that value as the destination rowBytes, never inW * 4.
// 4c. Resize into it: build two vImage_Buffers (src = caller's bgra/width/height/stride, dst = locked base/inW/inH/
//     bytesPerRow) and call vImageScale_ARGB8888(&src, &dst, NULL, kvImageNoFlags). It doesn't care about
//     channel order, so BGRA stays BGRA. No R/B swap or row flip needed: TD already delivers BGRA top-down.
// 4d. CVPixelBufferUnlockBaseAddress(input, 0).
// 4e. [MLFeatureValue featureValueWithPixelBuffer:input] -> [[MLDictionaryFeatureProvider alloc]
//     initWithDictionary:@{ @"image": value } error:&err].
// 4f. [model predictionFromFeatures:provider error:&err]; on nil, NSLog the error and return nonzero.
// 4g. CVPixelBufferRef out = [result featureValueForName:@"depth"].imageBufferValue. Confirm with
//     CVPixelBufferGetPixelFormatType that it's kCVPixelFormatType_OneComponent16Half; return nonzero if not.
// 4h. CVPixelBufferLockBaseAddress(out, kCVPixelBufferLock_ReadOnly); again use its own bytes-per-row.
// 4i. Half -> float into depthOut: vImageConvert_Planar16FtoPlanarF (src = out's base/outW/outH/bytesPerRow,
//     dst = depthOut/outW/outH/outW * sizeof(float)), or loop the rows reading __fp16.
// 4j. Unlock with the same kCVPixelBufferLock_ReadOnly flag. Don't release out: Core ML owns it.
// 4k. Return 0. Leave the min/max normalization to the TOP, so this stays a thin model wrapper.
//
// Not thread-safe: model->input is shared, so call depth_run from one thread at a time.
// Return codes: 0 ok, 1 bad arguments, 2 input lock failed, 3 resize failed, 4 feature provider failed,
//               5 prediction failed, 6 no "depth" output, 7 unexpected output format/size, 8 output lock failed,
//               9 half->float conversion failed.

int depth_run(DepthModel *model, const uint8_t *bgra, int width, int height, size_t stride, float *depthOut)
{
    if (!model || !bgra || !depthOut || width <= 0 || height <= 0 || stride < (size_t)width * 4)
        return 1;

    // 4a. Drains the per-frame Objective-C temporaries (feature value, provider, prediction result) every call.
    @autoreleasepool {
        // 4b. Lock the reusable input buffer for CPU writes. Its rows may be padded: use its own bytes-per-row.
        if (CVPixelBufferLockBaseAddress(model->input, 0) != kCVReturnSuccess)
            return 2;

        // 4c. Resize the caller's BGRA image into the model's input size. vImage_Buffer is {data, height, width, rowBytes}.
        vImage_Buffer src = { (void *)bgra, (vImagePixelCount)height, (vImagePixelCount)width, stride };
        vImage_Buffer dst = { CVPixelBufferGetBaseAddress(model->input),
                              (vImagePixelCount)model->inH, (vImagePixelCount)model->inW,
                              CVPixelBufferGetBytesPerRow(model->input) };
        vImage_Error verr = vImageScale_ARGB8888(&src, &dst, NULL, kvImageNoFlags);

        // 4d. Unlock before Core ML reads the buffer (also on the error path).
        CVPixelBufferUnlockBaseAddress(model->input, 0);
        if (verr != kvImageNoError)
            return 3;

        // 4e. Wrap the pixel buffer as the model's "image" input.
        NSError *err = nil;
        MLFeatureValue *value = [MLFeatureValue featureValueWithPixelBuffer:model->input];
        MLDictionaryFeatureProvider *provider =
            [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{ @"image": value } error:&err];
        if (!provider) {
            NSLog(@"DepthModel: feature provider failed: %@", err.localizedDescription);
            return 4;
        }

        // 4f. Run the model.
        id<MLFeatureProvider> result = [model->model predictionFromFeatures:provider error:&err];
        if (!result) {
            NSLog(@"DepthModel: prediction failed: %@", err.localizedDescription);
            return 5;
        }

        // 4g. Get the "depth" pixel buffer and check it's what we expect.
        //     Core ML owns it, but ARC may release `result` (and with it the buffer) right after its last use, so take
        //     our own reference for the rest of this function and drop it before returning.
        CVPixelBufferRef out = [result featureValueForName:@"depth"].imageBufferValue;
        if (!out)
            return 6;
        CVPixelBufferRetain(out);

        int rc = 0;
        if (CVPixelBufferGetPixelFormatType(out) != kCVPixelFormatType_OneComponent16Half ||
            CVPixelBufferGetWidth(out)  != (size_t)model->outW ||
            CVPixelBufferGetHeight(out) != (size_t)model->outH) {
            NSLog(@"DepthModel: unexpected depth output (format 0x%08x, %zux%zu)",
                  (unsigned)CVPixelBufferGetPixelFormatType(out), CVPixelBufferGetWidth(out), CVPixelBufferGetHeight(out));
            rc = 7;
        }
        // 4h. Lock read-only; the output has its own (possibly padded) bytes-per-row.
        else if (CVPixelBufferLockBaseAddress(out, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            rc = 8;
        }
        else {
            // 4i. Half -> float into the caller's tightly packed outW * outH buffer.
            vImage_Buffer hsrc = { CVPixelBufferGetBaseAddress(out),
                                   (vImagePixelCount)model->outH, (vImagePixelCount)model->outW,
                                   CVPixelBufferGetBytesPerRow(out) };
            vImage_Buffer fdst = { depthOut,
                                   (vImagePixelCount)model->outH, (vImagePixelCount)model->outW,
                                   (size_t)model->outW * sizeof(float) };
            if (vImageConvert_Planar16FtoPlanarF(&hsrc, &fdst, kvImageNoFlags) != kvImageNoError)
                rc = 9;

            // 4j. Unlock with the same read-only flag.
            CVPixelBufferUnlockBaseAddress(out, kCVPixelBufferLock_ReadOnly);
        }

        CVPixelBufferRelease(out);  // balances our CVPixelBufferRetain above, not Core ML's ownership
        // 4k. Raw relative inverse depth; normalization happens in the TOP.
        return rc;
    }
}

// ---- 5. depth_destroy ----
// 5a. If model is NULL, return.
// 5b. CVPixelBufferRelease(model->input).
// 5c. delete model; ARC releases the MLModel.

void depth_destroy(DepthModel *model)
{
    if (!model)
        return;

    // Tearing down the MLModel can create autoreleased objects too, so give them a pool to drain into.
    @autoreleasepool {
        CVPixelBufferRelease(model->input);  // CF object: ARC doesn't manage it
        model->input = NULL;
        delete model;                        // ARC releases the strong MLModel field here
    }
}

// ---- 6. depth_bundled_model_path ----
// 6a. dladdr on one of our own functions gives the path of the binary that contains it. Inside the plugin that's
//     .../DepthAnythingNativeTOP.plugin/Contents/MacOS/DepthAnythingNativeTOP, so two levels up is Contents/.
// 6b. build.sh copies the compiled model into Contents/Resources/; take the first *.mlmodelc found there.

int depth_bundled_model_path(char *buf, size_t len)
{
    if (!buf || len == 0)
        return 1;
    buf[0] = '\0';

    @autoreleasepool {
        Dl_info info;
        if (!dladdr((const void *)&depth_bundled_model_path, &info) || !info.dli_fname)
            return 2;

        NSString *binary = [NSString stringWithUTF8String:info.dli_fname];
        NSString *resources = [[[binary stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                               stringByAppendingPathComponent:@"Resources"];

        NSArray<NSString *> *names =
            [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:resources error:nil]
                sortedArrayUsingSelector:@selector(compare:)];
        for (NSString *name in names) {
            if ([name.pathExtension isEqualToString:@"mlmodelc"]) {
                NSString *path = [resources stringByAppendingPathComponent:name];
                return strlcpy(buf, path.fileSystemRepresentation, len) < len ? 0 : 3;  // 3 = buf too small
            }
        }
        return 4;  // no bundled model (not running from the plugin, or build.sh didn't copy one)
    }
}
