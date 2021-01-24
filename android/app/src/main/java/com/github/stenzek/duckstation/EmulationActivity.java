package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Vibrator;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
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
                .setTitle(R.string.emulation_activity_error)
                .setMessage(message)
                .setPositiveButton(R.string.emulation_activity_ok, (dialog, button) -> {
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
                    .setTitle(R.string.emulation_activity_error)
                    .setMessage(message)
                    .setPositiveButton(R.string.emulation_activity_ok, (dialog, button) -> {
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

    private EmulationThread mEmulationThread;

    private void stopEmulationThread() {
        if (mEmulationThread == null)
            return;

        mEmulationThread.stopAndJoin();
        mEmulationThread = null;
    }

    public void onEmulationStarted() {
        runOnUiThread(() -> {
            updateRequestedOrientation();
            updateOrientation();
        });
    }

    public void onEmulationStopped() {
        runOnUiThread(() -> {
            if (!mWasDestroyed && !mStopRequested)
                finish();
        });
    }

    public void onGameTitleChanged(String title) {
        runOnUiThread(() -> {
            mGameTitle = title;
        });
    }

    public float getRefreshRate() {
        WindowManager windowManager = getWindowManager();
        if (windowManager == null) {
            windowManager = ((WindowManager) getSystemService(Context.WINDOW_SERVICE));
            if (windowManager == null)
                return -1.0f;
        }

        Display display = windowManager.getDefaultDisplay();
        if (display == null)
            return -1.0f;

        return display.getRefreshRate();
    }

    public void openPauseMenu() {
        runOnUiThread(() -> {
            showMenu();
        });
    }

    private void doApplySettings() {
        AndroidHostInterface.getInstance().applySettings();
        updateRequestedOrientation();
        updateControllers();
        updateSustainedPerformanceMode();
    }

    private void applySettings() {
        if (!AndroidHostInterface.getInstance().isEmulationThreadRunning())
            return;

        if (AndroidHostInterface.getInstance().hasSurface()) {
            doApplySettings();
        } else {
            mApplySettingsOnSurfaceRestored = true;
        }
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
        AndroidHostInterface.getInstance().surfaceChanged(holder.getSurface(), format, width, height);

        if (mEmulationThread != null) {
            updateOrientation();

            if (mApplySettingsOnSurfaceRestored) {
                mApplySettingsOnSurfaceRestored = false;
                doApplySettings();
            }

            return;
        }

        final String bootPath = getIntent().getStringExtra("bootPath");
        final boolean resumeState = getIntent().getBooleanExtra("resumeState", false);
        final String bootSaveStatePath = getIntent().getStringExtra("saveStatePath");

        mEmulationThread = EmulationThread.create(this, bootPath, resumeState, bootSaveStatePath);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i("EmulationActivity", "Surface destroyed");

        // Save the resume state in case we never get back again...
        if (AndroidHostInterface.getInstance().isEmulationThreadRunning() && !mStopRequested)
            AndroidHostInterface.getInstance().saveResumeState(true);

        AndroidHostInterface.getInstance().surfaceChanged(null, 0, 0, 0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);
        super.onCreate(savedInstanceState);

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
        mContentView.setFocusableInTouchMode(true);
        mContentView.setFocusable(true);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mContentView.setFocusedByDefault(true);
        }
        mContentView.requestFocus();

        // Sort out rotation.
        updateOrientation();
        updateSustainedPerformanceMode();

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
        if (mEmulationThread != null) {
            mWasDestroyed = true;
            stopEmulationThread();
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
            if (AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
                applySettings();
            }
        } else if (requestCode == REQUEST_IMPORT_PATCH_CODES) {
            if (data == null)
                return;

            importPatchesFromFile(data.getData());
        } else if (requestCode == REQUEST_CHANGE_DISC_FILE) {
            if (data == null)
                return;

            String path = GameDirectoriesActivity.getPathFromUri(this, data.getData());
            if (path == null)
                return;

            AndroidHostInterface.getInstance().setMediaFilename(path);
        }
    }

    @Override
    public void onBackPressed() {
        showMenu();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            if (mContentView.onKeyDown(event.getKeyCode(), event))
                return true;
        } else if (event.getAction() == KeyEvent.ACTION_UP) {
            if (mContentView.onKeyUp(event.getKeyCode(), event))
                return true;
        }

        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        if (mContentView.onGenericMotionEvent(ev))
            return true;

        return super.dispatchGenericMotionEvent(ev);
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        if (checkActivityIsValid())
            updateOrientation(newConfig.orientation);
    }

    private void updateRequestedOrientation() {
        final String orientation = getStringSetting("Main/EmulationScreenOrientation", "unspecified");
        if (orientation.equals("portrait"))
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT);
        else if (orientation.equals("landscape"))
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
        else
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
    }

    private void updateOrientation() {
        final int orientation = getResources().getConfiguration().orientation;
        updateOrientation(orientation);
    }

    private void updateOrientation(int newOrientation) {
        if (newOrientation == Configuration.ORIENTATION_PORTRAIT)
            AndroidHostInterface.getInstance().setDisplayAlignment(AndroidHostInterface.DISPLAY_ALIGNMENT_TOP_OR_LEFT);
        else
            AndroidHostInterface.getInstance().setDisplayAlignment(AndroidHostInterface.DISPLAY_ALIGNMENT_CENTER);

        if (mTouchscreenController != null)
            mTouchscreenController.updateOrientation();
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
    private static final int REQUEST_CHANGE_DISC_FILE = 2;

    private void onMenuClosed() {
        enableFullscreenImmersive();

        if (AndroidHostInterface.getInstance().isEmulationThreadPaused())
            AndroidHostInterface.getInstance().pauseEmulationThread(false);
    }

    private void showMenu() {
        if (getBooleanSetting("Main/PauseOnMenu", false) &&
                !AndroidHostInterface.getInstance().isEmulationThreadPaused()) {
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
                    showDiscChangeMenu();
                    return;
                }

                case 3:     // Settings
                {
                    Intent intent = new Intent(EmulationActivity.this, SettingsActivity.class);
                    intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                    startActivityForResult(intent, REQUEST_CODE_SETTINGS);
                    return;
                }

                case 4:     // Change Touchscreen Controller
                {
                    showTouchscreenControllerMenu();
                    return;
                }

                case 5:     // Edit Touchscreen Controller Layout
                {
                    if (mTouchscreenController != null)
                        mTouchscreenController.startLayoutEditing();

                    onMenuClosed();
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
        final AlertDialog.Builder builder = new AlertDialog.Builder(this);

        final PatchCode[] codes = AndroidHostInterface.getInstance().getPatchCodeList();
        if (codes != null) {
            CharSequence[] items = new CharSequence[codes.length];
            boolean[] itemsChecked = new boolean[codes.length];
            for (int i = 0; i < codes.length; i++) {
                final PatchCode cc = codes[i];
                items[i] = cc.getDescription();
                itemsChecked[i] = cc.isEnabled();
            }

            builder.setMultiChoiceItems(items, itemsChecked, (dialogInterface, i, checked) -> {
                AndroidHostInterface.getInstance().setPatchCodeEnabled(i, checked);
            });
        }

        builder.setNegativeButton(R.string.emulation_activity_ok, (dialogInterface, i) -> {
            dialogInterface.dismiss();
        });
        builder.setNeutralButton(R.string.emulation_activity_import_patch_codes, (dialogInterface, i) -> {
            Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
            intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
            intent.setType("*/*");
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(Intent.createChooser(intent, getString(R.string.emulation_activity_choose_patch_code_file)), REQUEST_IMPORT_PATCH_CODES);
        });

        builder.setOnDismissListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    private void importPatchesFromFile(Uri uri) {
        String str = FileUtil.readFileFromUri(this, uri, 512 * 1024);
        if (str == null || !AndroidHostInterface.getInstance().importPatchCodesFromString(str)) {
            reportErrorOnUIThread(getString(R.string.emulation_activity_failed_to_import_patch_codes));
        }
    }

    private void showDiscChangeMenu() {
        final String[] paths = AndroidHostInterface.getInstance().getMediaPlaylistPaths();
        final int currentPath = AndroidHostInterface.getInstance().getMediaPlaylistIndex();
        final int numPaths = (paths != null) ? paths.length : 0;

        AlertDialog.Builder builder = new AlertDialog.Builder(this);

        CharSequence[] items = new CharSequence[numPaths + 1];
        for (int i = 0; i < numPaths; i++)
            items[i] = GameListEntry.getFileNameForPath(paths[i]);
        items[numPaths] = "Select New File...";

        builder.setSingleChoiceItems(items, (currentPath < numPaths) ? currentPath : -1, (dialogInterface, i) -> {
            dialogInterface.dismiss();
            onMenuClosed();

            if (i < numPaths) {
                AndroidHostInterface.getInstance().setMediaPlaylistIndex(i);
            } else {
                Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                intent.setType("*/*");
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(Intent.createChooser(intent, getString(R.string.main_activity_choose_disc_image)), REQUEST_CHANGE_DISC_FILE);
            }
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());
        builder.create().show();
    }

    /**
     * Touchscreen controller overlay
     */
    TouchscreenControllerView mTouchscreenController;

    public void updateControllers() {
        final String controllerType = getStringSetting("Controller1/Type", "DigitalController");
        final String viewType = getStringSetting("Controller1/TouchscreenControllerView", "digital");
        final boolean autoHideTouchscreenController = getBooleanSetting("Controller1/AutoHideTouchscreenController", false);
        final boolean hapticFeedback = getBooleanSetting("Controller1/HapticFeedback", false);
        final boolean vibration = getBooleanSetting("Controller1/Vibration", false);
        final FrameLayout activityLayout = findViewById(R.id.frameLayout);

        Log.i("EmulationActivity", "Controller type: " + controllerType);
        Log.i("EmulationActivity", "View type: " + viewType);

        final boolean hasAnyControllers = mContentView.initControllerMapping(controllerType);

        if (controllerType.equals("none") || viewType.equals("none") || (hasAnyControllers && autoHideTouchscreenController)) {
            if (mTouchscreenController != null) {
                activityLayout.removeView(mTouchscreenController);
                mTouchscreenController = null;
                mVibratorService = null;
            }
        } else {
            if (mTouchscreenController == null) {
                mTouchscreenController = new TouchscreenControllerView(this);
                if (vibration)
                    mVibratorService = (Vibrator) getSystemService(VIBRATOR_SERVICE);

                activityLayout.addView(mTouchscreenController);
            }

            mTouchscreenController.init(0, controllerType, viewType, hapticFeedback);
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

    private Vibrator mVibratorService;

    public void setVibration(boolean enabled) {
        if (mVibratorService == null)
            return;

        runOnUiThread(() -> {
            if (mVibratorService == null)
                return;

            if (enabled)
                mVibratorService.vibrate(1000);
            else
                mVibratorService.cancel();
        });
    }

    private boolean mSustainedPerformanceModeEnabled = false;

    private void updateSustainedPerformanceMode() {
        final boolean enabled = getBooleanSetting("Main/SustainedPerformanceMode", false);
        if (mSustainedPerformanceModeEnabled == enabled)
            return;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            getWindow().setSustainedPerformanceMode(enabled);
            Log.i("EmulationActivity", String.format("%s sustained performance mode.", enabled ? "enabling" : "disabling"));
        } else {
            Log.e("EmulationActivity", "Sustained performance mode not supported.");
        }
        mSustainedPerformanceModeEnabled = enabled;

    }
}
