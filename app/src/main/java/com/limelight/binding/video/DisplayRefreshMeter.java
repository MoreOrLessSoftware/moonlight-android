package com.limelight.binding.video;

import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;
import android.view.Display;

import com.limelight.LimeLog;

import java.util.ArrayList;

/**
 * Measures the display's actual refresh rate from vsync times.
 *
 * Android reports the nominal rate of the display mode (144 Hz), while the panel may run a
 * little off it (143.65 Hz on an iPlay 70 mini Ultra). A host that paces its frames to the
 * nominal rate drifts against our vsyncs, costing a repeated or skipped frame every few
 * seconds. Vsync timestamps are exact enough that half a second of them gives the rate to
 * well under 0.001 Hz.
 *
 * Runs on the main thread. The display mode may still be switching when the stream starts,
 * so a measurement that doesn't fit a steady rate is retried.
 */
public class DisplayRefreshMeter implements Choreographer.FrameCallback {
    public interface Listener {
        // Measured refresh rate, or 0 if it couldn't be measured
        void onMeasured(double refreshHz);
    }

    private static final long MEASURE_NS = 500_000_000L;
    // Frames right after starting can be delivered late while the main thread is busy
    private static final int SKIP_FRAMES = 5;
    private static final int MAX_ATTEMPTS = 4;
    private static final long TIMEOUT_MS = 3000;

    private final Display display;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ArrayList<Long> frameTimes = new ArrayList<>();
    private float nominalHz;
    private int skipped;
    private int attempts;
    private boolean done;

    private DisplayRefreshMeter(Display display, Listener listener) {
        this.display = display;
        this.listener = listener;
    }

    public static DisplayRefreshMeter measure(Display display, Listener listener) {
        DisplayRefreshMeter meter = new DisplayRefreshMeter(display, listener);
        meter.startAttempt();
        meter.handler.postDelayed(() -> {
            if (!meter.done) {
                LimeLog.warning("Display refresh measurement timed out");
                meter.finish(0);
            }
        }, TIMEOUT_MS);
        return meter;
    }

    // Stops the measurement without calling the listener
    public void cancel() {
        done = true;
        Choreographer.getInstance().removeFrameCallback(this);
        handler.removeCallbacksAndMessages(null);
    }

    /**
     * The frame rate for the host to run at so that its frames line up with our vsyncs: the
     * refresh rate divided by the whole number of vsyncs each frame is shown for. Returns 0
     * when the stream's frame rate doesn't divide the refresh rate (60 fps at 144 Hz), since
     * no host rate lines up with our vsyncs then.
     */
    public static double streamMatchedRate(double refreshHz, int streamFps) {
        if (refreshHz <= 0 || streamFps <= 0) {
            return 0;
        }
        long vsyncsPerFrame = Math.round(refreshHz / streamFps);
        if (vsyncsPerFrame < 1) {
            return 0;
        }
        double rate = refreshHz / vsyncsPerFrame;
        return Math.abs(rate - streamFps) <= streamFps * 0.02 ? rate : 0;
    }

    private void startAttempt() {
        attempts++;
        frameTimes.clear();
        skipped = 0;
        nominalHz = display.getRefreshRate();
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (done) {
            return;
        }
        if (skipped < SKIP_FRAMES) {
            skipped++;
        }
        else {
            frameTimes.add(frameTimeNanos);
        }

        if (frameTimes.size() < 2 || frameTimes.get(frameTimes.size() - 1) - frameTimes.get(0) < MEASURE_NS) {
            Choreographer.getInstance().postFrameCallback(this);
            return;
        }

        double hz = fit();
        if (hz > 0) {
            finish(hz);
        }
        else if (attempts < MAX_ATTEMPTS) {
            startAttempt();
        }
        else {
            LimeLog.warning("Display refresh rate didn't settle; not measured");
            finish(0);
        }
    }

    // Least squares fit of frame time against vsync index. Returns 0 if the times don't fit
    // one steady rate near the display's nominal one (the mode changed while measuring).
    private double fit() {
        if (nominalHz <= 0 || Math.abs(display.getRefreshRate() - nominalHz) > nominalHz * 0.01) {
            return 0;
        }
        final double nominalPeriod = 1e9 / nominalHz;
        final long first = frameTimes.get(0);
        final int n = frameTimes.size();
        long[] index = new long[n];
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; i++) {
            // Frames the main thread missed leave gaps of whole vsyncs
            double y = frameTimes.get(i) - first;
            index[i] = Math.round(y / nominalPeriod);
            sx += index[i];
            sy += y;
            sxx += (double) index[i] * index[i];
            sxy += index[i] * y;
        }
        double denominator = n * sxx - sx * sx;
        if (denominator <= 0) {
            return 0;
        }
        double period = (n * sxy - sx * sy) / denominator;
        double intercept = (sy - period * sx) / n;
        for (int i = 0; i < n; i++) {
            double residual = (frameTimes.get(i) - first) - (intercept + period * index[i]);
            if (Math.abs(residual) > period / 4) {
                return 0;
            }
        }
        double hz = 1e9 / period;
        return Math.abs(hz - nominalHz) <= nominalHz * 0.01 ? hz : 0;
    }

    private void finish(double hz) {
        if (done) {
            return;
        }
        cancel();
        if (hz > 0) {
            LimeLog.info(String.format(java.util.Locale.ROOT, "Measured display refresh rate: %.4f Hz (nominal %.3f Hz)", hz, nominalHz));
        }
        listener.onMeasured(hz);
    }
}
