package com.wzmwayne.oledpanel

import android.content.Context
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import java.util.LinkedHashSet

/**
 * Best-effort list of SSIDs the phone already knows. From Android 10 onward
 * getConfiguredNetworks() is restricted for ordinary apps, so an empty result
 * is a normal outcome and the UI falls back to manual SSID entry.
 */
object WifiSuggestions {
    fun load(context: Context): List<String> {
        val result = LinkedHashSet<String>()
        try {
            val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                ?: return emptyList()

            @Suppress("DEPRECATION")
            val configured: List<WifiConfiguration> = wm.configuredNetworks ?: emptyList()
            for (config in configured) {
                val ssid = config.SSID?.trim('"')?.trim().orEmpty()
                if (ssid.isNotEmpty()) result.add(ssid)
            }

            @Suppress("DEPRECATION")
            val current = wm.connectionInfo?.ssid?.trim('"')?.trim().orEmpty()
            if (current.isNotEmpty() && !current.contains("<unknown", ignoreCase = true)) {
                result.add(current)
            }
        } catch (e: SecurityException) {
            // Location permission missing or Wi-Fi list restricted: use manual input.
        } catch (e: Exception) {
            // Vendor ROMs throw a variety of exceptions here.
        }
        return result.toList()
    }
}
