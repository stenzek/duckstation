package com.github.stenzek.duckstation;

import android.os.Bundle;
import android.view.Gravity;
import android.view.MenuItem;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ListView;
import android.widget.PopupMenu;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

public class GameListFragment extends Fragment {
    private MainActivity mParent;
    private ListView mGameListView;

    public GameListFragment(MainActivity parent) {
        super(R.layout.fragment_game_list);
        this.mParent = parent;
    }

    private GameList getGameList() {
        return mParent.getGameList();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        mGameListView = view.findViewById(R.id.game_list_view);
        mGameListView.setAdapter(getGameList().getListViewAdapter());
        mGameListView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                mParent.startEmulation(getGameList().getEntry(position).getPath(), mParent.shouldResumeStateByDefault());
            }
        });
        mGameListView.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, int position,
                                           long id) {
                PopupMenu menu = new PopupMenu(getContext(), view, Gravity.RIGHT | Gravity.TOP);
                menu.getMenuInflater().inflate(R.menu.menu_game_list_entry, menu.getMenu());
                menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(MenuItem item) {
                        int id = item.getItemId();
                        if (id == R.id.game_list_entry_menu_start_game) {
                            mParent.startEmulation(getGameList().getEntry(position).getPath(), false);
                            return true;
                        } else if (id == R.id.game_list_entry_menu_resume_game) {
                            mParent.startEmulation(getGameList().getEntry(position).getPath(), true);
                            return true;
                        } else if (id == R.id.game_list_entry_menu_properties) {
                            mParent.openGameProperties(getGameList().getEntry(position).getPath());
                            return true;
                        }
                        return false;
                    }
                });
                menu.show();
                return true;
            }
        });
    }
}
