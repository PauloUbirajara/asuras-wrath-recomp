package org.asurawrath.recompiled

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.opengl.EGL14
import android.opengl.GLES20
import android.os.Build
import android.util.Log
import android.view.WindowManager
import java.util.Locale

/**
 * About › This device (as src/ui/about_info.cpp SystemInfo for a computer):
 * the phone, Android, the processor, memory, the GPU and the screen. Each
 * row has its English label too, for Copy (bug reports are read in English).
 */
object DeviceInfo {
    data class Row(val label: Int, val english: String, val value: String)

    private var gpu: String? = null

    fun rows(context: Context): List<Row> {
        val rows = mutableListOf<Row>()
        rows += Row(R.string.info_device, "Device", "${Build.MANUFACTURER} ${Build.MODEL}".trim())
        rows += Row(R.string.info_android, "Android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        val soc = if (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL != Build.UNKNOWN) {
            "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}"
        } else {
            Build.HARDWARE
        }
        rows += Row(R.string.info_processor, "Processor",
            "$soc, ${Runtime.getRuntime().availableProcessors()} cores, ${Build.SUPPORTED_ABIS.firstOrNull() ?: "?"}")
        val memory = ActivityManager.MemoryInfo()
        (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(memory)
        rows += Row(R.string.info_memory, "Memory", String.format(Locale.ROOT, "%.1f GB", memory.totalMem / 1e9))
        rows += Row(R.string.info_gpu, "GPU", listOfNotNull(gpu(), vulkan(context)).joinToString(", "))
        val display = (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).let { wm ->
            val bounds = wm.maximumWindowMetrics.bounds
            val hz = context.display?.refreshRate ?: 0f
            String.format(Locale.ROOT, "%d × %d, %.0f Hz", maxOf(bounds.width(), bounds.height()),
                minOf(bounds.width(), bounds.height()), hz)
        }
        rows += Row(R.string.info_screen, "Screen", display)
        return rows
    }

    /** The text Copy puts on the clipboard (in English, as the desktop's). */
    fun copyText(context: Context): String = buildString {
        append("Asura's Wrath Recompiled ").append(BuildConfig.GIT_VERSION.ifEmpty { BuildConfig.VERSION_NAME }).append(" (Android)\n")
        if (BuildConfig.GIT_COMMIT.isNotEmpty()) {
            append("Commit: ${BuildConfig.GIT_COMMIT} (${BuildConfig.GIT_BRANCH}, ${BuildConfig.GIT_DATE})\n")
        }
        append("Built: ${BuildConfig.BUILD_DATE}, shader cache format ${BuildConfig.SHADER_CACHE_VERSION}\n")
        for (row in rows(context)) append("${row.english}: ${row.value}\n")
    }

    /** GL_RENDERER from a throwaway EGL context (the GPU's name; read once). */
    private fun gpu(): String? {
        gpu?.let { return it }
        try {
            val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val version = IntArray(2)
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) return null
            val configs = arrayOfNulls<android.opengl.EGLConfig>(1)
            val count = IntArray(1)
            val attributes = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE,
            )
            EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0)
            if (count[0] == 0) return null
            val context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
            val surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
            EGL14.eglMakeCurrent(display, surface, surface, context)
            gpu = GLES20.glGetString(GLES20.GL_RENDERER)
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroySurface(display, surface)
            EGL14.eglDestroyContext(display, context)
            EGL14.eglTerminate(display)
        } catch (e: Exception) {
            Log.w(AppFolders.TAG, "about: no GPU name", e)
        }
        return gpu
    }

    /** "Vulkan 1.3" from the system feature (the driver's version). */
    private fun vulkan(context: Context): String? {
        val feature = context.packageManager.systemAvailableFeatures
            .firstOrNull { it.name == PackageManager.FEATURE_VULKAN_HARDWARE_VERSION } ?: return null
        val v = feature.version
        return "Vulkan ${v shr 22}.${(v shr 12) and 0x3ff}"
    }
}
