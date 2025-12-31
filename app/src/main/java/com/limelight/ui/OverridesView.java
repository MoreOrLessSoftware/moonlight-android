package com.limelight.ui;

import android.app.Activity;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.limelight.R;

public class OverridesView {
    public static final String PREF_BITRATE_OVERRIDE = "bitrate_override";
    public static final String PREF_PERF_OVERLAY_OVERRIDE = "perf_overlay_override";
    public static final String PREF_OVERRIDES_ENABLED = "overrides_enabled";
    private static final int BITRATE_STEP = 10000; // 10 Mbps steps in kbps
    private static final int SEEKBAR_MAX = 50; // 50 steps * 10 Mbps = 500 Mbps max

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
    }

    private void updateBitrateLabel(int seekBarPosition) {
        if (bitrateOverrideValue == null) {
            return;
        }

        if (seekBarPosition == 0) {
            bitrateOverrideValue.setText(R.string.bitrate_use_default);
        } else {
            int bitrateMbps = seekBarPosition * 10; // Each position is 10 Mbps
            bitrateOverrideValue.setText(String.format("Bitrate: %d Mbps", bitrateMbps));
        }
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
            perfOverlayLabel.setText("Performance stats: Enabled");
        } else if (override == 2) {
            // Force disabled - white icon with red slash
            perfOverlayIcon.setImageResource(R.drawable.ic_perf_overlay);
            perfOverlayLabel.setText("Performance stats: Disabled");
        } else {
            // Use default - gray icon to indicate auto/default mode
            perfOverlayIcon.setImageResource(R.drawable.ic_perf_overlay_default);
            perfOverlayLabel.setText("Performance stats: Default");
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
