package com.wzmwayne.oledpanel

import org.json.JSONArray
import org.json.JSONObject

/** 课表的读取与整体覆盖推送，两个界面共用 */
object CourseSync {

    fun load(pin: String, done: (List<Course>?, String?) -> Unit) {
        fetch(pin, 0, ArrayList(), 0, done)
    }

    private fun fetch(
        pin: String,
        from: Int,
        acc: MutableList<Course>,
        guard: Int,
        done: (List<Course>?, String?) -> Unit
    ) {
        if (guard > 40) {
            done(acc, null)
            return
        }
        val params = JSONObject().put("from", from).put("count", 4)
        BleManager.send(pin, "listtt", params) { res, _ ->
            if (res?.ok != true) {
                done(null, res?.msg ?: "设备无响应")
                return@send
            }
            val p = res.params ?: JSONObject()
            val total = p.optInt("total", 0)
            val arr = p.optJSONArray("courses") ?: JSONArray()
            var added = 0
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                acc.add(
                    Course(
                        acc.size,
                        o.optString("name", ""),
                        o.optInt("day", 1),
                        o.optInt("start", 0),
                        o.optInt("end", 0),
                        o.optInt("parity", 0),
                        o.optInt("global", 0)
                    )
                )
                added++
            }
            if (added == 0 || acc.size >= total) done(acc, null)
            else fetch(pin, from + added, acc, guard + 1, done)
        }
    }

    /**
     * 整表下发：settt 分片追加（设备边收边落盘），最后带 size + FNV-1a 哈希提交。
     * 设备校验不过就整份丢弃、旧课表不动，所以这里失败可以整表重发。
     * 固件只有「取课表 / 发课表」两条通道，不再逐条 addcourse。
     */
    fun pushJson(
        pin: String,
        json: String,
        onProgress: (Int, Int) -> Unit,
        done: (Boolean, String?) -> Unit
    ) {
        val bytes = json.toByteArray(Charsets.UTF_8)
        val hash = fnv1a(bytes)
        val parts = splitBytes(json, FRAG_BYTES)
        BleManager.send(pin, "settt", JSONObject().put("reset", true)) { res, _ ->
            if (res?.ok != true) {
                done(false, "初始化失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            sendPart(pin, parts, 0, onProgress) { ok, err ->
                if (!ok) {
                    done(false, err)
                    return@sendPart
                }
                val end = JSONObject()
                    .put("end", true)
                    .put("size", bytes.size)
                    .put("hash", hash)
                BleManager.send(pin, "settt", end) { r2, _ ->
                    if (r2?.ok == true) done(true, null)
                    else done(false, r2?.msg ?: "提交失败")
                }
            }
        }
    }

    private fun sendPart(
        pin: String,
        parts: List<String>,
        i: Int,
        onProgress: (Int, Int) -> Unit,
        done: (Boolean, String?) -> Unit
    ) {
        if (i >= parts.size) {
            done(true, null)
            return
        }
        BleManager.send(pin, "settt", JSONObject().put("data", parts[i])) { res, _ ->
            if (res?.ok != true) {
                done(false, "第 " + (i + 1) + " 片发送失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            onProgress(i + 1, parts.size)
            sendPart(pin, parts, i + 1, onProgress, done)
        }
    }

    /** 单片 JSON 正文的 UTF-8 字节上限：加信封后仍要小于 MTU-3（514） */
    private const val FRAG_BYTES = 400

    /** 按 UTF-8 字节切片（不劈开多字节字符，也不劈开代理对） */
    private fun splitBytes(s: String, maxBytes: Int): List<String> {
        val out = ArrayList<String>()
        val sb = StringBuilder()
        var used = 0
        var i = 0
        while (i < s.length) {
            val cp = Character.codePointAt(s, i)
            val n = Character.charCount(cp)
            val b = when {
                cp < 0x80 -> 1
                cp < 0x800 -> 2
                cp < 0x10000 -> 3
                else -> 4
            }
            if (used + b > maxBytes && sb.isNotEmpty()) {
                out.add(sb.toString())
                sb.setLength(0)
                used = 0
            }
            sb.appendRange(s, i, i + n)
            used += b
            i += n
        }
        if (sb.isNotEmpty()) out.add(sb.toString())
        return out
    }

    /** FNV-1a 32 位，与固件 fnv1a() 同一算法（初值 2166136261，乘 16777619） */
    private fun fnv1a(data: ByteArray): Long {
        var h = 2166136261L
        for (b in data) {
            h = (h xor (b.toLong() and 0xFF)) and 0xFFFFFFFFL
            h = (h * 16777619L) and 0xFFFFFFFFL
        }
        return h
    }

    /** 先 cleartt 再逐条 addcourse，等于整张表覆盖 */
    fun push(
        pin: String,
        list: List<Course>,
        onProgress: (Int, Int) -> Unit,
        done: (Boolean, String?) -> Unit
    ) {
        BleManager.send(pin, "cleartt") { res, _ ->
            if (res?.ok != true) {
                done(false, "清空失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            pushAt(pin, list, 0, onProgress, done)
        }
    }

    private fun pushAt(
        pin: String,
        list: List<Course>,
        i: Int,
        onProgress: (Int, Int) -> Unit,
        done: (Boolean, String?) -> Unit
    ) {
        if (i >= list.size) {
            done(true, null)
            return
        }
        val c = list[i]
        val params = JSONObject()
            .put("name", c.name)
            .put("day", c.day)
            .put("start", c.start)
            .put("end", c.end)
            .put("parity", c.parity)
            .put("global", c.global)
        BleManager.send(pin, "addcourse", params) { res, _ ->
            if (res?.ok != true) {
                done(false, "第 " + (i + 1) + " 项失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            onProgress(i + 1, list.size)
            pushAt(pin, list, i + 1, onProgress, done)
        }
    }
}
