package com.wzmwayne.oledpanel

import android.os.Handler
import android.os.Looper
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CopyOnWriteArrayList

/**
 * Process-wide, in-memory log. BLE callbacks are moved to the main thread by
 * BleManager before they get here, so listeners can touch views directly.
 */
object LogStore {
    enum class Dir { TX, RX, ERR }

    data class Entry(
        val id: Long,
        val time: Long,
        val dir: Dir,
        val raw: String,
        val res: Protocol.Res? = null
    )

    interface Listener {
        fun onLogAdded(entry: Entry)
        fun onLogsCleared()
    }

    private const val MAX = 2000
    private val main = Handler(Looper.getMainLooper())
    private val entries = ArrayList<Entry>()
    private val listeners = CopyOnWriteArrayList<Listener>()
    private var nextId = 1L
    private val clock = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    fun add(dir: Dir, raw: String, res: Protocol.Res? = null) {
        val entry = synchronized(entries) {
            val e = Entry(nextId++, System.currentTimeMillis(), dir, raw, res)
            entries.add(e)
            while (entries.size > MAX) entries.removeAt(0)
            e
        }
        main.post { listeners.forEach { it.onLogAdded(entry) } }
    }

    fun clear() {
        synchronized(entries) { entries.clear() }
        main.post { listeners.forEach { it.onLogsCleared() } }
    }

    fun snapshot(): List<Entry> = synchronized(entries) { entries.toList() }

    fun addListener(listener: Listener) {
        listeners.add(listener)
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    fun formatLine(entry: Entry): String {
        val time = clock.format(Date(entry.time))
        return "[" + time + "] " + entry.dir.name + " " + entry.raw
    }
}
