package com.github.stenzek.duckstation;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

public final class TouchscreenControllerDPadView extends View {
    private static final int NUM_DIRECTIONS = 4;
    private static final int NUM_POSITIONS = 8;
    private static final int DIRECTION_UP = 0;
    private static final int DIRECTION_RIGHT = 1;
    private static final int DIRECTION_DOWN = 2;
    private static final int DIRECTION_LEFT = 3;

    private final Drawable[] mUnpressedDrawables = new Drawable[NUM_DIRECTIONS];
    private final Drawable[] mPressedDrawables = new Drawable[NUM_DIRECTIONS];
    private final int[] mDirectionCodes = new int[] { -1, -1, -1, -1 };
    private final boolean[] mDirectionStates = new boolean[NUM_DIRECTIONS];

    private boolean mPressed = false;
    private int mPointerId = 0;
    private int mPointerX = 0;
    private int mPointerY = 0;

    private String mConfigName;
    private boolean mDefaultVisibility = true;

    private int mControllerIndex = -1;

    private static final int[][] DRAWABLES = {
            {R.drawable.ic_controller_up_button,R.drawable.ic_controller_up_button_pressed},
            {R.drawable.ic_controller_right_button,R.drawable.ic_controller_right_button_pressed},
            {R.drawable.ic_controller_down_button,R.drawable.ic_controller_down_button_pressed},
            {R.drawable.ic_controller_left_button,R.drawable.ic_controller_left_button_pressed},
    };


    public TouchscreenControllerDPadView(Context context) {
        super(context);
        init();
    }

    public TouchscreenControllerDPadView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public TouchscreenControllerDPadView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        init();
    }

    private void init() {
        for (int i = 0; i < NUM_DIRECTIONS; i++) {
            mUnpressedDrawables[i] = getContext().getDrawable(DRAWABLES[i][0]);
            mPressedDrawables[i] = getContext().getDrawable(DRAWABLES[i][1]);
        }
    }

    public String getConfigName() {
        return mConfigName;
    }
    public void setConfigName(String configName) {
        mConfigName = configName;
    }

    public boolean getDefaultVisibility() { return mDefaultVisibility; }
    public void setDefaultVisibility(boolean visibility) { mDefaultVisibility = visibility; }

    public void setControllerButtons(int controllerIndex, int leftCode, int rightCode, int upCode, int downCode) {
        mControllerIndex = controllerIndex;
        mDirectionCodes[DIRECTION_LEFT] = leftCode;
        mDirectionCodes[DIRECTION_RIGHT] = rightCode;
        mDirectionCodes[DIRECTION_UP] = upCode;
        mDirectionCodes[DIRECTION_DOWN] = downCode;
    }

    public void setUnpressed() {
        if (!mPressed && mPointerX == 0 && mPointerY == 0)
            return;

        mPressed = false;
        mPointerX = 0;
        mPointerY = 0;
        updateControllerState();
        invalidate();
    }

    public void setPressed(int pointerId, float pointerX, float pointerY) {
        final int posX = (int)(pointerX - getX());
        final int posY = (int)(pointerY - getY());

        boolean doUpdate = (pointerId != mPointerId || !mPressed || (posX != mPointerX || posY != mPointerY));
        mPointerId = pointerId;
        mPointerX = posX;
        mPointerY = posY;
        mPressed = true;

        if (doUpdate) {
            updateControllerState();
            invalidate();
        }
    }

    private void updateControllerState() {
        final int subX = mPointerX / (getWidth() / 3);
        final int subY = mPointerY / (getWidth() / 3);

        mDirectionStates[DIRECTION_UP] = (mPressed && subY == 0);
        mDirectionStates[DIRECTION_RIGHT] = (mPressed && subX == 2);
        mDirectionStates[DIRECTION_DOWN] = (mPressed && subY == 2);
        mDirectionStates[DIRECTION_LEFT] = (mPressed && subX == 0);

        AndroidHostInterface hostInterface = AndroidHostInterface.getInstance();
        for (int i = 0; i < NUM_DIRECTIONS; i++) {
            if (mDirectionCodes[i] >= 0)
                hostInterface.setControllerButtonState(mControllerIndex, mDirectionCodes[i], mDirectionStates[i]);
        }
    }

    private void drawDirection(int direction, int subX, int subY, Canvas canvas, int buttonWidth, int buttonHeight) {
        final int leftBounds = subX * buttonWidth;
        final int rightBounds = leftBounds + buttonWidth;
        final int topBounds = subY * buttonHeight;
        final int bottomBounds = topBounds + buttonHeight;

        final Drawable drawable = mDirectionStates[direction] ? mPressedDrawables[direction] : mUnpressedDrawables[direction];
        drawable.setBounds(leftBounds, topBounds, rightBounds, bottomBounds);
        drawable.draw(canvas);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        final int width = getWidth();
        final int height = getHeight();

        // Divide it into thirds - draw between.
        final int buttonWidth = width / 3;
        final int buttonHeight = height / 3;

        drawDirection(DIRECTION_UP, 1, 0, canvas, buttonWidth, buttonHeight);
        drawDirection(DIRECTION_RIGHT, 2, 1, canvas, buttonWidth, buttonHeight);
        drawDirection(DIRECTION_DOWN, 1, 2, canvas, buttonWidth, buttonHeight);
        drawDirection(DIRECTION_LEFT, 0, 1, canvas, buttonWidth, buttonHeight);
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
