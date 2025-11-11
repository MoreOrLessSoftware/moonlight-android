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
    private static final String SORT_ORDER_KEY = "_sort_order";

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

        // Add to the end of the sort order
        List<String> sortOrder = getSortOrder();
        sortOrder.add(key);
        saveSortOrder(sortOrder);

        preferences.edit()
                .putString(key, value)
                .apply();

        notifyUpdate();
    }
    
    /**
     * Remove a specific Quick Launch item by its unique key
     */
    public void removeQuickLaunchItem(String key) {
        // Remove from sort order
        List<String> sortOrder = getSortOrder();
        sortOrder.remove(key);
        saveSortOrder(sortOrder);

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
     * Get all Quick Launch items sorted by the saved sort order
     */
    public List<QuickLaunchItem> getAllQuickLaunchItems() {
        Map<String, ?> allItems = preferences.getAll();
        List<String> sortOrder = getSortOrder();

        // Build a map of key -> item for quick lookup
        Map<String, QuickLaunchItem> itemMap = new java.util.HashMap<>();
        for (Map.Entry<String, ?> entry : allItems.entrySet()) {
            if (entry.getKey().equals(SORT_ORDER_KEY)) continue; // Skip sort order entry
            QuickLaunchItem item = parseQuickLaunchItem(entry.getKey(), (String) entry.getValue());
            if (item != null) {
                itemMap.put(item.key, item);
            }
        }

        // Build result list in sort order
        List<QuickLaunchItem> items = new ArrayList<>();
        for (String key : sortOrder) {
            QuickLaunchItem item = itemMap.get(key);
            if (item != null) {
                items.add(item);
                itemMap.remove(key); // Remove so we can detect orphaned items
            }
        }

        // Add any items not in sort order (orphaned items from old data) at the end
        for (QuickLaunchItem orphanedItem : itemMap.values()) {
            items.add(orphanedItem);
            sortOrder.add(orphanedItem.key); // Add to sort order for future
        }

        // Save updated sort order if we found orphaned items
        if (!itemMap.isEmpty()) {
            saveSortOrder(sortOrder);
        }

        return items;
    }

    /**
     * Move a Quick Launch item left (earlier in the list)
     */
    public boolean moveQuickLaunchItemLeft(String key) {
        List<String> sortOrder = getSortOrder();
        int index = sortOrder.indexOf(key);

        // Can't move left if it's already first or not found
        if (index <= 0) {
            return false;
        }

        // Swap with previous item in sort order
        String temp = sortOrder.get(index - 1);
        sortOrder.set(index - 1, sortOrder.get(index));
        sortOrder.set(index, temp);

        saveSortOrder(sortOrder);
        notifyUpdate();
        return true;
    }

    /**
     * Move a Quick Launch item right (later in the list)
     */
    public boolean moveQuickLaunchItemRight(String key) {
        List<String> sortOrder = getSortOrder();
        int index = sortOrder.indexOf(key);

        // Can't move right if it's already last or not found
        if (index < 0 || index >= sortOrder.size() - 1) {
            return false;
        }

        // Swap with next item in sort order
        String temp = sortOrder.get(index + 1);
        sortOrder.set(index + 1, sortOrder.get(index));
        sortOrder.set(index, temp);

        saveSortOrder(sortOrder);
        notifyUpdate();
        return true;
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
    
    private List<String> getSortOrder() {
        String sortOrderString = preferences.getString(SORT_ORDER_KEY, "");
        List<String> sortOrder = new ArrayList<>();

        if (!sortOrderString.isEmpty()) {
            String[] keys = sortOrderString.split(",");
            for (String key : keys) {
                if (!key.isEmpty()) {
                    sortOrder.add(key);
                }
            }
        }

        return sortOrder;
    }

    private void saveSortOrder(List<String> sortOrder) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < sortOrder.size(); i++) {
            if (i > 0) {
                sb.append(",");
            }
            sb.append(sortOrder.get(i));
        }

        preferences.edit()
                .putString(SORT_ORDER_KEY, sb.toString())
                .apply();
    }

    private void notifyUpdate() {
        Intent updateIntent = new Intent(QUICK_LAUNCH_UPDATE_ACTION);
        // Make it an explicit broadcast by setting the package to avoid restrictions on newer Android versions
        updateIntent.setPackage(context.getPackageName());
        context.sendBroadcast(updateIntent);
    }
}