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
        if (choice < 0) {
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
                   (long long) e.held, (long long) e.slot,
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
    std::map<int64_t, int64_t> arrival;         // host pts -> decoded frame reached the renderer
    struct Received {
        int64_t receive;
        int64_t enqueue;
    };
    std::map<int64_t, Received> received;       // host pts -> network timing
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
            printf("  presented -> on screen: %s\n", percentiles(latency, false).c_str());
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
        fprintf(stderr, "usage: %s trace.csv [--events N] [--explain N]\n", argv[0]);
        return 2;
    }
    size_t maxEvents = 25;
    size_t explain = 0;
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--events") == 0) {
            maxEvents = static_cast<size_t>(atoi(argv[i + 1]));
        }
        if (strcmp(argv[i], "--explain") == 0) {
            explain = static_cast<size_t>(atoi(argv[i + 1]));
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

    FramePacer pacer(static_cast<PacingMode>(config.mode), config.streamFps, config.periodNs);
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
            lastRecordedDrops = static_cast<uint64_t>(toInt(f[5]));
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
            }
            replayed.onVsync(vsyncNs, pacer.vsyncIndex(), choice, pacer.slotVsyncs(), pacer.phaseLocked(),
                             (pacer.timeline().bufferNs() + pacer.scheduleDelayNs()) / 1e6);
        }
    }
    recorded.queueDrops = lastRecordedDrops;

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
