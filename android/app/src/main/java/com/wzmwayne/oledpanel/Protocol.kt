package com.wzmwayne.oledpanel

import org.json.JSONObject

/**
 * Wire protocol helpers. The device expects one UTF-8 JSON object per write and
 * answers with exactly one JSON object per notify.
 */
object Protocol {
    const val SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
    const val CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    const val CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"
    /** 二进制批量上传通道（无应答写），配合 bopen/bbad/bclose 使用 */
    const val DATA_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    data class Res(
        val ok: Boolean,
        val action: String,
        val msg: String,
        val params: JSONObject?
    )

    fun request(pin: String, action: String, params: JSONObject? = null): String {
        val obj = JSONObject()
        obj.put("pin", pin)
        obj.put("action", action)
        if (params != null) obj.put("params", params)
        return obj.toString()
    }

    fun parse(text: String): Res? {
        return try {
            val obj = JSONObject(text)
            Res(
                ok = obj.optBoolean("ok", false),
                action = obj.optString("action", ""),
                msg = obj.optString("msg", ""),
                params = obj.optJSONObject("params")
            )
        } catch (e: Exception) {
            null
        }
    }

    fun requestAction(raw: String): String = try {
        JSONObject(raw).optString("action", "")
    } catch (e: Exception) {
        ""
    }

    fun friendlyRequest(raw: String): String = try {
        val obj = JSONObject(raw)
        val action = obj.optString("action", "?")
        val params = obj.optJSONObject("params")
        if (params == null) action else action + "  " + maskedParams(params)
    } catch (e: Exception) {
        raw.take(120)
    }

    fun friendlyResponse(res: Res?, raw: String): String {
        if (res == null) return "无法解析：" + raw.take(100)
        val parts = mutableListOf<String>()
        parts.add(if (res.ok) "成功" else "失败")
        if (res.action.isNotBlank()) parts.add(res.action)
        if (res.msg.isNotBlank()) parts.add(res.msg)
        res.params?.let { parts.add(maskedParams(it)) }
        return parts.joinToString(" · ")
    }

    fun maskedParams(params: JSONObject): String {
        val keys = params.keys()
        val out = mutableListOf<String>()
        while (keys.hasNext()) {
            val key = keys.next()
            val value = params.opt(key)
            val shown = if (key.contains("pass", ignoreCase = true)) "***" else (value?.toString() ?: "null")
            out.add(key + "=" + shown)
        }
        return out.joinToString(", ")
    }

    fun paramsSummary(params: JSONObject?): String =
        if (params == null) "" else maskedParams(params)
}
