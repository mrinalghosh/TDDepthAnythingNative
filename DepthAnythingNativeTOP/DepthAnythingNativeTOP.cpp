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
// Plan of attack:
//   1. Synchronous first: do E1-E6 all inside execute(). Simple to debug, but TD's frame waits on the model, so
//      expect roughly 30-40 fps at best.
//   2. Then threaded: keep E1/E3/E6 in execute() and move E4/E5 to a worker (TIP W1-W5). TD runs at full rate and
//      the depth map updates as fast as the model can go. ../CPUMemoryTOP/CPUMemoryTOP.cpp (THREADING_EXAMPLE) does
//      the same FrameQueue hand-off with a test pattern; it's worth reading first.
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
    // TIP C1: Don't load the model here. depth_create takes ~10 s every time TD starts (Core ML prepares the model
    //         for the Neural Engine on each new process), and TD's UI is frozen for as long as the constructor runs.
    //         Synchronous version: load lazily on the first cook (E2). Threaded version: start the worker on the
    //         first cook too (W1), since only execute() can read the Model parameter, and load it in the worker.
}

DepthAnythingNativeTOP::~DepthAnythingNativeTOP() {
    // TIP D1: Threaded version: stop the worker first. Set myExit = true while holding myLock, myWake.notify_one(),
    //         then join it, but only if (myWorker.joinable()): the worker starts on the first cook, so a TOP that
    //         never cooked has none, and join() on an empty std::thread throws (= TD crashes).
    //         It may be in the middle of depth_run, so the model must outlive it.
    // TIP D2: Then depth_destroy(myModel); it's fine to call with nullptr.
    depth_destroy(myModel);
    // TIP D3: OP_SmartRef members (download results, buffers) release themselves; nothing to do for those.
}

void DepthAnythingNativeTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1) {
    // Cook every frame while something downstream is looking at this TOP, so new depth frames show up even if the
    // input itself didn't change (the threaded version finishes frames asynchronously).
    ginfo->cookEveryFrameIfAsked = true;
}

void DepthAnythingNativeTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void* reserved1) {
    // TIP E1: Get the input: const OP_TOPInput* top = inputs->getInputTOP(0); return early if it's nullptr.
    const OP_TOPInput* top = inputs->getInputTOP(0);
    if (!top) {
        return;
    }

    // TIP E2: Make sure the model is loaded (synchronous version; the worker does this in the threaded one).
    //         Only try once: set myLoadTried = true first, so a failing load isn't repeated every cook.

    if (!myModel && !myLoadTried) {
        myLoadTried = true;  // set before we start trying

        //  Pick the path: the Model parameter if it's set (P1), otherwise the bundled model:
        //      char path[1024];
        //      if (depth_bundled_model_path(path, sizeof path) != 0) { myError = "No bundled model"; return; }
        //  then load it:
        //      char err[512];
        //      myModel = depth_create(path, err, sizeof err);
        //  On nullptr, myError = err and return; the node shows it via getErrorString() (R1).
        //  After loading: depth_output_size(myModel, &myOutW, &myOutH) and myDepth.resize(myOutW * myOutH).
        //  (E4-E6 below write outW/outH for short; they mean myOutW/myOutH.)

        char path[1024];
        const char* par = inputs->getParFilePath("Model");
        if (par && *par) {  // not null and first character not empty
            snprintf(path, sizeof(path), par);
        }

        if (depth_bundled_model_path(path, sizeof(path)) != 0) {
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

    // TIP E3: Download the input as top-down BGRA, exactly what depth_run expects:
    //             OP_TOPInputDownloadOptions opts;
    //             opts.pixelFormat  = OP_PixelFormat::BGRA8Fixed;
    //             opts.verticalFlip = true;   // TD's textures are bottom-up; the model wants row 0 = top
    //             opts.colorSpace   = OP_ColorSpace::SRGB;   // the model was trained on ordinary sRGB photos
    //             OP_SmartRef<OP_TOPDownloadResult> res = top->downloadTexture(opts, nullptr);
    //         (Without colorSpace, the pixels follow the project's Working Color Space; in ACEScg, say, the model
    //         would get linear values and the depth gets subtly worse.)
    //         res->getData() blocks until the GPU->CPU copy lands. Either accept that stall for a first version, or do
    //         what the CPUMemoryTOP sample does: keep this frame's result in myPrevDownload and process last frame's
    //         instead (one frame of latency, no stall). Watch for an empty myPrevDownload on the first cook.
    OP_TOPInputDownloadOptions opts;
    opts.pixelFormat = OP_PixelFormat::BGRA8Fixed;
    opts.colorSpace = OP_ColorSpace::SRGB;
    opts.verticalFlip = true;  // NOTE: TD's textures are bottom up - model wants row 0 = top

    OP_SmartRef<OP_TOPDownloadResult> res = top->downloadTexture(opts, nullptr);  // copies the input image from the GPU to CPU so depth_run can read it
    // TIP E4: Run the model:
    //             int w = res->textureDesc.width, h = res->textureDesc.height;
    //             size_t stride = res->size / h;          // bytes per row; don't assume w * 4
    //             int rc = depth_run(myModel, (const uint8_t*)res->getData(), w, h, stride, myDepth.data());
    //         Nonzero rc = failure (codes are listed above depth_run in DepthModel.mm). Time it with
    //         std::chrono::steady_clock into myRunMs for the Info CHOP (I1).
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

    // TIP E5: Normalize into an output buffer:
    //             OP_SmartRef<TOP_Buffer> buf =
    //                 myContext->createOutputBuffer(outW * outH * sizeof(float), TOP_BufferFlags::None, nullptr);
    //             float* px = (float*)buf->data;
    //         The model already outputs ~0..1 with 1 = nearest point, but the far end rarely reaches 0, so a min/max
    //         stretch (std::minmax_element; guard max == min) gives a fuller range. Per-frame min/max makes the
    //         image "breathe" when objects enter or leave; smoothing min and max over time (e.g.
    //         m = m * 0.9 + frameMin * 0.1) calms it down. A toggle for raw output (P2) is handy for comparing.

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

    // TIP E6: Upload:
    //             TOP_UploadInfo info;
    //             info.textureDesc.width       = outW;
    //             info.textureDesc.height      = outH;
    //             info.textureDesc.texDim      = OP_TexDim::e2D;
    //             info.textureDesc.pixelFormat = OP_PixelFormat::Mono32Float;
    //             info.firstPixel = TOP_FirstPixel::TopLeft;   // our rows are top-down (E3); TD flips them back
    //             info.colorSpace = OP_ColorSpace::Passthrough; // depth is data, not color: never convert it
    //             output->uploadBuffer(&buf, info, nullptr);   // takes ownership: buf is empty afterwards
    //         Output is 518x392 whatever the input size; resample it downstream (Fit or Resolution TOP) to match.
    //         Mono32Float keeps full precision for displacement; Mono16Float halves the upload if that's enough.
    TOP_UploadInfo info;
    info.textureDesc.width = myOutW;
    info.textureDesc.height = myOutH;
    info.textureDesc.texDim = OP_TexDim::e2D;
    info.textureDesc.pixelFormat = OP_PixelFormat::Mono32Float;
    info.firstPixel = TOP_FirstPixel::TopLeft;
    info.colorSpace = OP_ColorSpace::Passthrough;
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
// TIP W4: Normalize into myFrameQueue.getBufferToUpdate(outW * outH * sizeof(float), TOP_BufferFlags::None) instead
//         of createOutputBuffer (skip the frame if it returns nullptr), then hand it over:
//             BufferInfo done;  done.buf = buf;  done.uploadInfo = info;   // info built as in E6
//             myFrameQueue.updateComplete(done);
// TIP W5: Anything both threads touch (myError, myRunMs, the model pointer) needs myLock or a std::atomic. A data
//         race here shows up as rare, random crashes in TD, which are hard to trace back.

int32_t DepthAnythingNativeTOP::getNumInfoCHOPChans(void* reserved1) {
    // TIP I1: Channels for an Info CHOP pointed at this TOP, e.g. run_ms (myRunMs), frames processed, frames the
    //         worker skipped. Return how many you have; getInfoCHOPChan is then called once per index.
    return 4;
}

void DepthAnythingNativeTOP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1) {
    // TIP I2: if (index == 0) { chan->name->setString("run_ms"); chan->value = (float)myRunMs; }
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
    // TIP I3: For an Info DAT table (e.g. model path, input size, output size): set infoSize->rows / cols,
    //         byColumn = false, return true, and fill rows in getInfoDATEntries. false = no table.
    return false;
}

void DepthAnythingNativeTOP::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries,
                                               void* reserved1) {
    // TIP I4: Row `index`: entries->values[0]->setString("model"); entries->values[1]->setString(path);
}

void DepthAnythingNativeTOP::getWarningString(OP_String* warning, void* reserved1) {
    // TIP R2: A yellow warning that doesn't stop the node, e.g. "Loading model..." while the worker is in
    //         depth_create: warning->setString("Loading model...");
}

void DepthAnythingNativeTOP::getErrorString(OP_String* error, void* reserved1) {
    // TD shows it on the node in red until this stops setting a string. Lock first if the worker writes it.
    if (!myError.empty()) {
        error->setString(myError.c_str());
    }
}

void DepthAnythingNativeTOP::setupParameters(OP_ParameterManager* manager, void* reserved1) {
    // Parameter names follow the opType rule: a capital first letter, then only lower case letters or digits.
    // Setting np.page / sp.page = "Depth" puts them on their own page.
    //
    // TIP P1: A model override, to try other .mlmodelc builds without rebuilding the plugin:
    //             OP_StringParameter sp;
    //             sp.name = "Model";  sp.label = "Model (.mlmodelc)";  sp.defaultValue = "";
    //             OP_ParAppendResult res = manager->appendFolder(sp);   // .mlmodelc is a folder, not a file
    //             assert(res == OP_ParAppendResult::Success);
    //         Read it with inputs->getParFilePath("Model"): an absolute path when it exists. Empty = bundled model.
    OP_StringParameter sp;
    sp.name = "Model";
    sp.label = "Model (.mlmodelc)";
    sp.defaultValue = "";
    OP_ParAppendResult res = manager->appendFolder(sp);
    assert(res == OP_ParAppendResult::Success);

    // TIP P2: A raw/normalized switch for E5:
    //             OP_NumericParameter np;  np.name = "Normalize";  np.label = "Normalize";  np.defaultValues[0] = 1;
    //             manager->appendToggle(np);          // read with inputs->getParInt("Normalize")
    OP_NumericParameter np;
    np.name = "Normalize";
    np.label = "Normalize";
    np.defaultValues[0] = 1;
    res = manager->appendToggle(np);
    assert(res == OP_ParAppendResult::Success);

    // TIP P3: A "Reload" pulse (manager->appendPulse(np)), handled in pulsePressed(): destroy the model and clear
    //         myError so the next cook loads it again, e.g. after changing Model.
    OP_NumericParameter reload;
    reload.name = "Reload";
    reload.label = "Reload";
    res = manager->appendPulse(reload);
    assert(res == OP_ParAppendResult::Success);
}

void DepthAnythingNativeTOP::pulsePressed(const char* name, void* reserved1) {
    // TIP P4: if (!strcmp(name, "Reload")) { ... }   With a worker running, signal it to reload rather than
    //         destroying the model under it (W5).
}
