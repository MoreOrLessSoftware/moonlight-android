package com.limelight.preferences;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.R;
import com.limelight.ui.overlay.CustomCommand;
import com.limelight.ui.overlay.CustomCommandsManager;

import java.util.List;

/**
 * Activity for managing custom overlay menu commands.
 * Allows users to add, edit, and delete custom keyboard commands.
 */
public class CustomCommandsActivity extends Activity {
    private CustomCommandsManager commandsManager;
    private ListView listView;
    private TextView emptyView;
    private View addButton;
    private CommandsAdapter adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_custom_commands);

        commandsManager = new CustomCommandsManager(this);

        // Initialize views
        listView = findViewById(R.id.commands_list);
        emptyView = findViewById(R.id.empty_view);
        addButton = findViewById(R.id.add_button);

        // Setup adapter
        adapter = new CommandsAdapter();
        listView.setAdapter(adapter);

        // Setup item click listener (for editing)
        listView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                CustomCommand command = adapter.getItem(position);
                showEditDialog(command, position);
            }
        });

        // Setup item long click listener (for options menu)
        listView.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, int position, long id) {
                CustomCommand command = adapter.getItem(position);
                showCommandOptionsMenu(command, position);
                return true;
            }
        });

        // Setup add button
        addButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAddDialog();
            }
        });

        updateEmptyView();
    }

    /**
     * Update visibility of empty state view
     */
    private void updateEmptyView() {
        if (adapter.getCount() == 0) {
            emptyView.setVisibility(View.VISIBLE);
            listView.setVisibility(View.GONE);
        } else {
            emptyView.setVisibility(View.GONE);
            listView.setVisibility(View.VISIBLE);
        }
    }

    /**
     * Show dialog to add a new command
     */
    private void showAddDialog() {
        CustomCommandEditorDialog dialog = new CustomCommandEditorDialog();
        dialog.setOnCommandSavedListener(new CustomCommandEditorDialog.OnCommandSavedListener() {
            @Override
            public void onCommandSaved(CustomCommand command) {
                commandsManager.addCommand(command);
                adapter.notifyDataSetChanged();
                updateEmptyView();
                Toast.makeText(CustomCommandsActivity.this,
                    R.string.editor_command_saved, Toast.LENGTH_SHORT).show();
            }
        });
        dialog.show(getFragmentManager(), "add_command");
    }

    /**
     * Show dialog to edit an existing command
     */
    private void showEditDialog(final CustomCommand command, final int position) {
        CustomCommandEditorDialog dialog = CustomCommandEditorDialog.newInstance(command);
        dialog.setOnCommandSavedListener(new CustomCommandEditorDialog.OnCommandSavedListener() {
            @Override
            public void onCommandSaved(CustomCommand updatedCommand) {
                commandsManager.updateCommand(command.getId(), updatedCommand);
                adapter.notifyDataSetChanged();
                Toast.makeText(CustomCommandsActivity.this,
                    R.string.editor_command_saved, Toast.LENGTH_SHORT).show();
            }
        });
        dialog.show(getFragmentManager(), "edit_command");
    }

    /**
     * Show dialog to confirm command deletion
     */
    private void showDeleteDialog(final CustomCommand command, final int position) {
        new AlertDialog.Builder(this)
            .setTitle(R.string.custom_commands_delete)
            .setMessage(R.string.custom_commands_delete_confirm)
            .setPositiveButton(R.string.yes, (dialog, which) -> {
                commandsManager.removeCommand(command.getId());
                adapter.notifyDataSetChanged();
                updateEmptyView();
                Toast.makeText(CustomCommandsActivity.this,
                    R.string.custom_commands_deleted, Toast.LENGTH_SHORT).show();
            })
            .setNegativeButton(R.string.no, null)
            .show();
    }

    /**
     * Show options menu for a command (move up, move down, delete)
     */
    private void showCommandOptionsMenu(final CustomCommand command, final int position) {
        final int itemCount = adapter.getCount();
        final boolean canMoveUp = position > 0;
        final boolean canMoveDown = position < itemCount - 1;

        // Build menu items list
        final CharSequence[] items;
        final int moveUpIndex;
        final int moveDownIndex;
        final int deleteIndex;

        if (canMoveUp && canMoveDown) {
            items = new CharSequence[]{
                getString(R.string.custom_commands_move_up),
                getString(R.string.custom_commands_move_down),
                getString(R.string.custom_commands_delete)
            };
            moveUpIndex = 0;
            moveDownIndex = 1;
            deleteIndex = 2;
        } else if (canMoveUp) {
            items = new CharSequence[]{
                getString(R.string.custom_commands_move_up),
                getString(R.string.custom_commands_delete)
            };
            moveUpIndex = 0;
            moveDownIndex = -1;
            deleteIndex = 1;
        } else if (canMoveDown) {
            items = new CharSequence[]{
                getString(R.string.custom_commands_move_down),
                getString(R.string.custom_commands_delete)
            };
            moveUpIndex = -1;
            moveDownIndex = 0;
            deleteIndex = 1;
        } else {
            // Only one item, just show delete
            items = new CharSequence[]{
                getString(R.string.custom_commands_delete)
            };
            moveUpIndex = -1;
            moveDownIndex = -1;
            deleteIndex = 0;
        }

        new AlertDialog.Builder(this)
            .setTitle(R.string.custom_commands_menu_title)
            .setItems(items, (dialog, which) -> {
                if (which == moveUpIndex) {
                    moveCommandUp(position);
                } else if (which == moveDownIndex) {
                    moveCommandDown(position);
                } else if (which == deleteIndex) {
                    showDeleteDialog(command, position);
                }
            })
            .setNegativeButton(R.string.editor_cancel, null)
            .show();
    }

    /**
     * Move a command up in the list
     */
    private void moveCommandUp(int position) {
        if (position > 0) {
            commandsManager.moveCommand(position, position - 1);
            adapter.notifyDataSetChanged();
        }
    }

    /**
     * Move a command down in the list
     */
    private void moveCommandDown(int position) {
        if (position < adapter.getCount() - 1) {
            commandsManager.moveCommand(position, position + 1);
            adapter.notifyDataSetChanged();
        }
    }

    /**
     * Adapter for displaying custom commands in a list
     */
    private class CommandsAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return commandsManager.getCommands().size();
        }

        @Override
        public CustomCommand getItem(int position) {
            return commandsManager.getCommands().get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            ViewHolder holder;

            if (convertView == null) {
                convertView = LayoutInflater.from(CustomCommandsActivity.this)
                    .inflate(R.layout.list_item_custom_command, parent, false);
                holder = new ViewHolder();
                holder.iconView = convertView.findViewById(R.id.command_icon);
                holder.nameView = convertView.findViewById(R.id.command_name);
                holder.keyComboView = convertView.findViewById(R.id.command_key_combo);
                convertView.setTag(holder);
            } else {
                holder = (ViewHolder) convertView.getTag();
            }

            CustomCommand command = getItem(position);
            holder.iconView.setImageResource(command.getIconResId());
            holder.nameView.setText(command.getName());
            holder.keyComboView.setText(command.getKeyCombination().toDisplayString());

            return convertView;
        }

        private class ViewHolder {
            ImageView iconView;
            TextView nameView;
            TextView keyComboView;
        }
    }
}
