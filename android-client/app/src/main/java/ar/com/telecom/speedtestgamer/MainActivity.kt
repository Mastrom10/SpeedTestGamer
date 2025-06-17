package ar.com.telecom.speedtestgamer

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import android.graphics.Color
import com.jjoe64.graphview.GraphView
import com.jjoe64.graphview.ValueDependentColor
import com.jjoe64.graphview.series.DataPoint
import com.jjoe64.graphview.series.BarGraphSeries
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MainActivity : AppCompatActivity() {

    private val SYNC_COUNT = 5

    private lateinit var editIp: EditText
    private lateinit var editPort: EditText
    private lateinit var startButton: Button
    private lateinit var editCount: EditText
    private lateinit var editTick: EditText
    private lateinit var editPayload: EditText
    private lateinit var statsView: TextView
    private lateinit var graph: GraphView
    private val series = BarGraphSeries<DataPoint>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        editIp = findViewById(R.id.editIp)
        editPort = findViewById(R.id.editPort)
        startButton = findViewById(R.id.buttonStart)
        editCount = findViewById(R.id.editCount)
        editTick = findViewById(R.id.editTick)
        editPayload = findViewById(R.id.editPayload)
        statsView = findViewById(R.id.textStats)
        graph = findViewById(R.id.graph)
        graph.addSeries(series)
        series.spacing = 50
        // enable scrolling of the graph when new data arrives
        graph.viewport.isXAxisBoundsManual = true
        graph.viewport.setMinX(0.0)
        graph.viewport.setMaxX(50.0)
        graph.viewport.isYAxisBoundsManual = true
        graph.viewport.setMinY(0.0)
        graph.viewport.setMaxY(100.0)
        series.valueDependentColor = ValueDependentColor { point ->
            when {
                point.y < 40 -> Color.GREEN
                point.y < 70 -> Color.YELLOW
                point.y >= 100 -> Color.rgb(139, 0, 0)
                else -> Color.RED
            }
        }



        startButton.setOnClickListener {
            startTest()
        }
    }

    private fun startTest() {
        series.resetData(arrayOf())
        statsView.text = "Running..."
        val ip = editIp.text.toString()
        val port = editPort.text.toString().toIntOrNull() ?: return
        val count = editCount.text.toString().toIntOrNull() ?: return
        val tickMs = editTick.text.toString().toIntOrNull() ?: return
        val payloadSize = editPayload.text.toString().toIntOrNull() ?: return

        lifecycleScope.launch(Dispatchers.IO) {
            val result = runTest(ip, port, count, tickMs, payloadSize)
            withContext(Dispatchers.Main) {
                statsView.text = result
            }
        }
    }

    private suspend fun runTest(ip: String, port: Int, count: Int, tickMs: Int, payloadSize: Int): String {
        val socket = DatagramSocket()
        socket.soTimeout = 3000
        val server = InetAddress.getByName(ip)

        // build request
        // Request consists of 4 uint32 fields and one uint64 timestamp
        // => 4*4 + 8 = 24 bytes
        val bufReq = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        bufReq.putInt(count)
        val sendTime = System.currentTimeMillis() * 1_000_000L
        bufReq.putLong(sendTime)
        bufReq.putInt(0) // client_id
        bufReq.putInt(payloadSize)
        bufReq.putInt(tickMs)
        val reqData = bufReq.array()
        socket.send(DatagramPacket(reqData, reqData.size, server, port))

        // receive multiple sync messages and pick the one with the smallest RTT
        val syncBuf = ByteArray(16)
        val syncPacket = DatagramPacket(syncBuf, syncBuf.size)
        var offset: Long = 0
        var bestRtt = Long.MAX_VALUE
        try {
            for (i in 0 until SYNC_COUNT) {
                socket.receive(syncPacket)
                val recvTime = System.currentTimeMillis() * 1_000_000L
                val sync = ByteBuffer.wrap(syncBuf).order(ByteOrder.LITTLE_ENDIAN)
                val serverTime = sync.long
                sync.int // server_id
                sync.int // tick
                val rtt = recvTime - sendTime
                val off = serverTime - (sendTime + rtt / 2)
                if (rtt < bestRtt) {
                    bestRtt = rtt
                    offset = off
                }
            }
        } catch (e: Exception) {
            socket.close()
            return "Failed to receive sync: ${e.message}"
        }

        val latencies = mutableListOf<Double>()
        var min = Double.MAX_VALUE
        var max = 0.0
        var lost = 0
        var outOfOrder = 0
        var expectedSeq = 0
        var x = 0
        val packetBuf = ByteArray(1500)
        var received = 0
        while (received < count) {
            val pkt = DatagramPacket(packetBuf, packetBuf.size)
            try {
                socket.receive(pkt)
            } catch (e: Exception) {
                socket.close()
                return "Failed to receive packet: ${e.message}"
            }
            val now = System.currentTimeMillis() * 1_000_000L
            if (pkt.length == 16) {
                val bb = ByteBuffer.wrap(pkt.data, 0, 16).order(ByteOrder.LITTLE_ENDIAN)
                val serverTime = bb.long
                bb.int
                bb.int
                val rtt = now - sendTime
                val off = serverTime - (sendTime + rtt / 2)
                if (rtt < bestRtt) {
                    bestRtt = rtt
                    offset = off
                }
                withContext(Dispatchers.Main) {
                    val avg = latencies.average()
                    statsView.text = String.format(
                        "Packets: %d Avg: %.2f ms Min: %.2f ms Max: %.2f ms Lost: %d OoO: %d Off: %.2f ms",
                        latencies.size, avg, min, max, lost, outOfOrder, offset / 1_000_000.0
                    )
                }
                continue
            }
            if (pkt.length < 20) {
                continue
            }
            val hdr = ByteBuffer.wrap(pkt.data, 0, 20).order(ByteOrder.LITTLE_ENDIAN)
            val seq = hdr.int
            val ts = hdr.long
            hdr.int // server_id
            hdr.int // tick
            if (seq > expectedSeq) {
                for (m in expectedSeq until seq) {
                    lost++
                    latencies.add(100.0)
                    if (100.0 > max) max = 100.0
                    withContext(Dispatchers.Main) {
                        series.appendData(DataPoint(x.toDouble(), 100.0), true, 50)
                        val avg = latencies.average()
                        statsView.text = String.format(
                            "Packets: %d Avg: %.2f ms Min: %.2f ms Max: %.2f ms Lost: %d OoO: %d Off: %.2f ms",
                            latencies.size, avg, min, max, lost, outOfOrder, offset / 1_000_000.0
                        )
                    }
                    x++
                }
            } else if (seq < expectedSeq) {
                outOfOrder++
            }
            expectedSeq = seq + 1

            var latency = (now - (ts - offset)) / 1e6
            if (latency > 100) {
                latency = 100.0
                lost++
            }
            latencies.add(latency)
            if (latency < min) min = latency
            if (latency > max) max = latency

            withContext(Dispatchers.Main) {
                series.appendData(DataPoint(x.toDouble(), latency), true, 50)
                val avg = latencies.average()
                statsView.text = String.format(
                    "Packets: %d Avg: %.2f ms Min: %.2f ms Max: %.2f ms Lost: %d OoO: %d Off: %.2f ms",
                    latencies.size, avg, min, max, lost, outOfOrder, offset / 1_000_000.0
                )
            }
            x++
            received++
        }

        socket.close()
        val avg = latencies.average()
        return String.format(
            "Finished\nAvg: %.2f ms Min: %.2f ms Max: %.2f ms Lost: %d OoO: %d Off: %.2f ms",
            avg, min, max, lost, outOfOrder, offset / 1_000_000.0
        )
    }
}