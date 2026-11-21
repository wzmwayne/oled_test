package com.wzmwayne.oledpanel

import android.content.Context

object AppPrefs {
    private const val FILE = "oled_panel"
    private const val KEY_PIN = "pin"
    private const val KEY_LAST_MAC = "last_mac"
    private const val KEY_LAST_NAME = "last_name"

    private fun sp(context: Context) =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    fun pin(context: Context): String = sp(context).getString(KEY_PIN, "") ?: ""

    fun setPin(context: Context, pin: String) {
        sp(context).edit().putString(KEY_PIN, pin).apply()
    }

    fun lastMac(context: Context): String = sp(context).getString(KEY_LAST_MAC, "") ?: ""

    fun lastName(context: Context): String = sp(context).getString(KEY_LAST_NAME, "") ?: ""

    fun setLastDevice(context: Context, mac: String, name: String) {
        sp(context).edit()
            .putString(KEY_LAST_MAC, mac)
            .putString(KEY_LAST_NAME, name)
            .apply()
    }
}
