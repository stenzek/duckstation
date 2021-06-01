package com.github.stenzek.duckstation;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.EditText;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.preference.PreferenceManager;

import java.util.ArrayList;

public class ControllerSettingsActivity extends AppCompatActivity {
    private ControllerSettingsCollectionFragment fragment;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.settings_activity);

        fragment = new ControllerSettingsCollectionFragment();
        fragment.setMultitapModeChangedListener(this::recreate);

        getSupportFragmentManager()
                .beginTransaction()
                .replace(R.id.settings, fragment)
                .commit();
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
            actionBar.setTitle(R.string.controller_mapping_activity_title);
        }
    }

    @Override
    protected void onSaveInstanceState(@NonNull Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.remove("android:support:fragments");
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_controller_mapping, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(@NonNull MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            onBackPressed();
            return true;
        }

        final int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_load_profile) {
            doLoadProfile();
            return true;
        } else if (id == R.id.action_save_profile) {
            doSaveProfile();
            return true;
        } else if (id == R.id.action_clear_bindings) {
            fragment.clearAllBindings();
            return true;
        } else {
            return super.onOptionsItemSelected(item);
        }
    }

    private void displayError(String text) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.emulation_activity_error)
                .setMessage(text)
                .setNegativeButton(R.string.main_activity_ok, ((dialog, which) -> dialog.dismiss()))
                .create()
                .show();
    }

    private void doLoadProfile() {
        final String[] profileNames = AndroidHostInterface.getInstance().getInputProfileNames();
        if (profileNames == null) {
            displayError(getString(R.string.controller_mapping_activity_no_profiles_found));
            return;
        }

        new AlertDialog.Builder(this)
                .setTitle(R.string.controller_mapping_activity_select_input_profile)
                .setItems(profileNames, (dialog, choice) -> {
                    doLoadProfile(profileNames[choice]);
                    dialog.dismiss();
                })
                .setNegativeButton(R.string.controller_mapping_activity_cancel, ((dialog, which) -> dialog.dismiss()))
                .create()
                .show();
    }

    private void doLoadProfile(String profileName) {
        if (!AndroidHostInterface.getInstance().loadInputProfile(profileName)) {
            displayError(String.format(getString(R.string.controller_mapping_activity_failed_to_load_profile), profileName));
            return;
        }

        fragment.updateAllBindings();
    }

    private void doSaveProfile() {
        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        final EditText input = new EditText(this);
        builder.setTitle(R.string.controller_mapping_activity_input_profile_name);
        builder.setView(input);
        builder.setPositiveButton(R.string.controller_mapping_activity_save, (dialog, which) -> {
            final String name = input.getText().toString();
            if (name.isEmpty()) {
                displayError(getString(R.string.controller_mapping_activity_name_must_be_provided));
                return;
            }

            if (!AndroidHostInterface.getInstance().saveInputProfile(name)) {
                displayError(getString(R.string.controller_mapping_activity_failed_to_save_input_profile));
                return;
            }

            Toast.makeText(ControllerSettingsActivity.this, String.format(ControllerSettingsActivity.this.getString(R.string.controller_mapping_activity_input_profile_saved), name),
                    Toast.LENGTH_LONG).show();
        });
        builder.setNegativeButton(R.string.controller_mapping_activity_cancel, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }


}