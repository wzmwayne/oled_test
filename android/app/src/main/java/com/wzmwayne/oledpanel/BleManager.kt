package com.wzmwayne.oledpanel

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.CopyOnWriteArraySet

/**
 * Owns the single GATT connection.
 *
 * Ordering rules implemented here:
 *  1. connect -> requestMtu(517) -> wait for onMtuChanged (with a fallback)
 *  2. discoverServices -> enable local notify -> write CCCD -> wait for callback
 *  3. only then are application writes allowed, one at a time, each waiting for
 *     onCharacteristicWrite before the next one starts.
 *
 * All listener callbacks are delivered on the main thread.
 */
object BleManager {
    enum class State { DISCONNECTED, CONNECTING, CONNECTED }

    interface Listener {
        fun onBleState(state: State, name: String?, address: String?)
        fun onBleRssi(rssi: Int)
        fun onBleError(message: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArraySet<Listener>()

    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var notifyReady = false
    private var servicesRequested = false
    private var writeBusy = false
    private var writeRetries = 0
    private val writeQueue = java.util.ArrayDeque<ByteArray>()
    private var writeTimeout: Runnable? = null

    private val pending = ArrayList<Pending>()
    private val rxBuffer = StringBuilder()
    private var rxFlush: Runnable? = null

    private var connectTimeout: Runnable? = null
    private var mtuFallback: Runnable? = null
    private var cccdFallback: Runnable? = null

    var state: State = State.DISCONNECTED
        private set
    var deviceName: String? = null
        private set
    var deviceAddress: String? = null
        private set
    var lastRssi: Int = 0
        private set
    var currentMtu: Int = 23
        private set

    fun addListener(listener: Listener) {
        listeners.add(listener)
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listener.onBleState(state, deviceName, deviceAddress)
        } else {
            main.post { listener.onBleState(state, deviceName, deviceAddress) }
        }
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    fun connect(context: Context, address: String, name: String?) {
        val ctx = context.applicationContext
        val manager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val adapter = manager?.adapter
        if (adapter == null) {
            failConnect("此设备没有蓝牙适配器")
            return
        }
        val enabled = try {
            adapter.isEnabled
        } catch (e: SecurityException) {
            false
        }
        if (!enabled) {
            failConnect("蓝牙未开启，请先打开蓝牙")
            return
        }

        closeGatt()
        clearTimers()
        resetQueues()

        state = State.CONNECTING
        deviceAddress = address
        deviceName = name?.takeIf { it.isNotBlank() } ?: "OLED 设备"
        lastRssi = 0
        currentMtu = 23
        notifyReady = false
        servicesRequested = false
        setState(State.CONNECTING)

        val device = try {
            adapter.getRemoteDevice(address)
        } catch (e: IllegalArgumentException) {
            failConnect("设备地址无效")
            return
        }

        val newGatt = try {
            device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            failConnect("缺少蓝牙连接权限")
            return
        } catch (e: Exception) {
            failConnect("连接启动失败：" + (e.message ?: "未知错误"))
            return
        }
        if (newGatt == null) {
            failConnect("连接启动失败")
            return
        }
        gatt = newGatt

        connectTimeout = Runnable {
            if (state == State.CONNECTING) {
                teardown()
                setState(State.DISCONNECTED)
                failPending()
                notifyError("连接超时（15 秒），请靠近设备后重试")
            }
        }.also { main.postDelayed(it, 15000) }
    }

    fun disconnect() {
        val g = gatt
        if (g == null) {
            setState(State.DISCONNECTED)
            failPending()
            return
        }
        try {
            g.disconnect()
        } catch (e: Exception) {
            // ignore
        }
        main.postDelayed({
            if (gatt === g) {
                teardown()
                setState(State.DISCONNECTED)
                failPending()
                notifyError("连接已断开")
            }
        }, 2000)
    }

    fun refreshRssi() {
        if (state != State.CONNECTED || writeBusy) return
        val g = gatt ?: return
        try {
            g.readRemoteRssi()
        } catch (e: Exception) {
            // RSSI is optional; ignore failures.
        }
    }

    /**
     * Writes the request and routes the matching JSON response to callback.
     * The callback is always invoked on the main thread. A null Res means the
     * request timed out or the device sent something that was not JSON.
     */
    fun send(
        pin: String,
        action: String,
        params: JSONObject? = null,
        callback: ((Protocol.Res?, String) -> Unit)? = null
    ) {
        if (state != State.CONNECTED || gatt == null || !notifyReady) {
            LogStore.add(LogStore.Dir.ERR, "未连接，无法发送 " + action)
            callback?.invoke(null, "")
            return
        }
        val raw = Protocol.request(pin, action, params)
        LogStore.add(LogStore.Dir.TX, raw)

        val request = Pending(action, callback)
        synchronized(pending) { pending.add(request) }
        val timeout = Runnable {
            synchronized(pending) { pending.remove(request) }
            LogStore.add(LogStore.Dir.ERR, action + " 超时无响应")
            callback?.invoke(null, "")
        }
        request.timeout = timeout
        main.postDelayed(timeout, 12000)

        enqueue(raw.toByteArray(Charsets.UTF_8))
    }

    private fun enqueue(data: ByteArray) {
        writeQueue.addLast(data)
        pump()
    }

    @Suppress("DEPRECATION")
    private fun pump() {
        if (!notifyReady || writeBusy || writeQueue.isEmpty()) return
        val g = gatt ?: return
        val chr = characteristic ?: return
        val data = writeQueue.pollFirst() ?: return
        writeBusy = true

        val started: Boolean = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(
                    chr,
                    data,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                chr.value = data
                g.writeCharacteristic(chr)
            }
        } catch (e: SecurityException) {
            false
        } catch (e: Exception) {
            false
        }

        if (!started) {
            writeBusy = false
            if (writeRetries < 3) {
                writeRetries++
                main.postDelayed({
                    if (notifyReady) {
                        writeQueue.addFirst(data)
                        pump()
                    }
                }, 250)
            } else {
                writeRetries = 0
                LogStore.add(LogStore.Dir.ERR, "写入启动失败，已丢弃一条命令")
            }
            return
        }

        writeRetries = 0
        writeTimeout?.let { main.removeCallbacks(it) }
        writeTimeout = Runnable {
            writeBusy = false
            LogStore.add(LogStore.Dir.ERR, "写入超时，继续发送下一条")
            pump()
        }.also { main.postDelayed(it, 3000) }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post { handleConnectionState(g, status, newState) }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            main.post {
                if (gatt !== g) return@post
                currentMtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else 23
                requestServices(g)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post { handleServicesDiscovered(g, status) }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            main.post { handleDescriptorWrite(g, descriptor, status) }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            chr: BluetoothGattCharacteristic,
            status: Int
        ) {
            main.post { handleWriteComplete(g, status) }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, chr: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            val value = chr.value ?: return
            main.post {
                if (gatt === g) consumeNotify(value)
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            chr: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            main.post {
                if (gatt === g) consumeNotify(value)
            }
        }

        override fun onReadRemoteRssi(g: BluetoothGatt, rssi: Int, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) return
            main.post {
                if (gatt !== g) return@post
                lastRssi = rssi
                listeners.forEach { it.onBleRssi(rssi) }
            }
        }
    }

    private fun handleConnectionState(g: BluetoothGatt, status: Int, newState: Int) {
        if (gatt !== g) return

        if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
            val resolvedName = try {
                g.device.name
            } catch (e: SecurityException) {
                null
            } catch (e: Exception) {
                null
            }
            if (!resolvedName.isNullOrBlank()) {
                deviceName = resolvedName
                postState()
            }
            currentMtu = 23
            servicesRequested = false
            val requested = try {
                g.requestMtu(517)
            } catch (e: Exception) {
                false
            }
            if (!requested) {
                requestServices(g)
            }
            mtuFallback?.let { main.removeCallbacks(it) }
            mtuFallback = Runnable {
                if (gatt === g && !servicesRequested) requestServices(g)
            }.also { main.postDelayed(it, 3000) }
            return
        }

        if (newState == BluetoothProfile.STATE_DISCONNECTED) {
            val previous = state
            teardown()
            setState(State.DISCONNECTED)
            failPending()
            if (previous == State.CONNECTING) {
                notifyError("连接失败或已断开，请靠近设备后重试")
            } else {
                notifyError("连接已断开")
            }
            return
        }

        if (status != BluetoothGatt.GATT_SUCCESS) {
            teardown()
            setState(State.DISCONNECTED)
            failPending()
            notifyError("连接失败（GATT 状态 " + status + "）")
        }
    }

    @Suppress("DEPRECATION")
    private fun handleServicesDiscovered(g: BluetoothGatt, status: Int) {
        if (gatt !== g) return
        if (status != BluetoothGatt.GATT_SUCCESS) {
            failConnected(g, "服务发现失败（状态 " + status + "）")
            return
        }
        val service = g.getService(UUID.fromString(Protocol.SERVICE_UUID))
        val chr = service?.getCharacteristic(UUID.fromString(Protocol.CHAR_UUID))
        if (chr == null) {
            failConnected(g, "未找到配置服务，请确认固件版本")
            return
        }
        characteristic = chr

        try {
            g.setCharacteristicNotification(chr, true)
        } catch (e: Exception) {
            // The CCCD write below is the authoritative step.
        }

        val descriptor = chr.getDescriptor(UUID.fromString(Protocol.CCCD_UUID))
        if (descriptor == null) {
            markReady()
            return
        }

        val started: Boolean = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(
                    descriptor,
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(descriptor)
            }
        } catch (e: Exception) {
            false
        }

        if (!started) {
            failConnected(g, "订阅通知失败")
            return
        }

        cccdFallback?.let { main.removeCallbacks(it) }
        cccdFallback = Runnable {
            if (gatt === g && !notifyReady) markReady()
        }.also { main.postDelayed(it, 2000) }
    }

    private fun handleDescriptorWrite(
        g: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int
    ) {
        if (gatt !== g) return
        if (descriptor.uuid != UUID.fromString(Protocol.CCCD_UUID)) return
        if (status == BluetoothGatt.GATT_SUCCESS) {
            markReady()
        } else {
            failConnected(g, "订阅通知失败（状态 " + status + "）")
        }
    }

    private fun handleWriteComplete(g: BluetoothGatt, status: Int) {
        if (gatt !== g) return
        writeTimeout?.let { main.removeCallbacks(it) }
        writeTimeout = null
        writeBusy = false
        if (status != BluetoothGatt.GATT_SUCCESS) {
            LogStore.add(LogStore.Dir.ERR, "写入失败（状态 " + status + "）")
        }
        pump()
    }

    private fun requestServices(g: BluetoothGatt) {
        if (gatt !== g || servicesRequested) return
        servicesRequested = true
        val started = try {
            g.discoverServices()
        } catch (e: Exception) {
            false
        }
        if (!started) {
            failConnected(g, "服务发现启动失败")
        }
    }

    private fun markReady() {
        if (notifyReady) return
        notifyReady = true
        connectTimeout?.let { main.removeCallbacks(it) }
        connectTimeout = null
        mtuFallback?.let { main.removeCallbacks(it) }
        mtuFallback = null
        cccdFallback?.let { main.removeCallbacks(it) }
        cccdFallback = null
        setState(State.CONNECTED)
        main.postDelayed({ refreshRssi() }, 300)
    }

    private fun consumeNotify(value: ByteArray) {
        if (value.isEmpty() || state == State.DISCONNECTED) return
        rxBuffer.append(String(value, Charsets.UTF_8))
        rxFlush?.let { main.removeCallbacks(it) }

        val text = rxBuffer.toString().trim()
        val res = Protocol.parse(text)
        if (res != null) {
            rxBuffer.setLength(0)
            LogStore.add(LogStore.Dir.RX, text, res)
            dispatch(res, text)
            return
        }

        if (rxBuffer.length > 8000) {
            val raw = rxBuffer.toString().trim()
            rxBuffer.setLength(0)
            LogStore.add(LogStore.Dir.RX, raw)
            dispatchRaw(raw)
            return
        }

        rxFlush = Runnable {
            if (rxBuffer.isNotEmpty()) {
                val raw = rxBuffer.toString().trim()
                rxBuffer.setLength(0)
                LogStore.add(LogStore.Dir.RX, raw)
                dispatchRaw(raw)
            }
        }.also { main.postDelayed(it, 800) }
    }

    private fun dispatch(res: Protocol.Res, raw: String) {
        val request = synchronized(pending) {
            if (pending.isEmpty()) {
                null
            } else {
                val index = pending.indexOfFirst { it.action == res.action }
                pending.removeAt(if (index >= 0) index else 0)
            }
        } ?: return
        request.timeout?.let { main.removeCallbacks(it) }
        request.callback?.invoke(res, raw)
    }

    private fun dispatchRaw(raw: String) {
        val request = synchronized(pending) {
            if (pending.isEmpty()) null else pending.removeAt(0)
        } ?: return
        request.timeout?.let { main.removeCallbacks(it) }
        request.callback?.invoke(null, raw)
    }

    private fun failConnected(g: BluetoothGatt, message: String) {
        if (gatt !== g) return
        teardown()
        setState(State.DISCONNECTED)
        failPending()
        notifyError(message)
    }

    private fun failConnect(message: String) {
        teardown()
        setState(State.DISCONNECTED)
        failPending()
        notifyError(message)
    }

    private fun teardown() {
        clearTimers()
        val old = gatt
        gatt = null
        characteristic = null
        notifyReady = false
        servicesRequested = false
        writeBusy = false
        writeRetries = 0
        writeQueue.clear()
        rxBuffer.setLength(0)
        currentMtu = 23
        lastRssi = 0
        if (old != null) {
            try {
                old.disconnect()
            } catch (e: Exception) {
                // ignore
            }
            try {
                old.close()
            } catch (e: Exception) {
                // ignore
            }
        }
    }

    private fun closeGatt() {
        val old = gatt
        gatt = null
        characteristic = null
        if (old != null) {
            try {
                old.disconnect()
            } catch (e: Exception) {
                // ignore
            }
            try {
                old.close()
            } catch (e: Exception) {
                // ignore
            }
        }
    }

    private fun resetQueues() {
        writeQueue.clear()
        writeBusy = false
        writeRetries = 0
        rxBuffer.setLength(0)
        failPending()
    }

    private fun clearTimers() {
        connectTimeout?.let { main.removeCallbacks(it) }
        connectTimeout = null
        mtuFallback?.let { main.removeCallbacks(it) }
        mtuFallback = null
        cccdFallback?.let { main.removeCallbacks(it) }
        cccdFallback = null
        writeTimeout?.let { main.removeCallbacks(it) }
        writeTimeout = null
        rxFlush?.let { main.removeCallbacks(it) }
        rxFlush = null
    }

    private fun failPending() {
        val failed = synchronized(pending) {
            val copy = pending.toList()
            pending.clear()
            copy
        }
        for (request in failed) {
            request.timeout?.let { main.removeCallbacks(it) }
        }
        if (failed.isNotEmpty()) {
            main.post {
                for (request in failed) {
                    request.callback?.invoke(null, "")
                }
            }
        }
    }

    private fun setState(newState: State) {
        state = newState
        postState()
    }

    private fun postState() {
        main.post {
            listeners.forEach { it.onBleState(state, deviceName, deviceAddress) }
        }
    }

    private fun notifyError(message: String) {
        LogStore.add(LogStore.Dir.ERR, message)
        main.post { listeners.forEach { it.onBleError(message) } }
    }

    private class Pending(
        val action: String,
        val callback: ((Protocol.Res?, String) -> Unit)?
    ) {
        var timeout: Runnable? = null
    }
}
