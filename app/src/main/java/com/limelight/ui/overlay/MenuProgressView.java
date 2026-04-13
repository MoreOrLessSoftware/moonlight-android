package com.limelight.ui.overlay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

import com.limelight.R;

/**
 * Custom view that displays the Moonlight app icon with a horizontal progress bar.
 * Used to show progress while holding the select button to open the overlay menu.
 */
public class MenuProgressView extends View {
    private static final int ICON_SIZE_DP = 48;
    private static final int PROGRESS_BAR_HEIGHT_DP = 4;

    private Paint progressPaint;
    private RectF progressBounds;
    private Drawable appIcon;
    private float progress = 0.0f;
    private int iconSize;
    private int progressBarHeight;

    public MenuProgressView(Context context) {
        super(context);
        init(context);
    }

    public MenuProgressView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context);
    }

    public MenuProgressView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init(context);
    }

    private void init(Context context) {
        // Convert DP to pixels
        float density = context.getResources().getDisplayMetrics().density;
        iconSize = (int) (ICON_SIZE_DP * density);
        progressBarHeight = (int) (PROGRESS_BAR_HEIGHT_DP * density);

        // Setup progress bar paint
        progressPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        progressPaint.setStyle(Paint.Style.FILL);
        progressPaint.setColor(0xFFFFFFFF); // White

        // Load app icon
        appIcon = context.getResources().getDrawable(R.mipmap.ic_launcher);

        // Initialize progress bounds
        progressBounds = new RectF();

        setVisibility(GONE);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        updateBounds();
    }

    private void updateBounds() {
        int width = getWidth();
        int height = getHeight();

        if (width == 0 || height == 0) {
            return;
        }

        // Center the icon
        int centerX = width / 2;
        int centerY = height / 2;

        // Set icon bounds
        if (appIcon != null) {
            int iconLeft = centerX - (iconSize / 2);
            int iconTop = centerY - (iconSize / 2);
            appIcon.setBounds(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize);

            // Set progress bar bounds - horizontal bar at bottom of icon
            int barLeft = iconLeft;
            int barTop = iconTop + iconSize - progressBarHeight;
            int barRight = iconLeft + iconSize;
            int barBottom = iconTop + iconSize;

            progressBounds.set(barLeft, barTop, barRight, barBottom);
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int size = iconSize + 20; // Icon size plus small padding
        setMeasuredDimension(size, size);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        if (appIcon == null) {
            return;
        }

        // Draw app icon
        appIcon.draw(canvas);

        // Draw horizontal progress bar at bottom of icon
        if (progress > 0) {
            float barWidth = progressBounds.width() * progress;
            canvas.drawRect(
                progressBounds.left,
                progressBounds.top,
                progressBounds.left + barWidth,
                progressBounds.bottom,
                progressPaint
            );
        }
    }

    /**
     * Update the progress value (0.0 to 1.0)
     */
    public void setProgress(float progress) {
        this.progress = Math.max(0.0f, Math.min(1.0f, progress));
        invalidate();
    }

    /**
     * Get the current progress value
     */
    public float getProgress() {
        return progress;
    }

    /**
     * Show the progress view
     */
    public void show() {
        setVisibility(VISIBLE);
        setProgress(0.0f);
    }

    /**
     * Hide the progress view
     */
    public void hide() {
        setVisibility(GONE);
        setProgress(0.0f);
    }
}
