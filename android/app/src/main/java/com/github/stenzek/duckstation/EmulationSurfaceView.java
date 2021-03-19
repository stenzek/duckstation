package com.github.stenzek.duckstation;

import android.content.Context;
import android.os.Vibrator;
import android.util.AttributeSet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;

import java.util.ArrayList;
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

    public static boolean isBindableDevice(InputDevice inputDevice) {
        if (inputDevice == null || inputDevice.isVirtual())
            return false;

        // Accept all devices with an axis or buttons, filter in events.
        final int sources = inputDevice.getSources();
        return ((sources & InputDevice.SOURCE_CLASS_JOYSTICK) == InputDevice.SOURCE_CLASS_JOYSTICK) ||
                ((sources & InputDevice.SOURCE_CLASS_BUTTON) == InputDevice.SOURCE_CLASS_BUTTON);
    }

    public static boolean isJoystickMotionEvent(MotionEvent event) {
        final int source = event.getSource();
        return ((source & InputDevice.SOURCE_CLASS_JOYSTICK) == InputDevice.SOURCE_CLASS_JOYSTICK);
    }

    public static boolean isGamepadDevice(InputDevice inputDevice) {
        final int sources = inputDevice.getSources();
        return ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD);
    }

    public static boolean isBindableKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_HOME:
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_POWER:
            case KeyEvent.KEYCODE_CAMERA:
            case KeyEvent.KEYCODE_CALL:
            case KeyEvent.KEYCODE_ENDCALL:
            case KeyEvent.KEYCODE_VOICE_ASSIST:
                return false;

            default:
                return true;
        }
    }

    private static boolean isExternalKeyCode(int keyCode) {
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

    private class InputDeviceData {
        private int deviceId;
        private String descriptor;
        private int[] axes;
        private float[] axisValues;
        private int controllerIndex;
        private Vibrator vibrator;

        public InputDeviceData(InputDevice device, int controllerIndex) {
            deviceId = device.getId();
            descriptor = device.getDescriptor();
            this.controllerIndex = controllerIndex;

            List<InputDevice.MotionRange> motionRanges = device.getMotionRanges();
            if (motionRanges != null && !motionRanges.isEmpty()) {
                axes = new int[motionRanges.size()];
                axisValues = new float[motionRanges.size()];
                for (int i = 0; i < motionRanges.size(); i++)
                    axes[i] = motionRanges.get(i).getAxis();
            }

            // device.getVibrator() always returns null, but might return a "null vibrator".
            final Vibrator potentialVibrator = device.getVibrator();
            if (potentialVibrator != null && potentialVibrator.hasVibrator())
                vibrator = potentialVibrator;
        }
    }

    private InputDeviceData[] mInputDevices = null;
    private boolean mHasAnyGamepads = false;

    public boolean hasAnyGamePads() {
        return mHasAnyGamepads;
    }

    public synchronized void updateInputDevices() {
        mInputDevices = null;
        mHasAnyGamepads = false;

        final ArrayList<InputDeviceData> inputDeviceIds = new ArrayList<>();
        for (int deviceId : InputDevice.getDeviceIds()) {
            final InputDevice device = InputDevice.getDevice(deviceId);
            if (device == null || !isBindableDevice(device))
                continue;

            if (isGamepadDevice(device))
                mHasAnyGamepads = true;

            final int controllerIndex = inputDeviceIds.size();
            Log.d("EmulationSurfaceView", String.format("Tracking device %d/%s (%s)",
                    controllerIndex, device.getDescriptor(), device.getName()));
            inputDeviceIds.add(new InputDeviceData(device, controllerIndex));
        }

        if (inputDeviceIds.isEmpty())
            return;

        mInputDevices = new InputDeviceData[inputDeviceIds.size()];
        inputDeviceIds.toArray(mInputDevices);
    }

    public synchronized String[] getInputDeviceNames() {
        if (mInputDevices == null)
            return null;

        final String[] deviceNames = new String[mInputDevices.length];
        for (int i = 0; i < mInputDevices.length; i++) {
            deviceNames[i] = mInputDevices[i].descriptor;
        }

        return deviceNames;
    }

    public synchronized boolean hasInputDeviceVibration(int controllerIndex) {
        if (mInputDevices == null || controllerIndex >= mInputDevices.length)
            return false;

        return (mInputDevices[controllerIndex].vibrator != null);
    }

    public synchronized void setInputDeviceVibration(int controllerIndex, float smallMotor, float largeMotor) {
        if (mInputDevices == null || controllerIndex >= mInputDevices.length)
            return;

        // shouldn't get here
        final InputDeviceData data = mInputDevices[controllerIndex];
        if (data.vibrator == null)
            return;

        final float MINIMUM_INTENSITY = 0.1f;
        if (smallMotor >= MINIMUM_INTENSITY || largeMotor >= MINIMUM_INTENSITY)
            data.vibrator.vibrate(1000);
        else
            data.vibrator.cancel();
    }

    public InputDeviceData getDataForDeviceId(int deviceId) {
        if (mInputDevices == null)
            return null;

        for (InputDeviceData data : mInputDevices) {
            if (data.deviceId == deviceId)
                return data;
        }

        return null;
    }

    public int getControllerIndexForDeviceId(int deviceId) {
        final InputDeviceData data = getDataForDeviceId(deviceId);
        return (data != null) ? data.controllerIndex : -1;
    }

    private boolean handleKeyEvent(int keyCode, KeyEvent event, boolean pressed) {
        if (!isBindableDevice(event.getDevice()))
            return false;

        /*Log.e("ESV", String.format("Code %d RC %d Pressed %d %s", keyCode,
                event.getRepeatCount(), pressed? 1 : 0, event.toString()));*/

        final AndroidHostInterface hi = AndroidHostInterface.getInstance();
        final int controllerIndex = getControllerIndexForDeviceId(event.getDeviceId());
        if (event.getRepeatCount() == 0 && controllerIndex >= 0)
            hi.handleControllerButtonEvent(controllerIndex, keyCode, pressed);

        // We don't want to eat external button events unless it's actually bound.
        if (isExternalKeyCode(keyCode))
            return (controllerIndex >= 0 && hi.hasControllerButtonBinding(controllerIndex, keyCode));
        else
            return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return handleKeyEvent(keyCode, event, true);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return handleKeyEvent(keyCode, event, false);
    }

    private float clamp(float value, float min, float max) {
        return (value < min) ? min : ((value > max) ? max : value);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (!isJoystickMotionEvent(event))
            return false;

        final InputDeviceData data = getDataForDeviceId(event.getDeviceId());
        if (data == null || data.axes == null)
            return false;

        for (int i = 0; i < data.axes.length; i++) {
            final int axis = data.axes[i];
            final float axisValue = event.getAxisValue(axis);
            float emuValue;

            switch (axis) {
                case MotionEvent.AXIS_BRAKE:
                case MotionEvent.AXIS_GAS:
                case MotionEvent.AXIS_LTRIGGER:
                case MotionEvent.AXIS_RTRIGGER:
                    // Scale 0..1 -> -1..1.
                    emuValue = (clamp(axisValue, 0.0f, 1.0f) * 2.0f) - 1.0f;
                    break;

                default:
                    // Everything else should already by -1..1 as per Android documentation.
                    emuValue = clamp(axisValue, -1.0f, 1.0f);
                    break;
            }

            if (data.axisValues[i] == emuValue)
                continue;

            /*Log.d("EmulationSurfaceView",
                    String.format("axis %d value %f emuvalue %f", axis, axisValue, emuValue));*/

            data.axisValues[i] = emuValue;
            AndroidHostInterface.getInstance().handleControllerAxisEvent(data.controllerIndex, axis, emuValue);
        }

        return true;
    }
}
