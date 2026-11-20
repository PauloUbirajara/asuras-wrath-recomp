package org.asurawrath.recompiled

import java.io.File

/**
 * launcher.cfg, the same file the native launcher and the game read
 * (src/ui/launcher.cpp, LauncherSettings): `key=value` lines. Keys this
 * screen doesn't show (key binds, the controller remap, ...) are kept as
 * they are.
 */
class LauncherConfig private constructor(private val file: File, private val lines: MutableList<String>) {

    operator fun get(key: String): String? =
        lines.firstNotNullOfOrNull { line ->
            if (!line.startsWith("#") && line.substringBefore('=', "") == key) line.substringAfter('=') else null
        }

    operator fun set(key: String, value: String) {
        val index = lines.indexOfFirst { !it.startsWith("#") && it.substringBefore('=', "") == key }
        if (index >= 0) lines[index] = "$key=$value" else lines += "$key=$value"
    }

    fun int(key: String, default: Int): Int = this[key]?.trim()?.toIntOrNull() ?: default
    fun bool(key: String, default: Boolean): Boolean =
        this[key]?.let { it == "1" || it == "true" } ?: default
    fun setBool(key: String, value: Boolean) = set(key, if (value) "1" else "0")

    /** Writes the file whole (a temporary file renamed over it). */
    fun save() {
        file.parentFile?.mkdirs()
        val temp = File(file.parentFile, file.name + ".tmp")
        temp.writeText(lines.joinToString("\n", postfix = "\n"))
        if (!temp.renameTo(file)) {
            file.writeText(temp.readText())
            temp.delete()
        }
    }

    companion object {
        fun load(file: File): LauncherConfig {
            val lines = if (file.isFile) file.readLines().filter { it.isNotEmpty() }.toMutableList()
            else mutableListOf("# Asura's Wrath Recompiled launcher settings")
            return LauncherConfig(file, lines)
        }
    }
}
