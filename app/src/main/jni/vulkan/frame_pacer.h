#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Decides which decoded frame to show at each display vsync.
//
// This file has no Android dependencies so the pacing logic can be tested on its own.

namespace vkr {

// Matches PreferenceConfiguration.FRAME_PACING_*
enum class PacingMode : int {
    MinLatency = 0,
    Balanced = 1,
    CapFps = 2,
    Smoothness = 3,
    HostTimed = 4,
};

struct FrameTiming {
    int64_t hostPtsNs = 0;   // Host present time, in the stream's timestamp epoch
    int64_t arrivalNs = 0;   // Local CLOCK_MONOTONIC time the decoded frame became available
    int64_t targetNs = 0;    // HostTimed: local time the frame is due, before any phase shift
};

// How much delay the host timeline adds to ride out variation in frame transit times.
// Matches PreferenceConfiguration.JITTER_BUFFER_*.
enum class JitterBuffer : int {
    LowLatency = 0,
    Balanced = 1,
    Smooth = 2,
    LowestLatency = 3,  // Less buffer than LowLatency, and never evens out the host's timing
};

// Maps host frame timestamps onto the local clock.
//
// Sunshine captures each frame as soon as the host desktop presents it and stamps it with
// that present time, so the spacing of host timestamps is the host display's real cadence.
// The time from a frame's host timestamp to its arrival here (transit) is then only encode,
// network and decode delay, and its variation is the jitter we have to buffer for. Showing a
// frame at hostPts + offset, where offset covers nearly all observed transit times, recreates
// the host's cadence with a constant delay.
class HostTimeline {
public:
    explicit HostTimeline(JitterBuffer jitterBuffer);

    void reset();

    // Records a decoded frame. Returns false if it broke the timeline (the stream restarted
    // or timestamps jumped) and the estimate was reset.
    bool addSample(int64_t hostPtsNs, int64_t arrivalNs);

    bool hasEstimate() const { return !window_.empty(); }

    // Local time minus host time that frames are scheduled at
    int64_t offsetNs() const { return offsetNs_; }

    // Smallest transit time currently in the window
    int64_t minTransitNs() const;

    // How long the scheduled time is after the smallest transit time
    int64_t bufferNs() const { return hasEstimate() ? offsetNs_ - minTransitNs() : 0; }

    JitterBuffer jitterBuffer() const { return jitterBuffer_; }

    // Largest buffer worth keeping. Frames wait in a queue of limited size for their time, so
    // beyond what it holds, a bigger buffer only pushes frames out of the queue unshown.
    void setMaxBufferNs(int64_t maxBufferNs) { maxBufferNs_ = maxBufferNs; }

    // The start of a stream, when its first frames arrive late (the first keyframe is large):
    // the buffer covers less of the transit times then, so those don't set it
    static constexpr int64_t kWarmupNs = 1'000'000'000;
    static constexpr double kWarmupCoverage = 0.9;

private:
    JitterBuffer jitterBuffer_;

    // Window of transit samples the offset is taken from
    int64_t windowNs_;

    // The offset covers this fraction of transit times. The rest arrive after their
    // scheduled time and are shown at the first vsync after they arrive.
    double coverage_;

    // When transit times come down, the offset follows by this fraction of the difference
    // per frame
    int64_t decayDivisor_;

    int64_t maxBufferNs_ = 0;
    int64_t startNs_ = 0;
    bool warmedUp_ = false;

    struct Sample {
        int64_t arrivalNs;
        int64_t transitNs;
    };

    std::deque<Sample> window_;
    std::deque<Sample> minQueue_;  // Increasing transit times, for the window minimum
    std::vector<int64_t> scratch_;
    int64_t offsetNs_ = 0;
    int64_t lastPtsNs_ = 0;
};

class FramePacer {
public:
    // Upper bound on maxQueued() in any mode
    static constexpr size_t kMaxQueuedFrames = 6;

    FramePacer(PacingMode mode, int streamFps, int64_t vsyncPeriodNs, JitterBuffer jitterBuffer);

    PacingMode mode() const { return mode_; }

    void setVsyncPeriod(int64_t periodNs);
    int64_t vsyncPeriodNs() const { return periodNs_; }

    // Most frames that may wait to be shown. The caller drops the oldest beyond this.
    size_t maxQueued() const;

    // Called for each decoded frame as it arrives. Fills in frame.targetNs.
    void onFrameArrived(FrameTiming& frame);

    // Called at each vsync with the waiting frames, oldest first. Returns the index of the
    // frame to show (the caller drops the ones before it), or -1 to keep the current one.
    int onVsync(int64_t vsyncNs, const FrameTiming* frames, size_t count);

    // The caller showed a frame outside onVsync (lowest latency mode presenting on arrival)
    void onPresentedImmediately(int64_t nowNs);

    // HostTimed: the vsync a waiting frame is due to be shown at, so the caller can present it
    // right away and ask for that vsync, rather than waiting for the vsync to come around.
    // Returns 0 if the frame can't be committed yet: frames are shown in the order they're
    // presented, so the vsync must be later than the last one a frame was committed to, and it
    // must be within kMaxPresentAheadVsyncs of the next vsync. Every frame presented ahead holds
    // a swapchain image until it's on screen; with too many, the render thread blocks waiting
    // for a free one, which makes it late for vsyncs and for frames arriving.
    int64_t plannedVsyncNs(const FrameTiming& frame) const;
    static constexpr int64_t kMaxPresentAheadVsyncs = 1;

    // A frame was presented ahead of time for vsyncNs (from plannedVsyncNs())
    void onPresentedAhead(int64_t vsyncNs);

    // The caller presents frames ahead (plannedVsyncNs()), so onVsync() only gets frames that
    // couldn't be: late ones. It then shows the newest due frame and nothing early; its rules
    // for easing frames into place would take frames that are about to be presented ahead
    // with time to spare and present them with none.
    void setPresentAhead(bool presentAhead) { presentAhead_ = presentAhead; }

    // With frames presented ahead, how long before its vsync a frame must be due for that vsync
    // (PresentScheduler::guardNs()). Frames due later go to the next vsync.
    void setPresentGuardNs(int64_t guardNs) { presentGuardNs_ = guardNs; }
    int64_t presentGuardNs() const { return presentAhead_ ? presentGuardNs_ : 0; }

    // The vsync count at a vsync time near the last one seen
    int64_t vsyncIndexAt(int64_t vsyncNs) const {
        return vsyncIndex_ + (vsyncNs - lastVsyncNs_ + (vsyncNs >= lastVsyncNs_ ? periodNs_ / 2 : -periodNs_ / 2)) / periodNs_;
    }

    // HostTimed: extra delay added to line frames up with the vsyncs they're shown at
    int64_t phaseShiftNs() const { return shiftNs_; }
    bool phaseLocked() const { return phaseLocked_; }
    // Vsyncs each frame is held for, from the spacing of host timestamps
    int64_t slotVsyncs() const { return slotVsyncs_; }
    int64_t slotParity() const { return slotParity_; }
    // Vsyncs counted so far, including ones whose callbacks were missed
    int64_t vsyncIndex() const { return vsyncIndex_; }
    const HostTimeline& timeline() const { return timeline_; }

    // Frames the pacer chose not to show
    uint64_t framesSkipped() const { return framesSkipped_; }

    // How far the host's frames drift against our vsyncs, in slots per frame
    double phaseDriftPerFrame() const { return driftPerFrame_; }

    // Delay added to frames' host timestamps beyond the transit buffer when locked: the time
    // after the fitted line they're scheduled at, plus the vsync alignment
    int64_t scheduleDelayNs() const { return shiftNs_ + static_cast<int64_t>(lateNs_); }

    // Window the frame schedule is fitted over
    static constexpr int64_t kPhaseWindowNs = 16'000'000'000;
    // What's kept of it when the frames stop fitting a line: enough to see a 71.82 fps game's
    // timestamps step by a 143.64 Hz host refresh (every 2.8 s) before locking again
    static constexpr int64_t kRelockWindowNs = 4'000'000'000;

private:
    void trackVsync(int64_t vsyncNs);
    void updateSlot(const FrameTiming& frame);
    void updatePhase(FrameTiming& frame);
    void resetPhase();
    int64_t slotPeriodNs() const { return slotVsyncs_ * periodNs_; }

    PacingMode mode_;
    int64_t streamIntervalNs_;

    // The vsync period, measured from vsync times once there are enough of them. The phase
    // lock needs it exact: Android may report 144 Hz for a panel that runs at 143.64 Hz.
    int64_t periodNs_;
    struct VsyncSample {
        int64_t index;
        int64_t timeNs;
    };
    std::deque<VsyncSample> vsyncs_;

    int64_t lastVsyncNs_ = 0;
    int64_t vsyncIndex_ = 0;  // Vsyncs seen, counting ones whose callbacks we missed
    int64_t lastPresentVsyncNs_ = 0;
    uint64_t framesSkipped_ = 0;

    // HostTimed
    HostTimeline timeline_;
    int64_t shiftNs_ = 0;
    bool phaseLocked_ = false;
    bool presentAhead_ = false;
    int64_t presentGuardNs_ = 0;
    double driftPerFrame_ = 0;
    double lateNs_ = 0;  // How far after the fitted line frames are scheduled
    double scheduledPtsNs_ = 0;  // Host time the last frame was scheduled at
    int64_t scheduledIndex_ = 0;
    int64_t lockedSinceNs_ = 0;

    // Frames are shown every slotVsyncs_ vsyncs when the phase is locked, on the vsyncs whose
    // index is slotParity_ more than a multiple of slotVsyncs_
    int64_t slotVsyncs_ = 1;
    int64_t slotParity_ = 0;
    std::deque<int64_t> ptsDeltas_;
    int64_t lastPtsNs_ = 0;

    // Recent frames the schedule is fitted to, over kPhaseWindowNs
    struct PhaseSample {
        int64_t timeNs;
        int64_t frameIndex;  // Slots since the first sample, counting frames the host skipped
        int64_t hostPtsNs;
    };
    std::deque<PhaseSample> phaseSamples_;
    int64_t frameIndex_ = 0;
};

}  // namespace vkr
