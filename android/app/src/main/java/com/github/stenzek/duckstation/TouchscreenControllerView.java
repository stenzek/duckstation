package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;

import java.util.ArrayList;

/**
 * TODO: document your custom view class.
 */
public class TouchscreenControllerView extends FrameLayout {
    private int mControllerIndex;
    private String mControllerType;
    private View mMainView;
    private ArrayList<TouchscreenControllerButtonView> mButtonViews = new ArrayList<>();
    private ArrayList<TouchscreenControllerAxisView> mAxisViews = new ArrayList<>();
    private boolean mHapticFeedback;

    public TouchscreenControllerView(Context context) {
        super(context);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    public void init(int controllerIndex, String controllerType, String viewType, boolean hapticFeedback) {
        mControllerIndex = controllerIndex;
        mControllerType = controllerType;
        mHapticFeedback = hapticFeedback;

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
            return handleTouchEvent(event);
        });

        linkButton(mMainView, R.id.controller_button_up, "Up");
        linkButton(mMainView, R.id.controller_button_right, "Right");
        linkButton(mMainView, R.id.controller_button_down, "Down");
        linkButton(mMainView, R.id.controller_button_left, "Left");
        linkButton(mMainView, R.id.controller_button_l1, "L1");
        linkButton(mMainView, R.id.controller_button_l2, "L2");
        linkButton(mMainView, R.id.controller_button_select, "Select");
        linkButton(mMainView, R.id.controller_button_start, "Start");
        linkButton(mMainView, R.id.controller_button_triangle, "Triangle");
        linkButton(mMainView, R.id.controller_button_circle, "Circle");
        linkButton(mMainView, R.id.controller_button_cross, "Cross");
        linkButton(mMainView, R.id.controller_button_square, "Square");
        linkButton(mMainView, R.id.controller_button_r1, "R1");
        linkButton(mMainView, R.id.controller_button_r2, "R2");

        if (!linkAxis(mMainView, R.id.controller_axis_left, "Left"))
            linkAxisToButtons(mMainView, R.id.controller_axis_left, "");

        linkAxis(mMainView, R.id.controller_axis_right, "Right");
    }

    private void linkButton(View view, int id, String buttonName) {
        TouchscreenControllerButtonView buttonView = (TouchscreenControllerButtonView) view.findViewById(id);
        if (buttonView == null)
            return;

        int code = AndroidHostInterface.getControllerButtonCode(mControllerType, buttonName);
        Log.i("TouchscreenController", String.format("%s -> %d", buttonName, code));

        if (code >= 0) {
            buttonView.setButtonCode(mControllerIndex, code);
            buttonView.setHapticFeedback(mHapticFeedback);
            mButtonViews.add(buttonView);
        } else {
            Log.e("TouchscreenController", String.format("Unknown button name '%s' " +
                    "for '%s'", buttonName, mControllerType));
        }
    }

    private boolean linkAxis(View view, int id, String axisName) {
        TouchscreenControllerAxisView axisView = (TouchscreenControllerAxisView) view.findViewById(id);
        if (axisView == null)
            return false;

        int xCode = AndroidHostInterface.getControllerAxisCode(mControllerType, axisName + "X");
        int yCode = AndroidHostInterface.getControllerAxisCode(mControllerType, axisName + "Y");
        Log.i("TouchscreenController", String.format("%s -> %d/%d", axisName, xCode, yCode));
        if (xCode < 0 && yCode < 0)
            return false;

        axisView.setControllerAxis(mControllerIndex, xCode, yCode);
        mAxisViews.add(axisView);
        return true;
    }

    private boolean linkAxisToButtons(View view, int id, String buttonPrefix) {
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
        mAxisViews.add(axisView);
        return true;
    }

    private boolean handleTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_UP: {
                if (!AndroidHostInterface.hasInstanceAndEmulationThreadIsRunning())
                    return false;

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
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_MOVE: {
                if (!AndroidHostInterface.hasInstanceAndEmulationThreadIsRunning())
                    return false;

                Rect rect = new Rect();
                final int pointerCount = event.getPointerCount();
                final int liftedPointerIndex = (event.getActionMasked() == MotionEvent.ACTION_POINTER_UP) ? event.getActionIndex() : -1;
                for (TouchscreenControllerButtonView buttonView : mButtonViews) {
                    buttonView.getHitRect(rect);
                    boolean pressed = false;
                    for (int i = 0; i < pointerCount; i++) {
                        if (i == liftedPointerIndex)
                            continue;

                        final int x = (int) event.getX(i);
                        final int y = (int) event.getY(i);
                        if (rect.contains(x, y)) {
                            buttonView.setPressed(true);
                            pressed = true;
                            break;
                        }
                    }

                    if (!pressed)
                        buttonView.setPressed(pressed);
                }

                for (TouchscreenControllerAxisView axisView : mAxisViews) {
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
                            break;
                        }
                    }
                    if (!pressed)
                        axisView.setUnpressed();
                }

                return true;
            }
        }

        return false;
    }
}
