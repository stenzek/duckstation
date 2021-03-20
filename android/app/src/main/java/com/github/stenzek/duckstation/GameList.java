package com.github.stenzek.duckstation;

import android.app.Activity;
import android.os.AsyncTask;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;

public class GameList {
    public interface OnRefreshListener {
        void onGameListRefresh();
    }

    private Activity mContext;
    private GameListEntry[] mEntries;
    private ArrayList<OnRefreshListener> mRefreshListeners = new ArrayList<>();

    public GameList(Activity context) {
        mContext = context;
        mEntries = new GameListEntry[0];
    }

    public void addRefreshListener(OnRefreshListener listener) {
        mRefreshListeners.add(listener);
    }
    public void removeRefreshListener(OnRefreshListener listener) {
        mRefreshListeners.remove(listener);
    }
    public void fireRefreshListeners() {
        for (OnRefreshListener listener : mRefreshListeners)
            listener.onGameListRefresh();
    }

    private class GameListEntryComparator implements Comparator<GameListEntry> {
        @Override
        public int compare(GameListEntry left, GameListEntry right) {
            return left.getTitle().compareTo(right.getTitle());
        }
    }

    public void refresh(boolean invalidateCache, boolean invalidateDatabase, Activity parentActivity) {
        // Search and get entries from native code
        AndroidProgressCallback progressCallback = new AndroidProgressCallback(mContext);
        AsyncTask.execute(() -> {
            AndroidHostInterface.getInstance().refreshGameList(invalidateCache, invalidateDatabase, progressCallback);
            GameListEntry[] newEntries = AndroidHostInterface.getInstance().getGameListEntries();
            Arrays.sort(newEntries, new GameListEntryComparator());

            mContext.runOnUiThread(() -> {
                try {
                    progressCallback.dismiss();
                } catch (Exception e) {
                    Log.e("GameList", "Exception dismissing refresh progress");
                    e.printStackTrace();
                }
                mEntries = newEntries;
                fireRefreshListeners();
            });
        });
    }

    public int getEntryCount() {
        return mEntries.length;
    }

    public GameListEntry getEntry(int index) {
        return mEntries[index];
    }

    public GameListEntry getEntryForPath(String path) {
        for (final GameListEntry entry : mEntries) {
            if (entry.getPath().equals(path))
                return entry;
        }

        return null;
    }
}
