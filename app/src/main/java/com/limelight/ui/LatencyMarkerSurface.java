package com.limelight.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.util.AttributeSet;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * The latency test marker's square on its own surface, laid over the stream by
 * LatencyMarkerView. It's drawn the moment the marker flips, on the thread that sent the input,
 * so it reaches the compositor like a video frame does rather than waiting for the next UI frame
 * to be drawn. A marker drawn late makes the measured latency look too short.
 */
public class LatencyMarkerSurface extends SurfaceView implements SurfaceHolder.Callback {
    // A buffer this small is filled in no time; the compositor scales it up
    private static final int BUFFER_SIZE = 16;

    private final Object lock = new Object();
    private boolean surfaceValid;
    private int color = Color.BLACK;

    public LatencyMarkerSurface(Context context) {
        super(context);
        init();
    }

    public LatencyMarkerSurface(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        // Above the stream's own surface, below the UI
        setZOrderMediaOverlay(true);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        getHolder().setFixedSize(BUFFER_SIZE, BUFFER_SIZE);
        getHolder().addCallback(this);
    }

    /** Shows the square in this color right away. Safe to call from any thread. */
    public void show(int color) {
        synchronized (lock) {
            this.color = color;
            draw();
        }
    }

    // Called with the lock held
    private void draw() {
        if (!surfaceValid) {
            return;
        }

        Canvas canvas = getHolder().lockCanvas();
        if (canvas == null) {
            return;
        }
        try {
            canvas.drawColor(color, PorterDuff.Mode.SRC);
        } finally {
            getHolder().unlockCanvasAndPost(canvas);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        synchronized (lock) {
            surfaceValid = true;
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        synchronized (lock) {
            draw();
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Waits for a draw in progress on another thread
        synchronized (lock) {
            surfaceValid = false;
        }
    }
}
