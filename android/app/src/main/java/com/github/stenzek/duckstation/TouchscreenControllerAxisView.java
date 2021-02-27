package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

public final class TouchscreenControllerAxisView extends View {
    private Drawable mBaseDrawable;
    private Drawable mStickUnpressedDrawable;
    private Drawable mStickPressedDrawable;
    private boolean mPressed = false;
    private int mPointerId = 0;
    private float mXValue = 0.0f;
    private float mYValue = 0.0f;
    private int mDrawXPos = 0;
    private int mDrawYPos = 0;

    private String mConfigName;
    private boolean mDefaultVisibility = true;

    private int mControllerIndex = -1;
    private int mXAxisCode = -1;
    private int mYAxisCode = -1;
    private int mLeftButtonCode = -1;
    private int mRightButtonCode = -1;
    private int mUpButtonCode = -1;
    private int mDownButtonCode = -1;

    public TouchscreenControllerAxisView(Context context) {
        super(context);
        init();
    }

    public TouchscreenControllerAxisView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public TouchscreenControllerAxisView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        init();
    }

    private void init() {
        mBaseDrawable = getContext().getDrawable(R.drawable.ic_controller_analog_base);
        mBaseDrawable.setCallback(this);
        mStickUnpressedDrawable = getContext().getDrawable(R.drawable.ic_controller_analog_stick_unpressed);
        mStickUnpressedDrawable.setCallback(this);
        mStickPressedDrawable = getContext().getDrawable(R.drawable.ic_controller_analog_stick_pressed);
        mStickPressedDrawable.setCallback(this);
    }

    public String getConfigName() {
        return mConfigName;
    }
    public void setConfigName(String configName) {
        mConfigName = configName;
    }

    public boolean getDefaultVisibility() { return mDefaultVisibility; }
    public void setDefaultVisibility(boolean visibility) { mDefaultVisibility = visibility; }

    public void setControllerAxis(int controllerIndex, int xCode, int yCode) {
        mControllerIndex = controllerIndex;
        mXAxisCode = xCode;
        mYAxisCode = yCode;
        mLeftButtonCode = -1;
        mRightButtonCode = -1;
        mUpButtonCode = -1;
        mDownButtonCode = -1;
    }

    public void setControllerButtons(int controllerIndex, int leftCode, int rightCode, int upCode, int downCode) {
        mControllerIndex = controllerIndex;
        mXAxisCode = -1;
        mYAxisCode = -1;
        mLeftButtonCode = leftCode;
        mRightButtonCode = rightCode;
        mUpButtonCode = upCode;
        mDownButtonCode = downCode;
    }

    public void setUnpressed() {
        if (!mPressed && mXValue == 0.0f && mYValue == 0.0f)
            return;

        mPressed = false;
        mXValue = 0.0f;
        mYValue = 0.0f;
        mDrawXPos = 0;
        mDrawYPos = 0;
        invalidate();
        updateControllerState();
    }

    public void setPressed(int pointerId, float pointerX, float pointerY) {
        final float dx = pointerX - (float) (getX() + (float) (getWidth() / 2));
        final float dy = pointerY - (float) (getY() + (float) (getHeight() / 2));
        // Log.i("SetPressed", String.format("px=%f,py=%f dx=%f,dy=%f", pointerX, pointerY, dx, dy));

        final float pointerDistance = Math.max(Math.abs(dx), Math.abs(dy));
        final float angle = (float) Math.atan2((double) dy, (double) dx);

        final float maxDistance = (float) Math.min((getWidth() - getPaddingLeft() - getPaddingRight()) / 2, (getHeight() - getPaddingTop() - getPaddingBottom()) / 2);
        final float length = Math.min(pointerDistance / maxDistance, 1.0f);
        // Log.i("SetPressed", String.format("pointerDist=%f,angle=%f,w=%d,h=%d,maxDist=%f,length=%f", pointerDistance, angle, getWidth(), getHeight(), maxDistance, length));

        final float xValue = (float) Math.cos((double) angle) * length;
        final float yValue = (float) Math.sin((double) angle) * length;
        mDrawXPos = (int) (xValue * maxDistance);
        mDrawYPos = (int) (yValue * maxDistance);

        boolean doUpdate = (pointerId != mPointerId || !mPressed || (xValue != mXValue || yValue != mYValue));
        mPointerId = pointerId;
        mPressed = true;
        mXValue = xValue;
        mYValue = yValue;
        // Log.i("SetPressed", String.format("xval=%f,yval=%f,drawX=%d,drawY=%d", mXValue, mYValue, mDrawXPos, mDrawYPos));

        if (doUpdate) {
            invalidate();
            updateControllerState();
        }
    }

    private void updateControllerState() {
        final float BUTTON_THRESHOLD = 0.33f;

        AndroidHostInterface hostInterface = AndroidHostInterface.getInstance();
        if (mXAxisCode >= 0)
            hostInterface.setControllerAxisState(mControllerIndex, mXAxisCode, mXValue);
        if (mYAxisCode >= 0)
            hostInterface.setControllerAxisState(mControllerIndex, mYAxisCode, mYValue);

        if (mLeftButtonCode >= 0)
            hostInterface.setControllerButtonState(mControllerIndex, mLeftButtonCode, (mXValue <= -BUTTON_THRESHOLD));
        if (mRightButtonCode >= 0)
            hostInterface.setControllerButtonState(mControllerIndex, mRightButtonCode, (mXValue >= BUTTON_THRESHOLD));
        if (mUpButtonCode >= 0)
            hostInterface.setControllerButtonState(mControllerIndex, mUpButtonCode, (mYValue <= -BUTTON_THRESHOLD));
        if (mDownButtonCode >= 0)
            hostInterface.setControllerButtonState(mControllerIndex, mDownButtonCode, (mYValue >= BUTTON_THRESHOLD));
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

        mBaseDrawable.setBounds(paddingLeft, paddingTop,
                paddingLeft + contentWidth, paddingTop + contentHeight);
        mBaseDrawable.draw(canvas);

        final int stickWidth = contentWidth / 3;
        final int stickHeight = contentHeight / 3;
        final int halfStickWidth = stickWidth / 2;
        final int halfStickHeight = stickHeight / 2;
        final int centerX = getWidth() / 2;
        final int centerY = getHeight() / 2;
        final int drawX = centerX + mDrawXPos;
        final int drawY = centerY + mDrawYPos;

        Drawable stickDrawable = mPressed ? mStickPressedDrawable : mStickUnpressedDrawable;
        stickDrawable.setBounds(drawX - halfStickWidth, drawY - halfStickHeight, drawX + halfStickWidth, drawY + halfStickHeight);
        stickDrawable.draw(canvas);
    }

    public boolean isPressed() {
        return mPressed;
    }

    public boolean hasPointerId() {
        return mPointerId >= 0;
    }

    public int getPointerId() {
        return mPointerId;
    }

    public void setPointerId(int mPointerId) {
        this.mPointerId = mPointerId;
    }
}
