package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
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
    private boolean mStopRequested = false;
    private boolean mApplySettingsOnSurfaceRestored = false;
    private String mGameTitle = null;
    private EmulationSurfaceView mContentView;
    private int mSaveStateSlot = 0;

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

    private void setStringSetting(String key, String value) {
        SharedPreferences.Editor editor = mPreferences.edit();
        editor.putString(key, value);
        editor.apply();
    }

    private void reportErrorOnUIThread(String message) {
        // Toast.makeText(this, message, Toast.LENGTH_LONG);
        new AlertDialog.Builder(this)
                .setTitle("Error")
                .setMessage(message)
                .setPositiveButton("OK", (dialog, button) -> {
                    dialog.dismiss();
                    enableFullscreenImmersive();
                })
                .create()
                .show();
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
                        enableFullscreenImmersive();
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
            AndroidHostInterface.getInstance().stopEmulationThread();
            if (!mWasDestroyed && !mStopRequested)
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

    /// Ends the activity if it was restored without properly being created.
    private boolean checkActivityIsValid() {
        if (!AndroidHostInterface.hasInstance() || !AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
            finish();
            return false;
        }

        return true;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Once we get a surface, we can boot.
        if (AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
            AndroidHostInterface.getInstance().surfaceChanged(holder.getSurface(), format, width, height);
            updateOrientation();

            if (mApplySettingsOnSurfaceRestored) {
                AndroidHostInterface.getInstance().applySettings();
                mApplySettingsOnSurfaceRestored = false;
            }

            if (AndroidHostInterface.getInstance().isEmulationThreadPaused())
                AndroidHostInterface.getInstance().pauseEmulationThread(false);

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
        if (!mStopRequested)
            AndroidHostInterface.getInstance().saveResumeState(true);

        AndroidHostInterface.getInstance().surfaceChanged(null, 0, 0, 0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);
        Log.i("EmulationActivity", "OnCreate");

        // we might be coming from a third-party launcher if the host interface isn't setup
        if (!AndroidHostInterface.hasInstance() && !AndroidHostInterface.createInstance(this)) {
            finish();
            return;
        }

        enableFullscreenImmersive();
        setContentView(R.layout.activity_emulation);

        mContentView = findViewById(R.id.fullscreen_content);
        mContentView.getHolder().addCallback(this);
        mContentView.setFocusable(true);
        mContentView.requestFocus();

        // Hook up controller input.
        updateControllers();
        registerInputDeviceListener();
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

        unregisterInputDeviceListener();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (!checkActivityIsValid()) {
            // we must've got killed off in the background :(
            return;
        }

        if (requestCode == REQUEST_CODE_SETTINGS) {
            if (AndroidHostInterface.getInstance().isEmulationThreadRunning())
                applySettings();
        } else if (requestCode == REQUEST_IMPORT_PATCH_CODES) {
            if (data != null)
                importPatchesFromFile(data.getData());
        }
    }

    @Override
    public void onBackPressed() {
        showMenu();
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        if (checkActivityIsValid())
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

    private void enableFullscreenImmersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        if (mContentView != null)
            mContentView.requestFocus();
    }

    private static final int REQUEST_CODE_SETTINGS = 0;
    private static final int REQUEST_IMPORT_PATCH_CODES = 1;

    private void onMenuClosed() {
        enableFullscreenImmersive();

        if (AndroidHostInterface.getInstance().isEmulationThreadPaused())
            AndroidHostInterface.getInstance().pauseEmulationThread(false);
    }

    private void showMenu() {
        if (getBooleanSetting("Main/PauseOnMenu", false) &&
                !AndroidHostInterface.getInstance().isEmulationThreadPaused())
        {
            AndroidHostInterface.getInstance().pauseEmulationThread(true);
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setItems(R.array.emulation_menu, (dialogInterface, i) -> {
            switch (i) {
                case 0:     // Quick Load
                {
                    AndroidHostInterface.getInstance().loadState(false, mSaveStateSlot);
                    onMenuClosed();
                    return;
                }

                case 1:     // Quick Save
                {
                    AndroidHostInterface.getInstance().saveState(false, mSaveStateSlot);
                    onMenuClosed();
                    return;
                }

                case 2:     // Save State Slot
                {
                    showSaveStateSlotMenu();
                    return;
                }

                case 3:     // Toggle Fast Forward
                {
                    AndroidHostInterface.getInstance().setFastForwardEnabled(!AndroidHostInterface.getInstance().isFastForwardEnabled());
                    onMenuClosed();
                    return;
                }

                case 4:     // More Options
                {
                    showMoreMenu();
                    return;
                }

                case 5:     // Quit
                {
                    mStopRequested = true;
                    finish();
                    return;
                }
            }
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void showSaveStateSlotMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setSingleChoiceItems(R.array.emulation_save_state_slot_menu, mSaveStateSlot, (dialogInterface, i) -> {
            mSaveStateSlot = i;
            dialogInterface.dismiss();
            onMenuClosed();
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void showMoreMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        if (mGameTitle != null && !mGameTitle.isEmpty())
            builder.setTitle(mGameTitle);

        builder.setItems(R.array.emulation_more_menu, (dialogInterface, i) -> {
            switch (i) {
                case 0:     // Reset
                {
                    AndroidHostInterface.getInstance().resetSystem();
                    onMenuClosed();
                    return;
                }

                case 1:     // Patch Codes
                {
                    showPatchesMenu();
                    return;
                }

                case 2:     // Change Disc
                {
                    onMenuClosed();
                    return;
                }

                case 3:     // Change Touchscreen Controller
                {
                    showTouchscreenControllerMenu();
                    onMenuClosed();
                    return;
                }

                case 4:     // Settings
                {
                    Intent intent = new Intent(EmulationActivity.this, SettingsActivity.class);
                    intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                    startActivityForResult(intent, REQUEST_CODE_SETTINGS);
                    return;
                }
            }
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void showTouchscreenControllerMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setItems(R.array.settings_touchscreen_controller_view_entries, (dialogInterface, i) -> {
            String[] values = getResources().getStringArray(R.array.settings_touchscreen_controller_view_values);
            setStringSetting("Controller1/TouchscreenControllerView", values[i]);
            updateControllers();
            onMenuClosed();
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void showPatchesMenu() {
        final PatchCode[] codes = AndroidHostInterface.getInstance().getPatchCodeList();

        AlertDialog.Builder builder = new AlertDialog.Builder(this);

        CharSequence[] items = new CharSequence[(codes != null) ? (codes.length + 1) : 1];
        items[0] = "Import Patch Codes...";
        if (codes != null) {
            for (int i = 0; i < codes.length; i++) {
                final PatchCode cc = codes[i];
                items[i + 1] = String.format("%s %s", cc.isEnabled() ? "(ON)" : "(OFF)", cc.getDescription());
            }
        }

        builder.setItems(items, (dialogInterface, i) -> {
            if (i > 0) {
                AndroidHostInterface.getInstance().setPatchCodeEnabled(i - 1, !codes[i - 1].isEnabled());
                onMenuClosed();
            } else {
                Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                intent.setType("*/*");
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(Intent.createChooser(intent, "Choose Patch Code File"), REQUEST_IMPORT_PATCH_CODES);
            }
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void importPatchesFromFile(Uri uri) {
        String str = FileUtil.readFileFromUri(this, uri, 512 * 1024);
        if (str == null || !AndroidHostInterface.getInstance().importPatchCodesFromString(str)) {
            reportErrorOnUIThread("Failed to import patch codes. Make sure you selected a PCSXR or Libretro format file.");
        }
    }

    /**
     * Touchscreen controller overlay
     */
    TouchscreenControllerView mTouchscreenController;

    public void updateControllers() {
        final String controllerType = getStringSetting("Controller1/Type", "DigitalController");
        final String viewType = getStringSetting("Controller1/TouchscreenControllerView", "digital");
        final boolean autoHideTouchscreenController = getBooleanSetting("Controller1/AutoHideTouchscreenController", false);
        final FrameLayout activityLayout = findViewById(R.id.frameLayout);

        Log.i("EmulationActivity", "Controller type: " + controllerType);
        Log.i("EmulationActivity", "View type: " + viewType);

        final boolean hasAnyControllers = mContentView.initControllerMapping(controllerType);

        if (controllerType == "none" || viewType == "none" || (hasAnyControllers && autoHideTouchscreenController)) {
            if (mTouchscreenController != null) {
                activityLayout.removeView(mTouchscreenController);
                mTouchscreenController = null;
            }
        } else {
            if (mTouchscreenController == null) {
                mTouchscreenController = new TouchscreenControllerView(this);
                activityLayout.addView(mTouchscreenController);
            }

            mTouchscreenController.init(0, controllerType, viewType);
        }
    }

    private InputManager.InputDeviceListener mInputDeviceListener;

    private void registerInputDeviceListener() {
        if (mInputDeviceListener != null)
            return;

        mInputDeviceListener = new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int i) {
                Log.i("EmulationActivity", String.format("InputDeviceAdded %d", i));
                updateControllers();
            }

            @Override
            public void onInputDeviceRemoved(int i) {
                Log.i("EmulationActivity", String.format("InputDeviceRemoved %d", i));
                updateControllers();
            }

            @Override
            public void onInputDeviceChanged(int i) {
                Log.i("EmulationActivity", String.format("InputDeviceChanged %d", i));
                updateControllers();
            }
        };

        InputManager inputManager = ((InputManager) getSystemService(Context.INPUT_SERVICE));
        if (inputManager != null)
            inputManager.registerInputDeviceListener(mInputDeviceListener, null);
    }

    private void unregisterInputDeviceListener() {
        if (mInputDeviceListener == null)
            return;

        InputManager inputManager = ((InputManager) getSystemService(Context.INPUT_SERVICE));
        if (inputManager != null)
            inputManager.unregisterInputDeviceListener(mInputDeviceListener);

        mInputDeviceListener = null;
    }
}
