package com.github.stenzek.duckstation;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;

import androidx.preference.PreferenceManager;

public class UpdateNotes {
    private static final int VERSION_CONTROLLER_UPDATE = 1;
    private static final int CURRENT_VERSION = VERSION_CONTROLLER_UPDATE;

    private static final String CONFIG_KEY = "Main/UpdateNotesVersion";

    private static int getVersion(MainActivity parent) {
        try {
            final SharedPreferences sp = PreferenceManager.getDefaultSharedPreferences(parent);
            return sp.getInt(CONFIG_KEY, 0);
        } catch (Exception e) {
            e.printStackTrace();
            return CURRENT_VERSION;
        }
    }

    public static void setVersion(MainActivity parent, int version) {
        final SharedPreferences sp = PreferenceManager.getDefaultSharedPreferences(parent);
        sp.edit().putInt(CONFIG_KEY, version).commit();
    }

    public static boolean displayUpdateNotes(MainActivity parent) {
        final int version = getVersion(parent);

        if (version < VERSION_CONTROLLER_UPDATE ) {
            displayControllerUpdateNotes(parent);
            setVersion(parent, VERSION_CONTROLLER_UPDATE);
            return true;
        }

        return false;
    }

    public static void displayControllerUpdateNotes(MainActivity parent) {
        final AlertDialog.Builder builder = new AlertDialog.Builder(parent);
        builder.setTitle(R.string.update_notes_title);
        builder.setMessage(R.string.update_notes_message_version_controller_update);
        builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
            dialog.dismiss();
            Intent intent = new Intent(parent, ControllerSettingsActivity.class);
            parent.startActivity(intent);
        });
        builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }
}
