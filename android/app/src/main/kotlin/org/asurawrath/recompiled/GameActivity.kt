package org.asurawrath.recompiled

import android.content.Intent
import android.os.Bundle
import android.os.Process
import java.io.File
import org.libsdl.app.SDLActivity

/**
 * The game: SDL's activity running libasura_wrath.so (the same code as the
 * desktop build), in its own process (AndroidManifest.xml). It starts
 * straight into the game with launcher.cfg's settings; Back opens the
 * in-game menu (InGameMenuActivity, through openMenu), and quitting there
 * ends this process.
 */
class GameActivity : SDLActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        val folders = AppFolders(this)
        folders.prepare()
        // The settings the game started with: the menu's Resume asks for a
        // restart when one that only applies at start has changed.
        startConfig = LauncherConfig.load(folders.config)
        // Notes from the in-game menu (LauncherActivity openGameMenu, the toast
        // example) are for the game they were left in: none from an earlier one.
        for (note in listOf(".open_game_menu", ".toast_example")) File(folders.root, note).delete()
        current = this
        super.onCreate(savedInstanceState)
    }

    override fun onResume() {
        super.onResume()
        applyScreenPadding()
    }

    /**
     * Settings › Display › Screen padding: a black border around the game
     * (the window's background), so the picture isn't flush with the
     * screen's rounded edges. SDL's surface shrinks to fit inside it; read
     * again after the in-game menu.
     */
    private fun applyScreenPadding() {
        val layout = mLayout ?: return
        val dp = LauncherConfig.load(AppFolders(this).config).int("screen_padding", 0).coerceIn(0, 64)
        val px = (dp * resources.displayMetrics.density).toInt()
        if (layout.paddingLeft != px) layout.setPadding(px, px, px, px)
    }

    /** Back in game (asura_wrath_app.h OpenAndroidMenu, on SDL's thread): the menu over the game, which pauses. */
    @Suppress("unused")
    fun openMenu() {
        runOnUiThread {
            startActivity(Intent(this, InGameMenuActivity::class.java))
            // Over the game at once, no slide-in (the theme has no window animation either).
            @Suppress("DEPRECATION") overridePendingTransition(0, 0)
        }
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

    companion object {
        /** The running game (this process has one). */
        var current: GameActivity? = null
            private set
        var startConfig: LauncherConfig? = null
            private set

        /** asura_wrath_app.h: the menu closed (reload launcher.cfg, maybe open the game menu). */
        @JvmStatic external fun nativeMenuClosed()
    }
}

/**
 * The menu Back opens in game: the launcher's pages, in the game's process
 * over the paused game, with Resume (Play's place), Restart and Quit. Its
 * Game menu entry opens the game's own (ImGui) menu, for what this one
 * hasn't: the button remap, the keyboard, achievements.
 */
class InGameMenuActivity : LauncherActivity() {
    override val inGame = true

    override fun finish() {
        super.finish()
        @Suppress("DEPRECATION") overridePendingTransition(0, 0)
    }

    override fun onDestroy() {
        super.onDestroy()
        // Not when the game ends (Quit, Restart): only when it carries on.
        if (GameActivity.current?.isFinishing == false) {
            try { GameActivity.nativeMenuClosed() } catch (e: UnsatisfiedLinkError) { }
        }
    }
}
