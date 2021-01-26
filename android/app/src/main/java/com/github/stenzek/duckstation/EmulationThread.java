package com.github.stenzek.duckstation;

import android.os.Build;
import android.os.Process;
import android.util.Log;
import android.view.Surface;

import androidx.annotation.NonNull;

public class EmulationThread extends Thread {
    private EmulationActivity emulationActivity;
    private String filename;
    private boolean resumeState;
    private String stateFilename;

    private EmulationThread(EmulationActivity emulationActivity, String filename, boolean resumeState, String stateFilename) {
        super("EmulationThread");
        this.emulationActivity = emulationActivity;
        this.filename = filename;
        this.resumeState = resumeState;
        this.stateFilename = stateFilename;
    }

    public static EmulationThread create(EmulationActivity emulationActivity, String filename, boolean resumeState, String stateFilename) {
        Log.i("EmulationThread", String.format("Starting emulation thread (%s)...", filename));

        EmulationThread thread = new EmulationThread(emulationActivity, filename, resumeState, stateFilename);
        thread.start();
        return thread;
    }

    private void setExclusiveCores() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                int[] cores = Process.getExclusiveCores();
                if (cores == null || cores.length == 0)
                    throw new Exception("Invalid return value from getExclusiveCores()");

                AndroidHostInterface.setThreadAffinity(cores);
            }
        } catch (Exception e) {
            Log.e("EmulationThread", "getExclusiveCores() failed");
        }
    }

    @Override
    public void run() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_MORE_FAVORABLE);
            setExclusiveCores();
        } catch (Exception e) {
            Log.i("EmulationThread", "Failed to set priority for emulation thread: " + e.getMessage());
        }

        AndroidHostInterface.getInstance().runEmulationThread(emulationActivity, filename, resumeState, stateFilename);
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
