package com.wzmwayne.oledpanel

import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/**
 * 课表元数据：给人和 AI 看的文本格式，导入导出都走它。
 *
 * {"v":1,"c":[{"n":"数学","d":1,"s":"08:00","e":"08:40","p":0}]}
 *   n 课程名  d 星期(1-7)  s/e 起止 "HH:MM" 或分钟数  p 0每周 1单周 2双周
 */
object TimetableMeta {

    fun export(list: List<Course>): String {
        val arr = JSONArray()
        list.sortedWith(compareBy({ it.day }, { it.start })).forEach { c ->
            val o = JSONObject()
            o.put("n", c.name)
            o.put("d", c.day)
            o.put("s", hhmm(c.start))
            o.put("e", hhmm(c.end))
            o.put("p", c.parity)
            arr.put(o)
        }
        val root = JSONObject()
        root.put("v", 1)
        root.put("c", arr)
        return root.toString(2)
    }

    /** 紧凑版，上传用：省掉缩进，40 门课约 1.8KB，塞得进固件 4KB 缓冲 */
    fun exportCompact(list: List<Course>): String {
        val arr = JSONArray()
        list.sortedWith(compareBy({ it.day }, { it.start })).forEach { c ->
            val o = JSONObject()
            o.put("n", c.name)
            o.put("d", c.day)
            o.put("s", c.start)
            o.put("e", c.end)
            o.put("p", c.parity)
            arr.put(o)
        }
        val root = JSONObject()
        root.put("c", arr)
        return root.toString()
    }

    /** 解析失败返回 null；单条不合法的会被跳过而不是整体失败 */
    fun parse(text: String): List<Course>? {
        val body = text.trim()
        if (body.isEmpty()) return null
        return try {
            val root = JSONObject(body)
            val arr = root.optJSONArray("c") ?: root.optJSONArray("courses") ?: return null
            val out = ArrayList<Course>()
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val name = o.optString("n", o.optString("name", "")).trim()
                if (name.isEmpty()) continue
                val day = o.optInt("d", o.optInt("day", 0))
                if (day !in 1..7) continue
                val s = parseTime(o.opt("s") ?: o.opt("start")) ?: continue
                val e = parseTime(o.opt("e") ?: o.opt("end")) ?: continue
                if (e <= s || e > 1439) continue
                val p = o.optInt("p", o.optInt("parity", 0)).coerceIn(0, 2)
                out.add(Course(out.size, truncateUtf8(name, 24), day, s, e, p))
            }
            out
        } catch (e: Exception) {
            null
        }
    }

    private fun parseTime(v: Any?): Int? = when (v) {
        is Number -> v.toInt()
        is String -> {
            val t = v.trim()
            if (t.contains(":")) {
                val parts = t.split(":")
                if (parts.size != 2) null else {
                    val h = parts[0].toIntOrNull()
                    val m = parts[1].toIntOrNull()
                    if (h != null && m != null && h in 0..23 && m in 0..59) h * 60 + m else null
                }
            } else t.toIntOrNull()
        }
        else -> null
    }

    private fun hhmm(min: Int): String =
        String.format(Locale.US, "%02d:%02d", min / 60, min % 60)

    private fun truncateUtf8(s: String, maxBytes: Int): String {
        val b = s.toByteArray(Charsets.UTF_8)
        if (b.size <= maxBytes) return s
        var end = maxBytes
        while (end > 0 && (b[end].toInt() and 0xC0) == 0x80) end--
        return String(b, 0, end, Charsets.UTF_8)
    }
}
