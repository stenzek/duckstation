package com.github.stenzek.duckstation;

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
import androidx.recyclerview.widget.RecyclerView;

public class GameGridFragment extends Fragment implements GameList.OnRefreshListener {
    private static final int SPACING_DIPS = 25;
    private static final int WIDTH_DIPS = 160;

    private final MainActivity mParent;
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
        private final MainActivity mParent;
        private final ImageView mImageView;
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

            // while it loads/generates
            mImageView.setImageDrawable(ContextCompat.getDrawable(mParent, R.drawable.ic_media_cdrom));

            final String coverPath = entry.getCoverPath();
            if (coverPath == null) {
                new GenerateCoverTask(mParent, mImageView, mEntry.getTitle()).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR);
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
            mParent.openGamePopupMenu(v, mEntry);
            return true;
        }
    }

    private static class ViewAdapter extends RecyclerView.Adapter<ViewHolder> {
        private final MainActivity mParent;
        private final LayoutInflater mInflater;
        private final GameList mGameList;

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
