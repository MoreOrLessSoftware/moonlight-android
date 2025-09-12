package com.limelight.preferences;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONException;
import org.json.JSONObject;

public class AppPreferences {
    private static final String APP_PREFERENCES_FILE = "AppPreferences";
    private static final String GLOBAL_DEFAULTS_KEY = "_global_defaults";

    public static class AppSettings {
        public String resolution;
        public int fps;
        public String framePacing;
        public int bitrate;
        public double actualDisplayRefreshRate;
        public boolean enablePerfOverlay;
        public boolean useGlobalSettings;

        public AppSettings() {
            this.useGlobalSettings = true;
        }

        public AppSettings(String resolution, int fps, String framePacing, int bitrate, double actualDisplayRefreshRate, boolean enablePerfOverlay, boolean useGlobalSettings) {
            this.resolution = resolution;
            this.fps = fps;
            this.framePacing = framePacing;
            this.bitrate = bitrate;
            this.actualDisplayRefreshRate = actualDisplayRefreshRate;
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
                json.optBoolean("enablePerfOverlay", false),
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
        AppSettings appSettings = getAppSettings(context, appKey);
        
        if (appSettings.useGlobalSettings) {
            return PreferenceConfiguration.readPreferences(context);
        }
        
        PreferenceConfiguration config = PreferenceConfiguration.readPreferences(context);
        
        if (appSettings.resolution != null) {
            String[] parts = appSettings.resolution.split("x");
            if (parts.length == 2) {
                try {
                    config.width = Integer.parseInt(parts[0]);
                    config.height = Integer.parseInt(parts[1]);
                } catch (NumberFormatException e) {
                    // Keep global settings
                }
            }
        }
        
        if (appSettings.fps > 0) {
            config.fps = appSettings.fps;
        }
        
        if (appSettings.framePacing != null) {
            config.framePacing = getFramePacingValue(appSettings.framePacing);
        }
        
        if (appSettings.bitrate > 0) {
            config.bitrate = appSettings.bitrate;
        }

        if (appSettings.actualDisplayRefreshRate > 0) {
            config.actualDisplayRefreshRate = (float) appSettings.actualDisplayRefreshRate;
        }

        config.enablePerfOverlay = appSettings.enablePerfOverlay;;
        
        return config;
    }
}