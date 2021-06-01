package com.github.stenzek.duckstation;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ListAdapter;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.ListFragment;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceScreen;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

public class GamePropertiesActivity extends AppCompatActivity {
    PropertyListAdapter mPropertiesListAdapter;
    GameListEntry mGameListEntry;

    public ListAdapter getPropertyListAdapter() {
        if (mPropertiesListAdapter != null)
            return mPropertiesListAdapter;

        mPropertiesListAdapter = new PropertyListAdapter(this);
        mPropertiesListAdapter.addItem("title", "Title", mGameListEntry.getTitle());
        mPropertiesListAdapter.addItem("serial", "Serial", mGameListEntry.getCode());
        mPropertiesListAdapter.addItem("type", "Type", mGameListEntry.getType().toString());
        mPropertiesListAdapter.addItem("path", "Path", mGameListEntry.getPath());
        mPropertiesListAdapter.addItem("region", "Region", mGameListEntry.getRegion().toString());
        mPropertiesListAdapter.addItem("compatibility", "Compatibility Rating", mGameListEntry.getCompatibilityRating().toString());
        return mPropertiesListAdapter;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        String path = getIntent().getStringExtra("path");
        if (path == null || path.isEmpty()) {
            finish();
            return;
        }

        mGameListEntry = AndroidHostInterface.getInstance().getGameListEntry(path);
        if (mGameListEntry == null) {
            finish();
            return;
        }

        setContentView(R.layout.settings_activity);
        getSupportFragmentManager()
                .beginTransaction()
                .replace(R.id.settings, new SettingsCollectionFragment(this))
                .commit();
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }

        setTitle(mGameListEntry.getTitle());
    }

    @Override
    protected void onSaveInstanceState(@NonNull Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.remove("android:support:fragments");
    }

    @Override
    public boolean onOptionsItemSelected(@NonNull MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            onBackPressed();
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    private void displayError(String text) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.emulation_activity_error)
                .setMessage(text)
                .setNegativeButton(R.string.main_activity_ok, ((dialog, which) -> dialog.dismiss()))
                .create()
                .show();
    }

    private void createBooleanGameSetting(PreferenceScreen ps, String key, int titleId) {
        GameSettingPreference pref = new GameSettingPreference(ps.getContext(), mGameListEntry.getPath(), key, titleId);
        ps.addPreference(pref);
    }

    private void createTraitGameSetting(PreferenceScreen ps, String key, int titleId) {
        GameTraitPreference pref = new GameTraitPreference(ps.getContext(), mGameListEntry.getPath(), key, titleId);
        ps.addPreference(pref);
    }

    private void createListGameSetting(PreferenceScreen ps, String key, int titleId, int entryId, int entryValuesId) {
        GameSettingPreference pref = new GameSettingPreference(ps.getContext(), mGameListEntry.getPath(), key, titleId, entryId, entryValuesId);
        ps.addPreference(pref);
    }

    public static class GameSettingsFragment extends PreferenceFragmentCompat {
        private GamePropertiesActivity activity;

        public GameSettingsFragment(GamePropertiesActivity activity) {
            this.activity = activity;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());
            activity.createListGameSetting(ps, "CPUOverclock", R.string.settings_cpu_overclocking, R.array.settings_advanced_cpu_overclock_entries, R.array.settings_advanced_cpu_overclock_values);
            activity.createListGameSetting(ps, "CDROMReadSpeedup", R.string.settings_cdrom_read_speedup, R.array.settings_cdrom_read_speedup_entries, R.array.settings_cdrom_read_speedup_values);
            activity.createListGameSetting(ps, "CDROMSeekSpeedup", R.string.settings_cdrom_seek_speedup, R.array.settings_cdrom_seek_speedup_entries, R.array.settings_cdrom_seek_speedup_values);

            activity.createListGameSetting(ps, "GPURenderer", R.string.settings_gpu_renderer, R.array.gpu_renderer_entries, R.array.gpu_renderer_values);
            activity.createListGameSetting(ps, "DisplayAspectRatio", R.string.settings_aspect_ratio, R.array.settings_display_aspect_ratio_names, R.array.settings_display_aspect_ratio_values);
            activity.createListGameSetting(ps, "DisplayCropMode", R.string.settings_crop_mode, R.array.settings_display_crop_mode_entries, R.array.settings_display_crop_mode_values);
            activity.createListGameSetting(ps, "GPUDownsampleMode", R.string.settings_downsample_mode, R.array.settings_downsample_mode_entries, R.array.settings_downsample_mode_values);
            activity.createBooleanGameSetting(ps, "DisplayLinearUpscaling", R.string.settings_linear_upscaling);
            activity.createBooleanGameSetting(ps, "DisplayIntegerUpscaling", R.string.settings_integer_upscaling);
            activity.createBooleanGameSetting(ps, "DisplayForce4_3For24Bit", R.string.settings_force_4_3_for_24bit);

            activity.createListGameSetting(ps, "GPUResolutionScale", R.string.settings_gpu_resolution_scale, R.array.settings_gpu_resolution_scale_entries, R.array.settings_gpu_resolution_scale_values);
            activity.createListGameSetting(ps, "GPUMSAA", R.string.settings_msaa, R.array.settings_gpu_msaa_entries, R.array.settings_gpu_msaa_values);
            activity.createBooleanGameSetting(ps, "GPUTrueColor", R.string.settings_true_color);
            activity.createBooleanGameSetting(ps, "GPUScaledDithering", R.string.settings_scaled_dithering);
            activity.createListGameSetting(ps, "GPUTextureFilter", R.string.settings_texture_filtering, R.array.settings_gpu_texture_filter_names, R.array.settings_gpu_texture_filter_values);
            activity.createBooleanGameSetting(ps, "GPUForceNTSCTimings", R.string.settings_force_ntsc_timings);
            activity.createBooleanGameSetting(ps, "GPUWidescreenHack", R.string.settings_widescreen_hack);
            activity.createBooleanGameSetting(ps, "GPUPGXP", R.string.settings_pgxp_geometry_correction);
            activity.createBooleanGameSetting(ps, "PGXPPreserveProjFP", R.string.settings_pgxp_preserve_projection_precision);
            activity.createBooleanGameSetting(ps, "GPUPGXPDepthBuffer", R.string.settings_pgxp_depth_buffer);
            activity.createTraitGameSetting(ps, "ForceSoftwareRenderer", R.string.settings_use_software_renderer);
            activity.createTraitGameSetting(ps, "ForceSoftwareRendererForReadbacks", R.string.settings_use_software_renderer_for_readbacks);
            activity.createTraitGameSetting(ps, "DisableWidescreen", R.string.settings_disable_widescreen);
            activity.createTraitGameSetting(ps, "ForcePGXPVertexCache", R.string.settings_pgxp_vertex_cache);
            activity.createTraitGameSetting(ps, "ForcePGXPCPUMode", R.string.settings_pgxp_cpu_mode);

            setPreferenceScreen(ps);
        }
    }

    public static class ControllerSettingsFragment extends PreferenceFragmentCompat {
        private GamePropertiesActivity activity;

        public ControllerSettingsFragment(GamePropertiesActivity activity) {
            this.activity = activity;
        }

        private void createInputProfileSetting(PreferenceScreen ps) {
            final GameSettingPreference pref = new GameSettingPreference(ps.getContext(), activity.mGameListEntry.getPath(), "InputProfileName", R.string.settings_input_profile);

            final String[] inputProfileNames = AndroidHostInterface.getInstance().getInputProfileNames();
            pref.setEntries(inputProfileNames);
            pref.setEntryValues(inputProfileNames);
            ps.addPreference(pref);
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            final PreferenceScreen ps = getPreferenceManager().createPreferenceScreen(getContext());

            activity.createListGameSetting(ps, "Controller1Type", R.string.settings_controller_type, R.array.settings_controller_type_entries, R.array.settings_controller_type_values);
            activity.createListGameSetting(ps, "MemoryCard1Type", R.string.settings_memory_card_1_type, R.array.settings_memory_card_mode_entries, R.array.settings_memory_card_mode_values);
            activity.createListGameSetting(ps, "MemoryCard2Type", R.string.settings_memory_card_2_type, R.array.settings_memory_card_mode_entries, R.array.settings_memory_card_mode_values);
            createInputProfileSetting(ps);

            setPreferenceScreen(ps);
        }
    }

    public static class SettingsCollectionFragment extends Fragment {
        private GamePropertiesActivity activity;
        private SettingsCollectionAdapter adapter;
        private ViewPager2 viewPager;

        public SettingsCollectionFragment(GamePropertiesActivity activity) {
            this.activity = activity;
        }

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_controller_settings, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            adapter = new SettingsCollectionAdapter(activity, this);
            viewPager = view.findViewById(R.id.view_pager);
            viewPager.setAdapter(adapter);

            TabLayout tabLayout = view.findViewById(R.id.tab_layout);
            new TabLayoutMediator(tabLayout, viewPager, (tab, position) -> {
                switch (position) {
                    case 0:
                        tab.setText(R.string.game_properties_tab_summary);
                        break;
                    case 1:
                        tab.setText(R.string.game_properties_tab_game_settings);
                        break;
                    case 2:
                        tab.setText(R.string.game_properties_tab_controller_settings);
                        break;
                }
            }).attach();
        }
    }

    public static class SettingsCollectionAdapter extends FragmentStateAdapter {
        private GamePropertiesActivity activity;

        public SettingsCollectionAdapter(@NonNull GamePropertiesActivity activity, @NonNull Fragment fragment) {
            super(fragment);
            this.activity = activity;
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            switch (position) {
                case 0: {           // Summary
                    ListFragment lf = new ListFragment();
                    lf.setListAdapter(activity.getPropertyListAdapter());
                    return lf;
                }

                case 1: {           // Game Settings
                    return new GameSettingsFragment(activity);
                }

                case 2: {           // Controller Settings
                    return new ControllerSettingsFragment(activity);
                }

                // TODO: Memory Card Editor

                default:
                    return null;
            }
        }

        @Override
        public int getItemCount() {
            return 3;
        }
    }
}