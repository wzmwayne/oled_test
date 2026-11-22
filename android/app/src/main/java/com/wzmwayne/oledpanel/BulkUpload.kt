package com.wzmwayne.oledpanel

import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * 蓝牙高速传书：二进制数据通道 + 分段校验。
 *
 * 设备侧约定（固件 btconf.cpp）：
 *   bopen  {name,size}          -> params.chunk 每段校验大小（默认 32KB）
 *   数据通道写 <=512 字节原始字节，设备边收边落盘并累计 FNV-1a
 *   每收满一段 NOTIFY {"action":"bputack","params":{len,hash}}
 *   bbad                        -> 回退到上一次确认的位置
 *   bclose {size,hash}          -> 整本核对，通过才入库
 *
 * 校验段越大越快；对不上就退回上一次确认点重传那一段。
 */
object BulkUpload {

    interface Callback {
        fun onProgress(sent: Int, total: Int, kbps: Int)
        fun onDone(ok: Boolean, message: String, failAt: Int)
    }

    private val main = Handler(Looper.getMainLooper())

    /* FNV-1a 32 位，与固件同一实现 */
    private fun fnv(h0: Int, d: ByteArray, from: Int, len: Int): Int {
        var h = h0
        var i = from
        val end = from + len
        while (i < end) {
            h = h xor (d[i].toInt() and 0xFF)
            h *= 16777619
            i++
        }
        return h
    }

    @Volatile private var ackLen = -1
    @Volatile private var ackHash = 0
    @Volatile private var ackAt = 0L

    /** 同步等一次 ack（在后台线程调用） */
    private fun waitAck(lastLen: Int, timeoutMs: Long): Pair<Int, Int>? {
        val until = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < until) {
            val l = ackLen
            if (l >= lastLen && ackAt > 0) return Pair(l, ackHash)
            Thread.sleep(5)
        }
        return null
    }

    /** 同步发一条 JSON 命令并等回包（在后台线程调用） */
    private fun cmd(pin: String, action: String, params: JSONObject?, timeoutMs: Long): Protocol.Res? {
        val latch = CountDownLatch(1)
        var out: Protocol.Res? = null
        main.post {
            BleManager.send(pin, action, params) { res, _ ->
                out = res
                latch.countDown()
            }
        }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return out
    }

    fun start(pin: String, fileName: String, data: ByteArray, cb: Callback) {
        Thread {
            var failAt = 0
            var accepted = false
            try {
                val total = data.size
                val totalHash = fnv(-2128831035, data, 0, total)

                BleManager.onBulkAck = { len, hash ->
                    ackLen = len
                    ackHash = hash
                    ackAt = System.currentTimeMillis()
                }
                BleManager.boostConnection()

                val open = cmd(pin, "bopen",
                    JSONObject().put("name", fileName).put("size", total), 15000)
                if (open?.ok != true) {
                    finish(cb, false, open?.msg ?: "设备拒绝开始", 0)
                    return@Thread
                }
                var chunk = open.params?.optInt("chunk", 32768) ?: 32768
                if (chunk < 4096) chunk = 32768
                if (BleManager.currentMtu < 200 && chunk > 8192) chunk = 8192

                var sent = 0
                var sentHash = -2128831035
                var confirmed = 0
                var confirmedHash = -2128831035
                ackLen = 0; ackHash = sentHash; ackAt = 0
                val t0 = System.currentTimeMillis()
                var lastReport = t0

                /* 单包上限 = 协商 MTU - 3。写 512 字节而 MTU 只有 247 时，
                 * Android 直接返回失败，表现就是"连接已断开"。 */
                val pkt = (BleManager.currentMtu - 3).coerceIn(20, 512)

                while (sent < total) {
                    val segEnd = minOf(sent + chunk, total)
                    var off = sent
                    var pkts = 0
                    while (off < segEnd) {
                        val n = minOf(pkt, segEnd - off)
                        val piece = data.copyOfRange(off, off + n)
                        var tries = 0
                        while (!BleManager.bulkWrite(piece)) {
                            /* 队列满会短暂失败，退让一下重试，别一失败就报断线 */
                            if (++tries >= 60 || BleManager.state != BleManager.State.CONNECTED) {
                                finish(cb, false,
                                    "写入失败（MTU " + BleManager.currentMtu + "，第 " + off + " 字节）", off)
                                return@Thread
                            }
                            Thread.sleep(5)
                        }
                        sentHash = fnv(sentHash, data, off, n)
                        off += n
                        pkts++
                        if (pkts % 8 == 0) Thread.sleep(3)
                    }
                    sent = segEnd

                    val ok = waitAck(sent, 4000)
                    if (ok == null) {
                        finish(cb, false, "等待校验回包超时", sent)
                        return@Thread
                    }
                    val (devLen, devHash) = ok
                    if (devLen != sent || devHash != sentHash) {
                        /* 对不上：退回上一次确认点重传这一段 */
                        val bad = cmd(pin, "bbad", null, 8000)
                        if (bad?.ok != true) {
                            finish(cb, false, "回退失败：" + (bad?.msg ?: "无响应"), sent)
                            return@Thread
                        }
                        sent = confirmed
                        sentHash = confirmedHash
                        Log.w("BulkUpload", "校验不符，回退到 " + sent)
                        continue
                    }
                    confirmed = sent
                    confirmedHash = sentHash
                    /* 段确认后才推进流控游标，否则 bbad 会退到更早的位置 */
                    ackAt = 0

                    val now = System.currentTimeMillis()
                    if (now - lastReport >= 200 || sent == total) {
                        lastReport = now
                        val kbps = if (now > t0) ((sent.toLong() * 1000 / (now - t0)) / 1024).toInt() else 0
                        val s = sent
                        main.post { cb.onProgress(s, total, kbps) }
                    }
                }

                val close = cmd(pin, "bclose",
                    JSONObject().put("size", total).put("hash", totalHash), 20000)
                val ok = close?.ok == true
                accepted = ok
                finish(cb, ok, close?.msg ?: if (ok) "完成" else "设备无响应", total)
            } catch (e: Exception) {
                finish(cb, false, "传输异常：" + (e.message ?: ""), failAt)
            } finally {
                BleManager.onBulkAck = null
                if (!accepted) cmd(pin, "bcancel", null, 5000)   /* 让设备把半截文件删掉 */
            }
        }.start()
    }

    private fun finish(cb: Callback, ok: Boolean, msg: String, at: Int) {
        main.post { cb.onDone(ok, msg, at) }
    }
}
