package com.github.stenzek.duckstation;

public class PatchCode {
    private int mIndex;
    private String mDescription;
    private boolean mEnabled;

    public PatchCode(int index, String description, boolean enabled) {
        mIndex = index;
        mDescription = description;
        mEnabled = enabled;
    }

    public int getIndex() {
        return mIndex;
    }

    public String getDescription() {
        return mDescription;
    }

    public boolean isEnabled() {
        return mEnabled;
    }
}
