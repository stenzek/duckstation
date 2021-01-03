package com.github.stenzek.duckstation;

import android.content.Context;
import android.util.AttributeSet;

import androidx.preference.ListPreference;

public class GameSettingPreference extends ListPreference {
    private String mGamePath;

    /**
     * Creates a boolean game property preference.
     */
    public GameSettingPreference(Context context, String gamePath, String settingKey, int titleId) {
        super(context);
        mGamePath = gamePath;
        setPersistent(false);
        setTitle(titleId);
        setKey(settingKey);
        setIconSpaceReserved(false);
        setSummaryProvider(SimpleSummaryProvider.getInstance());

        setEntries(R.array.settings_boolean_entries);
        setEntryValues(R.array.settings_boolean_values);

        updateValue();
    }

    /**
     * Creates a list game property preference.
     */
    public GameSettingPreference(Context context, String gamePath, String settingKey, int titleId, int entryArray, int entryValuesArray) {
        super(context);
        mGamePath = gamePath;
        setPersistent(false);
        setTitle(titleId);
        setKey(settingKey);
        setIconSpaceReserved(false);
        setSummaryProvider(SimpleSummaryProvider.getInstance());

        setEntries(entryArray);
        setEntryValues(entryValuesArray);

        updateValue();
    }

    private void updateValue() {
        final String value = AndroidHostInterface.getInstance().getGameSettingValue(mGamePath, getKey());
        if (value == null)
            super.setValue("null");
        else
            super.setValue(value);
    }

    @Override
    public void setValue(String value) {
        super.setValue(value);
        if (value.equals("null"))
            AndroidHostInterface.getInstance().setGameSettingValue(mGamePath, getKey(), null);
        else
            AndroidHostInterface.getInstance().setGameSettingValue(mGamePath, getKey(), value);
    }

    @Override
    public void setEntries(CharSequence[] entries) {
        final int length = (entries != null) ? entries.length : 0;
        CharSequence[] newEntries = new CharSequence[length + 1];
        newEntries[0] = getContext().getString(R.string.game_properties_preference_use_global_setting);
        if (entries != null)
            System.arraycopy(entries, 0, newEntries, 1, entries.length);
        super.setEntries(newEntries);
    }

    @Override
    public void setEntryValues(CharSequence[] entryValues) {
        final int length = (entryValues != null) ? entryValues.length : 0;
        CharSequence[] newEntryValues = new CharSequence[length + 1];
        newEntryValues[0] = "null";
        if (entryValues != null)
            System.arraycopy(entryValues, 0, newEntryValues, 1, length);
        super.setEntryValues(newEntryValues);
    }
}
