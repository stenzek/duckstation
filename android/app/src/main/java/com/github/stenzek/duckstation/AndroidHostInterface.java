package com.github.stenzek.duckstation;

import android.content.Context;
import android.os.Environment;
import android.util.Log;
import android.view.Surface;

public class AndroidHostInterface
{
    private long nativePointer;

    static public native AndroidHostInterface create(Context context, String userDirectory);

    public AndroidHostInterface(long nativePointer)
    {
        this.nativePointer = nativePointer;
    }

    public native boolean isEmulationThreadRunning();
    public native boolean startEmulationThread(Surface surface, String filename, String state_filename);
    public native void stopEmulationThread();

    public native void surfaceChanged(Surface surface, int format, int width, int height);

    // TODO: Find a better place for this.
    public native void setControllerType(int index, String typeName);
    public native void setControllerButtonState(int index, int buttonCode, boolean pressed);
    public native void setControllerAxisState(int index, int axisCode, float value);
    public static native int getControllerButtonCode(String controllerType, String buttonName);
    public static native int getControllerAxisCode(String controllerType, String axisName);

    public native void refreshGameList(boolean invalidateCache, boolean invalidateDatabase);
    public native GameListEntry[] getGameListEntries();

    public native void resetSystem();
    public native void loadState(boolean global, int slot);
    public native void saveState(boolean global, int slot);
    public native void applySettings();

    static {
        System.loadLibrary("duckstation-native");
    }

    static private AndroidHostInterface mInstance;
    static public boolean createInstance(Context context) {
        // Set user path.
        String externalStorageDirectory = Environment.getExternalStorageDirectory().getAbsolutePath();
        if (externalStorageDirectory.isEmpty())
            externalStorageDirectory = "/sdcard";

        externalStorageDirectory += "/duckstation";
        Log.i("AndroidHostInterface", "User directory: " + externalStorageDirectory);
        mInstance = create(context, externalStorageDirectory);
        return mInstance != null;
    }

    static public boolean hasInstance() {
        return mInstance != null;
    }
    static public AndroidHostInterface getInstance() {
        return mInstance;
    }
}
