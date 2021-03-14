package com.github.stenzek.duckstation;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;
import android.util.ArraySet;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import androidx.annotation.NonNull;

import java.util.HashMap;
import java.util.List;

public class ControllerBindingDialog extends AlertDialog {
    final static float DETECT_THRESHOLD = 0.25f;
    private final ControllerBindingPreference.Type mType;
    private final String mSettingKey;
    private String mCurrentBinding;
    private int mUpdatedAxisCode = -1;
    private final HashMap<Integer, float[]> mStartingAxisValues = new HashMap<>();

    public ControllerBindingDialog(Context context, String buttonName, String settingKey, String currentBinding, ControllerBindingPreference.Type type) {
        super(context);

        mType = type;
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
                return onKeyDown(keyCode, event);
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
        if (!EmulationSurfaceView.isBindableDevice(event.getDevice()) || !EmulationSurfaceView.isBindableKeyCode(event.getKeyCode())) {
            return super.onKeyUp(keyCode, event);
        }

        if (mType == ControllerBindingPreference.Type.BUTTON)
            mCurrentBinding = String.format("%s/Button%d", event.getDevice().getDescriptor(), event.getKeyCode());
        else if (mType == ControllerBindingPreference.Type.VIBRATION)
            mCurrentBinding = event.getDevice().getDescriptor();
        else
            return super.onKeyUp(keyCode, event);

        updateMessage();
        updateBinding();
        dismiss();
        return true;
    }

    private void setAxisCode(InputDevice device, int axisCode, boolean positive) {
        if (mUpdatedAxisCode >= 0)
            return;

        mUpdatedAxisCode = axisCode;

        final int controllerIndex = 0;
        if (mType == ControllerBindingPreference.Type.AXIS)
            mCurrentBinding = String.format("%s/Axis%d", device.getDescriptor(), axisCode);
        else
            mCurrentBinding = String.format("%s/%cAxis%d", device.getDescriptor(), (positive) ? '+' : '-', axisCode);

        updateBinding();
        updateMessage();
        dismiss();
    }

    private boolean doAxisDetection(MotionEvent event) {
        if (!EmulationSurfaceView.isBindableDevice(event.getDevice()))
            return false;

        final List<InputDevice.MotionRange> motionEventList = event.getDevice().getMotionRanges();
        if (motionEventList == null || motionEventList.isEmpty())
            return false;

        final int deviceId = event.getDeviceId();
        if (!mStartingAxisValues.containsKey(deviceId)) {
            final float[] axisValues = new float[motionEventList.size()];
            for (int axisIndex = 0; axisIndex < motionEventList.size(); axisIndex++) {
                final int axisCode = motionEventList.get(axisIndex).getAxis();

                // these are binary, so start at zero
                if (axisCode == MotionEvent.AXIS_HAT_X || axisCode == MotionEvent.AXIS_HAT_Y)
                    axisValues[axisIndex] = 0.0f;
                else
                    axisValues[axisIndex] = event.getAxisValue(axisCode);
            }

            mStartingAxisValues.put(deviceId, axisValues);
            return false;
        }

        final float[] axisValues = mStartingAxisValues.get(deviceId);
        for (int axisIndex = 0; axisIndex < motionEventList.size(); axisIndex++) {
            final int axisCode = motionEventList.get(axisIndex).getAxis();
            final float newValue = event.getAxisValue(axisCode);
            if (Math.abs(newValue - axisValues[axisIndex]) >= DETECT_THRESHOLD) {
                setAxisCode(event.getDevice(), axisCode, newValue >= 0.0f);
                break;
            }
        }

        return true;
    }

    @Override
    public boolean onGenericMotionEvent(@NonNull MotionEvent event) {
        if (mType != ControllerBindingPreference.Type.AXIS && mType != ControllerBindingPreference.Type.BUTTON) {
            return false;
        }

        if (doAxisDetection(event))
            return true;

        return super.onGenericMotionEvent(event);
    }
}
