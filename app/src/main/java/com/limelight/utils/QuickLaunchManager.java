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
    
    public interface RunningStatusListener {
        void onRunningStatusChanged(int runningAppId);
    }
    
    private RunningStatusListener runningStatusListener;
    private int currentRunningAppId = 0;
    
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

        public String getDisplayNameLong() {
            return customName + (!customName.equals(originalAppName) ? " (" + originalAppName + ")" : "");
        }
    }
    
    private final Context context;
    private final SharedPreferences preferences;
    
    public QuickLaunchManager(Context context) {
        this.context = context.getApplicationContext();
        this.preferences = context.getSharedPreferences(QUICK_LAUNCH_PREF_FILENAME, Context.MODE_PRIVATE);
    }
    
    public void setRunningStatusListener(RunningStatusListener listener) {
        this.runningStatusListener = listener;
    }
    
    public void updateRunningAppId(int runningAppId) {
        if (this.currentRunningAppId != runningAppId) {
            this.currentRunningAppId = runningAppId;
            if (runningStatusListener != null) {
                runningStatusListener.onRunningStatusChanged(runningAppId);
            }
        }
    }
    
    public int getCurrentRunningAppId() {
        return currentRunningAppId;
    }
    
    public boolean isAppRunning(int appId) {
        return currentRunningAppId == appId && currentRunningAppId != 0;
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
     * Get all Quick Launch items sorted by their keys (which include timestamps)
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

        // Sort by the timestamp in the key to maintain order
        items.sort((a, b) -> {
            // Extract timestamps from keys
            String[] aParts = a.key.split(":");
            String[] bParts = b.key.split(":");
            if (aParts.length >= 3 && bParts.length >= 3) {
                try {
                    long aTimestamp = Long.parseLong(aParts[2]);
                    long bTimestamp = Long.parseLong(bParts[2]);
                    return Long.compare(aTimestamp, bTimestamp);
                } catch (NumberFormatException e) {
                    // Fallback to string comparison if parsing fails
                    return a.key.compareTo(b.key);
                }
            }
            return a.key.compareTo(b.key);
        });

        return items;
    }

    /**
     * Move a Quick Launch item left (earlier in the list)
     */
    public boolean moveQuickLaunchItemLeft(String key) {
        List<QuickLaunchItem> items = getAllQuickLaunchItems();
        int index = -1;

        // Find the item's current position
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).key.equals(key)) {
                index = i;
                break;
            }
        }

        // Can't move left if it's already first or not found
        if (index <= 0) {
            return false;
        }

        // Swap with the previous item by recreating their keys with swapped timestamps
        QuickLaunchItem currentItem = items.get(index);
        QuickLaunchItem previousItem = items.get(index - 1);

        swapItemPositions(currentItem, previousItem);
        return true;
    }

    /**
     * Move a Quick Launch item right (later in the list)
     */
    public boolean moveQuickLaunchItemRight(String key) {
        List<QuickLaunchItem> items = getAllQuickLaunchItems();
        int index = -1;

        // Find the item's current position
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).key.equals(key)) {
                index = i;
                break;
            }
        }

        // Can't move right if it's already last or not found
        if (index < 0 || index >= items.size() - 1) {
            return false;
        }

        // Swap with the next item by recreating their keys with swapped timestamps
        QuickLaunchItem currentItem = items.get(index);
        QuickLaunchItem nextItem = items.get(index + 1);

        swapItemPositions(currentItem, nextItem);
        return true;
    }

    private void swapItemPositions(QuickLaunchItem item1, QuickLaunchItem item2) {
        // Get the values before removing
        String value1 = preferences.getString(item1.key, "");
        String value2 = preferences.getString(item2.key, "");

        // Parse the keys to extract timestamps
        String[] key1Parts = item1.key.split(":");
        String[] key2Parts = item2.key.split(":");

        if (key1Parts.length >= 3 && key2Parts.length >= 3) {
            // Create new keys with swapped timestamps
            String newKey1 = key1Parts[0] + ":" + key1Parts[1] + ":" + key2Parts[2];
            String newKey2 = key2Parts[0] + ":" + key2Parts[1] + ":" + key1Parts[2];

            // Do everything in a single transaction
            SharedPreferences.Editor editor = preferences.edit();
            editor.remove(item1.key);
            editor.remove(item2.key);
            editor.putString(newKey1, value1);
            editor.putString(newKey2, value2);
            editor.apply();

            notifyUpdate();
        }
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
        // Make it an explicit broadcast by setting the package to avoid restrictions on newer Android versions
        updateIntent.setPackage(context.getPackageName());
        context.sendBroadcast(updateIntent);
    }
}