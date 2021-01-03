package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AttributeSet;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;
import androidx.preference.Preference;
import androidx.preference.PreferenceManager;
import androidx.preference.PreferenceViewHolder;

import java.util.Set;

public class ControllerBindingPreference extends Preference {
    private enum Type {
        BUTTON,
        AXIS,
        HOTKEY
    }

    private String mBindingName;
    private String mValue;
    private TextView mValueView;
    private Type mType = Type.BUTTON;

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
        return R.drawable.ic_baseline_category_24;
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
        switch (mType) {
            case BUTTON:
                drawableId = getIconForButton(mBindingName);
                break;
            case AXIS:
                drawableId = getIconForAxis(mBindingName);
                break;
            case HOTKEY:
                drawableId = getIconForHotkey(mBindingName);
                break;
        }

        iconView.setImageDrawable(ContextCompat.getDrawable(getContext(), drawableId));
        nameView.setText(mBindingName);
        updateValue();
    }

    @Override
    protected void onClick() {
        ControllerBindingDialog dialog = new ControllerBindingDialog(getContext(), mBindingName, getKey(), mValue, (mType == Type.AXIS));
        dialog.setOnDismissListener((dismissedDialog) -> updateValue());
        dialog.show();
    }

    public void initButton(int controllerIndex, String buttonName) {
        mBindingName = buttonName;
        mType = Type.BUTTON;
        setKey(String.format("Controller%d/Button%s", controllerIndex, buttonName));
        updateValue();
    }

    public void initAxis(int controllerIndex, String axisName) {
        mBindingName = axisName;
        mType = Type.AXIS;
        setKey(String.format("Controller%d/Axis%s", controllerIndex, axisName));
        updateValue();
    }

    public void initHotkey(HotkeyInfo hotkeyInfo) {
        mBindingName = hotkeyInfo.getDisplayName();
        mType = Type.HOTKEY;
        setKey(hotkeyInfo.getBindingConfigKey());
        updateValue();
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
                sb.append(value);
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
