package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.util.Log;
import android.util.TypedValue;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.SeekBar;

import androidx.appcompat.app.AlertDialog;
import androidx.constraintlayout.widget.ConstraintLayout;
import androidx.preference.PreferenceManager;

import java.util.ArrayList;
import java.util.Map;
import java.util.HashMap;

/**
 * TODO: document your custom view class.
 */
public class TouchscreenControllerView extends FrameLayout {
    public static final int DEFAULT_OPACITY = 100;

    private int mControllerIndex;
    private String mControllerType;
    private String mViewType;
    private View mMainView;
    private ArrayList<TouchscreenControllerButtonView> mButtonViews = new ArrayList<>();
    private ArrayList<TouchscreenControllerAxisView> mAxisViews = new ArrayList<>();
    private boolean mHapticFeedback;
    private String mLayoutOrientation;
    private boolean mEditingLayout = false;
    private View mMovingView = null;
    private String mMovingName = null;
    private float mMovingLastX = 0.0f;
    private float mMovingLastY = 0.0f;
    private ConstraintLayout mEditLayout = null;
    private int mOpacity = 100;
    private Map<Integer, View> mGlidePairs = new HashMap<>();

    public TouchscreenControllerView(Context context) {
        super(context);
        setFocusable(false);
        setFocusableInTouchMode(false);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    private String getConfigKeyForXTranslation(String name) {
        return String.format("TouchscreenController/%s/%s%sXTranslation", mViewType, name, mLayoutOrientation);
    }

    private String getConfigKeyForYTranslation(String name) {
        return String.format("TouchscreenController/%s/%s%sYTranslation", mViewType, name, mLayoutOrientation);
    }

    private String getConfigKeyForVisibility(String name) {
        return String.format("TouchscreenController/%s/%s%sVisible", mViewType, name, mLayoutOrientation);
    }

    private void saveTranslationForButton(String name, float xTranslation, float yTranslation) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        final SharedPreferences.Editor editor = prefs.edit();
        editor.putFloat(getConfigKeyForXTranslation(name), xTranslation);
        editor.putFloat(getConfigKeyForYTranslation(name), yTranslation);
        editor.commit();
    }

    private void saveVisibilityForButton(String name, boolean visible) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        final SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean(getConfigKeyForVisibility(name), visible);
        editor.commit();
    }

    private void clearTranslationForAllButtons() {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        final SharedPreferences.Editor editor = prefs.edit();

        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            editor.remove(getConfigKeyForXTranslation(buttonView.getConfigName()));
            editor.remove(getConfigKeyForYTranslation(buttonView.getConfigName()));
            buttonView.setTranslationX(0.0f);
            buttonView.setTranslationY(0.0f);
        }

        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            editor.remove(getConfigKeyForXTranslation(axisView.getConfigName()));
            editor.remove(getConfigKeyForYTranslation(axisView.getConfigName()));
            axisView.setTranslationX(0.0f);
            axisView.setTranslationY(0.0f);
        }

        editor.commit();
        requestLayout();
    }

    private void reloadButtonSettings() {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());

        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            try {
                buttonView.setTranslationX(prefs.getFloat(getConfigKeyForXTranslation(buttonView.getConfigName()), 0.0f));
                buttonView.setTranslationY(prefs.getFloat(getConfigKeyForYTranslation(buttonView.getConfigName()), 0.0f));
                //Log.i("TouchscreenController", String.format("Translation for %s %f %f", buttonView.getConfigName(),
                //        buttonView.getTranslationX(), buttonView.getTranslationY()));

                final boolean visible = prefs.getBoolean(getConfigKeyForVisibility(buttonView.getConfigName()), buttonView.getDefaultVisibility());
                buttonView.setVisibility(visible ? VISIBLE : INVISIBLE);
            } catch (ClassCastException ex) {

            }
        }

        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            try {
                axisView.setTranslationX(prefs.getFloat(getConfigKeyForXTranslation(axisView.getConfigName()), 0.0f));
                axisView.setTranslationY(prefs.getFloat(getConfigKeyForYTranslation(axisView.getConfigName()), 0.0f));

                final boolean visible = prefs.getBoolean(getConfigKeyForVisibility(axisView.getConfigName()), axisView.getDefaultVisibility());
                axisView.setVisibility(visible ? VISIBLE : INVISIBLE);
            } catch (ClassCastException ex) {

            }
        }
    }

    private void setOpacity(int opacity) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        final SharedPreferences.Editor editor = prefs.edit();
        editor.putInt("TouchscreenController/Opacity", opacity);
        editor.commit();

        updateOpacity();
    }

    private void updateOpacity() {
        mOpacity = PreferenceManager.getDefaultSharedPreferences(getContext()).getInt("TouchscreenController/Opacity", DEFAULT_OPACITY);

        float alpha = (float)mOpacity / 100.0f;
        alpha = (alpha < 0.0f) ? 0.0f : ((alpha > 1.0f) ? 1.0f : alpha);

        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            buttonView.setAlpha(alpha);
        }
        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            axisView.setAlpha(alpha);
        }
    }

    private String getOrientationString() {
        switch (getContext().getResources().getConfiguration().orientation) {
            case Configuration.ORIENTATION_PORTRAIT:
                return "Portrait";
            case Configuration.ORIENTATION_LANDSCAPE:
            default:
                return "Landscape";
        }
    }

    /**
     * Checks if the orientation of the layout has changed, and if so, reloads button translations.
     */
    public void updateOrientation() {
        String newOrientation = getOrientationString();
        if (mLayoutOrientation != null && mLayoutOrientation.equals(newOrientation))
            return;

        Log.i("TouchscreenController", "New orientation: " + newOrientation);
        mLayoutOrientation = newOrientation;
        reloadButtonSettings();
        requestLayout();
    }

    public void init(int controllerIndex, String controllerType, String viewType, boolean hapticFeedback, boolean gliding) {
        mControllerIndex = controllerIndex;
        mControllerType = controllerType;
        mViewType = viewType;
        mHapticFeedback = hapticFeedback;
        mLayoutOrientation = getOrientationString();

        if (mEditingLayout)
            endLayoutEditing();

        mButtonViews.clear();
        mAxisViews.clear();
        removeAllViews();

        LayoutInflater inflater = LayoutInflater.from(getContext());
        switch (viewType) {
            case "digital":
                mMainView = inflater.inflate(R.layout.layout_touchscreen_controller_digital, this, true);
                break;

            case "analog_stick":
                mMainView = inflater.inflate(R.layout.layout_touchscreen_controller_analog_stick, this, true);
                break;

            case "analog_sticks":
                mMainView = inflater.inflate(R.layout.layout_touchscreen_controller_analog_sticks, this, true);
                break;

            case "none":
            default:
                mMainView = null;
                break;
        }

        if (mMainView == null)
            return;

        mMainView.setOnTouchListener((view1, event) -> {
            if (mEditingLayout)
                return handleEditingTouchEvent(event);
            else
                return handleTouchEvent(event);
        });

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());

        linkButton(mMainView, R.id.controller_button_up, "UpButton", "Up", true, false);
        linkButton(mMainView, R.id.controller_button_right, "RightButton", "Right", true, false);
        linkButton(mMainView, R.id.controller_button_down, "DownButton", "Down", true, false);
        linkButton(mMainView, R.id.controller_button_left, "LeftButton", "Left", true, false);
        linkButton(mMainView, R.id.controller_button_l1, "L1Button", "L1", true, gliding);
        linkButton(mMainView, R.id.controller_button_l2, "L2Button", "L2", true, gliding);
        linkButton(mMainView, R.id.controller_button_select, "SelectButton", "Select", true, gliding);
        linkButton(mMainView, R.id.controller_button_start, "StartButton", "Start", true, gliding);
        linkButton(mMainView, R.id.controller_button_triangle, "TriangleButton", "Triangle", true, gliding);
        linkButton(mMainView, R.id.controller_button_circle, "CircleButton", "Circle", true, gliding);
        linkButton(mMainView, R.id.controller_button_cross, "CrossButton", "Cross", true, gliding);
        linkButton(mMainView, R.id.controller_button_square, "SquareButton", "Square", true, gliding);
        linkButton(mMainView, R.id.controller_button_r1, "R1Button", "R1", true, gliding);
        linkButton(mMainView, R.id.controller_button_r2, "R2Button", "R2", true, gliding);

        if (!linkAxis(mMainView, R.id.controller_axis_left, "LeftAxis", "Left", true))
            linkAxisToButtons(mMainView, R.id.controller_axis_left, "LeftAxis", "");

        linkAxis(mMainView, R.id.controller_axis_right, "RightAxis", "Right", true);

        linkHotkeyButton(mMainView, R.id.controller_button_fast_forward, "FastForward",
                TouchscreenControllerButtonView.Hotkey.FAST_FORWARD, false);
        linkHotkeyButton(mMainView, R.id.controller_button_analog, "AnalogToggle",
                TouchscreenControllerButtonView.Hotkey.ANALOG_TOGGLE, false);

        reloadButtonSettings();
        updateOpacity();
        requestLayout();
    }

    private void linkButton(View view, int id, String configName, String buttonName, boolean defaultVisibility, boolean isGlidable) {
        TouchscreenControllerButtonView buttonView = (TouchscreenControllerButtonView) view.findViewById(id);
        if (buttonView == null)
            return;

        buttonView.setConfigName(configName);
        buttonView.setDefaultVisibility(defaultVisibility);
        buttonView.setIsGlidable(isGlidable);
        mButtonViews.add(buttonView);

        int code = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonName);
        Log.i("TouchscreenController", String.format("%s -> %d", buttonName, code));

        if (code >= 0) {
            buttonView.setButtonCode(mControllerIndex, code);
            buttonView.setHapticFeedback(mHapticFeedback);
        } else {
            Log.e("TouchscreenController", String.format("Unknown button name '%s' " +
                    "for '%s'", buttonName, mControllerType));
        }
    }

    private boolean linkAxis(View view, int id, String configName, String axisName, boolean defaultVisibility) {
        TouchscreenControllerAxisView axisView = (TouchscreenControllerAxisView) view.findViewById(id);
        if (axisView == null)
            return false;

        axisView.setConfigName(configName);
        axisView.setDefaultVisibility(defaultVisibility);
        mAxisViews.add(axisView);

        int xCode = AndroidHostInterface.getControllerAxisCode(mControllerType, axisName + "X");
        int yCode = AndroidHostInterface.getControllerAxisCode(mControllerType, axisName + "Y");
        Log.i("TouchscreenController", String.format("%s -> %d/%d", axisName, xCode, yCode));
        if (xCode < 0 && yCode < 0)
            return false;

        axisView.setControllerAxis(mControllerIndex, xCode, yCode);
        return true;
    }

    private boolean linkAxisToButtons(View view, int id, String configName, String buttonPrefix) {
        TouchscreenControllerAxisView axisView = (TouchscreenControllerAxisView) view.findViewById(id);
        if (axisView == null)
            return false;

        int leftCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Left");
        int rightCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Right");
        int upCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Up");
        int downCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Down");
        Log.i("TouchscreenController", String.format("%s(ButtonAxis) -> %d,%d,%d,%d", buttonPrefix, leftCode, rightCode, upCode, downCode));
        if (leftCode < 0 && rightCode < 0 && upCode < 0 && downCode < 0)
            return false;

        axisView.setControllerButtons(mControllerIndex, leftCode, rightCode, upCode, downCode);
        return true;
    }

    private void linkHotkeyButton(View view, int id, String configName, TouchscreenControllerButtonView.Hotkey hotkey, boolean defaultVisibility) {
        TouchscreenControllerButtonView buttonView = (TouchscreenControllerButtonView) view.findViewById(id);
        if (buttonView == null)
            return;

        buttonView.setConfigName(configName);
        buttonView.setDefaultVisibility(defaultVisibility);
        buttonView.setHotkey(hotkey);
        mButtonViews.add(buttonView);
    }

    private int dpToPixels(float dp) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp, getResources().getDisplayMetrics()));
    }

    public void startLayoutEditing() {
        if (mEditLayout == null) {
            LayoutInflater inflater = LayoutInflater.from(getContext());
            mEditLayout = (ConstraintLayout) inflater.inflate(R.layout.layout_touchscreen_controller_edit, this, false);
            ((Button) mEditLayout.findViewById(R.id.stop_editing)).setOnClickListener((view) -> endLayoutEditing());
            ((Button) mEditLayout.findViewById(R.id.reset_layout)).setOnClickListener((view) -> clearTranslationForAllButtons());
            addView(mEditLayout);
        }

        mEditingLayout = true;
    }

    public void endLayoutEditing() {
        if (mEditLayout != null) {
            ((ViewGroup) mMainView).removeView(mEditLayout);
            mEditLayout = null;
        }

        mEditingLayout = false;
        mMovingView = null;
        mMovingName = null;
        mMovingLastX = 0.0f;
        mMovingLastY = 0.0f;

        // unpause if we're paused (from the setting)
        if (AndroidHostInterface.getInstance().isEmulationThreadPaused())
            AndroidHostInterface.getInstance().pauseEmulationThread(false);
    }

    private boolean handleEditingTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_UP: {
                if (mMovingView != null) {
                    // save position
                    saveTranslationForButton(mMovingName, mMovingView.getTranslationX(), mMovingView.getTranslationY());
                    mMovingView = null;
                    mMovingName = null;
                    mMovingLastX = 0.0f;
                    mMovingLastY = 0.0f;
                }

                return true;
            }

            case MotionEvent.ACTION_DOWN: {
                if (mMovingView != null) {
                    // already moving a button
                    return true;
                }

                Rect rect = new Rect();
                final float x = event.getX();
                final float y = event.getY();
                for (TouchscreenControllerButtonView buttonView : mButtonViews) {
                    buttonView.getHitRect(rect);
                    if (rect.contains((int) x, (int) y)) {
                        mMovingView = buttonView;
                        mMovingName = buttonView.getConfigName();
                        mMovingLastX = x;
                        mMovingLastY = y;
                        return true;
                    }
                }

                for (TouchscreenControllerAxisView axisView : mAxisViews) {
                    axisView.getHitRect(rect);
                    if (rect.contains((int) x, (int) y)) {
                        mMovingView = axisView;
                        mMovingName = axisView.getConfigName();
                        mMovingLastX = x;
                        mMovingLastY = y;
                        return true;
                    }
                }

                // nothing..
                return true;
            }

            case MotionEvent.ACTION_MOVE: {
                if (mMovingView == null)
                    return true;

                final float x = event.getX();
                final float y = event.getY();
                final float dx = x - mMovingLastX;
                final float dy = y - mMovingLastY;
                mMovingLastX = x;
                mMovingLastY = y;

                final float posX = mMovingView.getX() + dx;
                final float posY = mMovingView.getY() + dy;
                //Log.d("Position", String.format("%f %f -> (%f %f) %f %f",
                //        mMovingView.getX(), mMovingView.getY(), dx, dy, posX, posY));
                mMovingView.setX(posX);
                mMovingView.setY(posY);
                mMovingView.invalidate();
                mMainView.requestLayout();
                return true;
            }
        }

        return false;
    }

    private boolean updateTouchButtonsFromEvent(MotionEvent event) {
        if (!AndroidHostInterface.hasInstanceAndEmulationThreadIsRunning())
            return false;

        Rect rect = new Rect();
        final int pointerCount = event.getPointerCount();
        final int liftedPointerIndex = (event.getActionMasked() == MotionEvent.ACTION_POINTER_UP) ? event.getActionIndex() : -1;
        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            if (buttonView.getVisibility() != VISIBLE)
                continue;

            buttonView.getHitRect(rect);
            boolean pressed = false;
            for (int i = 0; i < pointerCount; i++) {
                if (i == liftedPointerIndex)
                    continue;

                final int x = (int) event.getX(i);
                final int y = (int) event.getY(i);
                if (rect.contains(x, y)) {
                    buttonView.setPressed(true);
                    final int pointerId = event.getPointerId(i);
                    if (!mGlidePairs.containsKey(pointerId) && !mGlidePairs.containsValue(buttonView)) {
                        if (buttonView.getIsGlidable())
                            mGlidePairs.put(pointerId, buttonView);
                        else { mGlidePairs.put(pointerId, null); }
                    }
                    pressed = true;
                    break;
                }
            }

            if (!pressed  && !mGlidePairs.containsValue(buttonView))
                buttonView.setPressed(pressed);
        }

        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            if (axisView.getVisibility() != VISIBLE)
                continue;

            axisView.getHitRect(rect);
            boolean pressed = false;
            for (int i = 0; i < pointerCount; i++) {
                if (i == liftedPointerIndex)
                    continue;

                final int pointerId = event.getPointerId(i);
                final int x = (int) event.getX(i);
                final int y = (int) event.getY(i);

                if ((rect.contains(x, y) && !axisView.isPressed()) ||
                        (axisView.isPressed() && axisView.getPointerId() == pointerId)) {
                    axisView.setPressed(pointerId, x, y);
                    pressed = true;
                    mGlidePairs.put(pointerId, null);
                    break;
                }
            }
            if (!pressed)
                axisView.setUnpressed();
        }

        return true;
    }

    private boolean handleTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_UP: {
                if (!AndroidHostInterface.hasInstanceAndEmulationThreadIsRunning())
                    return false;

                mGlidePairs.clear();

                for (TouchscreenControllerButtonView buttonView : mButtonViews) {
                    buttonView.setPressed(false);
                }

                for (TouchscreenControllerAxisView axisView : mAxisViews) {
                    axisView.setUnpressed();
                }

                return true;
            }

            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_POINTER_UP: {
                final int pointerId = event.getPointerId(event.getActionIndex());
                if (mGlidePairs.containsKey(pointerId))
                    mGlidePairs.remove(pointerId);

                return updateTouchButtonsFromEvent(event);
            }
            case MotionEvent.ACTION_MOVE: {
                return updateTouchButtonsFromEvent(event);
            }
        }

        return false;
    }

    public AlertDialog.Builder createAddRemoveButtonDialog(Context context) {
        final AlertDialog.Builder builder = new AlertDialog.Builder(context);
        final CharSequence[] items = new CharSequence[mButtonViews.size() + mAxisViews.size()];
        final boolean[] itemsChecked = new boolean[mButtonViews.size() + mAxisViews.size()];
        int itemCount = 0;
        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            items[itemCount] = buttonView.getConfigName();
            itemsChecked[itemCount] = buttonView.getVisibility() == VISIBLE;
            itemCount++;
        }
        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            items[itemCount] = axisView.getConfigName();
            itemsChecked[itemCount] = axisView.getVisibility() == VISIBLE;
            itemCount++;
        }

        builder.setTitle(R.string.dialog_touchscreen_controller_buttons);
        builder.setMultiChoiceItems(items, itemsChecked, (dialog, which, isChecked) -> {
            if (which < mButtonViews.size()) {
                TouchscreenControllerButtonView buttonView = mButtonViews.get(which);
                buttonView.setVisibility(isChecked ? VISIBLE : INVISIBLE);
                saveVisibilityForButton(buttonView.getConfigName(), isChecked);
            } else {
                TouchscreenControllerAxisView axisView = mAxisViews.get(which - mButtonViews.size());
                axisView.setVisibility(isChecked ? VISIBLE : INVISIBLE);
                saveVisibilityForButton(axisView.getConfigName(), isChecked);
            }
        });
        builder.setNegativeButton(R.string.dialog_done, (dialog, which) -> {
            dialog.dismiss();
        });

        return builder;
    }

    public AlertDialog.Builder createOpacityDialog(Context context) {
        final SeekBar seekBar = new SeekBar(context);
        seekBar.setMax(100);
        seekBar.setProgress(mOpacity);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                setOpacity(progress);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });

        final AlertDialog.Builder builder = new AlertDialog.Builder(context);
        builder.setTitle(R.string.dialog_touchscreen_controller_opacity);
        builder.setView(seekBar);
        builder.setNegativeButton(R.string.dialog_done, (dialog, which) -> {
            dialog.dismiss();
        });
        return builder;
    }
}
