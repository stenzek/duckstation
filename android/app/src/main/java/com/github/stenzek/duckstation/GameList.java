package com.github.stenzek.duckstation;

import android.app.Activity;
import android.os.AsyncTask;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;

import java.util.Arrays;
import java.util.Comparator;

public class GameList {
    private Activity mContext;
    private GameListEntry[] mEntries;
    private ListViewAdapter mAdapter;

    public GameList(Activity context) {
        mContext = context;
        mAdapter = new ListViewAdapter();
        mEntries = new GameListEntry[0];
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
                mAdapter.notifyDataSetChanged();
            });
        });
    }

    public int getEntryCount() {
        return mEntries.length;
    }

    public GameListEntry getEntry(int index) {
        return mEntries[index];
    }

    private class ListViewAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return mEntries.length;
        }

        @Override
        public Object getItem(int position) {
            return mEntries[position];
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public int getViewTypeCount() {
            return 1;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(mContext)
                        .inflate(R.layout.game_list_view_entry, parent, false);
            }

            mEntries[position].fillView(convertView);
            return convertView;
        }
    }

    public BaseAdapter getListViewAdapter() {
        return mAdapter;
    }
}
