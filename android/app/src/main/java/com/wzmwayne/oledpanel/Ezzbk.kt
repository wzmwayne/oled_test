package com.wzmwayne.oledpanel

/**
 * .ezzbk 容器校验（设备端阅读格式，规范见 neobook/notes/ezzbk-spec-v0.1.md）。
 *
 * 布局（小端，v0.1）：
 *   "EZZB" | u8 algo | u8 flags | u16 reserved
 *   | u32 rawSize | u32 blockSize | u32 blockCount | u32 metaLen
 *   | meta[metaLen]（每行 key=value，UTF-8：title / writer / intro / source / date）
 *   | u32 off[count+1] | 各块压缩数据
 *   algo 0 = LZ4，1 = raw deflate（16KB 窗口）。
 *
 * ezbk -> ezzbk 的转换交给 neobook（它有封面、章节和元数据），
 * App 这边只在上传前挡住选错的文件，不做打包。
 */
object Ezzbk {

    private const val BLOCK = 16384
    private const val HDR = 24
    private const val META_MAX = 4096
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
        val metaLen = u32(data, 20)
        if (rawSize <= 0) return "原始长度为 0"
        if (blockSize < 1 || blockSize > BLOCK) return "块大小异常 " + blockSize
        if (count < 1 || count > 4096) return "块数异常 " + count
        if (metaLen > META_MAX) return "元数据区异常 " + metaLen

        // 偏移表在 meta 之后，头长随 metaLen 浮动
        val offBase = HDR.toLong() + metaLen
        val hdrLen = offBase + 4L * (count + 1)
        if (hdrLen > data.size) return "头部越界，文件可能被截断"

        var prev = -1L
        for (i in 0..count.toInt()) {
            val off = u32(data, (offBase + i * 4).toInt())
            if (off < hdrLen || off > data.size) return "第 " + i + " 个偏移越界"
            if (i == 0 && off != hdrLen) return "首块偏移不对"
            if (i > 0 && off <= prev) return "偏移不是递增的"
            prev = off
        }
        if (prev != data.size.toLong()) return "末块偏移与文件长度不符（可能没传完整）"
        return null
    }
}
