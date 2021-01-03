package com.github.stenzek.duckstation;

import android.content.SharedPreferences;
import android.util.ArraySet;

import java.util.Set;

public class PreferenceHelpers {
    /**
     * Clears all preferences in the specified section (starting with sectionName/).
     * We really don't want to have to do this with JNI...
     *
     * @param prefs       Preferences object.
     * @param sectionName Section to clear keys for.
     */
    public static void clearSection(SharedPreferences prefs, String sectionName) {
        String testSectionName = sectionName + "/";
        SharedPreferences.Editor editor = prefs.edit();
        for (String keyName : prefs.getAll().keySet()) {
            if (keyName.startsWith(testSectionName)) {
                editor.remove(keyName);
            }
        }

        editor.commit();
    }

    public static Set<String> getStringSet(SharedPreferences prefs, String keyName) {
        Set<String> values = null;
        try {
            values = prefs.getStringSet(keyName, null);
        } catch (Exception e) {
            try {
                String singleValue = prefs.getString(keyName, null);
                if (singleValue != null) {
                    values = new ArraySet<>();
                    values.add(singleValue);
                }
            } catch (Exception e2) {

            }
        }

        return values;
    }

    public static boolean addToStringList(SharedPreferences prefs, String keyName, String valueToAdd) {
        Set<String> values = getStringSet(prefs, keyName);
        if (values == null)
            values = new ArraySet<>();

        final boolean result = values.add(valueToAdd);
        prefs.edit().putStringSet(keyName, values).commit();
        return result;
    }

    public static boolean removeFromStringList(SharedPreferences prefs, String keyName, String valueToRemove) {
        Set<String> values = getStringSet(prefs, keyName);
        if (values == null)
            return false;

        final boolean result = values.remove(valueToRemove);
        prefs.edit().putStringSet(keyName, values).commit();
        return result;
    }

    public static void setStringList(SharedPreferences prefs, String keyName, String[] values) {
        Set<String> valueSet = new ArraySet<String>();
        for (String value : values)
            valueSet.add(value);

        prefs.edit().putStringSet(keyName, valueSet).commit();
    }
}
