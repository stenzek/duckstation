package com.github.stenzek.duckstation;

import android.app.Activity;
import android.app.ProgressDialog;

import androidx.appcompat.app.AlertDialog;

public class AndroidProgressCallback {
    private Activity mContext;
    private ProgressDialog mDialog;

    public AndroidProgressCallback(Activity context) {
        mContext = context;
        mDialog = new ProgressDialog(context);
        mDialog.setCancelable(false);
        mDialog.setCanceledOnTouchOutside(false);
        mDialog.setMessage(context.getString(R.string.android_progress_callback_please_wait));
        mDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        mDialog.setIndeterminate(false);
        mDialog.setMax(100);
        mDialog.setProgress(0);
        mDialog.show();
    }

    public void dismiss() {
        mDialog.dismiss();
    }

    public void setTitle(String text) {
        mContext.runOnUiThread(() -> {
            mDialog.setTitle(text);
        });
    }

    public void setStatusText(String text) {
        mContext.runOnUiThread(() -> {
            mDialog.setMessage(text);
        });
    }

    public void setProgressRange(int range) {
        mContext.runOnUiThread(() -> {
            mDialog.setMax(range);
        });
    }

    public void setProgressValue(int value) {
        mContext.runOnUiThread(() -> {
            mDialog.setProgress(value);
        });
    }

    public void modalError(String message) {
        Object lock = new Object();
        mContext.runOnUiThread(() -> {
            new AlertDialog.Builder(mContext)
                    .setTitle("Error")
                    .setMessage(message)
                    .setPositiveButton(mContext.getString(R.string.android_progress_callback_ok), (dialog, button) -> {
                        dialog.dismiss();
                    })
                    .setOnDismissListener((dialogInterface) -> {
                        synchronized (lock) {
                            lock.notify();
                        }
                    })
                    .create()
                    .show();
        });

        synchronized (lock) {
            try {
                lock.wait();
            } catch (InterruptedException e) {
            }
        }
    }

    public void modalInformation(String message) {
        Object lock = new Object();
        mContext.runOnUiThread(() -> {
            new AlertDialog.Builder(mContext)
                    .setTitle(mContext.getString(R.string.android_progress_callback_information))
                    .setMessage(message)
                    .setPositiveButton(mContext.getString(R.string.android_progress_callback_ok), (dialog, button) -> {
                        dialog.dismiss();
                    })
                    .setOnDismissListener((dialogInterface) -> {
                        synchronized (lock) {
                            lock.notify();
                        }
                    })
                    .create()
                    .show();
        });

        synchronized (lock) {
            try {
                lock.wait();
            } catch (InterruptedException e) {
            }
        }
    }

    private class ConfirmationResult {
        public boolean result = false;
    }

    public boolean modalConfirmation(String message) {
        ConfirmationResult result = new ConfirmationResult();
        mContext.runOnUiThread(() -> {
            new AlertDialog.Builder(mContext)
                    .setTitle(mContext.getString(R.string.android_progress_callback_confirmation))
                    .setMessage(message)
                    .setPositiveButton(mContext.getString(R.string.android_progress_callback_yes), (dialog, button) -> {
                        result.result = true;
                        dialog.dismiss();
                    })
                    .setNegativeButton(mContext.getString(R.string.android_progress_callback_no), (dialog, button) -> {
                        result.result = false;
                        dialog.dismiss();
                    })
                    .setOnDismissListener((dialogInterface) -> {
                        synchronized (result) {
                            result.notify();
                        }
                    })
                    .create()
                    .show();
        });

        synchronized (result) {
            try {
                result.wait();
            } catch (InterruptedException e) {
            }
        }

        return result.result;
    }
}
