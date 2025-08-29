package com.limelight.preferences;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceFragment;
import android.preference.PreferenceManager;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

import com.limelight.R;
import com.limelight.preferences.SeekBarPreference;
import com.limelight.utils.UiHelper;

import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;

public class AppStreamSettings extends Activity {
    public static final String EXTRA_APP_ID = "AppId";
    public static final String EXTRA_APP_NAME = "AppName";

    private int appId;
    private String appName;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        appId = getIntent().getIntExtra(EXTRA_APP_ID, -1);
        appName = getIntent().getStringExtra(EXTRA_APP_NAME);

        if (appId == -1 || appName == null) {
            finish();
            return;
        }

        UiHelper.setLocale(this);
        setContentView(R.layout.activity_stream_settings);
        setTitle("Settings - " + appName);

        getFragmentManager().beginTransaction().replace(
                R.id.stream_settings, new AppSettingsFragment()
        ).commitAllowingStateLoss();

        // Setup Save, Cancel, and Clear All buttons - need to wait for fragment to load
        findViewById(android.R.id.content).post(new Runnable() {
            @Override
            public void run() {
                Button saveButton = findViewById(R.id.button_save);
                Button cancelButton = findViewById(R.id.button_cancel);
                Button clearAllButton = findViewById(R.id.button_clear_all);
                
                if (saveButton != null && cancelButton != null && clearAllButton != null) {
                    saveButton.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            AppSettingsFragment fragment = (AppSettingsFragment) getFragmentManager().findFragmentById(R.id.stream_settings);
                            if (fragment != null) {
                                fragment.saveSettings();
                                finish();
                            }
                        }
                    });
                    
                    cancelButton.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            finish(); // Just close without saving
                        }
                    });
                    
                    clearAllButton.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            AppSettingsFragment fragment = (AppSettingsFragment) getFragmentManager().findFragmentById(R.id.stream_settings);
                            if (fragment != null) {
                                fragment.clearAllSettings();
                            }
                        }
                    });
                }
            }
        });

        UiHelper.notifyNewRootView(this);
    }

    public static class AppSettingsFragment extends PreferenceFragment {
        private static final String CUSTOM_RESOLUTION_KEY = "custom";

        private void setupResolutionPreference(ListPreference resolutionPref) {
            // Get original arrays from resources
            String[] originalEntries = getResources().getStringArray(R.array.resolution_names);
            String[] originalValues = getResources().getStringArray(R.array.resolution_values);
            
            // Create new arrays starting from original
            List<CharSequence> entries = new ArrayList<>(Arrays.asList(originalEntries));
            List<CharSequence> values = new ArrayList<>(Arrays.asList(originalValues));
            
            // Check if current value is a custom resolution (not in default list)
            String currentValue = resolutionPref.getValue();
            boolean isCustomResolution = currentValue != null && !Arrays.asList(originalValues).contains(currentValue) 
                && !CUSTOM_RESOLUTION_KEY.equals(currentValue);
            
            if (isCustomResolution) {
                // Add current custom resolution to the list
                entries.add(0, "Custom (" + currentValue + ")");
                values.add(0, currentValue);
            }
            
            // Add Custom option
            entries.add("Custom...");
            values.add(CUSTOM_RESOLUTION_KEY);
            
            // Update the preference
            resolutionPref.setEntries(entries.toArray(new CharSequence[0]));
            resolutionPref.setEntryValues(values.toArray(new CharSequence[0]));
            
            // Add listener for custom resolution
            resolutionPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    if (CUSTOM_RESOLUTION_KEY.equals(newValue)) {
                        showCustomResolutionDialog((ListPreference) preference);
                        return false; // Don't change the value yet
                    } else {
                        // Let the preference change, then update summaries after the change is processed
                        new Handler().post(new Runnable() {
                            @Override
                            public void run() {
                                updatePreferenceSummaries();
                            }
                        });
                        return true;
                    }
                }
            });
        }

        private void showCustomResolutionDialog(final ListPreference resolutionPref) {
            AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
            builder.setTitle("Custom Resolution");
            
            LinearLayout layout = new LinearLayout(getActivity());
            layout.setOrientation(LinearLayout.VERTICAL);
            layout.setPadding(50, 40, 50, 10);
            
            final EditText widthInput = new EditText(getActivity());
            widthInput.setHint("Width (e.g. 1920)");
            widthInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
            layout.addView(widthInput);
            
            final EditText heightInput = new EditText(getActivity());
            heightInput.setHint("Height (e.g. 1080)");
            heightInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
            layout.addView(heightInput);
            
            builder.setView(layout);
            
            builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    String width = widthInput.getText().toString().trim();
                    String height = heightInput.getText().toString().trim();
                    
                    if (validateResolution(width, height)) {
                        String customRes = width + "x" + height;
                        resolutionPref.setValue(customRes);
                        setupResolutionPreference(resolutionPref);
                        updatePreferenceSummaries();
                    }
                }
            });
            
            builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    // Revert selection if cancelled
                    setupResolutionPreference(resolutionPref);
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
            final int appId = activity.appId;

            AppPreferences.AppSettings currentSettings = AppPreferences.getAppSettings(getActivity(), appId);
            AppPreferences.AppSettings globalDefaults = AppPreferences.getGlobalDefaults(getActivity());

            CheckBoxPreference useGlobalPref = (CheckBoxPreference) findPreference("checkbox_use_global_settings");
            ListPreference resolutionPref = (ListPreference) findPreference("list_app_resolution");
            ListPreference fpsPref = (ListPreference) findPreference("list_app_fps");
            SeekBarPreference bitratePref = (SeekBarPreference) findPreference("seekbar_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");

            useGlobalPref.setChecked(currentSettings.useGlobalSettings);
            if (currentSettings.resolution != null) {
                resolutionPref.setValue(currentSettings.resolution);
            }
            if (currentSettings.fps != null) {
                fpsPref.setValue(currentSettings.fps);
            }
            if (currentSettings.bitrate != null) {
                bitratePref.setProgress(Integer.parseInt(currentSettings.bitrate));
            }
            if (currentSettings.framePacing != null) {
                framePacingPref.setValue(currentSettings.framePacing);
            }

            // Setup resolution preference with custom option
            setupResolutionPreference(resolutionPref);
            
            // Add listeners for FPS and Frame Pacing to update summaries
            fpsPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
                        }
                    });
                    return true;
                }
            });
            
            bitratePref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    new Handler().post(new Runnable() {
                        @Override
                        public void run() {
                            updatePreferenceSummaries();
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
                        }
                    });
                    return true;
                }
            });
            
            // Set current values as summaries
            updatePreferenceSummaries();
            
            updatePreferenceStates(useGlobalPref.isChecked());

            useGlobalPref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    boolean useGlobal = (Boolean) newValue;
                    updatePreferenceStates(useGlobal);
                    return true;
                }
            });

        }

        private void updatePreferenceStates(boolean useGlobal) {
            findPreference("list_app_resolution").setEnabled(!useGlobal);
            findPreference("list_app_fps").setEnabled(!useGlobal);
            findPreference("seekbar_app_bitrate_kbps").setEnabled(!useGlobal);
            findPreference("list_app_frame_pacing").setEnabled(!useGlobal);
        }
        
        private void updatePreferenceSummaries() {
            ListPreference resolutionPref = (ListPreference) findPreference("list_app_resolution");
            ListPreference fpsPref = (ListPreference) findPreference("list_app_fps");
            SeekBarPreference bitratePref = (SeekBarPreference) findPreference("seekbar_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            
            // Set resolution summary
            String resolution = resolutionPref.getValue();
            if (resolution != null) {
                resolutionPref.setSummary(resolution);
            } else {
                resolutionPref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }
            
            // Set FPS summary
            String fps = fpsPref.getValue();
            if (fps != null) {
                fpsPref.setSummary(fps + " FPS");
            } else {
                fpsPref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }
            
            // Set bitrate summary
            int bitrateKbps = bitratePref.getProgress();
            if (bitrateKbps > 0) {
                bitratePref.setSummary(String.format("%.1f Mbps", bitrateKbps / 1000.0f));
            } else {
                bitratePref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
            }
            
            // Set frame pacing summary - show the human readable name
            String framePacing = framePacingPref.getValue();
            if (framePacing != null) {
                CharSequence[] entries = framePacingPref.getEntries();
                CharSequence[] values = framePacingPref.getEntryValues();
                for (int i = 0; i < values.length; i++) {
                    if (framePacing.equals(values[i].toString())) {
                        framePacingPref.setSummary(entries[i]);
                        break;
                    }
                }
            } else {
                framePacingPref.setSummary(android.text.Html.fromHtml("<i>Not set</i>"));
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

            AppPreferences.AppSettings settings = new AppPreferences.AppSettings(
                resolutionPref.getValue(),
                fpsPref.getValue(),
                framePacingPref.getValue(),
                bitratePref.getProgress() > 0 ? String.valueOf(bitratePref.getProgress()) : null,
                useGlobalPref.isChecked()
            );

            AppPreferences.saveAppSettings(getActivity(), activity.appId, settings);
        }
        
        public void clearAllSettings() {
            ListPreference resolutionPref = (ListPreference) findPreference("list_app_resolution");
            ListPreference fpsPref = (ListPreference) findPreference("list_app_fps");
            SeekBarPreference bitratePref = (SeekBarPreference) findPreference("seekbar_app_bitrate_kbps");
            ListPreference framePacingPref = (ListPreference) findPreference("list_app_frame_pacing");
            
            // Rebuild resolution list without custom entries first
            setupResolutionPreference(resolutionPref);
            
            // Then clear all preference values
            resolutionPref.setValue(null);
            fpsPref.setValue(null);
            bitratePref.setProgress(0);
            framePacingPref.setValue(null);
            
            // Update summaries
            updatePreferenceSummaries();
        }
    }
}