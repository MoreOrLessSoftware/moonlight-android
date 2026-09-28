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
// No Android dependencies, so it can be tested on its own.
class PresentScheduler {
public:
    static constexpr int kMinDelay = 1;
    static constexpr int kMaxDelay = 4;

    int delayVsyncs() const { return delay_; }

    // When to ask for an image presented at `vsyncNs` to reach the screen: halfway through the
    // vsync interval before the one we want, so the compositor takes it for that vsync.
    int64_t desiredPresentNs(int64_t vsyncNs, int64_t periodNs) const {
        return vsyncNs + delay_ * periodNs - periodNs / 2;
    }

    // Feedback for one present: the vsync it was made at, the delay it asked for, and when the
    // device reports it reached the screen and could have at the earliest
    void onPresented(int64_t vsyncNs, int delayUsed, int64_t periodNs, int64_t actualNs, int64_t earliestNs,
                     int64_t marginNs);

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
    // a step at a time.
    int64_t guardNs() const { return guardNs_; }

    // How a frame presented ahead did: slackNs before its vsync, and whether it missed that vsync
    // or never reached the screen
    void onAheadOutcome(int64_t slackNs, bool failed, int64_t periodNs);

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

private:
    void resetWindows();

    int delay_ = 2;
    uint32_t raiseCount_ = 0;
    uint32_t raiseMisses_ = 0;
    uint32_t lowerCount_ = 0;
    uint32_t lowerNotSooner_ = 0;
    uint32_t lowerMisses_ = 0;
    uint32_t lowerBackoff_ = 1;  // Grows each time a shorter delay didn't hold
    bool loweredLast_ = false;
    uint32_t settling_ = kSettlingPresents;

    int64_t guardNs_ = kInitialGuardNs;
    double guardSamples_[kGuardBuckets] = {};
    double guardFails_[kGuardBuckets] = {};
    double guardTotal_ = 0;
    uint32_t guardSinceCheck_ = 0;
    uint32_t guardRaiseHold_ = 0;
    uint64_t missedTotal_ = 0;
};

}  // namespace vkr
