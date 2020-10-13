package com.github.stenzek.duckstation;

import android.content.Context;
import android.util.ArrayMap;
import android.util.AttributeSet;
import android.util.Log;
import android.util.Pair;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
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

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        final int source = event.getSource();
        if ((source & InputDevice.SOURCE_JOYSTICK) == 0)
            return super.onGenericMotionEvent(event);

        final InputDevice device = event.getDevice();
        for (int axis : AXISES) {
            Integer mapping = mControllerAxisMapping.containsKey(axis) ? mControllerAxisMapping.get(axis) : null;
            Pair<Integer, Integer> buttonMapping = mControllerAxisButtonMapping.containsKey(axis) ? mControllerAxisButtonMapping.get(axis) : null;
            if (mapping == null && buttonMapping == null)
                continue;

            final float axisValue = event.getAxisValue(axis);
            float emuValue;

            final InputDevice.MotionRange range = device.getMotionRange(axis, source);
            if (range != null) {
                final float transformedValue = (axisValue - range.getMin()) / range.getRange();
                emuValue = (transformedValue * 2.0f) - 1.0f;
            } else {
                emuValue = axisValue;
            }
            Log.d("EmulationSurfaceView", String.format("axis %d value %f emuvalue %f", axis, axisValue, emuValue));
            if (mapping != null) {
                AndroidHostInterface.getInstance().setControllerAxisState(0, mapping, emuValue);
            } else {
                final float DEAD_ZONE = 0.25f;
                AndroidHostInterface.getInstance().setControllerButtonState(0, buttonMapping.first, (emuValue <= -DEAD_ZONE));
                AndroidHostInterface.getInstance().setControllerButtonState(0, buttonMapping.second, (emuValue >= DEAD_ZONE));
                Log.d("EmulationSurfaceView", String.format("using emuValue %f for buttons %d %d", emuValue, buttonMapping.first, buttonMapping.second));
            }
        }

        return true;
    }

    private ArrayMap<Integer, Integer> mControllerKeyMapping;
    private ArrayMap<Integer, Integer> mControllerAxisMapping;
    private ArrayMap<Integer, Pair<Integer, Integer>> mControllerAxisButtonMapping;
    static final int[] AXISES = new int[]{MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z,
            MotionEvent.AXIS_RZ, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
            MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y};

    private void addControllerKeyMapping(int keyCode, String controllerType, String buttonName) {
        int mapping = AndroidHostInterface.getControllerButtonCode(controllerType, buttonName);
        Log.i("EmulationSurfaceView", String.format("Map %d to %d (%s)", keyCode, mapping,
                buttonName));
        if (mapping >= 0)
            mControllerKeyMapping.put(keyCode, mapping);
    }

    private void addControllerAxisMapping(int axis, String controllerType, String axisName, String negativeButtonName, String positiveButtonName) {
        if (axisName != null) {
            int mapping = AndroidHostInterface.getControllerAxisCode(controllerType, axisName);
            Log.i("EmulationSurfaceView", String.format("Map axis %d to %d (%s)", axis, mapping, axisName));
            if (mapping >= 0) {
                mControllerAxisMapping.put(axis, mapping);
                return;
            }
        }

        if (negativeButtonName != null && positiveButtonName != null) {
            final int negativeMapping = AndroidHostInterface.getControllerButtonCode(controllerType, negativeButtonName);
            final int positiveMapping = AndroidHostInterface.getControllerButtonCode(controllerType, positiveButtonName);
            Log.i("EmulationSurfaceView", String.format("Map axis %d to %d %d (button %s %s)", axis, negativeMapping, positiveMapping,
                    negativeButtonName, positiveButtonName));
            if (negativeMapping >= 0 && positiveMapping >= 0) {
                mControllerAxisButtonMapping.put(axis, new Pair<Integer, Integer>(negativeMapping, positiveMapping));
            }
        }
    }

    public void initControllerKeyMapping(String controllerType) {
        mControllerKeyMapping = new ArrayMap<>();
        mControllerAxisMapping = new ArrayMap<>();
        mControllerAxisButtonMapping = new ArrayMap<>();

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
        addControllerAxisMapping(MotionEvent.AXIS_X, controllerType, "LeftX", null, null);
        addControllerAxisMapping(MotionEvent.AXIS_Y, controllerType, "LeftY", null, null);
        addControllerAxisMapping(MotionEvent.AXIS_Z, controllerType, "RightX", null, null);
        addControllerAxisMapping(MotionEvent.AXIS_RZ, controllerType, "RightY", null, null);
        addControllerAxisMapping(MotionEvent.AXIS_LTRIGGER, controllerType, "L2", "L2", "L2");
        addControllerAxisMapping(MotionEvent.AXIS_RTRIGGER, controllerType, "R2", "R2", "R2");
        addControllerAxisMapping(MotionEvent.AXIS_HAT_X, controllerType, null, "Left", "Right");
        addControllerAxisMapping(MotionEvent.AXIS_HAT_Y, controllerType, null, "Up", "Down");
    }

    private boolean handleControllerKey(int keyCode, boolean pressed) {
        if (!mControllerKeyMapping.containsKey(keyCode))
            return false;

        final int mapping = mControllerKeyMapping.get(keyCode);
        AndroidHostInterface.getInstance().setControllerButtonState(0, mapping, pressed);
        Log.d("EmulationSurfaceView", String.format("handleControllerKey %d -> %d %d", keyCode, mapping, pressed ? 1 : 0));
        return true;
    }
}
