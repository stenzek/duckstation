package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.AsyncTask;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.widget.ImageView;

import java.lang.ref.WeakReference;

public class GenerateCoverTask extends AsyncTask<Void, Void, Bitmap> {
    private final Context mContext;
    private final WeakReference<ImageView> mView;
    private final String mTitle;

    public GenerateCoverTask(Context context, ImageView view, String title) {
        mContext = context;
        mView = new WeakReference<>(view);
        mTitle = title;
    }

    @Override
    protected Bitmap doInBackground(Void... voids) {
        try {
            final Bitmap background = BitmapFactory.decodeResource(mContext.getResources(), R.drawable.cover_placeholder);
            if (background == null)
                return null;

            final Bitmap bitmap = Bitmap.createBitmap(background.getWidth(), background.getHeight(), background.getConfig());
            final Canvas canvas = new Canvas(bitmap);
            final TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
            canvas.drawBitmap(background, 0.0f, 0.0f, paint);

            paint.setColor(Color.rgb(255, 255, 255));
            paint.setTextSize(100);
            paint.setShadowLayer(1.0f, 0.0f, 1.0f, Color.DKGRAY);
            paint.setTextAlign(Paint.Align.CENTER);

            final StaticLayout staticLayout = new StaticLayout(mTitle, paint,
                    canvas.getWidth(), Layout.Alignment.ALIGN_NORMAL, 1, 0, false);
            canvas.save();
            canvas.translate(canvas.getWidth() / 2, (canvas.getHeight() / 2) - (staticLayout.getHeight() / 2));
            staticLayout.draw(canvas);
            canvas.restore();
            return bitmap;
        } catch (Exception e) {
            e.printStackTrace();
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
