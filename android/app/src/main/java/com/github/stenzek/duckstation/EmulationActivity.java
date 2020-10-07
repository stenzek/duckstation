package com.github.stenzek.duckstation;

import android.annotation.SuppressLint;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.util.AndroidException;
import android.util.Log;
import android.view.Menu;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.MenuItem;
import android.widget.FrameLayout;
import android.widget.Toast;

import androidx.appcompat.widget.PopupMenu;
import androidx.preference.PreferenceManager;

/**
 * An example full-screen activity that shows and hides the system UI (i.e.
 * status bar and navigation/system bar) with user interaction.
 */
public class EmulationActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    /**
     * Settings interfaces.
     */
    private SharedPreferences mPreferences;
    private boolean mWasDestroyed = false;
    private boolean mWasPausedOnSurfaceLoss = false;
    private boolean mApplySettingsOnSurfaceRestored = false;
    private String mGameTitle = null;
    private EmulationSurfaceView mContentView;

    private boolean getBooleanSetting(String key, boolean defaultValue) {
        return mPreferences.getBoolean(key, defaultValue);
    }

    private void setBooleanSetting(String key, boolean value) {
        SharedPreferences.Editor editor = mPreferences.edit();
        editor.putBoolean(key, value);
        editor.apply();
    }

    private String getStringSetting(String key, String defaultValue) {
        return mPreferences.getString(key, defaultValue);
    }

    public void reportError(String message) {
        Log.e("EmulationActivity", message);

        Object lock = new Object();
        runOnUiThread(() -> {
            // Toast.makeText(this, message, Toast.LENGTH_LONG);
            new AlertDialog.Builder(this)
                    .setTitle("Error")
                    .setMessage(message)
                    .setPositiveButton("OK", (dialog, button) -> {
                        dialog.dismiss();
                        synchronized (lock) {
                            lock.notify();
                        }
                    })
                    .create()
                    .show();
        });

        synchronized (lock) {
            try {
                lock.wait();
            } catch (InterruptedException e) {
            }
        }
    }

    public void reportMessage(String message) {
        Log.i("EmulationActivity", message);
        runOnUiThread(() -> {
            Toast.makeText(this, message, Toast.LENGTH_SHORT);
        });
    }

    public void onEmulationStarted() {
    }

    public void onEmulationStopped() {
        runOnUiThread(() -> {
            if (!mWasDestroyed)
                finish();
        });
    }

    public void onGameTitleChanged(String title) {
        runOnUiThread(() -> {
            mGameTitle = title;
        });
    }

    private void applySettings() {
        if (!AndroidHostInterface.getInstance().isEmulationThreadRunning())
            return;

        if (AndroidHostInterface.getInstance().hasSurface())
            AndroidHostInterface.getInstance().applySettings();
        else
            mApplySettingsOnSurfaceRestored = true;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Once we get a surface, we can boot.
        if (AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
            final boolean hadSurface = AndroidHostInterface.getInstance().hasSurface();
            AndroidHostInterface.getInstance().surfaceChanged(holder.getSurface(), format, width, height);
            updateOrientation();

            if (holder.getSurface() != null && !hadSurface && AndroidHostInterface.getInstance().isEmulationThreadPaused() && !mWasPausedOnSurfaceLoss) {
                AndroidHostInterface.getInstance().pauseEmulationThread(false);
            }

            if (mApplySettingsOnSurfaceRestored) {
                AndroidHostInterface.getInstance().applySettings();
                mApplySettingsOnSurfaceRestored = false;
            }

            return;
        }

        final String bootPath = getIntent().getStringExtra("bootPath");
        final boolean resumeState = getIntent().getBooleanExtra("resumeState", false);
        final String bootSaveStatePath = getIntent().getStringExtra("saveStatePath");

        AndroidHostInterface.getInstance().startEmulationThread(this, holder.getSurface(), bootPath, resumeState, bootSaveStatePath);
        updateOrientation();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (!AndroidHostInterface.getInstance().isEmulationThreadRunning())
            return;

        Log.i("EmulationActivity", "Surface destroyed");

        // Save the resume state in case we never get back again...
        AndroidHostInterface.getInstance().saveResumeState(true);

        mWasPausedOnSurfaceLoss = AndroidHostInterface.getInstance().isEmulationThreadPaused();
        AndroidHostInterface.getInstance().pauseEmulationThread(true);
        AndroidHostInterface.getInstance().surfaceChanged(null, 0, 0, 0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);
        Log.i("EmulationActivity", "OnCreate");

        enableFullscreenImmersive();
        setContentView(R.layout.activity_emulation);

        mContentView = findViewById(R.id.fullscreen_content);
        mContentView.getHolder().addCallback(this);

        // Hook up controller input.
        final String controllerType = getStringSetting("Controller1/Type", "DigitalController");
        Log.i("EmulationActivity", "Controller type: " + controllerType);
        mContentView.initControllerKeyMapping(controllerType);

        // Create touchscreen controller.
        FrameLayout activityLayout = findViewById(R.id.frameLayout);
        mTouchscreenController = new TouchscreenControllerView(this);
        activityLayout.addView(mTouchscreenController);
        mTouchscreenController.init(0, controllerType);
        setTouchscreenControllerVisibility(getBooleanSetting("Controller1/EnableTouchscreenController", true));
    }

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        enableFullscreenImmersive();
    }

    @Override
    protected void onPostResume() {
        super.onPostResume();
        enableFullscreenImmersive();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("EmulationActivity", "OnStop");
        if (AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
            mWasDestroyed = true;
            AndroidHostInterface.getInstance().stopEmulationThread();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQUEST_CODE_SETTINGS) {
            if (AndroidHostInterface.getInstance().isEmulationThreadRunning())
                applySettings();
        }
    }

    @Override
    public void onBackPressed() {
        showMenu();
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        updateOrientation(newConfig.orientation);
    }

    private void updateOrientation() {
        final int orientation = getResources().getConfiguration().orientation;
        updateOrientation(orientation);
    }

    private void updateOrientation(int newOrientation) {
        if (!AndroidHostInterface.getInstance().isEmulationThreadRunning())
            return;

        if (newOrientation == Configuration.ORIENTATION_PORTRAIT)
            AndroidHostInterface.getInstance().setDisplayAlignment(AndroidHostInterface.DISPLAY_ALIGNMENT_TOP_OR_LEFT);
        else
            AndroidHostInterface.getInstance().setDisplayAlignment(AndroidHostInterface.DISPLAY_ALIGNMENT_CENTER);
    }

    private void enableFullscreenImmersive()
    {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private static final int REQUEST_CODE_SETTINGS = 0;

    private void showMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        if (mGameTitle != null && !mGameTitle.isEmpty())
            builder.setTitle(mGameTitle);

        builder.setItems(R.array.emulation_menu, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialogInterface, int i) {
                switch (i)
                {
                    case 0:     // Quick Load
                    {
                        AndroidHostInterface.getInstance().loadState(false, 0);
                        return;
                    }

                    case 1:     // Quick Save
                    {
                        AndroidHostInterface.getInstance().saveState(false, 0);
                        return;
                    }

                    case 2:     // Toggle Speed Limiter
                    {
                        boolean newSetting = !getBooleanSetting("Main/SpeedLimiterEnabled", true);
                        setBooleanSetting("Main/SpeedLimiterEnabled", newSetting);
                        applySettings();
                        return;
                    }

                    case 3:     // More Options
                    {
                        showMoreMenu();
                        return;
                    }

                    case 4:     // Quit
                    {
                        finish();
                        return;
                    }
                }
            }
        });
        builder.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialogInterface) {
                enableFullscreenImmersive();
            }
        });
        builder.create().show();
    }

    private void showMoreMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        if (mGameTitle != null && !mGameTitle.isEmpty())
            builder.setTitle(mGameTitle);

        builder.setItems(R.array.emulation_more_menu, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialogInterface, int i) {
                switch (i)
                {
                    case 0:     // Reset
                    {
                        AndroidHostInterface.getInstance().resetSystem();
                        return;
                    }

                    case 1:     // Cheats
                    {
                        showCheatsMenu();
                        return;
                    }

                    case 2:     // Change Disc
                    {
                        return;
                    }

                    case 3:     // Toggle Touchscreen Controller
                    {
                        setTouchscreenControllerVisibility(!mTouchscreenControllerVisible);
                        return;
                    }

                    case 4:     // Settings
                    {
                        Intent intent = new Intent(EmulationActivity.this, SettingsActivity.class);
                        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                        startActivityForResult(intent, REQUEST_CODE_SETTINGS);
                        return;
                    }

                    case 5:     // Quit
                    {
                        finish();
                        return;
                    }
                }
            }
        });
        builder.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialogInterface) {
                enableFullscreenImmersive();
            }
        });
        builder.create().show();
    }

    private void showCheatsMenu() {
        final CheatCode[] cheats = AndroidHostInterface.getInstance().getCheatList();
        if (cheats == null) {
            AndroidHostInterface.getInstance().addOSDMessage("No cheats are loaded.", 5.0f);
            return;
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this);

        CharSequence[] items = new CharSequence[cheats.length];
        for (int i = 0; i < cheats.length; i++) {
            final CheatCode cc = cheats[i];
            items[i] = String.format("%s %s", cc.isEnabled() ? "(ON)" : "(OFF)", cc.getName());
        }

        builder.setItems(items, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialogInterface, int i) {
                AndroidHostInterface.getInstance().setCheatEnabled(i, !cheats[i].isEnabled());
            }
        });
        builder.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface dialogInterface) {
                enableFullscreenImmersive();
            }
        });
        builder.create().show();
    }

    /**
     * Touchscreen controller overlay
     */
    TouchscreenControllerView mTouchscreenController;
    private boolean mTouchscreenControllerVisible = true;

    private void setTouchscreenControllerVisibility(boolean visible) {
        mTouchscreenControllerVisible = visible;
        mTouchscreenController.setVisibility(visible ? View.VISIBLE : View.INVISIBLE);
    }
}
