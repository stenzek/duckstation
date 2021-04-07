package com.github.stenzek.duckstation;

import android.os.AsyncTask;
import android.os.Bundle;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ImageView;
import android.widget.PopupMenu;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.content.ContextCompat;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.DividerItemDecoration;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

public class GameListFragment extends Fragment implements GameList.OnRefreshListener {
    private MainActivity mParent;
    private RecyclerView mRecyclerView;
    private GameListFragment.ViewAdapter mAdapter;

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

        mAdapter = new GameListFragment.ViewAdapter(mParent, getGameList());
        getGameList().addRefreshListener(this);

        mRecyclerView = view.findViewById(R.id.game_list_view);
        mRecyclerView.setAdapter(mAdapter);
        mRecyclerView.setLayoutManager(new LinearLayoutManager(mParent));
        mRecyclerView.addItemDecoration(new DividerItemDecoration(mRecyclerView.getContext(),
                DividerItemDecoration.VERTICAL));
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
        private View mItemView;
        private GameListEntry mEntry;

        public ViewHolder(@NonNull MainActivity parent, @NonNull View itemView) {
            super(itemView);
            mParent = parent;
            mItemView = itemView;
            mItemView.setOnClickListener(this);
            mItemView.setOnLongClickListener(this);
        }

        private String getSubTitle() {
            String fileName = GameListEntry.getFileNameForPath(mEntry.getPath());
            String sizeString = String.format("%.2f MB", (double) mEntry.getSize() / 1048576.0);
            return String.format("%s (%s)", fileName, sizeString);
        }

        public void bindToEntry(GameListEntry entry) {
            mEntry = entry;

            ((TextView) mItemView.findViewById(R.id.game_list_view_entry_title)).setText(entry.getTitle());
            ((TextView) mItemView.findViewById(R.id.game_list_view_entry_subtitle)).setText(getSubTitle());

            int regionDrawableId;
            switch (entry.getRegion()) {
                case NTSC_J:
                    regionDrawableId = R.drawable.flag_jp;
                    break;
                case PAL:
                    regionDrawableId = R.drawable.flag_eu;
                    break;
                case Other:
                    regionDrawableId = R.drawable.ic_baseline_help_24;
                    break;
                case NTSC_U:
                default:
                    regionDrawableId = R.drawable.flag_us;
                    break;
            }

            ((ImageView) mItemView.findViewById(R.id.game_list_view_entry_region_icon))
                    .setImageDrawable(ContextCompat.getDrawable(mItemView.getContext(), regionDrawableId));

            int typeDrawableId;
            switch (entry.getType()) {
                case PSExe:
                    typeDrawableId = R.drawable.ic_emblem_system;
                    break;

                case Playlist:
                    typeDrawableId = R.drawable.ic_baseline_playlist_play_24;
                    break;

                case PSF:
                    typeDrawableId = R.drawable.ic_baseline_library_music_24;
                    break;

                case Disc:
                default:
                    typeDrawableId = R.drawable.ic_media_cdrom;
                    break;
            }

            ImageView icon = ((ImageView) mItemView.findViewById(R.id.game_list_view_entry_type_icon));
            icon.setImageDrawable(ContextCompat.getDrawable(mItemView.getContext(), typeDrawableId));

            final String coverPath = entry.getCoverPath();
            if (coverPath != null) {
                new ImageLoadTask(icon).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR, coverPath);
            }

            int compatibilityDrawableId;
            switch (entry.getCompatibilityRating()) {
                case DoesntBoot:
                    compatibilityDrawableId = R.drawable.ic_star_1;
                    break;
                case CrashesInIntro:
                    compatibilityDrawableId = R.drawable.ic_star_2;
                    break;
                case CrashesInGame:
                    compatibilityDrawableId = R.drawable.ic_star_3;
                    break;
                case GraphicalAudioIssues:
                    compatibilityDrawableId = R.drawable.ic_star_4;
                    break;
                case NoIssues:
                    compatibilityDrawableId = R.drawable.ic_star_5;
                    break;
                case Unknown:
                default:
                    compatibilityDrawableId = R.drawable.ic_star_0;
                    break;
            }

            ((ImageView) mItemView.findViewById(R.id.game_list_view_compatibility_icon))
                    .setImageDrawable(ContextCompat.getDrawable(mItemView.getContext(), compatibilityDrawableId));
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

    private static class ViewAdapter extends RecyclerView.Adapter<GameListFragment.ViewHolder> {
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
        public GameListFragment.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new GameListFragment.ViewHolder(mParent, mInflater.inflate(R.layout.layout_game_list_entry, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull GameListFragment.ViewHolder holder, int position) {
            GameListEntry entry = mGameList.getEntry(position);
            holder.bindToEntry(entry);
        }

        @Override
        public int getItemCount() {
            return mGameList.getEntryCount();
        }

        @Override
        public int getItemViewType(int position) {
            return R.layout.layout_game_list_entry;
        }
    }
}
