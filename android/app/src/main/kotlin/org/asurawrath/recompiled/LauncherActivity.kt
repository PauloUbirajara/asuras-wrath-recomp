package org.asurawrath.recompiled

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.ImageView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.GravityCompat
import androidx.core.view.isVisible
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.slider.Slider
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import java.io.DataInputStream
import java.io.File
import java.io.InputStream

/**
 * The launcher on Android, laid out as the desktop one: a sidebar with the
 * pages and Play (a drawer behind the menu button in portrait), the page on
 * the parchment panel. Game data (the game and Files folders, the shader
 * cache, built here by ShaderBuildService or imported), Settings (the main
 * ones) and About. It edits launcher.cfg
 * (LauncherConfig) and starts GameActivity; controls, achievements and the
 * other settings are in the in-game menu, which Back opens.
 */
class LauncherActivity : AppCompatActivity() {

    private lateinit var folders: AppFolders
    private lateinit var config: LauncherConfig
    private var binding = false  // filling the views: their listeners don't save
    private var drawer: DrawerLayout? = null  // portrait only

    private enum class Page(val view: Int, val menu: Int, val title: Int) {
        GAME(R.id.page_game, R.id.menu_game, R.string.page_game),
        SETTINGS(R.id.page_settings, R.id.menu_settings, R.string.page_settings),
        ABOUT(R.id.page_about, R.id.menu_about, R.string.page_about),
    }
    private var page = Page.GAME

    private val pickGame = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let { folderPicked(it, files = false) }
    }
    private val pickFiles = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let { folderPicked(it, files = true) }
    }
    private val pickShaders = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let(::importShaders)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        folders = AppFolders(this)
        folders.prepare()
        config = LauncherConfig.load(folders.config)
        setContentView(R.layout.activity_launcher)
        LauncherArt.applyFonts(window.decorView)
        LauncherArt.bitmap(this, "logo.png")?.let { findViewById<ImageView>(R.id.logo).setImageBitmap(it) }
        setUpPages(savedInstanceState)
        setUpSettings()
        findViewById<Button>(R.id.game_choose).setOnClickListener { choose(pickGame) }
        findViewById<Button>(R.id.files_choose).setOnClickListener { choose(pickFiles) }
        findViewById<Button>(R.id.files_default).setOnClickListener {
            config["data_folder"] = ""
            save()
        }
        findViewById<Button>(R.id.shaders_import).setOnClickListener {
            pickShaders.launch(arrayOf("*/*"))
        }
        findViewById<Button>(R.id.shaders_build).setOnClickListener {
            if (ShaderBuildService.status.state == ShaderBuildService.State.RUNNING) {
                ShaderBuildService.stop(this)
            } else {
                buildShaders(fresh = false)
            }
        }
        findViewById<Button>(R.id.shaders_start_over).setOnClickListener { buildShaders(fresh = true) }
        findViewById<Button>(R.id.play).setOnClickListener { play() }
        findViewById<TextView>(R.id.about_version).text =
            getString(R.string.about_version, BuildConfig.VERSION_NAME, BuildConfig.SHADER_CACHE_VERSION)
    }

    override fun onResume() {
        super.onResume()
        ShaderBuildService.listener = { status ->
            showShaderBuild(status)
            if (status.state != ShaderBuildService.State.RUNNING) refresh()
        }
        // The game may have changed the settings (its in-game menu).
        config = LauncherConfig.load(folders.config)
        refresh()
        val restart = folders.restartRequest
        if (restart.isFile) {
            val what = restart.readText().trim()
            restart.delete()
            if (what == "game") startGame()
        }
    }

    override fun onPause() {
        ShaderBuildService.listener = null
        super.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_PAGE, page.name)
    }

    // --- Pages, menu -----------------------------------------------------------

    private fun setUpPages(savedInstanceState: Bundle?) {
        drawer = findViewById(R.id.drawer)
        findViewById<View>(R.id.menu_button).apply {
            isVisible = drawer != null
            setOnClickListener { drawer?.openDrawer(GravityCompat.START) }
        }
        for (p in Page.entries) findViewById<View>(p.menu).setOnClickListener { showPage(p) }
        // Back closes the drawer first.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                val d = drawer
                if (d != null && d.isDrawerOpen(GravityCompat.START)) {
                    d.closeDrawer(GravityCompat.START)
                } else {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            }
        })
        val saved = savedInstanceState?.getString(KEY_PAGE)
        showPage(Page.entries.firstOrNull { it.name == saved } ?: Page.GAME)
    }

    private fun showPage(p: Page) {
        page = p
        for (other in Page.entries) {
            findViewById<View>(other.view).isVisible = other == p
            findViewById<MenuButton>(other.menu).current = other == p
        }
        findViewById<TextView>(R.id.page_title).setText(p.title)
        drawer?.closeDrawer(GravityCompat.START)
    }

    // --- Folders ---------------------------------------------------------------

    /** The game folder: launcher.cfg's, else extracted/ when the game is there. */
    private fun gameFolder(): File? =
        config["game_data"]?.takeIf { it.isNotEmpty() }?.let(::File)
            ?: folders.defaultGame.takeIf { File(it, "default.xex").isFile }

    private fun choose(picker: androidx.activity.result.ActivityResultLauncher<Uri?>) {
        if (!StorageAccess.granted(this)) {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.access_title)
                .setMessage(R.string.access_text)
                .setPositiveButton(R.string.open_settings) { _, _ -> StorageAccess.request(this) }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return
        }
        picker.launch(null)
    }

    private fun folderPicked(uri: Uri, files: Boolean) {
        val path = StorageAccess.treePath(uri)
        if (path == null) {
            toast(getString(R.string.folder_unsupported))
            return
        }
        if (files) {
            if (!path.isDirectory && !path.mkdirs() || !path.canWrite()) {
                toast(getString(R.string.files_not_writable))
                return
            }
            config["data_folder"] = if (path == folders.defaultFiles) "" else path.absolutePath
        } else {
            config["game_data"] = path.absolutePath
        }
        save()
    }

    // --- Shader cache ----------------------------------------------------------

    private enum class ShaderState { READY, MISSING, OUTDATED, UNKNOWN }

    /** As ShaderCacheFitFor (src/ui/launcher.cpp), from the file's header. */
    private fun shaderState(file: File): Pair<ShaderState, Int> {
        val version = if (file.isFile) file.inputStream().use(::cacheVersion) ?: -1 else 0
        if (version == 0) return ShaderState.MISSING to 0
        if (version < BuildConfig.SHADER_CACHE_MIN_VERSION || version > BuildConfig.SHADER_CACHE_VERSION) {
            return ShaderState.UNKNOWN to version
        }
        // A render scale other than 100% needs format 8 (scaled resolves).
        val needed = if (config.int("render_scale", 100) != 100) 8 else BuildConfig.SHADER_CACHE_MIN_VERSION
        return (if (version < needed) ShaderState.OUTDATED else ShaderState.READY) to version
    }

    /** The version in a cache's header ("MSHC", then a little-endian uint32), or null. */
    private fun cacheVersion(input: InputStream): Int? {
        val header = ByteArray(8)
        if (DataInputStream(input).runCatching { readFully(header) }.isFailure) return null
        if (String(header, 0, 4, Charsets.US_ASCII) != "MSHC") return null
        return (header[4].toInt() and 0xff) or ((header[5].toInt() and 0xff) shl 8) or
            ((header[6].toInt() and 0xff) shl 16) or ((header[7].toInt() and 0xff) shl 24)
    }

    private fun importShaders(uri: Uri) {
        val target = folders.shaderCache(config)
        val version = contentResolver.openInputStream(uri)?.use(::cacheVersion)
        if (version == null || version < BuildConfig.SHADER_CACHE_MIN_VERSION ||
            version > BuildConfig.SHADER_CACHE_VERSION
        ) {
            toast(getString(R.string.shaders_import_failed, uri.lastPathSegment ?: ""))
            return
        }
        try {
            target.parentFile?.mkdirs()
            val temp = File(target.parentFile, target.name + ".import")
            contentResolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { input.copyTo(it) }
            }
            if (!temp.renameTo(target)) throw IllegalStateException("rename failed")
            toast(getString(R.string.shaders_imported))
        } catch (e: Exception) {
            toast(getString(R.string.shaders_import_failed, e.message ?: ""))
        }
        refresh()
    }

    /** Starts the build (a stopped one continues unless `fresh`). */
    private fun buildShaders(fresh: Boolean) {
        val game = gameFolder()
        if (game == null || !File(game, "default.xex").isFile) {
            toast(getString(R.string.game_needed))
            return
        }
        if (!game.absolutePath.startsWith(folders.root.absolutePath) && !StorageAccess.granted(this)) {
            choose(pickGame)
            return
        }
        // The progress notification (asked once; the build runs without it too).
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        ShaderBuildService.start(this, game, folders.shaderCache(config), fresh)
    }

    /** The build's progress line and buttons (as the desktop launcher's). */
    private fun showShaderBuild(s: ShaderBuildService.Status) {
        val running = s.state == ShaderBuildService.State.RUNNING
        val progress = findViewById<LinearProgressIndicator>(R.id.shaders_progress)
        progress.isVisible = running
        when {
            s.total > 0 -> { progress.isIndeterminate = false; progress.max = s.total; progress.progress = s.done }
            s.packages > 0 -> { progress.isIndeterminate = false; progress.max = s.packages; progress.progress = s.read }
            else -> if (!progress.isIndeterminate) {
                progress.isVisible = false
                progress.isIndeterminate = true
                progress.isVisible = running
            }
        }
        val text = ShaderBuildService.statusText(this, s)
        findViewById<TextView>(R.id.shaders_build_status).apply {
            isVisible = text.isNotEmpty()
            this.text = text
        }
        val cache = folders.shaderCache(config)
        val stoppedAt = if (running) 0 else ShaderBuildService.stoppedAt(cache)
        findViewById<Button>(R.id.shaders_build).text = when {
            running -> getString(R.string.shaders_build_stop)
            stoppedAt > 0 -> getString(R.string.shaders_build_continue, stoppedAt)
            shaderState(cache).first == ShaderState.MISSING -> getString(R.string.shaders_build)
            else -> getString(R.string.shaders_rebuild)
        }
        // "Rebuild" starts over by itself; a stopped build can be dropped.
        findViewById<Button>(R.id.shaders_start_over).isVisible = stoppedAt > 0
        findViewById<Button>(R.id.shaders_import).isEnabled = !running
    }

    // --- Settings --------------------------------------------------------------

    // The languages on the disc (XLanguage ids), as the desktop launcher lists them.
    private val languages = listOf(3 to "Deutsch", 1 to "English", 5 to "Español", 4 to "Français", 6 to "Italiano", 2 to "日本語")

    private fun setUpSettings() {
        val language = findViewById<MaterialAutoCompleteTextView>(R.id.language)
        language.setAdapter(ArrayAdapter(this, android.R.layout.simple_list_item_1, languages.map { it.second }))
        language.setOnItemClickListener { _, _, position, _ ->
            config["language"] = languages[position].first.toString()
            save()
        }

        findViewById<Slider>(R.id.render_scale).addOnChangeListener { _, value, fromUser ->
            showRenderScale(value.toInt())
            if (fromUser && !binding) {
                config["render_scale"] = value.toInt().toString()
                save()
            }
        }
        findViewById<Slider>(R.id.vibration).addOnChangeListener { _, value, fromUser ->
            findViewById<TextView>(R.id.vibration_value).text = getString(R.string.vibration_value, value.toInt())
            if (fromUser && !binding) {
                config["vibration"] = value.toInt().toString()
                save()
            }
        }
        val fpsButtons = mapOf(R.id.fps_30 to 30, R.id.fps_60 to 60, R.id.fps_120 to 120, R.id.fps_unlimited to 0)
        findViewById<MaterialButtonToggleGroup>(R.id.fps_cap).addOnButtonCheckedListener { _, id, checked ->
            if (checked && !binding) {
                config["fps_cap"] = fpsButtons.getValue(id).toString()
                save()
            }
        }
        val aspectButtons = mapOf(R.id.aspect_fit to "fit", R.id.aspect_fill to "fill", R.id.aspect_stretch to "stretch")
        findViewById<MaterialButtonToggleGroup>(R.id.aspect).addOnButtonCheckedListener { _, id, checked ->
            if (checked && !binding) {
                config["aspect"] = aspectButtons.getValue(id)
                save()
            }
        }
        for ((id, key) in switches) {
            findViewById<MaterialSwitch>(id).setOnCheckedChangeListener { _, checked ->
                if (!binding) {
                    config.setBool(key, checked)
                    save()
                }
            }
        }
    }

    // Switch -> launcher.cfg key, and the native launcher's default.
    private val switches = listOf(R.id.vsync to "vsync", R.id.show_fps to "show_fps", R.id.skip_intro to "skip_intro", R.id.toasts to "toasts")
    private val switchDefaults = mapOf("vsync" to true, "show_fps" to false, "skip_intro" to false, "toasts" to true)

    private fun showRenderScale(percent: Int) {
        val width = (1280 * percent + 50) / 100
        val height = (720 * percent + 50) / 100
        findViewById<TextView>(R.id.render_scale_value).text = getString(
            if (percent == 100) R.string.render_resolution_game else R.string.render_resolution_value,
            percent, width, height,
        )
    }

    // --- State -> views --------------------------------------------------------

    private fun save() {
        config.save()
        refresh()
    }

    private fun refresh() {
        binding = true
        try {
            refreshGame()
            refreshShaders()
            findViewById<TextView>(R.id.files_path).text = folders.files(config).absolutePath

            val language = config.int("language", 1)
            findViewById<MaterialAutoCompleteTextView>(R.id.language)
                .setText(languages.firstOrNull { it.first == language }?.second ?: "English", false)
            val renderScale = config.int("render_scale", 100).coerceIn(25, 200) / 5 * 5
            findViewById<Slider>(R.id.render_scale).value = renderScale.toFloat()
            showRenderScale(config.int("render_scale", 100))
            val vibration = config.int("vibration", 100).coerceIn(0, 200) / 10 * 10
            findViewById<Slider>(R.id.vibration).value = vibration.toFloat()
            findViewById<MaterialButtonToggleGroup>(R.id.fps_cap).check(
                when (config.int("fps_cap", 30)) {
                    0 -> R.id.fps_unlimited
                    in 1..45 -> R.id.fps_30
                    in 46..90 -> R.id.fps_60
                    else -> R.id.fps_120
                },
            )
            findViewById<MaterialButtonToggleGroup>(R.id.aspect).check(
                when (config["aspect"]) {
                    "fill" -> R.id.aspect_fill
                    "stretch" -> R.id.aspect_stretch
                    else -> R.id.aspect_fit
                },
            )
            for ((id, key) in switches) {
                findViewById<MaterialSwitch>(id).isChecked = config.bool(key, switchDefaults.getValue(key))
            }
        } finally {
            binding = false
        }
    }

    private fun refreshGame() {
        val game = gameFolder()
        val path = findViewById<TextView>(R.id.game_path)
        val status = findViewById<TextView>(R.id.game_status)
        path.text = game?.absolutePath ?: getString(R.string.game_not_chosen)
        val found = game != null && File(game, "default.xex").isFile
        status.isVisible = game != null
        status.text = getString(if (found) R.string.game_found else R.string.game_missing)
        status.setTextColor(ContextCompat.getColor(this, if (found) R.color.good else R.color.bad))
    }

    private fun refreshShaders() {
        showShaderBuild(ShaderBuildService.status)
        val (state, version) = shaderState(folders.shaderCache(config))
        val status = findViewById<TextView>(R.id.shaders_status)
        status.text = when (state) {
            ShaderState.READY -> getString(R.string.shaders_ready, version)
            ShaderState.MISSING -> getString(R.string.shaders_missing)
            ShaderState.OUTDATED -> getString(R.string.shaders_outdated, version)
            ShaderState.UNKNOWN -> getString(R.string.shaders_unknown, version)
        }
        status.setTextColor(ContextCompat.getColor(this, if (state == ShaderState.READY) R.color.good else R.color.bad))
        // As the desktop sidebar: what is missing, above Play (and a dot on
        // the menu button when the sidebar is a drawer).
        val game = gameFolder()
        val text = when {
            game == null || !File(game, "default.xex").isFile -> getString(R.string.notice_game)
            state != ShaderState.READY -> getString(R.string.notice_shaders)
            else -> null
        }
        val notice = findViewById<TextView>(R.id.play_notice)
        notice.isVisible = text != null
        notice.text = text
        findViewById<View>(R.id.menu_notice).isVisible = text != null && drawer != null
    }

    // --- Play ------------------------------------------------------------------

    private fun play() {
        // The game reads the cache at start; the build is still writing it.
        if (ShaderBuildService.status.state == ShaderBuildService.State.RUNNING) {
            toast(getString(R.string.shaders_building))
            return
        }
        val game = gameFolder()
        if (game == null || !File(game, "default.xex").isFile) {
            toast(getString(R.string.game_needed))
            return
        }
        // A folder outside the app's own needs the access the picker asked for.
        if (!game.absolutePath.startsWith(folders.root.absolutePath) && !StorageAccess.granted(this)) {
            choose(pickGame)
            return
        }
        if (config["game_data"].isNullOrEmpty()) {
            config["game_data"] = game.absolutePath
            config.save()
        }
        if (shaderState(folders.shaderCache(config)).first != ShaderState.READY) {
            MaterialAlertDialogBuilder(this)
                .setMessage(R.string.shaders_needed)
                .setPositiveButton(R.string.play_anyway) { _, _ -> startGame() }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return
        }
        startGame()
    }

    private fun startGame() {
        startActivity(Intent(this, GameActivity::class.java))
    }

    private fun toast(text: String) = Toast.makeText(this, text, Toast.LENGTH_LONG).show()

    companion object {
        private const val KEY_PAGE = "page"
    }
}
