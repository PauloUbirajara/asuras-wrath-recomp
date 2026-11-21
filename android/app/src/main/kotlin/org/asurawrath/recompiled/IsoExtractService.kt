package org.asurawrath.recompiled

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.StatFs
import android.util.Log
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.util.concurrent.TimeUnit

/**
 * The game disc image (.iso) extracted on the phone, as the desktop
 * launcher does with a dropped .iso (src/ui/launcher.cpp IsoExtract, the
 * SDK's DiscImageDevice): the image's XDVDFS game partition copied into a
 * folder, read straight from the picked document (no storage permission).
 *
 * A foreground service, so it keeps going with the launcher in the
 * background or the screen off. default.xex is written last: a folder with
 * it is a complete extraction.
 */
class IsoExtractService : Service() {

    enum class State { IDLE, RUNNING, DONE, STOPPED, FAILED }

    data class Status(
        val state: State = State.IDLE,
        val doneBytes: Long = 0,
        val totalBytes: Long = 0,
        val line: String = "",
        val out: String = "",
    )

    @Volatile private var cancel = false
    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private val main = Handler(Looper.getMainLooper())

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> cancel = true
            ACTION_START -> if (worker == null) {
                start(Uri.parse(intent.getStringExtra(EXTRA_ISO)!!), File(intent.getStringExtra(EXTRA_OUT)!!))
            }
        }
        return START_NOT_STICKY
    }

    private fun start(iso: Uri, out: File) {
        cancel = false
        publish(Status(State.RUNNING, line = getString(R.string.iso_reading), out = out.absolutePath))
        startForeground()
        wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AsurasWrath:iso")
            .apply { acquire(TimeUnit.HOURS.toMillis(2)) }
        worker = Thread({
            val final = try {
                extract(iso, out)
            } catch (e: Exception) {
                Log.e(AppFolders.TAG, "iso extract", e)
                status.copy(state = State.FAILED, line = e.message ?: e.toString())
            }
            main.post { finish(final) }
        }, "iso-extract").apply { start() }
    }

    // --- XDVDFS ----------------------------------------------------------------

    private class Entry(val path: String, val sector: Long, val size: Long, val directory: Boolean)

    private fun extract(iso: Uri, out: File): Status {
        val fd = contentResolver.openFileDescriptor(iso, "r")
            ?: return status.copy(state = State.FAILED, line = getString(R.string.iso_not_disc))
        fd.use {
            // Positional reads: a local file's descriptor (the storage
            // providers hand one out), not a pipe.
            FileInputStream(fd.fileDescriptor).channel.use { channel ->
                return extract(channel, out)
            }
        }
    }

    private fun read(channel: FileChannel, offset: Long, size: Int): ByteBuffer {
        val buffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
        var at = offset
        while (buffer.hasRemaining()) {
            val n = channel.read(buffer, at)
            if (n <= 0) throw IOException("the image ends early")
            at += n
        }
        buffer.flip()
        return buffer
    }

    private fun extract(channel: FileChannel, out: File): Status {
        // Sector 32 of the game partition holds the volume descriptor; the
        // partition starts at one of these offsets (XISO, XGD1, XGD2, XGD3).
        val size = channel.size()
        val gameOffset = longArrayOf(0L, 0xFB20L, 0x20600L, 0x2080000L, 0xFD90000L, 0x18300000L).firstOrNull { o ->
            o + 33 * SECTOR <= size &&
                String(read(channel, o + 32 * SECTOR, 20).array(), Charsets.US_ASCII) == MAGIC
        } ?: return status.copy(state = State.FAILED, line = getString(R.string.iso_not_disc))
        val volume = read(channel, gameOffset + 32 * SECTOR, 28)
        val rootSector = volume.getInt(20).toLong() and 0xffffffffL
        val rootSize = volume.getInt(24).toLong() and 0xffffffffL
        if (rootSize < 13 || rootSize > (32 shl 20)) {
            return status.copy(state = State.FAILED, line = getString(R.string.iso_not_disc))
        }

        val entries = ArrayList<Entry>()
        readDirectory(channel, gameOffset, rootSector, rootSize, "", entries, 0)
        val total = entries.filter { !it.directory }.sumOf { it.size }
        if (entries.none { it.path.equals("default.xex", ignoreCase = true) }) {
            return status.copy(state = State.FAILED, line = getString(R.string.iso_not_game))
        }
        out.mkdirs()
        // Files already there are overwritten, so only the difference counts.
        val free = StatFs(out.absolutePath).availableBytes + existingBytes(out, entries)
        if (free < total) {
            return status.copy(
                state = State.FAILED,
                line = getString(R.string.iso_no_space, total.toGiB(), free.toGiB()),
            )
        }
        // A previous complete extraction's default.xex goes first: until the
        // end this folder is not a game.
        entries.firstOrNull { it.path.equals("default.xex", ignoreCase = true) }?.let { File(out, it.path).delete() }

        var done = 0L
        publish(status.copy(totalBytes = total))
        val ordered = entries.sortedBy { it.path.equals("default.xex", ignoreCase = true) }
        val buffer = ByteBuffer.allocateDirect(CHUNK)
        for (e in ordered) {
            if (cancel) return status.copy(state = State.STOPPED)
            val dest = File(out, e.path)
            if (e.directory) {
                dest.mkdirs()
                continue
            }
            dest.parentFile?.mkdirs()
            publish(status.copy(doneBytes = done, line = e.path))
            FileOutputStream(dest).use { stream ->
                val target = stream.channel
                var at = gameOffset + e.sector * SECTOR
                var left = e.size
                while (left > 0) {
                    if (cancel) {
                        stream.close()
                        dest.delete()
                        return status.copy(state = State.STOPPED)
                    }
                    buffer.clear()
                    buffer.limit(minOf(CHUNK.toLong(), left).toInt())
                    while (buffer.hasRemaining()) {
                        val n = channel.read(buffer, at + buffer.position())
                        if (n <= 0) throw IOException("the image ends early (${e.path})")
                    }
                    buffer.flip()
                    val n = buffer.remaining()
                    while (buffer.hasRemaining()) target.write(buffer)
                    at += n
                    left -= n
                    done += n
                    publishProgress(done)
                }
            }
        }
        return status.copy(state = State.DONE, doneBytes = total, line = "")
    }

    /** A directory: a binary tree of entries at 4-byte steps (left, right, sector, size, attributes, name). */
    private fun readDirectory(
        channel: FileChannel, gameOffset: Long, sector: Long, size: Long, prefix: String,
        out: MutableList<Entry>, depth: Int,
    ) {
        if (size == 0L || depth > 32) return
        val table = read(channel, gameOffset + sector * SECTOR, size.toInt())
        val seen = HashSet<Int>()
        fun node(ordinal: Int) {
            val p = ordinal * 4
            if (p + 14 > size || !seen.add(ordinal)) return
            val left = table.getShort(p).toInt() and 0xffff
            val right = table.getShort(p + 2).toInt() and 0xffff
            if (left == 0xffff && right == 0xffff) return  // padding
            val entrySector = table.getInt(p + 4).toLong() and 0xffffffffL
            val entrySize = table.getInt(p + 8).toLong() and 0xffffffffL
            val attributes = table.get(p + 12).toInt() and 0xff
            val nameLength = table.get(p + 13).toInt() and 0xff
            if (p + 14 + nameLength > size) return
            val name = String(table.array(), p + 14, nameLength, Charsets.ISO_8859_1)
            if (left != 0) node(left)
            // Names stay inside the folder.
            if (name.isNotEmpty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\')) {
                val path = prefix + name
                val directory = attributes and ATTRIBUTE_DIRECTORY != 0
                out += Entry(path, entrySector, entrySize, directory)
                if (directory) readDirectory(channel, gameOffset, entrySector, entrySize, "$path/", out, depth + 1)
            }
            if (right != 0) node(right)
        }
        node(0)
    }

    private fun existingBytes(out: File, entries: List<Entry>): Long =
        entries.filter { !it.directory }.sumOf { File(out, it.path).takeIf(File::isFile)?.length() ?: 0L }

    private fun Long.toGiB() = String.format("%.1f GB", this / (1024.0 * 1024.0 * 1024.0))

    // --- Status and notification -------------------------------------------------

    private var lastProgress = 0L

    private fun publishProgress(done: Long) {
        val now = System.currentTimeMillis()
        if (now - lastProgress < 250) return
        lastProgress = now
        publish(status.copy(doneBytes = done))
    }

    private fun finish(final: Status) {
        worker = null
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
        cancel = true
        wakeLock?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    private fun notificationManager() = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun startForeground() {
        notificationManager().createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.iso_channel), NotificationManager.IMPORTANCE_LOW),
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
            .setContentTitle(getString(R.string.iso_title))
            .setContentText(statusText(this, s))
        if (ongoing) {
            if (s.totalBytes > 0) {
                b.setProgress(1000, (s.doneBytes * 1000 / s.totalBytes).toInt(), false)
            } else {
                b.setProgress(0, 0, true)
            }
            val stop = PendingIntent.getService(
                this, 1, Intent(this, IsoExtractService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE,
            )
            b.addAction(Notification.Action.Builder(null, getString(R.string.iso_stop), stop).build())
        }
        return b.build()
    }

    private var lastNotified = 0L

    private fun publish(s: Status) {
        status = s
        main.post {
            listener?.invoke(s)
            val now = System.currentTimeMillis()
            if (worker != null && s.state == State.RUNNING && now - lastNotified > 1000) {
                lastNotified = now
                notificationManager().notify(NOTIFICATION_RUNNING, notification(s, ongoing = true))
            }
        }
    }

    companion object {
        private const val ACTION_START = "start"
        private const val ACTION_STOP = "stop"
        private const val EXTRA_ISO = "iso"
        private const val EXTRA_OUT = "out"
        private const val CHANNEL = "iso"
        private const val NOTIFICATION_RUNNING = 11
        private const val NOTIFICATION_DONE = 12
        private const val SECTOR = 2048L
        private const val CHUNK = 4 shl 20
        private const val MAGIC = "MICROSOFT*XBOX*MEDIA"
        private const val ATTRIBUTE_DIRECTORY = 0x10

        @Volatile
        var status = Status()
            private set

        /** Called on the main thread when the status changes. */
        var listener: ((Status) -> Unit)? = null

        fun start(context: Context, iso: Uri, out: File) {
            context.startForegroundService(
                Intent(context, IsoExtractService::class.java).setAction(ACTION_START)
                    .putExtra(EXTRA_ISO, iso.toString())
                    .putExtra(EXTRA_OUT, out.absolutePath),
            )
        }

        fun stop(context: Context) {
            if (status.state != State.RUNNING) return
            context.startService(Intent(context, IsoExtractService::class.java).setAction(ACTION_STOP))
        }

        fun statusText(context: Context, s: Status): String = when {
            s.state == State.RUNNING && s.totalBytes > 0 -> context.getString(
                R.string.iso_progress, (s.doneBytes * 100 / s.totalBytes).toInt(), s.line,
            )
            s.state == State.RUNNING -> s.line
            s.state == State.DONE -> context.getString(R.string.iso_done)
            s.state == State.STOPPED -> context.getString(R.string.iso_stopped)
            s.state == State.FAILED -> context.getString(R.string.iso_failed, s.line)
            else -> ""
        }
    }
}
