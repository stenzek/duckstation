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

    public static final float MIN_VIEW_SCALE = 0.25f;
    public static final float MAX_VIEW_SCALE = 10.0f;

    public enum EditMode {
        NONE,
        POSITION,
        SCALE
    }

    private int mControllerIndex;
    private String mControllerType;
    private String mViewType;
    private View mMainView;
    private ArrayList<TouchscreenControllerButtonView> mButtonViews = new ArrayList<>();
    private ArrayList<TouchscreenControllerAxisView> mAxisViews = new ArrayList<>();
    private TouchscreenControllerDPadView mDPadView = null;
    private int mPointerButtonCode = -1;
    private boolean mHapticFeedback;
    private String mLayoutOrientation;
    private EditMode mEditMode = EditMode.NONE;
    private View mMovingView = null;
    private String mMovingName = null;
    private float mMovingLastX = 0.0f;
    private float mMovingLastY = 0.0f;
    private float mMovingLastScale = 0.0f;
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

    private String getConfigKeyForScale(String name) {
        return String.format("TouchscreenController/%s/%s%sScale", mViewType, name, mLayoutOrientation);
    }

    private String getConfigKeyForVisibility(String name) {
        return String.format("TouchscreenController/%s/%s%sVisible", mViewType, name, mLayoutOrientation);
    }

    private void saveSettingsForButton(String name, View view) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        final SharedPreferences.Editor editor = prefs.edit();
        editor.putFloat(getConfigKeyForXTranslation(name), view.getTranslationX());
        editor.putFloat(getConfigKeyForYTranslation(name), view.getTranslationY());
        editor.putFloat(getConfigKeyForScale(name), view.getScaleX());
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
            editor.remove(getConfigKeyForScale(buttonView.getConfigName()));
            buttonView.setTranslationX(0.0f);
            buttonView.setTranslationY(0.0f);
            buttonView.setScaleX(1.0f);
            buttonView.setScaleY(1.0f);
        }

        for (TouchscreenControllerAxisView axisView : mAxisViews) {
            editor.remove(getConfigKeyForXTranslation(axisView.getConfigName()));
            editor.remove(getConfigKeyForYTranslation(axisView.getConfigName()));
            editor.remove(getConfigKeyForScale(axisView.getConfigName()));
            axisView.setTranslationX(0.0f);
            axisView.setTranslationY(0.0f);
            axisView.setScaleX(1.0f);
            axisView.setScaleY(1.0f);
        }

        if (mDPadView != null) {
            editor.remove(getConfigKeyForXTranslation(mDPadView.getConfigName()));
            editor.remove(getConfigKeyForYTranslation(mDPadView.getConfigName()));
            editor.remove(getConfigKeyForScale(mDPadView.getConfigName()));
            mDPadView.setTranslationX(0.0f);
            mDPadView.setTranslationY(0.0f);
            mDPadView.setScaleX(1.0f);
            mDPadView.setScaleY(1.0f);
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
                buttonView.setScaleX(prefs.getFloat(getConfigKeyForScale(buttonView.getConfigName()), 1.0f));
                buttonView.setScaleY(prefs.getFloat(getConfigKeyForScale(buttonView.getConfigName()), 1.0f));
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
                axisView.setScaleX(prefs.getFloat(getConfigKeyForScale(axisView.getConfigName()), 1.0f));
                axisView.setScaleY(prefs.getFloat(getConfigKeyForScale(axisView.getConfigName()), 1.0f));

                final boolean visible = prefs.getBoolean(getConfigKeyForVisibility(axisView.getConfigName()), axisView.getDefaultVisibility());
                axisView.setVisibility(visible ? VISIBLE : INVISIBLE);
            } catch (ClassCastException ex) {

            }
        }

        if (mDPadView != null) {
            try {
                mDPadView.setTranslationX(prefs.getFloat(getConfigKeyForXTranslation(mDPadView.getConfigName()), 0.0f));
                mDPadView.setTranslationY(prefs.getFloat(getConfigKeyForYTranslation(mDPadView.getConfigName()), 0.0f));
                mDPadView.setScaleX(prefs.getFloat(getConfigKeyForScale(mDPadView.getConfigName()), 1.0f));
                mDPadView.setScaleY(prefs.getFloat(getConfigKeyForScale(mDPadView.getConfigName()), 1.0f));

                final boolean visible = prefs.getBoolean(getConfigKeyForVisibility(mDPadView.getConfigName()), mDPadView.getDefaultVisibility());
                mDPadView.setVisibility(visible ? VISIBLE : INVISIBLE);
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
        if (mDPadView != null)
            mDPadView.setAlpha(alpha);
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

        if (mEditMode != EditMode.NONE)
            endLayoutEditing();

        mButtonViews.clear();
        mAxisViews.clear();
        removeAllViews();

        LayoutInflater inflater = LayoutInflater.from(getContext());
        String pointerButtonName = null;
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

            case "lightgun":
                mMainView = inflater.inflate(R.layout.layout_touchscreen_controller_lightgun, this, true);
                pointerButtonName = "Trigger";
                break;

            case "none":
            default:
                mMainView = null;
                break;
        }

        if (mMainView == null)
            return;

        mMainView.setOnTouchListener((view1, event) -> {
            if (mEditMode != EditMode.NONE)
                return handleEditingTouchEvent(event);
            else
                return handleTouchEvent(event);
        });

        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());

        linkDPadToButtons(mMainView, R.id.controller_dpad, "DPad", "", true);
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
        linkHotkeyButton(mMainView, R.id.controller_button_pause, "OpenPauseMenu",
                TouchscreenControllerButtonView.Hotkey.OPEN_PAUSE_MENU, true);

        linkButton(mMainView, R.id.controller_button_a, "AButton", "A", true, true);
        linkButton(mMainView, R.id.controller_button_b, "BButton", "B", true, true);
        if (pointerButtonName != null)
            linkPointer(pointerButtonName);

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

    private boolean linkDPadToButtons(View view, int id, String configName, String buttonPrefix, boolean defaultVisibility) {
        TouchscreenControllerDPadView dpadView = (TouchscreenControllerDPadView) view.findViewById(id);
        if (dpadView == null)
            return false;

        dpadView.setConfigName(configName);
        dpadView.setDefaultVisibility(defaultVisibility);
        mDPadView = dpadView;

        int leftCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Left");
        int rightCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Right");
        int upCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Up");
        int downCode = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonPrefix + "Down");
        Log.i("TouchscreenController", String.format("%s(DPad) -> %d,%d,%d,%d", buttonPrefix, leftCode, rightCode, upCode, downCode));
        if (leftCode < 0 && rightCode < 0 && upCode < 0 && downCode < 0)
            return false;

        dpadView.setControllerButtons(mControllerIndex, leftCode, rightCode, upCode, downCode);
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

    private boolean linkPointer(String buttonName) {
        mPointerButtonCode = AndroidHostInterface.getInstance().getControllerButtonCode(mControllerType, buttonName);
        Log.i("TouchscreenController", String.format("Pointer -> %s,%d", buttonName, mPointerButtonCode));
        return (mPointerButtonCode >= 0);
    }

    private int dpToPixels(float dp) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp, getResources().getDisplayMetrics()));
    }

    public void startLayoutEditing(EditMode mode) {
        if (mEditLayout == null) {
            LayoutInflater inflater = LayoutInflater.from(getContext());
            mEditLayout = (ConstraintLayout) inflater.inflate(R.layout.layout_touchscreen_controller_edit, this, false);
            ((Button) mEditLayout.findViewById(R.id.options)).setOnClickListener((view) -> showEditorMenu());
            addView(mEditLayout);
        }

        mEditMode = mode;
    }

    public void endLayoutEditing() {
        if (mEditLayout != null) {
            ((ViewGroup) mMainView).removeView(mEditLayout);
            mEditLayout = null;
        }

        mEditMode = EditMode.NONE;
        mMovingView = null;
        mMovingName = null;
        mMovingLastX = 0.0f;
        mMovingLastY = 0.0f;

        // unpause if we're paused (from the setting)
        if (AndroidHostInterface.getInstance().isEmulationThreadPaused())
            AndroidHostInterface.getInstance().pauseEmulationThread(false);
    }

    private float snapToValue(float pos, float value) {
        return Math.round(pos / value) * value;
    }

    private float snapToGrid(float pos) {
        final float value = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 20.0f, getResources().getDisplayMetrics());
        return snapToValue(pos, value);
    }

    private boolean handleEditingTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_UP: {
                if (mMovingView != null) {
                    // save position
                    saveSettingsForButton(mMovingName, mMovingView);
                    mMovingView = null;
                    mMovingName = null;
                    mMovingLastX = 0.0f;
                    mMovingLastY = 0.0f;
                    mMovingLastScale = 0.0f;
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
                        mMovingLastX = snapToGrid(x);
                        mMovingLastY = snapToGrid(y);
                        mMovingLastScale = buttonView.getScaleX();
                        return true;
                    }
                }

                for (TouchscreenControllerAxisView axisView : mAxisViews) {
                    axisView.getHitRect(rect);
                    if (rect.contains((int) x, (int) y)) {
                        mMovingView = axisView;
                        mMovingName = axisView.getConfigName();
                        mMovingLastX = snapToGrid(x);
                        mMovingLastY = snapToGrid(y);
                        mMovingLastScale = axisView.getScaleX();
                        return true;
                    }
                }

                if (mDPadView != null) {
                    mDPadView.getHitRect(rect);
                    if (rect.contains((int) x, (int) y)) {
                        mMovingView = mDPadView;
                        mMovingName = mDPadView.getConfigName();
                        mMovingLastX = snapToGrid(x);
                        mMovingLastY = snapToGrid(y);
                        mMovingLastScale = mDPadView.getScaleX();
                        return true;
                    }
                }

                // nothing..
                return true;
            }

            case MotionEvent.ACTION_MOVE: {
                if (mMovingView == null)
                    return true;

                final float x = snapToGrid(event.getX());
                final float y = snapToGrid(event.getY());
                if (mEditMode == EditMode.POSITION) {
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
                } else {
                    final float lastDx = mMovingLastX - mMovingView.getX();
                    final float lastDy = mMovingLastY - mMovingView.getY();
                    final float dx = x - mMovingView.getX();
                    final float dy = y - mMovingView.getY();
                    final float lastDistance = Math.max(Math.abs(lastDx), Math.abs(lastDy));
                    final float distance =  Math.max(Math.abs(dx), Math.abs(dy));
                    final float scaler = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 50.0f, getResources().getDisplayMetrics());
                    final float scaleDiff = snapToValue((distance - lastDistance) / scaler, 0.1f);
                    final float scale = Math.max(Math.min(mMovingLastScale + mMovingLastScale * scaleDiff, MAX_VIEW_SCALE), MIN_VIEW_SCALE);
                    mMovingView.setScaleX(scale);
                    mMovingView.setScaleY(scale);
                }

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
                    axisView.setPressed(pointerId, x - rect.left, y - rect.top);
                    pressed = true;
                    mGlidePairs.put(pointerId, null);
                    break;
                }
            }
            if (!pressed)
                axisView.setUnpressed();
        }

        if (mDPadView != null && mDPadView.getVisibility() == VISIBLE) {
            mDPadView.getHitRect(rect);

            boolean pressed = false;
            for (int i = 0; i < pointerCount; i++) {
                if (i == liftedPointerIndex)
                    continue;

                final int x = (int) event.getX(i);
                final int y = (int) event.getY(i);
                if (rect.contains(x, y)) {
                    mDPadView.setPressed(event.getPointerId(i), x - rect.left, y - rect.top);
                    pressed = true;
                }
            }

            if (!pressed)
                mDPadView.setUnpressed();
        }

        if (mPointerButtonCode >= 0 && pointerCount > 0) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                AndroidHostInterface.getInstance().setControllerButtonState(mControllerIndex,
                        mPointerButtonCode, true);
            }

            AndroidHostInterface.getInstance().setMousePosition(
                    (int)event.getX(0),
                    (int)event.getY(0));
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

                if (mDPadView != null)
                    mDPadView.setUnpressed();

                if (mPointerButtonCode >= 0) {
                    AndroidHostInterface.getInstance().setControllerButtonState(
                            mControllerIndex, mPointerButtonCode, false);
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

    private void showEditorMenu() {
        AlertDialog.Builder builder = new AlertDialog.Builder(getContext());
        builder.setItems(R.array.touchscreen_layout_menu, (dialogInterface, i) -> {
            switch (i) {
                case 0:     // Change Opacity
                {
                    AlertDialog.Builder subBuilder = createOpacityDialog(getContext());
                    subBuilder.create().show();
                }
                break;

                case 1:     // Add/Remove Buttons
                {
                    AlertDialog.Builder subBuilder = createAddRemoveButtonDialog(getContext());
                    subBuilder.create().show();
                }
                break;

                case 2:     // Edit Positions
                {
                    mEditMode = EditMode.POSITION;
                }
                break;

                case 3:     // Edit Scale
                {
                    mEditMode = EditMode.SCALE;
                }
                break;

                case 4:     // Reset Layout
                {
                    clearTranslationForAllButtons();
                }
                break;

                case 5:     // Exit Editor
                {
                    endLayoutEditing();
                }
                break;
            }
        });
        builder.create().show();
    }
}
