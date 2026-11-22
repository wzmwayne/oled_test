package com.wzmwayne.oledpanel

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ezbk(stable) -> .ezb 设备格式。
 *
 * 设备没有 zlib，所以用 LZ4 分块（每块 4KB，可随机读取）。
 * 布局： "EZB1" | u32 rawSize | u32 blockSize | u32 blockCount
 *        | u32 off[count+1] | 各块
 *
 * 转换时剥掉 @image / @cover / @blob 与 ---，正文去掉行首反斜杠，
 * 保留 # / ## 标题行给设备做目录。
 */
object Ezzbk {

    private const val BLOCK = 16384

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

    /** 打成 .ezb 容器 */
    fun wrap(raw: ByteArray): ByteArray {
        val blocks = ArrayList<ByteArray>()
        var off = 0
        while (off < raw.size) {
            val len = minOf(BLOCK, raw.size - off)
            blocks.add(lz4(raw, off, len))
            off += len
        }
        if (blocks.isEmpty()) blocks.add(lz4(raw, 0, 0))

        val n = blocks.size
        val headerLen = 16 + 4 * (n + 1)
        val out = ByteArrayOutputStream(headerLen + blocks.sumOf { it.size })
        out.write("EZZB".toByteArray(Charsets.US_ASCII))

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
