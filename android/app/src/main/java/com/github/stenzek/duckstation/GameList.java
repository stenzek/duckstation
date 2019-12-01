package com.github.stenzek.duckstation;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.ArraySet;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.TextView;

import androidx.preference.PreferenceManager;

import java.util.Set;

public class GameList {
    static {
        System.loadLibrary("duckstation-native");
    }

    private Context mContext;
    private String mCachePath;
    private String mRedumpDatPath;
    private String[] mSearchDirectories;
    private boolean mSearchRecursively;
    private GameListEntry[] mEntries;

    static private native GameListEntry[] getEntries(String cachePath, String redumpDatPath,
                                                     String[] searchDirectories,
                                                     boolean searchRecursively);

    public GameList(Context context) {
        mContext = context;
        refresh();
    }

    public void refresh() {
        SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(mContext);
        mCachePath = preferences.getString("GameList/CachePath", "");
        mRedumpDatPath = preferences.getString("GameList/RedumpDatPath", "");

        Set<String> searchDirectories =
                preferences.getStringSet("GameList/SearchDirectories", null);
        if (searchDirectories != null) {
            mSearchDirectories = new String[searchDirectories.size()];
            searchDirectories.toArray(mSearchDirectories);
        } else {
            mSearchDirectories = new String[0];
        }

        mSearchRecursively = preferences.getBoolean("GameList/SearchRecursively", true);

        // Search and get entries from native code
        mEntries = getEntries(mCachePath, mRedumpDatPath, mSearchDirectories, mSearchRecursively);
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
        return new ListViewAdapter();
    }
}
