package com.limelight.ui.overlay;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Manages persistence and retrieval of custom commands from SharedPreferences.
 */
public class CustomCommandsManager {
    private static final String PREF_KEY_CUSTOM_COMMANDS = "overlay_custom_commands";
    private final Context context;
    private List<CustomCommand> customCommands;

    public CustomCommandsManager(Context context) {
        this.context = context.getApplicationContext();
        this.customCommands = new ArrayList<>();
        loadCommands();
    }

    /**
     * Load custom commands from SharedPreferences
     */
    private void loadCommands() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        String jsonString = prefs.getString(PREF_KEY_CUSTOM_COMMANDS, null);

        if (jsonString != null && !jsonString.isEmpty()) {
            try {
                JSONArray jsonArray = new JSONArray(jsonString);
                customCommands.clear();

                for (int i = 0; i < jsonArray.length(); i++) {
                    JSONObject jsonObject = jsonArray.getJSONObject(i);
                    CustomCommand command = CustomCommand.fromJson(jsonObject);
                    customCommands.add(command);
                }
            } catch (JSONException e) {
                e.printStackTrace();
                customCommands.clear();
            }
        }
    }

    /**
     * Save custom commands to SharedPreferences
     */
    public void saveCommands() {
        try {
            JSONArray jsonArray = new JSONArray();
            for (CustomCommand command : customCommands) {
                jsonArray.put(command.toJson());
            }

            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
            prefs.edit()
                .putString(PREF_KEY_CUSTOM_COMMANDS, jsonArray.toString())
                .apply();
        } catch (JSONException e) {
            e.printStackTrace();
        }
    }

    /**
     * Get all custom commands
     */
    public List<CustomCommand> getCommands() {
        return new ArrayList<>(customCommands);
    }

    /**
     * Add a new custom command
     */
    public void addCommand(CustomCommand command) {
        customCommands.add(command);
        saveCommands();
    }

    /**
     * Update an existing custom command
     */
    public void updateCommand(String id, CustomCommand updatedCommand) {
        for (int i = 0; i < customCommands.size(); i++) {
            if (customCommands.get(i).getId().equals(id)) {
                customCommands.set(i, updatedCommand);
                saveCommands();
                return;
            }
        }
    }

    /**
     * Remove a custom command by ID
     */
    public void removeCommand(String id) {
        customCommands.removeIf(command -> command.getId().equals(id));
        saveCommands();
    }

    /**
     * Get a custom command by ID
     */
    public CustomCommand getCommandById(String id) {
        for (CustomCommand command : customCommands) {
            if (command.getId().equals(id)) {
                return command;
            }
        }
        return null;
    }

    /**
     * Move a command from one position to another (for reordering)
     */
    public void moveCommand(int fromPosition, int toPosition) {
        if (fromPosition < 0 || fromPosition >= customCommands.size() ||
            toPosition < 0 || toPosition >= customCommands.size()) {
            return;
        }

        CustomCommand command = customCommands.remove(fromPosition);
        customCommands.add(toPosition, command);
        saveCommands();
    }

    /**
     * Clear all custom commands
     */
    public void clearAllCommands() {
        customCommands.clear();
        saveCommands();
    }

    /**
     * Reload commands from SharedPreferences (useful after external changes)
     */
    public void reload() {
        loadCommands();
    }
}
