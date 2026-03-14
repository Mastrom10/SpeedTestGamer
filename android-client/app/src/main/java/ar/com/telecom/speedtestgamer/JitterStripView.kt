package ar.com.telecom.speedtestgamer

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import kotlin.math.max

class JitterStripView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val values = ArrayList<Double>()
    private var capacity = 120
    private var scaleMs = 20.0
    private val paintBar = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xFF00C800.toInt()
    }
    private val paintSpike = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xFFAA0000.toInt()
    }
    private val paintAxis = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f
        color = 0x22FFFFFF
    }
    private val paintLabel = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        textSize = 22f
        color = 0xAAFFFFFF.toInt()
    }

    fun reset() { values.clear(); invalidate() }
    fun setCapacity(cap: Int) { capacity = max(30, cap) }
    fun setScale(ms: Double) { scaleMs = ms }

    fun append(jitterMs: Double) {
        values.add(jitterMs)
        while (values.size > capacity) values.removeAt(0)
        postInvalidateOnAnimation()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0 || h <= 0) return
        // guías
        val lines = 3
        for (i in 1..lines) {
            val y = h - h * (i.toFloat() / (lines + 1))
            canvas.drawLine(0f, y, w, y, paintAxis)
        }
        // label esquina
        canvas.drawText("Jitter", 8f, 18f, paintLabel)
        canvas.drawText(String.format("%.0fms", scaleMs), w - 60f, 18f, paintLabel)

        if (values.isEmpty()) return
        val barW = w / capacity
        val start = max(0, values.size - capacity)
        var x = 0
        for (i in start until values.size) {
            val v = values[i]
            val hVal = (v / scaleMs).coerceAtMost(1.0) * h
            val left = x * barW
            val right = left + barW * 0.7f
            val top = (h - hVal).toFloat()
            val isSpike = v > scaleMs
            canvas.drawRect(left, top, right, h, if (isSpike) paintSpike else paintBar)
            x++
        }
    }
}


