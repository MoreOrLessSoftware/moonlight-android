package com.limelight;

import java.io.IOException;
import java.io.StringReader;
import java.util.HashSet;
import java.util.List;

import com.limelight.computers.ComputerManagerService;
import com.limelight.grid.AppGridAdapter;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.http.PairingManager;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.utils.QuickLaunchManager;
import com.limelight.utils.CacheHelper;
import com.limelight.utils.Dialog;
import com.limelight.utils.ServerHelper;
import com.limelight.utils.ShortcutHelper;
import com.limelight.utils.SpinnerDialog;
import com.limelight.utils.UiHelper;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.os.Build;
import android.view.ContextMenu;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ContextMenu.ContextMenuInfo;
import android.widget.AbsListView;
import android.widget.AdapterView;
import android.widget.AdapterView.OnItemClickListener;
import android.widget.ImageView;
import android.widget.Toast;
import android.widget.AdapterView.AdapterContextMenuInfo;

import org.xmlpull.v1.XmlPullParserException;

// The app grid for one PC. PcView shows it in place of the PC grid, so the
// top bar, quick launch and overrides stay on screen while browsing apps.
public class AppView {
    public interface Host {
        // Called on the UI thread when the app list can no longer be shown
        void closeAppList();
    }

    private final Activity activity;
    private final Host host;
    private final ComputerManagerService.ComputerManagerBinder managerBinder;
    private final ComputerDetails computer;
    private final String uuidString;
    private final AppGridAdapter appGridAdapter;
    private final ShortcutHelper shortcutHelper;
    private final QuickLaunchManager quickLaunchManager;

    private ComputerManagerService.ApplistPoller poller;
    private SpinnerDialog blockingLoadSpinner;
    private String lastRawApplist;
    private int lastRunningAppId;
    private boolean suspendGridUpdates;
    private boolean polling;
    private volatile boolean closed;
    private final HashSet<Integer> hiddenAppIds = new HashSet<>();

    private final static int START_OR_RESUME_ID = 1;
    private final static int QUIT_ID = 2;
    private final static int START_WITH_QUIT = 4;
    private final static int VIEW_DETAILS_ID = 5;
    private final static int CREATE_SHORTCUT_ID = 6;
    private final static int HIDE_APP_ID = 7;
    private final static int APP_SETTINGS_ID = 8;
    private final static int ADD_QUICK_LAUNCH_ID = 9;

    public final static int REQUEST_APP_SETTINGS = 1001;

    public final static String HIDDEN_APPS_PREF_FILENAME = "HiddenApps";

    public final static String NAME_EXTRA = "Name";
    public final static String UUID_EXTRA = "UUID";
    public final static String NEW_PAIR_EXTRA = "NewPair";
    public final static String SHOW_HIDDEN_APPS_EXTRA = "ShowHiddenApps";

    // Must be called on the UI thread
    public AppView(Activity activity, Host host, ComputerManagerService.ComputerManagerBinder managerBinder,
                   final ComputerDetails computer, final boolean newlyPaired, boolean showHiddenApps) {
        this.activity = activity;
        this.host = host;
        this.managerBinder = managerBinder;
        this.computer = computer;
        this.uuidString = computer.uuid;

        shortcutHelper = new ShortcutHelper(activity);
        quickLaunchManager = QuickLaunchManager.getInstance(activity);

        SharedPreferences hiddenAppsPrefs = activity.getSharedPreferences(HIDDEN_APPS_PREF_FILENAME, Context.MODE_PRIVATE);
        for (String hiddenAppIdStr : hiddenAppsPrefs.getStringSet(uuidString, new HashSet<String>())) {
            hiddenAppIds.add(Integer.parseInt(hiddenAppIdStr));
        }

        appGridAdapter = new AppGridAdapter(activity,
                PreferenceConfiguration.readPreferences(activity),
                computer, managerBinder.getUniqueId(),
                showHiddenApps);
        appGridAdapter.updateHiddenApps(hiddenAppIds, true);

        // Add a launcher shortcut for this PC (forced, since this is user interaction).
        // This is off the main thread to prevent ANRs.
        new Thread() {
            @Override
            public void run() {
                shortcutHelper.createAppViewShortcut(computer, true, newlyPaired);
                shortcutHelper.reportComputerShortcutUsed(computer);
            }
        }.start();

        // Load the app grid with cached data (if possible).
        // This must be done _before_ polling starts so the
        // initial serverinfo response can update the running icon.
        populateAppGridWithCache();
    }

    public String getComputerUuid() {
        return uuidString;
    }

    // Safe to call repeatedly; only one polling thread runs at a time
    public synchronized void startPolling() {
        if (closed || polling) {
            return;
        }

        if (poller == null) {
            poller = managerBinder.createAppListPoller(computer);
        }
        poller.start();
        polling = true;
    }

    public synchronized void stopPolling() {
        if (poller != null) {
            poller.stop();
        }
        polling = false;

        appGridAdapter.cancelQueuedOperations();
    }

    public synchronized void close() {
        closed = true;
        stopPolling();

        if (blockingLoadSpinner != null) {
            blockingLoadSpinner.dismiss();
            blockingLoadSpinner = null;
        }
    }

    public void updateLayoutWithPreferences() {
        appGridAdapter.updateLayoutWithPreferences(activity, PreferenceConfiguration.readPreferences(activity));
    }

    public void onAppSettingsResult() {
        // Refresh the grid adapter to update settings indicators
        appGridAdapter.notifyDataSetChanged();
    }

    // Called from the polling thread for every computer update
    public void onComputerUpdated(final ComputerDetails details) {
        // Do nothing if updates are suspended
        if (suspendGridUpdates || closed) {
            return;
        }

        // Don't care about other computers
        if (!details.uuid.equalsIgnoreCase(uuidString)) {
            return;
        }

        if (details.state == ComputerDetails.State.OFFLINE) {
            // The PC is unreachable now
            activity.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (closed) {
                        return;
                    }

                    // Display a toast to the user and go back to the PC list
                    Toast.makeText(activity, activity.getResources().getText(R.string.lost_connection), Toast.LENGTH_SHORT).show();
                    host.closeAppList();
                }
            });

            return;
        }

        // Close immediately if the PC is no longer paired
        if (details.state == ComputerDetails.State.ONLINE && details.pairState != PairingManager.PairState.PAIRED) {
            activity.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (closed) {
                        return;
                    }

                    // Disable shortcuts referencing this PC for now
                    shortcutHelper.disableComputerShortcut(details,
                            activity.getResources().getString(R.string.scut_not_paired));

                    // Display a toast to the user and go back to the PC list
                    Toast.makeText(activity, activity.getResources().getText(R.string.scut_not_paired), Toast.LENGTH_SHORT).show();
                    host.closeAppList();
                }
            });

            return;
        }

        // App list is the same or empty
        if (details.rawAppList == null || details.rawAppList.equals(lastRawApplist)) {

            // Let's check if the running app ID changed
            if (details.runningGameId != lastRunningAppId) {
                // Update the currently running game using the app ID
                lastRunningAppId = details.runningGameId;
                updateUiWithServerinfo(details);
            }

            return;
        }

        lastRunningAppId = details.runningGameId;
        lastRawApplist = details.rawAppList;

        try {
            updateUiWithAppList(NvHTTP.getAppListByReader(new StringReader(details.rawAppList)));
            updateUiWithServerinfo(details);

            if (blockingLoadSpinner != null) {
                blockingLoadSpinner.dismiss();
                blockingLoadSpinner = null;
            }
        } catch (XmlPullParserException | IOException e) {
            e.printStackTrace();
        }
    }

    private void updateHiddenApps(boolean hideImmediately) {
        HashSet<String> hiddenAppIdStringSet = new HashSet<>();

        for (Integer hiddenAppId : hiddenAppIds) {
            hiddenAppIdStringSet.add(hiddenAppId.toString());
        }

        activity.getSharedPreferences(HIDDEN_APPS_PREF_FILENAME, Context.MODE_PRIVATE)
                .edit()
                .putStringSet(uuidString, hiddenAppIdStringSet)
                .apply();

        appGridAdapter.updateHiddenApps(hiddenAppIds, hideImmediately);
    }

    private void populateAppGridWithCache() {
        try {
            // Try to load from cache
            lastRawApplist = CacheHelper.readInputStreamToString(CacheHelper.openCacheFileForInput(activity.getCacheDir(), "applist", uuidString));
            List<NvApp> applist = NvHTTP.getAppListByReader(new StringReader(lastRawApplist));
            updateUiWithAppList(applist);
            LimeLog.info("Loaded applist from cache");
        } catch (IOException | XmlPullParserException e) {
            if (lastRawApplist != null) {
                LimeLog.warning("Saved applist corrupted: "+lastRawApplist);
                e.printStackTrace();
            }
            LimeLog.info("Loading applist from the network");
            // We'll need to load from the network
            loadAppsBlocking();
        }
    }

    private void loadAppsBlocking() {
        blockingLoadSpinner = SpinnerDialog.displayDialog(activity, activity.getResources().getString(R.string.applist_refresh_title),
                activity.getResources().getString(R.string.applist_refresh_msg), false);
    }

    public void onCreateContextMenu(ContextMenu menu, View v, ContextMenuInfo menuInfo) {
        AdapterContextMenuInfo info = (AdapterContextMenuInfo) menuInfo;
        AppObject selectedApp = (AppObject) appGridAdapter.getItem(info.position);

        menu.setHeaderTitle(selectedApp.app.getAppName());

        if (lastRunningAppId != 0) {
            if (lastRunningAppId == selectedApp.app.getAppId()) {
                menu.add(Menu.NONE, START_OR_RESUME_ID, 1, activity.getResources().getString(R.string.applist_menu_resume));
                menu.add(Menu.NONE, QUIT_ID, 2, activity.getResources().getString(R.string.applist_menu_quit));
            }
            else {
                menu.add(Menu.NONE, START_WITH_QUIT, 1, activity.getResources().getString(R.string.applist_menu_quit_and_start));
            }
        }

        // Only show the hide checkbox if this is not the currently running app or it's already hidden
        if (lastRunningAppId != selectedApp.app.getAppId() || selectedApp.isHidden) {
            MenuItem hideAppItem = menu.add(Menu.NONE, HIDE_APP_ID, 3, activity.getResources().getString(R.string.applist_menu_hide_app));
            hideAppItem.setCheckable(true);
            hideAppItem.setChecked(selectedApp.isHidden);
        }

        menu.add(Menu.NONE, VIEW_DETAILS_ID, 4, activity.getResources().getString(R.string.applist_menu_details));
        menu.add(Menu.NONE, APP_SETTINGS_ID, 5, "App Settings");
        menu.add(Menu.NONE, ADD_QUICK_LAUNCH_ID, 6, activity.getResources().getString(R.string.applist_menu_add_quick_launch));

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            // Only add an option to create shortcut if box art is loaded
            // and when we're in grid-mode (not list-mode).
            ImageView appImageView = info.targetView.findViewById(R.id.grid_image);
            if (appImageView != null) {
                // We have a grid ImageView, so we must be in grid-mode
                BitmapDrawable drawable = (BitmapDrawable)appImageView.getDrawable();
                if (drawable != null && drawable.getBitmap() != null) {
                    // We have a bitmap loaded too
                    menu.add(Menu.NONE, CREATE_SHORTCUT_ID, 6, activity.getResources().getString(R.string.applist_menu_scut));
                }
            }
        }
    }

    public boolean onContextItemSelected(MenuItem item) {
        AdapterContextMenuInfo info = (AdapterContextMenuInfo) item.getMenuInfo();
        final AppObject app = (AppObject) appGridAdapter.getItem(info.position);
        switch (item.getItemId()) {
            case START_WITH_QUIT:
                // Display a confirmation dialog first
                UiHelper.displayQuitConfirmationDialog(activity, new Runnable() {
                    @Override
                    public void run() {
                        ServerHelper.doStart(activity, app.app, computer, managerBinder);
                    }
                }, null);
                return true;

            case START_OR_RESUME_ID:
                // Resume is the same as start for us
                ServerHelper.doStart(activity, app.app, computer, managerBinder);
                return true;

            case QUIT_ID:
                // Display a confirmation dialog first
                UiHelper.displayQuitConfirmationDialog(activity, new Runnable() {
                    @Override
                    public void run() {
                        suspendGridUpdates = true;
                        ServerHelper.doQuit(activity, computer,
                                app.app, managerBinder, new Runnable() {
                            @Override
                            public void run() {
                                // Trigger a poll immediately
                                suspendGridUpdates = false;
                                if (poller != null) {
                                    poller.pollNow();
                                }
                            }
                        });
                    }
                }, null);
                return true;

            case VIEW_DETAILS_ID:
                Dialog.displayDialog(activity, activity.getResources().getString(R.string.title_details), app.app.toString(), false);
                return true;

            case APP_SETTINGS_ID:
                Intent settingsIntent = new Intent(activity, com.limelight.preferences.AppStreamSettings.class);
                settingsIntent.putExtra(com.limelight.preferences.AppStreamSettings.EXTRA_APP_KEY, computer.uuid + ":" + app.app.getAppId());
                settingsIntent.putExtra(com.limelight.preferences.AppStreamSettings.EXTRA_APP_NAME, app.app.getAppName());
                activity.startActivityForResult(settingsIntent, REQUEST_APP_SETTINGS);
                return true;

            case HIDE_APP_ID:
                if (item.isChecked()) {
                    // Transitioning hidden to shown
                    hiddenAppIds.remove(app.app.getAppId());
                }
                else {
                    // Transitioning shown to hidden
                    hiddenAppIds.add(app.app.getAppId());
                }
                updateHiddenApps(false);
                return true;

            case CREATE_SHORTCUT_ID:
                ImageView appImageView = info.targetView.findViewById(R.id.grid_image);
                Bitmap appBits = ((BitmapDrawable)appImageView.getDrawable()).getBitmap();
                if (!shortcutHelper.createPinnedGameShortcut(computer, app.app, appBits)) {
                    Toast.makeText(activity, activity.getResources().getString(R.string.unable_to_pin_shortcut), Toast.LENGTH_LONG).show();
                }
                return true;

            case ADD_QUICK_LAUNCH_ID:
                addToQuickLaunch(computer, app.app);
                return true;

            default:
                return false;
        }
    }

    private void updateUiWithServerinfo(final ComputerDetails details) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                boolean updated = false;

                    // Look through our current app list to tag the running app
                for (int i = 0; i < appGridAdapter.getCount(); i++) {
                    AppObject existingApp = (AppObject) appGridAdapter.getItem(i);

                    // There can only be one or zero apps running.
                    if (existingApp.isRunning &&
                            existingApp.app.getAppId() == details.runningGameId) {
                        // This app was running and still is, so we're done now
                        return;
                    }
                    else if (existingApp.app.getAppId() == details.runningGameId) {
                        // This app wasn't running but now is
                        existingApp.isRunning = true;
                        updated = true;
                    }
                    else if (existingApp.isRunning) {
                        // This app was running but now isn't
                        existingApp.isRunning = false;
                        updated = true;
                    }
                    else {
                        // This app wasn't running and still isn't
                    }
                }

                if (updated) {
                    appGridAdapter.notifyDataSetChanged();
                }
            }
        });
    }

    private void updateUiWithAppList(final List<NvApp> appList) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                boolean updated = false;

                // First handle app updates and additions
                for (NvApp app : appList) {
                    boolean foundExistingApp = false;

                    // Try to update an existing app in the list first
                    for (int i = 0; i < appGridAdapter.getCount(); i++) {
                        AppObject existingApp = (AppObject) appGridAdapter.getItem(i);
                        if (existingApp.app.getAppId() == app.getAppId()) {
                            // Found the app; update its properties
                            if (!existingApp.app.getAppName().equals(app.getAppName())) {
                                existingApp.app.setAppName(app.getAppName());
                                updated = true;
                            }

                            foundExistingApp = true;
                            break;
                        }
                    }

                    if (!foundExistingApp) {
                        // This app must be new
                        appGridAdapter.addApp(new AppObject(app));

                        // We could have a leftover shortcut from last time this PC was paired
                        // or if this app was removed then added again. Enable those shortcuts
                        // again if present.
                        shortcutHelper.enableAppShortcut(computer, app);

                        updated = true;
                    }
                }

                // Next handle app removals
                int i = 0;
                while (i < appGridAdapter.getCount()) {
                    boolean foundExistingApp = false;
                    AppObject existingApp = (AppObject) appGridAdapter.getItem(i);

                    // Check if this app is in the latest list
                    for (NvApp app : appList) {
                        if (existingApp.app.getAppId() == app.getAppId()) {
                            foundExistingApp = true;
                            break;
                        }
                    }

                    // This app was removed in the latest app list
                    if (!foundExistingApp) {
                        shortcutHelper.disableAppShortcut(computer, existingApp.app, "App removed from PC");
                        appGridAdapter.removeApp(existingApp);
                        updated = true;

                        // Check this same index again because the item at i+1 is now at i after
                        // the removal
                        continue;
                    }

                    // Move on to the next item
                    i++;
                }

                if (updated) {
                    appGridAdapter.notifyDataSetChanged();
                }
            }
        });
    }

    public int getAdapterFragmentLayoutId() {
        return PreferenceConfiguration.readPreferences(activity).smallIconMode ?
                    R.layout.app_grid_view_small : R.layout.app_grid_view;
    }

    public void receiveAbsListView(AbsListView listView) {
        listView.setAdapter(appGridAdapter);
        listView.setOnItemClickListener(new OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> arg0, View arg1, int pos,
                                    long id) {
                AppObject app = (AppObject) appGridAdapter.getItem(pos);

                // Only open the context menu if something is running, otherwise start it
                if (lastRunningAppId != 0) {
                    activity.openContextMenu(arg1);
                } else {
                    ServerHelper.doStart(activity, app.app, computer, managerBinder);
                }
            }
        });
        UiHelper.applyStatusBarPadding(listView);
        activity.registerForContextMenu(listView);
        listView.requestFocus();
    }

    private void addToQuickLaunch(ComputerDetails computer, NvApp app) {
        quickLaunchManager.addQuickLaunchItem(computer, app);
        Toast.makeText(activity, activity.getResources().getString(R.string.quick_launch_added), Toast.LENGTH_SHORT).show();
    }

    public static class AppObject {
        public final NvApp app;
        public boolean isRunning;
        public boolean isHidden;

        public AppObject(NvApp app) {
            if (app == null) {
                throw new IllegalArgumentException("app must not be null");
            }
            this.app = app;
        }

        @Override
        public String toString() {
            return app.getAppName();
        }
    }
}
