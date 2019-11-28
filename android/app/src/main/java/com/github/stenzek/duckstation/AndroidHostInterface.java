package com.github.stenzek.duckstation;

import android.view.Surface;

public class AndroidHostInterface
{
    private long nativePointer;

    static {
        System.loadLibrary("duckstation-native");
    }

    static public native AndroidHostInterface create();

    public AndroidHostInterface(long nativePointer)
    {
        this.nativePointer = nativePointer;
    }

    public native boolean isEmulationThreadRunning();
    public native boolean startEmulationThread(Surface surface, String filename, String state_filename);
    public native void stopEmulationThread();

    public native void surfaceChanged(Surface surface, int format, int width, int height);
}
