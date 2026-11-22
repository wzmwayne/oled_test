package com.wzmwayne.oledpanel

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import androidx.core.content.ContextCompat

/**
 * 课表网格：左侧时间轴 + 7 列（周一..周日）。
 *
 * 纵轴范围按实际课程自动拟合（最早一节前 1 小时 ~ 最晚一节后 1 小时），
 * 不会再无脑从 6 点画到 23 点。
 *
 * 交互：
 *   点星期表头  = 选中该列（之后添加课程默认落在这天）
 *   点左侧时间  = 在该时刻、当前选中列上新建课程
 *   点空白格    = 在该天该时刻新建
 *   点课程块    = 编辑
 */
class TimetableView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var courses: List<Course> = emptyList()
        set(value) {
            field = value
            recomputeRange()
            requestLayout()
            invalidate()
        }

    /** 当前选中的星期列，左侧时间轴的点击落在这天 */
    var selectedDay: Int = 1
        set(value) {
            field = value.coerceIn(1, 7)
            invalidate()
        }

    var onEmptyTap: ((day: Int, minutes: Int) -> Unit)? = null
    var onCourseTap: ((Course) -> Unit)? = null
    var onDaySelected: ((Int) -> Unit)? = null

    private var startHour = 6
    private var endHour = 22

    private val d = resources.displayMetrics.density
    private val colW = dp(50f)
    private val axisW = dp(40f)
    private val headerH = dp(28f)
    private val rowH = dp(50f)

    private val cFg = ContextCompat.getColor(context, R.color.fg)
    private val cDim = ContextCompat.getColor(context, R.color.dim)
    private val cLine = ContextCompat.getColor(context, R.color.line)
    private val cSurface = ContextCompat.getColor(context, R.color.surface)
    private val cAccent = ContextCompat.getColor(context, R.color.accent)

    private val palette = intArrayOf(
        0xFF4D9FFF.toInt(), 0xFF3DDC97.toInt(), 0xFFFFB454.toInt(),
        0xFFB48CFF.toInt(), 0xFFFF7A7A.toInt(), 0xFF4DD0E1.toInt()
    )

    private val pLine = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = cLine
        strokeWidth = d
        style = Paint.Style.STROKE
    }
    private val pFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val pText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = cFg
        textSize = sp(11f)
    }
    private val pSmall = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = cDim
        textSize = sp(9f)
    }

    private fun dp(v: Float) = v * d
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val dayNames = arrayOf("一", "二", "三", "四", "五", "六", "日")

    /** 纵轴跟着课表走：没课就是 6~22，有课就取最早/最晚前后各留一小时 */
    private fun recomputeRange() {
        if (courses.isEmpty()) {
            startHour = 6
            endHour = 22
            return
        }
        var minH = 24
        var maxH = 0
        for (c in courses) {
            minH = minOf(minH, c.start / 60)
            maxH = maxOf(maxH, (c.end + 59) / 60)
        }
        startHour = (minH - 1).coerceIn(0, 23)
        endHour = (maxH + 1).coerceIn(startHour + 2, 24)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = (axisW + colW * 7).toInt()
        val h = (headerH + rowH * (endHour - startHour)).toInt()
        setMeasuredDimension(w, h)
    }

    private fun hourTop(hour: Int): Float = headerH + (hour - startHour) * rowH

    private fun minutesToY(min: Int): Float {
        val lo = startHour * 60
        val hi = endHour * 60
        return headerH + (min.coerceIn(lo, hi) - lo) / 60f * rowH
    }

    private fun courseRect(c: Course): RectF {
        val x = axisW + (c.day - 1) * colW + dp(2f)
        val top = minutesToY(c.start)
        val bottom = minutesToY(c.end)
        return RectF(x, top + dp(1f), x + colW - dp(4f), maxOf(bottom - dp(1f), top + dp(14f)))
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(cSurface)

        // 表头：选中的星期高亮
        for (i in 0 until 7) {
            val left = axisW + i * colW
            if (i + 1 == selectedDay) {
                pFill.color = cAccent
                pFill.alpha = 45
                canvas.drawRect(left, 0f, left + colW, headerH, pFill)
            }
            pSmall.color = if (i + 1 == selectedDay) cAccent else cDim
            pSmall.textAlign = Paint.Align.CENTER
            canvas.drawText(dayNames[i], left + colW / 2, headerH - dp(9f), pSmall)
        }
        pSmall.textAlign = Paint.Align.LEFT
        pSmall.color = cDim

        // 小时线 + 左侧时间
        for (h in startHour..endHour) {
            val y = hourTop(h)
            canvas.drawLine(axisW, y, width.toFloat(), y, pLine)
            pSmall.textAlign = Paint.Align.RIGHT
            canvas.drawText(String.format("%02d:00", h), axisW - dp(6f), y + dp(4f), pSmall)
        }
        pSmall.textAlign = Paint.Align.LEFT

        // 列分隔线
        for (i in 0..7) {
            val x = axisW + i * colW
            canvas.drawLine(x, headerH, x, height.toFloat(), pLine)
        }

        // 课程块
        courses.forEachIndexed { idx, c ->
            val r = courseRect(c)
            pFill.color = palette[idx % palette.size]
            pFill.alpha = 200
            canvas.drawRoundRect(r, dp(4f), dp(4f), pFill)

            pText.color = 0xFF10141A.toInt()
            val textX = r.left + dp(3f)
            var textY = r.top + dp(13f)
            canvas.drawText(ellipsize(c.name, r.width() - dp(6f)), textX, textY, pText)

            if (r.height() > dp(30f)) {
                textY += dp(12f)
                pSmall.color = 0xFF10141A.toInt()
                canvas.drawText(
                    String.format("%d:%02d", c.start / 60, c.start % 60),
                    textX, textY, pSmall
                )
                pSmall.color = cDim
            }
        }
    }

    private fun ellipsize(text: String, maxW: Float): String {
        if (pText.measureText(text) <= maxW) return text
        var s = text
        while (s.isNotEmpty() && pText.measureText(s + "…") > maxW) s = s.dropLast(1)
        return s + "…"
    }

    private var downX = 0f
    private var downY = 0f

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (Math.abs(event.x - downX) > dp(12f) || Math.abs(event.y - downY) > dp(12f)) {
                    return true
                }

                // 表头：选列
                if (event.y < headerH) {
                    val day = ((event.x - axisW) / colW).toInt() + 1
                    if (day in 1..7) {
                        selectedDay = day
                        onDaySelected?.invoke(day)
                    }
                    return true
                }

                // 左侧时间轴：在当前选中列的这个时刻新建
                if (event.x < axisW) {
                    val min = startHour * 60 + (((event.y - headerH) / rowH) * 60).toInt()
                    onEmptyTap?.invoke(selectedDay, snap(min))
                    return true
                }

                // 课程块
                val hit = courses.firstOrNull { courseRect(it).contains(event.x, event.y) }
                if (hit != null) {
                    onCourseTap?.invoke(hit)
                    return true
                }

                // 空白格
                val day = ((event.x - axisW) / colW).toInt() + 1
                if (day !in 1..7) return true
                val min = startHour * 60 + (((event.y - headerH) / rowH) * 60).toInt()
                onEmptyTap?.invoke(day, snap(min))
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun snap(min: Int): Int {
        val s = (min / 5) * 5
        return s.coerceIn(startHour * 60, endHour * 60 - 30)
    }
}
