package com.github.stenzek.duckstation;

public class CheatCode {
    private int mIndex;
    private String mName;
    private boolean mEnabled;

    public CheatCode(int index, String name, boolean enabled) {
        mIndex = index;
        mName = name;
        mEnabled = enabled;
    }

    public int getIndex() { return mIndex; }
    public String getName() { return mName; }
    public boolean isEnabled() { return mEnabled; }
}
