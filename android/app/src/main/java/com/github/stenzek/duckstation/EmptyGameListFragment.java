package com.github.stenzek.duckstation;

import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

public class EmptyGameListFragment extends Fragment {
    private static final String SUPPORTED_FORMATS_STRING = ".cue, .iso, .img, .ecm, .mds, .chd, .pbp";

    private MainActivity parent;

    public EmptyGameListFragment(MainActivity parent) {
        super(R.layout.fragment_empty_game_list);
        this.parent = parent;
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        ((TextView)view.findViewById(R.id.supported_formats)).setText(
                getString(R.string.main_activity_empty_game_list_supported_formats, SUPPORTED_FORMATS_STRING));
        ((Button)view.findViewById(R.id.add_game_directory)).setOnClickListener(v -> parent.startAddGameDirectory());
        ((Button)view.findViewById(R.id.start_file)).setOnClickListener(v -> parent.startStartFile());
    }
}
