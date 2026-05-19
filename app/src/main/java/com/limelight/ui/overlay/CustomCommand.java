package com.limelight.ui.overlay;

import android.view.KeyEvent;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Represents a custom key command that can be triggered from the overlay menu.
 * Supports key combinations with modifiers (Ctrl, Alt, Shift, Win/Meta) and special keys.
 */
public class CustomCommand {
    private String id;
    private String name;
    private int iconResId;
    private KeyCombination keyCombination;

    public CustomCommand(String id, String name, int iconResId, KeyCombination keyCombination) {
        this.id = id;
        this.name = name;
        this.iconResId = iconResId;
        this.keyCombination = keyCombination;
    }

    public CustomCommand(String name, int iconResId, KeyCombination keyCombination) {
        this(UUID.randomUUID().toString(), name, iconResId, keyCombination);
    }

    public String getId() {
        return id;
    }

    public String getName() {
        // If name is empty, show the key combination instead
        if (name == null || name.trim().isEmpty()) {
            return keyCombination.toDisplayString();
        }
        return name;
    }

    public void setName(String name) {
        this.name = name;
    }

    public int getIconResId() {
        return iconResId;
    }

    public void setIconResId(int iconResId) {
        this.iconResId = iconResId;
    }

    public KeyCombination getKeyCombination() {
        return keyCombination;
    }

    public void setKeyCombination(KeyCombination keyCombination) {
        this.keyCombination = keyCombination;
    }

    /**
     * Serialize to JSON for storage
     */
    public JSONObject toJson() throws JSONException {
        JSONObject json = new JSONObject();
        json.put("id", id);
        json.put("name", name);
        json.put("iconResId", iconResId);
        json.put("keyCombination", keyCombination.toJson());
        return json;
    }

    /**
     * Deserialize from JSON
     */
    public static CustomCommand fromJson(JSONObject json) throws JSONException {
        return new CustomCommand(
            json.getString("id"),
            json.getString("name"),
            json.getInt("iconResId"),
            KeyCombination.fromJson(json.getJSONObject("keyCombination"))
        );
    }

    /**
     * Represents a key combination with modifiers and a primary key.
     * Supports: Ctrl, Alt, Shift, Meta/Win keys + any primary key code.
     */
    public static class KeyCombination {
        private boolean ctrl;
        private boolean alt;
        private boolean shift;
        private boolean meta; // Windows/Command key
        private int keyCode;

        public KeyCombination(boolean ctrl, boolean alt, boolean shift, boolean meta, int keyCode) {
            this.ctrl = ctrl;
            this.alt = alt;
            this.shift = shift;
            this.meta = meta;
            this.keyCode = keyCode;
        }

        public KeyCombination(int keyCode) {
            this(false, false, false, false, keyCode);
        }

        public boolean isCtrl() {
            return ctrl;
        }

        public void setCtrl(boolean ctrl) {
            this.ctrl = ctrl;
        }

        public boolean isAlt() {
            return alt;
        }

        public void setAlt(boolean alt) {
            this.alt = alt;
        }

        public boolean isShift() {
            return shift;
        }

        public void setShift(boolean shift) {
            this.shift = shift;
        }

        public boolean isMeta() {
            return meta;
        }

        public void setMeta(boolean meta) {
            this.meta = meta;
        }

        public int getKeyCode() {
            return keyCode;
        }

        public void setKeyCode(int keyCode) {
            this.keyCode = keyCode;
        }

        /**
         * Returns a human-readable string representation of the key combination.
         * E.g., "Ctrl+Shift+S", "Win+X", "F11"
         */
        public String toDisplayString() {
            StringBuilder sb = new StringBuilder();

            if (ctrl) sb.append("Ctrl+");
            if (alt) sb.append("Alt+");
            if (shift) sb.append("Shift+");
            if (meta) sb.append("Win/Cmd+");

            // Convert key code to friendly name
            sb.append(getFriendlyKeyName(keyCode));

            return sb.toString();
        }

        /**
         * Get friendly display name for a key code
         */
        private String getFriendlyKeyName(int keyCode) {
            switch (keyCode) {
                case KeyEvent.KEYCODE_META_LEFT: return "Win/Cmd";
                case KeyEvent.KEYCODE_SPACE: return "Space";
                case KeyEvent.KEYCODE_ENTER: return "Enter";
                case KeyEvent.KEYCODE_TAB: return "Tab";
                case KeyEvent.KEYCODE_ESCAPE: return "Escape";
                case KeyEvent.KEYCODE_DEL: return "Backspace";
                case KeyEvent.KEYCODE_FORWARD_DEL: return "Delete";
                case KeyEvent.KEYCODE_DPAD_LEFT: return "Left";
                case KeyEvent.KEYCODE_DPAD_RIGHT: return "Right";
                case KeyEvent.KEYCODE_DPAD_UP: return "Up";
                case KeyEvent.KEYCODE_DPAD_DOWN: return "Down";
                case KeyEvent.KEYCODE_MOVE_HOME: return "Home";
                case KeyEvent.KEYCODE_MOVE_END: return "End";
                case KeyEvent.KEYCODE_PAGE_UP: return "Page Up";
                case KeyEvent.KEYCODE_PAGE_DOWN: return "Page Down";
                case KeyEvent.KEYCODE_INSERT: return "Insert";
                case KeyEvent.KEYCODE_SYSRQ: return "Print Screen";
                case KeyEvent.KEYCODE_BREAK: return "Pause/Break";
                default:
                    // For letter/number keys, use KeyEvent.keyCodeToString
                    String keyName = KeyEvent.keyCodeToString(keyCode);
                    if (keyName.startsWith("KEYCODE_")) {
                        return keyName.substring(8); // Remove "KEYCODE_" prefix
                    }
                    return keyName;
            }
        }

        /**
         * Serialize to JSON
         */
        public JSONObject toJson() throws JSONException {
            JSONObject json = new JSONObject();
            json.put("ctrl", ctrl);
            json.put("alt", alt);
            json.put("shift", shift);
            json.put("meta", meta);
            json.put("keyCode", keyCode);
            return json;
        }

        /**
         * Deserialize from JSON
         */
        public static KeyCombination fromJson(JSONObject json) throws JSONException {
            return new KeyCombination(
                json.getBoolean("ctrl"),
                json.getBoolean("alt"),
                json.getBoolean("shift"),
                json.getBoolean("meta"),
                json.getInt("keyCode")
            );
        }
    }
}
