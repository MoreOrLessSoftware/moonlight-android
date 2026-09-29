// Analyses a frame pacing trace recorded on a device (see pacer_trace.h), and replays the
// session's frames and vsyncs through the current FramePacer to show how today's code would
// have paced it. Not part of the app build; build and run on the host with:
//   g++ -std=c++17 -O2 -I.. ../frame_pacer.cpp pacer_replay.cpp -o pacer_replay
//   ./pacer_replay pacer-20260928-081500.csv [--events 40]
// or use tools/pacer-trace.sh, which builds it and runs it on pulled traces.

#include "frame_pacer.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace vkr;

namespace {

struct Config {
    int mode = 4;
    int jitterBuffer = 0;  // Traces from before the setting used what is now LowLatency
    int presentAhead = 0;
    int streamFps = 60;
    int64_t periodNs = 16'666'667;
    std::string text;
};

// Uneven frames, and when they happened, for one run (recorded or replayed)
struct Tally {
    const char* name;
    uint64_t vsyncs = 0;
    uint64_t lockedVsyncs = 0;
    uint64_t shown = 0;
    uint64_t heldShort = 0;
    uint64_t heldLong = 0;
    uint64_t skipped = 0;
    uint64_t queueDrops = 0;
    double bufferSumMs = 0;
    int64_t firstVsyncNs = 0;
    int64_t lastVsyncNs = 0;
    int64_t lastShownIndex = -1;
    int64_t firstLockNs = 0;       // Before this the pacer is still measuring the stream
    uint64_t settlingUneven = 0;
    uint64_t steadyUneven = 0;
    std::map<int64_t, uint32_t> unevenPer10s;

    struct Event {
        int64_t timeNs;
        int64_t held;
        int64_t slot;
        int skipped;
        bool locked;
    };
    std::vector<Event> events;

    // A frame shown at vsyncNs that was committed ahead of it
    void onShown(int64_t vsyncNs, int64_t index, int skippedFrames, int64_t slot, bool locked, double) {
        if (firstVsyncNs == 0) {
            return;
        }
        if (locked && firstLockNs == 0) {
            firstLockNs = vsyncNs;
        }
        countShow(vsyncNs, index, skippedFrames, slot, locked);
    }

    void countShow(int64_t vsyncNs, int64_t index, int choice, int64_t slot, bool locked) {
        // Two frames asking for the same vsync (a late frame presented ahead, then the frame due
        // at its vsync): the compositor shows only the newer one
        if (lastShownIndex >= 0 && index <= lastShownIndex) {
            skipped++;
            (firstLockNs ? steadyUneven : settlingUneven)++;
            unevenPer10s[(vsyncNs - firstVsyncNs) / 10'000'000'000]++;
            events.push_back({vsyncNs, 0, slot, 1, locked});
            return;
        }
        shown++;
        skipped += static_cast<uint64_t>(choice);
        if (lastShownIndex >= 0) {
            const int64_t held = index - lastShownIndex;
            const bool uneven = held != slot || choice > 0;
            if (held < slot) heldShort++;
            if (held > slot) heldLong++;
            if (uneven) {
                (firstLockNs ? steadyUneven : settlingUneven)++;
                unevenPer10s[(vsyncNs - firstVsyncNs) / 10'000'000'000]++;
                events.push_back({vsyncNs, held, slot, choice, locked});
            }
        }
        lastShownIndex = index;
    }

    void onVsync(int64_t vsyncNs, int64_t index, int choice, int64_t slot, bool locked, double bufferMs) {
        if (firstVsyncNs == 0) {
            firstVsyncNs = vsyncNs;
        }
        lastVsyncNs = vsyncNs;
        if (locked && firstLockNs == 0) {
            firstLockNs = vsyncNs;
        }
        vsyncs++;
        lockedVsyncs += locked ? 1 : 0;
        bufferSumMs += bufferMs;
        if (choice >= 0) {
            countShow(vsyncNs, index, choice, slot, locked);
        }
    }

    void print(size_t maxEvents) const {
        const double seconds = (lastVsyncNs - firstVsyncNs) / 1e9;
        if (seconds <= 0) {
            printf("%s: no vsyncs\n", name);
            return;
        }
        printf("%s:\n", name);
        printf("  %.1f s, %" PRIu64 " frames shown (%.2f fps), locked %.0f%% of the time, mean schedule %.1f ms\n",
               seconds, shown, shown / seconds, vsyncs ? 100.0 * lockedVsyncs / vsyncs : 0.0,
               vsyncs ? bufferSumMs / vsyncs : 0.0);
        printf("  %" PRIu64 " held short, %" PRIu64 " held long, %" PRIu64 " skipped, %" PRIu64
               " dropped from the queue  (%.2f uneven frames a minute)\n",
               heldShort, heldLong, skipped, queueDrops,
               (heldShort + heldLong + skipped) * 60.0 / seconds);

        if (firstLockNs) {
            const double steadySeconds = (lastVsyncNs - firstLockNs) / 1e9;
            printf("  locked after %.1f s (%" PRIu64 " uneven frames while settling); after that %" PRIu64
                   " uneven frames in %.1f s (%.2f a minute)\n",
                   (firstLockNs - firstVsyncNs) / 1e9, settlingUneven, steadyUneven, steadySeconds,
                   steadySeconds > 0 ? steadyUneven * 60.0 / steadySeconds : 0.0);
        }
        else {
            printf("  never locked: the stream's frame times didn't settle into a steady rate\n");
        }
        if (!unevenPer10s.empty()) {
            printf("  uneven frames per 10 s:");
            const int64_t buckets = static_cast<int64_t>(seconds / 10) + 1;
            for (int64_t b = 0; b < buckets; b++) {
                auto it = unevenPer10s.find(b);
                printf(" %u", it == unevenPer10s.end() ? 0u : it->second);
            }
            printf("\n");
        }
        for (size_t i = 0; i < events.size() && i < maxEvents; i++) {
            const Event& e = events[i];
            printf("    t=%8.3f s  held %lld vsyncs (slot %lld)%s%s\n", (e.timeNs - firstVsyncNs) / 1e9,
                   (long long) (e.held > 0 ? e.held : 0), (long long) e.slot,
                   e.skipped ? ", skipped a frame" : "", e.locked ? "" : ", unlocked");
        }
        if (events.size() > maxEvents) {
            printf("    ... %zu more\n", events.size() - maxEvents);
        }
    }
};

std::vector<std::string> split(const char* line) {
    std::vector<std::string> fields;
    std::string field;
    for (const char* p = line; *p && *p != '\n' && *p != '\r'; p++) {
        if (*p == ',') {
            fields.push_back(field);
            field.clear();
        }
        else {
            field += *p;
        }
    }
    fields.push_back(field);
    return fields;
}

int64_t toInt(const std::string& s) {
    return std::strtoll(s.c_str(), nullptr, 10);
}

// Percentiles of a sample, in ms, relative to its minimum when `relative` is set
std::string percentiles(std::vector<int64_t> values, bool relative) {
    if (values.empty()) {
        return "no data";
    }
    std::sort(values.begin(), values.end());
    const int64_t base = relative ? values.front() : 0;
    auto at = [&](double q) { return (values[static_cast<size_t>(q * (values.size() - 1))] - base) / 1e6; };
    char text[160];
    snprintf(text, sizeof(text), "p50 %.2f  p90 %.2f  p99 %.2f  max %.2f ms", at(0.5), at(0.9), at(0.99), at(1.0));
    return text;
}

// What the device measured beyond the pacer's own decisions: how late the render thread ran,
// when frames really reached the screen, and where frames' delay came from
struct DeviceTiming {
    std::vector<int64_t> callbackLate;
    uint64_t lateCallbacks = 0;
    std::map<uint64_t, int64_t> presentVsync;   // presentId -> vsync it was presented at
    std::map<uint64_t, int64_t> presentActual;  // presentId -> when it reached the screen
    std::map<uint64_t, int> presentDelay;       // presentId -> vsyncs after it asked to be shown
    std::map<uint64_t, int64_t> presentPts;     // presentId -> host pts of the frame
    std::map<int64_t, int64_t> arrival;         // host pts -> decoded frame reached the renderer
    struct Received {
        int64_t receive;
        int64_t enqueue;
    };
    std::map<int64_t, Received> received;       // host pts -> network timing
    std::map<uint64_t, int64_t> presentCommit;  // presentId -> committed to its vsync (A lines)
    struct Rendered {
        int64_t start;
        int64_t fenced;
        int64_t acquired;
        int64_t queued;
    };
    std::map<uint64_t, Rendered> rendered;      // presentId -> render timing (Q lines)
    int64_t periodNs = 0;
    int64_t slotVsyncs = 1;

    void print() const {
        printf("Device timing:\n");
        if (!callbackLate.empty()) {
            printf("  vsync callbacks ran late by %s; %llu of %zu more than half a vsync late\n",
                   percentiles(callbackLate, false).c_str(), (unsigned long long) lateCallbacks, callbackLate.size());
        }

        if (presentActual.empty()) {
            printf("  no on-screen timing (older trace, or no VK_GOOGLE_display_timing)\n");
        }
        else {
            // Time from the vsync a frame was presented at to when it reached the screen. A
            // steady pipeline shows one value; a second cluster a vsync later is the compositor
            // missing frames.
            std::vector<int64_t> latency;
            uint64_t shortHolds = 0, longHolds = 0, frames = 0;
            int64_t lastActual = 0;
            std::map<int64_t, uint32_t> latencyVsyncs;
            for (const auto& entry : presentActual) {
                auto v = presentVsync.find(entry.first);
                if (v != presentVsync.end()) {
                    latency.push_back(entry.second - v->second);
                    if (periodNs > 0) {
                        latencyVsyncs[(entry.second - v->second + periodNs / 2) / periodNs]++;
                    }
                }
                if (lastActual != 0 && periodNs > 0) {
                    const double slots = static_cast<double>(entry.second - lastActual) / (periodNs * slotVsyncs);
                    shortHolds += slots < 0.75;
                    longHolds += slots > 1.25;
                    frames++;
                }
                lastActual = entry.second;
            }
            printf("  on screen: %llu frames, %llu held short, %llu held long\n",
                   (unsigned long long) frames, (unsigned long long) shortHolds, (unsigned long long) longHolds);
            printf("  scheduled vsync -> on screen: %s\n", percentiles(latency, false).c_str());

            // The client's share of latency: from the decoded frame reaching the renderer to it
            // being on screen (jitter buffer, waiting for its vsync, and the present delay)
            std::vector<int64_t> decodedToScreen;
            for (const auto& entry : presentActual) {
                auto p = presentPts.find(entry.first);
                if (p == presentPts.end()) continue;
                auto a = arrival.find(p->second);
                if (a != arrival.end()) {
                    decodedToScreen.push_back(entry.second - a->second);
                }
            }
            if (!decodedToScreen.empty()) {
                printf("  decoded -> on screen: %s\n", percentiles(decodedToScreen, false).c_str());
            }
            // Where the time between committing a frame presented ahead and handing it to the
            // compositor goes (it eats into the present guard)
            std::vector<int64_t> commitToStart, gpuWait, acquireWait, queueTime, commitToQueued;
            for (const auto& [id, r] : rendered) {
                auto commit = presentCommit.find(id);
                if (commit == presentCommit.end()) continue;
                commitToStart.push_back(r.start - commit->second);
                gpuWait.push_back(r.fenced - r.start);
                acquireWait.push_back(r.acquired - r.fenced);
                queueTime.push_back(r.queued - r.acquired);
                commitToQueued.push_back(r.queued - commit->second);
            }
            if (!commitToQueued.empty()) {
                printf("  committed -> handed to the compositor: %s\n", percentiles(commitToQueued, false).c_str());
                printf("    waiting to render:        %s\n", percentiles(commitToStart, false).c_str());
                printf("    waiting for the GPU:      %s\n", percentiles(gpuWait, false).c_str());
                printf("    waiting for an image:     %s\n", percentiles(acquireWait, false).c_str());
                printf("    recording and presenting: %s\n", percentiles(queueTime, false).c_str());
            }
            if (!presentDelay.empty()) {
                uint64_t missed = 0;
                std::map<int, uint32_t> delays;
                for (const auto& entry : presentDelay) {
                    delays[entry.second]++;
                    auto a = presentActual.find(entry.first);
                    auto v = presentVsync.find(entry.first);
                    if (a != presentActual.end() && v != presentVsync.end() && entry.second > 0 &&
                            a->second > v->second + entry.second * periodNs + periodNs / 2) {
                        missed++;
                    }
                }
                printf("  requested present delay:");
                for (const auto& d : delays) {
                    printf("  %d vsyncs: %u presents", d.first, d.second);
                }
                printf("; %llu reached the screen after the vsync they asked for\n", (unsigned long long) missed);
            }
            printf("  presented -> on screen in vsyncs:");
            for (const auto& bucket : latencyVsyncs) {
                printf("  %lld: %u", (long long) bucket.first, bucket.second);
            }
            printf("\n");
        }

        if (!received.empty()) {
            std::vector<int64_t> network, assembly, decode;
            for (const auto& entry : received) {
                network.push_back(entry.second.receive - entry.first);
                assembly.push_back(entry.second.enqueue - entry.second.receive);
                auto a = arrival.find(entry.first);
                if (a != arrival.end()) {
                    decode.push_back(a->second - entry.second.enqueue);
                }
            }
            printf("  frame delay, relative to each stage's fastest frame:\n");
            printf("    network (first packet):   %s\n", percentiles(network, true).c_str());
            printf("    receiving the whole frame: %s\n", percentiles(assembly, false).c_str());
            printf("    decode and delivery:      %s\n", percentiles(decode, true).c_str());
        }
        else {
            printf("  no network timing (older trace)\n");
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s trace.csv [--events N] [--explain N] [--jitter-buffer low|balanced|smooth]"
                        " [--present-ahead on|off] [--present-guard ms]\n", argv[0]);
        return 2;
    }
    size_t maxEvents = 25;
    size_t explain = 0;
    int jitterBufferOverride = -1;
    int presentAheadOverride = -1;
    int64_t presentGuardOverride = -1;
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--events") == 0) {
            maxEvents = static_cast<size_t>(atoi(argv[i + 1]));
        }
        if (strcmp(argv[i], "--explain") == 0) {
            explain = static_cast<size_t>(atoi(argv[i + 1]));
        }
        // Replay with a fixed present guard, in ms, rather than the device's
        if (strcmp(argv[i], "--present-guard") == 0) {
            presentGuardOverride = static_cast<int64_t>(atof(argv[i + 1]) * 1e6);
        }
        // Replay presenting frames as they arrive, or not, whatever the session did
        if (strcmp(argv[i], "--present-ahead") == 0) {
            presentAheadOverride = strcmp(argv[i + 1], "on") == 0 ? 1 : 0;
        }
        // Replay with a different jitter buffer than the session used
        if (strcmp(argv[i], "--jitter-buffer") == 0) {
            const char* value = argv[i + 1];
            jitterBufferOverride = strcmp(value, "lowest") == 0 ? 3 : strcmp(value, "low") == 0 ? 0 :
                                   strcmp(value, "smooth") == 0 ? 2 : 1;
        }
    }

    FILE* file = fopen(argv[1], "r");
    if (!file) {
        perror(argv[1]);
        return 1;
    }

    // Read everything first: the configuration line decides how the pacer is built
    std::vector<std::vector<std::string>> events;
    Config config;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        std::vector<std::string> fields = split(line);
        if (fields[0] == "C") {
            for (size_t i = 1; i < fields.size(); i++) {
                const size_t eq = fields[i].find('=');
                if (eq == std::string::npos) continue;
                const std::string key = fields[i].substr(0, eq);
                const std::string value = fields[i].substr(eq + 1);
                if (key == "mode") config.mode = static_cast<int>(toInt(value));
                if (key == "jitterBuffer") config.jitterBuffer = static_cast<int>(toInt(value));
                if (key == "presentAhead") config.presentAhead = static_cast<int>(toInt(value));
                if (key == "streamFps") config.streamFps = static_cast<int>(toInt(value));
                if (key == "periodNs") config.periodNs = toInt(value);
            }
            config.text = line;
            continue;
        }
        events.push_back(std::move(fields));
    }
    fclose(file);

    printf("%s", config.text.c_str());

    Tally recorded {"Recorded on the device"};
    Tally replayed {"Replayed through the current pacer"};

    static const char* const kJitterBufferNames[] = {"low latency", "balanced", "smooth", "lowest latency"};
    const int jitterBuffer = jitterBufferOverride >= 0 ? jitterBufferOverride : config.jitterBuffer;
    printf("Jitter buffer: %s on the device, %s in the replay\n", kJitterBufferNames[config.jitterBuffer % 4],
           kJitterBufferNames[jitterBuffer % 4]);
    const bool presentAhead = presentAheadOverride >= 0 ? presentAheadOverride != 0 : config.presentAhead != 0;
    printf("Presenting frames as they arrive: %s on the device, %s in the replay\n", config.presentAhead ? "on" : "off",
           presentAhead ? "on" : "off");
    // How long presented-ahead frames waited in the replay before their vsync: the time the GPU
    // and compositor get to finish them
    std::vector<int64_t> aheadSlack;
    uint64_t aheadCount = 0;
    uint64_t atVsyncCount = 0;

    FramePacer pacer(static_cast<PacingMode>(config.mode), config.streamFps, config.periodNs,
                     static_cast<JitterBuffer>(jitterBuffer));
    pacer.setPresentAhead(presentAhead);
    if (presentGuardOverride >= 0) {
        pacer.setPresentGuardNs(presentGuardOverride);
    }
    std::deque<FrameTiming> queue;
    int64_t recordedIndex = 0;
    int64_t lastRecordedVsync = 0;
    uint64_t decisionsDiffering = 0;
    uint64_t lastRecordedDrops = 0;
    DeviceTiming device;

    // --explain: the last few vsyncs of the replay, printed when a frame is held unevenly while
    // locked, to show whether a frame arrived late or its schedule moved
    struct VsyncState {
        int64_t vsyncNs;
        int choice;
        std::string queue;
    };
    std::deque<VsyncState> history;
    int64_t explainLastShown = -1;
    int64_t explainFirstNs = 0;
    size_t explained = 0;

    for (const auto& f : events) {
        if (f[0] == "R" && f.size() >= 2) {
            pacer.setVsyncPeriod(toInt(f[1]));
        }
        else if (f[0] == "F" && f.size() >= 6) {
            FrameTiming timing;
            timing.arrivalNs = toInt(f[1]);
            timing.hostPtsNs = toInt(f[2]);
            device.arrival[timing.hostPtsNs] = timing.arrivalNs;
            pacer.onFrameArrived(timing);
            queue.push_back(timing);
            while (queue.size() > pacer.maxQueued()) {
                queue.pop_front();
                replayed.queueDrops++;
            }
            if (presentAhead) {
                while (!queue.empty()) {
                    const int64_t showVsync = pacer.plannedVsyncNs(queue.front());
                    if (showVsync == 0) break;
                    pacer.onPresentedAhead(showVsync);
                    aheadSlack.push_back(showVsync - timing.arrivalNs);
                    aheadCount++;
                    replayed.onShown(showVsync, pacer.vsyncIndexAt(showVsync), 0, pacer.slotVsyncs(), pacer.phaseLocked(),
                                     (pacer.timeline().bufferNs() + pacer.scheduleDelayNs()) / 1e6);
                    queue.pop_front();
                }
            }
            lastRecordedDrops = static_cast<uint64_t>(toInt(f[5]));
        }
        else if (f[0] == "A" && f.size() >= 6) {
            // Use the device's present guard from here on: it depends on how the device's
            // compositor performed, which the replay can't model
            if (f.size() >= 7 && presentGuardOverride < 0) {
                pacer.setPresentGuardNs(toInt(f[6]));
            }
            const int64_t showVsync = toInt(f[2]);
            const int64_t period = device.periodNs > 0 ? device.periodNs : config.periodNs;
            recorded.onShown(showVsync, recordedIndex + (showVsync - lastRecordedVsync + period / 2) / period, 0,
                             device.slotVsyncs, true, 0);
            const uint64_t presentId = static_cast<uint64_t>(toInt(f[4]));
            device.presentVsync[presentId] = showVsync;
            device.presentDelay[presentId] = static_cast<int>(toInt(f[5]));
            device.presentPts[presentId] = toInt(f[3]);
            device.presentCommit[presentId] = toInt(f[1]);
        }
        else if (f[0] == "Q" && f.size() >= 6) {
            device.rendered[static_cast<uint64_t>(toInt(f[1]))] = {toInt(f[2]), toInt(f[3]), toInt(f[4]), toInt(f[5])};
        }
        else if (f[0] == "P" && f.size() >= 5) {
            device.presentActual[static_cast<uint64_t>(toInt(f[1]))] = toInt(f[2]);
        }
        else if (f[0] == "N" && f.size() >= 4) {
            device.received[toInt(f[1])] = {toInt(f[2]), toInt(f[3])};
        }
        else if (f[0] == "V" && f.size() >= 12) {
            const int64_t vsyncNs = toInt(f[1]);
            device.periodNs = toInt(f[10]);
            device.slotVsyncs = std::max<int64_t>(1, toInt(f[6]));
            if (f.size() >= 14) {
                const int64_t late = toInt(f[12]);
                device.callbackLate.push_back(late);
                device.lateCallbacks += late * 2 > device.periodNs;
                const uint64_t presentId = static_cast<uint64_t>(toInt(f[13]));
                if (presentId != 0) {
                    device.presentVsync[presentId] = vsyncNs;
                    device.presentPts[presentId] = toInt(f[4]);
                    if (f.size() >= 15) {
                        device.presentDelay[presentId] = static_cast<int>(toInt(f[14]));
                    }
                }
            }
            const int recordedChoice = static_cast<int>(toInt(f[3]));
            const int64_t recordedPeriod = toInt(f[10]);

            // The device's vsync count, rebuilt from the recorded times and periods
            if (lastRecordedVsync != 0 && recordedPeriod > 0) {
                recordedIndex += std::max<int64_t>(1, (vsyncNs - lastRecordedVsync + recordedPeriod / 2) / recordedPeriod);
            }
            lastRecordedVsync = vsyncNs;
            recorded.onVsync(vsyncNs, recordedIndex, recordedChoice, toInt(f[6]), toInt(f[5]) != 0,
                             (toInt(f[8]) + toInt(f[9])) / 1e6);

            std::vector<FrameTiming> timings(queue.begin(), queue.end());
            if (timings.size() > FramePacer::kMaxQueuedFrames) {
                timings.resize(FramePacer::kMaxQueuedFrames);
            }
            const int choice = pacer.onVsync(vsyncNs, timings.data(), timings.size());
            if (explain) {
                if (explainFirstNs == 0) explainFirstNs = vsyncNs;
                // Each waiting frame as arrival and due time relative to this vsync, in ms
                std::string queueText;
                char item[64];
                for (const FrameTiming& t : timings) {
                    snprintf(item, sizeof(item), " [arr %+.1f due %+.1f]", (t.arrivalNs - vsyncNs) / 1e6,
                             (t.targetNs + pacer.phaseShiftNs() - vsyncNs) / 1e6);
                    queueText += item;
                }
                history.push_back({vsyncNs, choice, queueText});
                if (history.size() > 5) history.pop_front();
                if (choice >= 0) {
                    const int64_t held = explainLastShown >= 0 ? pacer.vsyncIndex() - explainLastShown : pacer.slotVsyncs();
                    if ((held != pacer.slotVsyncs() || choice > 0) && pacer.phaseLocked() && explained < explain) {
                        explained++;
                        printf("t=%.3f s: held %lld (slot %lld, parity %lld), schedule +%.1f ms, shift %.1f ms, buffer %.1f ms, drift %+.0f ppm\n",
                               (vsyncNs - explainFirstNs) / 1e9, (long long) held, (long long) pacer.slotVsyncs(),
                               (long long) pacer.slotParity(), pacer.scheduleDelayNs() / 1e6, pacer.phaseShiftNs() / 1e6,
                               pacer.timeline().bufferNs() / 1e6, pacer.phaseDriftPerFrame() * 1e6);
                        for (const VsyncState& h : history) {
                            printf("    vsync %+7.1f ms: %s%s\n", (h.vsyncNs - vsyncNs) / 1e6,
                                   h.choice >= 0 ? "showed #" : "nothing", h.choice >= 0 ? std::to_string(h.choice).c_str() : "");
                            printf("        queue:%s\n", h.queue.empty() ? " empty" : h.queue.c_str());
                        }
                    }
                    explainLastShown = pacer.vsyncIndex();
                }
            }
            if (choice != recordedChoice) {
                decisionsDiffering++;
            }
            if (choice >= 0) {
                queue.erase(queue.begin(), queue.begin() + choice + 1);
                atVsyncCount++;
            }
            replayed.onVsync(vsyncNs, pacer.vsyncIndex(), choice, pacer.slotVsyncs(), pacer.phaseLocked(),
                             (pacer.timeline().bufferNs() + pacer.scheduleDelayNs()) / 1e6);
            if (presentAhead) {
                while (!queue.empty()) {
                    const int64_t showVsync = pacer.plannedVsyncNs(queue.front());
                    if (showVsync == 0) break;
                    pacer.onPresentedAhead(showVsync);
                    aheadSlack.push_back(showVsync - vsyncNs);
                    aheadCount++;
                    replayed.onShown(showVsync, pacer.vsyncIndexAt(showVsync), 0, pacer.slotVsyncs(), pacer.phaseLocked(),
                                     (pacer.timeline().bufferNs() + pacer.scheduleDelayNs()) / 1e6);
                    queue.pop_front();
                }
            }
        }
    }
    recorded.queueDrops = lastRecordedDrops;
    if (presentAhead) {
        printf("Replay presented %llu frames as they arrived and %llu at their vsync; time before their vsync: %s\n",
               (unsigned long long) aheadCount, (unsigned long long) atVsyncCount,
               percentiles(aheadSlack, false).c_str());
        size_t under2 = 0, under3 = 0;
        for (int64_t slack : aheadSlack) {
            under2 += slack < 2'000'000;
            under3 += slack < 3'000'000;
        }
        printf("  presented less than 2 ms before their vsync: %zu, less than 3 ms: %zu\n", under2, under3);
    }

    printf("\n");
    recorded.print(maxEvents);
    printf("\n");
    device.print();
    printf("\n");
    replayed.print(maxEvents);
    printf("\n%" PRIu64 " of %" PRIu64 " vsync decisions differ between the device and the replay%s\n",
           decisionsDiffering, recorded.vsyncs,
           decisionsDiffering ? " (expected if the pacer changed since the trace was recorded)" : "");
    return 0;
}
