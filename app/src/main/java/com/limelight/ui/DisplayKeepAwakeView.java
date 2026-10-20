package com.limelight.ui;

import android.content.Context;
import android.graphics.Color;
import android.util.AttributeSet;
import android.view.Choreographer;
import android.view.View;

/**
 * Keeps the display at its refresh rate while the stream is drawn into a front buffer. A front
 * buffer never sends the compositor a new frame, so a display that lowers its refresh rate once
 * nothing has changed for a while takes the screen for idle: a Pixel 4 XL dropped from 90 Hz to
 * 60 Hz 80 ms after the last update, though the stream's surface asked for 90, and only went back
 * up while the screen was touched. This pixel changes often enough that the display never sees
 * an idle screen, at a whole fraction of the refresh rate, so the rate the compositor sees it
 * change at divides into the one the stream asked for.
 */
public class DisplayKeepAwakeView extends View implements Choreographer.FrameCallback {
    // Well inside the 80 ms the Pixel 4 XL waited before lowering the refresh rate
    private static final long INTERVAL_NS = 30_000_000;

    // Nearly transparent, and in a corner the screen's rounding usually hides
    private static final int COLOR_A = Color.argb(1, 0, 0, 0);
    private static final int COLOR_B = Color.argb(2, 0, 0, 0);

    private boolean running;
    private boolean colorB;
    private long lastChangeNs;

    public DisplayKeepAwakeView(Context context) {
        super(context);
    }

    public DisplayKeepAwakeView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    /** Starts or stops changing the pixel. Call on the UI thread. */
    public void setRunning(boolean running) {
        if (this.running == running) {
            return;
        }
        this.running = running;
        if (running) {
            lastChangeNs = 0;
            setVisibility(VISIBLE);
            Choreographer.getInstance().postFrameCallback(this);
        }
        else {
            Choreographer.getInstance().removeFrameCallback(this);
            setVisibility(GONE);
        }
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!running) {
            return;
        }
        if (frameTimeNanos - lastChangeNs >= INTERVAL_NS) {
            lastChangeNs = frameTimeNanos;
            colorB = !colorB;
            setBackgroundColor(colorB ? COLOR_B : COLOR_A);
        }
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    protected void onDetachedFromWindow() {
        setRunning(false);
        super.onDetachedFromWindow();
    }
}
