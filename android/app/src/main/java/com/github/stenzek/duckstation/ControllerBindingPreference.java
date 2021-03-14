package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AttributeSet;
import android.view.InputDevice;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;
import androidx.preference.Preference;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceViewHolder;

import java.util.Set;

public class ControllerBindingPreference extends Preference {
    public enum Type {
        BUTTON,
        AXIS,
        VIBRATION
    }

    private enum VisualType {
        BUTTON,
        AXIS,
        VIBRATION,
        HOTKEY
    }

    private String mBindingName;
    private String mDisplayName;
    private String mValue;
    private TextView mValueView;
    private Type mType = Type.BUTTON;
    private VisualType mVisualType = VisualType.BUTTON;

    private static int getIconForButton(String buttonName) {
        if (buttonName.equals("Up")) {
            return R.drawable.ic_controller_up_button_pressed;
        } else if (buttonName.equals("Right")) {
            return R.drawable.ic_controller_right_button_pressed;
        } else if (buttonName.equals("Down")) {
            return R.drawable.ic_controller_down_button_pressed;
        } else if (buttonName.equals("Left")) {
            return R.drawable.ic_controller_left_button_pressed;
        } else if (buttonName.equals("Triangle")) {
            return R.drawable.ic_controller_triangle_button_pressed;
        } else if (buttonName.equals("Circle")) {
            return R.drawable.ic_controller_circle_button_pressed;
        } else if (buttonName.equals("Cross")) {
            return R.drawable.ic_controller_cross_button_pressed;
        } else if (buttonName.equals("Square")) {
            return R.drawable.ic_controller_square_button_pressed;
        } else if (buttonName.equals("Start")) {
            return R.drawable.ic_controller_start_button_pressed;
        } else if (buttonName.equals("Select")) {
            return R.drawable.ic_controller_select_button_pressed;
        } else if (buttonName.equals("L1")) {
            return R.drawable.ic_controller_l1_button_pressed;
        } else if (buttonName.equals("L2")) {
            return R.drawable.ic_controller_l2_button_pressed;
        } else if (buttonName.equals("R1")) {
            return R.drawable.ic_controller_r1_button_pressed;
        } else if (buttonName.equals("R2")) {
            return R.drawable.ic_controller_r2_button_pressed;
        }

        return R.drawable.ic_baseline_radio_button_unchecked_24;
    }

    private static int getIconForAxis(String axisName) {
        return R.drawable.ic_baseline_radio_button_checked_24;
    }

    private static int getIconForHotkey(String hotkeyDisplayName) {
        switch (hotkeyDisplayName) {
            case "FastForward":
            case "ToggleFastForward":
            case "Turbo":
            case "ToggleTurbo":
                return R.drawable.ic_controller_fast_forward;

            default:
                return R.drawable.ic_baseline_category_24;
        }
    }

    public ControllerBindingPreference(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
        setWidgetLayoutResource(R.layout.layout_controller_binding_preference);
        setIconSpaceReserved(false);
    }

    public ControllerBindingPreference(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        setWidgetLayoutResource(R.layout.layout_controller_binding_preference);
        setIconSpaceReserved(false);
    }

    public ControllerBindingPreference(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setWidgetLayoutResource(R.layout.layout_controller_binding_preference);
        setIconSpaceReserved(false);
    }

    @Override
    public void onBindViewHolder(PreferenceViewHolder holder) {
        super.onBindViewHolder(holder);

        ImageView iconView = ((ImageView) holder.findViewById(R.id.controller_binding_icon));
        TextView nameView = ((TextView) holder.findViewById(R.id.controller_binding_name));
        mValueView = ((TextView) holder.findViewById(R.id.controller_binding_value));

        int drawableId = R.drawable.ic_baseline_radio_button_checked_24;
        switch (mVisualType) {
            case BUTTON:
                drawableId = getIconForButton(mBindingName);
                break;
            case AXIS:
                drawableId = getIconForAxis(mBindingName);
                break;
            case HOTKEY:
                drawableId = getIconForHotkey(mBindingName);
                break;
            case VIBRATION:
                drawableId = R.drawable.ic_baseline_vibration_24;
                break;
        }

        iconView.setImageDrawable(ContextCompat.getDrawable(getContext(), drawableId));
        nameView.setText(mDisplayName);
        updateValue();
    }

    @Override
    protected void onClick() {
        ControllerBindingDialog dialog = new ControllerBindingDialog(getContext(), mBindingName, getKey(), mValue, mType);
        dialog.setOnDismissListener((dismissedDialog) -> updateValue());
        dialog.show();
    }

    public void initButton(int controllerIndex, String buttonName) {
        mBindingName = buttonName;
        mDisplayName = buttonName;
        mType = Type.BUTTON;
        mVisualType = VisualType.BUTTON;
        setKey(String.format("Controller%d/Button%s", controllerIndex, buttonName));
        updateValue();
    }

    public void initAxis(int controllerIndex, String axisName) {
        mBindingName = axisName;
        mDisplayName = axisName;
        mType = Type.AXIS;
        mVisualType = VisualType.AXIS;
        setKey(String.format("Controller%d/Axis%s", controllerIndex, axisName));
        updateValue();
    }

    public void initVibration(int controllerIndex) {
        mBindingName = "Rumble";
        mDisplayName = getContext().getString(R.string.controller_binding_device_for_vibration);
        mType = Type.VIBRATION;
        mVisualType = VisualType.VIBRATION;
        setKey(String.format("Controller%d/Rumble", controllerIndex));
        updateValue();
    }

    public void initHotkey(HotkeyInfo hotkeyInfo) {
        mBindingName = hotkeyInfo.getName();
        mDisplayName = hotkeyInfo.getDisplayName();
        mType = Type.BUTTON;
        mVisualType = VisualType.HOTKEY;
        setKey(hotkeyInfo.getBindingConfigKey());
        updateValue();
    }

    private String prettyPrintBinding(String value) {
        final int index = value.indexOf('/');
        String device, binding;
        if (index >= 0) {
            device = value.substring(0, index);
            binding = value.substring(index);
        } else {
            device = value;
            binding = "";
        }

        String humanName = device;
        int deviceIndex = -1;

        final int[] deviceIds = InputDevice.getDeviceIds();
        for (int i = 0; i < deviceIds.length; i++) {
            final InputDevice inputDevice = InputDevice.getDevice(deviceIds[i]);
            if (inputDevice == null || !inputDevice.getDescriptor().equals(device)) {
                continue;
            }

            humanName = inputDevice.getName();
            deviceIndex = i;
            break;
        }

        final int MAX_LENGTH = 40;
        if (humanName.length() > MAX_LENGTH) {
            final StringBuilder shortenedName = new StringBuilder();
            shortenedName.append(humanName, 0, MAX_LENGTH / 2);
            shortenedName.append("...");
            shortenedName.append(humanName, humanName.length() - (MAX_LENGTH / 2),
                    humanName.length());

            humanName = shortenedName.toString();
        }

        if (deviceIndex < 0)
            return String.format("%s[??]%s", humanName, binding);
        else
            return String.format("%s[%d]%s", humanName, deviceIndex, binding);
    }

    private void updateValue(String value) {
        mValue = value;
        if (mValueView != null) {
            if (value != null)
                mValueView.setText(value);
            else
                mValueView.setText(getContext().getString(R.string.controller_binding_dialog_no_binding));
        }
    }

    public void updateValue() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(getContext());
        Set<String> values = PreferenceHelpers.getStringSet(prefs, getKey());
        if (values != null) {
            StringBuilder sb = new StringBuilder();
            for (String value : values) {
                if (sb.length() > 0)
                    sb.append(", ");
                sb.append(prettyPrintBinding(value));
            }

            updateValue(sb.toString());
        } else {
            updateValue(null);
        }
    }

    public void clearBinding(SharedPreferences.Editor prefEditor) {
        try {
            prefEditor.remove(getKey());
        } catch (Exception e) {

        }

        updateValue(null);
    }
}
