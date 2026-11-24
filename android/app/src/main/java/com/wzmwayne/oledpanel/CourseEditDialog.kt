package com.wzmwayne.oledpanel

import android.app.TimePickerDialog
import android.content.Context
import android.view.LayoutInflater
import android.view.View
import android.widget.ArrayAdapter
import android.widget.CheckBox
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

/** 复用 sheet_add_course.xml 的课程编辑弹窗，课表界面与命令台共用一套字段 */
object CourseEditDialog {

    fun show(ctx: Context, initial: Course?, onSave: (Course) -> Unit, onDelete: (() -> Unit)? = null) {
        val view = LayoutInflater.from(ctx).inflate(R.layout.sheet_add_course, null)
        val tilName = view.findViewById<TextInputLayout>(R.id.tilCourseName)
        val etName = view.findViewById<TextInputEditText>(R.id.etCourseName)
        val tilEnd = view.findViewById<TextInputLayout>(R.id.tilEnd)
        val etStart = view.findViewById<TextInputEditText>(R.id.etStart)
        val etEnd = view.findViewById<TextInputEditText>(R.id.etEnd)
        val acDay = view.findViewById<MaterialAutoCompleteTextView>(R.id.acDay)
        val acParity = view.findViewById<MaterialAutoCompleteTextView>(R.id.acParity)
        val btnSave = view.findViewById<MaterialButton>(R.id.btnSaveCourse)
        val btnCancel = view.findViewById<MaterialButton>(R.id.btnCancelCourse)
        val btnDelete = view.findViewById<MaterialButton>(R.id.btnDeleteCourse)

        var day = initial?.day ?: 1
        var parity = initial?.parity ?: 0
        var startMin = initial?.start ?: (8 * 60)
        var endMin = initial?.end ?: (9 * 60 + 40)
        var isEvent = initial?.isEvent ?: false
        var isGlobal = (initial?.global ?: 0) == 1

        val dayLabels = Course.DAY_NAMES.toList()
        acDay.setAdapter(ArrayAdapter(ctx, android.R.layout.simple_list_item_1, dayLabels))
        acDay.setText(dayLabels[day - 1], false)
        acDay.setOnClickListener { acDay.showDropDown() }
        acDay.setOnItemClickListener { _, _, position, _ -> day = position + 1 }

        val parityLabels = listOf("每周", "单周", "双周")
        acParity.setAdapter(ArrayAdapter(ctx, android.R.layout.simple_list_item_1, parityLabels))
        acParity.setText(parityLabels[parity], false)
        acParity.setOnClickListener { acParity.showDropDown() }
        acParity.setOnItemClickListener { _, _, position, _ -> parity = position }

        etName.setText(initial?.name ?: "")
        etStart.setText(formatMin(startMin))
        etEnd.setText(formatMin(endMin))

        etStart.setOnClickListener {
            TimePickerDialog(ctx, { _, h, m ->
                startMin = h * 60 + m
                etStart.setText(formatMin(startMin))
                if (isEvent) {                 /* 事件只有单点，结束时间跟着走 */
                    endMin = startMin
                    etEnd.setText(formatMin(startMin))
                }
            }, startMin / 60, startMin % 60, true).show()
        }
        etEnd.setOnClickListener {
            TimePickerDialog(ctx, { _, h, m ->
                endMin = h * 60 + m
                etEnd.setText(formatMin(endMin))
            }, endMin / 60, endMin % 60, true).show()
        }

        /* 事件与全局开关：动态挂到内容容器末尾，免去改 layout */
        val container = (view as android.view.ViewGroup).getChildAt(0) as android.view.ViewGroup
        val cbEvent = CheckBox(ctx).apply {
            text = "这是事件（单点时间，只用开始时间）"
            isChecked = isEvent
        }
        val cbGlobal = CheckBox(ctx).apply {
            text = "全局事件（每天都算，主页滚动条末尾按时间追加，如「放学」）"
            isChecked = isGlobal
            visibility = if (isEvent) View.VISIBLE else View.GONE
        }
        container.addView(cbEvent)
        container.addView(cbGlobal)

        fun syncEventUi() {
            etEnd.isEnabled = !isEvent
            if (isEvent) {
                endMin = startMin
                etEnd.setText(formatMin(startMin))
            }
            cbGlobal.visibility = if (isEvent) View.VISIBLE else View.GONE
        }
        syncEventUi()

        cbEvent.setOnCheckedChangeListener { _, checked ->
            isEvent = checked
            syncEventUi()
        }
        cbGlobal.setOnCheckedChangeListener { _, checked -> isGlobal = checked }

        val dialog = MaterialAlertDialogBuilder(ctx)
            .setTitle(if (initial == null) "添加课程 / 事件" else "编辑课程 / 事件")
            .setView(view)
            .create()

        btnCancel.setOnClickListener { dialog.dismiss() }

        if (initial != null && onDelete != null) {
            btnDelete.visibility = View.VISIBLE
            btnDelete.setOnClickListener {
                dialog.dismiss()
                onDelete.invoke()
            }
        }

        btnSave.setOnClickListener {
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
            if (!isEvent && startMin >= endMin) {
                tilEnd.error = "结束时间必须晚于开始时间"
                return@setOnClickListener
            }
            tilEnd.error = null
            val realEnd = if (isEvent) startMin else endMin
            val g = if (isEvent && isGlobal) 1 else 0
            onSave(Course(initial?.index ?: -1, name, day, startMin, realEnd, parity, g))
            dialog.dismiss()
        }

        dialog.show()
    }

    private fun formatMin(min: Int): String = String.format("%02d:%02d", min / 60, min % 60)
}
