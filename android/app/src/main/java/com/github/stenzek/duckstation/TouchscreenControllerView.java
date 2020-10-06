package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Rect;
import android.text.method.Touch;
import android.util.AttributeSet;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;

import java.util.ArrayList;
import java.util.HashMap;

/**
 * TODO: document your custom view class.
 */
public class TouchscreenControllerView extends FrameLayout {
    private int mControllerIndex;
    private String mControllerType;
    private ArrayList<TouchscreenControllerButtonView> mButtonViews = new ArrayList<>();

    public TouchscreenControllerView(Context context) {
        super(context);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    public void init(int controllerIndex, String controllerType) {
        mControllerIndex = controllerIndex;
        mControllerType = controllerType;

        LayoutInflater inflater = LayoutInflater.from(getContext());
        View view = inflater.inflate(R.layout.layout_touchscreen_controller, this, true);
        view.setOnTouchListener((view1, event) -> {
            return handleTouchEvent(event);
        });

        // TODO: Make dynamic, editable.
        mButtonViews.clear();
        linkButton(view, R.id.controller_button_up, "Up");
        linkButton(view, R.id.controller_button_right, "Right");
        linkButton(view, R.id.controller_button_down, "Down");
        linkButton(view, R.id.controller_button_left, "Left");
        linkButton(view, R.id.controller_button_l1, "L1");
        linkButton(view, R.id.controller_button_l2, "L2");
        linkButton(view, R.id.controller_button_select, "Select");
        linkButton(view, R.id.controller_button_start, "Start");
        linkButton(view, R.id.controller_button_triangle, "Triangle");
        linkButton(view, R.id.controller_button_circle, "Circle");
        linkButton(view, R.id.controller_button_cross, "Cross");
        linkButton(view, R.id.controller_button_square, "Square");
        linkButton(view, R.id.controller_button_r1, "R1");
        linkButton(view, R.id.controller_button_r2, "R2");
    }

    private void linkButton(View view, int id, String buttonName) {
        TouchscreenControllerButtonView buttonView = (TouchscreenControllerButtonView) view.findViewById(id);
        buttonView.setButtonName(buttonName);

        int code = AndroidHostInterface.getInstance().getControllerButtonCode(mControllerType, buttonName);
        buttonView.setButtonCode(code);
        Log.i("TouchscreenController", String.format("%s -> %d", buttonName, code));

        if (code >= 0) {
            mButtonViews.add(buttonView);
        } else {
            Log.e("TouchscreenController", String.format("Unknown button name '%s' " +
                    "for '%s'", buttonName, mControllerType));
        }
    }

    private boolean handleTouchEvent(MotionEvent event) {
        switch (event.getActionMasked())
        {
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            {
                clearAllButtonPressedStates();
                return true;
            }

            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_MOVE:
            {
                final int x = (int)event.getX();
                final int y = (int)event.getY();
                Rect rect = new Rect();
                for (TouchscreenControllerButtonView buttonView : mButtonViews)
                {
                    buttonView.getHitRect(rect);
                    final boolean pressed = rect.contains(x, y);
                    if (buttonView.isPressed() == pressed)
                        continue;

                    buttonView.setPressed(pressed);
                    AndroidHostInterface.getInstance().setControllerButtonState(mControllerIndex, buttonView.getButtonCode(), pressed);
                }

                return true;
            }
        }

        return false;
    }

    private void clearAllButtonPressedStates() {
        for (TouchscreenControllerButtonView buttonView : mButtonViews) {
            if (!buttonView.isPressed())
                continue;

            AndroidHostInterface.getInstance().setControllerButtonState(mControllerIndex, buttonView.getButtonCode(), false);
            buttonView.setPressed(false);
        }
    }
}
