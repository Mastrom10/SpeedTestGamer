package ar.com.telecom.speedtestgamer

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import android.graphics.Color
import android.os.SystemClock
import java.net.SocketTimeoutException
// GraphView eliminado del gráfico principal. Solo mantenemos tipos básicos si hiciera falta.
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MainActivity : AppCompatActivity() {

    private val INIT_SYNC_COUNT = 10
    private val SYNC_INTERVAL_MS = 5000L
    private val MAX_ACCEPTABLE_RTT_NS = 1_000_000_000L // 1s
    private val MAX_OFFSET_JUMP_NS = 100_000_000L // 100ms
    private val INITIAL_MAX_Y_MS = 200.0 // rango fijo del gráfico
    private val LOST_BAR_MS = INITIAL_MAX_Y_MS // barra para timeouts/gaps

    private lateinit var editIp: EditText
    private lateinit var editPort: EditText
    private lateinit var startButton: Button
    private lateinit var clearButton: Button
    private lateinit var configButton: Button
    private lateinit var editCount: EditText
    private lateinit var editTick: EditText
    private lateinit var editPayload: EditText
    private lateinit var statsView: TextView
    private lateinit var clockView: TextView
    private lateinit var rxView: TextView
    private lateinit var qualityView: TextView
    private lateinit var versionView: TextView
    private lateinit var graph: StackedBarChartView
    // graphQueue eliminado
    private lateinit var jitterUp: JitterStripView
    private lateinit var jitterDown: JitterStripView
    private lateinit var hudPing: TextView
    private lateinit var hudMiss: TextView
    private lateinit var hudLoss: TextView
    private lateinit var hudJitter: TextView
    // eliminado progressQueue
    // Series antiguas (GraphView) eliminadas
    private lateinit var checkResidual: android.widget.CheckBox
    private lateinit var checkAutoScale: android.widget.CheckBox
    private lateinit var checkFullRtt: android.widget.CheckBox

    private var testJob: kotlinx.coroutines.Job? = null
    private var initialOffsetNs: Long? = null
    private var lastRttNs: Long = Long.MAX_VALUE
    private var graphMaxYMs: Double = INITIAL_MAX_Y_MS

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        editIp = findViewById(R.id.editIp)
        editPort = findViewById(R.id.editPort)
        startButton = findViewById(R.id.buttonStart)
        clearButton = findViewById(R.id.buttonClear)
        configButton = findViewById(R.id.buttonConfig)
        editCount = findViewById(R.id.editCount)
        editTick = findViewById(R.id.editTick)
        editPayload = findViewById(R.id.editPayload)
        statsView = findViewById(R.id.textStats)
        clockView = findViewById(R.id.textClockOffset)
        rxView = findViewById(R.id.textRx)
        versionView = findViewById(R.id.textVersion)
        qualityView = findViewById(R.id.textQuality)
        graph = findViewById(R.id.graph)
        // graphQueue ya no existe en el layout; removido
        jitterUp = findViewById(R.id.jitterUp)
        jitterDown = findViewById(R.id.jitterDown)
        hudPing = findViewById(R.id.hudPing)
        hudMiss = findViewById(R.id.hudMiss)
        hudLoss = findViewById(R.id.hudLoss)
        hudJitter = findViewById(R.id.hudJitter)
        // sin progressQueue
        checkResidual = findViewById(R.id.checkResidual)
        checkAutoScale = findViewById(R.id.checkAutoScale)
        checkFullRtt = findViewById(R.id.checkFullRtt)
        // configurar chart custom
        graph.setCapacity(50)
        graph.setAutoScale(checkAutoScale.isChecked)
        graph.setFixedMaxY(INITIAL_MAX_Y_MS)
        // series antiguas eliminadas



        loadPrefs()
        versionView.text = "Version: 1.1.0"

        startButton.setOnClickListener {
            lifecycleScope.launch {
                // Asegurar cancelación completa para evitar overlap de series/x
                testJob?.cancelAndJoin()
                if (startButton.text.toString() == "Stop") {
                    startButton.text = "Start"
                    return@launch
                }
                startTest()
            }
        }

        clearButton.setOnClickListener {
            lifecycleScope.launch {
                // Limpia solo datos de medición y estado transitorio, preserva config
                testJob?.cancelAndJoin()
                // Reset gráfico custom
                graph.reset()
                graph.setThreshold(null)
                statsView.text = getString(R.string.stats_placeholder)
                clockView.text = "Offset: - ms"
                initialOffsetNs = null
                lastRttNs = Long.MAX_VALUE
                // sin graphQueue
                startButton.text = "Play"
                // Reset de contadores de transferencia
                rxView.text = "RX: 0 B | Rate: 0 B/s"
            }
        }

        configButton.setOnClickListener {
            val container = findViewById<android.view.View>(R.id.configContainer)
            container.visibility = if (container.visibility == android.view.View.VISIBLE) android.view.View.GONE else android.view.View.VISIBLE
        }
    }

    private fun startTest() {
        graph.reset()
        var x = 0
        statsView.text = "Running..."
        val ip = editIp.text.toString()
        val port = editPort.text.toString().toIntOrNull() ?: return
        val count = editCount.text.toString().toIntOrNull() ?: return
        val tickMs = editTick.text.toString().toIntOrNull() ?: return
        val payloadSize = editPayload.text.toString().toIntOrNull() ?: return

        savePrefs(ip, port, count, tickMs, payloadSize)

        graph.setThreshold(tickMs.toDouble())
        startButton.text = "Stop"
        testJob = lifecycleScope.launch(Dispatchers.IO) {
            val result = runTest(ip, port, count, tickMs, payloadSize)
            withContext(Dispatchers.Main) {
                startButton.text = "Start"
                statsView.text = result
            }
        }
    }

    private suspend fun runTest(ip: String, port: Int, count: Int, tickMs: Int, payloadSize: Int): String {
        val socket = DatagramSocket()
        val server = InetAddress.getByName(ip)
        // timeout por tick: permite clasificar misses sin frenar el ritmo
        socket.soTimeout = (tickMs * 2).coerceAtLeast(50)

        val latencies = mutableListOf<Double>()
        var min = Double.MAX_VALUE
        var max = 0.0
        var misses = 0
        var lates = 0
        var onTime = 0
        var totalBytes: Long = 0
        var lastRateBytes: Long = 0
        var lastRateTs = SystemClock.elapsedRealtimeNanos()

        val tickNs = tickMs * 1_000_000L
        val startNs = SystemClock.elapsedRealtimeNanos() + 5_000_000L // 5ms para armar el primer envío

        for (seq in 0 until count) {
            if (!currentCoroutineContext().isActive) {
                socket.close()
                return "Cancelled"
            }

            val scheduledSend = startNs + seq * tickNs
            var now = SystemClock.elapsedRealtimeNanos()
            val waitNs = scheduledSend - now
            if (waitNs > 0) {
                // sleep grueso y pequeño spin para mayor precisión sin busy-wait largo
                val sleepMs = (waitNs / 1_000_000L).coerceAtLeast(0L)
                if (sleepMs > 0) Thread.sleep(sleepMs)
                while (SystemClock.elapsedRealtimeNanos() < scheduledSend) { /* spin corto */ }
            }

                    val sendTs = SystemClock.elapsedRealtimeNanos()
                    val hdr = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
            hdr.putInt(seq)
                    hdr.putLong(sendTs)
                    hdr.putInt(0xEEEEEEEE.toInt())
                    hdr.putInt(tickMs)
            val out = hdr.array()
            socket.send(DatagramPacket(out, out.size, server, port))

            val inBuf = ByteArray(64)
            val inPkt = DatagramPacket(inBuf, inBuf.size)
            var gotReply = false
            try {
                socket.receive(inPkt)
                val recvNs = SystemClock.elapsedRealtimeNanos()
                totalBytes += inPkt.length

                val bb = ByteBuffer.wrap(inPkt.data, 0, inPkt.length).order(ByteOrder.LITTLE_ENDIAN)
                if (inPkt.length >= 20) {
                    val rSeq = bb.int
                    val rTs = bb.long
                    val rSid = bb.int
                    /* val rTick = */ bb.int
                    if (rSid == 0xEEEEEEEE.toInt() && rSeq == seq && rTs == sendTs) {
                        val rttMs = (recvNs - sendTs) / 1e6
                        latencies.add(rttMs)
                        if (rttMs < min) min = rttMs
                        if (rttMs > max) max = rttMs
                        if (rttMs <= tickMs) onTime++ else lates++

                        // gráfico: base = min(rtt, tick) (verde), residual = max(0, rtt - tick) (rojo)
                        val base = kotlin.math.min(rttMs, tickMs.toDouble())
                        val total = rttMs
                        withContext(Dispatchers.Main) {
                            val p95 = percentile(latencies, 95.0)
                            // escalar sugerido entre tick y p95
                            val hint = kotlin.math.max(tickMs * 1.0, p95 * 1.2)
                            graph.setAutoScale(checkAutoScale.isChecked)
                            if (!checkAutoScale.isChecked) graph.setFixedMaxY(INITIAL_MAX_Y_MS)
                            graph.append(base, total, hint)
                            val rateWindowSec = (recvNs - lastRateTs) / 1_000_000_000.0
                            if (rateWindowSec >= 1.0) {
                                val bytesInWindow = totalBytes - lastRateBytes
                                lastRateBytes = totalBytes
                                lastRateTs = recvNs
                                rxView.text = String.format("RX: %s | Rate: %s/s", formatBytes(totalBytes), formatBytes((bytesInWindow / rateWindowSec).toLong()))
                            }
                            val onPct = onTime * 100.0 / (seq + 1)
                            val latePct = lates * 100.0 / (seq + 1)
                            val missPct = misses * 100.0 / (seq + 1)
                            statsView.text = String.format("Ticks: %d On-time: %.1f%% Late: %.1f%% Miss: %.1f%% Avg: %.1f ms", seq + 1, onPct, latePct, missPct, latencies.average())
                            hudPing.text = String.format("Tick: %d ms", tickMs)
                            hudMiss.text = String.format("On: %.1f%%", onPct)
                            hudLoss.text = String.format("Late: %.1f%%", latePct)
                            hudJitter.text = String.format("Miss: %.1f%%", missPct)
                        }
                        gotReply = true
                    }
                }
            } catch (e: SocketTimeoutException) {
                // sin respuesta en la ventana del timeout
            }

            if (!gotReply) {
                misses++
            withContext(Dispatchers.Main) {
                    val hint = tickMs * 1.2
                    graph.setAutoScale(checkAutoScale.isChecked)
                    if (!checkAutoScale.isChecked) graph.setFixedMaxY(INITIAL_MAX_Y_MS)
                    // barra roja hasta el umbral del tick para indicar miss
                    graph.append(0.0, tickMs.toDouble(), hint)
                    val onPct = onTime * 100.0 / (seq + 1)
                    val latePct = lates * 100.0 / (seq + 1)
                    val missPct = misses * 100.0 / (seq + 1)
                    statsView.text = String.format("Ticks: %d On-time: %.1f%% Late: %.1f%% Miss: %.1f%% Avg: %.1f ms", seq + 1, onPct, latePct, missPct, latencies.average())
                    hudPing.text = String.format("Tick: %d ms", tickMs)
                    hudMiss.text = String.format("On: %.1f%%", onPct)
                    hudLoss.text = String.format("Late: %.1f%%", latePct)
                    hudJitter.text = String.format("Miss: %.1f%%", missPct)
                }
            }
        }

        socket.close()
        return String.format(
            "Finished\nAvg: %.2f ms Min: %.2f ms Max: %.2f ms On-time: %d Late: %d Miss: %d",
            latencies.average(), min, max, onTime, lates, misses
        )
    }

    // escala fija a 200ms

    private fun formatBytes(bytes: Long): String {
        val abs = kotlin.math.abs(bytes.toDouble())
        return when {
            abs >= 1e9 -> String.format("%.2f GB", bytes / 1e9)
            abs >= 1e6 -> String.format("%.2f MB", bytes / 1e6)
            abs >= 1e3 -> String.format("%.2f KB", bytes / 1e3)
            else -> String.format("%d B", bytes)
        }
    }

    private fun percentile(data: List<Double>, p: Double): Double {
        if (data.isEmpty()) return 0.0
        val sorted = data.sorted()
        val rank = (p / 100.0) * (sorted.size - 1)
        val low = kotlin.math.floor(rank).toInt()
        val high = kotlin.math.ceil(rank).toInt()
        return if (low == high) sorted[low] else {
            val w = rank - low
            sorted[low] * (1 - w) + sorted[high] * w
        }
    }

    private fun movingStd(data: List<Double>, window: Int = 50): Double {
        if (data.isEmpty()) return 0.0
        val start = if (data.size > window) data.size - window else 0
        val slice = data.subList(start, data.size)
        val mean = slice.average()
        var acc = 0.0
        for (v in slice) acc += (v - mean) * (v - mean)
        return kotlin.math.sqrt(acc / slice.size)
    }

    private fun savePrefs(ip: String, port: Int, count: Int, tick: Int, payload: Int) {
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        prefs.edit().apply {
            putString("ip", ip)
            putInt("port", port)
            putInt("count", count)
            putInt("tick", tick)
            putInt("payload", payload)
        }.apply()
    }

    private fun loadPrefs() {
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        editIp.setText(prefs.getString("ip", ""))
        editPort.setText(prefs.getInt("port", 0).takeIf { it != 0 }?.toString() ?: "")
        editCount.setText(prefs.getInt("count", 0).takeIf { it != 0 }?.toString() ?: "")
        editTick.setText(prefs.getInt("tick", 0).takeIf { it != 0 }?.toString() ?: "")
        editPayload.setText(prefs.getInt("payload", 0).takeIf { it != 0 }?.toString() ?: "")
    }
}