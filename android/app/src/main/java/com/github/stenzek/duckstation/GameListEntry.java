package com.github.stenzek.duckstation;

import android.os.AsyncTask;
import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

public class GameListEntry {
    public enum EntryType {
        Disc,
        PSExe,
        Playlist,
        PSF
    }

    public enum CompatibilityRating {
        Unknown,
        DoesntBoot,
        CrashesInIntro,
        CrashesInGame,
        GraphicalAudioIssues,
        NoIssues,
    }

    private String mPath;
    private String mCode;
    private String mTitle;
    private String mFileTitle;
    private long mSize;
    private String mModifiedTime;
    private DiscRegion mRegion;
    private EntryType mType;
    private CompatibilityRating mCompatibilityRating;
    private String mCoverPath;

    public GameListEntry(String path, String code, String title, String fileTitle, long size, String modifiedTime, String region,
                         String type, String compatibilityRating, String coverPath) {
        mPath = path;
        mCode = code;
        mTitle = title;
        mFileTitle = fileTitle;
        mSize = size;
        mModifiedTime = modifiedTime;
        mCoverPath = coverPath;

        try {
            mRegion = DiscRegion.valueOf(region);
        } catch (IllegalArgumentException e) {
            mRegion = DiscRegion.NTSC_U;
        }

        try {
            mType = EntryType.valueOf(type);
        } catch (IllegalArgumentException e) {
            mType = EntryType.Disc;
        }

        try {
            mCompatibilityRating = CompatibilityRating.valueOf(compatibilityRating);
        } catch (IllegalArgumentException e) {
            mCompatibilityRating = CompatibilityRating.Unknown;
        }
    }

    public String getPath() {
        return mPath;
    }

    public String getCode() {
        return mCode;
    }

    public String getTitle() {
        return mTitle;
    }

    public String getFileTitle() {
        return mFileTitle;
    }

    public long getSize() { return mSize; }

    public String getModifiedTime() {
        return mModifiedTime;
    }

    public DiscRegion getRegion() {
        return mRegion;
    }

    public EntryType getType() {
        return mType;
    }

    public CompatibilityRating getCompatibilityRating() {
        return mCompatibilityRating;
    }

    public String getCoverPath() { return mCoverPath; }

    public void setCoverPath(String coverPath) { mCoverPath = coverPath; }

    public static String getFileNameForPath(String path) {
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0 && lastSlash < path.length() - 1)
            return path.substring(lastSlash + 1);
        else
            return path;
    }
}
