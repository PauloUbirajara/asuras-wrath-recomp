package org.asurawrath.recompiled

import android.view.InputDevice
import android.view.MotionEvent
import kotlin.math.abs
import kotlin.math.max

/**
 * A controller's axes as Android reports them: which pair of Z/RZ and RX/RY
 * is its right stick and which its triggers, for Controls › Test (PadState).
 * The game reads pads through SDL, set up per controller with Controls ›
 * Controller setup.
 */
class PadAxes {
    /**
     * The axis pairs (of Z/RZ and RX/RY) that are this pad's triggers, not its
     * right stick. Android reports Z/RZ/RX/RY as centred axes (-1..1) whatever
     * the pad uses them for, so a trigger there rests at -1, or (on pads that
     * also report LTRIGGER/RTRIGGER) moves with them.
     */
    val triggerPairs = mutableSetOf<Pair<Int, Int>>()
    private val followCount = mutableMapOf<Pair<Int, Int>, Int>()
    var learnedFor = -1
        private set

    /** Learns from a joystick event; true when the trigger pairs changed. */
    fun learn(d: InputDevice, event: MotionEvent): Boolean {
        val before = triggerPairs.toSet()
        if (d.id != learnedFor) {
            learnedFor = d.id
            triggerPairs.clear()
            followCount.clear()
            for (p in PadState.STICK_PAIRS) {
                val a = d.getMotionRange(p.first, InputDevice.SOURCE_JOYSTICK) ?: continue
                val b = d.getMotionRange(p.second, InputDevice.SOURCE_JOYSTICK) ?: continue
                if (a.min >= 0f && b.min >= 0f) triggerPairs += p
            }
            // At rest (the first event, before any trigger is pulled), a
            // trigger on a centred axis reads -1 and a stick 0.
            for (p in PadState.STICK_PAIRS) {
                if (!PadState.hasAxis(d, p.first) || !PadState.hasAxis(d, p.second)) continue
                if (event.getAxisValue(p.first) < -0.9f && event.getAxisValue(p.second) < -0.9f) triggerPairs += p
            }
        }
        // A pair that follows LTRIGGER/RTRIGGER (both pulled) is the triggers again.
        val l = max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        val r = max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
        if (l > 0.5f || r > 0.5f) {
            for (p in PadState.STICK_PAIRS) {
                if (p in triggerPairs || !PadState.hasAxis(d, p.first) || !PadState.hasAxis(d, p.second)) continue
                fun follows(axis: Int, t: Float): Boolean {
                    val v = event.getAxisValue(axis)
                    return abs(v - t) < 0.15f || abs((v + 1f) / 2f - t) < 0.15f
                }
                // Over several events, so a right stick held in a corner while
                // both triggers are pulled isn't taken for them.
                if (follows(p.first, l) && follows(p.second, r)) {
                    val n = (followCount[p] ?: 0) + 1
                    followCount[p] = n
                    if (n >= 8) triggerPairs += p
                } else {
                    followCount[p] = 0
                }
            }
        }
        return triggerPairs != before
    }

    /** The right stick's pair: the first of Z/RZ, RX/RY present and not the triggers. */
    fun stick(d: InputDevice?): Pair<Int, Int>? = PadState.STICK_PAIRS.firstOrNull { p ->
        PadState.hasAxis(d, p.first) && PadState.hasAxis(d, p.second) && p !in triggerPairs
    }

}
