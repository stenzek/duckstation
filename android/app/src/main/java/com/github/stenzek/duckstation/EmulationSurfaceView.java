package com.github.stenzek.duckstation;

import android.content.Context;
import android.util.ArrayMap;
import android.util.AttributeSet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.SurfaceView;

public class EmulationSurfaceView extends SurfaceView {
    public EmulationSurfaceView(Context context) {
        super(context);
    }

    public EmulationSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public EmulationSurfaceView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    private boolean isDPadOrButtonEvent(KeyEvent event) {
        final int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ||
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (isDPadOrButtonEvent(event) && event.getRepeatCount() == 0 &&
                handleControllerKey(keyCode, true)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (isDPadOrButtonEvent(event) && event.getRepeatCount() == 0 &&
                handleControllerKey(keyCode, false)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    private ArrayMap<Integer, Integer> mControllerKeyMapping;

    private void addControllerKeyMapping(int keyCode, String controllerType, String buttonName) {
        int mapping = AndroidHostInterface.getControllerButtonCode(controllerType, buttonName);
        Log.i("EmulationSurfaceView", String.format("Map %d to %d (%s)", keyCode, mapping,
                buttonName));
        if (mapping >= 0)
            mControllerKeyMapping.put(keyCode, mapping);
    }

    public void initControllerKeyMapping(String controllerType) {
        mControllerKeyMapping = new ArrayMap<>();

        // TODO: Don't hardcode...
        addControllerKeyMapping(KeyEvent.KEYCODE_DPAD_UP, controllerType, "Up");
        addControllerKeyMapping(KeyEvent.KEYCODE_DPAD_RIGHT, controllerType, "Right");
        addControllerKeyMapping(KeyEvent.KEYCODE_DPAD_DOWN, controllerType, "Down");
        addControllerKeyMapping(KeyEvent.KEYCODE_DPAD_LEFT, controllerType, "Left");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_L1, controllerType, "L1");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_L2, controllerType, "L2");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_SELECT, controllerType, "Select");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_START, controllerType, "Start");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_Y, controllerType, "Triangle");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_B, controllerType, "Circle");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_A, controllerType, "Cross");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_X, controllerType, "Square");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_R1, controllerType, "R1");
        addControllerKeyMapping(KeyEvent.KEYCODE_BUTTON_R2, controllerType, "R2");
    }

    private boolean handleControllerKey(int keyCode, boolean pressed) {
        if (!mControllerKeyMapping.containsKey(keyCode))
            return false;

        final int mapping = mControllerKeyMapping.get(keyCode);
        AndroidHostInterface.getInstance().setControllerButtonState(0, mapping, pressed);
        return true;
    }
}
