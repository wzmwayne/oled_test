package com.wzmwayne.oledpanel

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView

class ScanActivity : AppCompatActivity(), BleManager.Listener {

    private var bleAdapter: BluetoothAdapter? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanning = false
    private var filteredPhase = true
    private var openedCommand = false
    private var unsupported = false

    private val handler = Handler(Looper.getMainLooper())
    private val found = LinkedHashMap<String, ScanResult>()
    private lateinit var deviceAdapter: DeviceAdapter

    private lateinit var tvDot: TextView
    private lateinit var tvScanStatus: TextView
    private lateinit var tvEmpty: TextView
    private lateinit var btnScan: MaterialButton
    private lateinit var btnCommand: MaterialButton
    private lateinit var btnDisconnect: MaterialButton
    private lateinit var cardNotice: MaterialCardView
    private lateinit var tvNoticeTitle: TextView
    private lateinit var tvNoticeBody: TextView
    private lateinit var btnSettings: MaterialButton
    private lateinit var cardLast: MaterialCardView
    private lateinit var tvLastName: TextView
    private lateinit var tvLastMac: TextView
    private lateinit var btnLastConnect: MaterialButton

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) {
            cardNotice.visibility = View.GONE
            startScan()
        } else {
            showPermissionNotice()
        }
    }

    private val enableBluetoothLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            startScan()
        } else {
            toast("需要打开蓝牙才能扫描设备")
        }
    }

    private val fallbackRunnable = Runnable { switchToUnfiltered() }
    private val stopRunnable = Runnable { stopScan() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_scan)

        tvDot = findViewById(R.id.tvDot)
        tvScanStatus = findViewById(R.id.tvScanStatus)
        tvEmpty = findViewById(R.id.tvEmpty)
        btnScan = findViewById(R.id.btnScan)
        btnCommand = findViewById(R.id.btnCommand)
        btnDisconnect = findViewById(R.id.btnDisconnect)
        cardNotice = findViewById(R.id.cardNotice)
        tvNoticeTitle = findViewById(R.id.tvNoticeTitle)
        tvNoticeBody = findViewById(R.id.tvNoticeBody)
        btnSettings = findViewById(R.id.btnSettings)
        cardLast = findViewById(R.id.cardLast)
        tvLastName = findViewById(R.id.tvLastName)
        tvLastMac = findViewById(R.id.tvLastMac)
        btnLastConnect = findViewById(R.id.btnLastConnect)

        deviceAdapter = DeviceAdapter { item -> connectTo(item.address, item.name) }
        findViewById<RecyclerView>(R.id.recyclerDevices).apply {
            layoutManager = LinearLayoutManager(this@ScanActivity)
            adapter = deviceAdapter
        }

        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        bleAdapter = manager?.adapter
        if (!packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE) || bleAdapter == null) {
            unsupported = true
        }

        btnScan.setOnClickListener {
            if (scanning) stopScan() else startScan()
        }
        btnCommand.setOnClickListener { openCommand() }
        btnDisconnect.setOnClickListener { BleManager.disconnect() }
        btnSettings.setOnClickListener { openAppSettings() }
        btnLastConnect.setOnClickListener { connectToLast() }
        cardLast.setOnClickListener { connectToLast() }

        if (unsupported) {
            showUnsupported()
        } else if (!hasPermissions()) {
            showPermissionNotice()
        } else {
            updateUi()
        }
        refreshLastCard()
    }

    override fun onStart() {
        super.onStart()
        BleManager.addListener(this)
    }

    override fun onResume() {
        super.onResume()
        refreshLastCard()
        if (!unsupported && hasPermissions()) {
            if (cardNotice.visibility == View.VISIBLE) cardNotice.visibility = View.GONE
        }
        updateUi()
    }

    override fun onStop() {
        BleManager.removeListener(this)
        stopScan()
        super.onStop()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun requiredPermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    private fun hasPermissions(): Boolean = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private fun ensureReadyForScan(): Boolean {
        if (unsupported) {
            showUnsupported()
            return false
        }
        if (!hasPermissions()) {
            permissionLauncher.launch(requiredPermissions())
            return false
        }
        val adapter = bleAdapter ?: run {
            showUnsupported()
            return false
        }
        val enabled = try {
            adapter.isEnabled
        } catch (e: SecurityException) {
            false
        }
        if (!enabled) {
            enableBluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return false
        }
        return true
    }

    private fun startScan() {
        if (!ensureReadyForScan()) return
        if (scanning) stopScan()

        found.clear()
        deviceAdapter.submit(emptyList<DeviceAdapter.DeviceUi>())
        tvEmpty.visibility = View.GONE

        val newScanner = try {
            bleAdapter?.bluetoothLeScanner
        } catch (e: SecurityException) {
            null
        }
        if (newScanner == null) {
            updateScanStatus("无法获取 BLE 扫描器，请检查蓝牙状态", R.color.err)
            toast("无法启动 BLE 扫描")
            return
        }
        scanner = newScanner
        scanning = true
        filteredPhase = true
        btnScan.setText(R.string.stop_scan)
        updateScanStatus("正在扫描 OLED 设备…", R.color.accent)

        startLeScan(true)
        handler.postDelayed(fallbackRunnable, 4500)
        handler.postDelayed(stopRunnable, 16000)
    }

    private fun startLeScan(filtered: Boolean) {
        val sc = scanner ?: return
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            if (filtered) {
                // ScanFilter has no "prefix" match, so exact advertised name is used.
                // If the phone drops the name from results, the unfiltered fallback
                // below still finds the device.
                val filter = ScanFilter.Builder()
                    .setDeviceName("OLED-Panel")
                    .build()
                sc.startScan(listOf(filter), settings, scanCallback)
            } else {
                sc.startScan(null, settings, scanCallback)
            }
        } catch (e: SecurityException) {
            scanning = false
            updateScanStatus("扫描权限被拒绝", R.color.err)
        } catch (e: Exception) {
            scanning = false
            updateScanStatus("扫描启动失败：" + (e.message ?: "未知错误"), R.color.err)
        }
    }

    private fun switchToUnfiltered() {
        if (!scanning || !filteredPhase) return
        try {
            scanner?.stopScan(scanCallback)
        } catch (e: Exception) {
            // ignore
        }
        filteredPhase = false
        startLeScan(false)
        updateScanStatus("兼容扫描：显示全部设备，未命名设备可按 MAC 识别", R.color.warn)
    }

    private fun stopScan() {
        handler.removeCallbacks(fallbackRunnable)
        handler.removeCallbacks(stopRunnable)
        if (scanning) {
            try {
                scanner?.stopScan(scanCallback)
            } catch (e: Exception) {
                // ignore
            }
        }
        scanning = false
        scanner = null
        btnScan.setText(if (found.isEmpty()) R.string.scan else R.string.rescan)
        tvEmpty.visibility = if (found.isEmpty()) View.VISIBLE else View.GONE
        if (found.isEmpty()) {
            tvEmpty.setText(R.string.scan_empty)
            updateScanStatus("扫描结束，未发现设备", R.color.dim)
        } else {
            updateScanStatus("扫描结束，发现 " + found.size + " 台设备", R.color.fg)
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handleScanResult(result)
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            for (result in results) handleScanResult(result)
        }

        override fun onScanFailed(errorCode: Int) {
            updateScanStatus("扫描失败（错误码 " + errorCode + "）", R.color.err)
            stopScan()
        }
    }

    private fun handleScanResult(result: ScanResult) {
        val name = resolveName(result)
        if (filteredPhase && !name.startsWith("OLED")) return

        val address = try {
            result.device.address
        } catch (e: Exception) {
            return
        }
        val previous = found[address]
        if (previous == null ||
            previous.rssi != result.rssi ||
            resolveName(previous) != name
        ) {
            found[address] = result
            refreshDeviceList()
        }
    }

    private fun resolveName(result: ScanResult): String {
        val advertised = result.scanRecord?.deviceName?.trim().orEmpty()
        if (advertised.isNotEmpty()) return advertised
        return try {
            result.device.name?.trim().orEmpty()
        } catch (e: SecurityException) {
            ""
        } catch (e: Exception) {
            ""
        }
    }

    private fun refreshDeviceList() {
        val list = found.values.map { result ->
            DeviceAdapter.DeviceUi(
                name = resolveName(result),
                address = result.device.address,
                rssi = result.rssi
            )
        }.sortedWith(
            compareByDescending<DeviceAdapter.DeviceUi> {
                BleManager.state == BleManager.State.CONNECTED && it.address == BleManager.deviceAddress
            }.thenByDescending { it.name.startsWith("OLED") }
                .thenByDescending { it.rssi }
        )
        deviceAdapter.submit(list)
        tvEmpty.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE
        if (list.isEmpty()) {
            tvEmpty.text = if (scanning) "正在扫描…" else getString(R.string.scan_empty)
        }
    }

    private fun connectToLast() {
        val mac = AppPrefs.lastMac(this)
        if (mac.isBlank()) {
            toast("还没有连接记录，请先扫描")
            return
        }
        connectTo(mac, AppPrefs.lastName(this))
    }

    private fun connectTo(address: String, name: String) {
        if (unsupported) {
            showUnsupported()
            return
        }
        if (!ensureReadyForScan()) return
        if (BleManager.state == BleManager.State.CONNECTED && BleManager.deviceAddress == address) {
            openCommand()
            return
        }
        stopScan()
        openedCommand = false
        AppPrefs.setLastDevice(this, address, name)
        deviceAdapter.updateConnectionState(null, address)
        updateScanStatus("正在连接 " + (name.ifBlank { address }) + "…", R.color.warn)
        BleManager.connect(this, address, name)
        refreshLastCard()
    }

    private fun openCommand() {
        startActivity(Intent(this, CommandActivity::class.java))
    }

    private fun refreshLastCard() {
        val mac = AppPrefs.lastMac(this)
        if (mac.isBlank()) {
            cardLast.visibility = View.GONE
            return
        }
        cardLast.visibility = View.VISIBLE
        tvLastName.text = AppPrefs.lastName(this).ifBlank { "OLED 设备" }
        tvLastMac.text = mac
        val connectedHere = BleManager.state == BleManager.State.CONNECTED && BleManager.deviceAddress == mac
        btnLastConnect.setText(if (connectedHere) R.string.open_console else R.string.connect)
    }

    private fun updateUi() {
        if (unsupported) return
        if (!hasPermissions() && BleManager.state == BleManager.State.DISCONNECTED) return
        deviceAdapter.updateConnectionState(
            if (BleManager.state == BleManager.State.CONNECTED) BleManager.deviceAddress else null,
            if (BleManager.state == BleManager.State.CONNECTING) BleManager.deviceAddress else null
        )
        btnCommand.visibility = if (BleManager.state == BleManager.State.CONNECTED) View.VISIBLE else View.GONE
        btnDisconnect.visibility = if (BleManager.state == BleManager.State.DISCONNECTED) View.GONE else View.VISIBLE

        when (BleManager.state) {
            BleManager.State.CONNECTED -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.ok))
                tvScanStatus.setTextColor(ContextCompat.getColor(this, R.color.ok))
                tvScanStatus.text = "已连接 " + (BleManager.deviceName ?: "OLED-Panel")
            }
            BleManager.State.CONNECTING -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.warn))
                tvScanStatus.setTextColor(ContextCompat.getColor(this, R.color.warn))
                tvScanStatus.text = "正在连接 " + (BleManager.deviceName ?: "设备") + "…"
            }
            BleManager.State.DISCONNECTED -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.dim))
                tvScanStatus.setTextColor(ContextCompat.getColor(this, R.color.fg))
                if (!scanning) {
                    btnScan.setText(if (found.isEmpty()) R.string.scan else R.string.rescan)
                }
                if (AppPrefs.lastMac(this).isNotBlank()) {
                    tvScanStatus.text = "未连接，可点击上次设备一键重连"
                } else {
                    tvScanStatus.setText(R.string.status_idle)
                }
            }
        }
        refreshLastCard()
    }

    private fun updateScanStatus(text: String, colorRes: Int) {
        tvScanStatus.text = text
        tvScanStatus.setTextColor(ContextCompat.getColor(this, colorRes))
    }

    private fun showPermissionNotice() {
        if (unsupported) return
        cardNotice.visibility = View.VISIBLE
        tvNoticeTitle.setText(R.string.permission_title)
        tvNoticeBody.text = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            "需要「附近的设备」权限才能发现并连接 OLED 时钟。若已拒绝，请到系统设置中手动授予。"
        } else {
            "Android 11 及以下需要位置权限才能获取 BLE 扫描结果。若已拒绝，请到系统设置中手动授予。"
        }
        btnSettings.visibility = View.VISIBLE
        updateScanStatus("等待蓝牙权限", R.color.warn)
    }

    private fun showUnsupported() {
        cardNotice.visibility = View.VISIBLE
        tvNoticeTitle.setText(R.string.ble_unsupported_title)
        tvNoticeBody.setText(R.string.ble_unsupported_body)
        btnSettings.visibility = View.GONE
        btnScan.isEnabled = false
        btnCommand.visibility = View.GONE
        tvDot.setTextColor(ContextCompat.getColor(this, R.color.err))
        updateScanStatus("此设备不支持 BLE", R.color.err)
    }

    private fun openAppSettings() {
        val intent = Intent(
            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            Uri.fromParts("package", packageName, null)
        )
        startActivity(intent)
    }

    private fun toast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    override fun onBleState(state: BleManager.State, name: String?, address: String?) {
        updateUi()
        if (state == BleManager.State.CONNECTED) {
            if (!address.isNullOrBlank()) {
                AppPrefs.setLastDevice(this, address, name ?: "")
            }
            refreshLastCard()
            if (!openedCommand && !isFinishing) {
                openedCommand = true
                openCommand()
            }
        } else if (state == BleManager.State.DISCONNECTED || state == BleManager.State.CONNECTING) {
            openedCommand = false
        }
    }

    override fun onBleRssi(rssi: Int) {
        // RSSI is shown on the command screen; the scan list uses scan results.
    }

    override fun onBleError(message: String) {
        toast(message)
    }
}
