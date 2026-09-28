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

    // Window of transit samples the offset is taken from
    static constexpr int64_t kWindowNs = 2'000'000'000;

    // The offset covers this fraction of transit times. The rest arrive after their
    // scheduled time and are shown at the first vsync after they arrive.
    static constexpr double kCoverage = 0.95;

private:
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

    FramePacer(PacingMode mode, int streamFps, int64_t vsyncPeriodNs);

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
