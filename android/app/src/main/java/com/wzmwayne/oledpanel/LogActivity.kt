package com.wzmwayne.oledpanel

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder

class LogActivity : AppCompatActivity(), LogStore.Listener {

    private lateinit var adapter: LogAdapter
    private lateinit var recycler: RecyclerView
    private lateinit var tvCount: TextView
    private lateinit var tvEmpty: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log)
        setTitle(R.string.log_title)

        recycler = findViewById(R.id.recyclerLogs)
        tvCount = findViewById(R.id.tvLogCount)
        tvEmpty = findViewById(R.id.tvLogEmpty)

        adapter = LogAdapter()
        recycler.layoutManager = LinearLayoutManager(this)
        recycler.adapter = adapter

        findViewById<MaterialButton>(R.id.btnBack).setOnClickListener { finish() }
        findViewById<MaterialButton>(R.id.btnCopy).setOnClickListener { copyAll() }
        findViewById<MaterialButton>(R.id.btnClear).setOnClickListener { confirmClear() }

        refresh()
    }

    override fun onStart() {
        super.onStart()
        LogStore.addListener(this)
        refresh()
    }

    override fun onStop() {
        LogStore.removeListener(this)
        super.onStop()
    }

    private fun refresh() {
        val list = LogStore.snapshot()
        adapter.submit(list)
        tvCount.text = "共 " + list.size + " 条 · 点击条目切换原始 JSON"
        tvEmpty.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE
        recycler.visibility = if (list.isEmpty()) View.GONE else View.VISIBLE
        if (list.isNotEmpty()) {
            recycler.scrollToPosition(list.size - 1)
        }
    }

    private fun copyAll() {
        val list = LogStore.snapshot()
        if (list.isEmpty()) {
            toast("日志为空")
            return
        }
        val text = list.joinToString("\n") { LogStore.formatLine(it) }
        val manager = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        manager.setPrimaryClip(ClipData.newPlainText("OLED 日志", text))
        toast("已复制 " + list.size + " 条日志")
    }

    private fun confirmClear() {
        MaterialAlertDialogBuilder(this)
            .setTitle("清空日志")
            .setMessage("将删除当前手机上的全部通信记录，不影响设备。")
            .setNegativeButton("取消", null)
            .setPositiveButton("清空") { _, _ -> LogStore.clear() }
            .show()
    }

    private fun toast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    override fun onLogAdded(entry: LogStore.Entry) {
        refresh()
    }

    override fun onLogsCleared() {
        refresh()
    }
}
