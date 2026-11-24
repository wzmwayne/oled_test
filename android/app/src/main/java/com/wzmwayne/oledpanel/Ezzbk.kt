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
 *
 * 设备同时认两种旧头（20 字节、16 字节），校验这里也一样，免得
 * 设备打得开的书被 App 拦下来。
 */
object Ezzbk {

    private const val BLOCK = 16384
    private const val HDR = 24          /* v0.1 头长（不含 meta 与偏移表） */
    private const val HDR_V20 = 20      /* 旧头：有 algo/flags，无 metaLen */
    private const val HDR_V16 = 16      /* 更旧的头：连 algo 都没有，只能 LZ4 */
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
        if (data.size < HDR_V16) return "文件太小，不是 ezzbk"
        if (data[0] != 'E'.code.toByte() || data[1] != 'Z'.code.toByte() ||
            data[2] != 'Z'.code.toByte() || data[3] != 'B'.code.toByte()
        ) return "缺少 EZZB 文件头，只有 .ezzbk 能导入"

        // 先按 v0.1 试，再退到 20 字节头、16 字节头；第一条错误信息最贴近真相
        var firstError: String? = null
        for (pass in 0..2) {
            val err = checkLayout(data, pass)
            if (err == null) return null
            if (firstError == null) firstError = err
        }
        return firstError
    }

    /** pass 0 = v0.1，1 = 20 字节头，2 = 16 字节头 */
    private fun checkLayout(data: ByteArray, pass: Int): String? {
        var algo = 0
        var rawSize = 0L
        var blockSize = 0L
        var count = 0L
        var metaLen = 0L
        var offBase = 0L

        when (pass) {
            0 -> {
                if (data.size < HDR) return "文件太小，不是 ezzbk"
                algo = data[4].toInt() and 0xFF
                rawSize = u32(data, 8)
                blockSize = u32(data, 12)
                count = u32(data, 16)
                metaLen = u32(data, 20)
                offBase = HDR.toLong() + metaLen
            }
            1 -> {
                if (data.size < HDR_V20) return "文件太小，不是 ezzbk"
                algo = data[4].toInt() and 0xFF
                rawSize = u32(data, 8)
                blockSize = u32(data, 12)
                count = u32(data, 16)
                metaLen = 0
                offBase = HDR_V20.toLong()
            }
            else -> {
                algo = ALGO_LZ4
                rawSize = u32(data, 4)
                blockSize = u32(data, 8)
                count = u32(data, 12)
                metaLen = 0
                offBase = HDR_V16.toLong()
            }
        }

        if (algo != ALGO_LZ4 && algo != ALGO_DEFLATE) return "未知压缩算法 " + algo
        if (rawSize <= 0) return "原始长度为 0"
        if (blockSize < 1 || blockSize > BLOCK) return "块大小异常 " + blockSize
        if (count < 1 || count > 4096) return "块数异常 " + count
        if (metaLen > META_MAX) return "元数据区异常 " + metaLen

        // 偏移表紧跟在 meta 之后，头长随 metaLen 浮动
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
