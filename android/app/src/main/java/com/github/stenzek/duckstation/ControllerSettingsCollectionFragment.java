package com.github.stenzek.duckstation;


import android.annotation.SuppressLint;
import android.app.Dialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.DialogFragment;
import androidx.fragment.app.Fragment;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceScreen;
import androidx.preference.SeekBarPreference;
import androidx.preference.SwitchPreferenceCompat;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import java.util.ArrayList;
import java.util.HashMap;

public class ControllerSettingsCollectionFragment extends Fragment {
    public static final String MULTITAP_MODE_SETTINGS_KEY = "ControllerPorts/MultitapMode";
    private static final int NUM_MAIN_CONTROLLER_PORTS = 2;
    private static final int NUM_SUB_CONTROLLER_PORTS = 4;
    private static final char[] SUB_CONTROLLER_PORT_NAMES = new char[]{'A', 'B', 'C', 'D'};
    private static final int NUM_AUTO_FIRE_BUTTONS = 4;

    public interface MultitapModeChangedListener {
        void onChanged();
    }

    private final ArrayList<ControllerBindingPreference> preferences = new ArrayList<>();
    private SettingsCollectionAdapter adapter;
    private ViewPager2 viewPager;
    private String[] controllerPortNames;

    private MultitapModeChangedListener multitapModeChangedListener;

    public ControllerSettingsCollectionFragment() {
    }

    public static String getControllerTypeKey(int port) {
        return String.format("Controller%d/Type", port);
    }

    public static String getControllerType(SharedPreferences prefs, int port) {
        final String defaultControllerType = (port == 1) ? "DigitalController" : "None";
        return prefs.getString(getControllerTypeKey(port), defaultControllerType);
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_controller_settings, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        final String multitapMode = PreferenceManager.getDefaultSharedPreferences(getContext()).getString(
                MULTITAP_MODE_SETTINGS_KEY, "Disabled");

        final ArrayList<String> portNames = new ArrayList<>();
        for (int i = 0; i < NUM_MAIN_CONTROLLER_PORTS; i++) {
            final boolean isMultitap = (multitapMode.equals("BothPorts") ||
                    (i == 0 && multitapMode.equals("Port1Only")) ||
                    (i == 1 && multitapMode.equals("Port2Only")));

            if (isMultitap) {
                for (int j = 0; j < NUM_SUB_CONTROLLER_PORTS; j++) {
                    portNames.add(getContext().getString(
                            R.string.controller_settings_sub_port_format,
                            i + 1, SUB_CONTROLLER_PORT_NAMES[j]));
                }
            } else {
                portNames.add(getContext().getString(
                        R.string.controller_settings_main_port_format,
                        i + 1));
            }
        }

        controllerPortNames = new String[portNames.size()];
        portNames.toArray(controllerPortNames);

        adapter = new SettingsCollectionAdapter(this, controllerPortNames.length);
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

    public void setMultitapModeChangedListener(MultitapModeChangedListener multitapModeChangedListener) {
        this.multitapModeChangedListener = multitapModeChangedListener;
    }

    public void clearAllBindings() {
        SharedPreferences.Editor prefEdit = PreferenceManager.getDefaultSharedPreferences(getContext()).edit();
        for (ControllerBindingPreference pref : preferences)
            pref.clearBinding(prefEdit);
        prefEdit.commit();
    }

    public void updateAllBindings() {
        for (ControllerBindingPreference pref : preferences)
            pref.updateValue();
    }

    public static class SettingsFragment extends PreferenceFragmentCompat {
        private final ControllerSettingsCollectionFragment parent;

        public SettingsFragment(ControllerSettingsCollectionFragment parent) {
            this.parent = parent;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.controllers_preferences, rootKey);

            final Preference multitapModePreference = getPreferenceScreen().findPreference(MULTITAP_MODE_SETTINGS_KEY);
            if (multitapModePreference != null) {
                multitapModePreference.setOnPreferenceChangeListener((pref, newValue) -> {
                    if (parent.multitapModeChangedListener != null)
                        parent.multitapModeChangedListener.onChanged();

                    return true;
                });
            }
        }
    }

    public static class ControllerPortFragment extends PreferenceFragmentCompat {
        private final ControllerSettingsCollectionFragment parent;
        private final int controllerIndex;
        private PreferenceCategory mButtonsCategory;
        private PreferenceCategory mAxisCategory;
        private PreferenceCategory mSettingsCategory;
        private PreferenceCategory mAutoFireCategory;
        private PreferenceCategory mAutoFireBindingsCategory;

        public ControllerPortFragment(ControllerSettingsCollectionFragment parent, int controllerIndex) {
            this.parent = parent;
            this.controllerIndex = controllerIndex;
        }

        private static void clearBindingsInCategory(SharedPreferences.Editor editor, PreferenceCategory category) {
            for (int i = 0; i < category.getPreferenceCount(); i++) {
                final Preference preference = category.getPreference(i);
                if (preference instanceof ControllerBindingPreference)
                    ((ControllerBindingPreference) preference).clearBinding(editor);
            }
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

        private String getAutoToggleSummary(SharedPreferences sp, int slot) {
            final String button = sp.getString(String.format("AutoFire%dButton", slot), null);
            if (button == null || button.length() == 0)
                return "Not Configured";

            return String.format("%s every %d frames", button, sp.getInt("AutoFire%dFrequency", 2));
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
                final ControllerAutoMapper mapper = new ControllerAutoMapper(getContext(), controllerIndex, () -> {
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
                final AlertDialog.Builder builder = new AlertDialog.Builder(getContext());
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
            mButtonsCategory.setTitle(R.string.controller_settings_category_button_bindings);
            mButtonsCategory.setIconSpaceReserved(false);
            ps.addPreference(mButtonsCategory);

            mAxisCategory = new PreferenceCategory(getContext());
            mAxisCategory.setTitle(R.string.controller_settings_category_axis_bindings);
            mAxisCategory.setIconSpaceReserved(false);
            ps.addPreference(mAxisCategory);

            mSettingsCategory = new PreferenceCategory(getContext());
            mSettingsCategory.setTitle(R.string.controller_settings_category_settings);
            mSettingsCategory.setIconSpaceReserved(false);
            ps.addPreference(mSettingsCategory);

            mAutoFireCategory = new PreferenceCategory(getContext());
            mAutoFireCategory.setTitle(R.string.controller_settings_category_auto_fire_buttons);
            mAutoFireCategory.setIconSpaceReserved(false);
            ps.addPreference(mAutoFireCategory);

            mAutoFireBindingsCategory = new PreferenceCategory(getContext());
            mAutoFireBindingsCategory.setTitle(R.string.controller_settings_category_auto_fire_bindings);
            mAutoFireBindingsCategory.setIconSpaceReserved(false);
            ps.addPreference(mAutoFireBindingsCategory);

            createPreferences(controllerType);
        }

        @SuppressLint("DefaultLocale")
        private void createPreferences(String controllerType) {
            final PreferenceScreen ps = getPreferenceScreen();
            final SharedPreferences sp = getPreferenceManager().getSharedPreferences();
            final String[] buttonNames = AndroidHostInterface.getControllerButtonNames(controllerType);
            final String[] axisNames = AndroidHostInterface.getControllerAxisNames(controllerType);
            final int vibrationMotors = AndroidHostInterface.getControllerVibrationMotorCount(controllerType);

            if (buttonNames != null) {
                for (String buttonName : buttonNames) {
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initButton(controllerIndex, buttonName);
                    mButtonsCategory.addPreference(cbp);
                    parent.preferences.add(cbp);
                }
            }

            if (axisNames != null) {
                for (String axisName : axisNames) {
                    final int axisType = AndroidHostInterface.getControllerAxisType(controllerType, axisName);
                    final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                    cbp.initAxis(controllerIndex, axisName, axisType);
                    mAxisCategory.addPreference(cbp);
                    parent.preferences.add(cbp);
                }
            }

            if (vibrationMotors > 0) {
                final ControllerBindingPreference cbp = new ControllerBindingPreference(getContext(), null);
                cbp.initVibration(controllerIndex);
                mSettingsCategory.addPreference(cbp);
                parent.preferences.add(cbp);
            }

            if (controllerType.equals("AnalogController")) {
                mSettingsCategory.addPreference(
                        createTogglePreference(String.format("Controller%d/ForceAnalogOnReset", controllerIndex),
                                R.string.settings_enable_analog_mode_on_reset, R.string.settings_summary_enable_analog_mode_on_reset, true));

                mSettingsCategory.addPreference(
                        createTogglePreference(String.format("Controller%d/AnalogDPadInDigitalMode", controllerIndex),
                                R.string.settings_use_analog_sticks_for_dpad, R.string.settings_summary_use_analog_sticks_for_dpad, true));
            }

            if (buttonNames != null) {
              for (int autoFireSlot = 1; autoFireSlot <= NUM_AUTO_FIRE_BUTTONS; autoFireSlot++) {
                  final ListPreference autoFirePreference = new ListPreference(getContext());
                  autoFirePreference.setEntries(buttonNames);
                  autoFirePreference.setEntryValues(buttonNames);
                  autoFirePreference.setKey(String.format("Controller%d/AutoFire%dButton", controllerIndex, autoFireSlot));
                  autoFirePreference.setTitle(getContext().getString(R.string.controller_settings_auto_fire_n_button, autoFireSlot));
                  autoFirePreference.setSummaryProvider(ListPreference.SimpleSummaryProvider.getInstance());
                  autoFirePreference.setIconSpaceReserved(false);
                  mAutoFireCategory.addPreference(autoFirePreference);

                  final SeekBarPreference frequencyPreference = new SeekBarPreference(getContext());
                  frequencyPreference.setMin(1);
                  frequencyPreference.setMax(60);
                  frequencyPreference.setKey(String.format("Controller%d/AutoFire%dFrequency", controllerIndex, autoFireSlot));
                  frequencyPreference.setDefaultValue(2);
                  frequencyPreference.setTitle(getContext().getString(R.string.controller_settings_auto_fire_n_frequency, autoFireSlot));
                  frequencyPreference.setIconSpaceReserved(false);
                  frequencyPreference.setShowSeekBarValue(true);
                  mAutoFireCategory.addPreference(frequencyPreference);
              }

              for (int autoFireSlot = 1; autoFireSlot <= NUM_AUTO_FIRE_BUTTONS; autoFireSlot++) {
                  final ControllerBindingPreference bindingPreference = new ControllerBindingPreference(getContext(), null);
                  bindingPreference.initAutoFireButton(controllerIndex, autoFireSlot);
                  mAutoFireBindingsCategory.addPreference(bindingPreference);
              }
            }
        }

        private void removePreferences() {
            for (int i = 0; i < mButtonsCategory.getPreferenceCount(); i++) {
                parent.preferences.remove(mButtonsCategory.getPreference(i));
            }
            mButtonsCategory.removeAll();

            for (int i = 0; i < mAxisCategory.getPreferenceCount(); i++) {
                parent.preferences.remove(mAxisCategory.getPreference(i));
            }
            mAxisCategory.removeAll();

            for (int i = 0; i < mSettingsCategory.getPreferenceCount(); i++) {
                parent.preferences.remove(mSettingsCategory.getPreference(i));
            }
            mSettingsCategory.removeAll();

            for (int i = 0; i < mAutoFireCategory.getPreferenceCount(); i++) {
                parent.preferences.remove(mAutoFireCategory.getPreference(i));
            }
            mAutoFireCategory.removeAll();

            for (int i = 0; i < mAutoFireBindingsCategory.getPreferenceCount(); i++) {
                parent.preferences.remove(mAutoFireBindingsCategory.getPreference(i));
            }
            mAutoFireBindingsCategory.removeAll();
        }

        private void clearBindings() {
            final SharedPreferences.Editor editor = getPreferenceManager().getSharedPreferences().edit();
            clearBindingsInCategory(editor, mButtonsCategory);
            clearBindingsInCategory(editor, mAxisCategory);
            clearBindingsInCategory(editor, mSettingsCategory);
            editor.commit();

            Toast.makeText(parent.getContext(), parent.getString(
                    R.string.controller_settings_clear_controller_bindings_done, controllerIndex),
                    Toast.LENGTH_LONG).show();
        }
    }

    public static class HotkeyFragment extends PreferenceFragmentCompat {
        private final ControllerSettingsCollectionFragment parent;
        private final HotkeyInfo[] mHotkeyInfo;

        public HotkeyFragment(ControllerSettingsCollectionFragment parent) {
            this.parent = parent;
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
                    parent.preferences.add(cbp);
                }
            }

            setPreferenceScreen(ps);
        }
    }

    public static class SettingsCollectionAdapter extends FragmentStateAdapter {
        private final ControllerSettingsCollectionFragment parent;
        private final int controllerPorts;

        public SettingsCollectionAdapter(@NonNull ControllerSettingsCollectionFragment parent, int controllerPorts) {
            super(parent);
            this.parent = parent;
            this.controllerPorts = controllerPorts;
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            if (position == 0)
                return new SettingsFragment(parent);
            else if (position <= controllerPorts)
                return new ControllerPortFragment(parent, position);
            else
                return new HotkeyFragment(parent);
        }

        @Override
        public int getItemCount() {
            return controllerPorts + 2;
        }
    }
}