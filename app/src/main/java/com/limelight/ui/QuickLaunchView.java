package com.limelight.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.text.InputType;
import android.view.ContextMenu;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import com.limelight.R;
import com.limelight.computers.ComputerManagerService;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.preferences.AppStreamSettings;
import com.limelight.utils.QuickLaunchManager;
import com.limelight.utils.ServerHelper;
import com.limelight.utils.UiHelper;

import java.util.List;

public class QuickLaunchView {
    private static final int QUICK_LAUNCH_RENAME_ID = 1001;
    private static final int QUICK_LAUNCH_SETTINGS_ID = 1002;
    private static final int QUICK_LAUNCH_DELETE_ID = 1003;
    private static final int QUICK_LAUNCH_QUIT_ID = 1004;
    
    public interface QuickLaunchCallback {
        ComputerManagerService.ComputerManagerBinder getManagerBinder();
    }
    
    private final Activity activity;
    private final LinearLayout quickLaunchSection;
    private final LinearLayout quickLaunchContainer;
    private final QuickLaunchManager quickLaunchManager;
    private final QuickLaunchCallback callback;
    
    private String contextMenuQuickLaunchKey;
    private boolean receiverRegistered = false;
    
    private final BroadcastReceiver quickLaunchUpdateReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (QuickLaunchManager.QUICK_LAUNCH_UPDATE_ACTION.equals(intent.getAction())) {
                loadQuickLaunchButtons();
            }
        }
    };
    
    public QuickLaunchView(Activity activity, LinearLayout quickLaunchSection, 
                          LinearLayout quickLaunchContainer, QuickLaunchCallback callback) {
        this.activity = activity;
        this.quickLaunchSection = quickLaunchSection;
        this.quickLaunchContainer = quickLaunchContainer;
        this.quickLaunchManager = new QuickLaunchManager(activity);
        this.callback = callback;
        
        // Set up running status listener
        this.quickLaunchManager.setRunningStatusListener(new QuickLaunchManager.RunningStatusListener() {
            @Override
            public void onRunningStatusChanged(int runningAppId) {
                updateButtonColors();
            }
        });
        
        loadQuickLaunchButtons();
    }
    
    /**
     * Add an app to Quick Launch
     */
    public void addToQuickLaunch(ComputerDetails computer, NvApp app) {
        quickLaunchManager.addQuickLaunchItem(computer, app);
        Toast.makeText(activity, activity.getString(R.string.quick_launch_added), Toast.LENGTH_SHORT).show();
    }
    
    /**
     * Handle context menu creation for Quick Launch buttons
     */
    public boolean onCreateContextMenu(ContextMenu menu, View v, ContextMenu.ContextMenuInfo menuInfo) {
        if (v.getTag() != null && v.getTag().toString().startsWith("quicklaunch:")) {
            // Extract the key from the tag (format: "quicklaunch:key")
            String tag = v.getTag().toString();
            String key = tag.substring("quicklaunch:".length());
            contextMenuQuickLaunchKey = key;
            
            // Get the app ID for this Quick Launch item
            QuickLaunchManager.QuickLaunchItem item = getQuickLaunchItemByKey(key);
            if (item != null) {
                menu.setHeaderTitle(item.getDisplayName());
                
                // Add quit option if this app is running
                if (quickLaunchManager.isAppRunning(item.appId)) {
                    menu.add(Menu.NONE, QUICK_LAUNCH_QUIT_ID, 1, activity.getString(R.string.applist_menu_quit));
                }
                
                menu.add(Menu.NONE, QUICK_LAUNCH_RENAME_ID, 2, activity.getString(R.string.quick_launch_rename));
                menu.add(Menu.NONE, QUICK_LAUNCH_SETTINGS_ID, 3, activity.getString(R.string.quick_launch_settings));
                menu.add(Menu.NONE, QUICK_LAUNCH_DELETE_ID, 4, activity.getString(R.string.quick_launch_delete));
            }
            return true;
        }
        return false;
    }
    
    /**
     * Handle context menu item selection
     */
    public boolean onContextItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case QUICK_LAUNCH_QUIT_ID:
                quitQuickLaunchApp(contextMenuQuickLaunchKey);
                return true;
                
            case QUICK_LAUNCH_RENAME_ID:
                showRenameQuickLaunchDialog(contextMenuQuickLaunchKey);
                return true;
                
            case QUICK_LAUNCH_SETTINGS_ID:
                openQuickLaunchSettings(contextMenuQuickLaunchKey);
                return true;
                
            case QUICK_LAUNCH_DELETE_ID:
                removeFromQuickLaunch(contextMenuQuickLaunchKey);
                return true;
                
            default:
                return false;
        }
    }
    
    /**
     * Register broadcast receiver for updates
     */
    public void onResume() {
        if (!receiverRegistered) {
            IntentFilter filter = new IntentFilter(QuickLaunchManager.QUICK_LAUNCH_UPDATE_ACTION);
            activity.registerReceiver(quickLaunchUpdateReceiver, filter);
            receiverRegistered = true;
        }
        
        // Force an immediate refresh to ensure we show any recently added items
        loadQuickLaunchButtons();
    }
    
    /**
     * Unregister broadcast receiver
     */
    public void onPause() {
        unregisterReceiver();
    }
    
    /**
     * Cleanup on destroy
     */
    public void onDestroy() {
        unregisterReceiver();
    }
    
    private void unregisterReceiver() {
        if (receiverRegistered) {
            try {
                activity.unregisterReceiver(quickLaunchUpdateReceiver);
                receiverRegistered = false;
            } catch (IllegalArgumentException e) {
                // Receiver not registered, ignore
                receiverRegistered = false;
            }
        }
    }
    
    private void loadQuickLaunchButtons() {
        if (quickLaunchContainer == null) {
            return; // Not initialized yet
        }
        
        // Clear existing buttons
        quickLaunchContainer.removeAllViews();
        
        // Get all Quick Launch items
        List<QuickLaunchManager.QuickLaunchItem> items = quickLaunchManager.getAllQuickLaunchItems();
        
        if (items.isEmpty()) {
            quickLaunchSection.setVisibility(View.GONE);
            return;
        }
        
        quickLaunchSection.setVisibility(View.VISIBLE);
        
        for (QuickLaunchManager.QuickLaunchItem item : items) {
            Button quickLaunchButton = createQuickLaunchButton(item);
            quickLaunchContainer.addView(quickLaunchButton);
        }
    }
    
    private Button createQuickLaunchButton(final QuickLaunchManager.QuickLaunchItem item) {
        Button button = new Button(activity);
        button.setText(item.getDisplayName());
        button.setTextSize(12);
        
        // Set a tag to identify this as a Quick Launch button for context menus
        button.setTag("quicklaunch:" + item.key);
        
        // Set button dimensions and margins
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.setMargins(8, 0, 8, 0);
        button.setLayoutParams(params);
        
        // Update button color based on running status
        updateButtonColor(button, item);
        
        // Set click listener to launch the app
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                launchQuickLaunchApp(item);
            }
        });
        
        // Register for context menu
        activity.registerForContextMenu(button);
        
        return button;
    }
    
    private void launchQuickLaunchApp(QuickLaunchManager.QuickLaunchItem item) {
        ComputerManagerService.ComputerManagerBinder managerBinder = callback.getManagerBinder();
        if (managerBinder == null) {
            Toast.makeText(activity, "Computer manager not ready", Toast.LENGTH_SHORT).show();
            return;
        }
        
        // Find the computer by UUID
        ComputerDetails computer = managerBinder.getComputer(item.computerUuid);
        if (computer == null) {
            Toast.makeText(activity, "PC not found", Toast.LENGTH_SHORT).show();
            return;
        }
        
        // Create NvApp object with the appId
        NvApp app = new NvApp(item.originalAppName, item.appId, false);
        
        // Use ServerHelper to launch the app with the Quick Launch key
        ServerHelper.doStart(activity, app, computer, managerBinder, item.key);
    }
    
    private void openQuickLaunchSettings(String key) {
        // Get the Quick Launch item details
        List<QuickLaunchManager.QuickLaunchItem> items = quickLaunchManager.getAllQuickLaunchItems();
        QuickLaunchManager.QuickLaunchItem targetItem = null;
        
        for (QuickLaunchManager.QuickLaunchItem item : items) {
            if (item.key.equals(key)) {
                targetItem = item;
                break;
            }
        }
        
        if (targetItem != null) {
            // Open AppStreamSettings with the Quick Launch key
            Intent settingsIntent = new Intent(activity, AppStreamSettings.class);
            settingsIntent.putExtra(AppStreamSettings.EXTRA_APP_KEY, key);
            settingsIntent.putExtra(AppStreamSettings.EXTRA_APP_NAME, "Quick Launch: " + targetItem.getDisplayName());
            activity.startActivity(settingsIntent);
        }
    }
    
    private void showRenameQuickLaunchDialog(final String key) {
        // Get current custom name from the key
        String currentCustomName = quickLaunchManager.getCustomName(key);
        String originalName = quickLaunchManager.getOriginalName(key);
        
        // Create the input dialog
        AlertDialog.Builder builder = new AlertDialog.Builder(activity);
        builder.setTitle(activity.getString(R.string.quick_launch_rename_title));
        builder.setMessage("Display name for " + originalName + ":");
        
        final EditText input = new EditText(activity);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setText(currentCustomName);
        input.selectAll();
        builder.setView(input);
        
        builder.setPositiveButton("OK", new android.content.DialogInterface.OnClickListener() {
            @Override
            public void onClick(android.content.DialogInterface dialog, int which) {
                String newCustomName = input.getText().toString().trim();
                if (!newCustomName.isEmpty()) {
                    quickLaunchManager.updateCustomName(key, newCustomName);
                    Toast.makeText(activity, activity.getString(R.string.quick_launch_renamed), Toast.LENGTH_SHORT).show();
                }
            }
        });
        builder.setNegativeButton("Cancel", new android.content.DialogInterface.OnClickListener() {
            @Override
            public void onClick(android.content.DialogInterface dialog, int which) {
                dialog.cancel();
            }
        });
        
        builder.show();
    }
    
    private void removeFromQuickLaunch(String key) {
        quickLaunchManager.removeQuickLaunchItem(key);
        Toast.makeText(activity, activity.getString(R.string.quick_launch_removed), Toast.LENGTH_SHORT).show();
    }
    
    private QuickLaunchManager.QuickLaunchItem getQuickLaunchItemByKey(String key) {
        List<QuickLaunchManager.QuickLaunchItem> items = quickLaunchManager.getAllQuickLaunchItems();
        for (QuickLaunchManager.QuickLaunchItem item : items) {
            if (item.key.equals(key)) {
                return item;
            }
        }
        return null;
    }
    
    private void updateButtonColor(Button button, QuickLaunchManager.QuickLaunchItem item) {
        if (quickLaunchManager.isAppRunning(item.appId)) {
            // Set green background for running apps
            button.setBackgroundColor(Color.GREEN);
            button.setTextColor(Color.BLACK);
        } else {
            // Reset to default button appearance
            button.setBackgroundColor(Color.DKGRAY);
            button.setTextColor(Color.WHITE);
        }
    }
    
    private void updateButtonColors() {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                // Update all button colors
                for (int i = 0; i < quickLaunchContainer.getChildCount(); i++) {
                    View child = quickLaunchContainer.getChildAt(i);
                    if (child instanceof Button && child.getTag() != null && 
                        child.getTag().toString().startsWith("quicklaunch:")) {
                        
                        Button button = (Button) child;
                        String tag = button.getTag().toString();
                        String key = tag.substring("quicklaunch:".length());
                        QuickLaunchManager.QuickLaunchItem item = getQuickLaunchItemByKey(key);
                        
                        if (item != null) {
                            updateButtonColor(button, item);
                        }
                    }
                }
            }
        });
    }
    
    private void quitQuickLaunchApp(String key) {
        QuickLaunchManager.QuickLaunchItem item = getQuickLaunchItemByKey(key);
        if (item == null) {
            Toast.makeText(activity, "Quick Launch item not found", Toast.LENGTH_SHORT).show();
            return;
        }
        
        ComputerManagerService.ComputerManagerBinder managerBinder = callback.getManagerBinder();
        if (managerBinder == null) {
            Toast.makeText(activity, "Computer manager not ready", Toast.LENGTH_SHORT).show();
            return;
        }
        
        // Find the computer by UUID
        ComputerDetails computer = managerBinder.getComputer(item.computerUuid);
        if (computer == null) {
            Toast.makeText(activity, "PC not found", Toast.LENGTH_SHORT).show();
            return;
        }
        
        // Create NvApp object with the appId
        NvApp app = new NvApp(item.originalAppName, item.appId, false);
        
        // Display a confirmation dialog first
        UiHelper.displayQuitConfirmationDialog(activity, new Runnable() {
            @Override
            public void run() {
                // Use ServerHelper to quit the app
                ServerHelper.doQuit(activity, computer, app, managerBinder, new Runnable() {
                    @Override
                    public void run() {
                        // Update running status after quit
                        quickLaunchManager.updateRunningAppId(0);
                    }
                });
            }
        }, null);
    }
    
    /**
     * Update running app status from external source (e.g., PcView)
     */
    public void updateRunningStatus(int runningAppId) {
        quickLaunchManager.updateRunningAppId(runningAppId);
    }
}