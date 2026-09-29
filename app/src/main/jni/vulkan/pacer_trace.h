#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "frame_pacer.h"

namespace vkr {

// Records what the frame pacer sees and decides, so a session can be examined afterwards and
// replayed through the pacer on a PC (tests/pacer_replay.cpp). Also logs a summary to logcat
// every 10 seconds, whether or not a trace file is being written.
//
// Traces are written when the debug.moonlight.pacer_trace system property is 1, which adb can
// set: adb shell setprop debug.moonlight.pacer_trace 1 (see tools/pacer-trace.sh).
//
// The file is CSV, one event per line, in the order the pacer saw them:
//   C,key=value,...                  session configuration
//   R,periodNs                       vsync period handed to FramePacer::setVsyncPeriod()
//   F,arrivalNs,hostPtsNs,targetNs,queued,queueDrops
//                                    frame arrived (FramePacer::onFrameArrived())
//   V,vsyncNs,queued,choice,shownPtsNs,locked,slotVsyncs,slotParity,scheduleDelayNs,
//     transitBufferNs,periodNs,driftPpm,callbackLateNs,presentId,presentDelayVsyncs
//                                    vsync (FramePacer::onVsync()), choice -1 when nothing new
//                                    was shown; callbackLateNs is how long after the vsync the
//                                    callback ran, presentId identifies the present (0 if none)
//                                    and presentDelayVsyncs is the vsync after this one it asked
//                                    to reach the screen at (PresentScheduler)
//   I,nowNs                          frame shown on arrival (lowest latency mode, mailbox)
//   A,nowNs,showVsyncNs,hostPtsNs,presentId,presentDelayVsyncs,presentGuardNs
//                                    frame presented as it arrived, asking for the vsync
//                                    presentDelayVsyncs after showVsyncNs (host frame timing
//                                    with display timing); the V lines' choice doesn't include it.
//                                    presentGuardNs is the pacer's present guard at the time.
//   P,presentId,actualNs,earliestNs,marginNs
//                                    when a present actually reached the screen
//                                    (VK_GOOGLE_display_timing), reported a few frames later
//   Q,presentId,startNs,fencedNs,acquiredNs,queuedNs
//                                    rendering a present: started, done waiting for the GPU,
//                                    got a swapchain image, and handed to the compositor
//   N,hostPtsNs,receiveNs,enqueueNs  a frame's network timing from moonlight-common-c: when its
//                                    first packet arrived and when it was fully assembled
//                                    (CLOCK_MONOTONIC_RAW, so only compare within the column)
//   D,hostPtsNs,startNs,queuedNs,doneNs
//                                    a PyroWave frame's decode: handed to the renderer, queued on
//                                    the GPU, and finished there (then it arrives, as F)
//
// Every call must be made with the renderer's lock held.
class PacerTrace {
public:
    ~PacerTrace() { close(); }

    // Starts a trace file in `directory` if tracing is switched on
    void start(const std::string& directory, const std::string& configuration);
    void close();

    bool recording() const { return file_ != nullptr; }

    void period(int64_t periodNs);
    void frame(const FrameTiming& timing, size_t queued, uint64_t queueDrops);
    void vsync(int64_t vsyncNs, size_t queued, int choice, int64_t shownPtsNs, const FramePacer& pacer,
               int64_t callbackLateNs, uint64_t presentId, int presentDelayVsyncs);
    void immediate(int64_t nowNs);
    void ahead(int64_t nowNs, int64_t showVsyncNs, int64_t hostPtsNs, uint64_t presentId, int presentDelayVsyncs,
               const FramePacer& pacer);
    void presented(uint64_t presentId, int64_t actualNs, int64_t earliestNs, int64_t marginNs);
    // Rendering a present: started, done waiting for the GPU, got a swapchain image, presented
    void rendered(uint64_t presentId, int64_t startNs, int64_t fencedNs, int64_t acquiredNs, int64_t queuedNs);
    void received(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs);
    void decoded(int64_t hostPtsNs, int64_t startNs, int64_t queuedNs, int64_t doneNs);

private:
    void summarize(int64_t vsyncNs, const FramePacer& pacer);
    void countShown(int64_t index, int skipped, const FramePacer& pacer);
    void write(const char* format, ...) __attribute__((format(printf, 2, 3)));

    FILE* file_ = nullptr;
    uint64_t bytes_ = 0;
    int64_t lastFlushNs_ = 0;

    // Summary counters since the last summary
    int64_t windowStartNs_ = 0;
    uint32_t shown_ = 0;
    uint32_t heldShort_ = 0;
    uint32_t heldLong_ = 0;
    uint32_t skipped_ = 0;
    uint32_t lockedVsyncs_ = 0;
    uint32_t vsyncs_ = 0;
    uint64_t queueDropsAtStart_ = 0;
    uint64_t queueDrops_ = 0;
    int64_t lastShownIndex_ = -1;
    uint32_t lateCallbacks_ = 0;     // Callbacks that ran more than half a vsync late
    int64_t lastActualNs_ = 0;
    uint32_t screenShort_ = 0;       // As measured on screen, when display timing is available
    uint32_t screenLong_ = 0;
    uint32_t screenFrames_ = 0;
    int64_t screenSlotNs_ = 0;
    int presentDelay_ = 0;
    int64_t presentGuardNs_ = 0;
};

}  // namespace vkr
