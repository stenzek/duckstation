package com.github.stenzek.duckstation;

import android.content.Context;

import androidx.preference.ListPreference;
import androidx.preference.SwitchPreference;

public class GameTraitPreference extends SwitchPreference {
    private String mGamePath;

    /**
     * Creates a boolean game property preference.
     */
    public GameTraitPreference(Context context, String gamePath, String settingKey, int titleId) {
        super(context);
        mGamePath = gamePath;
        setPersistent(false);
        setTitle(titleId);
        setKey(settingKey);
        setIconSpaceReserved(false);
        updateValue();
    }

    private void updateValue() {
        final String value = AndroidHostInterface.getInstance().getGameSettingValue(mGamePath, getKey());
        super.setChecked(value != null && value.equals("true"));
    }

    @Override
    public void setChecked(boolean checked) {
        super.setChecked(checked);
        AndroidHostInterface.getInstance().setGameSettingValue(mGamePath, getKey(), checked ? "true" : "false");
    }
}
