package org.asurawrath.recompiled

import android.content.Context
import android.system.Os
import android.util.Log
import java.io.File

/**
 * Where everything lives: the app's external files folder
 * (Android/data/org.asurawrath.recompiled/files), reachable over USB.
 *
 *   launcher.cfg          the launcher's settings (the game reads them too)
 *   extracted/            a place to copy the game to, found by default
 *   asura_wrath_recomp/   the default Files folder: saves, cache/ (shader cache), logs/
 *   assets/               the launcher's art and the fan translations, unpacked from the APK
 */
class AppFolders(private val context: Context) {
    val root: File = context.getExternalFilesDir(null) ?: context.filesDir

    val config: File get() = File(root, "launcher.cfg")
    val defaultGame: File get() = File(root, "extracted")
    val defaultFiles: File get() = File(root, "asura_wrath_recomp")

    // An in-game Restart (platform_android.cpp ReplaceProcess) leaves this and
    // ends the game's process; the launcher starts the game again.
    val restartRequest: File get() = File(root, ".restart")

    /** The Files folder launcher.cfg names, else the default. */
    fun files(config: LauncherConfig): File =
        config["data_folder"]?.takeIf { it.isNotEmpty() }?.let(::File) ?: defaultFiles

    fun shaderCache(config: LauncherConfig): File = File(files(config), "cache/mantra_shaders.mshc")

    /** A fan translation's unpacked files (src/ui/translation.h), null if `language` isn't one. */
    fun translation(language: Int): File? =
        if (language == LANGUAGE_PT_BR) File(root, "assets/translations/pt-BR") else null

    /**
     * Folders, the environment the native code reads, and the unpacked art.
     * Both processes call it: the game's before its library loads.
     */
    fun prepare() {
        defaultGame.mkdirs()
        defaultFiles.mkdirs()
        // platform_android.cpp: ExePath() = $ASURA_APP_DIR/asura_wrath, so
        // assets/ and launcher.cfg (LauncherDir) are found next to that.
        Os.setenv("ASURA_APP_DIR", root.absolutePath, true)
        // The SDK's user folder: $XDG_DATA_HOME/asura_wrath_recomp.
        Os.setenv("XDG_DATA_HOME", root.absolutePath, true)
        Os.setenv("XDG_CACHE_HOME", context.cacheDir.absolutePath, true)
        Os.setenv("HOME", root.absolutePath, true)
        // Custom GPU drivers (GpuDrivers.kt, src/platform/gpu_driver.cpp):
        // where they're installed, and where libadrenotools' hooks are.
        Os.setenv("ASURA_GPU_DRIVERS", GpuDrivers(context).dir.absolutePath, true)
        Os.setenv("ASURA_NATIVE_LIB_DIR", context.applicationInfo.nativeLibraryDir, true)
        unpackAssets()
    }

    // The APK's assets/ui and assets/translations (the repo's assets/ folder)
    // as files, once per install: the fan translations' game files take the
    // place of the game's own (src/game/translation.cpp).
    private fun unpackAssets() {
        val stamp = File(root, "assets/.installed")
        val version = context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString()
        if (stamp.isFile && stamp.readText() == version) return
        try {
            copyAssetDir("ui", File(root, "assets/ui"))
            File(root, "assets/translations").deleteRecursively()
            copyAssetDir("translations", File(root, "assets/translations"))
            stamp.writeText(version)
        } catch (e: Exception) {
            Log.e(TAG, "unpacking the assets failed", e)
        }
    }

    private fun copyAssetDir(path: String, to: File) {
        val names = context.assets.list(path) ?: return
        if (names.isEmpty()) {
            to.parentFile?.mkdirs()
            context.assets.open(path).use { input -> to.outputStream().use { input.copyTo(it) } }
            return
        }
        to.mkdirs()
        for (name in names) copyAssetDir("$path/$name", File(to, name))
    }

    companion object {
        const val TAG = "AsurasWrath"
        /** launcher.cfg language= for Português (Brasil), the fan translation (src/ui/translation.h). */
        const val LANGUAGE_PT_BR = 9
    }
}
