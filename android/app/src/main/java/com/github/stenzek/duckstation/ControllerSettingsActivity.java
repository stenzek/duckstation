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
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceScreen;
import androidx.preference.SwitchPreferenceCompat;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import java.util.ArrayList;
import java.util.HashMap;

public class ControllerSettingsActivity extends AppCompatActivity {

    private static final int NUM_CONTROLLER_PORTS = 2;
    public static final String MULTITAP_MODE_SETTINGS_KEY = "ControllerPorts/MultitapMode";

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
                .setNegativeButton(R.string.controller_mapping_activity_cancel, ((dialog, which) -> dialog.dismiss()))
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

            Toast.makeText(ControllerSettingsActivity.this, String.format(ControllerSettingsActivity.this.getString(R.string.controller_mapping_activity_input_profile_saved), name),
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

    public static String getControllerTypeKey(int port) {
        return String.format("Controller%d/Type", port);
    }

    public static String getControllerType(SharedPreferences prefs, int port) {
        final String defaultControllerType = (port == 1) ? "DigitalController" : "None";
        return prefs.getString(getControllerTypeKey(port), defaultControllerType);
    }

    public static class SettingsFragment extends PreferenceFragmentCompat {
        ControllerSettingsActivity parent;

        public SettingsFragment(ControllerSettingsActivity parent) {
            this.parent = parent;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.controllers_preferences, rootKey);

            final Preference multitapModePreference = getPreferenceScreen().findPreference(MULTITAP_MODE_SETTINGS_KEY);
            if (multitapModePreference != null) {
                multitapModePreference.setOnPreferenceChangeListener((pref, newValue) -> {
                    parent.recreate();
                    return true;
                });
            }
        }
    }

    public static class ControllerPortFragment extends PreferenceFragmentCompat {
        private ControllerSettingsActivity activity;
        private int controllerIndex;
        private PreferenceCategory mButtonsCategory;
        private PreferenceCategory mAxisCategory;
        private PreferenceCategory mSettingsCategory;

        public ControllerPortFragment(ControllerSettingsActivity activity, int controllerIndex) {
            this.activity = activity;
            this.controllerIndex = controllerIndex;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());
            setPreferenceScreen(ps);
            createPreferences();
        }

        private SwitchPreferenceCompat createTogglePreference(String key, int title, int summary, boolean defaultValue) {
            final SwitchPreferenceCompat pref = new SwitchPreferenceCompat(getContext());
            pref.setKey(key);
            pref.setTitle(title);
            pref.setSummary(summary);
            pref.setIconSpaceReserved(false);
            pref.setDefaultValue(defaultValue);
            return pref;
        }

        private void createPreferences() {
            final PreferenceScreen ps = getPreferenceScreen();
            final SharedPreferences sp = getPreferenceManager().getSharedPreferences();
            final String controllerType = getControllerType(sp, controllerIndex);
            final String[] controllerButtons = AndroidHostInterface.getControllerButtonNames(controllerType);
            final String[] axisButtons = AndroidHostInterface.getControllerAxisNames(controllerType);
            final int vibrationMotors = AndroidHostInterface.getControllerVibrationMotorCount(controllerType);

            final ListPreference typePreference = new ListPreference(getContext());
            typePreference.setEntries(R.array.settings_controller_type_entries);
            typePreference.setEntryValues(R.array.settings_controller_type_values);
            typePreference.setKey(getControllerTypeKey(controllerIndex));
            typePreference.setValue(controllerType);
            typePreference.setTitle(R.string.settings_controller_type);
            typePreference.setSummaryProvider(ListPreference.SimpleSummaryProvider.getInstance());
            typePreference.setIconSpaceReserved(false);
            typePreference.setOnPreferenceChangeListener((pref, value) -> {
                removePreferences();
                createPreferences(value.toString());
                return true;
            });
            ps.addPreference(typePreference);

            final Preference autoBindPreference = new Preference(getContext());
            autoBindPreference.setTitle(R.string.controller_settings_automatic_mapping);
            autoBindPreference.setSummary(R.string.controller_settings_summary_automatic_mapping);
            autoBindPreference.setIconSpaceReserved(false);
            autoBindPreference.setOnPreferenceClickListener(preference -> {
                final ControllerAutoMapper mapper = new ControllerAutoMapper(activity, controllerIndex, () -> {
                    removePreferences();
                    createPreferences(typePreference.getValue());
                });
                mapper.start();
                return true;
            });
            ps.addPreference(autoBindPreference);

            final Preference clearBindingsPreference = new Preference(getContext());
            clearBindingsPreference.setTitle(R.string.controller_settings_clear_controller_bindings);
            clearBindingsPreference.setSummary(R.string.controller_settings_summary_clear_controller_bindings);
            clearBindingsPreference.setIconSpaceReserved(false);
            clearBindingsPreference.setOnPreferenceClickListener(preference -> {
                final AlertDialog.Builder builder = new AlertDialog.Builder(activity);
                builder.setMessage(R.string.controller_settings_clear_controller_bindings_confirm);
                builder.setPositiveButton(R.string.main_activity_yes, (dialog, which) -> {
                    dialog.dismiss();
                    clearBindings();
                });
                builder.setNegativeButton(R.string.main_activity_no, (dialog, which) -> dialog.dismiss());
                builder.create().show();
                return true;
            });
            ps.addPreference(clearBindingsPreference);

            mButtonsCategory = new PreferenceCategory(getContext());
            mButtonsCategory.setTitle(getContext().getString(R.string.controller_settings_category_button_bindings));
            mButtonsCategory.setIconSpaceReserved(false);
            ps.addPreference(mButtonsCategory);

            mAxisCategory = new PreferenceCategory(getContext());
            mAxisCategory.setTitle(getContext().getString(R.string.controller_settings_category_axis_bindings));
            mAxisCategory.setIconSpaceReserved(false);
            ps.addPreference(mAxisCategory);

            mSettingsCategory = new PreferenceCategory(getContext());
            mSettingsCategory.setTitle(getContext().getString(R.string.controller_settings_category_settings));
            mSettingsCategory.setIconSpaceReserved(false);
            ps.addPreference(mSettingsCategory);

            createPreferences(controllerType);
        }

        private void createPreferences(String controllerType) {
            final PreferenceScreen ps = getPreferenceScreen();
            final SharedPreferences sp = getPreferenceManager().getSharedPreferences();
            final String[] controllerButtons = AndroidHostInterface.getControllerButtonNames(controllerType);
            final String[] axisButtons = AndroidHostInterface.getControllerAxisNames(controllerType);
            final int vibrationMotors = AndroidHostInterface.getControllerVibrationMotorCount(controllerType);

            if (controllerButtons != null) {
                for (String buttonName : controllerButtons) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initButton(controllerIndex, buttonName);
                    mButtonsCategory.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }

            if (axisButtons != null) {
                for (String axisName : axisButtons) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initAxis(controllerIndex, axisName);
                    mAxisCategory.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }

            if (vibrationMotors > 0) {
                final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                cbp.initVibration(controllerIndex);
                mSettingsCategory.addPreference(cbp);
                activity.mPreferences.add(cbp);
            }

            if (controllerType.equals("AnalogController")) {
                mSettingsCategory.addPreference(
                        createTogglePreference(String.format("Controller%d/ForceAnalogOnReset", controllerIndex),
                                R.string.settings_enable_analog_mode_on_reset, R.string.settings_summary_enable_analog_mode_on_reset, true));

                mSettingsCategory.addPreference(
                        createTogglePreference(String.format("Controller%d/AnalogDPadInDigitalMode", controllerIndex),
                                R.string.settings_use_analog_sticks_for_dpad, R.string.settings_summary_use_analog_sticks_for_dpad, true));
            }
        }

        private void removePreferences() {
            for (int i = 0; i < mButtonsCategory.getPreferenceCount(); i++) {
                activity.mPreferences.remove(mButtonsCategory.getPreference(i));
            }
            mButtonsCategory.removeAll();

            for (int i = 0; i < mAxisCategory.getPreferenceCount(); i++) {
                activity.mPreferences.remove(mAxisCategory.getPreference(i));
            }
            mAxisCategory.removeAll();

            for (int i = 0; i < mSettingsCategory.getPreferenceCount(); i++) {
                activity.mPreferences.remove(mSettingsCategory.getPreference(i));
            }
            mSettingsCategory.removeAll();
        }

        private static void clearBindingsInCategory(SharedPreferences.Editor editor, PreferenceCategory category) {
            for (int i = 0; i < category.getPreferenceCount(); i++) {
                final Preference preference = category.getPreference(i);
                if (preference instanceof ControllerBindingPreference)
                    ((ControllerBindingPreference)preference).clearBinding(editor);
            }
        }

        private void clearBindings() {
            final SharedPreferences.Editor editor = getPreferenceManager().getSharedPreferences().edit();
            clearBindingsInCategory(editor, mButtonsCategory);
            clearBindingsInCategory(editor, mAxisCategory);
            clearBindingsInCategory(editor, mSettingsCategory);
            editor.commit();

            Toast.makeText(activity, activity.getString(
                    R.string.controller_settings_clear_controller_bindings_done, controllerIndex),
                    Toast.LENGTH_LONG).show();
        }
    }

    public static class HotkeyFragment extends PreferenceFragmentCompat {
        private ControllerSettingsActivity activity;
        private HotkeyInfo[] mHotkeyInfo;

        public HotkeyFragment(ControllerSettingsActivity activity) {
            this.activity = activity;
            this.mHotkeyInfo = AndroidHostInterface.getInstance().getHotkeyInfoList();
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());
            if (mHotkeyInfo != null) {
                final HashMap<String, PreferenceCategory> categoryMap = new HashMap<>();

                for (HotkeyInfo hotkeyInfo : mHotkeyInfo) {
                    PreferenceCategory category = categoryMap.containsKey(hotkeyInfo.getCategory()) ?
                            categoryMap.get(hotkeyInfo.getCategory()) : null;
                    if (category == null) {
                        category = new PreferenceCategory(getContext());
                        category.setTitle(hotkeyInfo.getCategory());
                        category.setIconSpaceReserved(false);
                        categoryMap.put(hotkeyInfo.getCategory(), category);
                        ps.addPreference(category);
                    }

                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initHotkey(hotkeyInfo);
                    category.addPreference(cbp);
                    activity.mPreferences.add(cbp);
                }
            }

            setPreferenceScreen(ps);
        }
    }

    public static class SettingsCollectionFragment extends Fragment {
        private ControllerSettingsActivity activity;
        private SettingsCollectionAdapter adapter;
        private ViewPager2 viewPager;
        private String[] controllerPortNames;

        private static final int NUM_MAIN_CONTROLLER_PORTS = 2;
        private static final int NUM_SUB_CONTROLLER_PORTS = 4;
        private static final char[] SUB_CONTROLLER_PORT_NAMES = new char[] {'A', 'B', 'C', 'D'};

        public SettingsCollectionFragment(ControllerSettingsActivity activity) {
            this.activity = activity;

            final String multitapMode = PreferenceManager.getDefaultSharedPreferences(activity).getString(
                    MULTITAP_MODE_SETTINGS_KEY, "Disabled");

            final ArrayList<String> portNames = new ArrayList<>();
            for (int i = 0; i < NUM_MAIN_CONTROLLER_PORTS; i++) {
                final boolean isMultitap = (multitapMode.equals("BothPorts") ||
                        (i == 0 && multitapMode.equals("Port1Only")) ||
                        (i == 1 && multitapMode.equals("Port2Only")));

                if (isMultitap) {
                    for (int j = 0; j < NUM_SUB_CONTROLLER_PORTS; j++) {
                        portNames.add(activity.getString(
                                R.string.controller_settings_sub_port_format,
                                i + 1, SUB_CONTROLLER_PORT_NAMES[j]));
                    }
                } else {
                    portNames.add(activity.getString(
                            R.string.controller_settings_main_port_format,
                            i + 1));
                }
            }

            controllerPortNames = new String[portNames.size()];
            portNames.toArray(controllerPortNames);
        }

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_controller_settings, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            adapter = new SettingsCollectionAdapter(activity, this, controllerPortNames.length);
            viewPager = view.findViewById(R.id.view_pager);
            viewPager.setAdapter(adapter);

            TabLayout tabLayout = view.findViewById(R.id.tab_layout);
            new TabLayoutMediator(tabLayout, viewPager, (tab, position) -> {
                if (position == 0)
                    tab.setText(R.string.controller_settings_tab_settings);
                else if (position <= controllerPortNames.length)
                    tab.setText(controllerPortNames[position - 1]);
                else
                    tab.setText(R.string.controller_settings_tab_hotkeys);
            }).attach();
        }
    }

    public static class SettingsCollectionAdapter extends FragmentStateAdapter {
        private ControllerSettingsActivity activity;
        private int controllerPorts;

        public SettingsCollectionAdapter(@NonNull ControllerSettingsActivity activity, @NonNull Fragment fragment, int controllerPorts) {
            super(fragment);
            this.activity = activity;
            this.controllerPorts = controllerPorts;
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            if (position == 0)
                return new SettingsFragment(activity);
            else if (position <= controllerPorts)
                return new ControllerPortFragment(activity, position);
            else
                return new HotkeyFragment(activity);
        }

        @Override
        public int getItemCount() {
            return controllerPorts + 2;
        }
    }
}