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

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DepthModel.h"
#include "FrameQueue.h"
#include "TOP_CPlusPlusBase.h"
using namespace TD;

// Runs Depth Anything V2 (Core ML, see DepthModel.h) on the input TOP and outputs a mono float depth map.
// The plugin boilerplate is done; the numbered TIPs in DepthAnythingNativeTOP.cpp walk through the TOP itself.
class DepthAnythingNativeTOP : public TOP_CPlusPlusBase {
   public:
    DepthAnythingNativeTOP(const OP_NodeInfo* info, TOP_Context* context);
    virtual ~DepthAnythingNativeTOP();

    virtual void getGeneralInfo(TOP_GeneralInfo*, const OP_Inputs*, void* reserved1) override;
    virtual void execute(TOP_Output*, const OP_Inputs*, void* reserved1) override;

    virtual int32_t getNumInfoCHOPChans(void* reserved1) override;
    virtual void getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1) override;

    virtual bool getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1) override;
    virtual void getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries,
                                   void* reserved1) override;

    virtual void getWarningString(OP_String* warning, void* reserved1) override;
    virtual void getErrorString(OP_String* error, void* reserved1) override;

    virtual void setupParameters(OP_ParameterManager* manager, void* reserved1) override;
    virtual void pulsePressed(const char* name, void* reserved1) override;

   private:
    // Information about the node using this instance (its name, path, ...). Valid for the node's lifetime.
    const OP_NodeInfo* myNodeInfo;

    // Creates output buffers. Valid for the node's lifetime; createOutputBuffer() is safe from any thread.
    TOP_Context* myContext;

    // Hands finished output buffers from a worker thread to execute() (see FrameQueue.h). Unused until the
    // threaded version (TIP W1-W5).
    FrameQueue myFrameQueue;

    // TIP M1: Members you'll likely add as you go (none are needed to compile):
    DepthModel* myModel = nullptr;  // owned; depth_destroy it in the destructor
    bool myLoadTried = false;       // a failed load isn't retried every cook
    std::string myError;            //    returned by getErrorString(); empty = no error
    int myOutW = 0, myOutH = 0;     // from depth_output_size (518x392)
    std::vector<float> myDepth;     // myOutW * myOutH, filled by depth_run
    double myRunMs = 0;             //     last depth_run time, for the Info CHOP
    //   OP_SmartRef<OP_TOPDownloadResult>  myPrevDownload;     last frame's download (E3's no-stall option)
    // Threaded version adds:
    //   std::thread                        myWorker;
    //   std::mutex                         myLock;             guards everything shared with the worker
    //   std::condition_variable            myWake;             execute() -> worker: "new frame" or "exit"
    //   OP_SmartRef<OP_TOPDownloadResult>  myPending;          newest input frame waiting for the worker
    //   std::string                        myModelPath;        resolved in execute(), loaded by the worker
    //   bool                               myExit = false;
    //   void                               workerLoop();
};
