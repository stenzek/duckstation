package com.github.stenzek.duckstation;

import android.graphics.Bitmap;

import java.nio.ByteBuffer;

public class MemoryCardFileInfo {
    public static final int ICON_WIDTH = 16;
    public static final int ICON_HEIGHT = 16;

    private final String filename;
    private final String title;
    private final int size;
    private final int firstBlock;
    private final int numBlocks;
    private final byte[][] iconFrames;

    public MemoryCardFileInfo(String filename, String title, int size, int firstBlock, int numBlocks, byte[][] iconFrames) {
        this.filename = filename;
        this.title = title;
        this.size = size;
        this.firstBlock = firstBlock;
        this.numBlocks = numBlocks;
        this.iconFrames = iconFrames;
    }

    public String getFilename() {
        return filename;
    }

    public String getTitle() {
        return title;
    }

    public int getSize() {
        return size;
    }

    public int getFirstBlock() {
        return firstBlock;
    }

    public int getNumBlocks() {
        return numBlocks;
    }

    public int getNumIconFrames() {
        return (iconFrames != null) ? iconFrames.length : 0;
    }

    public byte[] getIconFrame(int index) {
        return iconFrames[index];
    }

    public Bitmap getIconFrameBitmap(int index) {
        final byte[] data = getIconFrame(index);
        if (data == null)
            return null;

        final Bitmap bitmap = Bitmap.createBitmap(ICON_WIDTH, ICON_HEIGHT, Bitmap.Config.ARGB_8888);
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(data));
        return bitmap;
    }
}
