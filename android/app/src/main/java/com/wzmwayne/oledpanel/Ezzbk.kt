package com.wzmwayne.oledpanel

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ezbk(stable) -> .ezzbk 设备格式。
 *
 * 设备靠 ESP32-C3 ROM 里的 tinfl 解 raw deflate，所以每块独立 deflate
 * （块 16KB，可随机读取；块内距离天然 ≤ 块内偏移，非回卷输出缓冲即可）。
 *
 * 布局（v2）： "EZZB" | u8 algo | u8 flags | u16 reserved
 *             | u32 rawSize | u32 blockSize | u32 blockCount
 *             | u32 off[count+1] | 各块
 *             algo 0 = LZ4，1 = raw deflate（默认）。
 *
 * 转换时剥掉 @image / @cover / @blob 与 ---，正文去掉行首反斜杠，
 * 保留 # / ## 标题行给设备做目录。
 */
object Ezzbk {

    private const val BLOCK = 16384
    private const val HDR = 20
    private const val ALGO_LZ4 = 0
    private const val ALGO_DEFLATE = 1

    private fun u32(d: ByteArray, at: Int): Long =
        (d[at].toLong() and 0xFF) or
            ((d[at + 1].toLong() and 0xFF) shl 8) or
            ((d[at + 2].toLong() and 0xFF) shl 16) or
            ((d[at + 3].toLong() and 0xFF) shl 24)

    /**
     * 校验 .ezzbk 容器结构。返回 null 表示合格，否则是给用户看的原因。
     * 只做结构检查（不逐块解压），足以挡住"随便选了个文件"的情况。
     */
    fun verifyContainer(data: ByteArray): String? {
        if (data.size < HDR) return "文件太小，不是 ezzbk"
        if (data[0] != 'E'.code.toByte() || data[1] != 'Z'.code.toByte() ||
            data[2] != 'Z'.code.toByte() || data[3] != 'B'.code.toByte()
        ) return "缺少 EZZB 文件头，只有 .ezzbk 能导入"

        val algo = data[4].toInt() and 0xFF
        if (algo != ALGO_LZ4 && algo != ALGO_DEFLATE) return "未知压缩算法 " + algo

        val rawSize = u32(data, 8)
        val blockSize = u32(data, 12)
        val count = u32(data, 16)
        if (rawSize <= 0) return "原始长度为 0"
        if (blockSize < 1 || blockSize > BLOCK) return "块大小异常 " + blockSize
        if (count < 1 || count > 262144) return "块数异常 " + count

        val hdrLen = HDR.toLong() + 4L * (count + 1)
        if (hdrLen > data.size) return "头部越界，文件可能被截断"

        var prev = -1L
        for (i in 0..count.toInt()) {
            val off = u32(data, HDR + i * 4)
            if (off < hdrLen || off > data.size) return "第 " + i + " 个偏移越界"
            if (i == 0 && off != hdrLen) return "首块偏移不对"
            if (i > 0 && off <= prev) return "偏移不是递增的"
            prev = off
        }
        if (prev != data.size.toLong()) return "末块偏移与文件长度不符（可能没传完整）"
        return null
    }

    fun isEzbk(data: ByteArray): Boolean {
        val head = String(data, 0, minOf(6, data.size), Charsets.UTF_8)
        return head.removePrefix("\uFEFF") == "EZBK/1"
    }

    /** ezbk 或纯文本 -> 线性正文 */
    fun toRaw(data: ByteArray): ByteArray {
        val text = String(data, Charsets.UTF_8).removePrefix("\uFEFF")
        if (!text.startsWith("EZBK/1")) return text.toByteArray(Charsets.UTF_8)

        val lines = text.split('\n')
        var i = 1
        while (i < lines.size && lines[i].startsWith("@")) i++

        val out = StringBuilder(text.length)
        for (k in i until lines.size) {
            val line = lines[k].trimEnd('\r')
            when {
                line == "---" -> {}
                line.startsWith("@") -> {}
                line.startsWith("#") -> out.append(line).append('\n')
                line.startsWith("\\") -> {
                    val body = line.substring(1)
                    if (body.startsWith("#") || body.startsWith("\\")) out.append('\\')
                    out.append(body).append('\n')
                }
                line.isBlank() -> out.append('\n')
                else -> out.append(line).append('\n')
            }
        }
        return out.toString().toByteArray(Charsets.UTF_8)
    }

    /** 打成 .ezzbk 容器 */
    fun wrap(raw: ByteArray, algo: Int = ALGO_DEFLATE): ByteArray {
        val blocks = ArrayList<ByteArray>()
        var off = 0
        while (off < raw.size) {
            val len = minOf(BLOCK, raw.size - off)
            blocks.add(if (algo == ALGO_DEFLATE) deflate(raw, off, len) else lz4(raw, off, len))
            off += len
        }
        if (blocks.isEmpty()) {
            blocks.add(if (algo == ALGO_DEFLATE) deflate(raw, 0, 0) else lz4(raw, 0, 0))
        }

        val n = blocks.size
        val headerLen = HDR + 4 * (n + 1)
        val out = ByteArrayOutputStream(headerLen + blocks.sumOf { it.size })
        out.write("EZZB".toByteArray(Charsets.US_ASCII))
        out.write(byteArrayOf(algo.toByte(), 0, 0, 0))   // algo | flags | reserved

        val head = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        head.putInt(raw.size)
        head.putInt(BLOCK)
        head.putInt(n)
        out.write(head.array())

        var cur = headerLen
        val offs = ByteBuffer.allocate(4 * (n + 1)).order(ByteOrder.LITTLE_ENDIAN)
        for (b in blocks) {
            offs.putInt(cur)
            cur += b.size
        }
        offs.putInt(cur)
        out.write(offs.array())
        for (b in blocks) out.write(b)
        return out.toByteArray()
    }

    /** raw deflate（单块独立流，无 zlib 头尾） */
    private fun deflate(src: ByteArray, start: Int, len: Int): ByteArray {
        val d = java.util.zip.Deflater(java.util.zip.Deflater.BEST_COMPRESSION, true)
        val out = ByteArrayOutputStream(len / 2 + 32)
        try {
            d.setInput(src, start, len)
            d.finish()
            val buf = ByteArray(8192)
            while (!d.finished()) {
                val k = d.deflate(buf)
                if (k <= 0) break
                out.write(buf, 0, k)
            }
        } finally {
            d.end()
        }
        return out.toByteArray()
    }

    /** LZ4 block 压缩：贪心 + 4 字节哈希，窗口 64KB */
    private fun lz4(src: ByteArray, start: Int, len: Int): ByteArray {
        val out = ByteArrayOutputStream(len / 2 + 16)
        val table = HashMap<Int, Int>(1 shl 14)
        var anchor = 0
        var i = 0
        val limit = len - 12

        fun emitLen(v0: Int) {
            var v = v0
            while (v >= 255) {
                out.write(255)
                v -= 255
            }
            out.write(v)
        }

        while (i < limit) {
            val p = start + i
            val key = (src[p].toInt() and 0xFF) or
                ((src[p + 1].toInt() and 0xFF) shl 8) or
                ((src[p + 2].toInt() and 0xFF) shl 16) or
                ((src[p + 3].toInt() and 0xFF) shl 24)
            val ref = table.put(key, i)
            if (ref != null && i - ref < 65535) {
                val r = start + ref
                if (src[r] == src[p] && src[r + 1] == src[p + 1] &&
                    src[r + 2] == src[p + 2] && src[r + 3] == src[p + 3]
                ) {
                    var mlen = 4
                    while (i + mlen < len && mlen < 65535 &&
                        src[r + mlen] == src[start + i + mlen]
                    ) mlen++

                    val lit = i - anchor
                    val mlCode = (if (mlen - 4 >= 15) 15 else mlen - 4)
                    out.write(((if (lit >= 15) 15 else lit) shl 4) or mlCode)
                    if (lit >= 15) emitLen(lit - 15)
                    out.write(src, start + anchor, lit)

                    val offset = i - ref
                    out.write(offset and 0xFF)
                    out.write((offset shr 8) and 0xFF)
                    if (mlen - 4 >= 15) emitLen(mlen - 4 - 15)

                    i += mlen
                    anchor = i
                    continue
                }
            }
            i++
        }

        val lit = len - anchor
        out.write((if (lit >= 15) 15 else lit) shl 4)
        if (lit >= 15) emitLen(lit - 15)
        out.write(src, start + anchor, lit)
        return out.toByteArray()
    }
}
