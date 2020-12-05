package com.github.stenzek.duckstation;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.HapticFeedbackConstants;
import android.view.View;

/**
 * TODO: document your custom view class.
 */
public class TouchscreenControllerButtonView extends View {
    private Drawable mUnpressedDrawable;
    private Drawable mPressedDrawable;
    private boolean mPressed = false;
    private boolean mHapticFeedback = false;
    private int mControllerIndex = -1;
    private int mButtonCode = -1;

    public TouchscreenControllerButtonView(Context context) {
        super(context);
        init(context, null, 0);
    }

    public TouchscreenControllerButtonView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init(context, attrs, 0);
    }

    public TouchscreenControllerButtonView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        init(context, attrs, defStyle);
    }

    private void init(Context context, AttributeSet attrs, int defStyle) {
        // Load attributes
        final TypedArray a = getContext().obtainStyledAttributes(
                attrs, R.styleable.TouchscreenControllerButtonView, defStyle, 0);

        if (a.hasValue(R.styleable.TouchscreenControllerButtonView_unpressedDrawable)) {
            mUnpressedDrawable = a.getDrawable(R.styleable.TouchscreenControllerButtonView_unpressedDrawable);
            mUnpressedDrawable.setCallback(this);
        }

        if (a.hasValue(R.styleable.TouchscreenControllerButtonView_pressedDrawable)) {
            mPressedDrawable = a.getDrawable(R.styleable.TouchscreenControllerButtonView_pressedDrawable);
            mPressedDrawable.setCallback(this);
        }

        a.recycle();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        final int paddingLeft = getPaddingLeft();
        final int paddingTop = getPaddingTop();
        final int paddingRight = getPaddingRight();
        final int paddingBottom = getPaddingBottom();
        final int contentWidth = getWidth() - paddingLeft - paddingRight;
        final int contentHeight = getHeight() - paddingTop - paddingBottom;

        // Draw the example drawable on top of the text.
        Drawable drawable = mPressed ? mPressedDrawable : mUnpressedDrawable;
        if (drawable != null) {
            drawable.setBounds(paddingLeft, paddingTop,
                    paddingLeft + contentWidth, paddingTop + contentHeight);
            drawable.draw(canvas);
        }
    }

    public boolean isPressed() {
        return mPressed;
    }

    public void setPressed(boolean pressed) {
        if (pressed == mPressed)
            return;

        mPressed = pressed;
        invalidate();
        updateControllerState();

        if (mHapticFeedback) {
            performHapticFeedback(pressed ? HapticFeedbackConstants.VIRTUAL_KEY : HapticFeedbackConstants.VIRTUAL_KEY_RELEASE);
        }
    }

    public void setButtonCode(int controllerIndex, int code) {
        mControllerIndex = controllerIndex;
        mButtonCode = code;
    }

    public void setHapticFeedback(boolean enabled) {
        mHapticFeedback = enabled;
    }

    private void updateControllerState() {
        if (mButtonCode >= 0)
            AndroidHostInterface.getInstance().setControllerButtonState(mControllerIndex, mButtonCode, mPressed);
    }

    public Drawable getPressedDrawable() {
        return mPressedDrawable;
    }

    public void setPressedDrawable(Drawable pressedDrawable) {
        mPressedDrawable = pressedDrawable;
    }

    public Drawable getUnpressedDrawable() {
        return mUnpressedDrawable;
    }

    public void setUnpressedDrawable(Drawable unpressedDrawable) {
        mUnpressedDrawable = unpressedDrawable;
    }
}
