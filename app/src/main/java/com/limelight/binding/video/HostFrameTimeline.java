package com.limelight.binding.video;

import java.util.Arrays;

/**
 * Schedules frames for the direct (MediaCodec to SurfaceView) renderer in the "sync to host
 * frame timing" pacing mode. The Vulkan renderer does the same natively, with vsync phase
 * locking on top (see jni/vulkan/frame_pacer.h).
 *
 * Sunshine captures each frame as soon as the host desktop presents it and stamps it with
 * that present time, so the spacing of frame timestamps is the host display's real cadence.
 * The time from a frame's timestamp to its decode here (transit) is only encode, network and
 * decode delay. Scheduling each frame at timestamp + offset, where the offset covers nearly
 * every observed transit time, recreates the host's cadence with a constant delay. When the
 * host display runs slightly fast, the surplus frames get dropped one at a time instead of
 * building up a queue.
 */
class HostFrameTimeline {
    private static final long WINDOW_NS = 2_000_000_000L;
    private static final double COVERAGE = 0.95;
    private static final long MAX_PTS_BACKSTEP_NS = 100_000_000L;
    private static final long MAX_PTS_GAP_NS = 5_000_000_000L;
    private static final int CAPACITY = 1024;

    // Ring buffer of recent samples
    private final long[] arrivalNs = new long[CAPACITY];
    private final long[] transitNs = new long[CAPACITY];
    private final long[] scratch = new long[CAPACITY];
    private int head;
    private int count;

    private long offsetNs;
    private long lastPtsNs;
    private final long presentLatencyNs;

    /**
     * @param displayRefreshHz current display refresh rate. SurfaceFlinger shows a buffer at
     *                         the first vsync whose expected present time is at or after the
     *                         buffer's timestamp, which is about a frame after we release it,
     *                         so targets are pushed out by one refresh period.
     */
    HostFrameTimeline(float displayRefreshHz) {
        presentLatencyNs = displayRefreshHz > 1 ? (long) (1e9 / displayRefreshHz) : 16_666_667L;
    }

    /**
     * Records a decoded frame and returns the System.nanoTime() at which it should be shown,
     * for MediaCodec.releaseOutputBuffer(index, renderTimestampNs).
     */
    long onFrameDecoded(long hostPtsNs, long nowNs) {
        if (count > 0 && (hostPtsNs < lastPtsNs - MAX_PTS_BACKSTEP_NS || hostPtsNs > lastPtsNs + MAX_PTS_GAP_NS)) {
            // The stream restarted
            count = 0;
        }
        lastPtsNs = hostPtsNs;

        boolean first = count == 0;
        int tail = (head + count) % CAPACITY;
        arrivalNs[tail] = nowNs;
        transitNs[tail] = nowNs - hostPtsNs;
        if (count < CAPACITY) {
            count++;
        }
        else {
            head = (head + 1) % CAPACITY;
        }
        while (count > 1 && arrivalNs[head] < nowNs - WINDOW_NS) {
            head = (head + 1) % CAPACITY;
            count--;
        }

        for (int i = 0; i < count; i++) {
            scratch[i] = transitNs[(head + i) % CAPACITY];
        }
        Arrays.sort(scratch, 0, count);
        int index = count - 1;
        if (count >= 8) {
            index = (int) Math.ceil(COVERAGE * (count - 1));
        }
        long target = scratch[index];

        // Take more delay right away when frames start arriving later, and give it back slowly
        if (first || target > offsetNs) {
            offsetNs = target;
        }
        else {
            offsetNs -= (offsetNs - target) / 32;
        }

        return hostPtsNs + offsetNs + presentLatencyNs;
    }
}
