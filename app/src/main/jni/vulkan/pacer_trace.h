#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "frame_pacer.h"
#include "pyrowave_decoder.h"

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
//   J,jitterBuffer                   the jitter buffer preset changed mid-stream
//                                    (FramePacer::setJitterBuffer()), from the one in C
//   S,nowNs,format,colorSpace,hdr,width,height,presentMode
//                                    a swapchain was created: its VkFormat, VkColorSpaceKHR and
//                                    VkPresentModeKHR, and whether the stream was HDR (1) at the
//                                    time. HDR comes and goes mid-stream, each time with a new one.
//   F,arrivalNs,hostPtsNs,targetNs,queued,queueDrops
//                                    frame arrived (FramePacer::onFrameArrived())
//   V,vsyncNs,queued,choice,shownPtsNs,locked,slotVsyncs,slotParity,scheduleDelayNs,
//     transitBufferNs,periodNs,driftPpm,callbackLateNs,presentId,presentDelayVsyncs
//                                    vsync (FramePacer::onVsync()), choice -1 when nothing new
//                                    was shown; callbackLateNs is how long after the vsync the
//                                    callback ran, presentId identifies the present (0 if none)
//                                    and presentDelayVsyncs is the vsync after this one it asked
//                                    to reach the screen at (PresentScheduler)
//   I,nowNs,hostPtsNs,presentId,targetVsyncNs
//                                    frame shown on arrival (lowest latency mode, mailbox, or a
//                                    front buffer), and the present that identifies it in Q, G
//                                    and P lines (0 without display timing, as with a front
//                                    buffer). targetVsyncNs is the scan a front buffer aligned to
//                                    the scan drew it to finish by (0 otherwise). Traces from
//                                    before these were added have only nowNs, or no
//                                    targetVsyncNs.
//   W,nowNs,presentId                the frame on screen presented again to keep the compositor
//                                    awake while the host sends nothing (keep-alive), with the
//                                    present that identifies it in Q, G and P lines
//   A,nowNs,showVsyncNs,hostPtsNs,presentId,presentDelayVsyncs,presentGuardNs
//                                    frame presented as it arrived, asking for the vsync
//                                    presentDelayVsyncs after showVsyncNs (host frame timing
//                                    with display timing); the V lines' choice doesn't include it.
//                                    presentGuardNs is the pacer's present guard at the time.
//   P,presentId,actualNs,earliestNs,marginNs
//                                    when a present actually reached the screen
//                                    (VK_GOOGLE_display_timing), reported a few frames later
//   Q,presentId,startNs,fencedNs,acquiredNs,queuedNs,imageIndex,fenceWaited,releasedNs
//                                    rendering a present: started, done waiting for the GPU,
//                                    got a swapchain image, and handed to the compositor, and
//                                    which swapchain image it went to. fenceWaited is 1 if the
//                                    GPU wasn't done when asked, so the fence was waited on.
//                                    releasedNs is when the frame last rendered in the same slot
//                                    had been let go of (giving MediaCodec its image back), which
//                                    is between fencedNs and acquiredNs. (In traces from before
//                                    it was added, fencedNs came after letting go.)
//   G,presentId,gpuStartNs,gpuEndNs  when a present's rendering ran on the GPU, from timestamps
//                                    at the start and end of its commands: the start comes after
//                                    its swapchain image was free, so a late start is a wait for
//                                    the image and a long run is slow rendering. On the GPU's
//                                    clock, in ns: only compare within the record, or offset
//                                    it by when the CPU saw the same work finish (the next Q
//                                    using its fence). Written when that fence is next waited
//                                    on, so it comes after later Q lines. Then, if present,
//                                    acquiredNs,drawnNs: after taking MediaCodec's buffer from
//                                    the decoder (its layout change) and after drawing, so the
//                                    run splits into taking the buffer, drawing, and handing it
//                                    back. For PyroWave, which has neither, all three are close.
//   N,hostPtsNs,receiveNs,enqueueNs,queuedNs,commonToLocalNs,hostProcessingNs,rttMs
//                                    a frame's network timing from moonlight-common-c: when its
//                                    first packet arrived and when it was fully assembled, on
//                                    its clock (which starts with the process); then when it
//                                    went to the decoder on ours (so its decode is the F line's
//                                    arrivalNs less this), our clock less moonlight-common-c's
//                                    then (add to receiveNs/enqueueNs for ours), the host's own
//                                    capture-to-send time for it (0 if not reported), and the
//                                    estimated network round trip (0 if not known yet). Traces
//                                    from before these were added have only the first three.
//   D,hostPtsNs,startNs,queuedNs,doneNs,waitedNs,gpuStartNs,gpuEndNs,pushNs,recordNs,submitNs,
//     pyrowaveSubmitNs,pyrowaveSubmits
//                                    a PyroWave frame's decode: handed to the renderer, queued on
//                                    the GPU, and finished there (then it arrives, as F). waitedNs
//                                    is how much of start to queued was waiting for the queue or
//                                    for our commands to be free. gpuStartNs and gpuEndNs are when
//                                    it ran on the GPU, on its clock as for G, or 0 if the GPU
//                                    can't time it; gpuStartNs - queuedNs isn't meaningful, but a
//                                    late start against the G lines is a wait behind rendering.
//                                    A decode's start can come before the end of the one before,
//                                    which it overlaps.
//                                    pushNs, recordNs and submitNs split up the CPU's share, as
//                                    PyrowaveDecoder::DecodeTimes does; what's left of start to
//                                    queued is the renderer's and taking planes. Of recordNs,
//                                    PyroWave held the queue for pyrowaveSubmitNs, over
//                                    pyrowaveSubmits times. Traces from before these were added
//                                    have only the first four, seven or ten.
//   K,hostPtsNs,kind                 a partial PyroWave frame handed to the renderer: 1 it lost
//                                    packets, 2 it was cut short at its deadline
//   L,hostPtsNs,readyByNs,readyCostNs,commonToLocalNs,offsetUs
//                                    the deadline for cutting frames short, worked out from the
//                                    frame that just arrived (F): when it had to be ready by, the
//                                    percentile of ready costs, CLOCK_MONOTONIC minus
//                                    moonlight-common-c's clock, and the offset after a frame's
//                                    host timestamp handed to moonlight-common-c (on its clock)
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
    void jitterBuffer(JitterBuffer jitterBuffer);
    void swapchain(int64_t nowNs, int format, int colorSpace, bool hdr, uint32_t width, uint32_t height,
                   int presentMode);
    void frame(const FrameTiming& timing, size_t queued, uint64_t queueDrops);
    void vsync(int64_t vsyncNs, size_t queued, int choice, int64_t shownPtsNs, const FramePacer& pacer,
               int64_t callbackLateNs, uint64_t presentId, int presentDelayVsyncs);
    void immediate(int64_t nowNs, int64_t hostPtsNs, uint64_t presentId, int64_t targetVsyncNs);
    void keepAlive(int64_t nowNs, uint64_t presentId);
    void ahead(int64_t nowNs, int64_t showVsyncNs, int64_t hostPtsNs, uint64_t presentId, int presentDelayVsyncs,
               const FramePacer& pacer);
    void presented(uint64_t presentId, int64_t actualNs, int64_t earliestNs, int64_t marginNs);
    // Rendering a present: started, done waiting for the GPU, got a swapchain image, presented
    void rendered(uint64_t presentId, int64_t startNs, int64_t fencedNs, int64_t acquiredNs, int64_t queuedNs,
                  uint32_t imageIndex, bool fenceWaited, int64_t releasedNs);
    // When a present's rendering ran on the GPU, on the GPU's clock
    void gpuTimes(uint64_t presentId, int64_t gpuStartNs, int64_t gpuEndNs, int64_t acquiredNs, int64_t drawnNs);
    void received(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs, int64_t queuedNs, int64_t commonToLocalNs,
                  int64_t hostProcessingNs, uint32_t rttMs);
    void decoded(int64_t hostPtsNs, int64_t startNs, int64_t queuedNs, int64_t doneNs,
                 const PyrowaveDecoder::DecodeTimes& times, int64_t gpuStartNs, int64_t gpuEndNs);
    void partial(int64_t hostPtsNs, int kind);
    void partialDeadline(int64_t hostPtsNs, int64_t readyByNs, int64_t readyCostNs, int64_t commonToLocalNs,
                         int64_t offsetUs);

private:
    void summarize(int64_t vsyncNs, const FramePacer& pacer);
    void countShown(int64_t index, int skipped, const FramePacer& pacer);
    void write(const char* format, ...) __attribute__((format(printf, 2, 3)));
    // Writes out what's buffered if it's been kFlushIntervalNs since the last time
    void flushPeriodically(int64_t nowNs);

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
