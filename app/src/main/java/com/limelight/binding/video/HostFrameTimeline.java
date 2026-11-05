package com.limelight.binding.video;

import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import java.util.Arrays;

/**
 * Schedules frames for the direct (MediaCodec to SurfaceView) renderer in the "sync to host
 * frame timing" pacing mode, by the same rules as the Vulkan renderer's pacer
 * (jni/vulkan/frame_pacer.h), which any change here should follow:
 *
 * - Each frame is assigned the vsync after the previous frame's, plus as many slots as its host
 *   timestamp advanced (a slot is the whole number of vsyncs between host frames: one when the
 *   host's frame rate is matched to our refresh rate).
 * - A frame that can't make its vsync goes to the first one it can, and the frames after it
 *   follow from there.
 * - When every frame for the preset's pull hold could have made the vsync before its own with a
 *   margin to spare, frames move a vsync sooner.
 * - No frame waits the preset's wait cap or more after it could first be shown.
 * - Balanced adds headroom to when each frame could first be shown.
 *
 * The vsyncs come from Choreographer (onVsync()). SurfaceFlinger shows a buffer at the first
 * vsync whose expected present time is at or after the buffer's timestamp, about a refresh after
 * it latches it, so each frame is stamped half a vsync before the present of its assigned vsync:
 * well clear of the vsyncs either side, so timing jitter doesn't move it to one of them.
 *
 * Logs the preset whenever it's set, and a summary every 10 s, both starting "Host frame timing"
 * (this renderer writes no pacer trace).
 */
class HostFrameTimeline {
    // A host timestamp further from the last than this starts the schedule over
    private static final long MAX_PTS_GAP_NS = 250_000_000L;
    // How much of the part of a slot that host timestamps were early or late is carried to the
    // next frame (see jni/vulkan/frame_pacer.cpp)
    private static final double CARRY_KEEP = 0.5;

    // Host timestamp spacings the slot is taken from
    private static final int SLOT_DELTA_WINDOW = 15;
    private static final int MIN_SLOT_DELTAS = 8;

    // Vsyncs the period is measured over
    private static final int VSYNC_WINDOW = 240;
    private static final int MIN_VSYNCS_FOR_PERIOD = 60;

    // About how long after SurfaceFlinger latches a buffer at a vsync it's shown
    private final long presentLatencyNs;

    // The vsync period, measured from vsync times once there are enough of them
    private long periodNs;
    private final long[] vsyncIndexes = new long[VSYNC_WINDOW];
    private final long[] vsyncTimes = new long[VSYNC_WINDOW];
    private int vsyncHead;
    private int vsyncCount;
    private long lastVsyncNs;
    private long vsyncIndex;

    // The preset (see jni/vulkan/frame_pacer.cpp for how they were chosen)
    private long headroomNs;
    private long maxWaitVsyncs;
    private long pullHoldNs;
    private int pullMarginDivisor = 16;
    private int lateStreakToShift = 1;
    private int jitterBuffer = -1;
    // The preset to switch to, set from any thread (setJitterBuffer())
    private volatile int pendingJitterBuffer;

    private boolean anchored;
    private long lastPtsNs;
    private long lastAssignedNs;
    private long lastTargetNs;
    private double carry;
    private long holdSinceNs;
    private long holdMinRoomNs;
    private int consecutiveLate;
    private long slotVsyncs = 1;
    private final long[] ptsDeltas = new long[SLOT_DELTA_WINDOW];
    private final long[] sortedDeltas = new long[SLOT_DELTA_WINDOW];
    private int ptsDeltaHead;
    private int ptsDeltaCount;

    private long lateFrames;
    private long pulls;

    // The 10 s summary (logSummary())
    private static final long SUMMARY_INTERVAL_NS = 10_000_000_000L;
    private long summaryStartNs;
    private int summaryFrames;
    private long summaryWaitSumNs;
    private long summaryWaitMaxNs;
    private long summaryLateAtStart;
    private long summaryPullsAtStart;

    /**
     * @param displayRefreshHz current display refresh rate
     * @param streamFps target stream frame rate
     * @param jitterBuffer one of PreferenceConfiguration.JITTER_BUFFER_*
     */
    HostFrameTimeline(float displayRefreshHz, int streamFps, int jitterBuffer) {
        periodNs = displayRefreshHz > 1 ? (long) (1e9 / displayRefreshHz) : 16_666_667L;
        presentLatencyNs = periodNs;
        long streamIntervalNs = 1_000_000_000L / Math.max(streamFps, 1);
        long vsyncs = (streamIntervalNs + periodNs / 2) / periodNs;
        if (vsyncs >= 1 && vsyncs <= 8 && Math.abs(streamIntervalNs - vsyncs * periodNs) * 100 <= periodNs * 15) {
            slotVsyncs = vsyncs;
        }
        pendingJitterBuffer = jitterBuffer;
        applyPendingJitterBuffer();
    }

    /**
     * Switches to another preset mid-stream, from the next frame. The schedule moves to the new
     * preset's as frames arrive.
     * @param jitterBuffer one of PreferenceConfiguration.JITTER_BUFFER_*
     */
    void setJitterBuffer(int jitterBuffer) {
        pendingJitterBuffer = jitterBuffer;
    }

    private static String describeJitterBuffer(int jb) {
        switch (jb) {
            case PreferenceConfiguration.JITTER_BUFFER_BALANCED:
                return "Balanced";
            case PreferenceConfiguration.JITTER_BUFFER_LOWEST_LATENCY:
                return "Lowest latency";
            case PreferenceConfiguration.JITTER_BUFFER_MINIMUM:
                return "Minimum";
            case PreferenceConfiguration.JITTER_BUFFER_LOW_LATENCY:
            default:
                return "Low latency";
        }
    }

    private void applyPendingJitterBuffer() {
        int pending = pendingJitterBuffer;
        if (pending == jitterBuffer) {
            return;
        }
        LimeLog.info("Host frame timing: jitter buffer " + (jitterBuffer < 0 ? "" : "changed to ") +
                describeJitterBuffer(pending));
        jitterBuffer = pending;
        holdSinceNs = 0;
        holdMinRoomNs = 0;
        consecutiveLate = 0;

        if (pending == PreferenceConfiguration.JITTER_BUFFER_BALANCED) {
            headroomNs = 4_000_000L;
            maxWaitVsyncs = 3;
            pullHoldNs = 3_000_000_000L;
            pullMarginDivisor = 16;
            lateStreakToShift = 1;
        }
        else if (pending == PreferenceConfiguration.JITTER_BUFFER_LOWEST_LATENCY) {
            headroomNs = 0;
            maxWaitVsyncs = 2;
            pullHoldNs = 1_000_000_000L;
            pullMarginDivisor = 16;
            lateStreakToShift = 2;
        }
        else if (pending == PreferenceConfiguration.JITTER_BUFFER_MINIMUM) {
            headroomNs = 0;
            maxWaitVsyncs = 1;
            pullHoldNs = 0;
            pullMarginDivisor = 16;
            lateStreakToShift = 1;
        }
        else {
            headroomNs = 0;
            maxWaitVsyncs = 2;
            pullHoldNs = 2_000_000_000L;
            pullMarginDivisor = 16;
            lateStreakToShift = 1;
        }
    }

    /**
     * Records a vsync, from Choreographer: its frame time less the app vsync offset.
     */
    synchronized void onVsync(long vsyncNs) {
        if (lastVsyncNs != 0) {
            if (vsyncNs <= lastVsyncNs) {
                return;
            }
            // Count the vsyncs since the last callback, including any whose callbacks we missed
            vsyncIndex += Math.max(1, (vsyncNs - lastVsyncNs + periodNs / 2) / periodNs);
        }
        lastVsyncNs = vsyncNs;

        int tail = (vsyncHead + vsyncCount) % VSYNC_WINDOW;
        vsyncIndexes[tail] = vsyncIndex;
        vsyncTimes[tail] = vsyncNs;
        if (vsyncCount < VSYNC_WINDOW) {
            vsyncCount++;
        }
        else {
            vsyncHead = (vsyncHead + 1) % VSYNC_WINDOW;
        }

        // The mean period across the window. Its ends are the callbacks' times, which jitter by
        // a fraction of a millisecond: over a few hundred vsyncs, that's parts per million.
        if (vsyncCount >= MIN_VSYNCS_FOR_PERIOD) {
            long vsyncs = vsyncIndexes[tail] - vsyncIndexes[vsyncHead];
            if (vsyncs > 0) {
                long measured = (vsyncTimes[tail] - vsyncTimes[vsyncHead]) / vsyncs;
                if (Math.abs(measured - periodNs) * 10 < periodNs) {
                    periodNs = measured;
                }
            }
        }
    }

    /**
     * Records a decoded frame and returns the System.nanoTime() at which it should be shown,
     * for MediaCodec.releaseOutputBuffer(index, renderTimestampNs).
     */
    synchronized long onFrameDecoded(long hostPtsNs, long nowNs) {
        applyPendingJitterBuffer();

        updateSlot(hostPtsNs);
        long delta = hostPtsNs - lastPtsNs;
        boolean continuous = lastPtsNs != 0 && delta > 0 && delta <= MAX_PTS_GAP_NS;
        lastPtsNs = hostPtsNs;

        if (lastVsyncNs == 0) {
            // No vsyncs yet: as soon as possible
            return nowNs + presentLatencyNs;
        }

        // The earliest the frame could be shown: the first vsync at least the headroom after it
        // was decoded
        long readyNs = nowNs + headroomNs;
        long earliestNs = firstVsyncFrom(readyNs);

        long assignedNs;
        if (!anchored || !continuous) {
            // Start (or start over) at the first vsync the frame can make
            assignedNs = earliestNs;
            lastAssignedNs = earliestNs;
            anchored = true;
            carry = 0;
            holdSinceNs = 0;
            holdMinRoomNs = 0;
            consecutiveLate = 0;
        }
        else {
            // The vsync after the last frame's, as many slots on as the host timestamps moved
            long slotPeriodNs = slotVsyncs * periodNs;
            double slots = carry + (double) delta / slotPeriodNs;
            long step = Math.max(1, Math.round(slots));
            carry = (slots - step) * CARRY_KEEP;
            long plannedNs = nearestVsync(lastAssignedNs + step * slotPeriodNs);

            long roomNs = plannedNs - readyNs;
            long waitForCapNs = roomNs + headroomNs;
            long marginNs = pullMarginDivisor > 0 ? periodNs / pullMarginDivisor : 0;
            if (roomNs < 0) {
                // Too late for its vsync: the first one it can make. If the preset requires
                // multiple consecutive late frames before shifting the schedule (and this frame is
                // only a single vsync late), keep the schedule anchor on plannedNs so the next
                // on-time frame stays on the original cadence.
                assignedNs = Math.max(earliestNs, lastTargetNs);
                lateFrames++;
                consecutiveLate++;
                holdSinceNs = 0;
                holdMinRoomNs = 0;
                if (consecutiveLate >= lateStreakToShift ||
                        earliestNs > plannedNs + periodNs + periodNs / 2) {
                    lastAssignedNs = assignedNs;
                    consecutiveLate = 0;
                }
                else {
                    lastAssignedNs = plannedNs;
                }
            }
            else {
                consecutiveLate = 0;
                assignedNs = plannedNs;
                if (waitForCapNs >= maxWaitVsyncs * periodNs) {
                    // Waiting too long (frames bunched up after a stall, cold start decode, or the host running fast)
                    long excess = waitForCapNs / periodNs - (maxWaitVsyncs - 1);
                    assignedNs = Math.max(Math.max(assignedNs - excess * periodNs, earliestNs), lastTargetNs);
                    pulls++;
                    holdSinceNs = 0;
                    holdMinRoomNs = 0;
                }
                else if (roomNs >= periodNs + marginNs) {
                    // Could have made an earlier vsync. Once every frame could for long enough, move
                    // as many vsyncs sooner as held throughout the window. SurfaceFlinger drops the
                    // frame this one then shares a vsync with.
                    if (holdSinceNs == 0) {
                        holdSinceNs = nowNs;
                        holdMinRoomNs = roomNs;
                    }
                    else {
                        holdMinRoomNs = Math.min(holdMinRoomNs, roomNs);
                        if (nowNs - holdSinceNs >= pullHoldNs) {
                            long pullVsyncs = Math.max(1L, (holdMinRoomNs - marginNs) / periodNs);
                            assignedNs = Math.max(Math.max(assignedNs - pullVsyncs * periodNs, earliestNs), lastTargetNs);
                            pulls++;
                            holdSinceNs = 0;
                            holdMinRoomNs = 0;
                        }
                    }
                }
                else {
                    holdSinceNs = 0;
                    holdMinRoomNs = 0;
                }
                lastAssignedNs = assignedNs;
            }
        }
        lastTargetNs = assignedNs;

        // How long the frame waits for its vsync, headroom included
        long waitNs = assignedNs - nowNs;
        summaryFrames++;
        summaryWaitSumNs += waitNs;
        summaryWaitMaxNs = Math.max(summaryWaitMaxNs, waitNs);
        logSummary(nowNs);

        return assignedNs + presentLatencyNs - periodNs / 2;
    }

    // Every 10 s: frames, the wait for their vsyncs, the late frames and pulls since the last
    // summary (each about one frame held a vsync long or short), the slot, the measured refresh
    // rate and the preset
    private void logSummary(long nowNs) {
        if (summaryStartNs == 0) {
            summaryStartNs = nowNs;
            summaryLateAtStart = lateFrames;
            summaryPullsAtStart = pulls;
            return;
        }
        long elapsedNs = nowNs - summaryStartNs;
        if (elapsedNs < SUMMARY_INTERVAL_NS) {
            return;
        }
        double seconds = elapsedNs / 1e9;
        LimeLog.info(String.format(java.util.Locale.ROOT,
                "Host frame timing: %.0fs: %d frames (%.2f fps), wait mean %.1f max %.1f ms, %d late, %d pulled, " +
                        "%d vsync%s per frame, vsync %.3f Hz, %s",
                seconds, summaryFrames, summaryFrames / seconds,
                summaryFrames > 0 ? summaryWaitSumNs / 1e6 / summaryFrames : 0.0, summaryWaitMaxNs / 1e6,
                lateFrames - summaryLateAtStart, pulls - summaryPullsAtStart,
                slotVsyncs, slotVsyncs == 1 ? "" : "s", 1e9 / periodNs,
                describeJitterBuffer(jitterBuffer)));
        summaryStartNs = nowNs;
        summaryFrames = 0;
        summaryWaitSumNs = 0;
        summaryWaitMaxNs = 0;
        summaryLateAtStart = lateFrames;
        summaryPullsAtStart = pulls;
    }

    // Frames that couldn't make their vsync, moving the schedule a vsync or more later
    synchronized long getLateFrames() {
        return lateFrames;
    }

    // Times the schedule moved sooner
    synchronized long getPulls() {
        return pulls;
    }

    private long nearestVsync(long timeNs) {
        return lastVsyncNs + Math.round((double) (timeNs - lastVsyncNs) / periodNs) * periodNs;
    }

    // The first vsync at or after timeNs, and after the last vsync seen
    private long firstVsyncFrom(long timeNs) {
        if (timeNs <= lastVsyncNs + periodNs) {
            return lastVsyncNs + periodNs;
        }
        return lastVsyncNs + (timeNs - lastVsyncNs + periodNs - 1) / periodNs * periodNs;
    }

    // How many vsyncs each frame should stay on screen, from the spacing of host timestamps: one
    // when it's no whole number of vsyncs
    private void updateSlot(long hostPtsNs) {
        if (lastPtsNs != 0) {
            long delta = hostPtsNs - lastPtsNs;
            if (delta > 0 && delta < MAX_PTS_GAP_NS) {
                ptsDeltas[(ptsDeltaHead + ptsDeltaCount) % SLOT_DELTA_WINDOW] = delta;
                if (ptsDeltaCount < SLOT_DELTA_WINDOW) {
                    ptsDeltaCount++;
                }
                else {
                    ptsDeltaHead = (ptsDeltaHead + 1) % SLOT_DELTA_WINDOW;
                }
            }
        }
        if (ptsDeltaCount < MIN_SLOT_DELTAS) {
            return;
        }
        System.arraycopy(ptsDeltas, 0, sortedDeltas, 0, ptsDeltaCount);
        Arrays.sort(sortedDeltas, 0, ptsDeltaCount);
        long median = sortedDeltas[ptsDeltaCount / 2];
        long vsyncs = (median + periodNs / 2) / periodNs;
        if (vsyncs >= 1 && vsyncs <= 8 && Math.abs(median - vsyncs * periodNs) * 100 <= periodNs * 15) {
            slotVsyncs = vsyncs;
        }
        else {
            // No whole number of vsyncs: chain on the finest grid (see jni/vulkan/frame_pacer.cpp)
            slotVsyncs = 1;
        }
    }
}
