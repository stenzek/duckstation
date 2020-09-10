package com.github.stenzek.duckstation;

import android.annotation.SuppressLint;

import androidx.annotation.NonNull;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Menu;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.MenuItem;
import android.widget.FrameLayout;
import android.widget.Toast;

import androidx.preference.PreferenceManager;

/**
 * An example full-screen activity that shows and hides the system UI (i.e.
 * status bar and navigation/system bar) with user interaction.
 */
public class EmulationActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    /**
     * Settings interfaces.
     */
    SharedPreferences mPreferences;

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
            finish();
        });
    }

    public void onGameTitleChanged(String title) {
        runOnUiThread(() -> {
            setTitle(title);
        });
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

        Log.i("EmulationActivity", "Stopping emulation thread");
        AndroidHostInterface.getInstance().stopEmulationThread();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mPreferences = PreferenceManager.getDefaultSharedPreferences(this);

        setContentView(R.layout.activity_emulation);
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }

        mSystemUIVisible = true;
        mContentView = findViewById(R.id.fullscreen_content);
        mContentView.getHolder().addCallback(this);
        mContentView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                if (mSystemUIVisible)
                    hideSystemUI();
            }
        });

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
        hideSystemUI();
    }

    @Override
    protected void onStop() {
        super.onStop();

        if (AndroidHostInterface.getInstance().isEmulationThreadRunning()) {
            AndroidHostInterface.getInstance().stopEmulationThread();
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_emulation, menu);
        menu.findItem(R.id.show_controller).setChecked(mTouchscreenControllerVisible);
        menu.findItem(R.id.enable_speed_limiter).setChecked(getBooleanSetting("Main/SpeedLimiterEnabled", true));
        menu.findItem(R.id.show_controller).setChecked(getBooleanSetting("Controller1/EnableTouchscreenController", true));
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_settings) {
            Intent intent = new Intent(this, SettingsActivity.class);
            startActivity(intent);
            return true;
        } else if (id == R.id.show_controller) {
            setTouchscreenControllerVisibility(!mTouchscreenControllerVisible);
            item.setChecked(mTouchscreenControllerVisible);
            return true;
        } else if (id == R.id.enable_speed_limiter) {
            boolean newSetting = !getBooleanSetting("Main/SpeedLimiterEnabled", true);
            setBooleanSetting("Main/SpeedLimiterEnabled", newSetting);
            item.setChecked(newSetting);
            AndroidHostInterface.getInstance().applySettings();
            return true;
        } else if (id == R.id.reset) {
            AndroidHostInterface.getInstance().resetSystem();
        } else if (id == R.id.quick_load) {
            AndroidHostInterface.getInstance().loadState(false, 0);
        } else if (id == R.id.quick_save) {
            AndroidHostInterface.getInstance().saveState(false, 0);
        } else if (id == R.id.quit) {
            finish();
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    @Override
    public void onBackPressed() {
        if (mSystemUIVisible) {
            finish();
            return;
        }

        showSystemUI();
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

    /**
     * Some older devices needs a small delay between UI widget updates
     * and a change of the status and navigation bar.
     */
    private static final int UI_ANIMATION_DELAY = 300;
    private final Handler mSystemUIHideHandler = new Handler();
    private EmulationSurfaceView mContentView;
    private final Runnable mHidePart2Runnable = new Runnable() {
        @SuppressLint("InlinedApi")
        @Override
        public void run() {
            // Delayed removal of status and navigation bar

            // Note that some of these constants are new as of API 16 (Jelly Bean)
            // and API 19 (KitKat). It is safe to use them, as they are inlined
            // at compile-time and do nothing on earlier devices.
            mContentView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LOW_PROFILE
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);
        }
    };
    private final Runnable mShowPart2Runnable = new Runnable() {
        @Override
        public void run() {
            // Delayed display of UI elements
            ActionBar actionBar = getSupportActionBar();
            if (actionBar != null) {
                actionBar.show();
            }
        }
    };
    private boolean mSystemUIVisible;
    private final Runnable mHideRunnable = new Runnable() {
        @Override
        public void run() {
            hideSystemUI();
        }
    };

    private void hideSystemUI() {
        // Hide UI first
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.hide();
        }
        mSystemUIVisible = false;

        // Schedule a runnable to remove the status and navigation bar after a delay
        mSystemUIHideHandler.removeCallbacks(mShowPart2Runnable);
        mSystemUIHideHandler.postDelayed(mHidePart2Runnable, UI_ANIMATION_DELAY);
    }

    @SuppressLint("InlinedApi")
    private void showSystemUI() {
        // Show the system bar
        mContentView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        mSystemUIVisible = true;

        // Schedule a runnable to display UI elements after a delay
        mSystemUIHideHandler.removeCallbacks(mHidePart2Runnable);
        mSystemUIHideHandler.postDelayed(mShowPart2Runnable, UI_ANIMATION_DELAY);
    }

    /**
     * Schedules a call to hide() in delay milliseconds, canceling any
     * previously scheduled calls.
     */
    private void delayedHide(int delayMillis) {
        mSystemUIHideHandler.removeCallbacks(mHideRunnable);
        mSystemUIHideHandler.postDelayed(mHideRunnable, delayMillis);
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
