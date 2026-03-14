package ar.com.telecom.speedtestgamer

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import kotlin.math.ceil
import kotlin.math.max

class StackedBarChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private data class Sample(val baseMs: Double, val totalMs: Double)

    private val samples = ArrayList<Sample>()
    private var capacity: Int = 50
    private var autoScale: Boolean = true
    private var maxYMs: Double = 200.0
    private var fixedMaxYMs: Double = 200.0
    private var thresholdMs: Double? = null

    private val paintBase = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xFF00C853.toInt() // verde
    }
    private val paintResidual = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xEED50000.toInt() // rojo
    }
    private val paintAxis = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1f
        color = 0x22FFFFFF
    }
    private val paintThreshold = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = 0x66FFFF00.toInt() // amarillo
    }

    fun reset() {
        samples.clear()
        invalidate()
    }

    fun setAutoScale(enabled: Boolean) {
        autoScale = enabled
        invalidate()
    }

    fun setFixedMaxY(ms: Double) {
        fixedMaxYMs = ms
        if (!autoScale) {
            maxYMs = fixedMaxYMs
            invalidate()
        }
    }

    fun setCapacity(cap: Int) {
        capacity = cap.coerceAtLeast(10)
    }

    fun append(baseMs: Double, totalMs: Double, maxHintMs: Double? = null) {
        val clampedBase = baseMs.coerceAtLeast(0.0)
        val clampedTotal = max(totalMs, clampedBase)
        samples.add(Sample(clampedBase, clampedTotal))
        while (samples.size > capacity) samples.removeAt(0)
        if (autoScale) {
            val hint = maxHintMs ?: clampedTotal
            val target = max(hint, 50.0)
            // suavizado para evitar saltos bruscos
            maxYMs = if (target > maxYMs) {
                maxYMs * 0.7 + target * 0.3
            } else {
                max(maxYMs * 0.98, clampedTotal * 1.1)
            }
        } else {
            maxYMs = fixedMaxYMs
        }
        postInvalidateOnAnimation()
    }

    fun setThreshold(ms: Double?) {
        thresholdMs = ms
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0 || h <= 0) return

        // ejes horizontales suaves
        val lines = 4
        for (i in 1..lines) {
            val y = h - h * (i.toFloat() / (lines + 1))
            canvas.drawLine(0f, y, w, y, paintAxis)
        }

        // línea de umbral (tick)
        thresholdMs?.let { thr ->
            val y = (h - (thr / maxYMs).coerceAtMost(1.0) * h).toFloat()
            canvas.drawLine(0f, y, w, y, paintThreshold)
        }

        if (samples.isEmpty()) return

        val barW = w / capacity
        val start = max(0, samples.size - capacity)
        var x = 0
        for (i in start until samples.size) {
            val s = samples[i]
            val baseH = (s.baseMs / maxYMs).coerceAtMost(1.0) * h
            val totalH = (s.totalMs / maxYMs).coerceAtMost(1.0) * h
            val left = x * barW
            val right = left + barW * 0.9f
            val bottom = h
            // base
            canvas.drawRect(left, (bottom - baseH).toFloat(), right, bottom, paintBase)
            // residual
            canvas.drawRect(left, (bottom - totalH).toFloat(), right, (bottom - baseH).toFloat(), paintResidual)
            x++
        }
    }
}


