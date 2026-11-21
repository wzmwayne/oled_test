package com.wzmwayne.oledpanel

import java.util.Locale

data class WifiItem(val ssid: String, val pass: String)

data class Course(
    val index: Int,
    val name: String,
    val day: Int,
    val start: Int,
    val end: Int,
    val parity: Int
) {
    fun dayText(): String = DAY_NAMES.getOrElse(day - 1) { "周一" }

    fun timeText(): String = String.format(
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
