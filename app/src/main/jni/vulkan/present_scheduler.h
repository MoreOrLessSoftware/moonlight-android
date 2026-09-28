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

    // Presents between checks for a miss rate that needs a longer delay, and the number that
    // must all have been able to make a shorter delay before trying it
    static constexpr uint32_t kRaiseWindow = 256;
    static constexpr uint32_t kRaiseMisses = 3;
    static constexpr uint32_t kLowerWindow = 4096;
    static constexpr uint32_t kSettlingPresents = 60;

private:
    void resetWindows();

    int delay_ = 2;
    uint32_t raiseCount_ = 0;
    uint32_t raiseMisses_ = 0;
    uint32_t lowerCount_ = 0;
    uint32_t lowerNotSooner_ = 0;
    uint32_t lowerBackoff_ = 1;  // Grows each time a shorter delay didn't hold
    bool loweredLast_ = false;
    uint32_t settling_ = kSettlingPresents;
    uint64_t missedTotal_ = 0;
};

}  // namespace vkr
