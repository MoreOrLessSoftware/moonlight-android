package com.limelight.preferences;

import android.app.Activity;
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

        // An app's settings are a single page, so they don't need the global settings' tabs
        findViewById(R.id.settings_tabs).setVisibility(View.GONE);

        getFragmentManager().beginTransaction().replace(
                R.id.stream_settings, new AppSettingsFragment()
        ).commitAllowingStateLoss();

        UiHelper.notifyNewRootView(this);
    }

    public static class AppSettingsFragment extends PreferenceFragment {
        private boolean isQuickLaunch;

        // The name of the option for leaving a setting to the app's or global setting
        private String getDefaultEntryName() {
            return isQuickLaunch ? "[Use app default]" : "[Use global default]";
        }

        private void setupListPreference(ListPreference pref, int entriesResourceId, int valuesResourceId, String currentValue) {
            // Get original arrays from resources
            String[] originalEntries = getResources().getStringArray(entriesResourceId);
            String[] originalValues = getResources().getStringArray(valuesResourceId);

            // Create new arrays with default option at the beginning
            String[] newEntries = new String[originalEntries.length + 1];
            String[] newValues = new String[originalValues.length + 1];

            // Use different label depending on whether this is a quick launch item or app
            newEntries[0] = getDefaultEntryName();
            newValues[0] = ""; // Empty string represents null/default

            // Copy original arrays starting from index 1
            System.arraycopy(originalEntries, 0, newEntries, 1, originalEntries.length);
            System.arraycopy(originalValues, 0, newValues, 1, originalValues.length);

            // Update the preference
            pref.setEntryValues(newValues);
            pref.setEntries(newEntries);

            // Set current value (null becomes empty string for default)
            pref.setValue(currentValue == null ? "" : currentValue);
        }

        // The bitrate recommended for this app's resolution, frame rate and codec, or the
        // ones it falls back to when they're unset
        private int getRecommendedBitrate() {
            AppStreamSettings activity = (AppStreamSettings) getActivity();
            if (activity == null) {
                return 0;
            }

            // A Quick Launch item falls back to its app's settings, and an app to the global ones
            PreferenceConfiguration fallback;
            if (isQuickLaunch) {
                String[] keyParts = activity.appKey.split(":");
                fallback = AppPreferences.getEffectivePreferences(activity, keyParts[0] + ":" + keyParts[1], null, false);
            }
            else {
                fallback = PreferenceConfiguration.readPreferences(activity);
            }

            int width = fallback.width;
            int height = fallback.height;
            int fps = fallback.fps;
            boolean pyrowave = fallback.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE;

            CheckBoxPreference useGlobalPref = (CheckBoxPreference) findPreference("checkbox_use_global_settings");
            if (!useGlobalPref.isChecked()) {
                String resolution = ((ListPreference) findPreference("list_app_resolution")).getValue();
                if (resolution != null && !resolution.isEmpty()) {
                    width = PreferenceConfiguration.getWidthFromResolutionString(resolution);
                    height = PreferenceConfiguration.getHeightFromResolutionString(resolution);
                }

                int appFps = parseInt(((ListPreference) findPreference("list_app_fps")).getValue());
                if (appFps > 0) {
                    fps = appFps;
                }

                String videoFormat = ((ListPreference) findPreference("list_app_video_format")).getValue();
                if (videoFormat != null && !videoFormat.isEmpty()) {
                    pyrowave = PreferenceConfiguration.parseVideoFormat(videoFormat) == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE;
                }
            }

            return PreferenceConfiguration.getDefaultBitrate(width, height, fps, pyrowave);
        }

        // A list's empty value represents the default, which is saved as null
        private static String emptyToNull(String value) {
            return value != null && value.isEmpty() ? null : value;
        }

        private static int parseInt(String text) {
            if (text == null || text.isEmpty()) {
                return 0;
            }
            try {
                return Integer.parseInt(text);
            } catch (NumberFormatException e) {
                return 0;
            }
        }

        @Override
        public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
            View view = super.onCreateView(inflater, container, savedInstanceState);
            UiHelper.applyPreferenceListPadding(view);
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
            ResolutionListPreference resolutionPref = (ResolutionListPreference) findPreference("list_app_resolution");
            FpsListPreference fpsPref = (FpsListPreference) findPreference("list_app_fps");
            SeekBarPreference bitratePref = (SeekBarPreference) findPreference("seekbar_app_bitrate_kbps");
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");
            ListPreference enableHdrPref = (ListPreference) findPreference("list_app_enable_hdr");
            ListPreference enablePerfOverlayPref = (ListPreference) findPreference("list_app_enable_perf_overlay");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            ListPreference videoFormatPref = (ListPreference) findPreference("list_app_video_format");
            ListPreference jitterBufferPref = (ListPreference) findPreference("list_app_jitter_buffer");

            // Update checkbox label and summary based on whether this is a quick launch item
            if (isQuickLaunch) {
                useGlobalPref.setTitle("Use App Settings");
                useGlobalPref.setSummary("Do not use custom settings, instead use the app or global settings");
            }

            // The refresh rate's summary shows its value, so its help goes in its dialog
            actualDisplayRefreshRatePref.setDialogMessage(actualDisplayRefreshRatePref.getSummary());

            // Initialize fields
            useGlobalPref.setChecked(currentSettings.useGlobalSettings);
            // Resolution, FPS and bitrate go straight to entering a value, with
            // Clear going back to the default
            resolutionPref.setCustomOnly(true);
            resolutionPref.setDefaultEntry(getDefaultEntryName());
            resolutionPref.setValue(currentSettings.resolution != null ? currentSettings.resolution : "");
            fpsPref.setCustomOnly(true);
            fpsPref.setDefaultEntry(getDefaultEntryName());
            fpsPref.setValue(currentSettings.fps > 0 ? String.valueOf(currentSettings.fps) : "");
            bitratePref.setCustomOnly(true);
            bitratePref.setUnsetName(getDefaultEntryName());
            bitratePref.setProgress(currentSettings.bitrate);
            bitratePref.setRecommendedBitrateProvider(new SeekBarPreference.RecommendedBitrateProvider() {
                @Override
                public int getRecommendedBitrate() {
                    return AppSettingsFragment.this.getRecommendedBitrate();
                }
            });
            actualDisplayRefreshRatePref.setText(currentSettings.actualDisplayRefreshRate > 0 ? String.valueOf(currentSettings.actualDisplayRefreshRate) : "");
            setupListPreference(videoFormatPref, R.array.video_format_names, R.array.video_format_values, currentSettings.videoFormat);
            setupListPreference(framePacingPref, R.array.video_frame_pacing_names, R.array.video_frame_pacing_values, currentSettings.framePacing);
            setupListPreference(jitterBufferPref, R.array.jitter_buffer_names, R.array.jitter_buffer_values, currentSettings.jitterBuffer);
            setupListPreference(enableHdrPref, R.array.hdr_setting_names, R.array.hdr_setting_values, currentSettings.enableHdr);
            setupListPreference(enablePerfOverlayPref, R.array.perf_overlay_setting_names, R.array.perf_overlay_setting_values, currentSettings.enablePerfOverlay);

            updatePreferenceSummaries();
            updatePreferenceStates(useGlobalPref.isChecked());

            // Set the app-specific category title with the app name
            PreferenceCategory appCategory = (PreferenceCategory) findPreference("category_app_specific");
            if (appCategory != null) {
                appCategory.setTitle(activity.appName + " Settings");
            }

            // Save each change once the preference has taken its new value
            Preference.OnPreferenceChangeListener saveOnChange = new Preference.OnPreferenceChangeListener() {
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
            };
            resolutionPref.setOnPreferenceChangeListener(saveOnChange);
            fpsPref.setOnPreferenceChangeListener(saveOnChange);
            bitratePref.setOnPreferenceChangeListener(saveOnChange);
            framePacingPref.setOnPreferenceChangeListener(saveOnChange);
            videoFormatPref.setOnPreferenceChangeListener(saveOnChange);
            jitterBufferPref.setOnPreferenceChangeListener(saveOnChange);
            actualDisplayRefreshRatePref.setOnPreferenceChangeListener(saveOnChange);
            enableHdrPref.setOnPreferenceChangeListener(saveOnChange);
            enablePerfOverlayPref.setOnPreferenceChangeListener(saveOnChange);

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
            findPreference("list_app_resolution").setEnabled(!useGlobal);
            findPreference("list_app_fps").setEnabled(!useGlobal);
            findPreference("seekbar_app_bitrate_kbps").setEnabled(!useGlobal);
            findPreference("list_app_frame_pacing").setEnabled(!useGlobal);
            findPreference("list_app_video_format").setEnabled(!useGlobal);
            findPreference("list_app_jitter_buffer").setEnabled(!useGlobal);
            findPreference("text_app_actual_display_refresh_rate").setEnabled(!useGlobal);
            findPreference("list_app_enable_hdr").setEnabled(!useGlobal);
            findPreference("list_app_enable_perf_overlay").setEnabled(!useGlobal);
        }

        // The other settings show their value as their summary themselves
        private void updatePreferenceSummaries() {
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");

            // Set actual display refresh rate summary
            String actualDisplayRefreshRate = actualDisplayRefreshRatePref.getText();
            if (actualDisplayRefreshRate != null && !actualDisplayRefreshRate.isEmpty() && !actualDisplayRefreshRate.equals("0")) {
                actualDisplayRefreshRatePref.setSummary(actualDisplayRefreshRate + "Hz");
            } else {
                actualDisplayRefreshRatePref.setSummary(getDefaultEntryName());
            }
        }

        public void saveSettings() {
            AppStreamSettings activity = (AppStreamSettings) getActivity();
            if (activity == null) return;

            CheckBoxPreference useGlobalPref = (CheckBoxPreference) findPreference("checkbox_use_global_settings");
            ListPreference resolutionPref = (ListPreference) findPreference("list_app_resolution");
            ListPreference fpsPref = (ListPreference) findPreference("list_app_fps");
            SeekBarPreference bitratePref = (SeekBarPreference) findPreference("seekbar_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            EditTextPreference actualDisplayRefreshRatePref = (EditTextPreference) findPreference("text_app_actual_display_refresh_rate");
            ListPreference enableHdrPref = (ListPreference) findPreference("list_app_enable_hdr");
            ListPreference enablePerfOverlayPref = (ListPreference) findPreference("list_app_enable_perf_overlay");
            ListPreference videoFormatPref = (ListPreference) findPreference("list_app_video_format");
            ListPreference jitterBufferPref = (ListPreference) findPreference("list_app_jitter_buffer");

            // Convert empty string back to null for resolution (represents default)
            String resolutionValue = resolutionPref.getValue();
            if (resolutionValue != null && resolutionValue.isEmpty()) {
                resolutionValue = null;
            }

            // 0 represents the default for FPS and bitrate
            int fps = parseInt(fpsPref.getValue());
            int bitrateKbps = bitratePref.getProgress();

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
                resolutionValue,
                fps,
                framePacingValue,
                bitrateKbps,
                actualDisplayRefreshRate,
                enableHdrValue,
                enablePerfOverlayValue,
                emptyToNull(videoFormatPref.getValue()),
                emptyToNull(jitterBufferPref.getValue()),
                useGlobalPref.isChecked()
            );

            AppPreferences.saveAppSettings(getActivity(), activity.appKey, settings);
        }
    }
}
