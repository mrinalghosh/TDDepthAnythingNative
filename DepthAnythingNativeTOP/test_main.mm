// test_main.mm: standalone check of DepthModel without restarting TouchDesigner (TD won't reload a .plugin while running).
// Build + run with test.sh:  ./test.sh some_photo.jpg [depth.png]
// Not part of the plugin; keep it out of build.sh's source list.

// 1. Includes.
#import "DepthModel.h"
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

static double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static NSURL *fileURL(const char *path)
{
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
}

// 3. Load the image as BGRA, top-down (the same layout TD hands the TOP). Returns false if it can't be read.
static bool loadBGRA(const char *path, std::vector<uint8_t> &pixels, int &w, int &h, size_t &stride)
{
    // 3a. Decode the first image in the file.
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)fileURL(path), NULL);
    if (!src)
        return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    if (!img) {
        CFRelease(src);
        return false;
    }

    // 3b. Tightly packed BGRA buffer.
    w = (int)CGImageGetWidth(img);
    h = (int)CGImageGetHeight(img);
    stride = (size_t)w * 4;
    pixels.assign(stride * h, 0);

    // 3c. Premultiplied-first + 32-bit little-endian = B, G, R, A bytes in memory.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, stride, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);

    // 3d. Row 0 of the bitmap memory is the top of the image, matching TD's verticalFlip = true.
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);

    // 3e. CF objects: ARC doesn't touch them.
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    CFRelease(src);
    return true;
}

// 8. Normalize to 0..255 and write a grayscale PNG. Near objects should be bright.
static bool writeDepthPNG(const char *path, const std::vector<float> &depth, int w, int h, float mn, float mx)
{
    std::vector<uint8_t> gray(depth.size());
    float range = mx > mn ? mx - mn : 1.0f;
    for (size_t i = 0; i < depth.size(); i++)
        gray[i] = (uint8_t)std::clamp(255.0f * (depth[i] - mn) / range, 0.0f, 255.0f);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(gray.data(), w, h, 8, w, cs, (CGBitmapInfo)kCGImageAlphaNone);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef dst =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(path), CFSTR("public.png"), 1, NULL);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return ok;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        // 2. Arguments: model path, image path, optional output PNG.
        if (argc < 3) {
            fprintf(stderr, "usage: %s <model.mlmodelc> <image> [depth.png]\n", argv[0]);
            return 2;
        }

        std::vector<uint8_t> bgra;
        int w = 0, h = 0;
        size_t stride = 0;
        if (!loadBGRA(argv[2], bgra, w, h, stride)) {
            fprintf(stderr, "could not read image %s\n", argv[2]);
            return 1;
        }
        printf("image:  %dx%d, stride %zu\n", w, h, stride);

        // 4. Load the model and time it (first load is the slow one).
        char err[512];
        auto start = std::chrono::steady_clock::now();
        DepthModel *model = depth_create(argv[1], err, sizeof err);
        if (!model) {
            fprintf(stderr, "depth_create failed: %s\n", err);
            return 1;
        }
        printf("create: %.0f ms\n", msSince(start));

        // 5. Allocate the output.
        int outW = 0, outH = 0;
        depth_output_size(model, &outW, &outH);
        std::vector<float> depth((size_t)outW * outH);
        printf("output: %dx%d\n", outW, outH);

        // 6. A few timed runs: the first is warm-up, later runs show steady-state speed.
        int failures = 0;
        for (int i = 0; i < 5; i++) {
            start = std::chrono::steady_clock::now();
            int rc = depth_run(model, bgra.data(), w, h, stride, depth.data());
            printf("run %d:  %.1f ms%s\n", i, msSince(start), i == 0 ? " (warm-up)" : "");
            if (rc != 0) {
                printf("        depth_run returned %d\n", rc);
                failures++;
            }
        }

        // 7. Sanity check: a constant output usually means a stride or pixel-format mistake.
        auto [mnIt, mxIt] = std::minmax_element(depth.begin(), depth.end());
        float mn = *mnIt, mx = *mxIt;
        printf("depth:  min %.4f, max %.4f, center %.4f\n", mn, mx, depth[(size_t)(outH / 2) * outW + outW / 2]);
        if (!(mx > mn)) {
            printf("FAIL: depth is constant\n");
            failures++;
        }

        // 8. Optional PNG.
        if (argc > 3) {
            if (writeDepthPNG(argv[3], depth, outW, outH, mn, mx))
                printf("wrote:  %s\n", argv[3]);
            else {
                printf("FAIL: could not write %s\n", argv[3]);
                failures++;
            }
        }

        // 9. Clean up.
        depth_destroy(model);
        printf("%s\n", failures ? "FAILED" : "OK");
        return failures ? 1 : 0;
    }
}
