package com.limelight.ui;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Handler;
import android.view.View;
import android.view.WindowManager;
import android.widget.SeekBar;
import android.widget.TextView;

import com.limelight.R;

public class BrightnessSliderView {
    private static final String PREFS_NAME = "GameSettings";
    private static final String BRIGHTNESS_KEY = "streamBrightness";
    private static final int AUTO_HIDE_DELAY_MS = 2000;
    private static final int SHOW_DELAY_MS = 3000;
    private static final float TOUCH_ZONE_WIDTH_DP = 80;

    private final Activity activity;
    private final View brightnessSliderContainer;
    private final SeekBar brightnessSlider;
    private final TextView brightnessValueText;
    private final Handler handler = new Handler();
    private final Runnable hideRunnable;
    private final SharedPreferences prefs;
    private final float touchZoneWidth;
    private boolean initialized = false;

    public BrightnessSliderView(Activity activity) {
        this.activity = activity;
        this.brightnessSliderContainer = activity.findViewById(R.id.brightnessSliderContainer);
        this.brightnessSlider = activity.findViewById(R.id.brightnessSlider);
        this.brightnessValueText = activity.findViewById(R.id.brightnessValueText);
        this.prefs = activity.getSharedPreferences(PREFS_NAME, Activity.MODE_PRIVATE);

        // Cache the touch zone width calculation
        float density = activity.getResources().getDisplayMetrics().density;
        this.touchZoneWidth = TOUCH_ZONE_WIDTH_DP * density;

        // Set up the auto-hide runnable
        hideRunnable = new Runnable() {
            @Override
            public void run() {
                hide();
            }
        };

        setupBrightnessSlider();
    }

    private void setupBrightnessSlider() {
        // Get saved brightness from SharedPreferences (default to 0 = auto)
        float savedBrightness = prefs.getFloat(BRIGHTNESS_KEY, -1.0f);

        // Apply the saved brightness to the window
        WindowManager.LayoutParams layoutParams = activity.getWindow().getAttributes();
        if (savedBrightness < 0) {
            // Auto mode: use system brightness
            layoutParams.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
            brightnessSlider.setProgress(0);
        } else {
            layoutParams.screenBrightness = savedBrightness;
            brightnessSlider.setProgress((int)(savedBrightness * 100));
        }
        activity.getWindow().setAttributes(layoutParams);

        // Handle slider changes
        brightnessSlider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser) {
                    WindowManager.LayoutParams params = activity.getWindow().getAttributes();

                    if (progress == 0) {
                        // Auto mode: use system brightness
                        params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
                        prefs.edit().putFloat(BRIGHTNESS_KEY, -1.0f).apply();
                    } else {
                        // Manual brightness (1-100 maps to 0.01-1.0)
                        float brightness = progress / 100.0f;
                        params.screenBrightness = brightness;
                        prefs.edit().putFloat(BRIGHTNESS_KEY, brightness).apply();
                    }

                    activity.getWindow().setAttributes(params);
                }

                // Update brightness text and position (for both user and programmatic changes)
                if (progress == 0) {
                    brightnessValueText.setText("Auto");
                } else {
                    brightnessValueText.setText(progress + "%");
                }
                updateBrightnessTextPosition(progress);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
                // Cancel any pending hide operations
                handler.removeCallbacks(hideRunnable);
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                // Hide the slider after delay
                handler.postDelayed(hideRunnable, AUTO_HIDE_DELAY_MS);
            }
        });
    }

    private void updateBrightnessTextPosition(int progress) {
        // Calculate the vertical position based on slider progress
        // Since the slider is rotated 270 degrees, we need to calculate position differently
        brightnessSliderContainer.post(new Runnable() {
            @Override
            public void run() {
                int containerHeight = brightnessSliderContainer.getHeight();
                int textHeight = brightnessValueText.getHeight();

                // Calculate position: progress 0 = bottom, progress 100 = top
                // We need to invert this because of the 270 degree rotation
                float percentage = progress / 100.0f;
                int yPosition = (int)(containerHeight * (1.0f - percentage));

                // Adjust for text height to center it on the thumb
                yPosition -= textHeight / 2;

                // Clamp the position to keep text within container bounds
                int minY = 0;
                int maxY = containerHeight - textHeight;
                yPosition = Math.max(minY, Math.min(maxY, yPosition));

                // Update the Y position (relative to parent container)
                brightnessValueText.setY(yPosition);
            }
        });
    }

    /**
     * Check if a touch event is in the brightness slider activation zone
     */
    public boolean isTouchInActivationZone(float touchX) {
        return touchX < touchZoneWidth;
    }

    /**
     * Show the brightness slider
     */
    public void show() {
        if (brightnessSliderContainer.getVisibility() != View.VISIBLE) {
            // Set SeekBar width to match container height (only need to do this once)
            if (!initialized) {
                // Get the saved brightness value before adjusting width
                final float savedBrightness = prefs.getFloat(BRIGHTNESS_KEY, -1.0f);
                final int savedProgress = savedBrightness < 0 ? 0 : (int)(savedBrightness * 100);

                // Make invisible while we adjust the layout
                brightnessSliderContainer.setVisibility(View.INVISIBLE);
                brightnessValueText.setVisibility(View.INVISIBLE);

                brightnessSliderContainer.post(new Runnable() {
                    @Override
                    public void run() {
                        int containerHeight = brightnessSliderContainer.getHeight();
                        if (containerHeight > 0) {
                            android.view.ViewGroup.LayoutParams params = brightnessSlider.getLayoutParams();
                            params.width = containerHeight;
                            brightnessSlider.setLayoutParams(params);

                            // Re-apply the saved progress after layout change
                            brightnessSlider.setProgress(savedProgress);

                            initialized = true;

                            // Update the initial text position
                            updateBrightnessTextPosition(brightnessSlider.getProgress());

                            // Now make it visible with correct values
                            brightnessSliderContainer.setVisibility(View.VISIBLE);
                            brightnessValueText.setVisibility(View.VISIBLE);
                        }
                    }
                });
            } else {
                // Already initialized, just show it
                brightnessSliderContainer.setVisibility(View.VISIBLE);
                brightnessValueText.setVisibility(View.VISIBLE);

                // Update text position if already initialized
                updateBrightnessTextPosition(brightnessSlider.getProgress());
            }
        }

        // Cancel any pending hide operations
        handler.removeCallbacks(hideRunnable);

        // Schedule auto-hide
        handler.postDelayed(hideRunnable, SHOW_DELAY_MS);
    }

    /**
     * Hide the brightness slider
     */
    public void hide() {
        brightnessSliderContainer.setVisibility(View.GONE);
        brightnessValueText.setVisibility(View.GONE);
    }

    /**
     * Cleanup - call from onDestroy
     */
    public void cleanup() {
        if (handler != null && hideRunnable != null) {
            handler.removeCallbacks(hideRunnable);
        }
    }
}
