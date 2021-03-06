package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.res.Configuration;
import android.os.AsyncTask;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.widget.PopupMenu;
import androidx.core.content.ContextCompat;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

public class GameGridFragment extends Fragment implements GameList.OnRefreshListener {
    private static final int SPACING_DIPS = 25;
    private static final int WIDTH_DIPS = 160;

    private MainActivity mParent;
    private RecyclerView mRecyclerView;
    private ViewAdapter mAdapter;
    private GridAutofitLayoutManager mLayoutManager;

    public GameGridFragment(MainActivity parent) {
        super(R.layout.fragment_game_grid);
        this.mParent = parent;
    }

    private GameList getGameList() {
        return mParent.getGameList();
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        mAdapter = new ViewAdapter(mParent, getGameList());
        getGameList().addRefreshListener(this);

        mRecyclerView = view.findViewById(R.id.game_list_view);
        mRecyclerView.setAdapter(mAdapter);

        final int columnWidth = Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                WIDTH_DIPS + SPACING_DIPS, getResources().getDisplayMetrics()));
        mLayoutManager = new GridAutofitLayoutManager(getContext(), columnWidth);
        mRecyclerView.setLayoutManager(mLayoutManager);
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();

        getGameList().removeRefreshListener(this);
    }

    @Override
    public void onGameListRefresh() {
        mAdapter.notifyDataSetChanged();
    }

    private static class ViewHolder extends RecyclerView.ViewHolder implements View.OnClickListener, View.OnLongClickListener {
        private MainActivity mParent;
        private ImageView mImageView;
        private GameListEntry mEntry;

        public ViewHolder(@NonNull MainActivity parent, @NonNull View itemView) {
            super(itemView);
            mParent = parent;
            mImageView = itemView.findViewById(R.id.imageView);
            mImageView.setOnClickListener(this);
            mImageView.setOnLongClickListener(this);
        }

        public void bindToEntry(GameListEntry entry) {
            mEntry = entry;

            final String coverPath = entry.getCoverPath();
            if (coverPath == null) {
                mImageView.setImageDrawable(ContextCompat.getDrawable(mParent, R.drawable.ic_media_cdrom));
                return;
            }

            new ImageLoadTask(mImageView).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR, coverPath);
        }

        @Override
        public void onClick(View v) {
            mParent.startEmulation(mEntry.getPath(), mParent.shouldResumeStateByDefault());
        }

        @Override
        public boolean onLongClick(View v) {
            PopupMenu menu = new PopupMenu(mParent, v, Gravity.RIGHT | Gravity.TOP);
            menu.getMenuInflater().inflate(R.menu.menu_game_list_entry, menu.getMenu());
            menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                @Override
                public boolean onMenuItemClick(MenuItem item) {
                    int id = item.getItemId();
                    if (id == R.id.game_list_entry_menu_start_game) {
                        mParent.startEmulation(mEntry.getPath(), false);
                        return true;
                    } else if (id == R.id.game_list_entry_menu_resume_game) {
                        mParent.startEmulation(mEntry.getPath(), true);
                        return true;
                    } else if (id == R.id.game_list_entry_menu_properties) {
                        mParent.openGameProperties(mEntry.getPath());
                        return true;
                    }
                    return false;
                }
            });
            menu.show();
            return true;
        }
    }

    private static class ViewAdapter extends RecyclerView.Adapter<ViewHolder> {
        private MainActivity mParent;
        private LayoutInflater mInflater;
        private GameList mGameList;

        public ViewAdapter(@NonNull MainActivity parent, @NonNull GameList gameList) {
            mParent = parent;
            mInflater = LayoutInflater.from(parent);
            mGameList = gameList;
        }

        @NonNull
        @Override
        public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new ViewHolder(mParent, mInflater.inflate(R.layout.layout_game_grid_entry, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
            GameListEntry entry = mGameList.getEntry(position);
            holder.bindToEntry(entry);
        }

        @Override
        public int getItemCount() {
            return mGameList.getEntryCount();
        }

        @Override
        public int getItemViewType(int position) {
            return R.layout.layout_game_grid_entry;
        }
    }
}
