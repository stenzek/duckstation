package com.github.stenzek.duckstation;

import android.annotation.SuppressLint;

import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Menu;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.MenuItem;
import android.widget.FrameLayout;

import androidx.core.app.NavUtils;

/**
 * An example full-screen activity that shows and hides the system UI (i.e.
 * status bar and navigation/system bar) with user interaction.
 */
public class EmulationActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    /**
     * Interface to the native emulator core
     */
    AndroidHostInterface mHostInterface;

    /**
     * Touchscreen controller overlay
     */
    TouchscreenControllerView mTouchscreenController;
    private boolean mTouchscreenControllerVisible = true;

    /**
     * Whether or not the system UI should be auto-hidden after
     * {@link #AUTO_HIDE_DELAY_MILLIS} milliseconds.
     */
    private static final boolean AUTO_HIDE = true;

    /**
     * If {@link #AUTO_HIDE} is set, the number of milliseconds to wait after
     * user interaction before hiding the system UI.
     */
    private static final int AUTO_HIDE_DELAY_MILLIS = 3000;

    /**
     * Some older devices needs a small delay between UI widget updates
     * and a change of the status and navigation bar.
     */
    private static final int UI_ANIMATION_DELAY = 300;
    private final Handler mHideHandler = new Handler();
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
    private boolean mVisible;
    private final Runnable mHideRunnable = new Runnable() {
        @Override
        public void run() {
            hide();
        }
    };

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Once we get a surface, we can boot.
        if (mHostInterface.isEmulationThreadRunning()) {
            mHostInterface.surfaceChanged(holder.getSurface(), format, width, height);
            return;
        }

        String bootPath = getIntent().getStringExtra("bootPath");
        String bootSaveStatePath = getIntent().getStringExtra("bootSaveStatePath");

        if (!mHostInterface
                .startEmulationThread(holder.getSurface(), bootPath, bootSaveStatePath)) {
            Log.e("EmulationActivity", "Failed to start emulation thread");
            finishActivity(0);
            return;
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (!mHostInterface.isEmulationThreadRunning())
            return;

        Log.i("EmulationActivity", "Stopping emulation thread");
        mHostInterface.stopEmulationThread();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_emulation);
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }

        mVisible = true;
        mContentView = findViewById(R.id.fullscreen_content);
        mContentView.getHolder().addCallback(this);
        mContentView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                if (mVisible)
                    hide();
            }
        });

        mHostInterface = AndroidHostInterface.create(this);
        if (mHostInterface == null)
            throw new InstantiationError("Failed to create host interface");

        // Create touchscreen controller.
        FrameLayout activityLayout = findViewById(R.id.frameLayout);
        mTouchscreenController = new TouchscreenControllerView(this);
        activityLayout.addView(mTouchscreenController);
        mTouchscreenController.init(0, "DigitalController", mHostInterface);
        setTouchscreenControllerVisibility(true);

        // Hook up controller input.
        mContentView.initControllerKeyMapping(mHostInterface, "DigitalController");
    }

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);

        // Trigger the initial hide() shortly after the activity has been
        // created, to briefly hint to the user that UI controls
        // are available.
        delayedHide(100);
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_emulation, menu);
        menu.findItem(R.id.show_controller).setChecked(mTouchscreenControllerVisible);
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
        } else if (id == R.id.quit) {
            finish();
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    @Override
    public void onBackPressed() {
        if (mVisible) {
            finish();
            return;
        }

        show();
    }

    private void hide() {
        // Hide UI first
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.hide();
        }
        mVisible = false;

        // Schedule a runnable to remove the status and navigation bar after a delay
        mHideHandler.removeCallbacks(mShowPart2Runnable);
        mHideHandler.postDelayed(mHidePart2Runnable, UI_ANIMATION_DELAY);
    }

    @SuppressLint("InlinedApi")
    private void show() {
        // Show the system bar
        mContentView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        mVisible = true;

        // Schedule a runnable to display UI elements after a delay
        mHideHandler.removeCallbacks(mHidePart2Runnable);
        mHideHandler.postDelayed(mShowPart2Runnable, UI_ANIMATION_DELAY);
    }

    /**
     * Schedules a call to hide() in delay milliseconds, canceling any
     * previously scheduled calls.
     */
    private void delayedHide(int delayMillis) {
        mHideHandler.removeCallbacks(mHideRunnable);
        mHideHandler.postDelayed(mHideRunnable, delayMillis);
    }

    private void setTouchscreenControllerVisibility(boolean visible) {
        mTouchscreenControllerVisible = visible;
        mTouchscreenController.setVisibility(visible ? View.VISIBLE : View.INVISIBLE);
    }
}
