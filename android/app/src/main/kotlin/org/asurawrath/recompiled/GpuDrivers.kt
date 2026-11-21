package org.asurawrath.recompiled

import android.content.Context
import android.net.Uri
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.util.zip.ZipInputStream

/**
 * Custom Vulkan drivers for Snapdragon (Adreno) phones: Mesa Turnip or a
 * newer Qualcomm driver, in the usual driver .zip (meta.json + the driver
 * .so, as other Android emulators use). Each is unpacked in the app's
 * internal storage, where libraries may be loaded from:
 *
 *   files/gpu_drivers/<id>/meta.json, <libraryName>
 *
 * launcher.cfg's gpu_driver=<id> picks one for the next start; the game
 * loads it through libadrenotools (src/platform/gpu_driver.cpp), from
 * $ASURA_GPU_DRIVERS (set by AppFolders.prepare).
 */
class GpuDrivers(context: Context) {
    val dir = File(context.filesDir, "gpu_drivers")
    private val resolver = context.contentResolver

    data class Driver(val id: String, val name: String, val version: String, val description: String,
                      val author: String, val library: String)

    /** Installed drivers, by name. */
    fun list(): List<Driver> =
        dir.listFiles()?.filter { it.isDirectory && it.name != REDIRECT }?.mapNotNull(::read)
            ?.sortedBy { it.name.lowercase() } ?: emptyList()

    fun find(id: String?): Driver? = id?.takeIf { it.isNotEmpty() }?.let { read(File(dir, it)) }

    private fun read(folder: File): Driver? = try {
        val meta = JSONObject(File(folder, "meta.json").readText())
        val library = meta.optString("libraryName")
        if (library.isEmpty() || !File(folder, library).isFile) null
        else Driver(folder.name, meta.optString("name", folder.name), meta.optString("driverVersion"),
            meta.optString("description"), meta.optString("author"), library)
    } catch (e: Exception) {
        null
    }

    /** Unpacks a driver .zip; returns it, or throws with what's wrong. */
    fun install(uri: Uri): Driver {
        dir.mkdirs()
        val temp = File(dir, ".installing")
        temp.deleteRecursively()
        temp.mkdirs()
        try {
            resolver.openInputStream(uri)?.use { input ->
                ZipInputStream(input).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        // Files only, flattened: nothing lands outside the folder.
                        val name = entry.name.substringAfterLast('/')
                        if (entry.isDirectory || name.isEmpty() || name.startsWith(".")) continue
                        File(temp, name).outputStream().use { zip.copyTo(it) }
                    }
                }
            } ?: throw IllegalArgumentException("can't open the file")
            val meta = File(temp, "meta.json")
            if (!meta.isFile) throw IllegalArgumentException("no meta.json: not a driver .zip")
            val json = JSONObject(meta.readText())
            val library = json.optString("libraryName")
            if (library.isEmpty() || !File(temp, library).isFile) {
                throw IllegalArgumentException("the driver library ($library) isn't in the .zip")
            }
            val label = listOf(json.optString("name"), json.optString("driverVersion"))
                .filter { it.isNotEmpty() }.joinToString("-").ifEmpty { "driver" }
            var id = label.replace(Regex("[^A-Za-z0-9._-]+"), "_").trim('_', '.').take(60).ifEmpty { "driver" }
            if (id == REDIRECT) id = "${id}_"
            val target = File(dir, id)
            target.deleteRecursively()
            if (!temp.renameTo(target)) throw IllegalStateException("can't move it into place")
            return read(target) ?: throw IllegalStateException("unreadable after install")
        } finally {
            temp.deleteRecursively()
        }
    }

    fun delete(id: String) {
        if (id.isNotEmpty() && id != REDIRECT) File(dir, id).deleteRecursively()
    }

    companion object {
        /** The drivers' own files (libadrenotools' file redirect). */
        const val REDIRECT = "file_redirect"

        /** Custom drivers need an arm64 phone (libadrenotools); Adreno GPUs are Snapdragon's. */
        val supported: Boolean get() = Build.SUPPORTED_ABIS.firstOrNull() == "arm64-v8a"
    }
}
