package com.limelight;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;


import com.limelight.utils.QuickLaunchManager;
import com.limelight.utils.UiHelper;

import java.util.List;

/**
 * Activity that allows users to pick a Quick Launch item to create a shortcut for.
 * This is called when the user tries to add a Moonlight shortcut from their launcher or other apps.
 */
public class QuickLaunchShortcutPicker extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        UiHelper.setLocale(this);
        setContentView(R.layout.activity_shortcut_picker);

        // Get all Quick Launch items
        QuickLaunchManager quickLaunchManager = QuickLaunchManager.getInstance(this);
        final List<QuickLaunchManager.QuickLaunchItem> items = quickLaunchManager.getAllQuickLaunchItems();

        if (items.isEmpty()) {
            Toast.makeText(this, "No Quick Launch items found. Please create some first.", Toast.LENGTH_LONG).show();
            setResult(RESULT_CANCELED);
            finish();
            return;
        }

        // Set up the list view with custom adapter
        ListView listView = findViewById(R.id.shortcut_list);

        ArrayAdapter<QuickLaunchManager.QuickLaunchItem> adapter = new ArrayAdapter<QuickLaunchManager.QuickLaunchItem>(
                this, R.layout.list_item_quick_launch, items) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View view = convertView;
                if (view == null) {
                    view = LayoutInflater.from(getContext()).inflate(R.layout.list_item_quick_launch, parent, false);
                }

                QuickLaunchManager.QuickLaunchItem item = getItem(position);
                if (item != null) {
                    TextView customNameView = view.findViewById(R.id.custom_name);
                    TextView originalAppNameView = view.findViewById(R.id.original_app_name);

                    customNameView.setText(item.customName);
                    originalAppNameView.setText(item.originalAppName);
                }

                return view;
            }
        };

        listView.setAdapter(adapter);

        // Handle item selection
        listView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                QuickLaunchManager.QuickLaunchItem selectedItem = items.get(position);
                createShortcut(selectedItem);
            }
        });

        UiHelper.applyStatusBarPadding(listView);
    }

    private void createShortcut(QuickLaunchManager.QuickLaunchItem item) {
        // Create the intent that will be launched when the shortcut is tapped
        Intent launchIntent = new Intent(this, ShortcutTrampoline.class);
        launchIntent.setAction(Intent.ACTION_VIEW);
        launchIntent.putExtra(ShortcutTrampoline.EXTRA_QUICK_LAUNCH_NAME, item.customName);
        launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);

        // Create the shortcut result that will be returned to the caller
        Intent shortcutIntent = new Intent();
        shortcutIntent.putExtra(Intent.EXTRA_SHORTCUT_INTENT, launchIntent);
        shortcutIntent.putExtra(Intent.EXTRA_SHORTCUT_NAME, item.customName);

        // Use adaptive icon on Android 8.0+, simple icon on older versions
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            // Use adaptive icon resource with gradient background
            shortcutIntent.putExtra(Intent.EXTRA_SHORTCUT_ICON_RESOURCE,
                    Intent.ShortcutIconResource.fromContext(this, R.mipmap.ic_shortcut_quick_launch));
        } else {
            // Use simple play icon for pre-8.0 devices
            shortcutIntent.putExtra(Intent.EXTRA_SHORTCUT_ICON_RESOURCE,
                    Intent.ShortcutIconResource.fromContext(this, R.drawable.ic_play));
        }

        // Return the shortcut to the caller (launcher, automation app, etc.)
        setResult(RESULT_OK, shortcutIntent);
        finish();
    }
}
