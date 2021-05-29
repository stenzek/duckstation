package com.github.stenzek.duckstation;

public class PatchCode {
    private static final String UNGROUPED_NAME = "Ungrouped";

    private int mIndex;
    private String mGroup;
    private String mDescription;
    private boolean mEnabled;

    public PatchCode(int index, String group, String description, boolean enabled) {
        mIndex = index;
        mGroup = group;
        mDescription = description;
        mEnabled = enabled;
    }

    public int getIndex() {
        return mIndex;
    }

    public String getGroup() {
        return mGroup;
    }

    public String getDescription() {
        return mDescription;
    }

    public boolean isEnabled() {
        return mEnabled;
    }

    public String getDisplayText() {
        if (mGroup == null || mGroup.equals(UNGROUPED_NAME))
          return mDescription;
        else
          return String.format("(%s) %s", mGroup, mDescription);
    }
}
