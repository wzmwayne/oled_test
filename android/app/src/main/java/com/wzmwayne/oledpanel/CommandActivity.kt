package com.wzmwayne.oledpanel

import android.Manifest
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.util.Base64
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
import androidx.appcompat.app.AlertDialog
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

    /** 选 txt 用系统文件选择器，避免申请全盘存储权限 */
    private val bookLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) uploadBook(uri)
    }

    private var uploadAbort = false

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

        bindRow(R.id.rowGetTime, "gettime", "读取设备 RTC 时间") { doGetTime() }
        bindRow(R.id.rowSetTime, "settime", "选择本地时间并写入设备") { doSetTime() }
        bindRow(R.id.rowSyncNow, "同步为本地时间", "一键写入手机当前时间") { doSyncNow() }
        bindRow(R.id.rowSetNtp, "设置时区", "设备不联网，时区只用于把手机时间换算成本地时间") { doSetTz() }

        bindRow(R.id.rowCourseView, "查看课表", "分页读取完整课表") { showCourses() }
        bindRow(R.id.rowCourseAdd, "添加课程", "名称、星期、时间、单双周") { doAddCourse() }
        bindRow(R.id.rowCourseClear, "清空课表", "删除全部课程", danger = true) { confirmClearCourses() }
        bindRow(R.id.rowBooks, "书籍 / 上传 txt", "查看设备上的书，或从本机选文件上传") { showBooks() }

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
                lastTz = p.optInt("tz", lastTz)
                lastTtCount = p.optInt("tt_count", 0)
                lastWeekParity = p.optInt("week_parity", 0)
                status.text = (res?.msg ?: "").ifBlank { "读取成功" }
                status.setTextColor(ContextCompat.getColor(this, R.color.dim))
                renderKv(
                    container,
                    listOf(
                        "时区" to tzText(lastTz),
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

    private fun doSetTz() {
        val (dialog, view) = showSheet(R.layout.sheet_ntp)
        val tilNtp = view.findViewById<TextInputLayout>(R.id.tilNtp)
        val etNtp = view.findViewById<TextInputEditText>(R.id.etNtp)
        val acTz = view.findViewById<MaterialAutoCompleteTextView>(R.id.acTz)
        val tilCustom = view.findViewById<TextInputLayout>(R.id.tilCustomTz)
        val etCustom = view.findViewById<TextInputEditText>(R.id.etCustomTz)

        tilNtp.visibility = View.GONE

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
            sendCommand("settz", JSONObject().put("tz", tz)) { res, _ ->
                toastResult(res)
                if (res?.ok == true) {
                    lastTz = tz
                    dialog.dismiss()
                }
            }
        }
        view.findViewById<MaterialButton>(R.id.btnCancelNtp).setOnClickListener { dialog.dismiss() }
    }

    /** 课表改为独立界面，这里只负责跳转 */
    private fun showCourses() {
        startActivity(Intent(this, TimetableActivity::class.java))
    }

    /** 旧的内嵌课表面板，保留备用（当前不走这条路） */
    private fun showCoursesSheet() {
        val (dialog, view) = showSheet(R.layout.sheet_course_list)
        val status = view.findViewById<TextView>(R.id.tvSheetStatus)
        val recycler = view.findViewById<RecyclerView>(R.id.rvCourses)
        recycler.layoutManager = LinearLayoutManager(this)

        /* 本地草稿：增删改都只动这份列表，点「写入设备」才整体覆盖推送 */
        val draft = ArrayList<Course>()

        lateinit var adapter: CourseAdapter
        adapter = CourseAdapter(
            onClick = { c ->
                val idx = draft.indexOfFirst { it === c }
                if (idx >= 0) doAddCourse(draft[idx]) { updated ->
                    draft[idx] = updated.copy(index = idx)
                    adapter.submit(draft.toList())
                    status.text = "已修改（未写入）· 共 " + draft.size + " 门"
                }
            },
            onLongClick = { c ->
                val idx = draft.indexOfFirst { it === c }
                if (idx >= 0) {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("移除课程")
                        .setMessage("从草稿里移除「" + c.name + "」？")
                        .setNegativeButton("取消", null)
                        .setPositiveButton("移除") { _, _ ->
                            draft.removeAt(idx)
                            adapter.submit(draft.toList())
                            status.text = "已移除（未写入）· 共 " + draft.size + " 门"
                        }
                        .show()
                }
            }
        )
        recycler.adapter = adapter

        view.findViewById<MaterialButton>(R.id.btnCloseCourses).setOnClickListener { dialog.dismiss() }

        view.findViewById<MaterialButton>(R.id.btnAddCourse).setOnClickListener {
            doAddCourse(null) { c ->
                draft.add(c.copy(index = draft.size))
                adapter.submit(draft.toList())
                status.text = "已添加（未写入）· 共 " + draft.size + " 门"
            }
        }

        view.findViewById<MaterialButton>(R.id.btnFlipParity).setOnClickListener {
            var n = 0
            for (i in draft.indices) {
                val p = draft[i].parity
                if (p == 1 || p == 2) {
                    draft[i] = draft[i].copy(parity = if (p == 1) 2 else 1)
                    n++
                }
            }
            adapter.submit(draft.toList())
            status.text = if (n == 0) "草稿里没有单双周课程" else "已互换 " + n + " 门（未写入）"
        }

        view.findViewById<MaterialButton>(R.id.btnReloadCourses).setOnClickListener {
            loadCoursesInto(draft, adapter, status)
        }

        view.findViewById<MaterialButton>(R.id.btnSaveCourses).setOnClickListener {
            pushDraft(draft, adapter, status)
        }

        loadCoursesInto(draft, adapter, status)
    }

    /** 从设备读整张表填进草稿（不写回设备） */
    private fun loadCoursesInto(draft: MutableList<Course>, adapter: CourseAdapter, status: TextView) {
        val pin = requirePin() ?: run { status.text = "请先填写 6 位 PIN"; return }
        status.text = "正在读取…"
        fetchCourses(pin, 0, ArrayList(), 0) { list, total, error ->
            if (error != null) {
                status.text = "读取失败：" + error
                return@fetchCourses
            }
            draft.clear()
            draft.addAll(list)
            adapter.submit(draft.toList())
            status.text = if (list.isEmpty()) "设备上没有课程，点「添加课程」开始"
                          else "共 " + total + " 门 · 点按编辑，长按移除"
        }
    }

    /** 整体覆盖推送：先清空，再逐条写入，弹窗显示进度 */
    private fun pushDraft(draft: List<Course>, adapter: CourseAdapter, status: TextView) {
        val pin = requirePin() ?: return

        if (draft.isEmpty()) {
            MaterialAlertDialogBuilder(this)
                .setTitle("清空课表")
                .setMessage("草稿是空的，继续会把设备上的课表清空。")
                .setNegativeButton("取消", null)
                .setPositiveButton("清空") { _, _ ->
                    sendCommand("cleartt") { res, _ ->
                        toastResult(res)
                        if (res?.ok == true) {
                            status.text = "设备课表已清空"
                            loadCoursesInto(ArrayList(), adapter, status)
                        }
                    }
                }
                .show()
            return
        }

        val dlg = MaterialAlertDialogBuilder(this)
            .setTitle("写入设备")
            .setMessage("0 / " + draft.size)
            .setCancelable(false)
            .create()
        dlg.show()

        BleManager.send(pin, "cleartt") { res, _ ->
            if (res?.ok != true) {
                if (dlg.isShowing) dlg.dismiss()
                toast("清空失败：" + (res?.msg ?: "设备无响应"))
                return@send
            }
            pushCourseAt(pin, draft, 0, dlg, adapter, status)
        }
    }

    private fun pushCourseAt(
        pin: String,
        list: List<Course>,
        i: Int,
        dlg: AlertDialog,
        adapter: CourseAdapter,
        status: TextView
    ) {
        if (isFinishing || !dlg.isShowing) return
        if (i >= list.size) {
            dlg.dismiss()
            toast("已写入 " + list.size + " 门课")
            status.text = "已写入 " + list.size + " 门"
            return
        }
        val c = list[i]
        val params = JSONObject()
            .put("name", c.name)
            .put("day", c.day)
            .put("start", c.start)
            .put("end", c.end)
            .put("parity", c.parity)
        BleManager.send(pin, "addcourse", params) { res, _ ->
            if (res?.ok != true) {
                if (dlg.isShowing) dlg.dismiss()
                toast("第 " + (i + 1) + " 门写入失败：" + (res?.msg ?: "设备无响应"))
                return@send
            }
            dlg.setMessage("" + (i + 1) + " / " + list.size)
            handler.post { pushCourseAt(pin, list, i + 1, dlg, adapter, status) }
        }
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

    /**
     * @param edit  非空表示编辑已有课程（仅用于本地草稿模式）
     * @param onDraft 非空时只回填草稿，不直接写设备 —— 供「编辑后整体推送」使用
     */
    private fun doAddCourse(edit: Course? = null, onDraft: ((Course) -> Unit)? = null) {
        val (dialog, view) = showSheet(R.layout.sheet_add_course)
        val tilName = view.findViewById<TextInputLayout>(R.id.tilCourseName)
        val etName = view.findViewById<TextInputEditText>(R.id.etCourseName)
        val tilEnd = view.findViewById<TextInputLayout>(R.id.tilEnd)
        val etStart = view.findViewById<TextInputEditText>(R.id.etStart)
        val etEnd = view.findViewById<TextInputEditText>(R.id.etEnd)
        val acDay = view.findViewById<MaterialAutoCompleteTextView>(R.id.acDay)
        val acParity = view.findViewById<MaterialAutoCompleteTextView>(R.id.acParity)

        var selectedDay = edit?.day ?: 1
        var selectedParity = edit?.parity ?: 0
        var startMin = edit?.start ?: (8 * 60)
        var endMin = edit?.end ?: (9 * 60 + 40)
        if (edit != null) etName.setText(edit.name)

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

        if (edit != null) {
            acDay.setText(dayLabels[selectedDay - 1], false)
            acParity.setText(parityLabels[selectedParity], false)
            etStart.setText(formatMinutes(startMin))
            etEnd.setText(formatMinutes(endMin))
        }

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

            if (onDraft != null) {
                onDraft(Course(edit?.index ?: -1, name, selectedDay, startMin, endMin, selectedParity))
                dialog.dismiss()
                return@setOnClickListener
            }

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

    // ---------------- 阅读器 ----------------
    /** 每包原文 256 字节，base64 后约 342 字符，能塞进一次 BLE 写 */
    private val uploadChunkBytes = 256

    private fun showBooks() {
        val (dialog, view) = showSheet(R.layout.sheet_book_list)
        val status = view.findViewById<TextView>(R.id.tvBookStatus)
        val recycler = view.findViewById<RecyclerView>(R.id.rvBooks)
        recycler.layoutManager = LinearLayoutManager(this)

        lateinit var adapter: BookAdapter
        adapter = BookAdapter { book ->
            confirmDeleteBook(book, status) { loadBooks(adapter, status) }
        }
        recycler.adapter = adapter

        view.findViewById<MaterialButton>(R.id.btnBookClose).setOnClickListener { dialog.dismiss() }
        view.findViewById<MaterialButton>(R.id.btnBookUpload).setOnClickListener {
            bookLauncher.launch(arrayOf("text/plain", "application/octet-stream"))
        }
        loadBooks(adapter, status)
    }

    private fun loadBooks(adapter: BookAdapter, status: TextView) {
        val pin = requirePin() ?: run { status.text = "请先填写 6 位 PIN"; return }
        status.text = "正在读取…"
        BleManager.send(pin, "blist") { res, _ ->
            if (res?.ok != true) {
                status.text = "读取失败：" + (res?.msg ?: "设备无响应")
                return@send
            }
            val arr = res.params?.optJSONArray("books")
            val list = ArrayList<Book>()
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    list.add(Book(o.optString("name", ""), o.optLong("size", 0)))
                }
            }
            adapter.submit(list)
            status.text = if (list.isEmpty()) "设备上还没有书" else "共 " + list.size + " 本"
        }
    }

    private fun confirmDeleteBook(book: Book, status: TextView, refresh: () -> Unit) {
        MaterialAlertDialogBuilder(this)
            .setTitle("删除书籍")
            .setMessage("确定删除「" + book.name + "」？")
            .setNegativeButton("取消", null)
            .setPositiveButton("删除") { _, _ ->
                val pin = requirePin() ?: return@setPositiveButton
                BleManager.send(pin, "bdel", JSONObject().put("name", book.name)) { res, _ ->
                    toastResult(res)
                    if (res?.ok == true) refresh()
                }
            }
            .show()
    }

    private fun uploadBook(uri: Uri) {
        val pin = requirePin() ?: return
        val bytes = try {
            contentResolver.openInputStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            null
        }
        if (bytes == null || bytes.isEmpty()) {
            toast("读取文件失败或文件为空")
            return
        }

        /* ezbk / txt 一律转成 .ezzbk（去图片与封面 + LZ4 分块压缩）再传 */
        val raw = if (Ezzbk.isEzbk(bytes)) Ezzbk.toRaw(bytes) else bytes
        if (raw.isEmpty()) {
            toast("正文为空，可能整本都是图片")
            return
        }
        val packed = Ezzbk.wrap(raw)
        val baseName = queryDisplayName(uri).substringBeforeLast('.').ifBlank { "book" }
        val name = sanitizeBookName(baseName + ".ezzbk")

        /* 先问设备还剩多少空间，避免传到一半 littlefs 写失败 */
        BleManager.send(pin, "fsstat") { res, _ ->
            val free = res?.params?.optLong("free", -1L) ?: -1L
            val need = packed.size.toLong() + 4096L
            if (free >= 0 && free < need) {
                toast("设备空间不足：剩余 " + free / 1024 + " KB，需要 " + need / 1024 + " KB")
            } else {
                toast("压缩后 " + packed.size / 1024 + " KB（原文 " + raw.size / 1024 + " KB）")
                startUpload(pin, name, packed, free)
            }
        }
    }

    /** 走二进制数据通道：段内无应答连写，每段回包校验，比 base64 分块快一个量级 */
    private fun startUpload(pin: String, name: String, bytes: ByteArray, free: Long) {
        uploadAbort = false
        val dlg = MaterialAlertDialogBuilder(this)
            .setTitle("上传 " + name)
            .setMessage("准备中…  " + bytes.size / 1024 + " KB")
            .setNegativeButton("取消") { _, _ -> uploadAbort = true }
            .setCancelable(false)
            .create()
        dlg.show()

        val t0 = System.currentTimeMillis()
        BulkUpload.start(pin, name, bytes, object : BulkUpload.Callback {
            override fun onProgress(sent: Int, total: Int, kbps: Int) {
                if (isFinishing || uploadAbort) return
                val pct = if (total > 0) sent * 100 / total else 0
                dlg.setMessage(pct.toString() + "%  ·  " + sent / 1024 + " / " + total / 1024 +
                    " KB  ·  " + kbps + " KB/s")
            }

            override fun onDone(ok: Boolean, message: String, failAt: Int) {
                if (dlg.isShowing) dlg.dismiss()
                if (ok) {
                    val sec = ((System.currentTimeMillis() - t0) / 1000.0).coerceAtLeast(0.1)
                    toast("已上传 " + name + "（校验通过，" +
                        (bytes.size / 1024 / sec).toInt() + " KB/s）")
                    showBooks()
                } else {
                    toast("上传失败：" + message + (if (failAt > 0) "（第 " + failAt + " 字节处）" else ""))
                }
            }
        })
    }

    private fun queryDisplayName(uri: Uri): String {
        var name = "book.txt"
        try {
            contentResolver.query(uri, null, null, null, null)?.use { c ->
                val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (i >= 0 && c.moveToFirst()) name = c.getString(i) ?: name
            }
        } catch (e: Exception) {
            // 拿不到名字就用默认值
        }
        return name
    }

    /** 设备端限制：文件名 ≤32 字节，且不能含 / \ : */
    private fun sanitizeBookName(raw: String): String {
        var s = raw.replace('/', '_').replace('\\', '_').replace(':', '_').trim()
        if (s.isEmpty()) s = "book.txt"
        val b = s.toByteArray(Charsets.UTF_8)
        if (b.size > 32) {
            var end = 32
            while (end > 0 && (b[end].toInt() and 0xC0) == 0x80) end--
            s = String(b, 0, end, Charsets.UTF_8)
        }
        return s
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
