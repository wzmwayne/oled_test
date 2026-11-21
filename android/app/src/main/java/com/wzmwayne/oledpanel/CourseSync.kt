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
                        o.optInt("parity", 0)
                    )
                )
                added++
            }
            if (added == 0 || acc.size >= total) done(acc, null)
            else fetch(pin, from + added, acc, guard + 1, done)
        }
    }

    /**
     * 整表下发：settt 分片追加，最后 end 提交。
     * 固件只有「取课表 / 发课表」两条通道，不再逐条 addcourse。
     */
    fun pushJson(
        pin: String,
        json: String,
        onProgress: (Int, Int) -> Unit,
        done: (Boolean, String?) -> Unit
    ) {
        val parts = split(json, 300)
        BleManager.send(pin, "settt", JSONObject().put("reset", true)) { res, _ ->
            if (res?.ok != true) {
                done(false, "初始化失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            sendPart(pin, parts, 0, onProgress, done)
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
            BleManager.send(pin, "settt", JSONObject().put("end", true)) { res, _ ->
                if (res?.ok == true) done(true, null)
                else done(false, res?.msg ?: "提交失败")
            }
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

    /** 按字符切片，避免把代理对劈开 */
    private fun split(s: String, size: Int): List<String> {
        val out = ArrayList<String>()
        var i = 0
        while (i < s.length) {
            var end = minOf(i + size, s.length)
            if (end < s.length && Character.isHighSurrogate(s[end - 1])) end--
            out.add(s.substring(i, end))
            i = end
        }
        return out
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
        BleManager.send(pin, "addcourse", params) { res, _ ->
            if (res?.ok != true) {
                done(false, "第 " + (i + 1) + " 门失败：" + (res?.msg ?: "无响应"))
                return@send
            }
            onProgress(i + 1, list.size)
            pushAt(pin, list, i + 1, onProgress, done)
        }
    }
}
