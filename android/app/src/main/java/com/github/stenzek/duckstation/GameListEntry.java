package com.github.stenzek.duckstation;

import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

public class GameListEntry {
    private String mPath;
    private String mCode;
    private String mTitle;
    private ConsoleRegion mRegion;
    private long mSize;

    public GameListEntry(String path, String code, String title, String region, long size) {
        mPath = path;
        mCode = code;
        mTitle = title;
        mSize = size;

        try {
            mRegion = ConsoleRegion.valueOf(region);
        } catch (IllegalArgumentException e) {
            mRegion = ConsoleRegion.NTSC_U;
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

    public ConsoleRegion getRegion() {
        return mRegion;
    }

    public void fillView(View view) {
        ((TextView) view.findViewById(R.id.game_list_view_entry_title)).setText(mTitle);
        ((TextView) view.findViewById(R.id.game_list_view_entry_path)).setText(mPath);

        String sizeString = String.format("%.2f MB", (double) mSize / 1048576.0);
        ((TextView) view.findViewById(R.id.game_list_view_entry_size)).setText(sizeString);

        int drawableId;
        switch (mRegion) {
            case NTSC_J:
                drawableId = R.drawable.flag_jp;
                break;
            case NTSC_U:
            default:
                drawableId = R.drawable.flag_us;
                break;
            case PAL:
                drawableId = R.drawable.flag_eu;
                break;
        }

        ((ImageView) view.findViewById(R.id.game_list_view_entry_region_icon))
                .setImageDrawable(ContextCompat.getDrawable(view.getContext(), drawableId));
    }
}
