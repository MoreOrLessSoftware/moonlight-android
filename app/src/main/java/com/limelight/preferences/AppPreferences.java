package com.limelight.preferences;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import org.json.JSONException;
import org.json.JSONObject;

public class AppPreferences {
    private static final String APP_PREFERENCES_FILE = "AppPreferences";
    private static final String PREF_BITRATE_OVERRIDE = "bitrate_override"; // Global bitrate override
    private static final String PREF_PERF_OVERLAY_OVERRIDE = "perf_overlay_override"; // Global performance overlay override
    private static final String PREF_OVERRIDES_ENABLED = "overrides_enabled"; // Whether overrides section is enabled

    public static class AppSettings {
        public String resolution;
        public int fps;
        public String framePacing;
        public int bitrate;
        public double actualDisplayRefreshRate;
        public String enableHdr;
        public String enablePerfOverlay;
        public boolean useGlobalSettings;

        public AppSettings() {
            this.useGlobalSettings = true;
        }

        public AppSettings(String resolution, int fps, String framePacing, int bitrate, double actualDisplayRefreshRate, String enableHdr, String enablePerfOverlay, boolean useGlobalSettings) {
            this.resolution = resolution;
            this.fps = fps;
            this.framePacing = framePacing;
            this.bitrate = bitrate;
            this.actualDisplayRefreshRate = actualDisplayRefreshRate;
            this.enableHdr = enableHdr;
            this.enablePerfOverlay = enablePerfOverlay;
            this.useGlobalSettings = useGlobalSettings;
        }

        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("resolution", resolution);
            json.put("fps", fps);
            json.put("framePacing", framePacing);
            json.put("bitrate", bitrate);
            json.put("actualDisplayRefreshRate", actualDisplayRefreshRate);
            json.put("enableHdr", enableHdr);
            json.put("enablePerfOverlay", enablePerfOverlay);
            json.put("useGlobalSettings", useGlobalSettings);
            return json;
        }

        public static AppSettings fromJson(JSONObject json) throws JSONException {
            return new AppSettings(
                json.optString("resolution", null),
                json.optInt("fps", 0),
                json.optString("framePacing", null),
                json.optInt("bitrate", 0),
                json.optDouble("actualDisplayRefreshRate", 0),
                json.optString("enableHdr", null),
                json.optString("enablePerfOverlay", null),
                json.optBoolean("useGlobalSettings", true)
            );
        }
    }

    public static AppSettings getAppSettings(Context context, String appKey) {
        SharedPreferences prefs = context.getSharedPreferences(APP_PREFERENCES_FILE, Context.MODE_PRIVATE);
        String jsonString = prefs.getString(appKey, null);
        
        if (jsonString == null) {
            return new AppSettings();
        }
        
        try {
            JSONObject json = new JSONObject(jsonString);
            return AppSettings.fromJson(json);
        } catch (JSONException e) {
            return new AppSettings();
        }
    }

    public static void saveAppSettings(Context context, String appKey, AppSettings settings) {
        SharedPreferences prefs = context.getSharedPreferences(APP_PREFERENCES_FILE, Context.MODE_PRIVATE);
        try {
            JSONObject json = settings.toJson();
            prefs.edit().putString(appKey, json.toString()).apply();
        } catch (JSONException e) {
            e.printStackTrace();
        }
    }

    private static int getFramePacingValue(String framePacingString) {
        if (framePacingString == null) {
            return PreferenceConfiguration.FRAME_PACING_MIN_LATENCY;
        }
        switch (framePacingString) {
            case "latency":
                return PreferenceConfiguration.FRAME_PACING_MIN_LATENCY;
            case "balanced":
                return PreferenceConfiguration.FRAME_PACING_BALANCED;
            case "cap-fps":
                return PreferenceConfiguration.FRAME_PACING_CAP_FPS;
            case "smoothness":
                return PreferenceConfiguration.FRAME_PACING_MAX_SMOOTHNESS;
            default:
                return PreferenceConfiguration.FRAME_PACING_MIN_LATENCY;
        }
    }

    public static PreferenceConfiguration getEffectivePreferences(Context context, String appKey) {
        return getEffectivePreferences(context, appKey, null);
    }

    public static PreferenceConfiguration getEffectivePreferences(Context context, String appKey, String appKey2) {
        // Load quick launch settings (appKey2), app settings (appKey), and global settings
        AppSettings quickLaunchSettings = null;
        if (appKey2 != null) {
            quickLaunchSettings = getAppSettings(context, appKey2);
        }
        AppSettings appSettings = getAppSettings(context, appKey);

        // If both quick launch and app use global settings, return global config directly
        if ((quickLaunchSettings == null || quickLaunchSettings.useGlobalSettings) && appSettings.useGlobalSettings) {
            return PreferenceConfiguration.readPreferences(context);
        }

        // Start with global settings as the base
        PreferenceConfiguration config = PreferenceConfiguration.readPreferences(context);

        // Apply settings hierarchy: quick launch -> app -> global (already loaded)
        // For each setting, use quick launch if set, otherwise app, otherwise keep global

        // Resolution
        String resolution = null;
        if (quickLaunchSettings != null && quickLaunchSettings.resolution != null) {
            resolution = quickLaunchSettings.resolution;
        } else if (!appSettings.useGlobalSettings && appSettings.resolution != null) {
            resolution = appSettings.resolution;
        }
        if (resolution != null) {
            String[] parts = resolution.split("x");
            if (parts.length == 2) {
                try {
                    config.width = Integer.parseInt(parts[0]);
                    config.height = Integer.parseInt(parts[1]);
                } catch (NumberFormatException e) {
                    // Keep global settings
                }
            }
        }

        // FPS
        int fps = 0;
        if (quickLaunchSettings != null && quickLaunchSettings.fps > 0) {
            fps = quickLaunchSettings.fps;
        } else if (!appSettings.useGlobalSettings && appSettings.fps > 0) {
            fps = appSettings.fps;
        }
        if (fps > 0) {
            config.fps = fps;
        }

        // Frame pacing
        String framePacing = null;
        if (quickLaunchSettings != null && quickLaunchSettings.framePacing != null) {
            framePacing = quickLaunchSettings.framePacing;
        } else if (!appSettings.useGlobalSettings && appSettings.framePacing != null) {
            framePacing = appSettings.framePacing;
        }
        if (framePacing != null) {
            config.framePacing = getFramePacingValue(framePacing);
        }

        // Bitrate
        int bitrate = 0;
        if (quickLaunchSettings != null && quickLaunchSettings.bitrate > 0) {
            bitrate = quickLaunchSettings.bitrate;
        } else if (!appSettings.useGlobalSettings && appSettings.bitrate > 0) {
            bitrate = appSettings.bitrate;
        }
        if (bitrate > 0) {
            config.bitrate = bitrate;
        }

        // Display refresh rate
        double actualDisplayRefreshRate = 0;
        if (quickLaunchSettings != null && quickLaunchSettings.actualDisplayRefreshRate > 0) {
            actualDisplayRefreshRate = quickLaunchSettings.actualDisplayRefreshRate;
        } else if (!appSettings.useGlobalSettings && appSettings.actualDisplayRefreshRate > 0) {
            actualDisplayRefreshRate = appSettings.actualDisplayRefreshRate;
        }
        if (actualDisplayRefreshRate > 0) {
            config.actualDisplayRefreshRate = String.valueOf(actualDisplayRefreshRate);
        }

        // HDR
        String enableHdrValue = null;
        if (quickLaunchSettings != null && quickLaunchSettings.enableHdr != null) {
            enableHdrValue = quickLaunchSettings.enableHdr;
        } else if (!appSettings.useGlobalSettings && appSettings.enableHdr != null) {
            enableHdrValue = appSettings.enableHdr;
        }
        if (enableHdrValue != null) {
            config.enableHdr = Boolean.parseBoolean(enableHdrValue);
        }

        // Performance overlay
        String enablePerfOverlayValue = null;
        if (quickLaunchSettings != null && quickLaunchSettings.enablePerfOverlay != null) {
            enablePerfOverlayValue = quickLaunchSettings.enablePerfOverlay;
        } else if (!appSettings.useGlobalSettings && appSettings.enablePerfOverlay != null) {
            enablePerfOverlayValue = appSettings.enablePerfOverlay;
        }
        if (enablePerfOverlayValue != null) {
            config.enablePerfOverlay = Boolean.parseBoolean(enablePerfOverlayValue);
        }

        // Apply global overrides only if overrides are enabled (highest priority)
        // This override applies to all streams regardless of other settings
        SharedPreferences defaultPrefs = PreferenceManager.getDefaultSharedPreferences(context);
        boolean overridesEnabled = defaultPrefs.getBoolean(PREF_OVERRIDES_ENABLED, false);

        if (overridesEnabled) {
            // Apply global bitrate override
            int bitrateOverride = defaultPrefs.getInt(PREF_BITRATE_OVERRIDE, 0);
            if (bitrateOverride > 0) {
                config.bitrate = bitrateOverride;
            }

            // Apply global performance overlay override
            // 0 = use default (no override), 1 = force on, 2 = force off
            int perfOverlayOverride = defaultPrefs.getInt(PREF_PERF_OVERLAY_OVERRIDE, 0);
            if (perfOverlayOverride == 1) {
                config.enablePerfOverlay = true;
            } else if (perfOverlayOverride == 2) {
                config.enablePerfOverlay = false;
            }
            // If perfOverlayOverride == 0, keep the config value as is (use default)
        }

        return config;
    }
}