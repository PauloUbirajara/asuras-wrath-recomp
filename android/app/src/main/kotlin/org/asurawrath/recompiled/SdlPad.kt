package org.asurawrath.recompiled

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.File
import kotlin.math.max

/**
 * A controller as the game's SDL numbers it on Android, from Android's own
 * input events: the same numbering as SDL's Java side (SDLControllerManager:
 * the axes sorted by its RangeComparator, the hat as D-pad buttons) and its
 * native side (SDL_sysjoystick.c keycode_to_SDL), so a mapping written here
 * ("a:b0,leftx:a0,righttrigger:a4,...") means the same to the game.
 *
 * Controls › Controller setup saves one per controller model in
 * controller_setup_android.txt beside launcher.cfg:
 *   <vendor>,<product>,<name>,<SDL mapping fields>
 * (src/input/controller_mappings.cpp ApplyAndroidControllerSetup adds it
 * under SDL's GUID for that controller).
 */
object SdlPad {

    /** The axes SDL reports, in its order (index = SDL axis number); the hat isn't one. */
    fun axes(d: InputDevice): List<InputDevice.MotionRange> {
        val seen = HashSet<Int>()
        return d.motionRanges.sortedWith { a, b -> sortKey(a.axis) - sortKey(b.axis) }
            .filter { it.source and InputDevice.SOURCE_CLASS_JOYSTICK != 0 && seen.add(it.axis) }
            .filter { it.axis != MotionEvent.AXIS_HAT_X && it.axis != MotionEvent.AXIS_HAT_Y }
    }

    // SDLJoystickHandler.RangeComparator: GAS and BRAKE swapped, Z between RY and RZ.
    private fun sortKey(axis: Int): Int {
        var a = when (axis) {
            MotionEvent.AXIS_GAS -> MotionEvent.AXIS_BRAKE
            MotionEvent.AXIS_BRAKE -> MotionEvent.AXIS_GAS
            else -> axis
        }
        if (a == MotionEvent.AXIS_Z) a = MotionEvent.AXIS_RZ - 1
        else if (a > MotionEvent.AXIS_Z && a < MotionEvent.AXIS_RZ) a--
        return a
    }

    fun hasHat(d: InputDevice): Boolean = d.motionRanges.any {
        it.source and InputDevice.SOURCE_CLASS_JOYSTICK != 0 && it.axis == MotionEvent.AXIS_HAT_X
    }

    /** An axis as SDL reads it: -1..1 over the axis's range. */
    fun value(range: InputDevice.MotionRange, event: MotionEvent): Float =
        (event.getAxisValue(range.axis) - range.min) / range.range * 2f - 1f

    /** SDL's button number for a key (keycode_to_SDL, then scancode_to_SDL), or -1. */
    fun button(keyCode: Int, scanCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> 0
        KeyEvent.KEYCODE_BUTTON_B -> 1
        KeyEvent.KEYCODE_BUTTON_X -> 2
        KeyEvent.KEYCODE_BUTTON_Y -> 3
        KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_SELECT -> 4
        KeyEvent.KEYCODE_BUTTON_MODE -> 5
        KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_BUTTON_START -> 6
        KeyEvent.KEYCODE_BUTTON_THUMBL -> 7
        KeyEvent.KEYCODE_BUTTON_THUMBR -> 8
        KeyEvent.KEYCODE_BUTTON_L1 -> 9
        KeyEvent.KEYCODE_BUTTON_R1 -> 10
        KeyEvent.KEYCODE_DPAD_UP -> 11
        KeyEvent.KEYCODE_DPAD_DOWN -> 12
        KeyEvent.KEYCODE_DPAD_LEFT -> 13
        KeyEvent.KEYCODE_DPAD_RIGHT -> 14
        KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_MEDIA_RECORD -> 15
        KeyEvent.KEYCODE_BUTTON_R2 -> 16
        KeyEvent.KEYCODE_BUTTON_C -> 17
        KeyEvent.KEYCODE_BUTTON_Z -> 18
        in KeyEvent.KEYCODE_BUTTON_1..KeyEvent.KEYCODE_BUTTON_16 -> 20 + keyCode - KeyEvent.KEYCODE_BUTTON_1
        else -> when (scanCode) {
            0x220 -> 11; 0x221 -> 12; 0x222 -> 13; 0x223 -> 14  // BTN_DPAD_*
            else -> -1
        }
    }

    const val BUTTONS = 36

    /**
     * SDL's own mapping for an Android controller with no other
     * (SDL_CreateMappingForAndroidGamepad): the fields, from the buttons the
     * controller says it has and how many axes it has.
     */
    fun defaultFields(d: InputDevice): LinkedHashMap<String, String> {
        val keys = intArrayOf(
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_L1,
            KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_DPAD_CENTER,
        )
        val bits = intArrayOf(0, 1, 2, 3, 4, 6, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 4, 0)
        var mask = 0
        d.hasKeys(*keys).forEachIndexed { i, has -> if (has) mask = mask or (1 shl bits[i]) }
        if (hasHat(d)) mask = mask or (0xF shl 11)
        val axes = axes(d).size
        val out = LinkedHashMap<String, String>()
        fun b(bit: Int, field: String) { if (mask and (1 shl bit) != 0) out[field] = "b$bit" }
        b(0, "a")
        if (mask and (1 shl 1) != 0) out["b"] = "b1"
        else if (mask and (1 shl 4) != 0) { out["b"] = "b4"; mask = mask and (1 shl 4).inv() }
        b(2, "x"); b(3, "y"); b(4, "back")
        if (android.os.Build.VERSION.SDK_INT >= 30) b(5, "guide")
        b(6, "start"); b(7, "leftstick"); b(8, "rightstick"); b(9, "leftshoulder"); b(10, "rightshoulder")
        b(11, "dpup"); b(12, "dpdown"); b(13, "dpleft"); b(14, "dpright")
        if (axes >= 2) { out["leftx"] = "a0"; out["lefty"] = "a1" }
        if (axes >= 4) { out["rightx"] = "a2"; out["righty"] = "a3" }
        if (axes >= 6) { out["lefttrigger"] = "a4"; out["righttrigger"] = "a5" }
        return out
    }

    /** A controller model: as the game matches it (by USB ids, else by name). */
    data class Model(val vendor: Int, val product: Int, val name: String) {
        fun matches(v: Int, p: Int, n: String) =
            if (vendor != 0 || product != 0) v == vendor && p == product else clean(n) == name
    }

    fun model(d: InputDevice) = Model(d.vendorId, d.productId, clean(d.name))

    private fun clean(name: String) = name.replace(',', ' ').trim()

    /** controller_setup_android.txt: the setups saved, a line each. */
    class Setups(private val file: File) {
        private fun lines(): List<String> =
            if (file.isFile) file.readLines().filter { it.isNotBlank() && !it.startsWith("#") } else emptyList()

        private fun parse(line: String): Pair<Model, String>? {
            val parts = line.split(',', limit = 4)
            if (parts.size < 4) return null
            return Model(parts[0].trim().toIntOrNull() ?: 0, parts[1].trim().toIntOrNull() ?: 0, clean(parts[2])) to parts[3]
        }

        /** The fields saved for this controller, or null. */
        fun fields(d: InputDevice): LinkedHashMap<String, String>? {
            val m = model(d)
            for (line in lines()) {
                val (model, body) = parse(line) ?: continue
                if (model.matches(m.vendor, m.product, m.name)) return parseFields(body)
            }
            return null
        }

        fun save(d: InputDevice, fields: Map<String, String>) = write(d, buildFields(fields))

        fun remove(d: InputDevice) = write(d, null)

        fun count() = lines().size

        private fun write(d: InputDevice, body: String?) {
            val m = model(d)
            val kept = lines().filter { line -> parse(line)?.first?.let { it.matches(m.vendor, m.product, m.name) } != true }
            val out = StringBuilder("# Controls › Controller setup (Android): vendor,product,name,SDL mapping fields\n")
            for (line in kept) out.append(line).append('\n')
            if (body != null) out.append("${m.vendor},${m.product},${m.name},$body\n")
            val temp = File(file.path + ".tmp")
            temp.writeText(out.toString())
            temp.renameTo(file)
        }
    }

    /** "a:b0,leftx:a0,..." -> fields (stick axes split into halves: "-leftx" -> "-a0"). */
    fun parseFields(body: String): LinkedHashMap<String, String> {
        val out = LinkedHashMap<String, String>()
        for (field in body.split(',')) {
            val colon = field.indexOf(':')
            if (colon <= 0) continue
            val key = field.substring(0, colon)
            val value = field.substring(colon + 1)
            if (key in STICKS && value.startsWith("a")) {
                val inverted = value.endsWith("~")
                val axis = value.substring(1, value.length - if (inverted) 1 else 0)
                out["-$key"] = (if (inverted) "+a" else "-a") + axis
                out["+$key"] = (if (inverted) "-a" else "+a") + axis
            } else {
                out[key] = value
            }
        }
        return out
    }

    /** Fields -> "a:b0,...,leftx:a0," (a stick's halves joined when they are one axis's). */
    fun buildFields(fields: Map<String, String>): String {
        val out = StringBuilder()
        for ((key, value) in fields) {
            if (value.isEmpty() || key.trimStart('-', '+') in STICKS) continue
            out.append(key).append(':').append(value).append(',')
        }
        for (stick in STICKS) {
            val neg = fields["-$stick"].orEmpty()
            val pos = fields["+$stick"].orEmpty()
            if (neg.length > 2 && pos.length > 2 && neg.substring(1) == pos.substring(1) && neg[1] == 'a') {
                if (neg[0] == '-' && pos[0] == '+') { out.append("$stick:a${neg.substring(2)},"); continue }
                if (neg[0] == '+' && pos[0] == '-') { out.append("$stick:a${neg.substring(2)}~,"); continue }
            }
            if (neg.isNotEmpty()) out.append("-$stick:$neg,")
            if (pos.isNotEmpty()) out.append("+$stick:$pos,")
            // A whole axis kept as it was (not split: "leftx:b3" and the like).
            fields[stick]?.takeIf { it.isNotEmpty() && neg.isEmpty() && pos.isEmpty() }?.let { out.append("$stick:$it,") }
        }
        return out.toString()
    }

    val STICKS = listOf("leftx", "lefty", "rightx", "righty")

    /**
     * The raw state of a controller, numbered as SDL: buttons (the hat as
     * D-pad buttons 11..14) and axes (-1..1). Fed with the activity's events.
     */
    class Raw {
        var device: InputDevice? = null
            private set
        private var ranges: List<InputDevice.MotionRange> = emptyList()
        val buttons = BooleanArray(BUTTONS)
        var axes = FloatArray(0)
            private set
        /** The axes have been read (Android sends nothing until one moves). */
        var axesKnown = false
            private set

        private fun use(d: InputDevice) {
            if (device?.id == d.id) return
            device = d
            ranges = axes(d)
            buttons.fill(false)
            // Until a move: centred axes at 0, the others (0..1 triggers) at -1.
            axes = FloatArray(ranges.size) { i -> if (ranges[i].min < 0f) 0f else -1f }
            axesKnown = false
        }

        fun onKey(event: KeyEvent): Boolean {
            val d = event.device ?: return false
            if (!PadState.isPad(d)) return false
            val b = button(event.keyCode, event.scanCode)
            if (b < 0) return false
            use(d)
            when (event.action) {
                KeyEvent.ACTION_DOWN -> buttons[b] = true
                KeyEvent.ACTION_UP -> buttons[b] = false
            }
            return true
        }

        fun onMotion(event: MotionEvent): Boolean {
            if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.action != MotionEvent.ACTION_MOVE) return false
            val d = event.device ?: return false
            use(d)
            for (i in ranges.indices) axes[i] = value(ranges[i], event)
            axesKnown = true
            if (hasHat(d)) {
                val hx = Math.round(event.getAxisValue(MotionEvent.AXIS_HAT_X))
                val hy = Math.round(event.getAxisValue(MotionEvent.AXIS_HAT_Y))
                buttons[13] = hx < 0; buttons[14] = hx > 0
                buttons[11] = hy < 0; buttons[12] = hy > 0
            }
            return true
        }

        /** How far an input of a mapping is pressed, 0..1 ("b3", "+a2", "-a2", "a4", "a4~"). */
        fun read(input: String?): Float {
            if (input.isNullOrEmpty()) return 0f
            return when {
                input[0] == 'b' -> if (buttons.getOrElse(input.substring(1).toIntOrNull() ?: -1) { false }) 1f else 0f
                input[0] == '+' || input[0] == '-' -> {
                    val v = axes.getOrElse(input.substring(2).toIntOrNull() ?: -1) { 0f }
                    max(0f, if (input[0] == '+') v else -v)
                }
                input[0] == 'a' -> {
                    val inverted = input.endsWith("~")
                    val v = axes.getOrElse(input.substring(1, input.length - if (inverted) 1 else 0).toIntOrNull() ?: -1) { -1f }
                    ((if (inverted) -v else v) + 1f) / 2f
                }
                else -> 0f
            }
        }
    }
}
