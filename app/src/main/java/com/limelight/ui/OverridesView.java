package com.limelight.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.R;

public class OverridesView {
    public static final String PREF_BITRATE_OVERRIDE = "bitrate_override";
    public static final String PREF_PERF_OVERLAY_OVERRIDE = "perf_overlay_override";
    public static final String PREF_OVERRIDES_ENABLED = "overrides_enabled";
    private static final int BITRATE_STEP = 5000; // 5 Mbps steps in kbps
    private static final int SEEKBAR_MAX = 60; // 60 steps * 5 Mbps = 300 Mbps max

    private final Activity activity;
    private final LinearLayout overridesSection;
    private final SeekBar bitrateOverrideSeekBar;
    private final TextView bitrateOverrideValue;
    private final View perfOverlayButton;
    private final ImageView perfOverlayIcon;
    private final TextView perfOverlayLabel;
    private ImageView overridesToggleButton;
    private final SharedPreferences preferences;

    public OverridesView(Activity activity) {
        this.activity = activity;
        this.preferences = PreferenceManager.getDefaultSharedPreferences(activity);

        // Find views
        this.overridesSection = activity.findViewById(R.id.overridesSection);
        this.bitrateOverrideSeekBar = activity.findViewById(R.id.bitrateOverrideSeekBar);
        this.bitrateOverrideValue = activity.findViewById(R.id.bitrateOverrideValue);
        this.perfOverlayButton = activity.findViewById(R.id.perfOverlayButton);
        this.perfOverlayIcon = activity.findViewById(R.id.perfOverlayIcon);
        this.perfOverlayLabel = activity.findViewById(R.id.perfOverlayLabel);

        initializeBitrateOverride();
        initializePerfOverlayButton();

        // Set initial visibility based on preference (default to disabled/hidden)
        updateOverridesSectionVisibility();
    }

    public void toggleOverridesEnabled() {
        boolean currentlyEnabled = preferences.getBoolean(PREF_OVERRIDES_ENABLED, false);
        boolean newEnabled = !currentlyEnabled;

        preferences.edit()
            .putBoolean(PREF_OVERRIDES_ENABLED, newEnabled)
            .apply();

        updateOverridesSectionVisibility();
    }

    private void updateOverridesSectionVisibility() {
        if (overridesSection == null) {
            return;
        }

        boolean enabled = preferences.getBoolean(PREF_OVERRIDES_ENABLED, false);
        overridesSection.setVisibility(enabled ? View.VISIBLE : View.GONE);
    }

    public boolean isOverridesEnabled() {
        return preferences.getBoolean(PREF_OVERRIDES_ENABLED, false);
    }

    public void setupToggleButton(ImageView button) {
        this.overridesToggleButton = button;
        if (overridesToggleButton != null) {
            overridesToggleButton.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    toggleOverridesEnabled();
                    updateToggleButtonIcon();
                }
            });
            updateToggleButtonIcon();
        }
    }

    private void updateToggleButtonIcon() {
        if (overridesToggleButton == null) {
            return;
        }

        boolean enabled = preferences.getBoolean(PREF_OVERRIDES_ENABLED, false);
        if (enabled) {
            overridesToggleButton.setImageResource(R.drawable.ic_overrides_enabled);
        } else {
            overridesToggleButton.setImageResource(R.drawable.ic_overrides);
        }
    }

    private void initializeBitrateOverride() {
        if (bitrateOverrideSeekBar == null || bitrateOverrideValue == null) {
            return;
        }

        // Set up seekbar range
        bitrateOverrideSeekBar.setMax(SEEKBAR_MAX);

        // Load saved value (0 means "Use Default")
        int savedBitrate = preferences.getInt(PREF_BITRATE_OVERRIDE, 0);

        // Convert bitrate to seekbar position
        // Position 0 = "Use Default"
        // Position 1+ = (position * BITRATE_STEP) kbps
        int seekBarPosition = 0;
        if (savedBitrate > 0) {
            seekBarPosition = savedBitrate / BITRATE_STEP;
        }

        bitrateOverrideSeekBar.setProgress(seekBarPosition);
        updateBitrateLabel(seekBarPosition);

        // Set up click listener on label to show custom input dialog
        bitrateOverrideValue.setClickable(true);
        bitrateOverrideValue.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showBitrateInputDialog();
            }
        });

        // Set up seekbar listener
        bitrateOverrideSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                updateBitrateLabel(progress);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
                // Not needed
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                // Save the value when user stops dragging
                int progress = seekBar.getProgress();
                int bitrateKbps = progress == 0 ? 0 : progress * BITRATE_STEP;

                preferences.edit()
                    .putInt(PREF_BITRATE_OVERRIDE, bitrateKbps)
                    .apply();
            }
        });

        // Handle gamepad/keyboard navigation to use 5 Mbps steps
        bitrateOverrideSeekBar.setOnKeyListener(new View.OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) {
                    int currentProgress = bitrateOverrideSeekBar.getProgress();
                    int newProgress = currentProgress;

                    if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_MINUS) {
                        // Decrease by 1 step (5 Mbps)
                        newProgress = Math.max(0, currentProgress - 1);
                    } else if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT || keyCode == KeyEvent.KEYCODE_PLUS || keyCode == KeyEvent.KEYCODE_EQUALS) {
                        // Increase by 1 step (5 Mbps)
                        newProgress = Math.min(SEEKBAR_MAX, currentProgress + 1);
                    } else {
                        return false; // Let other keys be handled normally
                    }

                    if (newProgress != currentProgress) {
                        bitrateOverrideSeekBar.setProgress(newProgress);
                        updateBitrateLabel(newProgress);

                        // Save the value immediately
                        int bitrateKbps = newProgress == 0 ? 0 : newProgress * BITRATE_STEP;
                        preferences.edit()
                            .putInt(PREF_BITRATE_OVERRIDE, bitrateKbps)
                            .apply();

                        return true; // Event handled
                    }
                }
                return false;
            }
        });
    }

    private void updateBitrateLabel(int seekBarPosition) {
        if (bitrateOverrideValue == null) {
            return;
        }

        if (seekBarPosition == 0) {
            bitrateOverrideValue.setText(R.string.bitrate_use_default);
        } else {
            int bitrateMbps = seekBarPosition * 5; // Each position is 5 Mbps
            bitrateOverrideValue.setText(String.format("Bitrate: %d Mbps", bitrateMbps));
        }
    }

    private void showBitrateInputDialog() {
        AlertDialog.Builder builder = new AlertDialog.Builder(activity);
        builder.setTitle("Set Bitrate Override");
        builder.setMessage("Enter bitrate in Mbps (0 for default, max 500)");

        final EditText input = new EditText(activity);
        input.setInputType(InputType.TYPE_CLASS_NUMBER);

        // Pre-fill with current value
        int currentBitrate = preferences.getInt(PREF_BITRATE_OVERRIDE, 0);
        if (currentBitrate > 0) {
            input.setText(String.valueOf(currentBitrate / 1000)); // Convert kbps to Mbps
        }

        // Wrap EditText in a container with padding
        LinearLayout container = new LinearLayout(activity);
        container.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (20 * activity.getResources().getDisplayMetrics().density);
        container.setPadding(padding, 0, padding, 0);
        container.addView(input);

        builder.setView(container);

        builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                String text = input.getText().toString().trim();
                if (text.isEmpty()) {
                    return;
                }

                try {
                    int bitrateMbps = Integer.parseInt(text);

                    // Validate range
                    if (bitrateMbps < 0) {
                        bitrateMbps = 0;
                    } else if (bitrateMbps > 500) {
                        Toast.makeText(activity, "Maximum bitrate is 500 Mbps", Toast.LENGTH_SHORT).show();
                        bitrateMbps = 500;
                    }

                    // Convert to kbps and save
                    int bitrateKbps = bitrateMbps * 1000;
                    preferences.edit()
                        .putInt(PREF_BITRATE_OVERRIDE, bitrateKbps)
                        .apply();

                    // Update seekbar to closest position
                    int seekBarPosition = bitrateKbps > 0 ? bitrateKbps / BITRATE_STEP : 0;
                    if (bitrateOverrideSeekBar != null) {
                        bitrateOverrideSeekBar.setProgress(seekBarPosition);
                    }
                    updateBitrateLabel(seekBarPosition);

                } catch (NumberFormatException e) {
                    Toast.makeText(activity, "Invalid number", Toast.LENGTH_SHORT).show();
                }
            }
        });

        builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                dialog.cancel();
            }
        });

        builder.show();
    }

    /**
     * Get the current bitrate override value in kbps
     * @return bitrate in kbps, or 0 if "Use Default" is selected
     */
    public static int getBitrateOverride(Activity activity) {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity);
        return prefs.getInt(PREF_BITRATE_OVERRIDE, 0);
    }

    private void initializePerfOverlayButton() {
        if (perfOverlayButton == null || perfOverlayIcon == null || perfOverlayLabel == null) {
            return;
        }

        // Set up click listener
        perfOverlayButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                togglePerfOverlay();
            }
        });

        // Update initial state
        updatePerfOverlayButton();
    }

    private void togglePerfOverlay() {
        // Performance overlay override cycles through: 0 (default) -> 1 (force on) -> 2 (force off) -> 0 (default)
        int currentOverride = preferences.getInt(PREF_PERF_OVERLAY_OVERRIDE, 0);
        int newOverride = (currentOverride + 1) % 3;

        preferences.edit()
            .putInt(PREF_PERF_OVERLAY_OVERRIDE, newOverride)
            .apply();

        updatePerfOverlayButton();
    }

    private void updatePerfOverlayButton() {
        if (perfOverlayIcon == null || perfOverlayLabel == null) {
            return;
        }

        int override = preferences.getInt(PREF_PERF_OVERLAY_OVERRIDE, 0);

        if (override == 1) {
            // Force enabled - green icon
            perfOverlayIcon.setImageResource(R.drawable.ic_perf_overlay_enabled);
            perfOverlayLabel.setText("Stats overlay: Enabled");
        } else if (override == 2) {
            // Force disabled - white icon with red slash
            perfOverlayIcon.setImageResource(R.drawable.ic_perf_overlay);
            perfOverlayLabel.setText("Stats overlay: Disabled");
        } else {
            // Use default - gray icon to indicate auto/default mode
            perfOverlayIcon.setImageResource(R.drawable.ic_perf_overlay_default);
            perfOverlayLabel.setText("Stats overlay: Default");
        }
    }

    public void onResume() {
        // Reload the value in case it was changed elsewhere
        if (bitrateOverrideSeekBar != null) {
            int savedBitrate = preferences.getInt(PREF_BITRATE_OVERRIDE, 0);
            int seekBarPosition = savedBitrate > 0 ? savedBitrate / BITRATE_STEP : 0;
            bitrateOverrideSeekBar.setProgress(seekBarPosition);
            updateBitrateLabel(seekBarPosition);
        }

        // Update performance overlay button state
        updatePerfOverlayButton();

        // Update overrides section visibility and toggle button icon
        updateOverridesSectionVisibility();
        updateToggleButtonIcon();
    }

    public void onPause() {
        // Nothing to do on pause
    }

    public void onDestroy() {
        // Nothing to clean up
    }
}
