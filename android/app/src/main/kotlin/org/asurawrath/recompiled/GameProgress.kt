package org.asurawrath.recompiled

import java.io.File

/**
 * Game data › Progress: the story progress in the game's save and the episode
 * list from the game's own config. A port of src/ui/save_data.cpp (the
 * desktop launcher's Saves › Progress); keep the two in step.
 */
object GameProgress {
    const val EASY = 0
    const val NORMAL = 1
    const val HARD = 2
    const val DIFFICULTIES = 3

    class StoryRecord(val id: Int, val clearFlag: Int, val rank: Array<String>) {
        val reached get() = clearFlag != 0
        val cleared get() = clearFlag and 4 != 0 || rank.any { it.isNotEmpty() }
    }

    class Summary(
        val stories: List<StoryRecord>,
        val difficulty: Int,  // -1 = unknown
        val battlePoints: Int,
        val enemiesDefeated: Int,
    )

    class Episode(val id: Int, val part: Int, val number: String, val title: String)

    // --- UE3 tagged properties (the save, big-endian) ---------------------------

    private class Reader(val d: ByteArray, var end: Int, var pos: Int) {
        var ok = true
        fun u32(): Int {
            if (pos + 4 > end) { ok = false; return 0 }
            val v = (d[pos].toInt() and 0xFF shl 24) or (d[pos + 1].toInt() and 0xFF shl 16) or
                (d[pos + 2].toInt() and 0xFF shl 8) or (d[pos + 3].toInt() and 0xFF)
            pos += 4
            return v
        }
        // Inline FString (ANSI, with its terminating zero).
        fun string(): String {
            val n = u32()
            if (!ok || n <= 0 || n > 1024 || pos + n > end) { ok = false; return "" }
            val s = String(d, pos, n - 1, Charsets.ISO_8859_1)
            pos += n
            return s
        }
    }

    private class Prop(val name: String, val type: String, val index: Int) {
        var tag = ""
        var int = 0
        var nameValue = ""                        // ByteProperty's enum value
        val items = ArrayList<List<Prop>>()       // ArrayProperty of structs
        var fields: List<Prop> = emptyList()      // StructProperty
    }

    private fun readProp(r: Reader, end: Int, depth: Int): Prop? {
        val name = r.string()
        if (!r.ok || name == "None") return null
        val type = r.string()
        val size = r.u32()
        val p = Prop(name, type, r.u32())
        if (!r.ok) return null
        if (type == "ByteProperty") {
            p.tag = r.string()
            p.nameValue = r.string()
            return if (r.ok) p else null
        }
        if (type == "StructProperty") p.tag = r.string()
        val start = r.pos
        if (size < 0 || size > end - minOf(end, start)) { r.ok = false; return null }
        val v = Reader(r.d, start + size, start)
        when {
            type == "IntProperty" || type == "BoolProperty" -> p.int = v.u32()
            type == "StructProperty" && depth < 8 -> p.fields = readProps(v, start + size, depth + 1)
            type == "ArrayProperty" && depth < 8 -> {
                val count = v.u32()
                if (size != 4 + count * 4) {
                    var i = 0
                    while (i < count && v.ok) { p.items.add(readProps(v, start + size, depth + 1)); i++ }
                }
            }
        }
        r.pos = start + size
        return p
    }

    private fun readProps(r: Reader, end: Int, depth: Int): List<Prop> {
        val props = ArrayList<Prop>()
        while (r.ok && r.pos < end) props.add(readProp(r, end, depth) ?: break)
        return props
    }

    private fun List<Prop>.find(name: String, index: Int = 0) = firstOrNull { it.name == name && it.index == index }
    private fun List<Prop>.int(name: String, index: Int = 0) = find(name, index)?.int ?: 0

    // "BCRRank_S" -> "S", "BCRRank_None" -> "".
    private fun rankOf(p: Prop?): String {
        val v = p?.nameValue ?: return ""
        val rank = v.substringAfterLast('_')
        return if (rank == "None") "" else rank
    }

    /** The save's story progress; null if the file isn't a save. */
    fun readSave(file: File): Summary? {
        val d = try { file.readBytes() } catch (e: Exception) { return null }
        if (d.size < 16) return null
        // u32 0, u32 payload size, then the properties.
        val props = readProps(Reader(d, d.size, 8), d.size, 0)
        val stories = props.find("StoryClearFlgs") ?: return null
        val records = stories.items.map { item ->
            val results = item.find("ResultInfo")
            StoryRecord(item.int("Id"), item.int("ClearFlg"), Array(DIFFICULTIES) { i ->
                results?.items?.getOrNull(i)?.let { rankOf(it.find("TotalRank")) } ?: ""
            })
        }
        val difficulty = props.find("SystemParam")?.fields?.find("GameDifficulty")?.nameValue?.let {
            when {
                it.endsWith("_Easy") -> EASY
                it.endsWith("_Normal") -> NORMAL
                it.endsWith("_Hard") -> HARD
                else -> -1
            }
        } ?: -1
        return Summary(records, difficulty, props.int("SumBattlePoint"),
            props.int("KillEnemyNum", 0) + props.int("KillEnemyNum", 1))
    }

    /** The first file of a save folder that reads as a save (as the desktop's ScanSaves). */
    fun readSaveFolder(folder: File): Summary? =
        (folder.listFiles(File::isFile) ?: emptyArray()).sortedBy(File::getName).firstNotNullOfOrNull(::readSave)

    // --- Coalesced_<LANG>.bin -------------------------------------------------

    private class Coalesced(
        val chapters: Map<String, Map<String, String>>,  // BCUIDataProvider_ChapterInfo sections
        val titles: Map<String, String>,                // "S000" -> title
    )

    private fun languageCode(language: Int) = when (language) {
        2 -> "JPN"; 3 -> "DEU"; 4 -> "FRA"; 5 -> "ESN"; 6 -> "ITA"; else -> "INT"
    }

    private fun parseCoalesced(d: ByteArray): Coalesced? {
        var pos = 0
        var ok = true
        fun i32(): Int {
            if (pos + 4 > d.size) { ok = false; return 0 }
            val v = (d[pos].toInt() and 0xFF shl 24) or (d[pos + 1].toInt() and 0xFF shl 16) or
                (d[pos + 2].toInt() and 0xFF shl 8) or (d[pos + 3].toInt() and 0xFF)
            pos += 4
            return v
        }
        // FString: positive length = ANSI, negative = UTF-16LE; both end with a zero.
        fun fstring(): String {
            val n = i32()
            if (!ok || n == 0) return ""
            val s = if (n > 0) {
                if (n > d.size - pos) { ok = false; return "" }
                String(d, pos, n, Charsets.ISO_8859_1).also { pos += n }
            } else {
                val chars = -n.toLong()
                if (chars * 2 > d.size - pos) { ok = false; return "" }
                String(d, pos, (chars * 2).toInt(), Charsets.UTF_16LE).also { pos += (chars * 2).toInt() }
            }
            return s.trimEnd('\u0000')
        }
        val chapters = HashMap<String, HashMap<String, String>>()
        val titles = HashMap<String, String>()
        val files = i32()
        var f = 0
        while (ok && f < files) {
            val lower = fstring().lowercase()
            val gameIni = lower.endsWith("bcgame.ini")
            val strings = !lower.contains("_defaultfont") && lower.contains("\\bcgame.") && !gameIni
            val sections = i32()
            var s = 0
            while (ok && s < sections) {
                val section = fstring()
                val chapter = gameIni && section.contains("BCUIDataProvider_ChapterInfo")
                val title = strings && section == "Title"
                val keys = i32()
                var k = 0
                while (ok && k < keys) {
                    val key = fstring()
                    val value = fstring()
                    if (chapter) chapters.getOrPut(section) { HashMap() }[key] = value
                    if (title) titles[key] = value
                    k++
                }
                s++
            }
            f++
        }
        return if (ok && chapters.isNotEmpty()) Coalesced(chapters, titles) else null
    }

    private val cache = HashMap<String, Coalesced?>()

    private fun coalesced(game: File, language: Int): Coalesced? {
        val file = File(game, "BCGame/CookedXbox360/Coalesced_${languageCode(language)}.bin")
        val parsed = synchronized(cache) {
            cache.getOrPut(file.path) { try { parseCoalesced(file.readBytes()) } catch (e: Exception) { null } }
        }
        return parsed ?: if (language != 1) coalesced(game, 1) else null
    }

    /** The main story's episodes in order (`language`: XLanguage, 1 English ... 6 Italian). */
    fun episodes(game: File, language: Int): List<Episode> {
        val c = coalesced(game, language) ?: return emptyList()
        return c.chapters.values.filter { it["bDLC"] != "TRUE" }.map { keys ->
            val id = keys["Id"]?.trim()?.toIntOrNull() ?: 0
            Episode(id, keys["ChapterNo"]?.trim()?.toIntOrNull() ?: 0, keys["StoryNo"] ?: "", title(game, language, id))
        }.sortedBy { it.id }
    }

    /** Any stage's title ("S<id>" in [Title]), e.g. DLC episodes; empty if none. */
    fun title(game: File, language: Int, id: Int): String =
        coalesced(game, language)?.titles?.get(String.format(java.util.Locale.ROOT, "S%03d", id)) ?: ""
}
