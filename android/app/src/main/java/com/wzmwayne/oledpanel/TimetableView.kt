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
 * 课表网格：7 列（周一..周日）× 6:00-23:00 每小时一行。
 * 空白处点按 = 在该时刻新建，课程块点按 = 编辑。
 */
class TimetableView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var courses: List<Course> = emptyList()
        set(value) {
            field = value
            invalidate()
        }

    var onEmptyTap: ((day: Int, minutes: Int) -> Unit)? = null
    var onCourseTap: ((Course) -> Unit)? = null

    private val startHour = 6
    private val endHour = 23

    private val d = resources.displayMetrics.density
    private val colW = dp(50f)
    private val axisW = dp(32f)
    private val headerH = dp(26f)
    private val rowH = dp(50f)

    private val cFg = ContextCompat.getColor(context, R.color.fg)
    private val cDim = ContextCompat.getColor(context, R.color.dim)
    private val cLine = ContextCompat.getColor(context, R.color.line)
    private val cSurface = ContextCompat.getColor(context, R.color.surface)

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
    private val rect = RectF()

    private fun dp(v: Float) = v * d
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val dayNames = arrayOf("一", "二", "三", "四", "五", "六", "日")

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = (axisW + colW * 7).toInt()
        val h = (headerH + rowH * (endHour - startHour)).toInt()
        setMeasuredDimension(w, h)
    }

    private fun hourTop(hour: Int): Float = headerH + (hour - startHour) * rowH

    private fun minutesToY(min: Int): Float = headerH + (min - startHour * 60) / 60f * rowH

    private fun courseRect(c: Course): RectF {
        val x = axisW + (c.day - 1) * colW + dp(2f)
        val top = minutesToY(c.start.coerceIn(startHour * 60, endHour * 60))
        val bottom = minutesToY(c.end.coerceIn(startHour * 60, endHour * 60))
        return RectF(x, top + dp(1f), x + colW - dp(4f), maxOf(bottom - dp(1f), top + dp(14f)))
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(cSurface)

        // 表头
        canvas.drawText("", 0f, 0f, pSmall)
        for (i in 0 until 7) {
            val x = axisW + i * colW + colW / 2
            pSmall.textAlign = Paint.Align.CENTER
            canvas.drawText(dayNames[i], x, headerH - dp(8f), pSmall)
        }
        pSmall.textAlign = Paint.Align.LEFT

        // 小时线 + 时间轴
        for (h in startHour..endHour) {
            val y = hourTop(h)
            canvas.drawLine(axisW, y, width.toFloat(), y, pLine)
            canvas.drawText(String.format("%02d", h), dp(4f), y + dp(10f), pSmall)
        }
        // 列分隔线
        for (i in 0..7) {
            val x = axisW + i * colW
            canvas.drawLine(x, headerH, x, height.toFloat(), pLine)
        }

        // 课程块
        courses.forEachIndexed { idx, c ->
            val r = courseRect(c)
            val color = palette[idx % palette.size]
            pFill.color = color
            pFill.alpha = 200
            canvas.drawRoundRect(r, dp(4f), dp(4f), pFill)

            pText.color = 0xFF10141A.toInt()
            val textX = r.left + dp(3f)
            var textY = r.top + dp(13f)
            val maxW = r.width() - dp(6f)
            val name = c.name
            val shown = ellipsize(name, maxW)
            canvas.drawText(shown, textX, textY, pText)

            if (r.height() > dp(30f)) {
                textY += dp(12f)
                pSmall.color = 0xFF10141A.toInt()
                pSmall.textAlign = Paint.Align.LEFT
                canvas.drawText(formatRange(c), textX, textY, pSmall)
            }
        }
    }

    private fun formatRange(c: Course): String =
        String.format("%d:%02d", c.start / 60, c.start % 60)

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
                // 先找课程块
                val hit = courses.firstOrNull { courseRect(it).contains(event.x, event.y) }
                if (hit != null) {
                    onCourseTap?.invoke(hit)
                    return true
                }
                val day = ((event.x - axisW) / colW).toInt() + 1
                if (day !in 1..7) return true
                if (event.y < headerH) return true
                val min = startHour * 60 + (((event.y - headerH) / rowH) * 60).toInt()
                val snapped = (min / 5) * 5
                onEmptyTap?.invoke(day, snapped.coerceIn(startHour * 60, endHour * 60 - 30))
                return true
            }
        }
        return super.onTouchEvent(event)
    }
}
