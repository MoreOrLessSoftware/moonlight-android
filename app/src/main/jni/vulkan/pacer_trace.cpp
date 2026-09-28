#include "pacer_trace.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define LOG_TAG "VulkanPacer"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace vkr {

namespace {
    constexpr int64_t kSummaryIntervalNs = 10'000'000'000;
    constexpr int64_t kFlushIntervalNs = 2'000'000'000;

    // About an hour at 144 Hz
    constexpr uint64_t kMaxTraceBytes = 256ull * 1024 * 1024;

    bool tracingEnabled() {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.moonlight.pacer_trace", value);
        return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
    }
}

void PacerTrace::start(const std::string& directory, const std::string& configuration) {
    if (directory.empty() || !tracingEnabled()) {
        return;
    }

    mkdir(directory.c_str(), 0770);

    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char name[64];
    strftime(name, sizeof(name), "pacer-%Y%m%d-%H%M%S.csv", &local);
    const std::string path = directory + "/" + name;

    file_ = fopen(path.c_str(), "w");
    if (!file_) {
        ALOGW("Could not create pacer trace %s", path.c_str());
        return;
    }
    // Buffered, and flushed every couple of seconds rather than per line
    setvbuf(file_, nullptr, _IOFBF, 1 << 20);

    char model[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.model", model);
    write("# moonlight pacer trace v1\n");
    write("C,%s,device=%s\n", configuration.c_str(), model);
    ALOGI("Recording pacer trace to %s", path.c_str());
}

void PacerTrace::close() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

void PacerTrace::write(const char* format, ...) {
    if (!file_) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = vfprintf(file_, format, args);
    va_end(args);

    if (written > 0) {
        bytes_ += static_cast<uint64_t>(written);
    }
    if (bytes_ > kMaxTraceBytes) {
        ALOGW("Pacer trace reached its size limit and stopped");
        close();
    }
}

void PacerTrace::period(int64_t periodNs) {
    write("R,%lld\n", static_cast<long long>(periodNs));
}

void PacerTrace::frame(const FrameTiming& timing, size_t queued, uint64_t queueDrops) {
    queueDrops_ = queueDrops;
    write("F,%lld,%lld,%lld,%zu,%llu\n", static_cast<long long>(timing.arrivalNs),
          static_cast<long long>(timing.hostPtsNs), static_cast<long long>(timing.targetNs), queued,
          static_cast<unsigned long long>(queueDrops));
}

void PacerTrace::ahead(int64_t nowNs, int64_t showVsyncNs, int64_t hostPtsNs, uint64_t presentId,
                       int presentDelayVsyncs, const FramePacer& pacer) {
    write("A,%lld,%lld,%lld,%llu,%d,%lld\n", static_cast<long long>(nowNs), static_cast<long long>(showVsyncNs),
          static_cast<long long>(hostPtsNs), static_cast<unsigned long long>(presentId), presentDelayVsyncs,
          static_cast<long long>(pacer.presentGuardNs()));
    presentGuardNs_ = pacer.presentGuardNs();
    countShown(pacer.vsyncIndexAt(showVsyncNs), 0, pacer);
}

void PacerTrace::immediate(int64_t nowNs) {
    write("I,%lld\n", static_cast<long long>(nowNs));
}

void PacerTrace::rendered(uint64_t presentId, int64_t startNs, int64_t fencedNs, int64_t acquiredNs,
                          int64_t queuedNs) {
    write("Q,%llu,%lld,%lld,%lld,%lld\n", static_cast<unsigned long long>(presentId), static_cast<long long>(startNs),
          static_cast<long long>(fencedNs), static_cast<long long>(acquiredNs), static_cast<long long>(queuedNs));
}

void PacerTrace::presented(uint64_t presentId, int64_t actualNs, int64_t earliestNs, int64_t marginNs) {
    write("P,%llu,%lld,%lld,%lld\n", static_cast<unsigned long long>(presentId), static_cast<long long>(actualNs),
          static_cast<long long>(earliestNs), static_cast<long long>(marginNs));

    // How long the previous frame was actually on screen, in slots
    if (lastActualNs_ != 0 && screenSlotNs_ > 0) {
        const double slots = static_cast<double>(actualNs - lastActualNs_) / static_cast<double>(screenSlotNs_);
        if (slots < 0.75) {
            screenShort_++;
        }
        else if (slots > 1.25) {
            screenLong_++;
        }
        screenFrames_++;
    }
    lastActualNs_ = actualNs;
}

void PacerTrace::received(int64_t hostPtsNs, int64_t receiveNs, int64_t enqueueNs) {
    write("N,%lld,%lld,%lld\n", static_cast<long long>(hostPtsNs), static_cast<long long>(receiveNs),
          static_cast<long long>(enqueueNs));
}

void PacerTrace::vsync(int64_t vsyncNs, size_t queued, int choice, int64_t shownPtsNs, const FramePacer& pacer,
                       int64_t callbackLateNs, uint64_t presentId, int presentDelayVsyncs) {
    presentDelay_ = presentDelayVsyncs;
    write("V,%lld,%zu,%d,%lld,%d,%lld,%lld,%lld,%lld,%lld,%.1f,%lld,%llu,%d\n", static_cast<long long>(vsyncNs), queued, choice,
          static_cast<long long>(shownPtsNs), pacer.phaseLocked() ? 1 : 0,
          static_cast<long long>(pacer.slotVsyncs()), static_cast<long long>(pacer.slotParity()),
          static_cast<long long>(pacer.scheduleDelayNs()), static_cast<long long>(pacer.timeline().bufferNs()),
          static_cast<long long>(pacer.vsyncPeriodNs()), pacer.phaseDriftPerFrame() * 1e6,
          static_cast<long long>(callbackLateNs), static_cast<unsigned long long>(presentId), presentDelayVsyncs);

    if (callbackLateNs * 2 > pacer.vsyncPeriodNs()) {
        lateCallbacks_++;
    }
    screenSlotNs_ = pacer.vsyncPeriodNs() * pacer.slotVsyncs();

    if (file_ && vsyncNs - lastFlushNs_ >= kFlushIntervalNs) {
        fflush(file_);
        lastFlushNs_ = vsyncNs;
    }

    // Summary counters. A frame held for more or fewer vsyncs than its slot is a visible
    // hitch; the pacer should only produce them as often as the rate difference demands.
    vsyncs_++;
    if (pacer.phaseLocked()) {
        lockedVsyncs_++;
    }
    if (choice >= 0) {
        countShown(pacer.vsyncIndex(), choice, pacer);
    }

    summarize(vsyncNs, pacer);
}

void PacerTrace::countShown(int64_t index, int skipped, const FramePacer& pacer) {
    // Two frames for the same vsync: the compositor shows only the newer one
    if (lastShownIndex_ >= 0 && index <= lastShownIndex_) {
        skipped_++;
        return;
    }
    shown_++;
    skipped_ += static_cast<uint32_t>(skipped);
    if (lastShownIndex_ >= 0) {
        const int64_t held = index - lastShownIndex_;
        if (held < pacer.slotVsyncs()) {
            heldShort_++;
        }
        else if (held > pacer.slotVsyncs()) {
            heldLong_++;
        }
    }
    lastShownIndex_ = index;
}

void PacerTrace::summarize(int64_t vsyncNs, const FramePacer& pacer) {
    if (windowStartNs_ == 0) {
        windowStartNs_ = vsyncNs;
        queueDropsAtStart_ = queueDrops_;
        return;
    }
    const int64_t elapsed = vsyncNs - windowStartNs_;
    if (elapsed < kSummaryIntervalNs) {
        return;
    }

    const double seconds = elapsed / 1e9;
    const double period = static_cast<double>(pacer.vsyncPeriodNs());
    const double contentInterval = period * pacer.slotVsyncs() * (1.0 + pacer.phaseDriftPerFrame());
    char screen[96] = "on screen: no display timing";
    if (screenFrames_ > 0) {
        snprintf(screen, sizeof(screen), "on screen %u short / %u long, present delay %d vsyncs, guard %.1f ms",
                 screenShort_, screenLong_, presentDelay_, presentGuardNs_ / 1e6);
    }
    ALOGI("%.0fs: %u frames (%.2f fps), %u short / %u long holds (%s), %u skipped, %llu queue drops, "
          "%u late callbacks | %s %.0f%%, %lld vsync%s per frame, buffer %.1f ms + schedule %.1f ms, "
          "vsync %.3f Hz, content %.3f fps (drift %+.1f ppm)",
          seconds, shown_, shown_ / seconds, heldShort_, heldLong_, screen, skipped_,
          static_cast<unsigned long long>(queueDrops_ - queueDropsAtStart_), lateCallbacks_,
          pacer.phaseLocked() ? "locked" : "unlocked", vsyncs_ ? 100.0 * lockedVsyncs_ / vsyncs_ : 0.0,
          static_cast<long long>(pacer.slotVsyncs()), pacer.slotVsyncs() == 1 ? "" : "s",
          pacer.timeline().bufferNs() / 1e6, pacer.scheduleDelayNs() / 1e6,
          1e9 / period, 1e9 / contentInterval, pacer.phaseDriftPerFrame() * 1e6);

    windowStartNs_ = vsyncNs;
    queueDropsAtStart_ = queueDrops_;
    shown_ = heldShort_ = heldLong_ = skipped_ = lockedVsyncs_ = vsyncs_ = 0;
    lateCallbacks_ = screenShort_ = screenLong_ = screenFrames_ = 0;
}

}  // namespace vkr
