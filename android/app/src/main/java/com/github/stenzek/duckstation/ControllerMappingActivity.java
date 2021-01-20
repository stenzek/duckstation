package com.github.stenzek.duckstation;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceScreen;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import java.util.ArrayList;

public class ControllerMappingActivity extends AppCompatActivity {

    private static final int NUM_CONTROLLER_PORTS = 2;

    private ArrayList<ControllerBindingPreference> mPreferences = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.settings_activity);
        getSupportFragmentManager()
                .beginTransaction()
                .replace(R.id.settings, new SettingsCollectionFragment(this))
                .commit();
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
            actionBar.setTitle(R.string.controller_mapping_activity_title);
        }
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
            doClearBindings();
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
                .setNegativeButton("Cancel", ((dialog, which) -> dialog.dismiss()))
                .create()
                .show();
    }

    private void doLoadProfile(String profileName) {
        if (!AndroidHostInterface.getInstance().loadInputProfile(profileName)) {
            displayError(String.format(getString(R.string.controller_mapping_activity_failed_to_load_profile), profileName));
            return;
        }

        updateAllBindings();
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

            Toast.makeText(ControllerMappingActivity.this, String.format(ControllerMappingActivity.this.getString(R.string.controller_mapping_activity_input_profile_saved), name),
                    Toast.LENGTH_LONG).show();
        });
        builder.setNegativeButton(R.string.controller_mapping_activity_cancel, (dialog, which) -> dialog.dismiss());
        builder.create().show();
    }

    private void doClearBindings() {
        SharedPreferences.Editor prefEdit = PreferenceManager.getDefaultSharedPreferences(this).edit();
        for (ControllerBindingPreference pref : mPreferences)
            pref.clearBinding(prefEdit);
        prefEdit.commit();
    }

    private void updateAllBindings() {
        for (ControllerBindingPreference pref : mPreferences)
            pref.updateValue();
    }

    public static class ControllerPortFragment extends PreferenceFragmentCompat {
        private ControllerMappingActivity activity;
        private int controllerIndex;

        public ControllerPortFragment(ControllerMappingActivity activity, int controllerIndex) {
            this.activity = activity;
            this.controllerIndex = controllerIndex;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final SharedPreferences sp = getPreferenceManager().getSharedPreferences();
            final String defaultControllerType = controllerIndex == 0 ? "DigitalController" : "None";
            String controllerType = sp.getString(String.format("Controller%d/Type", controllerIndex), defaultControllerType);
            String[] controllerButtons = AndroidHostInterface.getControllerButtonNames(controllerType);
            String[] axisButtons = AndroidHostInterface.getControllerAxisNames(controllerType);

            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());
            if (controllerButtons != null) {
                for (String buttonName : controllerButtons) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initButton(controllerIndex, buttonName);
                    ps.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }
            if (axisButtons != null) {
                for (String axisName : axisButtons) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initAxis(controllerIndex, axisName);
                    ps.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }

            setPreferenceScreen(ps);
        }
    }

    public static class HotkeyFragment extends PreferenceFragmentCompat {
        private ControllerMappingActivity activity;
        private HotkeyInfo[] mHotkeyInfo;

        public HotkeyFragment(ControllerMappingActivity activity) {
            this.activity = activity;
            this.mHotkeyInfo = AndroidHostInterface.getInstance().getHotkeyInfoList();
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());
            if (mHotkeyInfo != null) {
                for (HotkeyInfo hotkeyInfo : mHotkeyInfo) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initHotkey(hotkeyInfo);
                    ps.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }

            setPreferenceScreen(ps);
        }
    }

    public static class SettingsCollectionFragment extends Fragment {
        private ControllerMappingActivity activity;
        private SettingsCollectionAdapter adapter;
        private ViewPager2 viewPager;

        public SettingsCollectionFragment(ControllerMappingActivity activity) {
            this.activity = activity;
        }

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_controller_mapping, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            adapter = new SettingsCollectionAdapter(activity, this);
            viewPager = view.findViewById(R.id.view_pager);
            viewPager.setAdapter(adapter);

            TabLayout tabLayout = view.findViewById(R.id.tab_layout);
            new TabLayoutMediator(tabLayout, viewPager, (tab, position) -> {
                if (position == NUM_CONTROLLER_PORTS)
                    tab.setText("Hotkeys");
                else
                    tab.setText(String.format("Port %d", position + 1));
            }).attach();
        }
    }

    public static class SettingsCollectionAdapter extends FragmentStateAdapter {
        private ControllerMappingActivity activity;

        public SettingsCollectionAdapter(@NonNull ControllerMappingActivity activity, @NonNull Fragment fragment) {
            super(fragment);
            this.activity = activity;
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            if (position != NUM_CONTROLLER_PORTS)
                return new ControllerPortFragment(activity, position + 1);
            else
                return new HotkeyFragment(activity);
        }

        @Override
        public int getItemCount() {
            return NUM_CONTROLLER_PORTS + 1;
        }
    }
}