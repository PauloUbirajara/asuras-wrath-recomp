package com.recomp.asurawrath;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

public class TouchOverlayView extends View {

    public static final int SDL_GAMEPAD_AXIS_LEFTX = 0;
    public static final int SDL_GAMEPAD_AXIS_LEFTY = 1;
    public static final int SDL_GAMEPAD_AXIS_RIGHTX = 2;
    public static final int SDL_GAMEPAD_AXIS_RIGHTY = 3;

    public static final int SDL_GAMEPAD_BUTTON_A = 0;
    public static final int SDL_GAMEPAD_BUTTON_B = 1;
    public static final int SDL_GAMEPAD_BUTTON_X = 2;
    public static final int SDL_GAMEPAD_BUTTON_Y = 3;
    public static final int SDL_GAMEPAD_BUTTON_BACK = 4;
    public static final int SDL_GAMEPAD_BUTTON_START = 6;
    public static final int SDL_GAMEPAD_BUTTON_LEFT_STICK = 7;
    public static final int SDL_GAMEPAD_BUTTON_RIGHT_STICK = 8;
    public static final int SDL_GAMEPAD_BUTTON_LEFT_SHOULDER = 9;
    public static final int SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER = 10;
    public static final int SDL_GAMEPAD_BUTTON_DPAD_UP = 11;
    public static final int SDL_GAMEPAD_BUTTON_DPAD_DOWN = 12;
    public static final int SDL_GAMEPAD_BUTTON_DPAD_LEFT = 13;
    public static final int SDL_GAMEPAD_BUTTON_DPAD_RIGHT = 14;

    private static class ButtonDef {

        final String name;
        final int drawableRes;
        final int sdlButton;
        final float relX;
        final float relY;
        final int sizeDp;

        ButtonDef(
            String name,
            int drawableRes,
            int sdlButton,
            float relX,
            float relY,
            int sizeDp
        ) {
            this.name = name;
            this.drawableRes = drawableRes;
            this.sdlButton = sdlButton;
            this.relX = relX;
            this.relY = relY;
            this.sizeDp = sizeDp;
        }
    }

    private static class ButtonState {

        ButtonDef def;
        Bitmap bitmap;
        final RectF hitRect = new RectF();
        boolean pressed;
    }

    private static class StickState {

        Bitmap bitmap;
        final RectF baseRect = new RectF();
        final RectF bmpDst = new RectF();
        float centerX, centerY;
        float currentX, currentY;
        float radius;
        boolean active;
        int pointerId = -1;
    }

    private final List<ButtonState> buttons = new ArrayList<>();
    private StickState leftStick;
    private StickState rightStick;

    private final Paint bitmapPaint = new Paint(
        Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG
    );
    private final Paint stickBasePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stickKnobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final SparseArray<ButtonState> pointerButtonMap =
        new SparseArray<>();

    private float opacity = 0.5f;
    private boolean layoutDone = false;
    private final int DEVICE_ID = 0;
    private boolean mControllerInitialized = false;

    private void ensureControllerInitialized() {
        if (!mControllerInitialized) {
            AsuraActivity.initVirtualController(DEVICE_ID); // Calls JNI
            mControllerInitialized = true;
        }
    }

    public TouchOverlayView(Context context) {
        super(context);
        init();
    }

    public TouchOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        setWillNotDraw(false);
        setClickable(true);
        setFocusable(false);
        setFocusableInTouchMode(false);

        stickBasePaint.setColor(0x40FFFFFF);
        stickBasePaint.setStyle(Paint.Style.FILL);
        stickKnobPaint.setColor(0x80FFFFFF);
        stickKnobPaint.setStyle(Paint.Style.FILL);

        SharedPreferences prefs = getContext().getSharedPreferences(
            "asura_prefs",
            Context.MODE_PRIVATE
        );
        opacity = prefs.getFloat("touch_overlay_opacity", 0.5f);
        updatePaintAlphas();

        setupButtons();
    }

    public void initVirtualController() {
        AsuraActivity.initVirtualController(DEVICE_ID);
    }

    private void updatePaintAlphas() {
        bitmapPaint.setAlpha((int) (opacity * 255));
        stickBasePaint.setAlpha((int) (opacity * 64));
        stickKnobPaint.setAlpha((int) (opacity * 128));
    }

    public void setOpacity(float alpha) {
        this.opacity = alpha;
        updatePaintAlphas();
        invalidate();
    }

    public void setupButtons() {
        buttons.clear();
        Context ctx = getContext();

        // ABXY Buttons
        addButton(
            "A",
            R.drawable.xbox_button_a,
            SDL_GAMEPAD_BUTTON_A,
            0.88f,
            0.70f,
            48
        );
        addButton(
            "B",
            R.drawable.xbox_button_b,
            SDL_GAMEPAD_BUTTON_B,
            0.95f,
            0.50f,
            48
        );
        addButton(
            "X",
            R.drawable.xbox_button_x,
            SDL_GAMEPAD_BUTTON_X,
            0.81f,
            0.50f,
            48
        );
        addButton(
            "Y",
            R.drawable.xbox_button_y,
            SDL_GAMEPAD_BUTTON_Y,
            0.88f,
            0.30f,
            48
        );

        // Bumpers & Stick Clicks
        addButton(
            "LB",
            R.drawable.xbox_lb,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
            0.05f,
            0.05f,
            56
        );
        addButton(
            "RB",
            R.drawable.xbox_rb,
            SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            0.95f,
            0.05f,
            56
        );
        addButton(
            "LS",
            R.drawable.xbox_lt,
            SDL_GAMEPAD_BUTTON_LEFT_STICK,
            0.15f,
            0.05f,
            56
        );
        addButton(
            "RS",
            R.drawable.xbox_rt,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            0.85f,
            0.05f,
            56
        );

        // Menu Buttons
        addButton(
            "Start",
            R.drawable.xbox_start,
            SDL_GAMEPAD_BUTTON_START,
            0.55f,
            0.90f,
            42
        );
        addButton(
            "Back",
            R.drawable.xbox_back,
            SDL_GAMEPAD_BUTTON_BACK,
            0.45f,
            0.90f,
            42
        );

        leftStick = new StickState();
        try {
            leftStick.bitmap = BitmapFactory.decodeResource(
                ctx.getResources(),
                R.drawable.xbox_stick_left
            );
        } catch (Exception ignored) {}

        rightStick = new StickState();
        try {
            rightStick.bitmap = BitmapFactory.decodeResource(
                ctx.getResources(),
                R.drawable.xbox_stick_right
            );
        } catch (Exception ignored) {}

        if (getWidth() > 0 && getHeight() > 0) {
            layoutButtons(getWidth(), getHeight());
        }
    }

    private void addButton(
        String name,
        int drawableRes,
        int sdlButton,
        float relX,
        float relY,
        int sizeDp
    ) {
        ButtonState bs = new ButtonState();
        bs.def = new ButtonDef(
            name,
            drawableRes,
            sdlButton,
            relX,
            relY,
            sizeDp
        );
        try {
            bs.bitmap = BitmapFactory.decodeResource(
                getContext().getResources(),
                drawableRes
            );
        } catch (Exception ignored) {}
        bs.pressed = false;
        buttons.add(bs);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutButtons(w, h);
    }

    private void layoutButtons(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float density = getResources().getDisplayMetrics().density;

        for (ButtonState bs : buttons) {
            float sizePx = bs.def.sizeDp * density;
            float cx = bs.def.relX * w;
            float cy = bs.def.relY * h;
            bs.hitRect.set(
                cx - sizePx / 2f,
                cy - sizePx / 2f,
                cx + sizePx / 2f,
                cy + sizePx / 2f
            );
        }

        float stickRadius = 60f * density;

        if (leftStick != null) {
            float lsCx = 0.15f * w;
            float lsCy = 0.50f * h;
            leftStick.baseRect.set(
                lsCx - stickRadius,
                lsCy - stickRadius,
                lsCx + stickRadius,
                lsCy + stickRadius
            );
            leftStick.centerX = lsCx;
            leftStick.centerY = lsCy;
            leftStick.currentX = lsCx;
            leftStick.currentY = lsCy;
            leftStick.radius = stickRadius;
        }

        if (rightStick != null) {
            float rsCx = 0.75f * w;
            float rsCy = 0.75f * h;
            rightStick.baseRect.set(
                rsCx - stickRadius,
                rsCy - stickRadius,
                rsCx + stickRadius,
                rsCy + stickRadius
            );
            rightStick.centerX = rsCx;
            rightStick.centerY = rsCy;
            rightStick.currentX = rsCx;
            rightStick.currentY = rsCy;
            rightStick.radius = stickRadius;
        }

        layoutDone = true;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (!layoutDone) return;

        for (int i = 0; i < buttons.size(); i++) {
            ButtonState bs = buttons.get(i);
            if (bs.bitmap == null) continue;
            int alpha = bs.pressed
                ? (int) (opacity * 255 * 0.6f)
                : (int) (opacity * 255);
            bitmapPaint.setAlpha(alpha);
            canvas.drawBitmap(bs.bitmap, null, bs.hitRect, bitmapPaint);
        }

        drawStick(canvas, leftStick);
        drawStick(canvas, rightStick);
    }

    private void drawStick(Canvas canvas, StickState stick) {
        if (stick == null || stick.radius <= 0) return;

        canvas.drawCircle(
            stick.centerX,
            stick.centerY,
            stick.radius,
            stickBasePaint
        );

        float knobRadius = stick.radius * 0.4f;
        canvas.drawCircle(
            stick.currentX,
            stick.currentY,
            knobRadius,
            stickKnobPaint
        );

        if (stick.bitmap != null) {
            float bmpSize = knobRadius * 1.6f;
            stick.bmpDst.set(
                stick.currentX - bmpSize / 2f,
                stick.currentY - bmpSize / 2f,
                stick.currentX + bmpSize / 2f,
                stick.currentY + bmpSize / 2f
            );
            int alpha = stick.active
                ? (int) (opacity * 255 * 0.8f)
                : (int) (opacity * 255);
            bitmapPaint.setAlpha(alpha);
            canvas.drawBitmap(stick.bitmap, null, stick.bmpDst, bitmapPaint);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        ensureControllerInitialized(); // Guarantees SDL event loop is running on native side

        int action = event.getActionMasked();
        int pointerIndex = event.getActionIndex();
        int pointerId = event.getPointerId(pointerIndex);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                handlePointerDown(
                    pointerId,
                    event.getX(pointerIndex),
                    event.getY(pointerIndex)
                );
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int pid = event.getPointerId(i);
                    handlePointerMove(pid, event.getX(i), event.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                handlePointerUp(pointerId);
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
        }

        invalidate();
        return true;
    }

    private void handlePointerDown(int pointerId, float x, float y) {
        if (
            leftStick != null &&
            leftStick.pointerId == -1 &&
            isInStickArea(leftStick, x, y)
        ) {
            leftStick.pointerId = pointerId;
            leftStick.active = true;
            updateStickPosition(leftStick, x, y, true);
            return;
        }
        if (
            rightStick != null &&
            rightStick.pointerId == -1 &&
            isInStickArea(rightStick, x, y)
        ) {
            rightStick.pointerId = pointerId;
            rightStick.active = true;
            updateStickPosition(rightStick, x, y, false);
            return;
        }

        for (int i = 0; i < buttons.size(); i++) {
            ButtonState bs = buttons.get(i);
            if (bs.hitRect.contains(x, y)) {
                bs.pressed = true;
                pointerButtonMap.put(pointerId, bs);
                injectButton(bs.def.sdlButton, true);
                return;
            }
        }
    }

    private void handlePointerMove(int pointerId, float x, float y) {
        if (leftStick != null && leftStick.pointerId == pointerId) {
            updateStickPosition(leftStick, x, y, true);
            return;
        }
        if (rightStick != null && rightStick.pointerId == pointerId) {
            updateStickPosition(rightStick, x, y, false);
            return;
        }
    }

    private void handlePointerUp(int pointerId) {
        if (leftStick != null && leftStick.pointerId == pointerId) {
            resetStick(leftStick, true);
            return;
        }
        if (rightStick != null && rightStick.pointerId == pointerId) {
            resetStick(rightStick, false);
            return;
        }

        ButtonState bs = pointerButtonMap.get(pointerId);
        if (bs != null) {
            pointerButtonMap.remove(pointerId);
            bs.pressed = false;
            injectButton(bs.def.sdlButton, false);
        }
    }

    private void releaseAll() {
        for (int i = 0; i < buttons.size(); i++) {
            ButtonState bs = buttons.get(i);
            if (bs.pressed) {
                bs.pressed = false;
                injectButton(bs.def.sdlButton, false);
            }
        }
        pointerButtonMap.clear();
        if (leftStick != null) resetStick(leftStick, true);
        if (rightStick != null) resetStick(rightStick, false);
    }

    private boolean isInStickArea(StickState stick, float x, float y) {
        float dx = x - stick.centerX;
        float dy = y - stick.centerY;
        float expandedRadius = stick.radius * 1.3f;
        return dx * dx + dy * dy <= expandedRadius * expandedRadius;
    }

    private void updateStickPosition(
        StickState stick,
        float x,
        float y,
        boolean isLeft
    ) {
        float dx = x - stick.centerX;
        float dy = y - stick.centerY;
        float distSq = dx * dx + dy * dy;
        float radiusSq = stick.radius * stick.radius;

        if (distSq > radiusSq) {
            float dist = (float) Math.sqrt(distSq);
            dx = (dx * stick.radius) / dist;
            dy = (dy * stick.radius) / dist;
        }

        stick.currentX = stick.centerX + dx;
        stick.currentY = stick.centerY + dy;

        float normX = dx / stick.radius;
        float normY = dy / stick.radius;

        // Apply radial deadzone (10%)
        float deadZone = 0.10f;
        float magnitude = (float) Math.hypot(normX, normY);

        if (magnitude < deadZone) {
            normX = 0.0f;
            normY = 0.0f;
        } else {
            // Rescale values past deadzone for smooth acceleration
            float factor =
                (magnitude - deadZone) / (1.0f - deadZone) / magnitude;
            normX *= factor;
            normY *= factor;
        }

        if (isLeft) {
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_LEFTX, normX);
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_LEFTY, normY);
        } else {
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_RIGHTX, normX);
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_RIGHTY, normY);
        }
    }

    private void resetStick(StickState stick, boolean isLeft) {
        stick.currentX = stick.centerX;
        stick.currentY = stick.centerY;
        stick.active = false;
        stick.pointerId = -1;

        if (isLeft) {
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_LEFTX, 0.0f);
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_LEFTY, 0.0f);
        } else {
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_RIGHTX, 0.0f);
            AsuraActivity.sendAxis(DEVICE_ID, SDL_GAMEPAD_AXIS_RIGHTY, 0.0f);
        }
    }

    private void injectButton(int button, boolean pressed) {
        AsuraActivity.sendButton(DEVICE_ID, button, pressed);
    }
}
