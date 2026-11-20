package org.asurawrath.recompiled

import java.io.File

/**
 * The Achievements page's list, as the desktop's LoadAchievements: names
 * and icons from <Files>/cache/achievements (achievements.tsv, <id>.png,
 * written by the game at start from default.xex, in the game's language),
 * unlocks from <Files>/achievements/43430817.toml.
 */
object Achievements {
    class Achievement(
        val id: Int, val gamerscore: Int, val name: String,
        val description: String, val lockedDescription: String, val icon: File,
    ) {
        var unlockedFiletime = 0L  // 0 = locked, 1 = unlocked at an unknown time
        val unlocked get() = unlockedFiletime != 0L
        /** Unlock time in ms since 1970, or null. */
        val unlockedMillis get() = if (unlockedFiletime > 1) unlockedFiletime / 10_000 - 11_644_473_600_000L else null
    }

    fun cacheDir(files: File) = File(files, "cache/achievements")

    /** The language the list is in (XLanguage), 0 if there is none yet. */
    fun cachedLanguage(files: File): Int {
        val dir = cacheDir(files)
        if (!File(dir, "achievements.tsv").isFile) return 0
        return try { File(dir, "language").readText().trim().toInt() } catch (e: Exception) { 0 }
    }

    fun load(files: File): List<Achievement> {
        val dir = cacheDir(files)
        val lines = try { File(dir, "achievements.tsv").readLines() } catch (e: Exception) { return emptyList() }
        if (lines.isEmpty()) return emptyList()
        val header = lines[0].split('\t')
        fun col(name: String) = header.indexOf(name)
        val cId = col("id"); val cGs = col("gamerscore"); val cName = col("name")
        val cDesc = col("description"); val cLocked = col("locked_description")
        val list = lines.drop(1).mapNotNull { line ->
            val f = line.split('\t')
            fun get(c: Int) = if (c >= 0 && c < f.size) f[c] else ""
            val id = get(cId).toIntOrNull() ?: 0
            if (id == 0) return@mapNotNull null
            val description = get(cDesc)
            // The XDBF's locked texts are "<String Placeholder>" for most entries.
            val locked = get(cLocked).takeUnless { it.isEmpty() || it.startsWith("<") } ?: description
            Achievement(id, get(cGs).toIntOrNull() ?: 0, get(cName), description, locked, File(dir, "$id.png"))
        }
        // [unlocked.<id>] / filetime = <FILETIME>
        var current: Achievement? = null
        try {
            File(files, "achievements/43430817.toml").forEachLine { raw ->
                val line = raw.trim()
                val unlocked = Regex("""^\[unlocked\.(\d+)]""").find(line)
                if (unlocked != null) {
                    val id = unlocked.groupValues[1].toInt()
                    current = list.firstOrNull { it.id == id }?.also { it.unlockedFiletime = 1 }
                } else {
                    Regex("""^filetime\s*=\s*(\d+)""").find(line)?.let { m ->
                        current?.unlockedFiletime = m.groupValues[1].toLongOrNull()?.takeIf { it != 0L } ?: 1
                    }
                }
            }
        } catch (e: Exception) {
        }
        // Most recent unlocks first (unknown dates after them), then the
        // locked ones in the game's order.
        return list.sortedByDescending { it.unlockedFiletime }
    }
}
