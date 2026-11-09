package com.limelight.ui.overlay;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Commands the host offers for the overlay menu, which run on the host (Sunshine's client
 * commands): its /clientcommands answer.
 */
public final class HostCommands {
    public static final HostCommands NONE = new HostCommands(Collections.emptyList(), Collections.emptyList());

    /** One command, which runs on the host when picked */
    public static final class Command {
        public final String id;
        public final String label;
        // A Material Symbols icon as "material:<name>", or null. Other kinds of icon from the host
        // are dropped.
        public final String icon;
        // Whether the menu closes once the command has run
        public final boolean closeMenu;

        Command(String id, String label, String icon, boolean closeMenu) {
            this.id = id;
            this.label = label;
            this.icon = icon;
            this.closeMenu = closeMenu;
        }
    }

    /** Commands shown together on a page of the menu, opened by a button in the row */
    public static final class Group {
        // Where it comes from on the host: "app", "program" or "plugin"
        public final String kind;
        public final String label;
        public final String icon;
        public final List<Command> commands;

        Group(String kind, String label, String icon, List<Command> commands) {
            this.kind = kind;
            this.label = label;
            this.icon = icon;
            this.commands = commands;
        }

        /** The group's icon, or one for where it comes from */
        public String iconOrDefault() {
            if (icon != null) {
                return icon;
            }
            switch (kind) {
                case "app": return "material:videogame_asset";
                case "program": return "material:web_asset";
                case "plugin": return "material:extension";
                default: return "material:folder";
            }
        }
    }

    public final List<Command> commands;
    public final List<Group> groups;

    private HostCommands(List<Command> commands, List<Group> groups) {
        this.commands = commands;
        this.groups = groups;
    }

    public boolean isEmpty() {
        return commands.isEmpty() && groups.isEmpty();
    }

    private static String icon(JSONObject object) {
        String icon = object.optString("icon", "");
        return icon.startsWith("material:") ? icon : null;
    }

    private static List<Command> readCommands(JSONArray array) {
        List<Command> commands = new ArrayList<>();
        if (array == null) {
            return commands;
        }
        for (int i = 0; i < array.length(); i++) {
            JSONObject object = array.optJSONObject(i);
            if (object == null) {
                continue;
            }
            String id = object.optString("id", "");
            String label = object.optString("label", "");
            if (!id.isEmpty() && !label.isEmpty()) {
                commands.add(new Command(id, label, icon(object), object.optBoolean("close-menu", true)));
            }
        }
        return commands;
    }

    /** Reads the host's /clientcommands answer, or returns null if it isn't one */
    public static HostCommands fromJson(String json) {
        try {
            JSONObject root = new JSONObject(json);
            if (!root.has("version")) {
                return null;
            }

            List<Group> groups = new ArrayList<>();
            JSONArray groupArray = root.optJSONArray("groups");
            if (groupArray != null) {
                for (int i = 0; i < groupArray.length(); i++) {
                    JSONObject object = groupArray.optJSONObject(i);
                    if (object == null) {
                        continue;
                    }
                    String label = object.optString("label", "");
                    List<Command> commands = readCommands(object.optJSONArray("commands"));
                    if (!label.isEmpty() && !commands.isEmpty()) {
                        groups.add(new Group(object.optString("kind", ""), label, icon(object), commands));
                    }
                }
            }

            return new HostCommands(readCommands(root.optJSONArray("commands")), groups);
        } catch (JSONException e) {
            return null;
        }
    }
}
