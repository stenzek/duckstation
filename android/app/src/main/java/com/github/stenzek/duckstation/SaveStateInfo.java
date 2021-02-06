package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageView;
import android.widget.TextView;

import java.nio.ByteBuffer;

public class SaveStateInfo {
    private String mPath;
    private String mGameTitle;
    private String mGameCode;
    private String mMediaPath;
    private String mTimestamp;
    private int mSlot;
    private boolean mGlobal;
    private Bitmap mScreenshot;

    public SaveStateInfo(String path, String gameTitle, String gameCode, String mediaPath, String timestamp, int slot, boolean global,
                         int screenshotWidth, int screenshotHeight, byte[] screenshotData) {
        mPath = path;
        mGameTitle = gameTitle;
        mGameCode = gameCode;
        mMediaPath = mediaPath;
        mTimestamp = timestamp;
        mSlot = slot;
        mGlobal = global;

        if (screenshotData != null) {
            try {
                mScreenshot = Bitmap.createBitmap(screenshotWidth, screenshotHeight, Bitmap.Config.ARGB_8888);
                mScreenshot.copyPixelsFromBuffer(ByteBuffer.wrap(screenshotData));
            } catch (Exception e) {
                mScreenshot = null;
            }
        }
    }

    public boolean exists() {
        return mPath != null;
    }

    public String getPath() {
        return mPath;
    }

    public String getGameTitle() {
        return mGameTitle;
    }

    public String getGameCode() {
        return mGameCode;
    }

    public String getMediaPath() {
        return mMediaPath;
    }

    public String getTimestamp() {
        return mTimestamp;
    }

    public int getSlot() {
        return mSlot;
    }

    public boolean isGlobal() {
        return mGlobal;
    }

    public Bitmap getScreenshot() {
        return mScreenshot;
    }

    private void fillView(Context context, View view) {
        ImageView imageView = (ImageView) view.findViewById(R.id.image);
        TextView summaryView = (TextView) view.findViewById(R.id.summary);
        TextView gameView = (TextView) view.findViewById(R.id.game);
        TextView pathView = (TextView) view.findViewById(R.id.path);
        TextView timestampView = (TextView) view.findViewById(R.id.timestamp);

        if (mScreenshot != null)
            imageView.setImageBitmap(mScreenshot);
        else
            imageView.setImageDrawable(context.getDrawable(R.drawable.ic_baseline_not_interested_60));

        String summaryText;
        if (mGlobal)
            summaryView.setText(String.format(context.getString(R.string.save_state_info_global_save_n), mSlot));
        else if (mSlot == 0)
            summaryView.setText(R.string.save_state_info_quick_save);
        else
            summaryView.setText(String.format(context.getString(R.string.save_state_info_game_save_n), mSlot));

        if (exists()) {
            gameView.setText(String.format("%s - %s", mGameCode, mGameTitle));

            int lastSlashPosition = mMediaPath.lastIndexOf('/');
            if (lastSlashPosition >= 0)
                pathView.setText(mMediaPath.substring(lastSlashPosition + 1));
            else
                pathView.setText(mMediaPath);

            timestampView.setText(mTimestamp);
        } else {
            gameView.setText(R.string.save_state_info_slot_is_empty);
            pathView.setText("");
            timestampView.setText("");
        }
    }

    public static class ListAdapter extends BaseAdapter {
        private final Context mContext;
        private final SaveStateInfo[] mInfos;

        public ListAdapter(Context context, SaveStateInfo[] infos) {
            mContext = context;
            mInfos = infos;
        }

        @Override
        public int getCount() {
            return mInfos.length;
        }

        @Override
        public Object getItem(int position) {
            return mInfos[position];
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(mContext).inflate(R.layout.save_state_view_entry, parent, false);
            }

            mInfos[position].fillView(mContext, convertView);
            return convertView;
        }
    }
}
