package com.github.stenzek.duckstation;

public class HotkeyInfo {
    private String mCategory;
    private String mName;
    private String mDisplayName;

    public HotkeyInfo(String category, String name, String displayName) {
        mCategory = category;
        mName = name;
        mDisplayName = displayName;
    }

    public String getCategory() {
        return mCategory;
    }

    public String getName() {
        return mName;
    }

    public String getDisplayName() {
        return mDisplayName;
    }

    public String getBindingConfigKey() {
        return String.format("Hotkeys/%s", mName);
    }
}
