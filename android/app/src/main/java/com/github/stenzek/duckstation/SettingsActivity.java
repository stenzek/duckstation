package com.github.stenzek.duckstation;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;
import androidx.preference.PreferenceFragmentCompat;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

public class SettingsActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.settings_activity);
        getSupportFragmentManager()
                .beginTransaction()
                .replace(R.id.settings, new SettingsCollectionFragment())
                .commit();
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }
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

    public static class SettingsFragment extends PreferenceFragmentCompat {
        private final int resourceId;

        public SettingsFragment(int resourceId) {
            this.resourceId = resourceId;
        }

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(resourceId, rootKey);
        }
    }

    public static class SettingsCollectionFragment extends Fragment {
        private SettingsCollectionAdapter adapter;
        private ViewPager2 viewPager;

        @Nullable
        @Override
        public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
            return inflater.inflate(R.layout.fragment_settings_collection, container, false);
        }

        @Override
        public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
            adapter = new SettingsCollectionAdapter(this);
            viewPager = view.findViewById(R.id.view_pager);
            viewPager.setAdapter(adapter);

            TabLayout tabLayout = view.findViewById(R.id.tab_layout);
            new TabLayoutMediator(tabLayout, viewPager,
                    (tab, position) -> tab.setText(getResources().getStringArray(R.array.settings_tabs)[position])
            ).attach();
        }
    }

    public static class SettingsCollectionAdapter extends FragmentStateAdapter {
        public SettingsCollectionAdapter(@NonNull Fragment fragment) {
            super(fragment);
        }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            switch (position) {
                case 0:     // General
                    return new SettingsFragment(R.xml.general_preferences);

                case 1:     // Display
                    return new SettingsFragment(R.xml.display_preferences);

                case 2:     // Audio
                    return new SettingsFragment(R.xml.audio_preferences);

                case 3:     // Enhancements
                    return new SettingsFragment(R.xml.enhancements_preferences);

                case 4:     // Achievements
                    return new AchievementSettingsFragment();

                case 5:     // Advanced
                    return new SettingsFragment(R.xml.advanced_preferences);

                default:
                    return new Fragment();
            }
        }

        @Override
        public int getItemCount() {
            return 6;
        }
    }
}