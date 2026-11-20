package org.asurawrath.recompiled

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * On-screen controls over the game (GameActivity): two sticks, A B X Y, the
 * bumpers and triggers, Back and Start. They feed a pad of their own in the
 * game (pad_remap.cpp TouchPadDriver, through GameActivity.nativeTouchPad),
 * which plays alongside a real controller and goes through the same remap
 * and Turbo.
 *
 * launcher.cfg (Controls › Touch controls):
 *   touch_show            1: shown over the game (default)
 *   touch_hide_with_pad   1: hidden while a controller is connected (default)
 *   touch_size            percent (100: buttons 64 dp, sticks 132 dp across)
 *   touch_opacity         percent
 *
 * The view takes every touch while shown, and hides while the game's own
 * (ImGui) menu is open, which takes touches as the mouse.
 *
 * Button art: kenney.nl input prompts (CC0).
 */
class TouchControlsView(context: Context) : View(context) {

    // PadControl (src/input/pad_remap.h): the bit each control sets.
    private object Pad {
        const val A = 0; const val B = 1; const val X = 2; const val Y = 3
        const val LB = 4; const val RB = 5; const val LT = 6; const val RT = 7
        const val BACK = 8; const val START = 9
    }

    /** A button: its control, art, centre (fraction of the view) and size (dp at 100%). */
    private class Button(val control: Int, art: Int, val fx: Float, val fy: Float, val sizeDp: Float,
                         context: Context) {
        val bitmap: Bitmap? = BitmapFactory.decodeResource(context.resources, art)
        val rect = RectF()
        var pressed = false
    }

    private class Stick(art: Int, val fx: Float, val fy: Float, context: Context) {
        val bitmap: Bitmap? = BitmapFactory.decodeResource(context.resources, art)
        var cx = 0f; var cy = 0f; var radius = 0f
        var x = 0f; var y = 0f        // -1..1, screen axes (y down)
        var pointer = -1
    }

    private val buttons = listOf(
        Button(Pad.A, R.drawable.touch_button_a, 0.875f, 0.70f, 64f, context),
        Button(Pad.B, R.drawable.touch_button_b, 0.945f, 0.52f, 64f, context),
        Button(Pad.X, R.drawable.touch_button_x, 0.805f, 0.52f, 64f, context),
        Button(Pad.Y, R.drawable.touch_button_y, 0.875f, 0.34f, 64f, context),
        Button(Pad.LT, R.drawable.touch_lt, 0.06f, 0.12f, 56f, context),
        Button(Pad.LB, R.drawable.touch_lb, 0.16f, 0.12f, 56f, context),
        Button(Pad.RB, R.drawable.touch_rb, 0.84f, 0.12f, 56f, context),
        Button(Pad.RT, R.drawable.touch_rt, 0.94f, 0.12f, 56f, context),
        Button(Pad.BACK, R.drawable.touch_back, 0.42f, 0.90f, 44f, context),
        Button(Pad.START, R.drawable.touch_start, 0.58f, 0.90f, 44f, context),
    )
    private val left = Stick(R.drawable.touch_stick_left, 0.15f, 0.66f, context)
    private val right = Stick(R.drawable.touch_stick_right, 0.66f, 0.76f, context)
    private val pointerButton = HashMap<Int, Button>()

    private val art = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = 0xFFFFFFFF.toInt()
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt() }

    private var shown = true
    private var hideWithPad = true
    private var scale = 1f
    private var opacity = 0.5f
    private var suspended = false

    private val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    private val padListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = updateVisibility()
        override fun onInputDeviceRemoved(deviceId: Int) = updateVisibility()
        override fun onInputDeviceChanged(deviceId: Int) = updateVisibility()
    }
    // The game menu opening and closing (native), checked a few times a second.
    private val poll = object : Runnable {
        override fun run() {
            val now = try { GameActivity.nativeTouchSuspended() } catch (e: UnsatisfiedLinkError) { false }
            if (now != suspended) {
                suspended = now
                updateVisibility()
            }
            postDelayed(this, 250)
        }
    }

    init {
        isClickable = true
        isFocusable = false
    }

    /** launcher.cfg's touch settings (at start and after the in-game menu). */
    fun reload(config: LauncherConfig) {
        shown = config.bool("touch_show", true)
        hideWithPad = config.bool("touch_hide_with_pad", true)
        scale = config.int("touch_size", 100).coerceIn(50, 200) / 100f
        opacity = config.int("touch_opacity", 50).coerceIn(10, 100) / 100f
        layoutControls(width, height)
        updateVisibility()
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        inputManager.registerInputDeviceListener(padListener, handler)
        post(poll)
    }

    override fun onDetachedFromWindow() {
        inputManager.unregisterInputDeviceListener(padListener)
        removeCallbacks(poll)
        releaseAll()
        super.onDetachedFromWindow()
    }

    private fun updateVisibility() {
        val show = !suspended && shown && !(hideWithPad && controllerConnected())
        if (show == (visibility == VISIBLE)) return
        if (!show) releaseAll()
        visibility = if (show) VISIBLE else GONE
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutControls(w, h)
    }

    private fun layoutControls(w: Int, h: Int) {
        if (w <= 0 || h <= 0) return
        val dp = resources.displayMetrics.density * scale
        for (b in buttons) {
            val half = b.sizeDp * dp / 2
            val cx = b.fx * w
            val cy = b.fy * h
            b.rect.set(cx - half, cy - half, cx + half, cy + half)
        }
        for (s in listOf(left, right)) {
            s.radius = 66 * dp
            // Kept on screen whatever the size.
            s.cx = (s.fx * w).coerceIn(s.radius, w - s.radius)
            s.cy = (s.fy * h).coerceIn(s.radius, h - s.radius)
        }
        ring.strokeWidth = 3 * dp
    }

    override fun onDraw(canvas: Canvas) {
        for (b in buttons) {
            val bitmap = b.bitmap ?: continue
            art.alpha = (255 * opacity * if (b.pressed) 1.6f else 1f).toInt().coerceAtMost(255)
            canvas.drawBitmap(bitmap, null, b.rect, art)
        }
        for (s in listOf(left, right)) {
            ring.alpha = (255 * opacity * 0.7f).toInt()
            canvas.drawCircle(s.cx, s.cy, s.radius, ring)
            val kx = s.cx + s.x * s.radius
            val ky = s.cy + s.y * s.radius
            val knob = s.radius * 0.45f
            fill.alpha = (255 * opacity * if (s.pointer >= 0) 0.5f else 0.3f).toInt()
            canvas.drawCircle(kx, ky, knob, fill)
            s.bitmap?.let {
                art.alpha = (255 * opacity).toInt()
                canvas.drawBitmap(it, null, RectF(kx - knob, ky - knob, kx + knob, ky + knob), art)
            }
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                down(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) {
                move(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> up(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        send()
        invalidate()
        return true
    }

    private fun down(id: Int, x: Float, y: Float) {
        // A stick takes a touch anywhere near it (half again its size).
        for (s in listOf(left, right)) {
            if (s.pointer < 0 && hypot(x - s.cx, y - s.cy) <= s.radius * 1.5f) {
                s.pointer = id
                aim(s, x, y)
                return
            }
        }
        buttonAt(x, y)?.let { b ->
            b.pressed = true
            pointerButton[id] = b
        }
    }

    // A finger sliding off a button onto another presses that one instead
    // (mashing, moving between A and B).
    private fun move(id: Int, x: Float, y: Float) {
        for (s in listOf(left, right)) {
            if (s.pointer == id) {
                aim(s, x, y)
                return
            }
        }
        val now = buttonAt(x, y) ?: return
        val was = pointerButton[id]
        if (was === now) return
        was?.let { if (pointerButton.count { (_, b) -> b === it } == 1) it.pressed = false }
        now.pressed = true
        pointerButton[id] = now
    }

    private fun up(id: Int) {
        for (s in listOf(left, right)) {
            if (s.pointer == id) {
                s.pointer = -1
                s.x = 0f
                s.y = 0f
                return
            }
        }
        val b = pointerButton.remove(id) ?: return
        if (pointerButton.values.none { it === b }) b.pressed = false
    }

    private fun buttonAt(x: Float, y: Float): Button? {
        // Slightly larger than drawn; the nearest when two overlap.
        var best: Button? = null
        var bestDistance = Float.MAX_VALUE
        for (b in buttons) {
            val reach = b.rect.width() * 0.65f
            val d = hypot(x - b.rect.centerX(), y - b.rect.centerY())
            if (d <= reach && d < bestDistance) {
                best = b
                bestDistance = d
            }
        }
        return best
    }

    private fun aim(s: Stick, x: Float, y: Float) {
        var dx = (x - s.cx) / s.radius
        var dy = (y - s.cy) / s.radius
        val length = hypot(dx, dy)
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        s.x = dx
        s.y = dy
    }

    private fun releaseAll() {
        for (b in buttons) b.pressed = false
        pointerButton.clear()
        for (s in listOf(left, right)) {
            s.pointer = -1
            s.x = 0f
            s.y = 0f
        }
        send()
        invalidate()
    }

    private fun send() {
        var controls = 0
        for (b in buttons) if (b.pressed) controls = controls or (1 shl b.control)
        // A small dead zone, then the full range past it; y up for the game.
        fun axis(v: Float, other: Float): Float {
            val length = hypot(v, other)
            if (length < DEAD_ZONE) return 0f
            return v * min(1f, (length - DEAD_ZONE) / (1f - DEAD_ZONE)) / length
        }
        try {
            GameActivity.nativeTouchPad(controls, axis(left.x, left.y), -axis(left.y, left.x),
                axis(right.x, right.y), -axis(right.y, right.x))
        } catch (e: UnsatisfiedLinkError) {
        }
    }

    companion object {
        private const val DEAD_ZONE = 0.12f

        /** A controller (not the phone's own buttons) is connected. */
        fun controllerConnected(): Boolean = InputDevice.getDeviceIds().any { id ->
            val device = InputDevice.getDevice(id) ?: return@any false
            !device.isVirtual && (device.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                device.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)
        }
    }
}
