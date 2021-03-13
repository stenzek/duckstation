package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.SharedPreferences;

import androidx.preference.PreferenceManager;

public final class Achievement {
    public static final int CATEGORY_LOCAL = 0;
    public static final int CATEGORY_CORE = 3;
    public static final int CATEGORY_UNOFFICIAL = 5;

    private final int id;
    private final String name;
    private final String description;
    private final String lockedBadgePath;
    private final String unlockedBadgePath;
    private final int points;
    private final boolean locked;

    public Achievement(int id, String name, String description, String lockedBadgePath,
                       String unlockedBadgePath, int points, boolean locked) {
        this.id = id;
        this.name = name;
        this.description = description;
        this.lockedBadgePath = lockedBadgePath;
        this.unlockedBadgePath = unlockedBadgePath;
        this.points = points;
        this.locked = locked;
    }

    /**
     * Returns true if challenge mode will be enabled when a game is started.
     * Does not depend on the emulation running.
     *
     * @param context context to pull settings from
     * @return true if challenge mode will be used, false otherwise
     */
    public static boolean willChallengeModeBeEnabled(Context context) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        return prefs.getBoolean("Cheevos/Enabled", false) &&
               prefs.getBoolean("Cheevos/ChallengeMode", false);
    }

    public int getId() {
        return id;
    }

    public String getName() {
        return name;
    }

    public String getDescription() {
        return description;
    }

    public String getLockedBadgePath() {
        return lockedBadgePath;
    }

    public String getUnlockedBadgePath() {
        return unlockedBadgePath;
    }

    public int getPoints() {
        return points;
    }

    public boolean isLocked() {
        return locked;
    }

    public String getBadgePath() {
        return locked ? lockedBadgePath : unlockedBadgePath;
    }
}
