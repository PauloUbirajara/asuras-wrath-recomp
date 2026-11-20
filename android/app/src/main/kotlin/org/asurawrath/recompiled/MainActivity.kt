package org.asurawrath.recompiled

import android.os.Bundle
import android.system.Os
import android.util.Log
import org.libsdl.app.SDLActivity
import java.io.File

/**
 * The whole app: SDL's activity running the native launcher and game
 * (libasura_wrath.so, the same code as the desktop build).
 *
 * Everything lives in the app's external files folder
 * (Android/data/org.asurawrath.recompiled/files), reachable over USB:
 *   extracted/            the game (default.xex and the rest), copied there by the player
 *   asura_wrath_recomp/   launcher.cfg, saves, cache/ (shader cache), logs/
 *   assets/               the launcher's art, unpacked from the APK
 */
class MainActivity : SDLActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        prepareFolders()
        super.onCreate(savedInstanceState)
    }

    // SDL is linked into librexruntime.so (its JNI_OnLoad registers SDL's
    // natives); the last library is the one whose SDL_main runs.
    override fun getLibraries(): Array<String> =
        arrayOf("c++_shared", "rexruntime", "asura_wrath")

    private fun prepareFolders() {
        val root = getExternalFilesDir(null) ?: filesDir
        File(root, "extracted").mkdirs()
        // platform_android.cpp: ExePath() = $ASURA_APP_DIR/asura_wrath, so the
        // launcher finds assets/ and extracted/ next to that.
        Os.setenv("ASURA_APP_DIR", root.absolutePath, true)
        // platform_posix.cpp / the SDK: the user folder is $XDG_DATA_HOME/asura_wrath_recomp.
        Os.setenv("XDG_DATA_HOME", root.absolutePath, true)
        Os.setenv("XDG_CACHE_HOME", cacheDir.absolutePath, true)
        Os.setenv("HOME", root.absolutePath, true)
        unpackAssets(root)
    }

    // The APK's assets/ (the repo's assets/ folder) as files, once per install.
    private fun unpackAssets(root: File) {
        val stamp = File(root, "assets/.installed")
        val version = packageManager.getPackageInfo(packageName, 0).lastUpdateTime.toString()
        if (stamp.isFile && stamp.readText() == version) return
        try {
            copyAssetDir("", File(root, "assets"))
            stamp.writeText(version)
        } catch (e: Exception) {
            Log.e(TAG, "unpacking the assets failed", e)
        }
    }

    private fun copyAssetDir(path: String, to: File) {
        val names = assets.list(path) ?: return
        if (names.isEmpty() && path.isNotEmpty()) {
            to.parentFile?.mkdirs()
            assets.open(path).use { input -> to.outputStream().use { input.copyTo(it) } }
            return
        }
        to.mkdirs()
        for (name in names) {
            // The APK's own entries (images/, webkit/ ...) are not ours.
            if (path.isEmpty() && name != "ui") continue
            copyAssetDir(if (path.isEmpty()) name else "$path/$name", File(to, name))
        }
    }

    companion object {
        private const val TAG = "AsurasWrath"
    }
}
