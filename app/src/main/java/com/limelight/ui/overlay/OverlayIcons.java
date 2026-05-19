package com.limelight.ui.overlay;

import com.limelight.R;

/**
 * Helper class that provides access to all predefined overlay menu icons.
 */
public class OverlayIcons {

    /**
     * Represents an icon option for custom commands
     */
    public static class IconOption {
        public final int resourceId;
        public final String name;

        public IconOption(int resourceId, String name) {
            this.resourceId = resourceId;
            this.name = name;
        }
    }

    /**
     * All available predefined icons for custom commands
     */
    public static final IconOption[] AVAILABLE_ICONS = {
        new IconOption(R.drawable.ic_overlay_key_press, "Key Press"),
        new IconOption(R.drawable.ic_overlay_keyboard, "Keyboard"),
        new IconOption(R.drawable.ic_overlay_gamepad, "Gamepad"),
        new IconOption(R.drawable.ic_overlay_screenshot, "Screenshot"),
        new IconOption(R.drawable.ic_overlay_microphone, "Microphone"),
        new IconOption(R.drawable.ic_overlay_volume, "Volume"),
        new IconOption(R.drawable.ic_overlay_settings, "Settings"),
        new IconOption(R.drawable.ic_overlay_star, "Star"),
        new IconOption(R.drawable.ic_overlay_lightning, "Lightning"),
        new IconOption(R.drawable.ic_overlay_monitor, "Monitor"),
        new IconOption(R.drawable.ic_overlay_folder, "Folder"),
        new IconOption(R.drawable.ic_overlay_play, "Play"),
        new IconOption(R.drawable.ic_overlay_stop, "Stop"),
        new IconOption(R.drawable.ic_overlay_record, "Record"),
        new IconOption(R.drawable.ic_overlay_camera, "Camera"),
        new IconOption(R.drawable.ic_overlay_power, "Power"),
        new IconOption(R.drawable.ic_overlay_restart, "Restart"),
        new IconOption(R.drawable.ic_overlay_sleep, "Sleep"),
        new IconOption(R.drawable.ic_overlay_volume_up, "Volume Up"),
        new IconOption(R.drawable.ic_overlay_volume_down, "Volume Down"),
        new IconOption(R.drawable.ic_overlay_volume_mute, "Mute"),
        new IconOption(R.drawable.ic_overlay_media_next, "Next Track"),
        new IconOption(R.drawable.ic_overlay_media_previous, "Previous Track"),
        new IconOption(R.drawable.ic_overlay_settings_brightness, "Brightness"),
        new IconOption(R.drawable.ic_overlay_contrast, "Contrast"),
        new IconOption(R.drawable.ic_overlay_hdr, "HDR"),
        new IconOption(R.drawable.ic_overlay_windows, "Windows Key"),
        new IconOption(R.drawable.ic_overlay_save, "Save"),
        new IconOption(R.drawable.ic_overlay_delete, "Delete"),
        new IconOption(R.drawable.ic_overlay_minimize, "Minimize"),
        new IconOption(R.drawable.ic_overlay_maximize, "Maximize"),
        new IconOption(R.drawable.ic_overlay_restore, "Restore Window"),
        new IconOption(R.drawable.ic_overlay_fullscreen, "Fullscreen"),
        new IconOption(R.drawable.ic_overlay_fullscreen_exit, "Exit Fullscreen"),
        new IconOption(R.drawable.ic_overlay_snap_left, "Snap Left"),
        new IconOption(R.drawable.ic_overlay_snap_right, "Snap Right"),
        new IconOption(R.drawable.ic_overlay_task_manager, "Task Manager"),
        new IconOption(R.drawable.ic_overlay_desktop, "Show Desktop"),
        new IconOption(R.drawable.ic_overlay_window_menu, "Window Menu")
    };

    /**
     * Get the name of an icon by its resource ID
     */
    public static String getIconName(int resourceId) {
        for (IconOption icon : AVAILABLE_ICONS) {
            if (icon.resourceId == resourceId) {
                return icon.name;
            }
        }
        return "Unknown";
    }

    /**
     * Get the resource ID of an icon by its name, falling back to the default icon if not found.
     */
    public static int getIconByName(String name) {
        for (IconOption icon : AVAILABLE_ICONS) {
            if (icon.name.equals(name)) {
                return icon.resourceId;
            }
        }
        return getDefaultIcon();
    }

    /**
     * Get the default icon (key press)
     */
    public static int getDefaultIcon() {
        return R.drawable.ic_overlay_key_press;
    }
}
