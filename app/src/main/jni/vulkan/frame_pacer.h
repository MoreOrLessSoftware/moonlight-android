#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

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
    int64_t targetNs = 0;    // HostTimed: the vsync the frame is assigned to
};

// Matches PreferenceConfiguration.JITTER_BUFFER_*. HostTimed pacing currently behaves the same
// for all of them (Low latency); the setting is kept so it can be tuned per preset later.
enum class JitterBuffer : int {
    LowLatency = 0,
    Balanced = 1,
    Smooth = 2,
    LowestLatency = 3,
    Instant = 4,
};

// HostTimed pacing puts host frames on our vsyncs, one slot apart (a slot is the whole number of
// vsyncs between host frames: one when the host's frame rate is matched to our refresh rate),
// with as little delay as the frames' arrival allows:
//
// - Each frame is assigned the vsync after the previous frame's, plus as many slots as its host
//   timestamp advanced. Frames keep the host's cadence on our vsyncs with no clock mapping.
// - A frame that can't make its vsync goes to the first one it can, and the frames after it
//   follow from there: the delay grows by a vsync only when a frame actually needs it.
// - When every frame for kPullHoldNs could have made the vsync before its own with a margin to
//   spare, frames move a vsync sooner (one frame replaced by the next).
// - No frame waits kMaxWaitVsyncs or more after it could first be shown; frames that would are
//   moved sooner at once.
//
// A host running slightly slow against our display shows as an occasional late frame (one frame
// held a vsync longer), one slightly fast as an occasional pull (one frame skipped): the least
// the rate difference requires.
class FramePacer {
public:
    // Upper bound on maxQueued() in any mode
    static constexpr size_t kMaxQueuedFrames = 6;

    // A frame waits less than this many vsyncs from when it could first be shown (its arrival
    // plus the present guard) to the vsync it's assigned
    static constexpr int64_t kMaxWaitVsyncs = 2;
    // How long every frame must have had a vsync to spare before frames move a vsync sooner.
    // Replayed through a 72 fps HEVC session on a 144 Hz iPlay 70 mini Ultra over Wi-Fi, where
    // a lone frame arrived 5-10 ms late every few seconds, 3 s rather than 1 s cut the uneven
    // frames by about a quarter for about 1 ms more median wait; longer saved little more.
    static constexpr int64_t kPullHoldNs = 3'000'000'000;
    // ...plus this fraction of a vsync, so that frames don't go straight back
    static constexpr int64_t kPullMarginDivisor = 4;
    // A host timestamp further from the last than this starts the schedule over
    static constexpr int64_t kMaxPtsGapNs = 250'000'000;

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
    // Returns 0 if the frame can't be committed yet: it must be within presentAheadVsyncs() of
    // the last vsync, and not before the last vsync a frame was committed to. The same vsync
    // replaces that frame.
    int64_t plannedVsyncNs(const FrameTiming& frame) const { return frameVsyncNs(frame, true); }

    // How many vsyncs ahead frames may be committed: one slot
    int64_t presentAheadVsyncs() const { return std::clamp<int64_t>(slotVsyncs_, 1, kMaxPresentAheadVsyncs); }
    static constexpr int64_t kMaxPresentAheadVsyncs = 4;

    // A frame was presented ahead of time for vsyncNs (from plannedVsyncNs())
    void onPresentedAhead(int64_t vsyncNs);

    // The caller presents frames ahead (plannedVsyncNs()), so onVsync() only gets frames that
    // couldn't be
    void setPresentAhead(bool presentAhead) { presentAhead_ = presentAhead; }

    void setJitterBuffer(JitterBuffer jitterBuffer) { jitterBuffer_ = jitterBuffer; }
    JitterBuffer jitterBuffer() const { return jitterBuffer_; }

    // With frames presented ahead, how long before its vsync a frame must be ready for that
    // vsync (PresentScheduler::guardNs())
    void setPresentGuardNs(int64_t guardNs) { presentGuardNs_ = guardNs; }
    int64_t presentGuardNs() const { return presentAhead_ ? presentGuardNs_ : 0; }

    // The vsync count at a vsync time near the last one seen
    int64_t vsyncIndexAt(int64_t vsyncNs) const {
        return vsyncIndex_ + (vsyncNs - lastVsyncNs_ + (vsyncNs >= lastVsyncNs_ ? periodNs_ / 2 : -periodNs_ / 2)) / periodNs_;
    }
    // Vsyncs counted so far, including ones whose callbacks were missed
    int64_t vsyncIndex() const { return vsyncIndex_; }

    // HostTimed: frames are being assigned vsyncs (there's a schedule to follow)
    bool anchored() const { return anchored_; }
    // Vsyncs each frame is held for, from the spacing of host timestamps
    int64_t slotVsyncs() const { return slotVsyncs_; }
    // How long the newest frame waits from when it could first be shown to its vsync
    int64_t waitNs() const { return waitNs_; }

    // Frames the pacer chose not to show
    uint64_t framesSkipped() const { return framesSkipped_; }
    // Frames that couldn't make their vsync, moving the schedule a vsync or more later
    uint64_t lateFrames() const { return lateFrames_; }
    // Times the schedule moved sooner
    uint64_t pulls() const { return pulls_; }

    // Frames still arriving when they should be ready are cut short then, where the decoder can
    // show what arrived (PyroWave), rather than shown late (LiSetPartialFrameDeadline()).
    //
    // How long a frame took from its last packet to being ready to show (decoded)
    void addReadyCost(int64_t costNs);

    // HostTimed: how long after a frame's host timestamp its last packet must be in for it to be
    // ready in time for its vsync, judged from a frame that just arrived: the time it must be
    // ready by, less the 90th percentile of recent ready costs. False before frames are
    // scheduled, or before there are enough ready costs to go on.
    // readyByNs and readyCostNs, if given, get the time the frame must be ready by and the
    // percentile of ready costs, for the trace.
    bool partialDeadlineOffsetNs(const FrameTiming& frame, int64_t* offsetNs, int64_t* readyByNs = nullptr,
                                 int64_t* readyCostNs = nullptr) const;
    static constexpr int kPartialReadyPercentile = 90;
    static constexpr size_t kPartialReadySamples = 64;
    static constexpr size_t kPartialMinReadySamples = 16;

private:
    // plannedVsyncNs(), or with committable false, the vsync even if it's too far off to
    // commit the frame to yet
    int64_t frameVsyncNs(const FrameTiming& frame, bool committable) const;
    void trackVsync(int64_t vsyncNs);
    void updateSlot(int64_t hostPtsNs);
    // The vsync on our grid nearest to timeNs, and the first at or after it (and after the last
    // vsync seen)
    int64_t nearestVsyncNs(int64_t timeNs) const;
    int64_t firstVsyncFromNs(int64_t timeNs) const;

    PacingMode mode_;
    int64_t streamIntervalNs_;
    JitterBuffer jitterBuffer_;

    // The vsync period, measured from vsync times once there are enough of them: Android may
    // report 144 Hz for a panel that runs at 143.64 Hz
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
    bool presentAhead_ = false;
    int64_t presentGuardNs_ = 0;
    bool anchored_ = false;
    int64_t lastPtsNs_ = 0;
    int64_t lastAssignedNs_ = 0;  // The vsync the last frame was assigned
    double carry_ = 0;            // Part of a slot the host timestamps were ahead or behind
    int64_t holdSinceNs_ = 0;     // Since when every frame had a vsync to spare, or 0
    int64_t waitNs_ = 0;
    uint64_t lateFrames_ = 0;
    uint64_t pulls_ = 0;

    int64_t slotVsyncs_ = 1;
    std::deque<int64_t> ptsDeltas_;

    // Recent ready costs (addReadyCost())
    std::deque<int64_t> readyCosts_;
};

}  // namespace vkr
