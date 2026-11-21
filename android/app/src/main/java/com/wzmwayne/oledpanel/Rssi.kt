package com.wzmwayne.oledpanel

import android.content.Context
import androidx.core.content.ContextCompat

object Rssi {
    fun label(rssi: Int): String = if (rssi == 0 || rssi == 127) "—" else rssi.toString() + " dBm"

    fun color(context: Context, rssi: Int): Int = ContextCompat.getColor(
        context,
        when {
            rssi == 0 || rssi == 127 -> R.color.dim
            rssi >= -60 -> R.color.ok
            rssi >= -75 -> R.color.warn
            else -> R.color.err
        }
    )
}
