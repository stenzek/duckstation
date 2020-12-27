package com.github.stenzek.duckstation;

import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.util.ArraySet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import androidx.annotation.NonNull;

import java.util.HashMap;

public class ControllerBindingDialog extends AlertDialog {
    private boolean mIsAxis;
    private String mSettingKey;
    private String mCurrentBinding;

    public ControllerBindingDialog(Context context, String buttonName, String settingKey, String currentBinding, boolean isAxis) {
        super(context);

        mIsAxis = isAxis;
        mSettingKey = settingKey;
        mCurrentBinding = currentBinding;
        if (mCurrentBinding == null)
            mCurrentBinding = getContext().getString(R.string.controller_binding_dialog_no_binding);

        setTitle(buttonName);
        updateMessage();
        setButton(BUTTON_POSITIVE, context.getString(R.string.controller_binding_dialog_cancel), (dialogInterface, button) -> dismiss());
        setButton(BUTTON_NEGATIVE, context.getString(R.string.controller_binding_dialog_clear), (dialogInterface, button) -> {
            mCurrentBinding = null;
            updateBinding();
            dismiss();
        });

        setOnKeyListener(new DialogInterface.OnKeyListener() {
            @Override
            public boolean onKey(DialogInterface dialog, int keyCode, KeyEvent event) {
                if (onKeyDown(keyCode, event))
                    return true;

                return false;
            }
        });
    }

    private void updateMessage() {
        setMessage(String.format(getContext().getString(R.string.controller_binding_dialog_message), mCurrentBinding));
    }

    private void updateBinding() {
        SharedPreferences.Editor editor = PreferenceManager.getDefaultSharedPreferences(getContext()).edit();
        if (mCurrentBinding != null) {
            ArraySet<String> values = new ArraySet<>();
            values.add(mCurrentBinding);
            editor.putStringSet(mSettingKey, values);
        } else {
            try {
                editor.remove(mSettingKey);
            } catch (Exception e) {

            }
        }

        editor.commit();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (mIsAxis || !EmulationSurfaceView.isDPadOrButtonEvent(event))
            return super.onKeyUp(keyCode, event);

        int buttonIndex = EmulationSurfaceView.getButtonIndexForKeyCode(keyCode);
        if (buttonIndex < 0)
            return super.onKeyUp(keyCode, event);

        // TODO: Multiple controllers
        final int controllerIndex = 0;
        mCurrentBinding = String.format("Controller%d/Button%d", controllerIndex, buttonIndex);
        updateMessage();
        updateBinding();
        dismiss();
        return true;
    }

    private int mUpdatedAxisCode = -1;

    private void setAxisCode(int axisCode, boolean positive) {
        final int axisIndex = EmulationSurfaceView.getAxisIndexForAxisCode(axisCode);
        if (mUpdatedAxisCode >= 0 || axisIndex < 0)
            return;

        mUpdatedAxisCode = axisCode;

        final int controllerIndex = 0;
        if (mIsAxis)
            mCurrentBinding = String.format("Controller%d/Axis%d", controllerIndex, axisIndex);
        else
            mCurrentBinding = String.format("Controller%d/%cAxis%d", controllerIndex, (positive) ? '+' : '-', axisIndex);

        updateBinding();
        updateMessage();
        dismiss();
    }

    final static float DETECT_THRESHOLD = 0.25f;

    private HashMap<Integer, float[]> mStartingAxisValues = new HashMap<>();

    private boolean doAxisDetection(MotionEvent event) {
        if ((event.getSource() & (InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_DPAD)) == 0)
            return false;

        final int[] axisCodes = EmulationSurfaceView.getKnownAxisCodes();
        final int deviceId = event.getDeviceId();

        if (!mStartingAxisValues.containsKey(deviceId)) {
            final float[] axisValues = new float[axisCodes.length];
            for (int axisIndex = 0; axisIndex < axisCodes.length; axisIndex++) {
                final int axisCode = axisCodes[axisIndex];

                // these are binary, so start at zero
                if (axisCode == MotionEvent.AXIS_HAT_X || axisCode == MotionEvent.AXIS_HAT_Y)
                    axisValues[axisIndex] = 0.0f;
                else
                    axisValues[axisIndex] = event.getAxisValue(axisCode);
            }

            mStartingAxisValues.put(deviceId, axisValues);
        }

        final float[] axisValues = mStartingAxisValues.get(deviceId);
        for (int axisIndex = 0; axisIndex < axisCodes.length; axisIndex++) {
            final float newValue = event.getAxisValue(axisCodes[axisIndex]);
            if (Math.abs(newValue - axisValues[axisIndex]) >= DETECT_THRESHOLD) {
                setAxisCode(axisCodes[axisIndex], newValue >= 0.0f);
                break;
            }
        }

        return true;
    }

    @Override
    public boolean onGenericMotionEvent(@NonNull MotionEvent event) {
        if (doAxisDetection(event))
            return true;

        return super.onGenericMotionEvent(event);
    }
}
