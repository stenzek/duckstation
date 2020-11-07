package com.github.stenzek.duckstation;

import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

public class GameListEntry {
    public enum EntryType {
        Disc,
        PSExe,
        Playlist
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


    public GameListEntry(String path, String code, String title, String fileTitle, long size, String modifiedTime, String region,
                         String type, String compatibilityRating) {
        mPath = path;
        mCode = code;
        mTitle = title;
        mFileTitle = fileTitle;
        mSize = size;
        mModifiedTime = modifiedTime;

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

    public String getFileTitle() { return mFileTitle; }

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

    public static String getFileNameForPath(String path) {
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0 && lastSlash < path.length() - 1)
            return path.substring(lastSlash + 1);
        else
            return path;
    }

    private String getSubTitle() {
        String fileName = getFileNameForPath(mPath);
        String sizeString = String.format("%.2f MB", (double) mSize / 1048576.0);
        return String.format("%s (%s)", fileName, sizeString);
    }

    public void fillView(View view) {
        ((TextView) view.findViewById(R.id.game_list_view_entry_title)).setText(mTitle);
        ((TextView) view.findViewById(R.id.game_list_view_entry_subtitle)).setText(getSubTitle());
        ((TextView) view.findViewById(R.id.game_list_view_entry_code)).setText(mCode);

        int regionDrawableId;
        switch (mRegion) {
            case NTSC_J:
                regionDrawableId = R.drawable.flag_jp;
                break;
            case NTSC_U:
            default:
                regionDrawableId = R.drawable.flag_us;
                break;
            case PAL:
                regionDrawableId = R.drawable.flag_eu;
                break;
        }

        ((ImageView) view.findViewById(R.id.game_list_view_entry_region_icon))
                .setImageDrawable(ContextCompat.getDrawable(view.getContext(), regionDrawableId));

        int typeDrawableId;
        switch (mType) {
            case Disc:
            default:
                typeDrawableId = R.drawable.ic_media_cdrom;
                break;

            case PSExe:
                typeDrawableId = R.drawable.ic_emblem_system;
                break;

            case Playlist:
                typeDrawableId = R.drawable.ic_baseline_playlist_play_24;
                break;
        }

        ((ImageView) view.findViewById(R.id.game_list_view_entry_type_icon))
                .setImageDrawable(ContextCompat.getDrawable(view.getContext(), typeDrawableId));
    }
}
