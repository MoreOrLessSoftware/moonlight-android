package com.limelight.ui;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
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
    // Brightness steps for each d-pad press or stick repeat, in percent (touch moves by 1%)
    private static final int CONTROLLER_STEP = 5;
    private static final float STICK_THRESHOLD = 0.5f;
    private static final long STICK_REPEAT_MS = 150;

    private final Activity activity;
    private final View brightnessSliderContainer;
    private final SeekBar brightnessSlider;
    private final TextView brightnessValueText;
    private final Handler handler = new Handler();
    private final Runnable hideRunnable;
    private final SharedPreferences prefs;
    private final float touchZoneWidth;
    private boolean initialized = false;
    // Up for a gamepad or remote (showForController())
    private boolean controllerMode = false;
    private Runnable onControllerClosed;
    // The key that closed it, whose release is taken too
    private int closingKeyCode = KeyEvent.KEYCODE_UNKNOWN;
    private long lastStickStepMs = 0;

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
                    applyBrightness(progress);
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
                // Hide the slider after delay, unless a controller is driving it
                if (!controllerMode) {
                    handler.postDelayed(hideRunnable, AUTO_HIDE_DELAY_MS);
                }
            }
        });

        // With the slider focused for a controller, its keys and stick come here rather than
        // going to the stream
        brightnessSlider.setOnKeyListener((v, keyCode, event) -> handleKeyEvent(event));
        brightnessSlider.setOnGenericMotionListener((v, event) -> handleMotionEvent(event));
    }

    private void applyBrightness(int progress) {
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

    // Moves the brightness to the next multiple of CONTROLLER_STEP up or down, between Auto (0)
    // and 100%, so a level set by touch (1% steps) snaps onto the controller's steps
    private void stepBrightness(int direction) {
        int current = brightnessSlider.getProgress();
        int snapped = direction > 0 ? (current / CONTROLLER_STEP + 1) * CONTROLLER_STEP
                                    : ((current + CONTROLLER_STEP - 1) / CONTROLLER_STEP - 1) * CONTROLLER_STEP;
        int progress = Math.max(0, Math.min(100, snapped));
        if (progress != brightnessSlider.getProgress()) {
            brightnessSlider.setProgress(progress);
            applyBrightness(progress);
        }
    }

    /**
     * Shows the slider for a gamepad or remote: up and down change the brightness, and A, B or
     * back close it. It stays up until then.
     *
     * @param onClosed run once it's closed, to give focus back to the stream
     */
    public void showForController(Runnable onClosed) {
        controllerMode = true;
        onControllerClosed = onClosed;
        show();
    }

    /** Whether the slider is up for a controller (showForController()) */
    public boolean isControllerMode() {
        return controllerMode;
    }

    private void focusForController() {
        if (controllerMode) {
            brightnessSlider.setFocusableInTouchMode(true);
            brightnessSlider.requestFocus();
        }
    }

    private void closeForController() {
        hide();
    }

    /**
     * Key events while the slider is up for a controller. Takes every one, so none reach the
     * stream; the release of the key that closed it is taken too.
     */
    public boolean handleKeyEvent(KeyEvent event) {
        if (!controllerMode) {
            if (event.getAction() == KeyEvent.ACTION_UP && event.getKeyCode() == closingKeyCode) {
                closingKeyCode = KeyEvent.KEYCODE_UNKNOWN;
                return true;
            }
            return false;
        }

        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_DPAD_UP:
                    stepBrightness(1);
                    break;
                case KeyEvent.KEYCODE_DPAD_DOWN:
                    stepBrightness(-1);
                    break;
                case KeyEvent.KEYCODE_BUTTON_A:
                case KeyEvent.KEYCODE_BUTTON_B:
                case KeyEvent.KEYCODE_DPAD_CENTER:
                case KeyEvent.KEYCODE_ENTER:
                case KeyEvent.KEYCODE_BACK:
                    if (event.getRepeatCount() == 0) {
                        closingKeyCode = event.getKeyCode();
                        closeForController();
                    }
                    break;
            }
        }
        return true;
    }

    /** Stick and d-pad hat motion while the slider is up for a controller */
    public boolean handleMotionEvent(MotionEvent event) {
        if (!controllerMode) {
            return false;
        }
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            float y = event.getAxisValue(MotionEvent.AXIS_Y);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            float combinedY = Math.abs(y) > Math.abs(hatY) ? y : hatY;

            // Held, it repeats at STICK_REPEAT_MS; let go, the next push steps at once
            long now = SystemClock.uptimeMillis();
            if (Math.abs(combinedY) < STICK_THRESHOLD) {
                lastStickStepMs = 0;
            }
            else if (lastStickStepMs == 0 || now - lastStickStepMs >= STICK_REPEAT_MS) {
                // Stick and hat up are negative
                stepBrightness(combinedY < 0 ? 1 : -1);
                lastStickStepMs = now;
            }
        }
        return true;
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
                            focusForController();
                        }
                    }
                });
            } else {
                // Already initialized, just show it
                brightnessSliderContainer.setVisibility(View.VISIBLE);
                brightnessValueText.setVisibility(View.VISIBLE);

                // Update text position if already initialized
                updateBrightnessTextPosition(brightnessSlider.getProgress());
                focusForController();
            }
        }
        else {
            focusForController();
        }

        // Cancel any pending hide operations
        handler.removeCallbacks(hideRunnable);

        // Schedule auto-hide, unless a controller is driving it
        if (!controllerMode) {
            handler.postDelayed(hideRunnable, SHOW_DELAY_MS);
        }
    }

    /**
     * Hide the brightness slider
     */
    public void hide() {
        handler.removeCallbacks(hideRunnable);
        brightnessSliderContainer.setVisibility(View.GONE);
        brightnessValueText.setVisibility(View.GONE);

        if (controllerMode) {
            controllerMode = false;
            lastStickStepMs = 0;
            brightnessSlider.clearFocus();
            brightnessSlider.setFocusableInTouchMode(false);
            Runnable onClosed = onControllerClosed;
            onControllerClosed = null;
            if (onClosed != null) {
                onClosed.run();
            }
        }
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
