package com.github.stenzek.duckstation;

import android.app.Activity;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.FileUtils;
import android.util.Log;
import android.util.Property;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.ListAdapter;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.ListFragment;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceScreen;
import androidx.recyclerview.widget.DividerItemDecoration;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.Set;

public class GameDirectoriesActivity extends AppCompatActivity {
    private static final int REQUEST_ADD_DIRECTORY_TO_GAME_LIST = 1;
    private static final String FORCE_SAF_CONFIG_KEY = "GameList/ForceStorageAccessFramework";

    private class DirectoryListAdapter extends RecyclerView.Adapter {
        private class Entry {
            private String mPath;
            private boolean mRecursive;

            public Entry(String path, boolean recursive) {
                mPath = path;
                mRecursive = recursive;
            }

            public String getPath() {
                return mPath;
            }

            public boolean isRecursive() {
                return mRecursive;
            }

            public void toggleRecursive() {
                mRecursive = !mRecursive;
            }
        }

        private class EntryComparator implements Comparator<Entry> {
            @Override
            public int compare(Entry left, Entry right) {
                return left.getPath().compareTo(right.getPath());
            }
        }

        private Context mContext;
        private Entry[] mEntries;

        public DirectoryListAdapter(Context context) {
            mContext = context;
            reload();
        }

        public void reload() {
            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(mContext);
            ArrayList<Entry> entries = new ArrayList<>();

            try {
                Set<String> paths = prefs.getStringSet("GameList/Paths", null);
                if (paths != null) {
                    for (String path : paths)
                        entries.add(new Entry(path, false));
                }
            } catch (Exception e) {
            }

            try {
                Set<String> paths = prefs.getStringSet("GameList/RecursivePaths", null);
                if (paths != null) {
                    for (String path : paths)
                        entries.add(new Entry(path, true));
                }
            } catch (Exception e) {
            }

            mEntries = new Entry[entries.size()];
            entries.toArray(mEntries);
            Arrays.sort(mEntries, new EntryComparator());
            notifyDataSetChanged();
        }

        private class ViewHolder extends RecyclerView.ViewHolder implements View.OnClickListener {
            private int mPosition;
            private Entry mEntry;
            private TextView mPathView;
            private TextView mRecursiveView;
            private ImageButton mToggleRecursiveView;
            private ImageButton mRemoveView;

            public ViewHolder(View rootView) {
                super(rootView);
                mPathView = rootView.findViewById(R.id.path);
                mRecursiveView = rootView.findViewById(R.id.recursive);
                mToggleRecursiveView = rootView.findViewById(R.id.toggle_recursive);
                mToggleRecursiveView.setOnClickListener(this);
                mRemoveView = rootView.findViewById(R.id.remove);
                mRemoveView.setOnClickListener(this);
            }

            public void bindData(int position, Entry entry) {
                mPosition = position;
                mEntry = entry;
                updateText();
            }

            private void updateText() {
                mPathView.setText(mEntry.getPath());
                mRecursiveView.setText(getString(mEntry.isRecursive() ? R.string.game_directories_scanning_subdirectories : R.string.game_directories_not_scanning_subdirectories));
                mToggleRecursiveView.setImageDrawable(getDrawable(mEntry.isRecursive() ? R.drawable.ic_baseline_folder_24 : R.drawable.ic_baseline_folder_open_24));
            }

            @Override
            public void onClick(View v) {
                if (mToggleRecursiveView == v) {
                    removeSearchDirectory(mContext, mEntry.getPath(), mEntry.isRecursive());
                    mEntry.toggleRecursive();
                    addSearchDirectory(mContext, mEntry.getPath(), mEntry.isRecursive());
                    updateText();
                } else if (mRemoveView == v) {
                    removeSearchDirectory(mContext, mEntry.getPath(), mEntry.isRecursive());
                    reload();
                }
            }
        }

        @NonNull
        @Override
        public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            final View view = LayoutInflater.from(parent.getContext()).inflate(viewType, parent, false);
            return new ViewHolder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
            ((ViewHolder) holder).bindData(position, mEntries[position]);
        }

        @Override
        public int getItemViewType(int position) {
            return R.layout.layout_game_directory_entry;
        }

        @Override
        public long getItemId(int position) {
            return mEntries[position].getPath().hashCode();
        }

        @Override
        public int getItemCount() {
            return mEntries.length;
        }
    }

    DirectoryListAdapter mDirectoryListAdapter;
    RecyclerView mRecyclerView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_game_directories);
        Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }

        mDirectoryListAdapter = new DirectoryListAdapter(this);
        mRecyclerView = findViewById(R.id.recycler_view);
        mRecyclerView.setAdapter(mDirectoryListAdapter);
        mRecyclerView.setLayoutManager(new LinearLayoutManager(this));
        mRecyclerView.addItemDecoration(new DividerItemDecoration(mRecyclerView.getContext(),
                DividerItemDecoration.VERTICAL));

        findViewById(R.id.fab).setOnClickListener((v) -> startAddGameDirectory());
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_edit_game_directories, menu);

        menu.findItem(R.id.force_saf)
                .setEnabled(android.os.Build.VERSION.SDK_INT < 30)
                .setChecked(PreferenceManager.getDefaultSharedPreferences(this).getBoolean(
                        FORCE_SAF_CONFIG_KEY, false))
                .setOnMenuItemClickListener(item -> {
                    final SharedPreferences sharedPreferences =
                            PreferenceManager.getDefaultSharedPreferences(this);
                    final boolean newValue =!sharedPreferences.getBoolean(
                            FORCE_SAF_CONFIG_KEY, false);
                    sharedPreferences.edit()
                            .putBoolean(FORCE_SAF_CONFIG_KEY, newValue)
                            .commit();
                    item.setChecked(newValue);
                    return true;
                });

        return true;
    }

    @Override
    public boolean onOptionsItemSelected(@NonNull MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            onBackPressed();
            return true;
        }

        if (item.getItemId() == R.id.add_directory) {
            startAddGameDirectory();
            return true;
        } else if (item.getItemId() == R.id.add_path) {
            startAddPath();
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    public static boolean useStorageAccessFramework(Context context) {
        // Use legacy storage on devices older than Android 11... apparently some of them
        // have broken storage access framework....
        if (android.os.Build.VERSION.SDK_INT >= 30)
            return true;

        return PreferenceManager.getDefaultSharedPreferences(context).getBoolean(
                "GameList/ForceStorageAccessFramework", false);
    }

    public static void addSearchDirectory(Context context, String path, boolean recursive) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        final String key = recursive ? "GameList/RecursivePaths" : "GameList/Paths";
        PreferenceHelpers.addToStringList(prefs, key, path);
        Log.i("GameDirectoriesActivity", "Added path '" + path + "' to game list search directories");
    }

    public static void removeSearchDirectory(Context context, String path, boolean recursive) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        final String key = recursive ? "GameList/RecursivePaths" : "GameList/Paths";
        PreferenceHelpers.removeFromStringList(prefs, key, path);
        Log.i("GameDirectoriesActivity", "Removed path '" + path + "' from game list search directories");
    }

    private void startAddGameDirectory() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addCategory(Intent.CATEGORY_DEFAULT);
        i.putExtra(Intent.EXTRA_LOCAL_ONLY, true);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        i.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(Intent.createChooser(i, getString(R.string.main_activity_choose_directory)),
                REQUEST_ADD_DIRECTORY_TO_GAME_LIST);
    }

    private void startAddPath() {
        final EditText text = new EditText(this);
        text.setSingleLine();

        new AlertDialog.Builder(this)
                .setTitle(R.string.edit_game_directories_add_path)
                .setMessage(R.string.edit_game_directories_add_path_summary)
                .setView(text)
                .setPositiveButton("Add", (dialog, which) -> {
                    final String path = text.getText().toString();
                    if (!path.isEmpty()) {
                        addSearchDirectory(GameDirectoriesActivity.this, path, true);
                        mDirectoryListAdapter.reload();
                    }
                })
                .setNegativeButton("Cancel", (dialog, which) -> dialog.dismiss())
                .show();
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        switch (requestCode) {
            case REQUEST_ADD_DIRECTORY_TO_GAME_LIST: {
                if (resultCode != RESULT_OK || data.getData() == null)
                    return;

                // Use legacy storage on devices older than Android 11... apparently some of them
                // have broken storage access framework....
                if (!useStorageAccessFramework(this)) {
                    final String path = FileHelper.getFullPathFromTreeUri(data.getData(), this);
                    if (path != null) {
                        addSearchDirectory(this, path, true);
                        mDirectoryListAdapter.reload();
                        return;
                    }
                }

                try {
                    getContentResolver().takePersistableUriPermission(data.getData(),
                            Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception e) {
                    Toast.makeText(this, "Failed to take persistable permission.", Toast.LENGTH_LONG);
                    e.printStackTrace();
                }

                addSearchDirectory(this, data.getDataString(), true);
                mDirectoryListAdapter.reload();
            }
            break;
        }
    }
}