package org.asurawrath.recompiled

import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.slider.Slider

/**
 * Controls › Touch controls › Edit layout: the touch controls full screen,
 * as over the game (landscape, the screen padding too). Drag one to move it;
 * the picked one's size is the slider. Saved to launcher.cfg's touch_layout
 * when leaving (Done or Back); Reset puts them all back.
 */
class TouchLayoutActivity : AppCompatActivity() {

    private lateinit var config: LauncherConfig
    private lateinit var controls: TouchControlsView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val folders = AppFolders(this)
        config = LauncherConfig.load(folders.config)
        val dp = resources.displayMetrics.density

        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        val root = FrameLayout(this).apply { setBackgroundColor(0xFF1E1A16.toInt()) }
        // As GameActivity.applyScreenPadding: the controls sit inside the padding.
        val padding = (config.int("screen_padding", 0).coerceIn(0, 64) * dp).toInt()
        root.setPadding(padding, padding, padding, padding)
        controls = TouchControlsView(this, editing = true)
        root.addView(controls, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        // The editor's bar, in the middle (where the default layout has none).
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setBackgroundResource(R.drawable.row_background)
            setPadding((14 * dp).toInt(), (10 * dp).toInt(), (14 * dp).toInt(), (10 * dp).toInt())
        }
        val picked = TextView(this).apply {
            setTextAppearance(R.style.Text_Paper_Body)
            text = getString(R.string.touch_layout_pick)
            gravity = Gravity.CENTER
        }
        bar.addView(picked)
        val slider = Slider(this).apply {
            valueFrom = TouchControlsView.MIN_SCALE.toFloat()
            valueTo = TouchControlsView.MAX_SCALE.toFloat()
            stepSize = 10f
            value = 100f
            isEnabled = false
            labelBehavior = com.google.android.material.slider.LabelFormatter.LABEL_GONE
            addOnChangeListener { _, v, fromUser ->
                if (fromUser) controls.setSelectedScale(v.toInt())
                controls.selected?.let { picked.text = getString(R.string.touch_layout_size, name(it), v.toInt()) }
            }
        }
        bar.addView(slider, LinearLayout.LayoutParams((220 * dp).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun button(text: Int, onClick: () -> Unit) = MaterialButton(this, null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
            setText(text)
            setOnClickListener { onClick() }
        }
        buttons.addView(button(R.string.touch_layout_reset) { controls.resetLayout() })
        buttons.addView(button(R.string.touch_layout_done) { finish() },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                marginStart = (8 * dp).toInt()
            })
        bar.addView(buttons)
        root.addView(bar, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.CENTER))
        LauncherArt.applyFonts(bar)

        controls.onSelect = { c ->
            slider.isEnabled = true
            slider.value = (Math.round(c.scale * 10) * 10).toFloat().coerceIn(slider.valueFrom, slider.valueTo)
            picked.text = getString(R.string.touch_layout_size, name(c), slider.value.toInt())
        }
        setContentView(root)
        controls.reload(config)
    }

    private fun name(c: TouchControlsView.Control): String = when (c.id) {
        "ls" -> getString(R.string.touch_left_stick)
        "rs" -> getString(R.string.touch_right_stick)
        "hide" -> getString(R.string.touch_hide)
        "back" -> "Back"
        "start" -> "Start"
        else -> c.id.uppercase()
    }

    override fun onPause() {
        super.onPause()
        // Saved as it is when leaving (Done, Back, another app).
        val layout = controls.layoutText()
        config = LauncherConfig.load(AppFolders(this).config)
        if (layout.isEmpty()) config.remove("touch_layout") else config["touch_layout"] = layout
        config.save()
    }
}
