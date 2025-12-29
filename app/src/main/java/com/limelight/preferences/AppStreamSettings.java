package com.limelight.preferences;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.os.Handler;
import android.preference.CheckBoxPreference;
import android.preference.EditTextPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceCategory;
import android.preference.PreferenceFragment;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;

import com.limelight.R;
import com.limelight.utils.UiHelper;

public class AppStreamSettings extends Activity {
    public static final String EXTRA_APP_KEY = "AppKey";
    public static final String EXTRA_APP_NAME = "AppName";

    private String appKey;
    private String appName;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        appKey = getIntent().getStringExtra(EXTRA_APP_KEY);
        appName = getIntent().getStringExtra(EXTRA_APP_NAME);

        if (appKey == null || appName == null) {
            finish();
            return;
        }

        UiHelper.setLocale(this);
        setContentView(R.layout.activity_stream_settings);
        setTitle("Settings - " + appName);

        getFragmentManager().beginTransaction().replace(
                R.id.stream_settings, new AppSettingsFragment()
        ).commitAllowingStateLoss();

        UiHelper.notifyNewRootView(this);
    }

    public static class AppSettingsFragment extends PreferenceFragment {
        private String currentResolution;
        private boolean isQuickLaunch;

        private void setupFramePacingPreference(ListPreference framePacingPref, String currentValue, boolean isQuickLaunch) {
            // Get original arrays from resources
            String[] originalEntries = getResources().getStringArray(R.array.video_frame_pacing_names);
            String[] originalValues = getResources().getStringArray(R.array.video_frame_pacing_values);

            // Create new arrays with default option at the beginning
            String[] newEntries = new String[originalEntries.length + 1];
            String[] newValues = new String[originalValues.length + 1];

            // Use different label depending on whether this is a quick launch item or app
            newEntries[0] = isQuickLaunch ? "[Use app default]" : "[Use global default]";
            newValues[0] = ""; // Empty string represents null/default

            // Copy original arrays starting from index 1
            System.arraycopy(originalEntries, 0, newEntries, 1, originalEntries.length);
            System.arraycopy(originalValues, 0, newValues, 1, originalValues.length);

            // Update the preference
            framePacingPref.setEntries(newEntries);
            framePacingPref.setEntryValues(newValues);

            // Set current value (null becomes empty string for default)
            framePacingPref.setValue(currentValue == null ? "" : currentValue);
        }

        private void setupBooleanPreference(ListPreference pref, int entriesResourceId, int valuesResourceId, String currentValue, boolean isQuickLaunch) {
            // Get original arrays from resources
            String[] originalEntries = getResources().getStringArray(entriesResourceId);
            String[] originalValues = getResources().getStringArray(valuesResourceId);

            // Create new arrays with default option at the beginning
            String[] newEntries = new String[originalEntries.length + 1];
            String[] newValues = new String[originalValues.length + 1];

            // Use different label depending on whether this is a quick launch item or app
            newEntries[0] = isQuickLaunch ? "[Use app default]" : "[Use global default]";
            newValues[0] = ""; // Empty string represents null/default

            // Copy original arrays starting from index 1
            System.arraycopy(originalEntries, 0, newEntries, 1, originalEntries.length);
            System.arraycopy(originalValues, 0, newValues, 1, originalValues.length);

            // Update the preference
            pref.setEntries(newEntries);
            pref.setEntryValues(newValues);

            // Set current value (null becomes empty string for default)
            pref.setValue(currentValue == null ? "" : currentValue);
        }

        private void showCustomResolutionDialog() {
            AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
            builder.setTitle("Resolution");
            
            LinearLayout layout = new LinearLayout(getActivity());
            layout.setOrientation(LinearLayout.VERTICAL);
            layout.setPadding(50, 40, 50, 10);
            
            final EditText widthInput = new EditText(getActivity());
            widthInput.setHint("Width (e.g. 1920)");
            widthInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
            
            final EditText heightInput = new EditText(getActivity());
            heightInput.setHint("Height (e.g. 1080)");
            heightInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
            
            // Pre-populate with current resolution if it exists
            if (currentResolution != null && currentResolution.contains("x")) {
                String[] parts = currentResolution.split("x");
                if (parts.length == 2) {
                    widthInput.setText(parts[0]);
                    heightInput.setText(parts[1]);
                }
            }
            
            layout.addView(widthInput);
            layout.addView(heightInput);
            builder.setView(layout);
            
            builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    String width = widthInput.getText().toString().trim();
                    String height = heightInput.getText().toString().trim();
                    
                    if (validateResolution(width, height)) {
                        currentResolution = width + "x" + height;
                        updatePreferenceSummaries();
                        saveSettings();
                    }
                }
            });
            
            builder.setNeutralButton("Clear", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    currentResolution = null;
                    updatePreferenceSummaries();
                    saveSettings();
                }
            });
            
            builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    // Do nothing on cancel
                }
            });
            
            builder.show();
        }
        
        private boolean validateResolution(String width, String height) {
            if (width.isEmpty() || height.isEmpty()) {
                return false;
            }
            
            try {
                int w = Integer.parseInt(width);
                int h = Integer.parseInt(height);
                return w > 0 && h > 0 && w <= 7680 && h <= 4320; // Reasonable limits
            } catch (NumberFormatException e) {
                return false;
            }
        }

        @Override
        public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
            View view = super.onCreateView(inflater, container, savedInstanceState);
            UiHelper.applyStatusBarPadding(view);
            return view;
        }

        @Override
        public void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);

            addPreferencesFromResource(R.xml.app_preferences);

            AppStreamSettings activity = (AppStreamSettings) getActivity();

            AppPreferences.AppSettings currentSettings = AppPreferences.getAppSettings(getActivity(), activity.appKey);

            // Detect if this is a quick launch item (key format: uuid:appid:timestamp vs uuid:appid)
            isQuickLaunch = activity.appKey != null && activity.appKey.split(":").length == 3;

            CheckBoxPreference useGlobalPref = (CheckBoxPreference) findPreference("checkbox_use_global_settings");
            Preference resolutionPref = findPreference("pref_app_resolution");
            EditTextPreference fpsPref = (EditTextPreference) findPreference("text_app_fps");
            EditTextPreference bitratePref = (EditTextPreference) findPreference("text_app_bitrate_kbps");
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");
            ListPreference enableHdrPref = (ListPreference) findPreference("list_app_enable_hdr");
            ListPreference enablePerfOverlayPref = (ListPreference) findPreference("list_app_enable_perf_overlay");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");

            // Update checkbox label and summary based on whether this is a quick launch item
            if (isQuickLaunch) {
                useGlobalPref.setTitle("Use App Settings");
                useGlobalPref.setSummary("Do not use custom settings, instead use the app or global settings");
            }

            // Initialize fields
            currentResolution = currentSettings.resolution;
            useGlobalPref.setChecked(currentSettings.useGlobalSettings);
            fpsPref.setText(currentSettings.fps > 0 ? String.valueOf(currentSettings.fps) : "");
            bitratePref.setText(currentSettings.bitrate > 0 ? String.valueOf(currentSettings.bitrate / 1000) : "");
            actualDisplayRefreshRatePref.setText(currentSettings.actualDisplayRefreshRate > 0 ? String.valueOf(currentSettings.actualDisplayRefreshRate) : "");
            setupFramePacingPreference(framePacingPref, currentSettings.framePacing, isQuickLaunch);
            setupBooleanPreference(enableHdrPref, R.array.hdr_setting_names, R.array.hdr_setting_values, currentSettings.enableHdr, isQuickLaunch);
            setupBooleanPreference(enablePerfOverlayPref, R.array.perf_overlay_setting_names, R.array.perf_overlay_setting_values, currentSettings.enablePerfOverlay, isQuickLaunch);

            updatePreferenceSummaries();
            updatePreferenceStates(useGlobalPref.isChecked());

            // Set the app-specific category title with the app name
            PreferenceCategory appCategory = (PreferenceCategory) findPreference("category_app_specific");
            if (appCategory != null) {
                appCategory.setTitle(activity.appName + " Settings");
            }

            // Set up resolution preference click handler
            resolutionPref.setOnPreferenceClickListener(new Preference.OnPreferenceClickListener() {
                @Override
                public boolean onPreferenceClick(Preference preference) {
                    showCustomResolutionDialog();
                    return true;
                }
            });
            
            // Add listeners for FPS and Frame Pacing to update summaries
            fpsPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });
            
            bitratePref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    // newValue is now a String for EditTextPreference, not Integer
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });
            
            framePacingPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });

            actualDisplayRefreshRatePref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });

            enableHdrPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });

            enablePerfOverlayPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                            saveSettings();
                        }
                    });
                    return true;
                }
            });

            useGlobalPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            boolean useGlobal = (Boolean) newValue;
                            updatePreferenceStates(useGlobal);
                            saveSettings();
                        }
                    });
                    return true;
                }
            });
        }

        private void updatePreferenceStates(boolean useGlobal) {
            findPreference("pref_app_resolution").setEnabled(!useGlobal);
            findPreference("text_app_fps").setEnabled(!useGlobal);
            findPreference("text_app_bitrate_kbps").setEnabled(!useGlobal);
            findPreference("list_app_frame_pacing").setEnabled(!useGlobal);
            findPreference("text_app_actual_display_refresh_rate").setEnabled(!useGlobal);
            findPreference("list_app_enable_hdr").setEnabled(!useGlobal);
            findPreference("list_app_enable_perf_overlay").setEnabled(!useGlobal);
        }
        
        private void updatePreferenceSummaries() {
            Preference resolutionPref = findPreference("pref_app_resolution");
            EditTextPreference fpsPref = (EditTextPreference) findPreference("text_app_fps");
            EditTextPreference bitratePref = (EditTextPreference) findPreference("text_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");
            ListPreference enableHdrPref = (ListPreference) findPreference("list_app_enable_hdr");
            ListPreference enablePerfOverlayPref = (ListPreference) findPreference("list_app_enable_perf_overlay");

            // Set resolution summary
            if (currentResolution != null && !currentResolution.isEmpty()) {
                resolutionPref.setSummary(currentResolution);
            } else {
                resolutionPref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }

            // Set FPS summary
            String fps = fpsPref.getText();
            if (fps != null && !fps.isEmpty() && !fps.equals("0")) {
                fpsPref.setSummary(fps + " FPS");
            } else {
                fpsPref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }

            // Set bitrate summary
            String bitrateMbps = bitratePref.getText();
            if (bitrateMbps != null && !bitrateMbps.isEmpty() && !bitrateMbps.equals("0")) {
                bitratePref.setSummary(bitrateMbps + " Mbps");
            } else {
                bitratePref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }

            // Set frame pacing summary - show the human readable name or default label
            String framePacing = framePacingPref.getValue();
            CharSequence[] framePacingEntries = framePacingPref.getEntries();
            CharSequence[] framePacingValues = framePacingPref.getEntryValues();

            // Find and display the matching entry (including the default option at index 0)
            for (int i = 0; i < framePacingValues.length; i++) {
                if ((framePacing == null && framePacingValues[i].toString().isEmpty()) ||
                    (framePacing != null && framePacing.equals(framePacingValues[i].toString()))) {
                    framePacingPref.setSummary(framePacingEntries[i]);
                    break;
                }
            }

            // Set actual display refresh rate summary
            String actualDisplayRefreshRate = actualDisplayRefreshRatePref.getText();
            if (actualDisplayRefreshRate != null && !actualDisplayRefreshRate.isEmpty() && !actualDisplayRefreshRate.equals("0")) {
                actualDisplayRefreshRatePref.setSummary(actualDisplayRefreshRate + "Hz");
            } else {
                actualDisplayRefreshRatePref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }

            // Set HDR summary - show the human readable name or default label
            String enableHdr = enableHdrPref.getValue();
            CharSequence[] hdrEntries = enableHdrPref.getEntries();
            CharSequence[] hdrValues = enableHdrPref.getEntryValues();
            for (int i = 0; i < hdrValues.length; i++) {
                if ((enableHdr == null && hdrValues[i].toString().isEmpty()) ||
                    (enableHdr != null && enableHdr.equals(hdrValues[i].toString()))) {
                    enableHdrPref.setSummary(hdrEntries[i]);
                    break;
                }
            }

            // Set perf overlay summary - show the human readable name or default label
            String enablePerfOverlay = enablePerfOverlayPref.getValue();
            CharSequence[] perfOverlayEntries = enablePerfOverlayPref.getEntries();
            CharSequence[] perfOverlayValues = enablePerfOverlayPref.getEntryValues();
            for (int i = 0; i < perfOverlayValues.length; i++) {
                if ((enablePerfOverlay == null && perfOverlayValues[i].toString().isEmpty()) ||
                    (enablePerfOverlay != null && enablePerfOverlay.equals(perfOverlayValues[i].toString()))) {
                    enablePerfOverlayPref.setSummary(perfOverlayEntries[i]);
                    break;
                }
            }
        }

        public void saveSettings() {
            AppStreamSettings activity = (AppStreamSettings) getActivity();
            if (activity == null) return;

            CheckBoxPreference useGlobalPref = (CheckBoxPreference) findPreference("checkbox_use_global_settings");
            Preference resolutionPref = findPreference("pref_app_resolution");
            EditTextPreference fpsPref = (EditTextPreference) findPreference("text_app_fps");
            EditTextPreference bitratePref = (EditTextPreference) findPreference("text_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");
            ListPreference enableHdrPref = (ListPreference) findPreference("list_app_enable_hdr");
            ListPreference enablePerfOverlayPref = (ListPreference) findPreference("list_app_enable_perf_overlay");

            int fps = 0;
            String fpsText = fpsPref.getText();
            if (fpsText != null && !fpsText.isEmpty()) {
                try {
                    fps = Integer.parseInt(fpsText);
                } catch (NumberFormatException ignored) {
                }
            }

            // Convert bitrate from Mbps back to kbps for storage
            int bitrateKbps = 0;
            String bitrateMbpsText = bitratePref.getText();
            if (bitrateMbpsText != null && !bitrateMbpsText.isEmpty()) {
                try {
                    int bitrateMbps = Integer.parseInt(bitrateMbpsText);
                    bitrateKbps = bitrateMbps * 1000;
                } catch (NumberFormatException ignored) {
                }
            }

            // Convert empty string back to null for frame pacing (represents default)
            String framePacingValue = framePacingPref.getValue();
            if (framePacingValue != null && framePacingValue.isEmpty()) {
                framePacingValue = null;
            }

            double actualDisplayRefreshRate = 0;
            String actualDisplayRefreshRateText = actualDisplayRefreshRatePref.getText();
            if (actualDisplayRefreshRateText != null && !actualDisplayRefreshRateText.isEmpty()) {
                try {
                    actualDisplayRefreshRate = Double.parseDouble(actualDisplayRefreshRateText);
                } catch (NumberFormatException ignored) {
                }
            }

            // Convert empty string back to null for HDR (represents default)
            String enableHdrValue = enableHdrPref.getValue();
            if (enableHdrValue != null && enableHdrValue.isEmpty()) {
                enableHdrValue = null;
            }

            // Convert empty string back to null for perf overlay (represents default)
            String enablePerfOverlayValue = enablePerfOverlayPref.getValue();
            if (enablePerfOverlayValue != null && enablePerfOverlayValue.isEmpty()) {
                enablePerfOverlayValue = null;
            }

            AppPreferences.AppSettings settings = new AppPreferences.AppSettings(
                currentResolution,
                fps,
                framePacingValue,
                bitrateKbps,
                actualDisplayRefreshRate,
                enableHdrValue,
                enablePerfOverlayValue,
                useGlobalPref.isChecked()
            );

            AppPreferences.saveAppSettings(getActivity(), activity.appKey, settings);
        }
    }
}