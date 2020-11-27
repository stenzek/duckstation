package com.github.stenzek.duckstation;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.AsyncTask;
import android.widget.ImageView;

import java.lang.ref.WeakReference;

public class ImageLoadTask extends AsyncTask<String, Void, Bitmap> {
    private WeakReference<ImageView> mView;

    public ImageLoadTask(ImageView view) {
        mView = new WeakReference<>(view);
    }

    @Override
    protected Bitmap doInBackground(String... strings) {
        try {
            return BitmapFactory.decodeFile(strings[0]);
        } catch (Exception e) {
            return null;
        }
    }

    @Override
    protected void onPostExecute(Bitmap bitmap) {
        ImageView iv = mView.get();
        if (iv != null)
            iv.setImageBitmap(bitmap);
    }
}
