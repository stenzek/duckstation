package com.github.stenzek.duckstation;

import android.os.Process;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.NonNull;

public class EmulationThread extends Thread {
    private EmulationActivity emulationActivity;
    private Surface surface;
    private String filename;
    private boolean resumeState;
    private String stateFilename;

    private EmulationThread(EmulationActivity emulationActivity, Surface surface, String filename, boolean resumeState, String stateFilename) {
        super("EmulationThread");
        this.emulationActivity = emulationActivity;
        this.surface = surface;
        this.filename = filename;
        this.resumeState = resumeState;
        this.stateFilename = stateFilename;
    }

    public static EmulationThread create(EmulationActivity emulationActivity, Surface surface, String filename, boolean resumeState, String stateFilename) {
        Log.i("EmulationThread", String.format("Starting emulation thread (%s)...", filename));

        EmulationThread thread = new EmulationThread(emulationActivity, surface, filename, resumeState, stateFilename);
        thread.start();
        return thread;
    }

    @Override
    public void run() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);
        } catch (Exception e) {
            Log.i("EmulationThread", "Failed to set priority for emulation thread: " + e.getMessage());
        }

        AndroidHostInterface.getInstance().runEmulationThread(emulationActivity, surface, filename, resumeState, stateFilename);
        Log.i("EmulationThread", "Emulation thread exiting.");
    }

    public void stopAndJoin() {
        AndroidHostInterface.getInstance().stopEmulationThreadLoop();
        try {
            join();
        } catch (InterruptedException e) {
            Log.i("EmulationThread", "join() interrupted: " + e.getMessage());
        }
    }
}
