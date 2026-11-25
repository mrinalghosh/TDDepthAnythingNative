/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
 * and can only be used, and/or modified for use, in conjunction with
 * Derivative's TouchDesigner software, and only if you are a licensee who has
 * accepted Derivative's TouchDesigner license or assignment agreement
 * (which also govern the use of this file). You may share or redistribute
 * a modified version of this file provided the following conditions are met:
 *
 * 1. The shared file or redistribution must retain the information set out
 * above and this list of conditions.
 * 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
 * to endorse or promote products derived from this file without specific
 * prior written permission from Derivative.
 */

#include "DepthAnythingNativeTOP.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>

// =====================================================================================================================
// How the pieces fit together
//
//   execute()  [TD main thread, once per cook]          worker thread  [threaded version only]
//   ------------------------------------------          --------------------------------------
//   E1 get input TOP
//   E3 start GPU->CPU download  -- OP_TOPDownloadResult -->  getData()  (waits for the download)
//                                                            E4 depth_run   (~25 ms on an M-series Mac)
//                                                            E5 normalize into a TOP_Buffer
//   E6 upload newest finished buffer  <-- FrameQueue -----  updateComplete()
//
// Now: synchronous, E1-E6 all inside execute(), so TD's frame waits on the model.
// Next: threaded. Keep E1/E3/E6 in execute() and move E4/E5 to a worker (TIP W1-W5). TD runs at full rate and the
// depth map updates as fast as the model can go. ../CPUMemoryTOP/CPUMemoryTOP.cpp (THREADING_EXAMPLE) does the same
// FrameQueue hand-off with a test pattern; it's worth reading first.
//
// Iterating: ./build.sh && ./install.sh, then restart TouchDesigner (it only loads plugins at startup).
// Check DepthModel on its own with ./test.sh photo.jpg out.png; no TD restart needed.
// =====================================================================================================================

// Plugin entry points. TD finds these plain C functions by name when it loads the .plugin.
extern "C" {

DLLEXPORT
void FillTOPPluginInfo(TOP_PluginInfo* info) {
    // This must always be set to this constant
    if (!info->setAPIVersion(TOPCPlusPlusAPIVersion))
        return;

    // CPU memory mode: we fill CPU buffers and TD uploads them to textures for us.
    info->executeMode = TOP_ExecuteMode::CPUMem;

    // Unique name for this TOP: a capital A-Z first, then only lower case a-z or 0-9.
    info->customOPInfo.opType->setString("Depthanything");

    // The text that shows up in the OP Create Dialog, and the 3 letter icon on the node.
    info->customOPInfo.opLabel->setString("Depth Anything");
    info->customOPInfo.opIcon->setString("DEP");

    info->customOPInfo.authorName->setString("Mrinal Ghosh");
    info->customOPInfo.authorEmail->setString("");

    // Needs exactly one image to estimate depth from; TD flags the node if it's missing.
    info->customOPInfo.minInputs = 1;
    info->customOPInfo.maxInputs = 1;

    info->customOPInfo.opHelpURL->setString("https://github.com/mrinalghosh/TDDepthAnythingNative");
}

DLLEXPORT
TOP_CPlusPlusBase* CreateTOPInstance(const OP_NodeInfo* info, TOP_Context* context) {
    // Called once per Depth Anything TOP in the project.
    return new DepthAnythingNativeTOP(info, context);
}

DLLEXPORT
void DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context* context) {
    // Called when the TOP is deleted, TD shuts down, or the TOP loads a different plugin.
    delete (DepthAnythingNativeTOP*)instance;
}
};

DepthAnythingNativeTOP::DepthAnythingNativeTOP(const OP_NodeInfo* info, TOP_Context* context)
    : myNodeInfo(info), myContext(context), myFrameQueue(context) {
    // The model isn't loaded here: depth_create takes ~10 s and TD's UI is frozen while the constructor runs.
    // It loads on the first cook instead (E2); in the threaded version, the worker loads it (W1).
}

DepthAnythingNativeTOP::~DepthAnythingNativeTOP() {
    // TIP D1: Threaded version: stop the worker first. Set myExit = true while holding myLock, myWake.notify_one(),
    //         then join it, but only if (myWorker.joinable()): the worker starts on the first cook, so a TOP that
    //         never cooked has none, and join() on an empty std::thread throws (= TD crashes).
    //         It may be in the middle of depth_run, so the model must outlive it.
    depth_destroy(myModel);  // fine with nullptr; OP_SmartRef members release themselves
}

void DepthAnythingNativeTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1) {
    // Cook every frame while something downstream is looking at this TOP, so new depth frames show up even if the
    // input itself didn't change (the threaded version finishes frames asynchronously).
    ginfo->cookEveryFrameIfAsked = true;
}

void DepthAnythingNativeTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void* reserved1) {
    // E1: Get the input TOP.
    const OP_TOPInput* top = inputs->getInputTOP(0);
    if (!top) {
        return;
    }

    // E2: Load the model on the first cook: the Model parameter if set, otherwise the bundled one. Tried only once.
    if (!myModel && !myLoadTried) {
        myLoadTried = true;  // set before we start trying

        char path[1024];
        const char* par = inputs->getParFilePath("Model");
        if (par && *par) {  // not null and first character not empty
            snprintf(path, sizeof(path), "%s", par);
        } else if (depth_bundled_model_path(path, sizeof(path)) != 0) {
            myError = "No bundled model found";  // getErrorString()
            return;
        }

        char err[512];
        myModel = depth_create(path, err, sizeof(err));
        if (!myModel) {
            myError = err;
            return;
        }

        depth_output_size(myModel, &myOutW, &myOutH);
        myDepth.resize(myOutW * myOutH);  // allocate the vector myDepth
    }

    if (!myModel)
        return;

    // E3: Start the GPU->CPU download as top-down sRGB BGRA, what depth_run expects.
    OP_TOPInputDownloadOptions opts;
    opts.pixelFormat = OP_PixelFormat::BGRA8Fixed;
    opts.colorSpace = OP_ColorSpace::SRGB;  // the model was trained on sRGB photos, whatever the project's color space
    opts.verticalFlip = true;  // NOTE: TD's textures are bottom up - model wants row 0 = top

    OP_SmartRef<OP_TOPDownloadResult> res = top->downloadTexture(opts, nullptr);  // copies the input image from the GPU to CPU so depth_run can read it
    // E4: Wait for the download (getData) and run the model, timing both for the Info CHOP.
    //     Nonzero rc = failure; the codes are listed above depth_run in DepthModel.mm.
    int w = res->textureDesc.width, h = res->textureDesc.height;
    size_t stride = res->size / h;  // bytes per row | NOTE: this is because GPUs and drivers sometimes add even amounts of padding at the end of each row

    auto t0 = std::chrono::steady_clock::now();
    const uint8_t* pixels = static_cast<const uint8_t*>(res->getData());
    myDownloadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    t0 = std::chrono::steady_clock::now();
    int rc = depth_run(myModel, pixels, w, h, stride, myDepth.data());
    myRunMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rc) {  // failure
        myError = "depth_run failed (" + std::to_string(rc) + ")";
        return;
    }
    myError.clear();  // frame computed so no error message
    ++myFrames;

    // E5: Copy into an output buffer, min/max-stretched to 0..1 when Normalize is on (1 = nearest).
    OP_SmartRef<TOP_Buffer> buf = myContext->createOutputBuffer(myOutW * myOutH * sizeof(float), TOP_BufferFlags::None, nullptr);
    float* px = static_cast<float*>(buf->data);
    if (inputs->getParInt("Normalize")) {
        auto mm = std::minmax_element(myDepth.begin(), myDepth.end());
        float lo = *mm.first, hi = *mm.second;
        float range = hi - lo;
        for (size_t i = 0; i < myDepth.size(); ++i) {
            px[i] = (range > 0) ? (myDepth[i] - lo) / range : 0.0f;
        }
    } else {
        std::copy(myDepth.begin(), myDepth.end(), px);
    }

    // E6: Upload. uploadBuffer takes ownership, so buf is empty afterwards.
    TOP_UploadInfo info;
    info.textureDesc.width = myOutW;
    info.textureDesc.height = myOutH;
    info.textureDesc.texDim = OP_TexDim::e2D;
    info.textureDesc.pixelFormat = OP_PixelFormat::Mono32Float;
    info.firstPixel = TOP_FirstPixel::TopLeft;     // our rows are top-down (E3); TD flips them back
    info.colorSpace = OP_ColorSpace::Passthrough;  // depth is data, not color: never convert it
    output->uploadBuffer(&buf, info, nullptr);

    // Threaded version (TIP W1-W5 below): keep E1 and E3 here, but hand `res` to the worker instead of calling
    // getData(), and upload whatever the worker finished last:
    //             BufferInfo done = myFrameQueue.getBufferToUpload();
    //             if (done.buf) output->uploadBuffer(&done.buf, done.uploadInfo, nullptr);
    //         When nothing new is ready, skip the upload, as the sample's threading example does.
}

// ---- Worker thread (threaded version) ----
// TIP W1: Add `void workerLoop();` to the class and start it once, on the first cook:
//             if (!myWorker.joinable()) { myModelPath = path; myWorker = std::thread([this] { workerLoop(); }); }
//         Resolve `path` in execute() first (E2's path logic): the worker can't read parameters, since `inputs` only
//         exists during execute(). Load the model at the top of workerLoop (the rest of E2 moves here), so the
//         ~10 s load never blocks TD. Store load errors in myError (under myLock), and use getWarningString to show
//         "Loading model..." meanwhile (R2).
// TIP W2: execute() hands frames over: lock myLock, myPending = std::move(res), unlock, myWake.notify_one().
//         Overwriting a frame the worker hasn't started yet is intended: it should always take the newest frame.
// TIP W3: workerLoop: wait on myWake until myPending or myExit is set; on myExit, return. Move myPending into a local
//         while holding the lock, then unlock before the slow part: getData() (thread-safe; may wait for the GPU),
//         depth_run (E4), normalize (E5).
// TIP W4: Normalize into myFrameQueue.getBufferToUpdate(myOutW * myOutH * sizeof(float), TOP_BufferFlags::None) instead
//         of createOutputBuffer (skip the frame if it returns nullptr), then hand it over:
//             BufferInfo done;  done.buf = buf;  done.uploadInfo = info;   // info built as in E6
//             myFrameQueue.updateComplete(done);
// TIP W5: Anything both threads touch (myError, myRunMs, the model pointer) needs myLock or a std::atomic. A data
//         race here shows up as rare, random crashes in TD, which are hard to trace back.

int32_t DepthAnythingNativeTOP::getNumInfoCHOPChans(void* reserved1) {
    // Channels for an Info CHOP pointed at this TOP; getInfoCHOPChan is called once per index.
    return 4;
}

void DepthAnythingNativeTOP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1) {
    switch (index) {
        case 0:
            chan->name->setString("run_ms");
            chan->value = (float)myRunMs;
            break;
        case 1:
            chan->name->setString("download_ms");
            chan->value = (float)myDownloadMs;
            break;
        case 2:
            chan->name->setString("loaded");
            chan->value = (myModel != nullptr) ? 1.f : 0.f;
            break;
        case 3:
            chan->name->setString("frames");
            chan->value = (float)myFrames;
            break;
    }
}

bool DepthAnythingNativeTOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1) {
    return false;  // no Info DAT table
}

void DepthAnythingNativeTOP::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries,
                                               void* reserved1) {}

void DepthAnythingNativeTOP::getWarningString(OP_String* warning, void* reserved1) {
    // TIP R2: Threaded version: a yellow warning that doesn't stop the node while the worker is in depth_create (W1):
    //         warning->setString("Loading model...");
}

void DepthAnythingNativeTOP::getErrorString(OP_String* error, void* reserved1) {
    // TD shows it on the node in red until this stops setting a string. Lock first if the worker writes it.
    if (!myError.empty()) {
        error->setString(myError.c_str());
    }
}

void DepthAnythingNativeTOP::setupParameters(OP_ParameterManager* manager, void* reserved1) {
    // Parameter names follow the opType rule: a capital first letter, then only lower case letters or digits.

    // Model override (empty = bundled model), read in E2.
    OP_StringParameter sp;
    sp.name = "Model";
    sp.label = "Model (.mlmodelc)";
    sp.defaultValue = "";
    OP_ParAppendResult res = manager->appendFolder(sp);  // .mlmodelc is a folder, not a file
    assert(res == OP_ParAppendResult::Success);

    // Raw/normalized switch for E5.
    OP_NumericParameter np;
    np.name = "Normalize";
    np.label = "Normalize";
    np.defaultValues[0] = 1;
    res = manager->appendToggle(np);
    assert(res == OP_ParAppendResult::Success);

    // Reload pulse, handled in pulsePressed().
    OP_NumericParameter reload;
    reload.name = "Reload";
    reload.label = "Reload";
    res = manager->appendPulse(reload);
    assert(res == OP_ParAppendResult::Success);
}

void DepthAnythingNativeTOP::pulsePressed(const char* name, void* reserved1) {
    // TIP P4: Reload (not implemented yet): destroy the model and clear myError and myLoadTried so the next cook loads
    //         it again. With a worker running, signal it to reload rather than destroying the model under it (W5).
}
