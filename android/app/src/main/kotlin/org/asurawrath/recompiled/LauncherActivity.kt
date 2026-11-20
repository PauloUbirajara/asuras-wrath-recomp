package org.asurawrath.recompiled

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.text.format.DateFormat
import android.text.format.Formatter
import android.util.TypedValue
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.appcompat.widget.TooltipCompat
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.os.LocaleListCompat
import androidx.core.view.GravityCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import androidx.core.widget.NestedScrollView
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.slider.Slider
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import java.io.DataInputStream
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.util.Locale

/**
 * The launcher on Android, laid out as the desktop one: a sidebar with the
 * pages and Play (a drawer behind the menu button in portrait), the page on
 * the parchment panel. A page with sections lists them first (each with a
 * line of its state); a section then shows alone, the title a breadcrumb
 * back to the list (Game data › Shader cache). Pages, as the desktop's:
 *
 *   Game data   Folders, Shader cache (built here by ShaderBuildService or
 *               imported), Saves, Logs; Traces and Progress listed, not yet
 *               on Android
 *   Controls    Test (the controller live), Controller (vibration, deadzones)
 *   Settings    System (language: also this launcher's), Display, Enhancements
 *   About       the build, this device (Copy), the notice
 *
 * It edits launcher.cfg (LauncherConfig) and starts GameActivity; the button
 * remap and the other settings are in the in-game menu, which Back opens.
 */
open class LauncherActivity : AppCompatActivity() {

    /** InGameMenuActivity: over the running game (Resume, Restart, Quit; no shader build). */
    protected open val inGame = false

    private lateinit var folders: AppFolders
    private lateinit var config: LauncherConfig
    private var binding = false  // filling the views: their listeners don't save
    private var drawer: DrawerLayout? = null  // portrait only

    /** A section of a page: its view, its name; `enabled` false: listed, greyed (not on Android yet). */
    private enum class Section(val view: Int, val title: Int, val enabled: Boolean = true) {
        FOLDERS(R.id.section_folders, R.string.section_folders),
        SHADERS(R.id.section_shaders, R.string.section_shaders),
        SAVES(R.id.section_saves, R.string.section_saves),
        LOGS(R.id.section_logs, R.string.section_logs),
        TRACES(0, R.string.section_traces, enabled = false),
        PROGRESS(0, R.string.section_progress, enabled = false),
        TEST(R.id.section_test, R.string.section_test),
        CONTROLLER(R.id.section_controller, R.string.section_controller),
        SYSTEM(R.id.section_system, R.string.section_system),
        DISPLAY(R.id.section_display, R.string.section_display),
        ENHANCEMENTS(R.id.section_enhancements, R.string.section_enhancements),
    }

    private enum class Page(val menu: Int, val title: Int, val sections: List<Section>, val view: Int = 0) {
        GAME(R.id.menu_game, R.string.page_game,
            listOf(Section.FOLDERS, Section.SHADERS, Section.SAVES, Section.LOGS, Section.TRACES, Section.PROGRESS)),
        CONTROLS(R.id.menu_controls, R.string.page_controls, listOf(Section.TEST, Section.CONTROLLER)),
        SETTINGS(R.id.menu_settings, R.string.page_settings, listOf(Section.SYSTEM, Section.DISPLAY, Section.ENHANCEMENTS)),
        ABOUT(R.id.menu_about, R.string.page_about, emptyList(), R.id.page_about),
    }
    private var page = Page.GAME
    private var section: Section? = null
    private val summaries = HashMap<Section, TextView>()  // the list's state lines

    private val pad = PadState()
    private val inputListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshPad()
        override fun onInputDeviceRemoved(deviceId: Int) {
            if (pad.device?.id == deviceId) pad.device = null
            refreshPad()
        }
        override fun onInputDeviceChanged(deviceId: Int) = refreshPad()
    }

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
        // The launcher's language is the game's (Settings › System); set
        // again at each start (Android before 13 doesn't keep it).
        applyLanguage()
        // In game (landscape, over the paused game): the desktop's layout, the
        // sidebar docked beside the page, the game dimmed behind both.
        val wide = inGame && resources.configuration.screenWidthDp > resources.configuration.screenHeightDp
        setContentView(if (wide) R.layout.activity_launcher_wide else R.layout.activity_launcher)
        // A phone in landscape: the sidebar's logo and big buttons don't fit its height.
        val c = resources.configuration
        if (c.screenWidthDp > c.screenHeightDp && c.screenHeightDp < 520) compactSidebar()
        if (inGame) hideSystemBars()
        LauncherArt.applyFonts(window.decorView)
        LauncherArt.bitmap(this, "logo.png")?.let { findViewById<ImageView>(R.id.logo).setImageBitmap(it) }
        setUpPages(savedInstanceState)
        setUpSettings()
        setUpControls()
        setUpAbout()
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
        findViewById<Button>(R.id.play).setOnClickListener { if (inGame) resume() else play() }
        if (inGame) setUpInGame()
        findViewById<TextView>(R.id.play_notice).setOnClickListener {
            showPage(Page.GAME, if (gameFound()) Section.SHADERS else Section.FOLDERS)
        }
    }

    override fun onResume() {
        super.onResume()
        ShaderBuildService.listener = { status ->
            showShaderBuild(status)
            if (status.state != ShaderBuildService.State.RUNNING) refresh()
        }
        (getSystemService(Context.INPUT_SERVICE) as InputManager).registerInputDeviceListener(inputListener, null)
        // The game may have changed the settings (its in-game menu).
        config = LauncherConfig.load(folders.config)
        applyLanguage()
        refresh()
        val restart = folders.restartRequest
        if (!inGame && restart.isFile) {
            val what = restart.readText().trim()
            restart.delete()
            if (what == "game") startGame()
        }
    }

    override fun onPause() {
        ShaderBuildService.listener = null
        (getSystemService(Context.INPUT_SERVICE) as InputManager).unregisterInputDeviceListener(inputListener)
        super.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_PAGE, page.name)
        outState.putString(KEY_SECTION, section?.name)
    }

    // --- Language --------------------------------------------------------------

    // The languages on the disc (XLanguage ids, as launcher.cfg's language=)
    // and the launcher's own translations of them.
    private val languages = listOf(
        3 to ("Deutsch" to "de"), 1 to ("English" to "en"), 5 to ("Español" to "es"),
        4 to ("Français" to "fr"), 6 to ("Italiano" to "it"), 2 to ("日本語" to "ja"),
    )

    /** Sets the app's language to launcher.cfg's (AppCompat recreates the activity when it changes). */
    private fun applyLanguage() {
        val id = config.int("language", 1)
        val tag = languages.firstOrNull { it.first == id }?.second?.second ?: "en"
        if (AppCompatDelegate.getApplicationLocales().toLanguageTags() != tag) {
            AppCompatDelegate.setApplicationLocales(LocaleListCompat.forLanguageTags(tag))
        }
    }

    // --- Pages, sections, breadcrumb ------------------------------------------

    private fun setUpPages(savedInstanceState: Bundle?) {
        drawer = findViewById(R.id.drawer)
        findViewById<View>(R.id.menu_button).apply {
            isVisible = drawer != null
            setOnClickListener { drawer?.openDrawer(GravityCompat.START) }
        }
        for (p in Page.entries) findViewById<View>(p.menu).setOnClickListener { showPage(p) }
        // The page's name in a section's breadcrumb goes back to its list.
        findViewById<TextView>(R.id.page_title).setOnClickListener { if (section != null) showPage(page) }
        // Back: closes the drawer, then leaves a section for its list.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                val d = drawer
                when {
                    d != null && d.isDrawerOpen(GravityCompat.START) -> d.closeDrawer(GravityCompat.START)
                    section != null -> showPage(page)
                    inGame -> resume()
                    else -> {
                        isEnabled = false
                        onBackPressedDispatcher.onBackPressed()
                        isEnabled = true
                    }
                }
            }
        })
        val p = Page.entries.firstOrNull { it.name == savedInstanceState?.getString(KEY_PAGE) } ?: Page.GAME
        val s = Section.entries.firstOrNull { it.name == savedInstanceState?.getString(KEY_SECTION) }
        showPage(p, s)
    }

    /** Shows a page: its section `s`, else its list (or its own view). */
    private fun showPage(p: Page, s: Section? = null) {
        page = p
        section = s?.takeIf { it in p.sections && it.enabled }
        for (other in Page.entries) findViewById<MenuButton>(other.menu).current = other == p
        val shown = section?.view ?: p.view.takeIf { it != 0 } ?: R.id.section_list
        val pages = findViewById<ViewGroup>(R.id.section_list).parent as ViewGroup
        for (i in 0 until pages.childCount) pages.getChildAt(i).isVisible = pages.getChildAt(i).id == shown
        if (shown == R.id.section_list) showList()
        // The title: "Game data", or "Game data › Shader cache".
        val title = findViewById<TextView>(R.id.page_title)
        title.setText(p.title)
        title.isClickable = section != null
        title.background = if (section != null) selectable() else null
        title.setTextColor(ContextCompat.getColor(this, if (section != null) R.color.panel_text_dim else R.color.panel_text))
        findViewById<View>(R.id.crumb_sep).isVisible = section != null
        findViewById<TextView>(R.id.section_title).apply {
            isVisible = section != null
            section?.let { setText(it.title) }
        }
        findViewById<NestedScrollView>(R.id.panel_scroll).scrollTo(0, 0)
        drawer?.closeDrawer(GravityCompat.START)
        when (section) {
            Section.SAVES -> refreshSaves()
            Section.LOGS -> refreshLogs()
            Section.TEST -> refreshPad()
            else -> {}
        }
    }

    private fun selectable() = TypedValue().let {
        theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true)
        ContextCompat.getDrawable(this, it.resourceId)
    }

    /** The page's sections as rows: the name, a line of its state, a chevron. */
    private fun showList() {
        val list = findViewById<LinearLayout>(R.id.section_list)
        list.removeAllViews()
        summaries.clear()
        for (s in page.sections) {
            list.addView(row(getString(s.title), summary(s), s.enabled, { summaries[s] = it }) { showPage(page, s) })
        }
        if (inGame && (page == Page.SETTINGS || page == Page.CONTROLS)) {
            // What this menu hasn't: the game's own menu, over the game.
            list.addView(row(getString(R.string.game_menu), getString(R.string.game_menu_summary)) { openGameMenu() })
        } else if (page == Page.SETTINGS || page == Page.CONTROLS) {
            list.addView(hint(getString(if (page == Page.SETTINGS) R.string.settings_more else R.string.remap_hint)))
        }
        LauncherArt.applyFonts(list)
    }

    private fun row(title: String, summary: String?, enabled: Boolean = true, summaryView: ((TextView) -> Unit)? = null,
                    onClick: (() -> Unit)?): View {
        val dp = resources.displayMetrics.density
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = (56 * dp).toInt()
            setPadding((14 * dp).toInt(), (10 * dp).toInt(), (10 * dp).toInt(), (10 * dp).toInt())
            background = ContextCompat.getDrawable(context, R.drawable.row_background)
            isEnabled = enabled
            alpha = if (enabled) 1f else 0.5f
            if (enabled && onClick != null) setOnClickListener { onClick() }
        }
        val texts = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(TextView(this).apply {
            setTextAppearance(R.style.Text_Paper_RowTitle)
            text = title
        })
        if (!summary.isNullOrEmpty()) {
            texts.addView(TextView(this).apply {
                setTextAppearance(R.style.Text_Paper_Hint)
                text = summary
                summaryView?.invoke(this)
            })
        }
        row.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (onClick != null) {
            row.addView(TextView(this).apply {
                text = "›"
                textSize = 26f
                setTextColor(ContextCompat.getColor(context, R.color.panel_text_dim))
                setPadding((8 * dp).toInt(), 0, 0, 0)
            })
        }
        return row
    }

    private fun hint(text: String) = TextView(this).apply {
        setTextAppearance(R.style.Text_Paper_Hint)
        this.text = text
        setPadding(0, (8 * resources.displayMetrics.density).toInt(), 0, 0)
    }

    /** A section's line in the list. */
    private fun summary(s: Section): String? = when (s) {
        Section.FOLDERS -> gameFolder()?.let {
            if (gameFound()) getString(R.string.summary_game, it.name) else getString(R.string.game_missing_short)
        } ?: getString(R.string.game_not_chosen)
        Section.SHADERS -> if (ShaderBuildService.status.state == ShaderBuildService.State.RUNNING) {
            ShaderBuildService.statusText(this, ShaderBuildService.status)
        } else {
            shaderText(shaderState(folders.shaderCache(config)))
        }
        Section.SAVES -> scanSaves().maxByOrNull { it.modified }?.let {
            getString(R.string.summary_saves, DateFormat.getDateFormat(this).format(it.modified) + " " +
                DateFormat.getTimeFormat(this).format(it.modified))
        } ?: getString(R.string.saves_none_short)
        Section.LOGS -> logFiles().size.let { resources.getQuantityString(R.plurals.summary_logs, it, it) }
        Section.TRACES, Section.PROGRESS -> getString(R.string.not_on_android)
        Section.TEST -> PadState.connectedPad()?.let { getString(R.string.pad_connected, it.name) }
            ?: getString(R.string.pad_none_short)
        Section.CONTROLLER -> getString(R.string.summary_controller, config.int("vibration", 100),
            percent(config.double("deadzone_left")), percent(config.double("deadzone_right")))
        Section.SYSTEM -> languageName()
        Section.DISPLAY -> getString(R.string.summary_display, config.int("render_scale", 100),
            getString(aspectNames.getValue(config["aspect"] ?: "fit")))
        Section.ENHANCEMENTS -> config.int("fps_cap", 30).let {
            getString(R.string.summary_enhancements, if (it == 0) getString(R.string.fps_unlimited) else it.toString())
        }
    }

    private fun percent(fraction: Double) = (fraction * 100 + 0.5).toInt()

    // --- Folders ---------------------------------------------------------------

    /** The game folder: launcher.cfg's, else extracted/ when the game is there. */
    private fun gameFolder(): File? =
        config["game_data"]?.takeIf { it.isNotEmpty() }?.let(::File)
            ?: folders.defaultGame.takeIf { File(it, "default.xex").isFile }

    private fun gameFound() = gameFolder()?.let { File(it, "default.xex").isFile } == true

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

    private fun shaderText(state: Pair<ShaderState, Int>): String = when (state.first) {
        ShaderState.READY -> getString(R.string.shaders_ready, Formatter.formatShortFileSize(this, folders.shaderCache(config).length()))
        ShaderState.MISSING -> getString(R.string.shaders_missing)
        ShaderState.OUTDATED -> getString(R.string.shaders_outdated, state.second)
        ShaderState.UNKNOWN -> getString(R.string.shaders_unknown, state.second)
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
        if (game == null || !gameFound()) {
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
        findViewById<Button>(R.id.shaders_import).isEnabled = !running && !inGame
        if (inGame) {
            // The game reads the cache; no build or import under it.
            findViewById<Button>(R.id.shaders_build).isEnabled = false
            findViewById<Button>(R.id.shaders_start_over).isEnabled = false
            findViewById<TextView>(R.id.shaders_build_status).apply {
                isVisible = true
                setText(R.string.shaders_in_game)
            }
        }
        summaries[Section.SHADERS]?.text = summary(Section.SHADERS)
    }

    // --- Saves -----------------------------------------------------------------

    private data class Save(val name: String, val profile: String, val folder: File, val header: File,
                            val bytes: Long, val modified: Long, val files: List<File>)

    // As LauncherDialog::ScanSaves: <Files>/<profile xuid>/43430817/00000001/<save>/.
    private fun scanSaves(): List<Save> {
        val files = folders.files(config)
        val profiles = files.listFiles { f -> f.isDirectory && f.name.length == 16 && f.name.all { it.isLetterOrDigit() && it.lowercaseChar() in "0123456789abcdef" } }
            ?: return emptyList()
        return profiles.flatMap { profile ->
            val dir = File(profile, "43430817/00000001")
            (dir.listFiles(File::isDirectory) ?: emptyArray()).map { save ->
                val all = save.walkTopDown().filter(File::isFile).toList()
                Save(save.name, profile.name, save, File(profile, "43430817/Headers/00000001/${save.name}.header"),
                    all.sumOf(File::length), all.maxOfOrNull(File::lastModified) ?: save.lastModified(),
                    (save.listFiles(File::isFile) ?: emptyArray()).sortedBy(File::getName))
            }
        }
    }

    private fun refreshSaves() {
        val list = findViewById<LinearLayout>(R.id.saves_list)
        list.removeAllViews()
        val saves = scanSaves()
        if (saves.isEmpty()) {
            list.addView(body(getString(R.string.saves_none)))
            list.addView(labelled(getString(R.string.folder), folders.files(config).absolutePath))
        }
        for (save in saves) {
            val card = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            card.addView(TextView(this).apply { setTextAppearance(R.style.Text_Paper_Heading); text = save.name })
            card.addView(labelled(getString(R.string.save_last), DateFormat.getDateFormat(this).format(save.modified) +
                " " + DateFormat.getTimeFormat(this).format(save.modified)))
            card.addView(labelled(getString(R.string.save_size), Formatter.formatShortFileSize(this, save.bytes)))
            card.addView(labelled(getString(R.string.save_profile), save.profile))
            card.addView(labelled(getString(R.string.folder), save.folder.absolutePath +
                "\n" + save.files.joinToString(", ") { "${it.name} (${Formatter.formatShortFileSize(this, it.length())})" }))
            card.addView((LayoutInflater.from(this).inflate(R.layout.paper_button, card, false) as Button).apply {
                text = getString(R.string.save_delete)
                setOnClickListener { deleteSave(save) }
            })
            list.addView(card)
        }
        LauncherArt.applyFonts(list)
    }

    private fun deleteSave(save: Save) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.save_delete_title)
            .setMessage(R.string.save_delete_text)
            .setPositiveButton(R.string.save_delete) { _, _ ->
                val ok = save.folder.deleteRecursively().also { if (it) save.header.delete() }
                toast(getString(if (ok) R.string.save_deleted else R.string.save_delete_failed))
                refreshSaves()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun body(text: String) = TextView(this).apply {
        setTextAppearance(R.style.Text_Paper_Body)
        this.text = text
    }

    /** A small label over its value (the desktop's label column, on a phone's width). */
    private fun labelled(label: String, value: String) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, (6 * resources.displayMetrics.density).toInt(), 0, 0)
        addView(TextView(context).apply { setTextAppearance(R.style.Text_Paper_Label); text = label })
        addView(TextView(context).apply { setTextAppearance(R.style.Text_Paper_Path); text = value; setTextIsSelectable(true) })
    }

    // --- Logs ------------------------------------------------------------------

    private fun logDir() = File(folders.files(config), "logs")

    private fun logFiles(): List<File> =
        (logDir().listFiles { f -> f.isFile && f.name.endsWith(".log") } ?: emptyArray()).sortedByDescending(File::lastModified)

    private fun refreshLogs() {
        findViewById<TextView>(R.id.logs_path).text = logDir().absolutePath
        val list = findViewById<LinearLayout>(R.id.logs_list)
        list.removeAllViews()
        val logs = logFiles()
        if (logs.isEmpty()) list.addView(body(getString(R.string.logs_none)))
        for (log in logs) {
            val when_ = DateFormat.getDateFormat(this).format(log.lastModified()) + " " +
                DateFormat.getTimeFormat(this).format(log.lastModified())
            list.addView(row(log.name, "${Formatter.formatShortFileSize(this, log.length())} · $when_") { showLog(log) })
        }
        LauncherArt.applyFonts(list)
    }

    /** The log's end (its last 256 KB) in a dialog; Share sends the whole file. */
    private fun showLog(log: File) {
        val text = try {
            RandomAccessFile(log, "r").use { f ->
                val start = maxOf(0L, f.length() - 256 * 1024)
                f.seek(start)
                val bytes = ByteArray((f.length() - start).toInt())
                f.readFully(bytes)
                String(bytes, Charsets.UTF_8).let { if (start > 0) "…\n" + it.substringAfter('\n') else it }
            }
        } catch (e: Exception) {
            e.message ?: e.toString()
        }
        val dp = resources.displayMetrics.density
        val view = TextView(this).apply {
            this.text = text
            typeface = Typeface.MONOSPACE
            textSize = 11f
            setTextIsSelectable(true)
            setPadding((16 * dp).toInt(), (8 * dp).toInt(), (16 * dp).toInt(), (8 * dp).toInt())
        }
        val scroll = ScrollView(this).apply { addView(view) }
        MaterialAlertDialogBuilder(this)
            .setTitle(log.name)
            .setView(scroll)
            .setPositiveButton(R.string.log_share) { _, _ -> shareLog(log) }
            .setNegativeButton(R.string.close, null)
            .show()
        scroll.post { scroll.fullScroll(View.FOCUS_DOWN) }
    }

    private fun shareLog(log: File) {
        try {
            val uri = FileProvider.getUriForFile(this, "$packageName.files", log)
            startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, log.name)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }, log.name))
        } catch (e: Exception) {
            toast(e.message ?: e.toString())
        }
    }

    // --- Controls --------------------------------------------------------------

    private fun setUpControls() {
        findViewById<PadTestView>(R.id.pad_test).apply {
            state = pad
            labels = arrayOf("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS",
                getString(R.string.pad_up), getString(R.string.pad_down), getString(R.string.pad_left), getString(R.string.pad_right))
        }
        findViewById<Slider>(R.id.vibration).addOnChangeListener { _, value, fromUser ->
            showVibration(value.toInt())
            if (fromUser && !binding) {
                config["vibration"] = value.toInt().toString()
                save()
            }
        }
        findViewById<Button>(R.id.vibration_test).setOnClickListener {
            if (!pad.vibrate(config.int("vibration", 100))) toast(getString(R.string.vibration_none))
        }
        for ((id, key) in listOf(R.id.deadzone_left to "deadzone_left", R.id.deadzone_right to "deadzone_right")) {
            findViewById<Slider>(id).addOnChangeListener { _, value, fromUser ->
                showDeadzones()
                if (fromUser && !binding) {
                    config[key] = (value.toInt() / 100.0).toString()
                    save()
                }
            }
        }
    }

    private fun showVibration(value: Int) {
        findViewById<TextView>(R.id.vibration_value).text =
            if (value == 0) getString(R.string.off) else getString(R.string.vibration_value, value)
        findViewById<Button>(R.id.vibration_test).isEnabled = value > 0
    }

    private fun showDeadzones() {
        val left = findViewById<Slider>(R.id.deadzone_left).value.toInt()
        val right = findViewById<Slider>(R.id.deadzone_right).value.toInt()
        findViewById<TextView>(R.id.deadzone_left_value).text = getString(R.string.percent, left)
        findViewById<TextView>(R.id.deadzone_right_value).text = getString(R.string.percent, right)
        findViewById<PadTestView>(R.id.pad_test).apply {
            deadzoneLeft = left / 100f
            deadzoneRight = right / 100f
            invalidate()
        }
    }

    private fun refreshPad() {
        val device = pad.device ?: PadState.connectedPad()
        findViewById<TextView>(R.id.pad_name).text =
            device?.let { getString(R.string.pad_connected, it.name) } ?: getString(R.string.pad_none)
        findViewById<PadTestView>(R.id.pad_test).apply {
            map = PadState.map(config)
            invalidate()
        }
        if (section == null && page == Page.CONTROLS) showList()
    }

    private fun testing() = section == Section.TEST

    // The controller goes to the test view (and is kept from moving the
    // focus or going Back while it shows).
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (pad.onMotion(event) && testing()) {
            findViewById<PadTestView>(R.id.pad_test).invalidate()
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val wasNamed = pad.device != null
        if (pad.onKey(event) && testing()) {
            if (!wasNamed) refreshPad()
            findViewById<PadTestView>(R.id.pad_test).invalidate()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    // --- Settings --------------------------------------------------------------

    private val aspectNames = mapOf("fit" to R.string.aspect_fit_short, "fill" to R.string.aspect_fill_short,
        "stretch" to R.string.aspect_stretch_short)

    private fun languageName() = config.int("language", 1).let { id ->
        languages.firstOrNull { it.first == id }?.second?.first ?: "English"
    }

    private fun setUpSettings() {
        val language = findViewById<MaterialAutoCompleteTextView>(R.id.language)
        // An adapter that never filters: AutoCompleteTextView filters its list by
        // the text it shows (restored on recreate), which left only "English".
        val names = languages.map { it.second.first }
        language.setDropDownBackgroundDrawable(
            android.graphics.drawable.ColorDrawable(ContextCompat.getColor(this, R.color.field)),
        )
        language.setAdapter(object : ArrayAdapter<String>(this, R.layout.dropdown_item, names) {
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View =
                super.getView(position, convertView, parent).also {
                    (it as TextView).typeface = LauncherArt.regularFont(context)
                }
            override fun getFilter() = object : android.widget.Filter() {
                override fun performFiltering(constraint: CharSequence?) =
                    FilterResults().apply { values = names; count = names.size }
                override fun publishResults(constraint: CharSequence?, results: FilterResults?) = notifyDataSetChanged()
            }
        })
        language.setOnItemClickListener { _, _, position, _ ->
            config["language"] = languages[position].first.toString()
            config.save()
            applyLanguage()
        }

        findViewById<Slider>(R.id.render_scale).addOnChangeListener { _, value, fromUser ->
            showRenderScale(value.toInt())
            if (fromUser && !binding) {
                config["render_scale"] = value.toInt().toString()
                save()
            }
        }
        // Frame rate limit: the slider (its far end is Unlimited), the chips suggest.
        findViewById<Slider>(R.id.fps_cap).addOnChangeListener { _, value, fromUser ->
            val cap = if (value.toInt() >= FPS_UNLIMITED_SLOT) 0 else value.toInt()
            showFpsCap(cap)
            if (fromUser && !binding) {
                config["fps_cap"] = cap.toString()
                save()
            }
        }
        for ((id, cap) in fpsChips) {
            findViewById<Chip>(id).setOnClickListener {
                config["fps_cap"] = cap.toString()
                save()
            }
        }
        findViewById<Slider>(R.id.screen_padding).addOnChangeListener { _, value, fromUser ->
            showScreenPadding(value.toInt())
            if (fromUser && !binding) {
                config["screen_padding"] = value.toInt().toString()
                save()
            }
        }
        // Wide screens: a radio list, each option with its ⓘ (tap: the tooltip).
        for ((key, ids) in aspectRows) {
            findViewById<RadioButton>(ids.first).setOnClickListener {
                config["aspect"] = key
                save()
            }
            findViewById<View>(ids.second).apply {
                TooltipCompat.setTooltipText(this, contentDescription)
                setOnClickListener { performLongClick() }
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
        // This screen's shape, for Wide screens.
        val bounds = windowManager.maximumWindowMetrics.bounds
        val w = maxOf(bounds.width(), bounds.height())
        val h = minOf(bounds.width(), bounds.height())
        findViewById<TextView>(R.id.aspect_screen).text =
            getString(R.string.aspect_screen, String.format(Locale.ROOT, "%.2f", w.toFloat() / h))
    }

    // Switch -> launcher.cfg key, and the native launcher's default.
    private val switches = listOf(
        R.id.vsync to "vsync", R.id.show_fps to "show_fps", R.id.skip_intro to "skip_intro",
        R.id.toasts to "toasts", R.id.detailed_logs to "detailed_logs",
    )
    private val switchDefaults = mapOf(
        "vsync" to true, "show_fps" to false, "skip_intro" to false, "toasts" to true, "detailed_logs" to false,
    )

    private val fpsChips = listOf(R.id.fps_30 to 30, R.id.fps_60 to 60, R.id.fps_120 to 120, R.id.fps_unlimited to 0)
    private val aspectRows = mapOf(
        "fit" to (R.id.aspect_fit to R.id.aspect_fit_info),
        "fill" to (R.id.aspect_fill to R.id.aspect_fill_info),
        "stretch" to (R.id.aspect_stretch to R.id.aspect_stretch_info),
    )

    private fun showScreenPadding(dp: Int) {
        findViewById<TextView>(R.id.screen_padding_value).text =
            if (dp == 0) getString(R.string.off) else getString(R.string.screen_padding_value, dp)
    }

    private fun showFpsCap(cap: Int) {
        findViewById<TextView>(R.id.fps_cap_value).text = when (cap) {
            0 -> getString(R.string.fps_unlimited)
            30 -> getString(R.string.fps_value_game, cap)
            else -> getString(R.string.fps_value, cap)
        }
    }

    private fun showRenderScale(percent: Int) {
        val width = (1280 * percent + 50) / 100
        val height = (720 * percent + 50) / 100
        findViewById<TextView>(R.id.render_scale_value).text = getString(
            if (percent == 100) R.string.render_resolution_game else R.string.render_resolution_value,
            percent, width, height,
        )
    }

    // --- About -----------------------------------------------------------------

    private fun setUpAbout() {
        findViewById<TextView>(R.id.about_repository).apply {
            text = BuildConfig.REPOSITORY_URL
            paint.isUnderlineText = true
            setOnClickListener {
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(BuildConfig.REPOSITORY_URL))) }
            }
        }
        val build = findViewById<LinearLayout>(R.id.about_build)
        build.addView(labelled(getString(R.string.about_version_label),
            BuildConfig.GIT_VERSION.ifEmpty { BuildConfig.VERSION_NAME }))
        build.addView(labelled(getString(R.string.about_built), BuildConfig.BUILD_DATE))
        build.addView(labelled(getString(R.string.about_shader_format), BuildConfig.SHADER_CACHE_VERSION.toString()))
        val device = findViewById<LinearLayout>(R.id.about_device)
        for (row in DeviceInfo.rows(this)) device.addView(labelled(getString(row.label), row.value))
        LauncherArt.applyFonts(build)
        LauncherArt.applyFonts(device)
        findViewById<Button>(R.id.about_copy).setOnClickListener {
            val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            clipboard.setPrimaryClip(ClipData.newPlainText(getString(R.string.app_name), DeviceInfo.copyText(this)))
            findViewById<TextView>(R.id.about_copy_note).setText(R.string.about_copied)
        }
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

            findViewById<MaterialAutoCompleteTextView>(R.id.language).setText(languageName(), false)
            val renderScale = config.int("render_scale", 100).coerceIn(25, 200) / 5 * 5
            findViewById<Slider>(R.id.render_scale).value = renderScale.toFloat()
            showRenderScale(config.int("render_scale", 100))
            val vibration = config.int("vibration", 100).coerceIn(0, 200) / 10 * 10
            findViewById<Slider>(R.id.vibration).value = vibration.toFloat()
            showVibration(vibration)
            findViewById<Slider>(R.id.deadzone_left).value = percent(config.double("deadzone_left")).coerceIn(0, 50).toFloat()
            findViewById<Slider>(R.id.deadzone_right).value = percent(config.double("deadzone_right")).coerceIn(0, 50).toFloat()
            showDeadzones()
            val cap = config.int("fps_cap", 30).coerceIn(0, 240)
            findViewById<Slider>(R.id.fps_cap).value =
                if (cap == 0) FPS_UNLIMITED_SLOT.toFloat() else (cap.coerceAtLeast(15) / 5 * 5).toFloat()
            showFpsCap(cap)
            for ((id, value) in fpsChips) findViewById<Chip>(id).isChecked = value == cap
            val padding = config.int("screen_padding", 0).coerceIn(0, 64) / 4 * 4
            findViewById<Slider>(R.id.screen_padding).value = padding.toFloat()
            showScreenPadding(padding)
            val aspect = config["aspect"]?.takeIf { it in aspectRows } ?: "fit"
            for ((key, ids) in aspectRows) findViewById<RadioButton>(ids.first).isChecked = key == aspect
            for ((id, key) in switches) {
                findViewById<MaterialSwitch>(id).isChecked = config.bool(key, switchDefaults.getValue(key))
            }
            when (section) {
                null -> if (page.sections.isNotEmpty()) showList()
                Section.SAVES -> refreshSaves()
                Section.LOGS -> refreshLogs()
                Section.TEST -> refreshPad()
                else -> {}
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
        val found = gameFound()
        status.isVisible = game != null
        status.text = getString(if (found) R.string.game_found else R.string.game_missing)
        status.setTextColor(ContextCompat.getColor(this, if (found) R.color.good else R.color.bad))
    }

    private fun refreshShaders() {
        showShaderBuild(ShaderBuildService.status)
        val cache = folders.shaderCache(config)
        val state = shaderState(cache)
        val status = findViewById<TextView>(R.id.shaders_status)
        status.text = shaderText(state)
        status.setTextColor(ContextCompat.getColor(this, if (state.first == ShaderState.READY) R.color.good else R.color.bad))
        findViewById<TextView>(R.id.shaders_path).text = cache.absolutePath
        // As the desktop sidebar: what is missing, above Play (and a dot on
        // the menu button when the sidebar is a drawer). Tapping it goes there.
        val text = when {
            !gameFound() -> getString(R.string.notice_game)
            state.first != ShaderState.READY -> getString(R.string.notice_shaders)
            else -> null
        }
        val notice = findViewById<TextView>(R.id.play_notice)
        notice.isVisible = text != null && !inGame
        notice.text = text
        findViewById<View>(R.id.menu_notice).isVisible = text != null && drawer != null && !inGame
    }

    // --- In game (InGameMenuActivity) -------------------------------------------

    /** The sidebar in a landscape phone's height: no logo, Restart and Quit side by side. */
    private fun compactSidebar() {
        val dp = resources.displayMetrics.density
        if (inGame) findViewById<View>(R.id.sidebar).setBackgroundColor(ContextCompat.getColor(this, R.color.in_game_sidebar))
        findViewById<View>(R.id.logo).isVisible = false
        findViewById<View>(R.id.logo_caption).isVisible = false
        (findViewById<View>(R.id.menu).layoutParams as ViewGroup.MarginLayoutParams).topMargin = (16 * dp).toInt()
        findViewById<LinearLayout>(R.id.in_game_actions).orientation = LinearLayout.HORIZONTAL
        for (id in intArrayOf(R.id.restart, R.id.quit)) {
            findViewById<Button>(id).apply {
                layoutParams = LinearLayout.LayoutParams(0, (48 * dp).toInt(), 1f)
                textSize = 16f
            }
        }
        findViewById<Button>(R.id.play).apply {
            layoutParams.height = (64 * dp).toInt()
            (layoutParams as ViewGroup.MarginLayoutParams).bottomMargin = (4 * dp).toInt()
            textSize = 26f
        }
    }

    /** In game: no status or navigation bar, as the game (a swipe shows them). */
    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun setUpInGame() {
        findViewById<Button>(R.id.play).setText(R.string.in_game_resume)
        findViewById<View>(R.id.in_game_actions).isVisible = true
        findViewById<Button>(R.id.restart).setOnClickListener {
            confirm(R.string.restart_title, getString(R.string.progress_lost), R.string.restart) { restartGame() }
        }
        findViewById<Button>(R.id.quit).setOnClickListener {
            confirm(R.string.quit_title, getString(R.string.progress_lost), R.string.quit) { endGame() }
        }
    }

    private fun confirm(title: Int, text: String, yes: Int, action: () -> Unit) {
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(text)
            .setPositiveButton(yes) { _, _ -> action() }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    // As the desktop's NeedsRestart, and the game folder and language the
    // game runs with: these apply at the next start.
    private fun restartNeeded(): Boolean {
        val start = GameActivity.startConfig ?: return false
        fun fill(c: LauncherConfig) = c["aspect"] == "fill"
        return start.bool("vsync", true) != config.bool("vsync", true) || fill(start) != fill(config) ||
            start.int("language", 1) != config.int("language", 1) ||
            (start["game_data"] ?: "") != (config["game_data"] ?: "")
    }

    /** Back to the game (it reloads launcher.cfg: asura_wrath_app.h OnMenuClosed). */
    private fun resume() {
        if (!restartNeeded()) {
            finish()
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.restart_title)
            .setMessage(getString(R.string.restart_needed) + "\n\n" + getString(R.string.progress_lost))
            .setPositiveButton(R.string.restart) { _, _ -> restartGame() }
            .setNegativeButton(R.string.later) { _, _ -> finish() }
            .show()
    }

    // As platform_android.cpp ReplaceProcess: a note for LauncherActivity,
    // which starts the game again once this process ends.
    private fun restartGame() {
        folders.restartRequest.writeText("game\n")
        endGame()
    }

    /**
     * Ends the game's process (this menu's too), back to the launcher. Both
     * activities finish first: Android would otherwise bring the game back
     * (an activity whose process died is recreated when it's on top), and
     * SDL's own finish waits for a game that is paused under this menu.
     */
    private fun endGame() {
        GameActivity.current?.finish()
        finish()
        android.os.Process.killProcess(android.os.Process.myPid())
    }

    /** The game's own menu (ImGui), opened by the game when it comes back. */
    private fun openGameMenu() {
        File(folders.root, ".open_game_menu").writeText("1\n")
        finish()
    }

    // --- Play ------------------------------------------------------------------

    private fun play() {
        // The game reads the cache at start; the build is still writing it.
        if (ShaderBuildService.status.state == ShaderBuildService.State.RUNNING) {
            toast(getString(R.string.shaders_building))
            return
        }
        val game = gameFolder()
        if (game == null || !gameFound()) {
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
        private const val KEY_SECTION = "section"
        private const val FPS_UNLIMITED_SLOT = 245  // the frame rate slider's far end
    }
}
