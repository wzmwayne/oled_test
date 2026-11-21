package com.wzmwayne.oledpanel

import android.Manifest
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.IdRes
import androidx.annotation.LayoutRes
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import java.util.TimeZone

class CommandActivity : AppCompatActivity(), BleManager.Listener {

    private lateinit var tilPin: TextInputLayout
    private lateinit var etPin: TextInputEditText
    private lateinit var tvDeviceName: TextView
    private lateinit var tvConnStatus: TextView
    private lateinit var tvRssi: TextView
    private lateinit var tvDot: TextView

    private val handler = Handler(Looper.getMainLooper())
    private var selfCheckSent = false
    private var lastNtp = "ntp.aliyun.com"
    private var lastTz = 8
    private var lastTtCount = 0
    private var lastWeekParity = 0

    private var pendingLocationAction: (() -> Unit)? = null

    private val locationLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        val action = pendingLocationAction
        pendingLocationAction = null
        if (!granted) {
            toast("未授予位置权限，将只能手动输入 WiFi 名称")
        }
        action?.invoke()
    }

    private val rssiPoller = object : Runnable {
        override fun run() {
            BleManager.refreshRssi()
            handler.postDelayed(this, 5000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_command)

        tilPin = findViewById(R.id.tilPin)
        etPin = findViewById(R.id.etPin)
        tvDeviceName = findViewById(R.id.tvDeviceName)
        tvConnStatus = findViewById(R.id.tvConnStatus)
        tvRssi = findViewById(R.id.tvRssi)
        tvDot = findViewById(R.id.tvDot)

        findViewById<MaterialButton>(R.id.btnLogs).setOnClickListener {
            startActivity(Intent(this, LogActivity::class.java))
        }
        findViewById<MaterialButton>(R.id.btnDisconnectTop).setOnClickListener {
            confirmDisconnect()
        }

        setupRows()
        restorePin()
        updateConnectionUi()
    }

    override fun onStart() {
        super.onStart()
        BleManager.addListener(this)
        handler.postDelayed(rssiPoller, 3000)
    }

    override fun onStop() {
        handler.removeCallbacks(rssiPoller)
        BleManager.removeListener(this)
        super.onStop()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun restorePin() {
        val saved = AppPrefs.pin(this)
        etPin.setText(saved)
        etPin.setSelection(etPin.text?.length ?: 0)
        etPin.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                tilPin.error = null
                AppPrefs.setPin(this@CommandActivity, s?.toString().orEmpty())
            }
        })
    }

    private fun setupRows() {
        bindRow(R.id.rowPing, "ping", "连通性自检，设备返回 pong") { doPing() }
        bindRow(R.id.rowGet, "get", "读取 NTP、时区、WiFi 与课表概况") { doGet() }

        bindRow(R.id.rowListWifi, "listwifi", "查看设备保存的网络，可点击删除") { showWifiList(false) }
        bindRow(R.id.rowSetWifi, "setwifi", "输入 SSID 与密码，或从系统 WiFi 选择") { doSetWifi() }
        bindRow(R.id.rowDelWifi, "delwifi", "从已保存列表中选择并删除") { showWifiList(true) }

        bindRow(R.id.rowGetTime, "gettime", "读取设备 RTC 时间") { doGetTime() }
        bindRow(R.id.rowSetTime, "settime", "选择本地时间并写入设备") { doSetTime() }
        bindRow(R.id.rowSyncNow, "同步为本地时间", "一键写入手机当前时间") { doSyncNow() }
        bindRow(R.id.rowSetNtp, "setntp", "设置 NTP 服务器与时区") { doSetNtp() }

        bindRow(R.id.rowCourseView, "查看课表", "分页读取完整课表") { showCourses() }
        bindRow(R.id.rowCourseAdd, "添加课程", "名称、星期、时间、单双周") { doAddCourse() }
        bindRow(R.id.rowCourseClear, "清空课表", "删除全部课程", danger = true) { confirmClearCourses() }

        bindRow(R.id.rowUnpair, "unpair", "清除配对并重新生成 PIN", danger = true) { confirmUnpair() }
    }

    private fun bindRow(
        @IdRes id: Int,
        title: String,
        subtitle: String,
        danger: Boolean = false,
        action: () -> Unit
    ) {
        val root = findViewById<View>(id)
        val titleView = root.findViewById<TextView>(R.id.tvActionTitle)
        titleView.text = title
        root.findViewById<TextView>(R.id.tvActionSubtitle).text = subtitle
        if (danger) {
            titleView.setTextColor(ContextCompat.getColor(this, R.color.err))
        }
        root.setOnClickListener { action() }
    }

    private fun requirePin(): String? {
        val pin = etPin.text?.toString()?.trim().orEmpty()
        if (pin.length != 6 || !pin.all { it.isDigit() }) {
            tilPin.error = "请输入设备屏幕上的 6 位 PIN"
            return null
        }
        tilPin.error = null
        return pin
    }

    private fun sendCommand(
        action: String,
        params: JSONObject? = null,
        callback: ((Protocol.Res?, String) -> Unit)? = null
    ) {
        val pin = requirePin() ?: return
        if (BleManager.state != BleManager.State.CONNECTED) {
            toast("设备未连接")
            return
        }
        BleManager.send(pin, action, params) { res, raw ->
            if (res != null && !res.ok && res.msg.contains("PIN")) {
                tilPin.error = "PIN 错误，请核对设备屏幕"
            }
            callback?.invoke(res, raw)
        }
    }

    private fun toastResult(res: Protocol.Res?) {
        if (res == null) {
            toast("设备无响应")
            return
        }
        toast(if (res.ok) res.msg.ifBlank { "完成" } else "失败：" + res.msg.ifBlank { "未知错误" })
    }

    private fun doPing() {
        sendCommand("ping") { res, _ ->
            toastResult(res)
            if (res?.ok == true) {
                tvConnStatus.text = "已连接 · 自检通过"
            }
        }
    }

    private fun doGet() {
        val (dialog, view) = showSheet(R.layout.sheet_result)
        val status = view.findViewById<TextView>(R.id.tvSheetStatus)
        val container = view.findViewById<LinearLayout>(R.id.kvContainer)
        view.findViewById<TextView>(R.id.tvSheetTitle).text = "设备信息"
        view.findViewById<View>(R.id.btnSheetCopy).visibility = View.GONE
        view.findViewById<MaterialButton>(R.id.btnSheetClose).setOnClickListener { dialog.dismiss() }
        status.text = "读取中…"

        sendCommand("get") { res, _ ->
            if (res?.ok == true) {
                val p = res?.params ?: JSONObject()
                lastNtp = p.optString("ntp", lastNtp)
                lastTz = p.optInt("tz", lastTz)
                lastTtCount = p.optInt("tt_count", 0)
                lastWeekParity = p.optInt("week_parity", 0)
                status.text = (res?.msg ?: "").ifBlank { "读取成功" }
                status.setTextColor(ContextCompat.getColor(this, R.color.dim))
                renderKv(
                    container,
                    listOf(
                        "NTP 服务器" to lastNtp,
                        "时区" to tzText(lastTz),
                        "WiFi 数量" to p.optInt("wifi_count", 0).toString() + " 个",
                        "课表数量" to lastTtCount.toString() + " 条",
                        "本周" to weekParityText(lastWeekParity)
                    )
                )
            } else {
                status.text = "失败：" + (res?.msg ?: "设备无响应")
                status.setTextColor(ContextCompat.getColor(this, R.color.err))
            }
        }
    }

    private fun showWifiList(forDelete: Boolean) {
        val (dialog, view) = showSheet(R.layout.sheet_wifi_list)
        val status = view.findViewById<TextView>(R.id.tvSheetStatus)
        val recycler = view.findViewById<RecyclerView>(R.id.rvWifi)
        view.findViewById<TextView>(R.id.tvSheetTitle).text =
            if (forDelete) "删除 WiFi" else "已保存的 WiFi"
        view.findViewById<MaterialButton>(R.id.btnWifiClose).setOnClickListener { dialog.dismiss() }

        recycler.layoutManager = LinearLayoutManager(this)
        val adapter = WifiAdapter { item ->
            confirmDeleteWifi(item, status, recycler)
        }
        recycler.adapter = adapter
        loadWifiList(adapter, status)
    }

    private fun loadWifiList(adapter: WifiAdapter, status: TextView) {
        status.text = "读取中…"
        status.setTextColor(ContextCompat.getColor(this, R.color.dim))
        sendCommand("listwifi") { res, _ ->
            if (res?.ok == true) {
                val arr = res?.params?.optJSONArray("list") ?: JSONArray()
                val list = ArrayList<WifiItem>()
                for (i in 0 until arr.length()) {
                    val item = arr.optJSONObject(i) ?: continue
                    list.add(WifiItem(item.optString("ssid", ""), item.optString("pass", "")))
                }
                adapter.submit(list)
                status.text = if (list.isEmpty()) {
                    "设备上还没有保存 WiFi"
                } else {
                    "共 " + list.size + " 个 · 点击条目删除"
                }
            } else {
                status.text = "读取失败：" + (res?.msg ?: "设备无响应")
                status.setTextColor(ContextCompat.getColor(this, R.color.err))
            }
        }
    }

    private fun confirmDeleteWifi(
        item: WifiItem,
        status: TextView,
        recycler: RecyclerView
    ) {
        MaterialAlertDialogBuilder(this)
            .setTitle("删除 WiFi")
            .setMessage("确定从设备删除「" + item.ssid + "」？")
            .setNegativeButton("取消", null)
            .setPositiveButton("删除") { _, _ ->
                sendCommand("delwifi", JSONObject().put("ssid", item.ssid)) { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) {
                        loadWifiList(recycler.adapter as WifiAdapter, status)
                    }
                }
            }
            .show()
    }

    private fun doSetWifi() {
        withLocation {
            val (dialog, view) = showSheet(R.layout.sheet_setwifi)
            val tilSaved = view.findViewById<TextInputLayout>(R.id.tilSaved)
            val acSaved = view.findViewById<MaterialAutoCompleteTextView>(R.id.acSaved)
            val tilSsid = view.findViewById<TextInputLayout>(R.id.tilSsid)
            val etSsid = view.findViewById<TextInputEditText>(R.id.etSsid)
            val etPass = view.findViewById<TextInputEditText>(R.id.etPass)
            val hint = view.findViewById<TextView>(R.id.tvWifiHint)

            val suggestions = WifiSuggestions.load(this)
            if (suggestions.isNotEmpty()) {
                tilSaved.visibility = View.VISIBLE
                acSaved.setAdapter(
                    ArrayAdapter(this, android.R.layout.simple_list_item_1, suggestions)
                )
                acSaved.setOnClickListener { acSaved.showDropDown() }
                acSaved.setOnItemClickListener { _, _, position, _ ->
                    etSsid.setText(suggestions[position])
                }
            } else {
                tilSaved.visibility = View.GONE
                hint.text = "无法读取系统 WiFi 列表，请手动输入 SSID 和密码。"
            }

            view.findViewById<MaterialButton>(R.id.btnSaveWifi).setOnClickListener {
                val ssid = etSsid.text?.toString()?.trim().orEmpty()
                val pass = etPass.text?.toString().orEmpty()
                if (ssid.isEmpty()) {
                    tilSsid.error = "请输入 WiFi 名称"
                    return@setOnClickListener
                }
                tilSsid.error = null
                sendCommand("setwifi", JSONObject().put("ssid", ssid).put("pass", pass)) { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) dialog.dismiss()
                }
            }
            view.findViewById<MaterialButton>(R.id.btnCancelWifi).setOnClickListener { dialog.dismiss() }
        }
    }

    private fun withLocation(action: () -> Unit) {
        if (ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            action()
            return
        }
        pendingLocationAction = action
        locationLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private fun doGetTime() {
        val (dialog, view) = showSheet(R.layout.sheet_result)
        val status = view.findViewById<TextView>(R.id.tvSheetStatus)
        val container = view.findViewById<LinearLayout>(R.id.kvContainer)
        val copyButton = view.findViewById<MaterialButton>(R.id.btnSheetCopy)
        view.findViewById<TextView>(R.id.tvSheetTitle).text = "设备时间"
        view.findViewById<MaterialButton>(R.id.btnSheetClose).setOnClickListener { dialog.dismiss() }
        status.text = "读取中…"
        copyButton.visibility = View.GONE

        sendCommand("gettime") { res, _ ->
            val ts = res?.params?.optLong("ts", 0L) ?: 0L
            if (res?.ok == true && ts > 0) {
                val local = formatTime(ts, false)
                val utc = formatTime(ts, true)
                status.text = "读取成功"
                renderKv(
                    container,
                    listOf(
                        "本地时间" to local,
                        "UTC 时间" to utc,
                        "时间戳（UTC 秒）" to ts.toString()
                    )
                )
                val copyText = "本地时间：" + local +
                    "\nUTC 时间：" + utc +
                    "\n时间戳：" + ts
                copyButton.visibility = View.VISIBLE
                copyButton.setOnClickListener { copyToClipboard("设备时间", copyText) }
            } else {
                status.text = "读取失败：" + (res?.msg ?: "设备无响应")
                status.setTextColor(ContextCompat.getColor(this, R.color.err))
            }
        }
    }

    private fun doSetTime() {
        val now = Calendar.getInstance()
        DatePickerDialog(
            this,
            { _, year, month, day ->
                TimePickerDialog(
                    this,
                    { _, hour, minute ->
                        val local = Calendar.getInstance().apply {
                            set(year, month, day, hour, minute, 0)
                            set(Calendar.MILLISECOND, 0)
                        }
                        val ts = local.timeInMillis / 1000
                        MaterialAlertDialogBuilder(this)
                            .setTitle("写入设备时间")
                            .setMessage(
                                "本地：" + formatTime(ts, false) +
                                    "\nUTC：" + formatTime(ts, true) +
                                    "\n时间戳：" + ts +
                                    "\n\n将按 UTC 秒写入设备 RTC。"
                            )
                            .setNegativeButton("取消", null)
                            .setPositiveButton("写入") { _, _ ->
                                sendCommand("settime", JSONObject().put("ts", ts)) { res, _ ->
                                    toastResult(res)
                                }
                            }
                            .show()
                    },
                    now.get(Calendar.HOUR_OF_DAY),
                    now.get(Calendar.MINUTE),
                    true
                ).show()
            },
            now.get(Calendar.YEAR),
            now.get(Calendar.MONTH),
            now.get(Calendar.DAY_OF_MONTH)
        ).show()
    }

    /** 一键把手机当前时间写入设备 RTC（走 settime，UTC 秒） */
    private fun doSyncNow() {
        val ts = System.currentTimeMillis() / 1000L
        MaterialAlertDialogBuilder(this)
            .setTitle("同步为本地时间")
            .setMessage(
                "将把手机当前时间写入设备：\n" +
                    formatTime(ts, false) + "（本地）\n" +
                    formatTime(ts, true) + "（UTC）"
            )
            .setNegativeButton("取消", null)
            .setPositiveButton("写入") { _, _ ->
                sendCommand("settime", JSONObject().put("ts", ts)) { res, _ ->
                    toastResult(res)
                }
            }
            .show()
    }

    private fun doSetNtp() {
        val (dialog, view) = showSheet(R.layout.sheet_ntp)
        val tilNtp = view.findViewById<TextInputLayout>(R.id.tilNtp)
        val etNtp = view.findViewById<TextInputEditText>(R.id.etNtp)
        val acTz = view.findViewById<MaterialAutoCompleteTextView>(R.id.acTz)
        val tilCustom = view.findViewById<TextInputLayout>(R.id.tilCustomTz)
        val etCustom = view.findViewById<TextInputEditText>(R.id.etCustomTz)

        etNtp.setText(lastNtp)

        val options = tzOptions()
        val labels = options.map { it.first }
        acTz.setAdapter(ArrayAdapter(this, android.R.layout.simple_list_item_1, labels))

        var customMode = false
        var selectedTz = lastTz
        val knownIndex = options.indexOfFirst { it.second == lastTz }
        if (knownIndex >= 0) {
            acTz.setText(labels[knownIndex], false)
        } else {
            customMode = true
            acTz.setText(options.last().first, false)
            tilCustom.visibility = View.VISIBLE
            etCustom.setText(lastTz.toString())
        }

        acTz.setOnClickListener { acTz.showDropDown() }
        acTz.setOnItemClickListener { _, _, position, _ ->
            val value = options[position].second
            if (value == CUSTOM_TZ) {
                customMode = true
                selectedTz = lastTz
                tilCustom.visibility = View.VISIBLE
                etCustom.setText(lastTz.toString())
            } else {
                customMode = false
                selectedTz = value
                tilCustom.visibility = View.GONE
                tilCustom.error = null
            }
        }

        view.findViewById<MaterialButton>(R.id.btnSaveNtp).setOnClickListener {
            val server = etNtp.text?.toString()?.trim().orEmpty()
            if (server.isEmpty()) {
                tilNtp.error = "请输入 NTP 服务器"
                return@setOnClickListener
            }
            tilNtp.error = null

            val tz = if (customMode) {
                etCustom.text?.toString()?.trim()?.toIntOrNull()
            } else {
                selectedTz
            }
            if (tz == null || tz < -12 || tz > 14) {
                tilCustom.error = "时区范围是 -12 到 14"
                return@setOnClickListener
            }
            tilCustom.error = null
            sendCommand("setntp", JSONObject().put("server", server).put("tz", tz)) { res, _ ->
                toastResult(res)
                if (res?.ok == true) {
                    lastNtp = server
                    lastTz = tz
                    dialog.dismiss()
                }
            }
        }
        view.findViewById<MaterialButton>(R.id.btnCancelNtp).setOnClickListener { dialog.dismiss() }
    }

    private fun showCourses() {
        val (dialog, view) = showSheet(R.layout.sheet_course_list)
        val status = view.findViewById<TextView>(R.id.tvSheetStatus)
        val recycler = view.findViewById<RecyclerView>(R.id.rvCourses)
        view.findViewById<MaterialButton>(R.id.btnCloseCourses).setOnClickListener { dialog.dismiss() }
        view.findViewById<MaterialButton>(R.id.btnAddCourse).setOnClickListener {
            dialog.dismiss()
            doAddCourse()
        }
        recycler.layoutManager = LinearLayoutManager(this)
        val adapter = CourseAdapter { course ->
            confirmDeleteCourse(course, dialog, status)
        }
        recycler.adapter = adapter
        view.findViewById<MaterialButton>(R.id.btnFlipParity).setOnClickListener {
            confirmFlipParity(adapter, status)
        }
        loadCourses(adapter, status)
    }

    private fun loadCourses(adapter: CourseAdapter, status: TextView) {
        val pin = requirePin()
        if (pin == null) {
            status.text = "请先在顶部填写 6 位 PIN"
            status.setTextColor(ContextCompat.getColor(this, R.color.err))
            return
        }
        status.text = "正在读取课表…"
        status.setTextColor(ContextCompat.getColor(this, R.color.dim))
        fetchCourses(pin, 0, ArrayList<Course>(), 0) { list, total, error ->
            if (error != null) {
                status.text = "读取失败：" + error
                status.setTextColor(ContextCompat.getColor(this, R.color.err))
            } else {
                adapter.submit(list)
                if (list.isEmpty()) {
                    status.text = "暂无课程"
                } else {
                    status.text = "共 " + total + " 门课 · 长按可删除"
                }
            }
        }
    }

    /**
     * listtt is paged. The firmware truncates count to 4 per response, so ask
     * for exactly 4 each round until total is reached.
     */
    private fun fetchCourses(
        pin: String,
        from: Int,
        acc: MutableList<Course>,
        guard: Int,
        done: (List<Course>, Int, String?) -> Unit
    ) {
        if (guard > 40) {
            done(acc, acc.size, null)
            return
        }
        val params = JSONObject().put("from", from).put("count", 4)
        BleManager.send(pin, "listtt", params) { res, _ ->
            if (res?.ok != true) {
                if (res != null && res.msg.contains("PIN")) {
                    tilPin.error = "PIN 错误，请核对设备屏幕"
                }
                done(acc, 0, res?.msg ?: "设备无响应")
                return@send
            }
            val p = res?.params ?: JSONObject()
            val total = p.optInt("total", 0)
            val arr = p.optJSONArray("courses") ?: JSONArray()
            var added = 0
            for (i in 0 until arr.length()) {
                val item = arr.optJSONObject(i) ?: continue
                val name = item.optString("name", "")
                val day = item.optInt("day", 1).coerceIn(1, 7)
                val start = item.optInt("start", 0).coerceIn(0, 1439)
                val end = item.optInt("end", 0).coerceIn(0, 1439)
                val parity = item.optInt("parity", 0).coerceIn(0, 2)
                acc.add(Course(acc.size, name, day, start, end, parity))
                added++
            }
            if (added == 0 || acc.size >= total) {
                done(acc, if (total > 0) total else acc.size, null)
            } else {
                fetchCourses(pin, from + added, acc, guard + 1, done)
            }
        }
    }

    private fun confirmDeleteCourse(course: Course, dialog: BottomSheetDialog, status: TextView) {
        MaterialAlertDialogBuilder(this)
            .setTitle("删除课程")
            .setMessage("确定删除「" + course.name + "」（" + course.metaText() + "）？")
            .setNegativeButton("取消", null)
            .setPositiveButton("删除") { _, _ ->
                val pin = requirePin() ?: return@setPositiveButton
                BleManager.send(
                    pin,
                    "delcourse",
                    JSONObject().put("index", course.index)
                ) { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) {
                        val adapter = (dialog.findViewById<RecyclerView>(R.id.rvCourses))?.adapter
                        if (adapter is CourseAdapter) loadCourses(adapter, status)
                    }
                }
            }
            .show()
    }

    private fun doAddCourse() {
        val (dialog, view) = showSheet(R.layout.sheet_add_course)
        val tilName = view.findViewById<TextInputLayout>(R.id.tilCourseName)
        val etName = view.findViewById<TextInputEditText>(R.id.etCourseName)
        val tilEnd = view.findViewById<TextInputLayout>(R.id.tilEnd)
        val etStart = view.findViewById<TextInputEditText>(R.id.etStart)
        val etEnd = view.findViewById<TextInputEditText>(R.id.etEnd)
        val acDay = view.findViewById<MaterialAutoCompleteTextView>(R.id.acDay)
        val acParity = view.findViewById<MaterialAutoCompleteTextView>(R.id.acParity)

        var selectedDay = 1
        var selectedParity = 0
        var startMin = 8 * 60
        var endMin = 9 * 60 + 40

        etStart.setText(formatMinutes(startMin))
        etEnd.setText(formatMinutes(endMin))

        val dayLabels = Course.DAY_NAMES.toList()
        acDay.setAdapter(ArrayAdapter(this, android.R.layout.simple_list_item_1, dayLabels))
        acDay.setText(dayLabels[0], false)
        acDay.setOnClickListener { acDay.showDropDown() }
        acDay.setOnItemClickListener { _, _, position, _ -> selectedDay = position + 1 }

        val parityLabels = listOf("每周", "单周", "双周")
        acParity.setAdapter(
            ArrayAdapter(this, android.R.layout.simple_list_item_1, parityLabels)
        )
        acParity.setText(parityLabels[0], false)
        acParity.setOnClickListener { acParity.showDropDown() }
        acParity.setOnItemClickListener { _, _, position, _ -> selectedParity = position }

        etStart.setOnClickListener {
            pickTime(startMin) { value ->
                startMin = value
                etStart.setText(formatMinutes(value))
            }
        }
        etEnd.setOnClickListener {
            pickTime(endMin) { value ->
                endMin = value
                etEnd.setText(formatMinutes(value))
            }
        }

        view.findViewById<MaterialButton>(R.id.btnSaveCourse).setOnClickListener {
            val name = etName.text?.toString()?.trim().orEmpty()
            if (name.isEmpty()) {
                tilName.error = "请输入课程名"
                return@setOnClickListener
            }
            if (name.toByteArray(Charsets.UTF_8).size > 24) {
                tilName.error = "课程名最多 24 字节（约 8 个汉字）"
                return@setOnClickListener
            }
            tilName.error = null
            if (startMin >= endMin) {
                tilEnd.error = "结束时间必须晚于开始时间"
                return@setOnClickListener
            }
            tilEnd.error = null

            sendCommand(
                "addcourse",
                JSONObject()
                    .put("name", name)
                    .put("day", selectedDay)
                    .put("start", startMin)
                    .put("end", endMin)
                    .put("parity", selectedParity)
            ) { res, _ ->
                toastResult(res)
                if (res?.ok == true) {
                    lastTtCount += 1
                    dialog.dismiss()
                }
            }
        }
        view.findViewById<MaterialButton>(R.id.btnCancelCourse).setOnClickListener { dialog.dismiss() }
    }

    /**
     * 单双周按 ISO 周数判定。若学校口径与 ISO 整体反相，开学第一周点一次即可全部对调，
     * 不用把每条单双周课程删了重录。
     */
    private fun confirmFlipParity(adapter: CourseAdapter, status: TextView) {
        MaterialAlertDialogBuilder(this)
            .setTitle("单双周互换")
            .setMessage(
                "把所有「单周」课程改成「双周」、把「双周」改成「单周」，每周都上的课不受影响。\n\n" +
                    "用途：设备的单双周按 ISO 周数算，如果开学第一周对不上，点一次即可整体校正。"
            )
            .setNegativeButton("取消", null)
            .setPositiveButton("互换") { _, _ ->
                sendCommand("fliptt") { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) loadCourses(adapter, status)
                }
            }
            .show()
    }

    private fun confirmClearCourses() {
        MaterialAlertDialogBuilder(this)
            .setTitle("清空课表")
            .setMessage("将删除设备上的全部课程，且无法恢复。确定继续？")
            .setNegativeButton("取消", null)
            .setPositiveButton("清空") { _, _ ->
                sendCommand("cleartt") { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) lastTtCount = 0
                }
            }
            .show()
    }

    private fun confirmUnpair() {
        MaterialAlertDialogBuilder(this)
            .setTitle("解除配对")
            .setMessage(
                "将清除设备上的配对信息、断开当前连接，并重新生成 6 位 PIN。\n" +
                    "之后需要重新扫描连接，并输入设备屏幕上的新 PIN。"
            )
            .setNegativeButton("取消", null)
            .setPositiveButton("解除") { _, _ ->
                sendCommand("unpair") { res, _ ->
                    if (res?.ok == true) {
                        AppPrefs.setPin(this, "")
                        etPin.setText("")
                        toast("已解除，设备将断开并重新广播，PIN 已清除")
                        handler.postDelayed({ if (!isFinishing) finish() }, 1500)
                    } else {
                        toastResult(res)
                    }
                }
            }
            .show()
    }

    private fun confirmDisconnect() {
        MaterialAlertDialogBuilder(this)
            .setTitle("断开连接")
            .setMessage("确定断开与设备的连接？")
            .setNegativeButton("取消", null)
            .setPositiveButton("断开") { _, _ -> BleManager.disconnect() }
            .show()
    }

    private fun selfCheck() {
        val pin = etPin.text?.toString()?.trim().orEmpty()
        if (pin.length != 6 || !pin.all { it.isDigit() }) return
        BleManager.send(pin, "ping") { res, _ ->
            if (res?.ok == true) {
                tvConnStatus.text = "已连接 · 自检通过"
            } else if (res != null && res.msg.contains("PIN")) {
                tilPin.error = "PIN 错误，请核对设备屏幕"
            }
        }
    }

    private fun updateConnectionUi() {
        tvDeviceName.text = BleManager.deviceName ?: "OLED 设备"
        when (BleManager.state) {
            BleManager.State.CONNECTED -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.ok))
                tvConnStatus.text = "已连接 · MTU " + BleManager.currentMtu
                tvConnStatus.setTextColor(ContextCompat.getColor(this, R.color.dim))
            }
            BleManager.State.CONNECTING -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.warn))
                tvConnStatus.text = "正在连接…"
                tvConnStatus.setTextColor(ContextCompat.getColor(this, R.color.warn))
            }
            BleManager.State.DISCONNECTED -> {
                tvDot.setTextColor(ContextCompat.getColor(this, R.color.err))
                tvConnStatus.text = "连接已断开，请返回扫描页重连"
                tvConnStatus.setTextColor(ContextCompat.getColor(this, R.color.err))
            }
        }
        tvRssi.text = Rssi.label(BleManager.lastRssi)
        tvRssi.setTextColor(Rssi.color(this, BleManager.lastRssi))
    }

    private fun showSheet(@LayoutRes layout: Int): Pair<BottomSheetDialog, View> {
        val dialog = BottomSheetDialog(this)
        val view = layoutInflater.inflate(layout, null)
        dialog.setContentView(view)
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        dialog.show()
        return dialog to view
    }

    private fun renderKv(container: LinearLayout, rows: List<Pair<String, String>>) {
        container.removeAllViews()
        for (row in rows) {
            val item = layoutInflater.inflate(R.layout.item_kv, container, false)
            item.findViewById<TextView>(R.id.tvKvKey).text = row.first
            item.findViewById<TextView>(R.id.tvKvValue).text = row.second
            container.addView(item)
        }
    }

    private fun pickTime(initialMinutes: Int, onPicked: (Int) -> Unit) {
        TimePickerDialog(
            this,
            { _, hour, minute -> onPicked(hour * 60 + minute) },
            initialMinutes / 60,
            initialMinutes % 60,
            true
        ).show()
    }

    private fun formatMinutes(minutes: Int): String =
        String.format(Locale.US, "%02d:%02d", minutes / 60, minutes % 60)

    private fun formatTime(tsSeconds: Long, utc: Boolean): String {
        val format = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA)
        format.timeZone = if (utc) TimeZone.getTimeZone("UTC") else TimeZone.getDefault()
        return format.format(Date(tsSeconds * 1000))
    }

    private fun tzText(tz: Int): String = "UTC" + (if (tz >= 0) "+" else "") + tz

    private fun weekParityText(value: Int): String = when (value) {
        1 -> "单周"
        2 -> "双周"
        else -> "未知"
    }

    private fun tzOptions(): List<Pair<String, Int>> = listOf(
        "UTC-10 檀香山" to -10,
        "UTC-8 洛杉矶" to -8,
        "UTC-7 丹佛" to -7,
        "UTC-6 芝加哥" to -6,
        "UTC-5 纽约" to -5,
        "UTC-4 圣地亚哥" to -4,
        "UTC-3 圣保罗" to -3,
        "UTC+0 伦敦" to 0,
        "UTC+1 柏林" to 1,
        "UTC+2 开罗" to 2,
        "UTC+3 莫斯科" to 3,
        "UTC+4 迪拜" to 4,
        "UTC+5 卡拉奇" to 5,
        "UTC+7 曼谷" to 7,
        "UTC+8 北京" to 8,
        "UTC+9 东京" to 9,
        "UTC+10 悉尼" to 10,
        "UTC+12 奥克兰" to 12,
        "自定义…" to CUSTOM_TZ
    )

    private fun copyToClipboard(label: String, text: String) {
        val manager = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        manager.setPrimaryClip(ClipData.newPlainText(label, text))
        toast("已复制")
    }

    private fun toast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    override fun onBleState(state: BleManager.State, name: String?, address: String?) {
        updateConnectionUi()
        if (state == BleManager.State.CONNECTED) {
            if (!selfCheckSent) {
                selfCheckSent = true
                selfCheck()
            }
        } else if (state == BleManager.State.DISCONNECTED || state == BleManager.State.CONNECTING) {
            selfCheckSent = false
        }
    }

    override fun onBleRssi(rssi: Int) {
        tvRssi.text = Rssi.label(rssi)
        tvRssi.setTextColor(Rssi.color(this, rssi))
    }

    override fun onBleError(message: String) {
        tvConnStatus.text = message
        tvConnStatus.setTextColor(ContextCompat.getColor(this, R.color.err))
        toast(message)
    }

    companion object {
        private const val CUSTOM_TZ = 99
    }
}
