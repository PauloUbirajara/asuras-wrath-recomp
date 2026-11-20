package org.asurawrath.recompiled

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import androidx.core.content.ContextCompat
import java.util.Locale
import kotlin.math.hypot
import kotlin.math.min

/**
 * On-screen controls over the game (GameActivity): two sticks, A B X Y, the
 * bumpers and triggers, Back and Start, and Hide between those two (hides
 * the others until tapped again, for cutscenes). They feed a pad of their own
 * in the game (pad_remap.cpp TouchPadDriver, through
 * GameActivity.nativeTouchPad), which plays alongside a real controller and
 * goes through the same remap and Turbo.
 *
 * launcher.cfg (Controls › Touch controls):
 *   touch_show            1: shown over the game (default)
 *   touch_hide_with_pad   1: hidden while a controller is connected (default)
 *   touch_size            percent, all of them (100: buttons 64 dp, sticks 132 dp across)
 *   touch_opacity         percent
 *   touch_layout          moved or resized ones (Touch layout, TouchLayoutActivity):
 *                         id:x:y:size;... (x, y the centre as a fraction of the
 *                         screen, size percent)
 *
 * The view takes every touch while shown. In `editing` (TouchLayoutActivity)
 * the controls are dragged instead of pressed, and nothing reaches the game.
 *
 * Button art: kenney.nl input prompts (CC0).
 */
class TouchControlsView(context: Context, private val editing: Boolean = false) : View(context) {

    // PadControl (src/input/pad_remap.h): the bit each control sets.
    private object Pad {
        const val A = 0; const val B = 1; const val X = 2; const val Y = 3
        const val LB = 4; const val RB = 5; const val LT = 6; const val RT = 7
        const val BACK = 8; const val START = 9
    }

    /**
     * A control: its launcher.cfg id, PadControl (-1: none), art, default
     * centre (fraction of the view) and size (dp at 100%: a button's width,
     * a stick's ring across).
     */
    class Control(val id: String, val pad: Int, art: Int, val defaultX: Float, val defaultY: Float,
                  val sizeDp: Float, context: Context, val stick: Boolean = false) {
        val bitmap: Bitmap? = if (art != 0) BitmapFactory.decodeResource(context.resources, art) else null
        var fx = defaultX; var fy = defaultY; var scale = 1f
        val rect = RectF()          // where it's drawn (a stick: its ring's bounds)
        var pressed = false
        var x = 0f; var y = 0f      // a stick: -1..1, screen axes (y down)
        var pointer = -1
        val radius get() = rect.width() / 2
    }

    val controls = listOf(
        Control("a", Pad.A, R.drawable.touch_button_a, 0.875f, 0.70f, 64f, context),
        Control("b", Pad.B, R.drawable.touch_button_b, 0.945f, 0.52f, 64f, context),
        Control("x", Pad.X, R.drawable.touch_button_x, 0.805f, 0.52f, 64f, context),
        Control("y", Pad.Y, R.drawable.touch_button_y, 0.875f, 0.34f, 64f, context),
        Control("lt", Pad.LT, R.drawable.touch_lt, 0.06f, 0.12f, 56f, context),
        Control("lb", Pad.LB, R.drawable.touch_lb, 0.16f, 0.12f, 56f, context),
        Control("rb", Pad.RB, R.drawable.touch_rb, 0.84f, 0.12f, 56f, context),
        Control("rt", Pad.RT, R.drawable.touch_rt, 0.94f, 0.12f, 56f, context),
        Control("back", Pad.BACK, R.drawable.touch_back, 0.42f, 0.08f, 44f, context),
        Control("hide", -1, 0, 0.50f, 0.08f, 40f, context),
        Control("start", Pad.START, R.drawable.touch_start, 0.58f, 0.08f, 44f, context),
        Control("ls", -1, R.drawable.touch_stick_left, 0.15f, 0.66f, 132f, context, stick = true),
        Control("rs", -1, R.drawable.touch_stick_right, 0.66f, 0.76f, 132f, context, stick = true),
    )
    private val left = controls.first { it.id == "ls" }
    private val right = controls.first { it.id == "rs" }
    private val hide = controls.first { it.id == "hide" }
    private val sticks = listOf(left, right)
    private val buttons = controls.filter { !it.stick }
    private val pointerButton = HashMap<Int, Control>()

    private val art = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = 0xFFFFFFFF.toInt()
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt() }
    private val dark = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF000000.toInt() }
    private val selection = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = ContextCompat.getColor(context, R.color.accent)
    }
    private val eye = Path()

    private var shown = true
    private var hideWithPad = true
    private var size = 1f
    private var opacity = 0.5f
    private var suspended = false
    /** Hide pressed: only it shows, until pressed again. */
    private var hiddenByUser = false

    /** Editing: the control picked (moved by dragging, resized by the editor's slider). */
    var selected: Control? = null
        private set
    var onSelect: ((Control) -> Unit)? = null
    private var dragDx = 0f
    private var dragDy = 0f

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
        size = config.int("touch_size", 100).coerceIn(50, 200) / 100f
        opacity = config.int("touch_opacity", 50).coerceIn(10, 100) / 100f
        readLayout(config["touch_layout"].orEmpty())
        layoutControls(width, height)
        updateVisibility()
        invalidate()
    }

    private fun readLayout(text: String) {
        for (c in controls) { c.fx = c.defaultX; c.fy = c.defaultY; c.scale = 1f }
        for (entry in text.split(';')) {
            val parts = entry.split(':')
            if (parts.size != 4) continue
            val c = controls.firstOrNull { it.id == parts[0].trim() } ?: continue
            c.fx = parts[1].toFloatOrNull()?.coerceIn(0f, 1f) ?: continue
            c.fy = parts[2].toFloatOrNull()?.coerceIn(0f, 1f) ?: continue
            c.scale = (parts[3].toIntOrNull() ?: 100).coerceIn(MIN_SCALE, MAX_SCALE) / 100f
        }
    }

    /** touch_layout for the controls not at their defaults ("" when none is). */
    fun layoutText(): String = controls.filter { c ->
        c.fx != c.defaultX || c.fy != c.defaultY || c.scale != 1f
    }.joinToString(";") { c ->
        String.format(Locale.ROOT, "%s:%.4f:%.4f:%d", c.id, c.fx, c.fy, Math.round(c.scale * 100))
    }

    fun resetLayout() {
        readLayout("")
        layoutControls(width, height)
        invalidate()
        selected?.let { onSelect?.invoke(it) }
    }

    /** Editing: the picked control's size (percent of its default). */
    fun setSelectedScale(percent: Int) {
        val c = selected ?: return
        c.scale = percent.coerceIn(MIN_SCALE, MAX_SCALE) / 100f
        layoutControls(width, height)
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (editing) return
        inputManager.registerInputDeviceListener(padListener, handler)
        post(poll)
    }

    override fun onDetachedFromWindow() {
        if (!editing) {
            inputManager.unregisterInputDeviceListener(padListener)
            removeCallbacks(poll)
            releaseAll()
        }
        super.onDetachedFromWindow()
    }

    private fun updateVisibility() {
        val show = editing || (!suspended && shown && !(hideWithPad && controllerConnected()))
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
        val dp = resources.displayMetrics.density * size
        for (c in controls) {
            val half = c.sizeDp * c.scale * dp / 2
            // Kept on screen whatever the size.
            val cx = (c.fx * w).coerceIn(half, maxOf(half, w - half))
            val cy = (c.fy * h).coerceIn(half, maxOf(half, h - half))
            c.rect.set(cx - half, cy - half, cx + half, cy + half)
        }
        ring.strokeWidth = 3 * dp
        selection.strokeWidth = 3 * resources.displayMetrics.density
    }

    override fun onDraw(canvas: Canvas) {
        val alpha = if (editing) maxOf(opacity, 0.7f) else opacity
        if (!hiddenByUser || editing) {
            for (c in buttons) {
                if (c === hide) continue
                val bitmap = c.bitmap ?: continue
                art.alpha = (255 * alpha * if (c.pressed) 1.6f else 1f).toInt().coerceAtMost(255)
                canvas.drawBitmap(bitmap, null, c.rect, art)
            }
            for (s in sticks) {
                ring.alpha = (255 * alpha * 0.7f).toInt()
                canvas.drawCircle(s.rect.centerX(), s.rect.centerY(), s.radius, ring)
                val kx = s.rect.centerX() + s.x * s.radius
                val ky = s.rect.centerY() + s.y * s.radius
                val knob = s.radius * 0.45f
                fill.alpha = (255 * alpha * if (s.pointer >= 0) 0.5f else 0.3f).toInt()
                canvas.drawCircle(kx, ky, knob, fill)
                s.bitmap?.let {
                    art.alpha = (255 * alpha).toInt()
                    canvas.drawBitmap(it, null, RectF(kx - knob, ky - knob, kx + knob, ky + knob), art)
                }
            }
        }
        drawHide(canvas, if (hiddenByUser && !editing) min(alpha, 0.35f) else alpha)
        selected?.let { c ->
            val pad = 4 * resources.displayMetrics.density
            canvas.drawRect(c.rect.left - pad, c.rect.top - pad, c.rect.right + pad, c.rect.bottom + pad, selection)
        }
    }

    // Hide: an eye on a disc (crossed out while the others are hidden).
    private fun drawHide(canvas: Canvas, alpha: Float) {
        val r = hide.radius
        val cx = hide.rect.centerX()
        val cy = hide.rect.centerY()
        fill.alpha = (255 * alpha * if (hide.pressed) 1f else 0.6f).toInt().coerceAtMost(255)
        canvas.drawCircle(cx, cy, r, fill)
        dark.alpha = (255 * min(1f, alpha * 1.6f)).toInt()
        dark.style = Paint.Style.STROKE
        dark.strokeWidth = r * 0.12f
        eye.reset()
        eye.moveTo(cx - r * 0.6f, cy)
        eye.quadTo(cx, cy - r * 0.62f, cx + r * 0.6f, cy)
        eye.quadTo(cx, cy + r * 0.62f, cx - r * 0.6f, cy)
        canvas.drawPath(eye, dark)
        dark.style = Paint.Style.FILL
        canvas.drawCircle(cx, cy, r * 0.17f, dark)
        if (hiddenByUser) {
            dark.style = Paint.Style.STROKE
            canvas.drawLine(cx - r * 0.55f, cy + r * 0.55f, cx + r * 0.55f, cy - r * 0.55f, dark)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (editing) return edit(event)
        // Hidden: only Hide answers; the rest of the screen is the game's.
        if (hiddenByUser && event.actionMasked == MotionEvent.ACTION_DOWN &&
            hypot(event.x - hide.rect.centerX(), event.y - hide.rect.centerY()) > hide.radius * 1.3f) return false
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

    // Editing: a touch picks the control under it and drags it.
    private fun edit(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val c = controlAt(event.x, event.y)
                selected = c
                if (c != null) {
                    dragDx = c.rect.centerX() - event.x
                    dragDy = c.rect.centerY() - event.y
                    onSelect?.invoke(c)
                }
            }
            MotionEvent.ACTION_MOVE -> selected?.let { c ->
                if (width > 0 && height > 0) {
                    c.fx = ((event.x + dragDx) / width).coerceIn(0f, 1f)
                    c.fy = ((event.y + dragDy) / height).coerceIn(0f, 1f)
                    layoutControls(width, height)
                }
            }
        }
        invalidate()
        return true
    }

    private fun controlAt(x: Float, y: Float): Control? = controls.minByOrNull {
        hypot(x - it.rect.centerX(), y - it.rect.centerY()) / it.radius
    }?.takeIf { hypot(x - it.rect.centerX(), y - it.rect.centerY()) <= it.radius * 1.3f }

    private fun down(id: Int, x: Float, y: Float) {
        // Hide toggles the others (a tap; nothing reaches the game).
        if (hypot(x - hide.rect.centerX(), y - hide.rect.centerY()) <= hide.radius * 1.3f) {
            hiddenByUser = !hiddenByUser
            if (hiddenByUser) releaseAll()
            return
        }
        if (hiddenByUser) return
        // A stick takes a touch anywhere near it (half again its size).
        for (s in sticks) {
            if (s.pointer < 0 && hypot(x - s.rect.centerX(), y - s.rect.centerY()) <= s.radius * 1.5f) {
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
        for (s in sticks) {
            if (s.pointer == id) {
                aim(s, x, y)
                return
            }
        }
        if (hiddenByUser) return
        val now = buttonAt(x, y) ?: return
        val was = pointerButton[id]
        if (was === now) return
        was?.let { if (pointerButton.count { (_, b) -> b === it } == 1) it.pressed = false }
        now.pressed = true
        pointerButton[id] = now
    }

    private fun up(id: Int) {
        for (s in sticks) {
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

    private fun buttonAt(x: Float, y: Float): Control? {
        // Slightly larger than drawn; the nearest when two overlap.
        var best: Control? = null
        var bestDistance = Float.MAX_VALUE
        for (b in buttons) {
            if (b.pad < 0) continue
            val reach = b.rect.width() * 0.65f
            val d = hypot(x - b.rect.centerX(), y - b.rect.centerY())
            if (d <= reach && d < bestDistance) {
                best = b
                bestDistance = d
            }
        }
        return best
    }

    private fun aim(s: Control, x: Float, y: Float) {
        var dx = (x - s.rect.centerX()) / s.radius
        var dy = (y - s.rect.centerY()) / s.radius
        val length = hypot(dx, dy)
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        s.x = dx
        s.y = dy
    }

    private fun releaseAll() {
        for (c in controls) {
            c.pressed = false
            c.pointer = -1
            c.x = 0f
            c.y = 0f
        }
        pointerButton.clear()
        send()
        invalidate()
    }

    private fun send() {
        if (editing) return
        var bits = 0
        for (b in buttons) if (b.pressed && b.pad >= 0) bits = bits or (1 shl b.pad)
        // A small dead zone, then the full range past it; y up for the game.
        fun axis(v: Float, other: Float): Float {
            val length = hypot(v, other)
            if (length < DEAD_ZONE) return 0f
            return v * min(1f, (length - DEAD_ZONE) / (1f - DEAD_ZONE)) / length
        }
        try {
            GameActivity.nativeTouchPad(bits, axis(left.x, left.y), -axis(left.y, left.x),
                axis(right.x, right.y), -axis(right.y, right.x))
        } catch (e: UnsatisfiedLinkError) {
        }
    }

    companion object {
        private const val DEAD_ZONE = 0.12f
        const val MIN_SCALE = 50
        const val MAX_SCALE = 250

        /** A controller (not the phone's own buttons) is connected. */
        fun controllerConnected(): Boolean = InputDevice.getDeviceIds().any { id ->
            val device = InputDevice.getDevice(id) ?: return@any false
            !device.isVirtual && (device.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                device.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)
        }
    }
}
