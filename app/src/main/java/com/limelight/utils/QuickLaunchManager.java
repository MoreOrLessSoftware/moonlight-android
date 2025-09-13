package com.limelight.utils;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class QuickLaunchManager {
    public static final String QUICK_LAUNCH_PREF_FILENAME = "QuickLaunch";
    public static final String QUICK_LAUNCH_UPDATE_ACTION = "com.limelight.QUICK_LAUNCH_UPDATED";
    
    public static class QuickLaunchItem {
        public final String key;
        public final String computerUuid;
        public final int appId;
        public final String computerName;
        public final String originalAppName;
        public final String customName;
        
        public QuickLaunchItem(String key, String computerUuid, int appId, String computerName, 
                              String originalAppName, String customName) {
            this.key = key;
            this.computerUuid = computerUuid;
            this.appId = appId;
            this.computerName = computerName;
            this.originalAppName = originalAppName;
            this.customName = customName != null ? customName : originalAppName;
        }
        
        public String getDisplayName() {
            return customName;
        }
    }
    
    private final Context context;
    private final SharedPreferences preferences;
    
    public QuickLaunchManager(Context context) {
        this.context = context.getApplicationContext();
        this.preferences = context.getSharedPreferences(QUICK_LAUNCH_PREF_FILENAME, Context.MODE_PRIVATE);
    }
    
    /**
     * Add an app to Quick Launch
     */
    public void addQuickLaunchItem(ComputerDetails computer, NvApp app) {
        String key = createUniqueKey(computer.uuid, app.getAppId());
        String value = createValue(computer.name, app.getAppName(), app.getAppName());
        
        preferences.edit()
                .putString(key, value)
                .apply();
                
        notifyUpdate();
    }
    
    /**
     * Remove a specific Quick Launch item by its unique key
     */
    public void removeQuickLaunchItem(String key) {
        preferences.edit()
                .remove(key)
                .apply();
                
        notifyUpdate();
    }
    
    
    /**
     * Update the custom name for a specific Quick Launch item by its unique key
     */
    public void updateCustomName(String key, String newCustomName) {
        String currentValue = preferences.getString(key, "");
        
        if (!currentValue.isEmpty()) {
            String[] valueParts = currentValue.split("\\|");
            if (valueParts.length >= 2) {
                String computerName = valueParts[0];
                String originalAppName = valueParts[1];
                String newValue = createValue(computerName, originalAppName, newCustomName);
                
                preferences.edit()
                        .putString(key, newValue)
                        .apply();
                        
                notifyUpdate();
            }
        }
    }
    
    /**
     * Get all Quick Launch items
     */
    public List<QuickLaunchItem> getAllQuickLaunchItems() {
        List<QuickLaunchItem> items = new ArrayList<>();
        Map<String, ?> allItems = preferences.getAll();
        
        for (Map.Entry<String, ?> entry : allItems.entrySet()) {
            QuickLaunchItem item = parseQuickLaunchItem(entry.getKey(), (String) entry.getValue());
            if (item != null) {
                items.add(item);
            }
        }
        
        return items;
    }
    
    /**
     * Get the custom name for a specific Quick Launch item by its unique key
     */
    public String getCustomName(String key) {
        String value = preferences.getString(key, "");
        
        if (!value.isEmpty()) {
            String[] valueParts = value.split("\\|");
            if (valueParts.length >= 3) {
                return valueParts[2]; // Custom name
            } else if (valueParts.length >= 2) {
                return valueParts[1]; // Fallback to original name
            }
        }
        
        return "";
    }

    public String getOriginalName(String key) {
        String value = preferences.getString(key, "");

        if (!value.isEmpty()) {
            String[] valueParts = value.split("\\|");
            if (valueParts.length >= 2) {
                return valueParts[1]; // Fallback to original name
            }
        }

        return "";
    }

    private String createUniqueKey(String computerUuid, int appId) {
        // Add a timestamp to make the key unique and allow duplicates
        long timestamp = System.currentTimeMillis();
        return computerUuid + ":" + appId + ":" + timestamp;
    }
    
    private String createValue(String computerName, String originalAppName, String customName) {
        return computerName + "|" + originalAppName + "|" + customName;
    }
    
    private QuickLaunchItem parseQuickLaunchItem(String key, String value) {
        // Parse key: "computerUuid:appId:timestamp"
        String[] keyParts = key.split(":");
        if (keyParts.length < 3) return null;
        
        String computerUuid = keyParts[0];
        int appId;
        try {
            appId = Integer.parseInt(keyParts[1]);
        } catch (NumberFormatException e) {
            return null;
        }
        // keyParts[2] is the timestamp for uniqueness
        
        // Parse value: "computerName|originalAppName|customName"
        String[] valueParts = value.split("\\|");
        if (valueParts.length < 3) return null;
        
        String computerName = valueParts[0];
        String originalAppName = valueParts[1];
        String customName = valueParts[2];
        
        return new QuickLaunchItem(key, computerUuid, appId, computerName, originalAppName, customName);
    }
    
    private void notifyUpdate() {
        Intent updateIntent = new Intent(QUICK_LAUNCH_UPDATE_ACTION);
        context.sendBroadcast(updateIntent);
    }
}