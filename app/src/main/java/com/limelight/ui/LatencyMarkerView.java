package com.limelight.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

import java.util.Locale;

/**
 * Latency test marker, drawn over the stream: a square that flips between black and white on
 * every key or mouse button press sent to the host, for timing against the trigger patch of
 * Sunshine's latency-test app with a camera. Below it, a timer shows the time since the last
 * flip, and the progress of an automatic test.
 *
 * This view only draws the timer. The square is on a LatencyMarkerSurface laid over the stream
 * and drawn the moment the marker flips. Drawn through this view, the UI's own drawing would show
 * each flip a frame or so late; drawn into the stream's frames, it would wait for the next frame
 * and for the pacer to present it. Either makes the measured latency look too short.
 *
 * Same layout as moonlight-qt's OverlayManager::getLatencyMarkerRect(), which mirrors
 * compute_layout() in Sunshine's tools/latency_test/pattern.h: the marker sits on the trigger
 * patch's rows, a quarter of the way across the stream.
 */
public class LatencyMarkerView extends View {
    private final Paint timerBackgroundPaint = new Paint();
    private final Paint timerTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Rect markerRect = new Rect();

    private View streamView;
    private LatencyMarkerSurface markerSurface;

    // Flipped from whichever thread sent the input
    private volatile boolean lit;
    private volatile long flipNanos;
    private volatile int testDone;
    private volatile int testTotal;

    public LatencyMarkerView(Context context) {
        super(context);
        init();
    }

    public LatencyMarkerView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        timerBackgroundPaint.setColor(Color.BLACK);
        timerTextPaint.setColor(Color.WHITE);
        timerTextPaint.setTypeface(Typeface.MONOSPACE);
        setWillNotDraw(false);
    }

    /**
     * The view the stream is drawn in, whose bounds the marker is laid out in, and the surface
     * the square is drawn on. Both are siblings of this view.
     */
    public void setStreamView(View streamView, LatencyMarkerSurface markerSurface) {
        this.streamView = streamView;
        this.markerSurface = markerSurface;
        markerSurface.setVisibility(View.VISIBLE);
        // Not from within the layout pass itself: a size changed there doesn't get measured
        streamView.addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
                post(this::layoutMarkerSurface));
        post(this::layoutMarkerSurface);
    }

    private void layoutMarkerSurface() {
        if (streamView.getWidth() == 0 || streamView.getHeight() == 0) {
            return;
        }

        Rect rect = new Rect();
        getMarkerRect(streamView.getWidth(), streamView.getHeight(), rect);
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) markerSurface.getLayoutParams();
        int left = streamView.getLeft() + rect.left;
        int top = streamView.getTop() + rect.top;
        if (params.width == rect.width() && params.height == rect.height() &&
                params.leftMargin == left && params.topMargin == top) {
            return;
        }
        params.width = rect.width();
        params.height = rect.height();
        params.gravity = Gravity.TOP | Gravity.LEFT;
        params.leftMargin = left;
        params.topMargin = top;
        markerSurface.setLayoutParams(params);
    }

    /** Flips the marker between black and white. Safe to call from any thread. */
    public void flip() {
        flipNanos = System.nanoTime();
        // Synchronized so a stale color from one thread can't land after a newer one from another
        synchronized (this) {
            lit = !lit;
            LatencyMarkerSurface surface = markerSurface;
            if (surface != null) {
                surface.show(lit ? Color.WHITE : Color.BLACK);
            }
        }
        postInvalidateOnAnimation();
    }

    /** Progress of an automatic test shown after the timer, or total 0 for none */
    public void setTestProgress(int done, int total) {
        testDone = done;
        testTotal = total;
        postInvalidateOnAnimation();
    }

    /**
     * Same sizing as compute_layout() in Sunshine's tools/latency_test/pattern.h: cells are an
     * eighth of the short side, on a 64 px grid (16 px on small screens), and the trigger patch
     * is two cells square, centered on the grid.
     */
    private static void getMarkerRect(int width, int height, Rect rect) {
        int rawCell = Math.min(width, height) / 8;
        int grid = rawCell >= 64 ? 64 : 16;
        int cellAlign = Math.max(grid, 32);
        int cell = Math.max(rawCell - rawCell % cellAlign, 32);

        int x = width / 4 - cell;
        int y = height / 2 - cell;
        x = Math.max(x - x % grid, 0);
        y = Math.max(y - y % grid, 0);
        rect.set(x, y, x + 2 * cell, y + 2 * cell);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (streamView == null || streamView.getWidth() == 0 || streamView.getHeight() == 0) {
            return;
        }

        // The stream view is a sibling, possibly letterboxed within our shared parent
        getMarkerRect(streamView.getWidth(), streamView.getHeight(), markerRect);
        markerRect.offset(streamView.getLeft() - getLeft(), streamView.getTop() - getTop());


        String text;
        long flip = flipNanos;
        if (flip == 0) {
            text = "----.- ms";
        }
        else {
            double sinceMs = (System.nanoTime() - flip) / 1000000.0;
            text = String.format(Locale.US, "%6.1f ms", Math.min(sinceMs, 9999.9));
        }
        int total = testTotal;
        String progress = total > 0 ? String.format(Locale.US, "%d/%d", testDone, total) : null;

        // Below the marker, on an opaque background since it sits on the noisy test pattern.
        // Kept within the marker's width, with the test's progress on a second line: on a
        // display not much wider than 16:9 the host's input bit row starts just right of it.
        timerTextPaint.setTextSize(markerRect.height() / 4.0f);
        float fit = markerRect.width() /
                (timerTextPaint.measureText("----.- ms") + timerTextPaint.getTextSize() / 4);
        if (fit < 1) {
            timerTextPaint.setTextSize(timerTextPaint.getTextSize() * fit);
        }
        float padding = timerTextPaint.getTextSize() / 8;
        float lineHeight = timerTextPaint.descent() - timerTextPaint.ascent();
        float left = markerRect.left;
        float top = markerRect.bottom + markerRect.height() / 8.0f;
        float baseline = top + padding - timerTextPaint.ascent();
        canvas.drawRect(left, top, markerRect.right,
                top + 2 * padding + lineHeight * (progress != null ? 2 : 1),
                timerBackgroundPaint);
        canvas.drawText(text, left + padding, baseline, timerTextPaint);
        if (progress != null) {
            canvas.drawText(progress, left + padding, baseline + lineHeight, timerTextPaint);
        }

        // Keep the timer running every frame
        postInvalidateOnAnimation();
    }
}
