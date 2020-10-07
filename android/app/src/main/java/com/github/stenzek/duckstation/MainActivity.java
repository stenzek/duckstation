package com.github.stenzek.duckstation;

import android.Manifest;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;

import com.google.android.material.floatingactionbutton.FloatingActionButton;
import com.google.android.material.snackbar.Snackbar;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.documentfile.provider.DocumentFile;
import androidx.preference.PreferenceManager;

import android.content.Intent;

import androidx.collection.ArraySet;

import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.AdapterView;
import android.widget.ListView;
import android.widget.PopupMenu;
import android.widget.Toast;

import java.io.File;
import java.util.HashSet;
import java.util.Set;
import java.util.prefs.Preferences;

import static com.google.android.material.snackbar.Snackbar.make;

public class MainActivity extends AppCompatActivity {
    private static final int REQUEST_EXTERNAL_STORAGE_PERMISSIONS = 1;
    private static final int REQUEST_ADD_DIRECTORY_TO_GAME_LIST = 2;

    private GameList mGameList;
    private ListView mGameListView;
    private boolean mHasExternalStoragePermissions = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_main);
        Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        findViewById(R.id.fab_add_game_directory).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startAddGameDirectory();
            }
        });
        findViewById(R.id.fab_resume).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startEmulation(null, true);
            }
        });

        // Set up game list view.
        mGameList = new GameList(this);
        mGameListView = findViewById(R.id.game_list_view);
        mGameListView.setAdapter(mGameList.getListViewAdapter());
        mGameListView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                startEmulation(mGameList.getEntry(position).getPath(), true);
            }
        });
        mGameListView.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, int position,
                                           long id) {
                PopupMenu menu = new PopupMenu(MainActivity.this, view,
                        Gravity.RIGHT | Gravity.TOP);
                menu.getMenuInflater().inflate(R.menu.menu_game_list_entry, menu.getMenu());
                menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(MenuItem item) {
                        int id = item.getItemId();
                        if (id == R.id.game_list_entry_menu_start_game) {
                            startEmulation(mGameList.getEntry(position).getPath(), false);
                            return true;
                        } else if (id == R.id.game_list_entry_menu_resume_game) {
                            startEmulation(mGameList.getEntry(position).getPath(), true);
                            return true;
                        }
                        return false;
                    }
                });
                menu.show();
                return true;
            }
        });

        mHasExternalStoragePermissions = checkForExternalStoragePermissions();
        if (mHasExternalStoragePermissions)
            completeStartup();
    }

    private void completeStartup() {
        if (!AndroidHostInterface.hasInstance() && !AndroidHostInterface.createInstance(this)) {
            Log.i("MainActivity", "Failed to create host interface");
            throw new RuntimeException("Failed to create host interface");
        }

        mGameList.refresh(false, false);
    }

    private void startAddGameDirectory() {
        if (!checkForExternalStoragePermissions())
            return;

        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addCategory(Intent.CATEGORY_DEFAULT);
        i.putExtra(Intent.EXTRA_LOCAL_ONLY, true);
        startActivityForResult(Intent.createChooser(i, "Choose directory"),
                REQUEST_ADD_DIRECTORY_TO_GAME_LIST);
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_main, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_resume) {
            startEmulation(null, true);
        } else if (id == R.id.action_start_bios) {
            startEmulation(null, false);
        } else if (id == R.id.action_add_game_directory) {
            startAddGameDirectory();
        } else if (id == R.id.action_scan_for_new_games) {
            mGameList.refresh(false, false);
        }
        if (id == R.id.action_rescan_all_games) {
            mGameList.refresh(true, true);
        }
        if (id == R.id.action_settings) {
            Intent intent = new Intent(this, SettingsActivity.class);
            startActivity(intent);
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        switch (requestCode) {
            case REQUEST_ADD_DIRECTORY_TO_GAME_LIST: {
                if (resultCode != RESULT_OK)
                    return;

                Uri treeUri = data.getData();
                String path = FileUtil.getFullPathFromTreeUri(treeUri, this);
                if (path.length() < 5) {
                    new AlertDialog.Builder(this)
                            .setTitle("Error")
                            .setMessage("Failed to get path for the selected directory. Please make sure the directory is in external storage.\n" +
                                        "Tap the overflow button in the directory selector.\nSelect \"Show Internal Storage\".\n" +
                                        "Tap the menu button and select your device name.")
                            .setPositiveButton("OK", (dialog, button) -> {})
                            .create()
                            .show();
                    return;
                }

                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
                Set<String> currentValues = prefs.getStringSet("GameList/RecursivePaths", null);
                if (currentValues == null)
                    currentValues = new HashSet<String>();

                currentValues.add(path);
                SharedPreferences.Editor editor = prefs.edit();
                editor.putStringSet("GameList/RecursivePaths", currentValues);
                editor.apply();
                Log.i("MainActivity", "Added path '" + path + "' to game list search directories");
                mGameList.refresh(false, false);
            }
            break;
        }
    }

    private boolean checkForExternalStoragePermissions() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED &&
                ContextCompat
                        .checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                        PackageManager.PERMISSION_GRANTED) {
            return true;
        }

        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE},
                REQUEST_EXTERNAL_STORAGE_PERMISSIONS);
        return false;
    }

    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        // check that all were successful
        for (int i = 0; i < grantResults.length; i++) {
            if (grantResults[i] == PackageManager.PERMISSION_GRANTED) {
                if (!mHasExternalStoragePermissions) {
                    mHasExternalStoragePermissions = true;
                    completeStartup();
                }
            } else {
                Toast.makeText(this,
                        "External storage permissions are required to use DuckStation.",
                        Toast.LENGTH_LONG);
                finish();
            }
        }
    }

    private boolean startEmulation(String bootPath, boolean resumeState) {
        Intent intent = new Intent(this, EmulationActivity.class);
        intent.putExtra("bootPath", bootPath);
        intent.putExtra("resumeState", resumeState);
        startActivity(intent);
        return true;
    }
}
