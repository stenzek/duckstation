package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.DialogInterface;
import android.content.res.Configuration;
import android.os.AsyncTask;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.DialogFragment;
import androidx.recyclerview.widget.DividerItemDecoration;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.Arrays;
import java.util.Comparator;

public class AchievementListFragment extends DialogFragment {

    private RecyclerView mRecyclerView;
    private AchievementListFragment.ViewAdapter mAdapter;
    private final Achievement[] mAchievements;
    private DialogInterface.OnDismissListener mOnDismissListener;

    public AchievementListFragment(Achievement[] achievements) {
        mAchievements = achievements;
        sortAchievements();
    }

    public void setOnDismissListener(DialogInterface.OnDismissListener l) {
        mOnDismissListener = l;
    }

    @Override
    public void onDismiss(@NonNull DialogInterface dialog) {
        if (mOnDismissListener != null)
            mOnDismissListener.onDismiss(dialog);

        super.onDismiss(dialog);
    }

    @Override
    public void onResume() {
        super.onResume();

        if (getDialog() == null)
            return;

        final boolean isLandscape = (getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE);
        final float scale = (float) getContext().getResources().getDisplayMetrics().densityDpi / DisplayMetrics.DENSITY_DEFAULT;
        final int width = Math.round((isLandscape ? 700.0f : 400.0f) * scale);
        final int height = Math.round((isLandscape ? 400.0f : 700.0f) * scale);
        getDialog().getWindow().setLayout(width, height);
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_achievement_list, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        mAdapter = new AchievementListFragment.ViewAdapter(getContext(), mAchievements);

        mRecyclerView = view.findViewById(R.id.recyclerView);
        mRecyclerView.setAdapter(mAdapter);
        mRecyclerView.setLayoutManager(new LinearLayoutManager(getContext()));
        mRecyclerView.addItemDecoration(new DividerItemDecoration(mRecyclerView.getContext(),
                DividerItemDecoration.VERTICAL));

        fillHeading(view);
    }

    private void fillHeading(@NonNull View view) {
        final AndroidHostInterface hi = AndroidHostInterface.getInstance();
        final String gameTitle = hi.getCheevoGameTitle();
        if (gameTitle != null) {
            final String formattedTitle = hi.isCheevosChallengeModeActive() ?
                    String.format(getString(R.string.achievement_title_challenge_mode_format_string), gameTitle) :
                    gameTitle;
            ((TextView) view.findViewById(R.id.title)).setText(formattedTitle);
        }

        final int cheevoCount = hi.getCheevoCount();
        final int unlockedCheevoCount = hi.getUnlockedCheevoCount();
        final String summary = String.format(getString(R.string.achievement_summary_format_string),
                unlockedCheevoCount, cheevoCount, hi.getCheevoPointsForGame(), hi.getCheevoMaximumPointsForGame());
        ((TextView) view.findViewById(R.id.summary)).setText(summary);

        ProgressBar pb = ((ProgressBar) view.findViewById(R.id.progressBar));
        pb.setMax(cheevoCount);
        pb.setProgress(unlockedCheevoCount);

        final ImageView icon = ((ImageView) view.findViewById(R.id.icon));
        final String badgePath = hi.getCheevoGameIconPath();
        if (badgePath != null) {
            new ImageLoadTask(icon).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR, badgePath);
        }
    }

    private void sortAchievements() {
        Arrays.sort(mAchievements, (o1, o2) -> {
            if (o2.isLocked() && !o1.isLocked())
                return -1;
            else if (o1.isLocked() && !o2.isLocked())
                return 1;

            return o1.getName().compareTo(o2.getName());
        });
    }

    private static class ViewHolder extends RecyclerView.ViewHolder implements View.OnClickListener, View.OnLongClickListener {
        private final View mItemView;

        public ViewHolder(@NonNull View itemView) {
            super(itemView);
            mItemView = itemView;
            mItemView.setOnClickListener(this);
            mItemView.setOnLongClickListener(this);
        }

        public void bindToEntry(Achievement cheevo) {
            ImageView icon = ((ImageView) mItemView.findViewById(R.id.icon));
            icon.setImageDrawable(mItemView.getContext().getDrawable(R.drawable.ic_baseline_lock_24));

            final String badgePath = cheevo.getBadgePath();
            if (badgePath != null) {
                new ImageLoadTask(icon).executeOnExecutor(AsyncTask.THREAD_POOL_EXECUTOR, badgePath);
            }

            ((TextView) mItemView.findViewById(R.id.title)).setText(cheevo.getName());
            ((TextView) mItemView.findViewById(R.id.description)).setText(cheevo.getDescription());

            ((ImageView) mItemView.findViewById(R.id.locked_icon)).setImageDrawable(
                    mItemView.getContext().getDrawable(cheevo.isLocked() ? R.drawable.ic_baseline_lock_24 : R.drawable.ic_baseline_lock_open_24));

            final String pointsString = String.format(mItemView.getContext().getString(R.string.achievement_points_format_string), cheevo.getPoints());
            ((TextView) mItemView.findViewById(R.id.points)).setText(pointsString);
        }

        @Override
        public void onClick(View v) {
            //
        }

        @Override
        public boolean onLongClick(View v) {
            return false;
        }
    }

    private static class ViewAdapter extends RecyclerView.Adapter<AchievementListFragment.ViewHolder> {
        private final LayoutInflater mInflater;
        private final Achievement[] mAchievements;

        public ViewAdapter(@NonNull Context context, Achievement[] achievements) {
            mInflater = LayoutInflater.from(context);
            mAchievements = achievements;
        }

        @NonNull
        @Override
        public AchievementListFragment.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new AchievementListFragment.ViewHolder(mInflater.inflate(R.layout.layout_achievement_entry, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull AchievementListFragment.ViewHolder holder, int position) {
            holder.bindToEntry(mAchievements[position]);
        }

        @Override
        public int getItemCount() {
            return (mAchievements != null) ? mAchievements.length : 0;
        }

        @Override
        public int getItemViewType(int position) {
            return R.layout.layout_game_list_entry;
        }
    }
}
