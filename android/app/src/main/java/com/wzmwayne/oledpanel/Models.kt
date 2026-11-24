package com.wzmwayne.oledpanel

import java.util.Locale

data class Course(
    val index: Int,
    val name: String,
    val day: Int,
    val start: Int,
    val end: Int,
    val parity: Int,
    val global: Int = 0        /* 仅事件有意义：1=全局（追加在主页行尾） */
) {
    /** start == end 即事件（单点） */
    val isEvent: Boolean get() = start == end

    fun dayText(): String = DAY_NAMES.getOrElse(day - 1) { "周一" }

    fun timeText(): String =
        if (isEvent) String.format(Locale.US, "%02d:%02d", start / 60, start % 60)
        else String.format(
            Locale.US,
            "%02d:%02d-%02d:%02d",
            start / 60, start % 60, end / 60, end % 60
        )

    fun metaText(): String = dayText() + " " + timeText()

    fun parityText(): String = when (parity) {
        1 -> "单周"
        2 -> "双周"
        else -> "每周"
    }

    companion object {
        val DAY_NAMES = arrayOf("周一", "周二", "周三", "周四", "周五", "周六", "周日")
    }
}
