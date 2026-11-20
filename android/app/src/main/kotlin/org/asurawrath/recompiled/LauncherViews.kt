package org.asurawrath.recompiled

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.LightingColorFilter
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.util.AttributeSet
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import androidx.appcompat.widget.AppCompatTextView
import androidx.core.content.ContextCompat
import kotlin.math.min

/** The launcher's art and fonts, from the APK's assets/ui (the desktop launcher's files). */
object LauncherArt {
    private val bitmaps = HashMap<String, Bitmap?>()
    private var regular: Typeface? = null
    private var bold: Typeface? = null

    fun bitmap(context: Context, name: String): Bitmap? = bitmaps.getOrPut(name) {
        try {
            context.assets.open("ui/$name").use(BitmapFactory::decodeStream)
        } catch (e: Exception) {
            null
        }
    }

    private fun font(context: Context, name: String): Typeface? = try {
        Typeface.createFromAsset(context.assets, "ui/$name")
    } catch (e: Exception) {
        null
    }

    fun regularFont(context: Context): Typeface? {
        if (regular == null) regular = font(context, "NotoSerif-Regular.ttf")
        return regular
    }

    /** Noto Serif (the desktop launcher's font) on every text under `view`. */
    fun applyFonts(view: View) {
        val context = view.context
        if (regular == null) regular = font(context, "NotoSerif-Regular.ttf")
        if (bold == null) bold = font(context, "NotoSerif-Bold.ttf")
        fun walk(v: View) {
            if (v is TextView) {
                val isBold = v.typeface?.isBold == true
                (if (isBold) bold else regular)?.let { v.typeface = it }
            }
            if (v is ViewGroup) for (i in 0 until v.childCount) walk(v.getChildAt(i))
        }
        walk(view)
    }
}

/**
 * The parchment panel: launcher_background.png nine-sliced as the desktop
 * launcher does (LauncherDialog::DrawPanel). The frame keeps its look at any
 * size; the children (a header, then the scrolling page) go inside its inner
 * panel.
 */
class PanelLayout @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) :
    FrameLayout(context, attrs) {

    private val image = LauncherArt.bitmap(context, "launcher_background.png")
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val inner = Rect()
    private val xs = FloatArray(4)
    private val ys = FloatArray(4)
    private val source = Rect()
    private val target = RectF()

    init {
        setWillNotDraw(false)
    }

    /** In game: no parchment image, the panel's plain colour over the dimmed game. */
    var plain = false
        set(value) {
            field = value
            requestLayout()
            invalidate()
        }

    private fun layoutFrame(w: Int, h: Int) {
        val density = resources.displayMetrics.density
        val bitmap = if (plain) null else image
        if (bitmap == null) {
            val margin = (16 * density).toInt()
            inner.set(margin, margin, w - margin, h - margin)
            return
        }
        val iw = bitmap.width.toFloat()
        val ih = bitmap.height.toFloat()
        // The desktop's scale, but on a phone no wider a frame than ~24dp
        // beside the page, so the page keeps its width.
        val k = min(min(w / iw, h / ih) * 0.8f, 24 * density / INNER[0])
        xs[0] = 0f; xs[1] = SLICE[0] * k; xs[2] = w - SLICE[2] * k; xs[3] = w.toFloat()
        ys[0] = 0f; ys[1] = SLICE[1] * k; ys[2] = h - SLICE[3] * k; ys[3] = h.toFloat()
        fun map(v: Float, size: Float, s0: Float, s1: Float, screen: FloatArray): Float = when {
            v <= s0 -> screen[0] + (screen[1] - screen[0]) * v / s0
            v >= size - s1 -> screen[2] + (screen[3] - screen[2]) * (v - (size - s1)) / s1
            else -> screen[1] + (screen[2] - screen[1]) * (v - s0) / (size - s0 - s1)
        }
        inner.set(
            map(INNER[0], iw, SLICE[0], SLICE[2], xs).toInt(),
            map(INNER[1], ih, SLICE[1], SLICE[3], ys).toInt(),
            map(INNER[2], iw, SLICE[0], SLICE[2], xs).toInt(),
            map(INNER[3], ih, SLICE[1], SLICE[3], ys).toInt(),
        )
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        layoutFrame(w, h)
        val pad = (12 * resources.displayMetrics.density).toInt()
        val contentW = inner.width() - 2 * pad
        var used = 0
        for (i in 0 until childCount) {
            val child = getChildAt(i)
            if (child.visibility == View.GONE) continue
            val last = i == childCount - 1
            val heightSpec = if (last) {
                MeasureSpec.makeMeasureSpec(maxOf(0, inner.height() - used - pad), MeasureSpec.EXACTLY)
            } else {
                MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED)
            }
            child.measure(MeasureSpec.makeMeasureSpec(maxOf(0, contentW), MeasureSpec.EXACTLY), heightSpec)
            used += child.measuredHeight
        }
        setMeasuredDimension(w, h)
    }

    override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
        val pad = (12 * resources.displayMetrics.density).toInt()
        var y = inner.top + pad
        for (i in 0 until childCount) {
            val child = getChildAt(i)
            if (child.visibility == View.GONE) continue
            child.layout(inner.left + pad, y, inner.left + pad + child.measuredWidth, y + child.measuredHeight)
            y += child.measuredHeight
        }
    }

    override fun onDraw(canvas: Canvas) {
        val bitmap = (if (plain) null else image) ?: return super.onDraw(canvas)
        val iw = bitmap.width
        val ih = bitmap.height
        val us = intArrayOf(0, SLICE[0].toInt(), iw - SLICE[2].toInt(), iw)
        val vs = intArrayOf(0, SLICE[1].toInt(), ih - SLICE[3].toInt(), ih)
        for (j in 0 until 3) {
            for (i in 0 until 3) {
                source.set(us[i], vs[j], us[i + 1], vs[j + 1])
                target.set(xs[i], ys[j], xs[i + 1], ys[j + 1])
                canvas.drawBitmap(bitmap, source, target, paint)
            }
        }
    }

    companion object {
        // In launcher_background.png's pixels (2000x1125), as DrawPanel.
        private val SLICE = floatArrayOf(200f, 140f, 200f, 140f)  // left, top, right, bottom
        private val INNER = floatArrayOf(222f, 165f, 1778f, 952f)  // the inner panel
    }
}

/**
 * A sidebar button: menu_button.png's band repeated along the button,
 * warm when it is the page shown (LauncherDialog::SidebarButton).
 */
class MenuButton @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) :
    AppCompatTextView(context, attrs) {

    private val band = LauncherArt.bitmap(context, "menu_button.png")
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val matrix = Matrix()

    var current = false
        set(value) {
            field = value
            setTextColor(ContextCompat.getColor(context, if (value) R.color.sidebar_text_selected else R.color.sidebar_text))
            invalidate()
        }

    init {
        gravity = Gravity.CENTER
        textSize = 18f
        isClickable = true
        isFocusable = true
        current = false
        band?.let { paint.shader = BitmapShader(it, Shader.TileMode.REPEAT, Shader.TileMode.CLAMP) }
    }

    override fun setPressed(pressed: Boolean) {
        super.setPressed(pressed)
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        band?.let {
            val scale = height.toFloat() / it.height
            matrix.setScale(scale, scale)
            paint.shader.setLocalMatrix(matrix)
            // Multiplied: grey normally, white pressed, warm when current.
            val tint = when {
                current -> 0xFFC799
                isPressed -> 0xFFFFFF
                else -> 0xCCCCCC
            }
            paint.colorFilter = LightingColorFilter(tint, 0)
            canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        }
        super.onDraw(canvas)
    }
}
