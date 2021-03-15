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
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ListView;

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

    public String[] getInputDeviceNames() {
        return (mContentView != null) ? mContentView.getInputDeviceNames() : null;
    }

    public boolean hasInputDeviceVibration(int controllerIndex) {
        return (mContentView != null) ? mContentView.hasInputDeviceVibration(controllerIndex) : null;
    }

    public void setInputDeviceVibration(int controllerIndex, float smallMotor, float largeMotor) {
        if (mContentView != null)
            mContentView.setInputDeviceVibration(controllerIndex, smallMotor, largeMotor);
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
        final boolean saveStateOnExit = getBooleanSetting("Main/SaveStateOnExit", true);
        final boolean resumeState = getIntent().getBooleanExtra("resumeState", saveStateOnExit);
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

    private boolean disableDialogMenuItem(AlertDialog dialog, int index) {
        final ListView listView = dialog.getListView();
        if (listView == null)
            return false;

        final View childItem = listView.getChildAt(index);
        if (childItem == null)
            return false;

        childItem.setEnabled(false);
        childItem.setClickable(false);
        childItem.setOnClickListener((v) -> {});
        return true;
    }

    private void showMenu() {
        if (getBooleanSetting("Main/PauseOnMenu", false) &&
                !AndroidHostInterface.getInstance().isEmulationThreadPaused()) {
            AndroidHostInterface.getInstance().pauseEmulationThread(true);
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        if (mGameTitle != null && !mGameTitle.isEmpty())
            builder.setTitle(mGameTitle);

        builder.setItems(R.array.emulation_menu, (dialogInterface, i) -> {
            switch (i) {
                case 0:     // Load State
                {
                    showSaveStateMenu(false);
                    return;
                }

                case 1:     // Save State
                {
                    showSaveStateMenu(true);
                    return;
                }

                case 2:     // Toggle Fast Forward
                {
                    AndroidHostInterface.getInstance().setFastForwardEnabled(!AndroidHostInterface.getInstance().isFastForwardEnabled());
                    onMenuClosed();
                    return;
                }

                case 3:     // Achievements
                {
                    showAchievementsPopup();
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

        final AlertDialog dialog = builder.create();
        dialog.setOnShowListener(dialogInterface -> {
            // Disable cheevos if not loaded.
            if (AndroidHostInterface.getInstance().getCheevoCount() == 0)
                disableDialogMenuItem(dialog, 3);

            // Disable load state for challenge mode.
            if (AndroidHostInterface.getInstance().isCheevosChallengeModeActive())
                disableDialogMenuItem(dialog, 0);
        });
        dialog.show();
    }

    private void showSaveStateMenu(boolean saving) {
        final SaveStateInfo[] infos = AndroidHostInterface.getInstance().getSaveStateInfo(true);
        if (infos == null) {
            onMenuClosed();
            return;
        }

        final AlertDialog.Builder builder = new AlertDialog.Builder(this);
        final ListView listView = new ListView(this);
        listView.setAdapter(new SaveStateInfo.ListAdapter(this, infos));
        builder.setView(listView);
        builder.setOnDismissListener((dialog) -> {
            onMenuClosed();
        });

        final AlertDialog dialog = builder.create();

        listView.setOnItemClickListener((parent, view, position, id) -> {
            SaveStateInfo info = infos[position];
            if (saving) {
                AndroidHostInterface.getInstance().saveState(info.isGlobal(), info.getSlot());
            } else {
                AndroidHostInterface.getInstance().loadState(info.isGlobal(), info.getSlot());
            }
            dialog.dismiss();
        });

        dialog.show();
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

                case 3:     // Change Touchscreen Controller
                {
                    showTouchscreenControllerMenu();
                    return;
                }

                case 4:     // Settings
                {
                    Intent intent = new Intent(EmulationActivity.this, ControllerSettingsActivity.class);
                    intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                    startActivityForResult(intent, REQUEST_CODE_SETTINGS);
                    return;
                }

                case 5:     // Controller Settings
                {
                    Intent intent = new Intent(EmulationActivity.this, SettingsActivity.class);
                    intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
                    startActivityForResult(intent, REQUEST_CODE_SETTINGS);
                    return;
                }
            }
        });
        builder.setOnCancelListener(dialogInterface -> onMenuClosed());

        final AlertDialog dialog = builder.create();
        dialog.setOnShowListener(dialogInterface -> {
            // Disable patch codes when challenge mode is active.
            if (AndroidHostInterface.getInstance().isCheevosChallengeModeActive())
                disableDialogMenuItem(dialog, 1);
        });
        dialog.show();
    }

    private void showTouchscreenControllerMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(R.string.dialog_touchscreen_controller_settings);
        builder.setItems(R.array.emulation_touchscreen_menu, (dialogInterface, i) -> {
            switch (i) {
                case 0:     // Change Type
                {
                    final String currentValue = getStringSetting("Controller1/TouchscreenControllerView", "");
                    final String[] values = getResources().getStringArray(R.array.settings_touchscreen_controller_view_values);
                    int currentIndex = -1;
                    for (int k = 0; k < values.length; k++) {
                        if (currentValue.equals(values[k])) {
                            currentIndex = k;
                            break;
                        }
                    }

                    final AlertDialog.Builder subBuilder = new AlertDialog.Builder(this);
                    subBuilder.setTitle(R.string.dialog_touchscreen_controller_type);
                    subBuilder.setSingleChoiceItems(R.array.settings_touchscreen_controller_view_entries, currentIndex, (dialog, j) -> {
                        setStringSetting("Controller1/TouchscreenControllerView", values[j]);
                        updateControllers();
                    });
                    subBuilder.setNegativeButton(R.string.dialog_done, (dialog, which) -> {
                        dialog.dismiss();
                    });
                    subBuilder.setOnDismissListener(dialog -> onMenuClosed());
                    subBuilder.create().show();
                }
                break;

                case 1:     // Change Opacity
                {
                    if (mTouchscreenController != null) {
                        AlertDialog.Builder subBuilder = mTouchscreenController.createOpacityDialog(this);
                        subBuilder.setOnDismissListener(dialog -> onMenuClosed());
                        subBuilder.create().show();
                    } else {
                        onMenuClosed();
                    }

                }
                break;

                case 2:     // Add/Remove Buttons
                {
                    if (mTouchscreenController != null) {
                        AlertDialog.Builder subBuilder = mTouchscreenController.createAddRemoveButtonDialog(this);
                        subBuilder.setOnDismissListener(dialog -> onMenuClosed());
                        subBuilder.create().show();
                    } else {
                        onMenuClosed();
                    }
                }
                break;

                case 3:     // Edit Layout
                {
                    if (mTouchscreenController != null) {
                        // we deliberately don't call onMenuClosed() here to keep the system paused.
                        // but we need to re-enable immersive mode to get proper editing.
                        enableFullscreenImmersive();
                        mTouchscreenController.startLayoutEditing();
                    } else {
                        // no controller
                        onMenuClosed();
                    }
                }
                break;
            }
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
        items[numPaths] = getString(R.string.emulation_activity_change_disc_select_new_file);

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

    private void showAchievementsPopup() {
        final Achievement[] achievements = AndroidHostInterface.getInstance().getCheevoList();
        if (achievements == null) {
            onMenuClosed();
            return;
        }

        final AchievementListFragment alf = new AchievementListFragment(achievements);
        alf.show(getSupportFragmentManager(), "fragment_achievement_list");
        alf.setOnDismissListener(dialog -> onMenuClosed());
    }

    /**
     * Touchscreen controller overlay
     */
    TouchscreenControllerView mTouchscreenController;

    public void updateControllers() {
        final String controllerType = getStringSetting("Controller1/Type", "DigitalController");
        final String viewType = getStringSetting("Controller1/TouchscreenControllerView", "digital");
        final boolean autoHideTouchscreenController = getBooleanSetting("Controller1/AutoHideTouchscreenController", false);
        final boolean touchGliding = getBooleanSetting("Controller1/TouchGliding", false);
        final boolean hapticFeedback = getBooleanSetting("Controller1/HapticFeedback", false);
        final boolean vibration = getBooleanSetting("Controller1/Vibration", false);
        final FrameLayout activityLayout = findViewById(R.id.frameLayout);

        Log.i("EmulationActivity", "Controller type: " + controllerType);
        Log.i("EmulationActivity", "View type: " + viewType);

        mContentView.updateInputDevices();
        AndroidHostInterface.getInstance().updateInputMap();

        final boolean hasAnyControllers = mContentView.hasAnyGamePads();
        if (controllerType.equals("none") || viewType.equals("none") || (hasAnyControllers && autoHideTouchscreenController)) {
            if (mTouchscreenController != null) {
                activityLayout.removeView(mTouchscreenController);
                mTouchscreenController = null;
            }
        } else {
            if (mTouchscreenController == null) {
                mTouchscreenController = new TouchscreenControllerView(this);
                activityLayout.addView(mTouchscreenController);
            }

            mTouchscreenController.init(0, controllerType, viewType, hapticFeedback, touchGliding);
        }

        if (vibration)
            mVibratorService = (Vibrator) getSystemService(VIBRATOR_SERVICE);
        else
            mVibratorService = null;

        // Place notifications in the middle of the screen, rather then the bottom (because touchscreen).
        float notificationVerticalPosition = 1.0f;
        float notificationVerticalDirection = -1.0f;
        if (mTouchscreenController != null) {
            notificationVerticalPosition = 0.3f;
            notificationVerticalDirection = -1.0f;
        }
        AndroidHostInterface.getInstance().setFullscreenUINotificationVerticalPosition(
                notificationVerticalPosition, notificationVerticalDirection);
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
