package com.limelight.ui;

import android.app.Activity;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.limelight.R;

public class OverridesView {
    public static final String PREF_BITRATE_OVERRIDE = "bitrate_override";
    private static final int BITRATE_STEP = 10000; // 10 Mbps steps in kbps
    private static final int SEEKBAR_MAX = 50; // 50 steps * 10 Mbps = 500 Mbps max

    private final Activity activity;
    private final LinearLayout overridesSection;
    private final SeekBar bitrateOverrideSeekBar;
    private final TextView bitrateOverrideValue;
    private final SharedPreferences preferences;

    public OverridesView(Activity activity) {
        this.activity = activity;
        this.preferences = PreferenceManager.getDefaultSharedPreferences(activity);

        // Find views
        this.overridesSection = activity.findViewById(R.id.overridesSection);
        this.bitrateOverrideSeekBar = activity.findViewById(R.id.bitrateOverrideSeekBar);
        this.bitrateOverrideValue = activity.findViewById(R.id.bitrateOverrideValue);

        initializeBitrateOverride();
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

    public void onResume() {
        // Reload the value in case it was changed elsewhere
        if (bitrateOverrideSeekBar != null) {
            int savedBitrate = preferences.getInt(PREF_BITRATE_OVERRIDE, 0);
            int seekBarPosition = savedBitrate > 0 ? savedBitrate / BITRATE_STEP : 0;
            bitrateOverrideSeekBar.setProgress(seekBarPosition);
            updateBitrateLabel(seekBarPosition);
        }
    }

    public void onPause() {
        // Nothing to do on pause
    }

    public void onDestroy() {
        // Nothing to clean up
    }
}
