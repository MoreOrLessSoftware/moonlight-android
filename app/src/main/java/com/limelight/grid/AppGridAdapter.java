package com.limelight.grid;

import android.content.Context;
import android.graphics.BitmapFactory;
import android.text.TextUtils;
import android.view.View;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import com.limelight.AppView;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.grid.assets.CachedAppAssetLoader;
import com.limelight.grid.assets.DiskAssetLoader;
import com.limelight.grid.assets.MemoryAssetLoader;
import com.limelight.grid.assets.NetworkAssetLoader;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.preferences.AppPreferences;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.utils.UiHelper;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

@SuppressWarnings("unchecked")
public class AppGridAdapter extends GenericGridAdapter<AppView.AppObject> {
    private static final int ART_WIDTH_PX = 300;
    private static final int COMPACT_WIDTH_DP = 115;
    private static final int CARD_WIDTH_DP = 96;
    private static final int LARGE_WIDTH_DP = 150;

    private final ComputerDetails computer;
    private final String uniqueId;
    private final boolean showHiddenApps;

    private CachedAppAssetLoader loader;
    private Set<Integer> hiddenAppIds = new HashSet<>();
    private ArrayList<AppView.AppObject> allApps = new ArrayList<>();

    public AppGridAdapter(Context context, PreferenceConfiguration prefs, ComputerDetails computer, String uniqueId, boolean showHiddenApps) {
        super(context, getLayoutIdForPreferences(prefs));

        this.computer = computer;
        this.uniqueId = uniqueId;
        this.showHiddenApps = showHiddenApps;

        updateLayoutWithPreferences(context, prefs);
    }

    public void updateHiddenApps(Set<Integer> newHiddenAppIds, boolean hideImmediately) {
        this.hiddenAppIds.clear();
        this.hiddenAppIds.addAll(newHiddenAppIds);

        if (hideImmediately) {
            // Reconstruct the itemList with the new hidden app set
            itemList.clear();
            for (AppView.AppObject app : allApps) {
                app.isHidden = hiddenAppIds.contains(app.app.getAppId());

                if (!app.isHidden || showHiddenApps) {
                    itemList.add(app);
                }
            }
        }
        else {
            // Just update the isHidden state to show the correct UI indication
            for (AppView.AppObject app : allApps) {
                app.isHidden = hiddenAppIds.contains(app.app.getAppId());
            }
        }

        notifyDataSetChanged();
    }

    private static int getLayoutIdForPreferences(PreferenceConfiguration prefs) {
        switch (prefs.appGridMode) {
            case PreferenceConfiguration.APP_GRID_MODE_COMPACT:
                return R.layout.app_grid_item_compact;
            case PreferenceConfiguration.APP_GRID_MODE_CARDS:
                return R.layout.app_grid_item_card;
            default:
                return R.layout.app_grid_item;
        }
    }

    public void updateLayoutWithPreferences(Context context, PreferenceConfiguration prefs) {
        int dpi = context.getResources().getDisplayMetrics().densityDpi;
        int dp;

        switch (prefs.appGridMode) {
            case PreferenceConfiguration.APP_GRID_MODE_COMPACT:
                dp = COMPACT_WIDTH_DP;
                break;
            case PreferenceConfiguration.APP_GRID_MODE_CARDS:
                dp = CARD_WIDTH_DP;
                break;
            default:
                dp = LARGE_WIDTH_DP;
                break;
        }

        double scalingDivisor = ART_WIDTH_PX / (dp * (dpi / 160.0));
        if (scalingDivisor < 1.0) {
            // We don't want to make them bigger before draw-time
            scalingDivisor = 1.0;
        }
        LimeLog.info("Art scaling divisor: " + scalingDivisor);

        if (loader != null) {
            // Cancel operations on the old loader
            cancelQueuedOperations();
        }

        this.loader = new CachedAppAssetLoader(computer, scalingDivisor,
                new NetworkAssetLoader(context, uniqueId),
                new MemoryAssetLoader(),
                new DiskAssetLoader(context),
                BitmapFactory.decodeResource(context.getResources(), R.drawable.no_app_image));

        // This will trigger the view to reload with the new layout
        setLayoutId(getLayoutIdForPreferences(prefs));
    }

    public void cancelQueuedOperations() {
        loader.cancelForegroundLoads();
        loader.cancelBackgroundLoads();
        loader.freeCacheMemory();
    }

    private static void sortList(List<AppView.AppObject> list) {
        Collections.sort(list, new Comparator<AppView.AppObject>() {
            @Override
            public int compare(AppView.AppObject lhs, AppView.AppObject rhs) {
                return lhs.app.getAppName().toLowerCase().compareTo(rhs.app.getAppName().toLowerCase());
            }
        });
    }

    public void addApp(AppView.AppObject app) {
        // Update hidden state
        app.isHidden = hiddenAppIds.contains(app.app.getAppId());

        // Always add the app to the all apps list
        allApps.add(app);
        sortList(allApps);

        // Add the app to the adapter data if it's not hidden
        if (showHiddenApps || !app.isHidden) {
            // Queue a request to fetch this bitmap into cache
            loader.queueCacheLoad(app.app);

            // Add the app to our sorted list
            itemList.add(app);
            sortList(itemList);
        }
    }

    public void removeApp(AppView.AppObject app) {
        itemList.remove(app);
        allApps.remove(app);
    }

    @Override
    public void clear() {
        super.clear();
        allApps.clear();
    }

    @Override
    public void populateView(View parentView, ImageView imgView, ProgressBar prgView, TextView txtView, ImageView overlayView, ImageView settingsIndicator, AppView.AppObject obj) {
        // Round the box art's corners to its rounded_image_mask background. The layouts'
        // android:clipToOutline is only read from XML on Android 12 and newer.
        imgView.setClipToOutline(true);

        // Let the cached asset loader handle it
        loader.populateImageView(obj.app, imgView, txtView);

        if (obj.isRunning) {
            // Show the play button overlay
            overlayView.setImageResource(R.drawable.ic_play);
            overlayView.setVisibility(View.VISIBLE);
        }
        else {
            overlayView.setVisibility(View.GONE);
        }

        // And a border, so the running app is easy to spot in the grid
        View runningBorder = parentView.findViewById(R.id.running_border);
        if (runningBorder != null) {
            runningBorder.setVisibility(obj.isRunning ? View.VISIBLE : View.GONE);
        }

        String appKey = this.computer.uuid + ":" + obj.app.getAppId();
        AppPreferences.AppSettings appSettings = AppPreferences.getAppSettings(context, appKey);

        // Cards always show the name, with the app's settings overrides below it
        TextView cardTitle = parentView.findViewById(R.id.card_title);
        if (cardTitle != null) {
            cardTitle.setText(obj.app.getAppName());
        }
        TextView cardDetails = parentView.findViewById(R.id.card_details);
        if (cardDetails != null) {
            String overrides = appSettings.useGlobalSettings ? "" : describeOverrides(appSettings);
            cardDetails.setText(overrides);
            cardDetails.setVisibility(overrides.isEmpty() ? View.GONE : View.VISIBLE);
        }

        // Show settings indicator if app has custom settings (use global settings is false)
        if (settingsIndicator != null) {
            if (!appSettings.useGlobalSettings) {
                settingsIndicator.setVisibility(View.VISIBLE);
            } else {
                settingsIndicator.setVisibility(View.GONE);
            }
        }

        if (obj.isHidden) {
            parentView.setAlpha(0.40f);
        }
        else {
            parentView.setAlpha(1.0f);
        }
    }

    // The codec's short name, since the setting's names are too long for a card
    private static String describeVideoFormat(String videoFormat) {
        switch (videoFormat) {
            case "forceav1":
                return "AV1";
            case "forceh265":
                return "HEVC";
            case "neverh265":
                return "H.264";
            case "forcepyrowave":
                return "PyroWave";
            default:
                return "Auto codec";
        }
    }

    // A short summary of the settings this app overrides, e.g. "2560×1440 · 120 FPS · 50 Mbps"
    private String describeOverrides(AppPreferences.AppSettings settings) {
        List<String> parts = new ArrayList<>();

        if (settings.resolution != null) {
            parts.add(settings.resolution.replace('x', '×'));
        }
        if (settings.fps > 0) {
            parts.add(settings.fps + " FPS");
        }
        if (settings.videoFormat != null) {
            parts.add(describeVideoFormat(settings.videoFormat));
        }
        if (settings.bitrate > 0) {
            // Bitrates are saved in Kbps
            parts.add(UiHelper.formatRate(settings.bitrate / 1000.0, " Mbps"));
        }
        if (settings.framePacing != null) {
            String[] names = context.getResources().getStringArray(R.array.video_frame_pacing_names);
            String[] values = context.getResources().getStringArray(R.array.video_frame_pacing_values);
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(settings.framePacing)) {
                    parts.add(names[i]);
                    break;
                }
            }
        }
        if (settings.jitterBuffer != null) {
            String[] names = context.getResources().getStringArray(R.array.jitter_buffer_names);
            String[] values = context.getResources().getStringArray(R.array.jitter_buffer_values);
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(settings.jitterBuffer)) {
                    // Without a "(Recommended)" tag, which doesn't fit on a card
                    parts.add("Jitter: " + names[i].replaceAll(" \\(.*\\)$", ""));
                    break;
                }
            }
        }
        if (settings.actualDisplayRefreshRate > 0) {
            parts.add("Display " + UiHelper.formatRefreshRate(settings.actualDisplayRefreshRate));
        }
        if (settings.enableHdr != null) {
            parts.add(Boolean.parseBoolean(settings.enableHdr) ? "HDR on" : "HDR off");
        }
        if (settings.enablePerfOverlay != null) {
            parts.add(Boolean.parseBoolean(settings.enablePerfOverlay) ? "Perf overlay on" : "Perf overlay off");
        }

        return TextUtils.join(" · ", parts);
    }
}
