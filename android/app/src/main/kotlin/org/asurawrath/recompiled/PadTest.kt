package org.asurawrath.recompiled

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.util.AttributeSet
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import androidx.core.content.ContextCompat
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/**
 * Controls › Test: the controller as the launcher sees it through Android's
 * input events (the game sees it through SDL, with the same Xbox layout); a
 * controller set up in Controls › Controller setup, through that setup.
 * Controls are numbered as the game's PadControl (src/input/pad_remap.h).
 */
class PadState {
    var buttons = 0          // bit = PadControl (the triggers too, past the threshold)
    var lx = 0f; var ly = 0f // -1..1, up positive
    var rx = 0f; var ry = 0f
    var lt = 0f; var rt = 0f // 0..1
    var device: InputDevice? = null

    fun down(control: Int) = buttons and (1 shl control) != 0

    private fun set(control: Int, on: Boolean) {
        buttons = if (on) buttons or (1 shl control) else buttons and (1 shl control).inv()
    }

    /** Controls › Controller setup's mappings: a controller set up there reads through its own. */
    var setups: SdlPad.Setups? = null
        set(value) { field = value; setupFor = -1 }
    private val raw = SdlPad.Raw()
    private var setupFor = -1
    private var setup: Map<String, String>? = null

    /** The setups changed (Controller setup saved one). */
    fun reloadSetup() { setupFor = -1 }

    private fun setupOf(d: InputDevice?): Map<String, String>? {
        if (d == null) return null
        if (d.id != setupFor) {
            setupFor = d.id
            setup = setups?.fields(d)
        }
        return setup
    }

    // The controls from the raw inputs, through the controller's setup (as the game reads it).
    private fun applySetup(f: Map<String, String>) {
        fun r(field: String) = raw.read(f[field])
        for ((control, field) in SETUP_FIELDS.withIndex()) set(control, r(field) > 0.5f)
        lt = r("lefttrigger")
        rt = r("righttrigger")
        set(LT, lt > TRIGGER_THRESHOLD)
        set(RT, rt > TRIGGER_THRESHOLD)
        lx = r("+leftx") - r("-leftx")
        ly = r("-lefty") - r("+lefty")
        rx = r("+rightx") - r("-rightx")
        ry = r("-righty") - r("+righty")
    }

    /** A gamepad key: true if it is one of the pad's controls. */
    fun onKey(event: KeyEvent): Boolean {
        if (raw.onKey(event)) {
            setupOf(event.device)?.let {
                device = event.device
                applySetup(it)
                return true
            }
        }
        val control = KEYS[event.keyCode] ?: return false
        if (!isPad(event.device)) return false
        device = event.device
        when (event.action) {
            KeyEvent.ACTION_DOWN -> set(control, true)
            KeyEvent.ACTION_UP -> set(control, false)
        }
        // A digital trigger (no axis) counts as fully pressed.
        if (control == LT && !hasAnalogTrigger(event.device)) lt = if (down(LT)) 1f else 0f
        if (control == RT && !hasAnalogTrigger(event.device)) rt = if (down(RT)) 1f else 0f
        return true
    }

    /** Which of Z/RZ and RX/RY are the triggers (PadAxes, shared with the game's mapping fix). */
    private val axes = PadAxes()

    private fun triggerValue(d: InputDevice?, event: MotionEvent, axis: Int): Float {
        val v = event.getAxisValue(axis)
        val centred = (d?.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK)?.min ?: 0f) < 0f
        return (if (centred) (v + 1f) / 2f else v).coerceIn(0f, 1f)
    }

    private fun hasAnalogTrigger(d: InputDevice?) =
        hasAxis(d, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS) ||
            (d != null && d.id == axes.learnedFor && axes.triggerPairs.isNotEmpty())

    /** A joystick move: sticks, triggers, the d-pad hat. */
    fun onMotion(event: MotionEvent): Boolean {
        if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.action != MotionEvent.ACTION_MOVE) return false
        val d = event.device
        device = d
        if (raw.onMotion(event)) {
            setupOf(d)?.let {
                applySetup(it)
                return true
            }
        }
        lx = event.getAxisValue(MotionEvent.AXIS_X)
        ly = -event.getAxisValue(MotionEvent.AXIS_Y)
        if (d != null) axes.learn(d, event)
        // The right stick: Z/RZ on most pads, RX/RY on the others (where
        // Z/RZ are the triggers).
        val stick = STICK_PAIRS.firstOrNull { p -> hasAxis(d, p.first) && hasAxis(d, p.second) && p !in axes.triggerPairs }
        if (stick != null) {
            rx = event.getAxisValue(stick.first)
            ry = -event.getAxisValue(stick.second)
        } else {
            rx = 0f; ry = 0f
        }
        val pair = axes.triggerPairs.firstOrNull()
        if (hasAxis(d, MotionEvent.AXIS_LTRIGGER) || hasAxis(d, MotionEvent.AXIS_BRAKE)) {
            lt = max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        } else if (pair != null) {
            lt = triggerValue(d, event, pair.first)
        }
        if (hasAxis(d, MotionEvent.AXIS_RTRIGGER) || hasAxis(d, MotionEvent.AXIS_GAS)) {
            rt = max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
        } else if (pair != null) {
            rt = triggerValue(d, event, pair.second)
        }
        if (hasAnalogTrigger(d)) {
            set(LT, lt > TRIGGER_THRESHOLD)
            set(RT, rt > TRIGGER_THRESHOLD)
        }
        if (hasAxis(d, MotionEvent.AXIS_HAT_X)) {
            val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            set(LEFT, hx < -0.5f); set(RIGHT, hx > 0.5f)
            set(UP, hy < -0.5f); set(DOWN, hy > 0.5f)
        }
        return true
    }

    /** Vibrates the controller at `percent` (100: half power, as a strong rumble from the game). */
    fun vibrate(percent: Int): Boolean {
        val d = device ?: connectedPad() ?: return false
        val vibrator: Vibrator = if (Build.VERSION.SDK_INT >= 31) d.vibratorManager.defaultVibrator
        else @Suppress("DEPRECATION") d.vibrator
        if (!vibrator.hasVibrator()) return false
        val amplitude = min(255, 128 * percent / 100).coerceAtLeast(1)
        vibrator.vibrate(
            VibrationEffect.createOneShot(600, if (vibrator.hasAmplitudeControl()) amplitude else VibrationEffect.DEFAULT_AMPLITUDE),
        )
        return true
    }

    companion object {
        const val A = 0; const val B = 1; const val X = 2; const val Y = 3
        const val LB = 4; const val RB = 5; const val LT = 6; const val RT = 7
        const val BACK = 8; const val START = 9; const val LS = 10; const val RS = 11
        const val UP = 12; const val DOWN = 13; const val LEFT = 14; const val RIGHT = 15
        const val COUNT = 16
        /** launcher.cfg's names (pad.<target>=<source>), as PadControlName. */
        val NAMES = arrayOf("a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "ls", "rs", "up", "down", "left", "right")
        /** The right stick's candidate axes, in order of preference. */
        val STICK_PAIRS = listOf(
            MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ,
            MotionEvent.AXIS_RX to MotionEvent.AXIS_RY,
        )
        private const val TRIGGER_THRESHOLD = 30f / 255f  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
        /** SDL's mapping field for each control (PadControl order). */
        private val SETUP_FIELDS = listOf("a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger",
            "righttrigger", "back", "start", "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright")

        private val KEYS = mapOf(
            KeyEvent.KEYCODE_BUTTON_A to A, KeyEvent.KEYCODE_BUTTON_B to B,
            KeyEvent.KEYCODE_BUTTON_X to X, KeyEvent.KEYCODE_BUTTON_Y to Y,
            KeyEvent.KEYCODE_BUTTON_L1 to LB, KeyEvent.KEYCODE_BUTTON_R1 to RB,
            KeyEvent.KEYCODE_BUTTON_L2 to LT, KeyEvent.KEYCODE_BUTTON_R2 to RT,
            KeyEvent.KEYCODE_BUTTON_SELECT to BACK, KeyEvent.KEYCODE_BUTTON_START to START,
            KeyEvent.KEYCODE_BUTTON_THUMBL to LS, KeyEvent.KEYCODE_BUTTON_THUMBR to RS,
            KeyEvent.KEYCODE_DPAD_UP to UP, KeyEvent.KEYCODE_DPAD_DOWN to DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT to LEFT, KeyEvent.KEYCODE_DPAD_RIGHT to RIGHT,
        )

        fun hasAxis(d: InputDevice?, vararg axes: Int) =
            d != null && axes.any { d.getMotionRange(it, InputDevice.SOURCE_JOYSTICK) != null }

        /**
         * A real controller. Phones list built-in inputs as gamepads or
         * joysticks too (Xiaomi's fingerprint readers, uinput-fpc and
         * uinput-goodix; gpio keys; shoulder triggers), which hid the touch
         * controls as if a controller were connected: a controller here also
         * has a stick, a hat or an A button, and isn't one of those by name.
         */
        fun isPad(d: InputDevice?): Boolean {
            if (d == null || d.isVirtual) return false
            if (!d.supportsSource(InputDevice.SOURCE_GAMEPAD) && !d.supportsSource(InputDevice.SOURCE_JOYSTICK)) return false
            val name = d.name.lowercase()
            if (BUILT_IN.any { it in name }) return false
            val stick = (d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null &&
                d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null) ||
                d.getMotionRange(MotionEvent.AXIS_HAT_X, InputDevice.SOURCE_JOYSTICK) != null
            return stick || d.hasKeys(KeyEvent.KEYCODE_BUTTON_A)[0]
        }

        // Names of built-in inputs that claim to be gamepads.
        private val BUILT_IN = listOf("uinput", "fpc", "goodix", "fingerprint", "gpio", "gf_input",
            "silead", "egis", "qwerty", "virtual", "xiaomi-touch", "touchscreen", "pmic", "keypad")

        /** The first controller connected, or null. */
        fun connectedPad(): InputDevice? =
            InputDevice.getDeviceIds().asSequence().mapNotNull(InputDevice::getDevice).firstOrNull(::isPad)

        /** source[target] from launcher.cfg's pad.* keys (as PadMapFrom). */
        fun map(config: LauncherConfig): IntArray = IntArray(COUNT) { target ->
            NAMES.indexOf(config["pad.${NAMES[target]}"] ?: "").takeIf { it >= 0 } ?: target
        }
    }
}

/**
 * The live view (as LauncherDialog::DrawController's test): the triggers,
 * the sticks with their deadzones shaded, and the buttons lit as the game
 * sees them after the remap.
 */
class PadTestView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : View(context, attrs) {

    var state = PadState()
    var map = IntArray(PadState.COUNT) { it }
    var deadzoneLeft = 0f
    var deadzoneRight = 0f
    /** Chip labels (the d-pad directions are words, translated). */
    var labels = arrayOf("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS", "Up", "Down", "Left", "Right")

    private val density = resources.displayMetrics.density
    private val scaled = resources.displayMetrics.scaledDensity
    private val line = ContextCompat.getColor(context, R.color.panel_text)
    private val accent = ContextCompat.getColor(context, R.color.accent)
    private val field = ContextCompat.getColor(context, R.color.field)
    private val light = ContextCompat.getColor(context, R.color.sidebar_text_selected)
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f * density }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 13 * scaled }
    private val rect = RectF()

    // Layout, from the width: the sticks row, then the chips.
    private var radius = 0f
    private var barW = 0f
    private var gap = 0f
    private var chipW = 0f
    private var chipH = 0f
    private var perRow = 7

    init {
        LauncherArt.regularFont(context)?.let { text.typeface = it }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec).toFloat()
        barW = 18 * density
        gap = 18 * density
        // LT, L, R, RT across the width; no bigger than 64dp a stick.
        radius = min(64 * density, (w - 2 * barW - 3 * gap) / 4)
        chipH = 34 * density
        val chipGap = 6 * density
        perRow = if (w > 520 * density) 7 else if (w > 300 * density) 5 else 4
        chipW = (w - (perRow - 1) * chipGap) / perRow
        val rows = (14 + perRow - 1) / perRow
        val h = radius * 2 + text.textSize + 10 * density + gap + rows * chipH + (rows - 1) * chipGap
        setMeasuredDimension(w.toInt(), h.toInt())
    }

    private fun mappedDown(target: Int): Boolean = state.down(map[target])

    private fun mappedTrigger(target: Int): Float = when (val source = map[target]) {
        PadState.LT -> state.lt
        PadState.RT -> state.rt
        else -> if (state.down(source)) 1f else 0f
    }

    override fun onDraw(canvas: Canvas) {
        stroke.color = line
        text.color = line
        val sticksW = 2 * barW + 4 * radius + 3 * gap
        var x = (width - sticksW) / 2
        val top = 0f
        fun trigger(value: Float, label: String) {
            rect.set(x, top, x + barW, top + radius * 2)
            fill.color = field
            canvas.drawRect(rect, fill)
            val h = rect.height() * value.coerceIn(0f, 1f)
            fill.color = if (value > 30f / 255f) accent else 0x28000000
            canvas.drawRect(rect.left, rect.bottom - h, rect.right, rect.bottom, fill)
            canvas.drawRect(rect, stroke)
            canvas.drawText(label, x, top + radius * 2 + text.textSize + 4 * density, text)
            x += barW + gap
        }
        fun stick(sx: Float, sy: Float, deadzone: Float, label: String) {
            val cx = x + radius
            val cy = top + radius
            fill.color = field
            canvas.drawCircle(cx, cy, radius, fill)
            if (deadzone > 0) {
                fill.color = 0x28000000
                canvas.drawCircle(cx, cy, radius * deadzone, fill)
            }
            canvas.drawCircle(cx, cy, radius, stroke)
            val moving = sqrt(sx * sx + sy * sy) > deadzone && (abs(sx) > 0.01f || abs(sy) > 0.01f)
            fill.color = if (moving) accent else line
            canvas.drawCircle(cx + sx.coerceIn(-1f, 1f) * radius, cy - sy.coerceIn(-1f, 1f) * radius, 7 * density, fill)
            canvas.drawText(String.format("%s %+.2f %+.2f", label, sx, sy), x, top + radius * 2 + text.textSize + 4 * density, text)
            x += radius * 2 + gap
        }
        trigger(mappedTrigger(PadState.LT), "LT")
        stick(state.lx, state.ly, deadzoneLeft, "L")
        stick(state.rx, state.ry, deadzoneRight, "R")
        trigger(mappedTrigger(PadState.RT), "RT")

        // The buttons, after the remap.
        val chipGap = 6 * density
        val chipsTop = top + radius * 2 + text.textSize + 10 * density + gap
        var slot = 0
        for (c in 0 until PadState.COUNT) {
            if (c == PadState.LT || c == PadState.RT) continue
            val col = slot % perRow
            val row = slot / perRow
            val left = col * (chipW + chipGap)
            val t = chipsTop + row * (chipH + chipGap)
            rect.set(left, t, left + chipW, t + chipH)
            val down = mappedDown(c)
            fill.color = if (down) accent else field
            val r = 4 * density
            canvas.drawRoundRect(rect, r, r, fill)
            canvas.drawRoundRect(rect, r, r, stroke)
            text.color = if (down) light else line
            val label = labels[c]
            val tw = text.measureText(label)
            canvas.drawText(label, rect.centerX() - tw / 2, rect.centerY() + text.textSize * 0.35f, text)
            slot++
        }
    }
}
