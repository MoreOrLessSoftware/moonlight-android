#pragma once

#include <cstdint>

namespace vkr {

// Decides how many vsyncs after a present its image should reach the screen.
//
// The compositor takes a presented image at its next deadline, which on some devices falls only
// a millisecond or two after we present. When rendering occasionally runs a little slower (the
// GPU clocking down between frames, say), the image misses that deadline and reaches the screen
// a vsync late, then the next one comes on time: a frame held one vsync too long followed by
// one held one vsync too short, however evenly the pacer chose them.
//
// So each present asks for a specific vsync (VK_GOOGLE_display_timing's desiredPresentTime),
// far enough out that the image is always ready for it, and the compositor holds it until
// then. That's how Android's own frame pacing library (Swappy) does it. The delay starts at
// what the device usually manages, rises a vsync when presents miss their vsync, and comes back
// down when the device's reports show every present could have made the shorter delay.
//
// With frames presented ahead (onAheadOutcome()), the delay comes down with the guard it needs:
// frames presented further ahead of their vsync make a shorter delay where others don't. Each
// present's report says how long before its vsync it needed to be presented for the vsync it
// could have made at the earliest (its slack less presentMargin), whatever delay it asked for.
// On an iPlay 70 mini Ultra at 144 Hz, that was 5.1-5.9 ms for one vsync (1st-99th percentile)
// at a delay of either one or two; a 6 ms guard at a delay of one took 3-8 ms off the time from
// decode to screen against a 2 ms guard at two.
//
// No Android dependencies, so it can be tested on its own.
class PresentScheduler {
public:
    static constexpr int kMinDelay = 1;
    static constexpr int kMaxDelay = 4;

    int delayVsyncs() const { return delay_; }

    // Holds the delay (in vsyncs) or the guard (in ns) at a value rather than learning it, for
    // trying a setting on a device. Either can be left out (0, or below 0 for the guard).
    void fix(int delayVsyncs, int64_t guardNs);

    // When to ask for an image presented at `vsyncNs` to reach the screen: halfway through the
    // vsync interval before the one we want, so the compositor takes it for that vsync.
    int64_t desiredPresentNs(int64_t vsyncNs, int64_t periodNs) const {
        return vsyncNs + delay_ * periodNs - periodNs / 2;
    }

    // Feedback for one present: the vsync it was made at, the delay it asked for, and when the
    // device reports it reached the screen and could have at the earliest. slackNs is how long
    // before its vsync a frame presented ahead was presented (-1 for others).
    //
    // Two kinds of miss don't count toward raising the delay, since more delay wouldn't have
    // shown the frame on time:
    // - frames shown just after a pause in the stream (kResumeGapNs, and at least three frames'
    //   time, without a present): on an
    //   iPlay 70 mini Ultra, the first frames after the network stalled for about 90 ms reached
    //   the screen a vsync late at any delay, the device reporting they couldn't have been
    //   sooner. Five of them within 2 s raised the delay to three vsyncs, where they went on.
    // - frames presented ahead with more than a vsync beyond the guard to spare
    void onPresented(int64_t vsyncNs, int delayUsed, int64_t periodNs, int64_t actualNs, int64_t earliestNs,
                     int64_t marginNs, int64_t slackNs = -1);
    static constexpr int64_t kResumeGapNs = 50'000'000;
    static constexpr uint32_t kResumePresents = 3;  // Presents after a pause its misses are excused for

    // Whether a present that reached the screen at or after targetNs could have made the vsync
    // before it: the image was ready in time for it (earliest), or it was ready over a vsync
    // before the compositor needed it (margin)
    static bool couldBeSooner(int64_t targetNs, int64_t earliestNs, int64_t marginNs, int64_t periodNs) {
        return earliestNs <= targetNs - periodNs + periodNs / 4 || marginNs >= periodNs + periodNs / 8;
    }

    uint64_t missedTotal() const { return missedTotal_; }

    // A new swapchain: its first presents miss for reasons of their own, so they're not counted
    void onSwapchainCreated() { settling_ = kSettlingPresents; }

    // How long before its vsync a frame presented ahead must be presented. With less time than
    // that, the compositor may not have it in time even for the vsyncs the delay allows: on a
    // Pixel 10 Pro, frames presented under 1 ms before their vsync nearly all missed it or were
    // dropped, under 2 ms about 40%, and 2-3.5 ms still 3-20%, against almost none from 4 ms. The pacer schedules
    // frames this much earlier (FramePacer::setPresentGuardNs()), which costs a little latency
    // where a longer delay would cost a whole vsync.
    //
    // Learned per device from how often frames fail at each slack: the lowest slack from which
    // frames all but never fail (kGuardMaxFailPercent), found by working down from the frames
    // presented with the most time to spare. It rises as soon as frames fail above it, and falls
    // a step at a time. At most half a vsync, unless a shorter delay needed more.
    //
    // Frames presented with less slack are the only ones that show a lower guard is safe, and
    // with the guard doing its job they're rare: in steady play nearly every frame is presented
    // with most of a vsync to spare. So it also falls, a step at a time, to what nearly all
    // recent frames needed for the current delay (kNeedWindow reports), the way the guard for a
    // shorter delay is chosen. On an iPlay 70 mini Ultra streaming PyroWave at 144 fps, a 4.5 ms
    // guard raised while the host idled on its desktop stayed for the rest of the session, where
    // frames needed under 3 ms.
    int64_t guardNs() const { return guardNs_; }

    // How a frame presented ahead did: slackNs before vsyncNs, the vsync it was presented for,
    // the delay it asked for, and if it reached the screen, the device's report of when, when it
    // could have at the earliest, and its presentMargin
    void onAheadOutcome(int64_t slackNs, int64_t vsyncNs, int delayUsed, bool reached, int64_t actualNs,
                        int64_t earliestNs, int64_t marginNs, int64_t periodNs);

    static constexpr int64_t kInitialGuardNs = 2'000'000;
    static constexpr int64_t kMinGuardNs = 500'000;
    static constexpr int64_t kGuardBucketNs = 250'000;
    static constexpr int kGuardBuckets = 40;           // Slack up to 10 ms
    static constexpr int kGuardBandBuckets = 4;        // Failure rates are judged over 1 ms of slack
    static constexpr double kGuardMinBandSamples = 24;
    static constexpr double kGuardMinBandFails = 3;
    static constexpr double kGuardMaxFailPercent = 1.0;
    static constexpr double kGuardHistory = 16384;      // Samples kept, older ones fading out
    static constexpr uint32_t kGuardCheckSamples = 32;
    static constexpr uint32_t kGuardRaiseHoldSamples = 512;

    // Presents between checks for a miss rate that needs a longer delay, and the number that
    // must all have been able to make a shorter delay before trying it
    // A delay that's too short misses a large share of presents, so it's caught quickly. A
    // compositor that now and then misses one (about one in a thousand on a Pixel 10 Pro) isn't
    // worth a vsync of latency on every frame.
    static constexpr uint32_t kRaiseWindow = 1024;
    static constexpr uint32_t kRaiseMisses = 4;
    static constexpr uint32_t kLowerWindow = 2048;
    static constexpr uint32_t kLowerMaxMisses = 1;
    static constexpr uint32_t kSettlingPresents = 60;

    // A shorter delay is taken with a longer guard only if the guard grows by less than this
    // share of the vsync saved, since the pacer can't always place frames to absorb all of it.
    // Growth from kInitialGuardNs, or the current guard if more (considerLowering()).
    static constexpr double kLowerMaxGuardCost = 0.75;
    // The guard for a shorter delay covers this percentile of what frames needed, plus a bucket
    static constexpr double kLowerNeedPercentile = 99.0;
    // and leaves this much of the time frames can be committed ahead (setAheadWindowNs()):
    // frames are committed at the latest at a vsync callback, a fraction of a millisecond after
    // a vsync, and a guard past what's left would put nearly every frame a vsync later
    static constexpr int64_t kLowerGuardHeadroomNs = 500'000;

    // How far ahead of its vsync a frame can be committed (FramePacer::presentAheadVsyncs()
    // vsyncs). A shorter delay is only taken with a guard that fits, and one whose guard no
    // longer fits (the game's frame rate went up) is given up at once.
    void setAheadWindowNs(int64_t windowNs);
    static constexpr int kNeedBuckets = 100;  // Needed slack up to 25 ms, in kGuardBucketNs
    // What frames needed is learned over this long, after each change of delay, and at least this many
    // reports, before taking a shorter delay. Many sessions are short, and the time frames need
    // varies little (0.2 ms either way on the iPlay 70 mini Ultra), so seconds of it are enough;
    // the extra bucket on the guard covers the percentile's error with this few.
    static constexpr int64_t kLowerLearnNs = 5'000'000'000;
    static constexpr uint32_t kLowerMinNeedSamples = 256;
    // Reports the guard for the current delay is judged from (guardNs()): about 14 s at 144 fps.
    // A fixed window rather than a fading history, so a slow stretch passes out of it.
    static constexpr int kNeedWindow = 2048;

private:
    void resetWindows();
    void resetGuardHistory();
    // Back to the delay and guard from before the last lowering
    void undoLowering();
    // With frames presented ahead: once there are enough outcomes, takes a shorter delay if
    // frames could have made it with a guard that costs less than the vsync saved
    void considerLowering(int64_t vsyncNs, int64_t periodNs);

    int delay_ = 2;
    bool delayFixed_ = false;
    bool guardFixed_ = false;
    bool aheadSeen_ = false;  // Frames are presented ahead, so considerLowering() decides
    uint32_t raiseCount_ = 0;
    uint32_t raiseMisses_ = 0;
    uint32_t lowerCount_ = 0;
    uint32_t lowerNotSooner_ = 0;
    uint32_t lowerMisses_ = 0;
    bool loweredLast_ = false;
    uint32_t settling_ = kSettlingPresents;

    int64_t guardNs_ = kInitialGuardNs;
    double guardSamples_[kGuardBuckets] = {};
    double guardFails_[kGuardBuckets] = {};
    double guardTotal_ = 0;
    uint32_t guardSinceCheck_ = 0;
    uint32_t guardRaiseHold_ = 0;
    uint64_t missedTotal_ = 0;

    // How long before their vsync frames needed to be presented to reach the screen a vsync
    // sooner than the delay, for the guard that delay would need (reports with a presentMargin)
    double needSamples_[kNeedBuckets] = {};
    double needTotal_ = 0;
    uint32_t needCount_ = 0;     // Since the delay last changed
    int64_t needStartNs_ = 0;    // The vsync of the first of those

    // What the last kNeedWindow reports needed for the current delay, by bucket, and the ring of
    // their buckets that ages them out
    uint32_t delayNeedSamples_[kNeedBuckets] = {};
    uint8_t delayNeedRing_[kNeedWindow] = {};
    uint32_t delayNeedCount_ = 0;
    uint32_t delayNeedNext_ = 0;
    // The guard that covers nearly all of those, or -1 with too few
    int64_t delayNeedGuardNs() const;

    // The guard before the delay was last lowered, put back if it's raised again, and the most
    // the guard may be raised to while lowered
    int64_t guardBeforeLower_ = 0;
    int64_t loweredGuardCapNs_ = 0;
    int64_t aheadWindowNs_ = 0;  // 0 until set: one vsync
    int64_t lastReportVsyncNs_ = 0;   // The vsync of the last present reported to onPresented()
    uint32_t resumingPresents_ = 0;   // Presents left whose misses a pause before them excuses
};

}  // namespace vkr
