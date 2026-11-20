package org.asurawrath.recompiled

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * The shader cache build on the phone: mantra_shadergen (the same tool as
 * the desktop launcher's Build), shipped as lib/<abi>/libmantra_shadergen.so
 * with the cross-built DXC next to it, run as a program in the background.
 *
 * A foreground service, so Android keeps the app (and the tool's worker
 * processes) alive while the launcher is in the background or the screen is
 * off; a partial wake lock keeps the CPU going. Stop asks the tool to save
 * its progress (<cache>.stop, as the desktop launcher does); the next build
 * continues from <cache>.partial.
 */
class ShaderBuildService : Service() {

    enum class State { IDLE, RUNNING, DONE, STOPPED, FAILED }

    /** What the launcher shows; changed on the tool's reader thread. */
    data class Status(
        val state: State = State.IDLE,
        val done: Int = 0,
        val total: Int = 0,
        val read: Int = 0,
        val packages: Int = 0,
        val line: String = "",
    )

    private var process: Process? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var out: File? = null
    private val main = Handler(Looper.getMainLooper())

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> requestStop()
            ACTION_START -> if (process == null) {
                start(
                    File(intent.getStringExtra(EXTRA_GAME)!!),
                    File(intent.getStringExtra(EXTRA_OUT)!!),
                    intent.getBooleanExtra(EXTRA_FRESH, false),
                )
            }
        }
        return START_NOT_STICKY
    }

    private fun start(game: File, out: File, fresh: Boolean) {
        this.out = out
        out.parentFile?.mkdirs()
        File(out.path + ".stop").delete()
        val libDir = applicationInfo.nativeLibraryDir
        val args = mutableListOf(
            "$libDir/libmantra_shadergen.so",
            "--game", game.absolutePath,
            "--out", out.absolutePath,
            // It stops by itself (saving its progress) if this process dies.
            "--parent", android.os.Process.myPid().toString(),
            // Shaders the game saw while playing (mantra writes them there).
            "--dumps", File(out.parentFile, "shader_dumps").absolutePath,
        )
        if (fresh) args += "--fresh"
        publish(Status(State.RUNNING, line = getString(R.string.shaders_build_starting)))
        startForeground()
        wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AsurasWrath:shaders")
            .apply { acquire(TimeUnit.HOURS.toMillis(6)) }
        val p = try {
            ProcessBuilder(args).redirectErrorStream(true).apply {
                // libdxcompiler.so and libc++_shared.so are next to it; its
                // work folder goes in $TMPDIR (there is no /tmp).
                environment()["LD_LIBRARY_PATH"] = libDir
                environment()["TMPDIR"] = cacheDir.absolutePath
            }.start()
        } catch (e: Exception) {
            Log.e(AppFolders.TAG, "shader build: cannot start", e)
            finish(Status(State.FAILED, line = e.message ?: e.toString()))
            return
        }
        process = p
        Thread({ read(p) }, "shadergen-output").start()
    }

    // As LauncherDialog::ShaderBuild::Start (src/ui/launcher.cpp): the
    // progress lines, the last line, the exit code (5: stopped).
    private fun read(p: Process) {
        var s = status
        val translated = Regex("""^\s*translated (\d+)/(\d+)""")
        val read = Regex("""^\s*read (\d+)/(\d+) packages""")
        p.inputStream.bufferedReader().useLines { lines ->
            for (raw in lines) {
                val line = raw.trim()
                if (line.isEmpty()) continue
                Log.i(AppFolders.TAG, "shadergen: $line")
                translated.find(line)?.let { s = s.copy(done = it.groupValues[1].toInt(), total = it.groupValues[2].toInt()) }
                read.find(line)?.let { s = s.copy(read = it.groupValues[1].toInt(), packages = it.groupValues[2].toInt()) }
                s = s.copy(line = line)
                publish(s)
            }
        }
        val code = p.waitFor()
        val state = when (code) {
            0 -> State.DONE
            EXIT_STOPPED -> State.STOPPED
            else -> State.FAILED
        }
        main.post { finish(s.copy(state = state)) }
    }

    private fun requestStop() {
        val p = process ?: return
        val o = out ?: return
        File(o.path + ".stop").writeText("stop\n")
        // It checks a few times a second; saving takes a moment.
        main.postDelayed({ if (process === p) p.destroyForcibly() }, 20_000)
    }

    private fun finish(final: Status) {
        process = null
        out?.let { File(it.path + ".stop").delete() }
        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null
        publish(final)
        stopForeground(STOP_FOREGROUND_REMOVE)
        if (final.state == State.DONE || final.state == State.FAILED) {
            notificationManager().notify(NOTIFICATION_DONE, notification(final, ongoing = false))
        }
        stopSelf()
    }

    override fun onDestroy() {
        process?.destroyForcibly()
        wakeLock?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    // --- Notification ----------------------------------------------------------

    private fun notificationManager() = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun startForeground() {
        notificationManager().createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.shaders_build_channel), NotificationManager.IMPORTANCE_LOW),
        )
        notificationManager().cancel(NOTIFICATION_DONE)
        val n = notification(status, ongoing = true)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_RUNNING, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_RUNNING, n)
        }
    }

    private fun notification(s: Status, ongoing: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, LauncherActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val b = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(open)
            .setOngoing(ongoing)
            .setOnlyAlertOnce(true)
            .setContentTitle(getString(R.string.section_shaders))
            .setContentText(statusText(this, s))
        if (ongoing) {
            if (s.total > 0) b.setProgress(s.total, s.done, false)
            else if (s.packages > 0) b.setProgress(s.packages, s.read, false)
            else b.setProgress(0, 0, true)
            val stop = PendingIntent.getService(
                this, 1, Intent(this, ShaderBuildService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE,
            )
            b.addAction(Notification.Action.Builder(null, getString(R.string.shaders_build_stop), stop).build())
        }
        return b.build()
    }

    private var lastNotified = 0L

    private fun publish(s: Status) {
        status = s
        main.post {
            listener?.invoke(s)
            // The notification at most once a second.
            val now = System.currentTimeMillis()
            if (process != null && s.state == State.RUNNING && now - lastNotified > 1000) {
                lastNotified = now
                notificationManager().notify(NOTIFICATION_RUNNING, notification(s, ongoing = true))
            }
        }
    }

    companion object {
        private const val ACTION_START = "start"
        private const val ACTION_STOP = "stop"
        private const val EXTRA_GAME = "game"
        private const val EXTRA_OUT = "out"
        private const val EXTRA_FRESH = "fresh"
        private const val CHANNEL = "shaders"
        private const val NOTIFICATION_RUNNING = 1
        private const val NOTIFICATION_DONE = 2
        private const val EXIT_STOPPED = 5  // mantra_shadergen's kExitStopped

        /** The build's state, for the launcher (this process only). */
        @Volatile
        var status = Status()
            private set

        /** Called on the main thread when the status changes. */
        var listener: ((Status) -> Unit)? = null

        fun start(context: Context, game: File, out: File, fresh: Boolean) {
            context.startForegroundService(
                Intent(context, ShaderBuildService::class.java).setAction(ACTION_START)
                    .putExtra(EXTRA_GAME, game.absolutePath)
                    .putExtra(EXTRA_OUT, out.absolutePath)
                    .putExtra(EXTRA_FRESH, fresh),
            )
        }

        fun stop(context: Context) {
            if (status.state != State.RUNNING) return
            context.startService(Intent(context, ShaderBuildService::class.java).setAction(ACTION_STOP))
        }

        /** Shaders done in a stopped build (<cache>.partial's entry count); 0 if none. */
        fun stoppedAt(cache: File): Int {
            val partial = File(cache.path + ".partial")
            if (!partial.isFile) return 0
            val h = ByteArray(12)
            partial.inputStream().use { if (it.read(h) != 12) return 0 }
            if (String(h, 0, 4, Charsets.US_ASCII) != "MSHC") return 0
            fun le(at: Int) = (h[at].toInt() and 0xff) or ((h[at + 1].toInt() and 0xff) shl 8) or
                ((h[at + 2].toInt() and 0xff) shl 16) or ((h[at + 3].toInt() and 0xff) shl 24)
            return if (le(4) == BuildConfig.SHADER_CACHE_VERSION) le(8) else 0
        }

        fun statusText(context: Context, s: Status): String = when {
            s.state == State.RUNNING && s.total > 0 ->
                context.getString(R.string.shaders_build_translating, s.done, s.total)
            s.state == State.RUNNING && s.packages > 0 ->
                context.getString(R.string.shaders_build_reading, s.read, s.packages)
            s.state == State.RUNNING -> s.line
            s.state == State.DONE -> context.getString(R.string.shaders_build_done)
            s.state == State.STOPPED -> context.getString(R.string.shaders_build_stopped)
            s.state == State.FAILED -> context.getString(R.string.shaders_build_failed, s.line)
            else -> ""
        }
    }
}
