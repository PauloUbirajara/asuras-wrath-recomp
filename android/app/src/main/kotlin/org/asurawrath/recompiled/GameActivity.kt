package org.asurawrath.recompiled

import android.os.Bundle
import android.os.Process
import org.libsdl.app.SDLActivity

/**
 * The game: SDL's activity running libasura_wrath.so (the same code as the
 * desktop build), in its own process (AndroidManifest.xml). It starts
 * straight into the game with launcher.cfg's settings; Back opens the
 * in-game menu, and quitting there ends this process.
 */
class GameActivity : SDLActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        AppFolders(this).prepare()
        super.onCreate(savedInstanceState)
    }

    // SDL is linked into librexruntime.so (its JNI_OnLoad registers SDL's
    // natives); the last library is the one whose SDL_main runs.
    override fun getLibraries(): Array<String> =
        arrayOf("c++_shared", "rexruntime", "asura_wrath")

    // LauncherActivity is the launcher here: the game itself, full screen.
    override fun getArguments(): Array<String> =
        arrayOf("--launcher=false", "--fullscreen=true")

    override fun onDestroy() {
        super.onDestroy()
        // The native runtime can't start twice in one process: the next Play
        // gets a new one.
        if (isFinishing) Process.killProcess(Process.myPid())
    }
}
