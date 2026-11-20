package org.asurawrath.recompiled

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat
import kotlin.math.abs

/** A list of controls, each a row (its name, its input) that listens when tapped. */
private class Rows(private val context: Context, private val list: LinearLayout, labels: List<String>,
                   onTap: (Int) -> Unit, onLongTap: ((Int) -> Unit)? = null) {
    private val values = ArrayList<TextView>()

    init {
        val dp = context.resources.displayMetrics.density
        list.removeAllViews()
        for ((i, label) in labels.withIndex()) {
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                minimumHeight = (44 * dp).toInt()
                setPadding((12 * dp).toInt(), (6 * dp).toInt(), (12 * dp).toInt(), (6 * dp).toInt())
                background = ContextCompat.getDrawable(context, R.drawable.row_background)
                setOnClickListener { onTap(i) }
                onLongTap?.let { f -> setOnLongClickListener { f(i); true } }
            }
            row.addView(TextView(context).apply {
                setTextAppearance(R.style.Text_Paper_Body)
                text = label
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            val value = TextView(context).apply {
                setTextAppearance(R.style.Text_Paper_Body)
                gravity = Gravity.END
            }
            row.addView(value, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            values += value
            list.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = (4 * dp).toInt() })
        }
        LauncherArt.applyFonts(list)
    }

    fun set(i: Int, text: String, dim: Boolean, listening: Boolean) {
        values[i].text = text
        values[i].setTextColor(ContextCompat.getColor(context, when {
            listening -> R.color.accent
            dim -> R.color.panel_text_dim
            else -> R.color.panel_text
        }))
    }
}

/**
 * Controls › Controller setup (Android's controller_setup.cpp): for a
 * controller the game reads wrongly. Each control is a row with the input
 * SDL gets for it; tapping it listens for the next one (10 s), Start setup
 * goes through them all. Each answer is saved at once (SdlPad.Setups), and
 * the game uses it from when it next comes to the front.
 */
class ControllerSetup(
    private val context: Context,
    private val setups: SdlPad.Setups,
    private val views: Views,
    private val onSaved: () -> Unit,
) {
    class Views(val pad: TextView, val start: View, val forget: View, val prompt: TextView,
                val skip: View, val stop: View, val message: TextView, val rows: LinearLayout)

    private enum class Kind { BUTTON, TRIGGER, STICK }
    private class Step(val field: String, val kind: Kind)

    private val steps = listOf(
        Step("a", Kind.BUTTON), Step("b", Kind.BUTTON), Step("x", Kind.BUTTON), Step("y", Kind.BUTTON),
        Step("leftshoulder", Kind.BUTTON), Step("rightshoulder", Kind.BUTTON),
        Step("lefttrigger", Kind.TRIGGER), Step("righttrigger", Kind.TRIGGER),
        Step("back", Kind.BUTTON), Step("start", Kind.BUTTON),
        Step("leftstick", Kind.BUTTON), Step("rightstick", Kind.BUTTON),
        Step("dpup", Kind.BUTTON), Step("dpdown", Kind.BUTTON), Step("dpleft", Kind.BUTTON), Step("dpright", Kind.BUTTON),
        Step("-lefty", Kind.STICK), Step("+lefty", Kind.STICK), Step("-leftx", Kind.STICK), Step("+leftx", Kind.STICK),
        Step("-righty", Kind.STICK), Step("+righty", Kind.STICK), Step("-rightx", Kind.STICK), Step("+rightx", Kind.STICK),
    )
    private val labels = context.resources.getStringArray(R.array.setup_labels).toList()
    private val prompts = context.resources.getStringArray(R.array.setup_prompts).toList()
    private val rows = Rows(context, views.rows, labels, { listen(it, sequence = false) })

    private val raw = SdlPad.Raw()
    private var device: InputDevice? = null
    private var fields = LinkedHashMap<String, String>()
    private var saved = false

    private var listening = -1
    private var sequence = false
    private var since = 0L
    private var armed = false
    private var rest = FloatArray(0)
    private var pending: String? = null
    private var pendingSince = 0L

    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            if (listening < 0) return
            check()
            show()
            handler.postDelayed(this, 100)
        }
    }

    init {
        views.start.setOnClickListener { listen(0, sequence = true) }
        views.stop.setOnClickListener { stop() }
        views.skip.setOnClickListener { if (listening + 1 < steps.size) listen(listening + 1, true) else stop() }
        views.forget.setOnClickListener {
            device?.let { setups.remove(it) }
            message(R.string.setup_forgotten)
            load(device)
            onSaved()
        }
    }

    /** The section is shown: the controller connected now. */
    fun refresh() {
        if (listening < 0) load(raw.device ?: PadState.connectedPad())
        show()
    }

    /** Leaving the section: stops listening. */
    fun pause() = stop()

    val isListening get() = listening >= 0

    private fun load(d: InputDevice?) {
        device = d
        val own = d?.let { setups.fields(it) }
        saved = own != null
        fields = own ?: d?.let { SdlPad.parseFields(SdlPad.buildFields(SdlPad.defaultFields(it))) } ?: LinkedHashMap()
    }

    private fun listen(step: Int, sequence: Boolean) {
        val d = raw.device ?: PadState.connectedPad()
        if (d == null) {
            message(R.string.setup_no_pad)
            return
        }
        if (d.id != device?.id) load(d)
        listening = step
        this.sequence = sequence
        since = SystemClock.uptimeMillis()
        armed = false
        pending = null
        rest = raw.axes.copyOf()
        views.message.text = ""
        handler.removeCallbacks(tick)
        handler.post(tick)
        show()
    }

    private fun stop() {
        listening = -1
        sequence = false
        handler.removeCallbacks(tick)
        show()
    }

    private fun message(id: Int) {
        views.message.text = context.getString(id)
    }

    /** A controller's key: true if it was taken (while listening, every one of the controller's). */
    fun onKey(event: KeyEvent): Boolean {
        if (!raw.onKey(event)) return false
        if (listening < 0) return false
        if (raw.device?.id != device?.id) load(raw.device)
        check()
        show()
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!raw.onMotion(event)) return false
        if (rest.size != raw.axes.size) rest = raw.axes.copyOf()
        if (listening < 0) return false
        check()
        return true
    }

    private fun released(): Boolean =
        raw.buttons.none { it } && raw.axes.indices.all { abs(raw.axes[it] - rest.getOrElse(it) { 0f }) <= LET_GO }

    private fun check() {
        if (listening < 0) return
        val now = SystemClock.uptimeMillis()
        if (now - since > LISTEN_MS) {
            stop()
            message(R.string.setup_timeout)
            return
        }
        if (!armed) {
            if (released()) {
                armed = true
            } else if (now - since > 1500 && raw.buttons.none { it }) {
                // Axes Android hadn't sent yet (nothing until one moves):
                // what the controller reads now, no button held, is its rest.
                rest = raw.axes.copyOf()
                armed = true
            }
            return
        }
        val kind = steps[listening].kind
        val button = raw.buttons.indexOfFirst { it }.takeIf { it >= 0 }?.let { "b$it" }
        var axis: String? = null
        var best = PRESSED
        for (i in raw.axes.indices) {
            val r = rest.getOrElse(i) { 0f }
            val moved = abs(raw.axes[i] - r)
            if (moved <= best) continue
            best = moved
            axis = when {
                kind == Kind.TRIGGER && r < -PRESSED -> "a$i"  // a trigger resting at one end: the whole axis
                kind == Kind.TRIGGER && r > PRESSED -> "a$i~"
                else -> (if (raw.axes[i] > r) "+a" else "-a") + i
            }
        }
        val got = when (kind) {
            Kind.BUTTON -> button ?: axis
            Kind.STICK -> axis ?: button
            Kind.TRIGGER -> axis ?: button?.let { b ->
                // A trigger may also send a button: wait a moment for its axis.
                if (pending == null) { pending = b; pendingSince = now; null }
                else if (now - pendingSince > 200) pending else null
            }
        } ?: return
        set(listening, got)
        if (sequence && listening + 1 < steps.size) {
            listen(listening + 1, true)
        } else {
            if (sequence) message(R.string.setup_done)
            stop()
        }
    }

    // One answer, saved at once; an input another control had is taken from it.
    private fun set(step: Int, input: String) {
        val d = device ?: return
        for (key in fields.keys.toList()) if (fields[key] == input) fields.remove(key)
        fields[steps[step].field] = input
        setups.save(d, fields)
        saved = true
        onSaved()
    }

    private fun describe(input: String?): String = when {
        input.isNullOrEmpty() -> context.getString(R.string.setup_not_set)
        input[0] == 'b' -> context.getString(R.string.setup_input_button, input.substring(1))
        input[0] == '+' || input[0] == '-' -> context.getString(R.string.setup_input_axis, "${input.substring(2)} ${input[0]}")
        else -> context.getString(R.string.setup_input_axis, input.substring(1))
    }

    private fun show() {
        val d = device
        views.pad.text = if (d == null) context.getString(R.string.setup_no_pad)
            else context.getString(if (saved) R.string.setup_uses_yours else R.string.setup_uses_default, d.name)
        val on = listening >= 0
        views.start.visibility = if (on || d == null) View.GONE else View.VISIBLE
        views.forget.visibility = if (on || !saved) View.GONE else View.VISIBLE
        views.stop.visibility = if (on) View.VISIBLE else View.GONE
        views.skip.visibility = if (on && sequence) View.VISIBLE else View.GONE
        views.prompt.visibility = if (on) View.VISIBLE else View.GONE
        if (on) {
            val left = ((LISTEN_MS - (SystemClock.uptimeMillis() - since)) / 1000 + 1).coerceIn(0, 10)
            views.prompt.text = context.getString(R.string.setup_listening, prompts[listening], left) +
                if (!armed) "\n" + context.getString(R.string.setup_let_go) else ""
        }
        views.rows.visibility = if (d == null) View.GONE else View.VISIBLE
        for (i in steps.indices) {
            val input = fields[steps[i].field]
            rows.set(i, if (i == listening) "…" else describe(input), input.isNullOrEmpty(), i == listening)
        }
    }

    companion object {
        const val LISTEN_MS = 10_000L
        private const val PRESSED = 0.5f   // an axis this far from rest counts
        private const val LET_GO = 0.25f   // and back within this, let go
    }
}

/**
 * Controls › Controller › Button remap (launcher.cfg pad.<target>=<source>,
 * as the desktop's Controller tab): which of the controller's buttons acts
 * as each of the game's. Tapping a row listens for the button (10 s); two
 * buttons trade places, so each stays reachable. A long tap resets a row.
 */
class ButtonRemap(
    private val context: Context,
    private val config: () -> LauncherConfig,
    private val pad: PadState,
    private val prompt: TextView,
    list: LinearLayout,
    private val onChanged: () -> Unit,
) {
    private val names = context.resources.getStringArray(R.array.setup_labels).take(PadState.COUNT)
    private val short = arrayOf("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS",
        context.getString(R.string.pad_up), context.getString(R.string.pad_down),
        context.getString(R.string.pad_left), context.getString(R.string.pad_right))
    private val rows = Rows(context, list, names, ::listen) { target -> setSource(target, target) }

    private var listening = -1
    private var since = 0L
    private var held = 0
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            if (listening < 0) return
            if (SystemClock.uptimeMillis() - since > ControllerSetup.LISTEN_MS) {
                stop()
                prompt.visibility = View.VISIBLE
                prompt.text = context.getString(R.string.setup_timeout)
                return
            }
            show()
            handler.postDelayed(this, 250)
        }
    }

    val isListening get() = listening >= 0

    private fun listen(target: Int) {
        listening = target
        since = SystemClock.uptimeMillis()
        held = pad.buttons  // held now: wait for a fresh press
        handler.removeCallbacks(tick)
        handler.post(tick)
        show()
    }

    fun stop() {
        listening = -1
        handler.removeCallbacks(tick)
        show()
    }

    /** After the pad's state changed (PadState fed with an event). True while listening. */
    fun onPad(): Boolean {
        if (listening < 0) return false
        held = held and pad.buttons
        val fresh = pad.buttons and held.inv()
        if (fresh != 0) {
            setSource(listening, Integer.numberOfTrailingZeros(fresh))
            stop()
        }
        return true
    }

    // As LauncherDialog::SetPadSource: the button that had it gets the target's.
    private fun setSource(target: Int, source: Int) {
        val c = config()
        val map = PadState.map(c)
        for (other in 0 until PadState.COUNT) {
            if (other != target && map[other] == source) map[other] = map[target]
        }
        map[target] = source
        for (t in 0 until PadState.COUNT) {
            val key = "pad.${PadState.NAMES[t]}"
            if (map[t] != t) c[key] = PadState.NAMES[map[t]] else c.remove(key)
        }
        onChanged()
    }

    fun resetAll() {
        val c = config()
        for (t in 0 until PadState.COUNT) c.remove("pad.${PadState.NAMES[t]}")
        stop()
        onChanged()
    }

    fun show() {
        val map = PadState.map(config())
        prompt.visibility = if (listening >= 0) View.VISIBLE else View.GONE
        if (listening >= 0) {
            val left = ((ControllerSetup.LISTEN_MS - (SystemClock.uptimeMillis() - since)) / 1000 + 1).coerceIn(0, 10)
            prompt.text = context.getString(R.string.remap_listening, names[listening], left)
        }
        for (t in 0 until PadState.COUNT) {
            rows.set(t, if (t == listening) "…" else short[map[t]], map[t] == t, t == listening)
        }
    }
}
