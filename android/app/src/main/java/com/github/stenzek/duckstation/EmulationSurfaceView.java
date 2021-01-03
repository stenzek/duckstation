package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AttributeSet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;

import androidx.core.util.Pair;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

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

    public static boolean isDPadOrButtonEvent(KeyEvent event) {
        final int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ||
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private boolean isExternalKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_HOME:
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_VOLUME_UP:
            case KeyEvent.KEYCODE_VOLUME_DOWN:
            case KeyEvent.KEYCODE_VOLUME_MUTE:
            case KeyEvent.KEYCODE_POWER:
            case KeyEvent.KEYCODE_CAMERA:
            case KeyEvent.KEYCODE_CALL:
            case KeyEvent.KEYCODE_ENDCALL:
            case KeyEvent.KEYCODE_VOICE_ASSIST:
                return true;

            default:
                return false;
        }
    }

    private static final int[] buttonKeyCodes = new int[]{
            KeyEvent.KEYCODE_BUTTON_A,      // 0/Cross
            KeyEvent.KEYCODE_BUTTON_B,      // 1/Circle
            KeyEvent.KEYCODE_BUTTON_X,      // 2/Square
            KeyEvent.KEYCODE_BUTTON_Y,      // 3/Triangle
            KeyEvent.KEYCODE_BUTTON_SELECT, // 4/Select
            KeyEvent.KEYCODE_BUTTON_MODE,   // 5/Analog
            KeyEvent.KEYCODE_BUTTON_START,  // 6/Start
            KeyEvent.KEYCODE_BUTTON_THUMBL, // 7/L3
            KeyEvent.KEYCODE_BUTTON_THUMBR, // 8/R3
            KeyEvent.KEYCODE_BUTTON_L1,     // 9/L1
            KeyEvent.KEYCODE_BUTTON_R1,     // 10/R1
            KeyEvent.KEYCODE_DPAD_UP,       // 11/Up
            KeyEvent.KEYCODE_DPAD_DOWN,     // 12/Down
            KeyEvent.KEYCODE_DPAD_LEFT,     // 13/Left
            KeyEvent.KEYCODE_DPAD_RIGHT,    // 14/Right
            KeyEvent.KEYCODE_BUTTON_L2,     // 15
            KeyEvent.KEYCODE_BUTTON_R2,     // 16
            KeyEvent.KEYCODE_BUTTON_C,      // 17
            KeyEvent.KEYCODE_BUTTON_Z,      // 18
    };
    private static final int[] axisCodes = new int[]{
            MotionEvent.AXIS_X,             // 0/LeftX
            MotionEvent.AXIS_Y,             // 1/LeftY
            MotionEvent.AXIS_Z,             // 2/RightX
            MotionEvent.AXIS_RZ,            // 3/RightY
            MotionEvent.AXIS_LTRIGGER,      // 4/L2
            MotionEvent.AXIS_RTRIGGER,      // 5/R2
            MotionEvent.AXIS_RX,            // 6
            MotionEvent.AXIS_RY,            // 7
            MotionEvent.AXIS_HAT_X,         // 8
            MotionEvent.AXIS_HAT_Y,         // 9
    };

    public static int getButtonIndexForKeyCode(int keyCode) {
        for (int buttonIndex = 0; buttonIndex < buttonKeyCodes.length; buttonIndex++) {
            if (buttonKeyCodes[buttonIndex] == keyCode)
                return buttonIndex;
        }

        Log.e("EmulationSurfaceView", String.format("Button code %d not found", keyCode));
        return -1;
    }

    public static int[] getKnownAxisCodes() {
        return axisCodes;
    }

    public static int getAxisIndexForAxisCode(int axisCode) {
        for (int axisIndex = 0; axisIndex < axisCodes.length; axisIndex++) {
            if (axisCodes[axisIndex] == axisCode)
                return axisIndex;
        }

        Log.e("EmulationSurfaceView", String.format("Axis code %d not found", axisCode));
        return -1;
    }


    private class ButtonMapping {
        public ButtonMapping(int deviceId, int deviceButton, int controllerIndex, int button) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceButton;
            this.controllerIndex = controllerIndex;
            this.buttonMapping = button;
        }

        public int deviceId;
        public int deviceAxisOrButton;
        public int controllerIndex;
        public int buttonMapping;
    }

    private class AxisMapping {
        public AxisMapping(int deviceId, int deviceAxis, InputDevice.MotionRange motionRange, int controllerIndex, int axis) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceAxis;
            this.deviceMotionRange = motionRange;
            this.controllerIndex = controllerIndex;
            this.axisMapping = axis;
            this.positiveButton = -1;
            this.negativeButton = -1;
        }

        public AxisMapping(int deviceId, int deviceAxis, InputDevice.MotionRange motionRange, int controllerIndex, int positiveButton, int negativeButton) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceAxis;
            this.deviceMotionRange = motionRange;
            this.controllerIndex = controllerIndex;
            this.axisMapping = -1;
            this.positiveButton = positiveButton;
            this.negativeButton = negativeButton;
        }

        public int deviceId;
        public int deviceAxisOrButton;
        public InputDevice.MotionRange deviceMotionRange;
        public int controllerIndex;
        public int axisMapping;
        public int positiveButton;
        public int negativeButton;
    }

    private ArrayList<ButtonMapping> mControllerKeyMapping;
    private ArrayList<AxisMapping> mControllerAxisMapping;

    private boolean handleControllerKey(int deviceId, int keyCode, boolean pressed) {
        boolean result = false;
        for (ButtonMapping mapping : mControllerKeyMapping) {
            if (mapping.deviceId != deviceId || mapping.deviceAxisOrButton != keyCode)
                continue;

            AndroidHostInterface.getInstance().handleControllerButtonEvent(0, mapping.buttonMapping, pressed);
            Log.d("EmulationSurfaceView", String.format("handleControllerKey %d -> %d %d", keyCode, mapping.buttonMapping, pressed ? 1 : 0));
            result = true;
        }

        return result;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!isDPadOrButtonEvent(event) || isExternalKeyCode(keyCode))
            return false;

        if (event.getRepeatCount() == 0)
            handleControllerKey(event.getDeviceId(), keyCode, true);

        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (!isDPadOrButtonEvent(event) || isExternalKeyCode(keyCode))
            return false;

        if (event.getRepeatCount() == 0)
            handleControllerKey(event.getDeviceId(), keyCode, false);

        return true;
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        final int source = event.getSource();
        if ((source & (InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_DPAD)) == 0)
            return false;

        final int deviceId = event.getDeviceId();
        for (AxisMapping mapping : mControllerAxisMapping) {
            if (mapping.deviceId != deviceId)
                continue;

            final float axisValue = event.getAxisValue(mapping.deviceAxisOrButton);
            float emuValue;

            if (mapping.deviceMotionRange != null) {
                final float transformedValue = (axisValue - mapping.deviceMotionRange.getMin()) / mapping.deviceMotionRange.getRange();
                emuValue = (transformedValue * 2.0f) - 1.0f;
            } else {
                emuValue = axisValue;
            }
            Log.d("EmulationSurfaceView", String.format("axis %d value %f emuvalue %f", mapping.deviceAxisOrButton, axisValue, emuValue));

            if (mapping.axisMapping >= 0) {
                AndroidHostInterface.getInstance().handleControllerAxisEvent(0, mapping.axisMapping, emuValue);
            }

            final float DEAD_ZONE = 0.25f;
            if (mapping.negativeButton >= 0) {
                AndroidHostInterface.getInstance().handleControllerButtonEvent(0, mapping.negativeButton, (emuValue <= -DEAD_ZONE));
            }
            if (mapping.positiveButton >= 0) {
                AndroidHostInterface.getInstance().handleControllerButtonEvent(0, mapping.positiveButton, (emuValue >= DEAD_ZONE));
            }
        }

        return true;
    }

    private boolean addControllerKeyMapping(int deviceId, int keyCode, int controllerIndex) {
        int mapping = getButtonIndexForKeyCode(keyCode);
        Log.i("EmulationSurfaceView", String.format("Map %d to %d", keyCode, mapping));
        if (mapping >= 0) {
            mControllerKeyMapping.add(new ButtonMapping(deviceId, keyCode, controllerIndex, mapping));
            return true;
        }

        return false;
    }

    private boolean addControllerAxisMapping(int deviceId, List<InputDevice.MotionRange> motionRanges, int axis, int controllerIndex) {
        InputDevice.MotionRange range = null;
        for (InputDevice.MotionRange curRange : motionRanges) {
            if (curRange.getAxis() == axis) {
                range = curRange;
                break;
            }
        }
        if (range == null)
            return false;

        int mapping = getAxisIndexForAxisCode(axis);
        int negativeButton = -1;
        int positiveButton = -1;

        if (mapping >= 0) {
            Log.i("EmulationSurfaceView", String.format("Map axis %d to %d", axis, mapping));
            mControllerAxisMapping.add(new AxisMapping(deviceId, axis, range, controllerIndex, mapping));
            return true;
        }

        if (negativeButton >= 0 && negativeButton >= 0) {
            Log.i("EmulationSurfaceView", String.format("Map axis %d to buttons %d %d", axis, negativeButton, positiveButton));
            mControllerAxisMapping.add(new AxisMapping(deviceId, axis, range, controllerIndex, positiveButton, negativeButton));
            return true;
        }

        Log.w("EmulationSurfaceView", String.format("Axis %d was not mapped", axis));
        return false;
    }

    private static boolean isJoystickDevice(int deviceId) {
        if (deviceId < 0)
            return false;

        final InputDevice dev = InputDevice.getDevice(deviceId);
        if (dev == null)
            return false;

        final int sources = dev.getSources();
        if ((sources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0)
            return true;

        if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
            return true;

        return (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    public boolean initControllerMapping(String controllerType) {
        mControllerKeyMapping = new ArrayList<>();
        mControllerAxisMapping = new ArrayList<>();

        final int[] deviceIds = InputDevice.getDeviceIds();
        for (int deviceId : deviceIds) {
            if (!isJoystickDevice(deviceId))
                continue;

            InputDevice device = InputDevice.getDevice(deviceId);
            List<InputDevice.MotionRange> motionRanges = device.getMotionRanges();
            int controllerIndex = 0;

            for (int keyCode : buttonKeyCodes) {
                addControllerKeyMapping(deviceId, keyCode, controllerIndex);
            }

            if (motionRanges != null) {
                for (int axisCode : axisCodes) {
                    addControllerAxisMapping(deviceId, motionRanges, axisCode, controllerIndex);
                }
            }
        }

        return !mControllerKeyMapping.isEmpty() || !mControllerKeyMapping.isEmpty();
    }

}
