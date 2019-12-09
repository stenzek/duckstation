package com.github.stenzek.duckstation;

import android.content.Context;
import android.util.AttributeSet;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.FrameLayout;

/**
 * TODO: document your custom view class.
 */
public class TouchscreenControllerView extends FrameLayout implements TouchscreenControllerButtonView.ButtonStateChangedListener {
    private int mControllerIndex;
    private String mControllerType;
    private AndroidHostInterface mHostInterface;

    public TouchscreenControllerView(Context context) {
        super(context);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public TouchscreenControllerView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    public void init(int controllerIndex, String controllerType,
                     AndroidHostInterface hostInterface) {
        mControllerIndex = controllerIndex;
        mControllerType = controllerType;
        mHostInterface = hostInterface;

        if (mHostInterface != null)
            mHostInterface.setControllerType(controllerIndex, controllerType);

        LayoutInflater inflater = LayoutInflater.from(getContext());
        View view = inflater.inflate(R.layout.layout_touchscreen_controller, this, true);

        // TODO: Make dynamic, editable.
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

    private void linkButton(View view, int id, String buttonName)
    {
        TouchscreenControllerButtonView buttonView = (TouchscreenControllerButtonView)view.findViewById(id);
        buttonView.setButtonName(buttonName);
        buttonView.setButtonStateChangedListener(this);

        if (mHostInterface != null)
        {
            int code = mHostInterface.getControllerButtonCode(mControllerType, buttonName);
            buttonView.setButtonCode(code);
            Log.i("TouchscreenController", String.format("%s -> %d", buttonName, code));

            if (code < 0) {
                Log.e("TouchscreenController", String.format("Unknown button name '%s' " +
                        "for '%s'", buttonName, mControllerType));
            }
        }
    }


    @Override
    public void onButtonStateChanged(TouchscreenControllerButtonView view, boolean pressed) {
        if (mHostInterface == null || view.getButtonCode() < 0)
            return;

        mHostInterface.setControllerButtonState(mControllerIndex, view.getButtonCode(), pressed);
    }
}
