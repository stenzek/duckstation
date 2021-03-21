package com.github.stenzek.duckstation;

import android.content.SharedPreferences;
import android.os.Vibrator;
import android.text.InputType;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.widget.EditText;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.PreferenceManager;

import java.util.ArrayList;
import java.util.List;

public class ControllerAutoMapper {
    public interface CompleteCallback {
        public void onComplete();
    }

    final private ControllerSettingsActivity parent;
    final private int port;
    private final CompleteCallback completeCallback;

    private InputDevice device;
    private SharedPreferences prefs;
    private SharedPreferences.Editor editor;
    private StringBuilder log;
    private String keyBase;
    private String controllerType;

    public ControllerAutoMapper(ControllerSettingsActivity activity, int port, CompleteCallback completeCallback) {
        this.parent = activity;
        this.port = port;
        this.completeCallback = completeCallback;
    }

    private void log(String format, Object... args) {
        log.append(String.format(format, args));
        log.append('\n');
    }

    private void setButtonBindingToKeyCode(String buttonName, int keyCode) {
        log("Binding button '%s' to key '%s' (%d)", buttonName, KeyEvent.keyCodeToString(keyCode), keyCode);

        final String key = String.format("%sButton%s", keyBase, buttonName);
        final String value = String.format("%s/Button%d", device.getDescriptor(), keyCode);
        editor.putString(key, value);
    }

    private void setButtonBindingToAxis(String buttonName, int axis, int direction) {
        final char directionIndicator = (direction < 0) ? '-' : '+';
        log("Binding button '%s' to axis '%s' (%d) direction %c", buttonName, MotionEvent.axisToString(axis), axis, directionIndicator);

        final String key = String.format("%sButton%s", keyBase, buttonName);
        final String value = String.format("%s/%cAxis%d", device.getDescriptor(), directionIndicator, axis);
        editor.putString(key, value);
    }

    private void setAxisBindingToAxis(String axisName, int axis) {
        log("Binding axis '%s' to axis '%s' (%d)", axisName, MotionEvent.axisToString(axis), axis);

        final String key = String.format("%sAxis%s", keyBase, axisName);
        final String value = String.format("%s/Axis%d", device.getDescriptor(), axis);
        editor.putString(key, value);
    }

    private void doAutoBindingButton(String buttonName, int[] keyCodes, int[][] axisCodes) {
        // Prefer the axis codes, as it dispatches to that first.
        if (axisCodes != null) {
            final List<InputDevice.MotionRange> motionRangeList = device.getMotionRanges();
            for (int[] axisAndDirection : axisCodes) {
                final int axis = axisAndDirection[0];
                final int direction = axisAndDirection[1];
                for (InputDevice.MotionRange range : motionRangeList) {
                    if (range.getAxis() == axis) {
                        setButtonBindingToAxis(buttonName, axis, direction);
                        return;
                    }
                }
            }
        }

        if (keyCodes != null) {
            final boolean[] keysPresent = device.hasKeys(keyCodes);
            for (int i = 0; i < keysPresent.length; i++) {
                if (keysPresent[i]) {
                    setButtonBindingToKeyCode(buttonName, keyCodes[i]);
                    return;
                }
            }
        }

        log("No automatic bindings found for button '%s'", buttonName);
    }

    private void doAutoBindingAxis(String axisName, int[] axisCodes) {
        // Prefer the axis codes, as it dispatches to that first.
        if (axisCodes != null) {
            final List<InputDevice.MotionRange> motionRangeList = device.getMotionRanges();
            for (final int axis : axisCodes) {
                for (InputDevice.MotionRange range : motionRangeList) {
                    if (range.getAxis() == axis) {
                        setAxisBindingToAxis(axisName, axis);
                        return;
                    }
                }
            }
        }

        log.append(String.format("No automatic bindings found for axis '%s'\n", axisName));
    }

    public void start() {
        final ArrayList<InputDevice> deviceList = new ArrayList<>();
        for (final int deviceId : InputDevice.getDeviceIds()) {
            final InputDevice inputDevice = InputDevice.getDevice(deviceId);
            if (inputDevice == null || !EmulationSurfaceView.isBindableDevice(inputDevice) ||
                    !EmulationSurfaceView.isGamepadDevice(inputDevice)) {
                continue;
            }

            deviceList.add(inputDevice);
        }

        if (deviceList.isEmpty()) {
            final AlertDialog.Builder builder = new AlertDialog.Builder(parent);
            builder.setTitle(R.string.main_activity_error);
            builder.setMessage(R.string.controller_auto_mapping_no_devices);
            builder.setPositiveButton(R.string.main_activity_ok, (dialog, which) -> dialog.dismiss());
            builder.create().show();
            return;
        }

        final String[] deviceNames = new String[deviceList.size()];
        for (int i = 0; i < deviceList.size(); i++)
            deviceNames[i] = deviceList.get(i).getName();

        final AlertDialog.Builder builder = new AlertDialog.Builder(parent);
        builder.setTitle(R.string.controller_auto_mapping_select_device);
        builder.setItems(deviceNames, (dialog, which) -> {
            process(deviceList.get(which));
        });
        builder.create().show();
    }

    private void process(InputDevice device) {
        this.prefs = PreferenceManager.getDefaultSharedPreferences(parent);
        this.editor = prefs.edit();
        this.log = new StringBuilder();
        this.device = device;

        this.keyBase = String.format("Controller%d/", port);
        this.controllerType = parent.getControllerType(prefs, port);

        setButtonBindings();
        setAxisBindings();
        setVibrationBinding();

        this.editor.commit();
        this.editor = null;

        final AlertDialog.Builder builder = new AlertDialog.Builder(parent);
        builder.setTitle(R.string.controller_auto_mapping_results);

        final EditText editText = new EditText(parent);
        editText.setText(log.toString());
        editText.setInputType(InputType.TYPE_NULL | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        editText.setSingleLine(false);
        builder.setView(editText);

        builder.setPositiveButton(R.string.main_activity_ok, (dialog, which) -> dialog.dismiss());
        builder.create().show();

        if (completeCallback != null)
            completeCallback.onComplete();
    }

    private void setButtonBindings() {
        final String[] buttonNames = AndroidHostInterface.getInstance().getControllerButtonNames(controllerType);
        if (buttonNames == null || buttonNames.length == 0) {
            log("No axes to bind.");
            return;
        }

        for (final String buttonName : buttonNames) {
            switch (buttonName) {
                case "Up":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_DPAD_UP}, new int[][]{{MotionEvent.AXIS_HAT_Y, -1}});
                    break;
                case "Down":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_DPAD_DOWN}, new int[][]{{MotionEvent.AXIS_HAT_Y, 1}});
                    break;
                case "Left":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_DPAD_LEFT}, new int[][]{{MotionEvent.AXIS_HAT_X, -1}});
                    break;
                case "Right":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_DPAD_RIGHT}, new int[][]{{MotionEvent.AXIS_HAT_X, 1}});
                    break;
                case "Select":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_SELECT}, null);
                    break;
                case "Start":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_START}, null);
                    break;
                case "Triangle":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_Y}, null);
                    break;
                case "Cross":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_A}, null);
                    break;
                case "Circle":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_B}, null);
                    break;
                case "Square":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_X}, null);
                    break;
                case "L1":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_L1}, null);
                    break;
                case "L2":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_L2}, new int[][]{{MotionEvent.AXIS_LTRIGGER, 1}, {MotionEvent.AXIS_Z, 1}, {MotionEvent.AXIS_BRAKE, 1}});
                    break;
                case "R1":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_R1}, null);
                    break;
                case "R2":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_R2}, new int[][]{{MotionEvent.AXIS_RTRIGGER, 1}, {MotionEvent.AXIS_RZ, 1}, {MotionEvent.AXIS_GAS, 1}});
                    break;
                case "L3":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_THUMBL}, null);
                    break;
                case "R3":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_THUMBR}, null);
                    break;
                case "Analog":
                    doAutoBindingButton(buttonName, new int[]{KeyEvent.KEYCODE_BUTTON_MODE}, null);
                    break;
                default:
                    log("Button '%s' not supported by auto mapping.", buttonName);
                    break;
            }
        }
    }

    private void setAxisBindings() {
        final String[] axisNames = AndroidHostInterface.getInstance().getControllerAxisNames(controllerType);
        if (axisNames == null || axisNames.length == 0) {
            log("No axes to bind.");
            return;
        }

        for (final String axisName : axisNames) {
            switch (axisName) {
                case "LeftX":
                    doAutoBindingAxis(axisName, new int[]{MotionEvent.AXIS_X});
                    break;
                case "LeftY":
                    doAutoBindingAxis(axisName, new int[]{MotionEvent.AXIS_Y});
                    break;
                case "RightX":
                    doAutoBindingAxis(axisName, new int[]{MotionEvent.AXIS_RX, MotionEvent.AXIS_Z});
                    break;
                case "RightY":
                    doAutoBindingAxis(axisName, new int[]{MotionEvent.AXIS_RY, MotionEvent.AXIS_RZ});
                    break;
                default:
                    log("Axis '%s' not supported by auto mapping.", axisName);
                    break;
            }
        }
    }

    private void setVibrationBinding() {
        final int motorCount = AndroidHostInterface.getInstance().getControllerVibrationMotorCount(controllerType);
        if (motorCount == 0) {
            log("No vibration motors to bind.");
            return;
        }

        final Vibrator vibrator = device.getVibrator();
        if (vibrator == null || !vibrator.hasVibrator()) {
            log("Selected device has no vibrator, cannot bind vibration.");
            return;
        }

        log("Binding vibration to device '%s'.", device.getDescriptor());

        final String key = String.format("%sRumble", keyBase);
        editor.putString(key, device.getDescriptor());
    }
}
