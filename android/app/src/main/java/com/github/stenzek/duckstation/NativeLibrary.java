package com.github.stenzek.duckstation;

public class NativeLibrary {
    static
    {
        System.loadLibrary("duckstation-native");
    }

    public native boolean createSystem();
    public native boolean bootSystem(String filename, String stateFilename);
    public native void runFrame();
}
