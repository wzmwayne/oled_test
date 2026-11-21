package com.wzmwayne.oledpanel

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder

/**
 * 独立课表界面：网格看整周，点空白新建、点课程编辑，改动先落草稿，
 * 「写入设备」时整张表覆盖推送。
 */
class TimetableActivity : AppCompatActivity() {

    private lateinit var grid: TimetableView
    private lateinit var tvStatus: TextView
    private val draft = ArrayList<Course>()
    private val handler = Handler(Looper.getMainLooper())
    private var pin = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_timetable)

        pin = AppPrefs.pin(this)
        grid = findViewById(R.id.timetable)
        tvStatus = findViewById(R.id.tvTtStatus)

        grid.onCourseTap = { c -> editCourse(c) }
        grid.onEmptyTap = { day, min -> addCourse(day, min) }

        findViewById<MaterialButton>(R.id.btnTtBack).setOnClickListener { finish() }
        findViewById<MaterialButton>(R.id.btnTtReload).setOnClickListener { load() }
        findViewById<MaterialButton>(R.id.btnTtAdd).setOnClickListener { addCourse(1, 8 * 60) }
        findViewById<MaterialButton>(R.id.btnTtSave).setOnClickListener { save() }

        load()
    }

    private fun load() {
        if (pin.length != 6) {
            tvStatus.text = "没有 PIN，请先在命令台连接设备"
            return
        }
        tvStatus.text = "正在读取…"
        CourseSync.load(pin) { list, err ->
            if (list == null) {
                tvStatus.text = "读取失败：" + (err ?: "未知错误")
            } else {
                draft.clear()
                draft.addAll(list)
                grid.courses = draft.toList()
                tvStatus.text = if (list.isEmpty()) "还没有课程，点网格空白处添加"
                                else "共 " + list.size + " 门 · 点块编辑，点空白新建"
            }
        }
    }

    private fun addCourse(day: Int, startMin: Int) {
        val end = (startMin + 45).coerceAtMost(23 * 60)
        val seed = Course(-1, "", day, startMin, end, 0)
        CourseEditDialog.show(this, seed, { c ->
            draft.add(c.copy(index = draft.size))
            grid.courses = draft.toList()
            tvStatus.text = "已添加（未写入）· 共 " + draft.size + " 门"
        })
    }

    private fun editCourse(course: Course) {
        CourseEditDialog.show(this, course, { updated ->
            val idx = draft.indexOfFirst { it === course }
            if (idx >= 0) {
                draft[idx] = updated.copy(index = idx)
                grid.courses = draft.toList()
                tvStatus.text = "已修改（未写入）· 共 " + draft.size + " 门"
            }
        }, {
            val idx = draft.indexOfFirst { it === course }
            if (idx >= 0) {
                draft.removeAt(idx)
                grid.courses = draft.toList()
                tvStatus.text = "已删除（未写入）· 共 " + draft.size + " 门"
            }
        })
    }

    private fun save() {
        if (pin.length != 6) {
            Toast.makeText(this, "没有 PIN", Toast.LENGTH_SHORT).show()
            return
        }
        if (draft.isEmpty()) {
            MaterialAlertDialogBuilder(this)
                .setTitle("清空课表")
                .setMessage("当前没有课程，继续会清空设备上的课表。")
                .setNegativeButton("取消", null)
                .setPositiveButton("清空") { _, _ ->
                    CourseSync.push(pin, emptyList(), { _, _ -> }) { ok, err ->
                        if (ok) {
                            tvStatus.text = "设备课表已清空"
                        } else {
                            Toast.makeText(this, err ?: "失败", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
                .show()
            return
        }

        val dlg: AlertDialog = MaterialAlertDialogBuilder(this)
            .setTitle("写入设备")
            .setMessage("0 / " + draft.size)
            .setCancelable(false)
            .create()
        dlg.show()

        CourseSync.push(pin, draft, { done, total ->
            handler.post { if (dlg.isShowing) dlg.setMessage("$done / $total") }
        }) { ok, err ->
            handler.post {
                if (dlg.isShowing) dlg.dismiss()
                if (ok) {
                    tvStatus.text = "已写入 " + draft.size + " 门"
                    Toast.makeText(this, "已写入 " + draft.size + " 门", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this, err ?: "写入失败", Toast.LENGTH_LONG).show()
                }
            }
        }
    }
}
