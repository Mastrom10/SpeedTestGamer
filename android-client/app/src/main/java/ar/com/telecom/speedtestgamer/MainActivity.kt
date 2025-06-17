package ar.com.telecom.speedtestgamer

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.jjoe64.graphview.GraphView
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

        // receive sync
        val syncBuf = ByteArray(16)
        val syncPacket = DatagramPacket(syncBuf, syncBuf.size)
        val offset: Long
        try {
            socket.receive(syncPacket)
            val recvTime = System.currentTimeMillis() * 1_000_000L
            val sync = ByteBuffer.wrap(syncBuf).order(ByteOrder.LITTLE_ENDIAN)
            val serverTime = sync.long
            sync.int // server_id
            sync.int // tick
            offset = serverTime - (sendTime + (recvTime - sendTime) / 2)
        } catch (e: Exception) {
            socket.close()
            return "Failed to receive sync: ${e.message}"
        }

        val latencies = mutableListOf<Double>()
        var min = Double.MAX_VALUE
        var max = 0.0
        val packetBuf = ByteArray(1500)
        for (i in 0 until count) {
            val pkt = DatagramPacket(packetBuf, packetBuf.size)
            try {
                socket.receive(pkt)
            } catch (e: Exception) {
                socket.close()
                return "Failed to receive packet: ${e.message}"
            }
            val now = System.currentTimeMillis() * 1_000_000L
            val hdr = ByteBuffer.wrap(pkt.data, 0, 20).order(ByteOrder.LITTLE_ENDIAN)
            hdr.int // seq
            val ts = hdr.long
            hdr.int // server_id
            hdr.int // tick
            val latency = (now - (ts - offset)) / 1e6
            latencies.add(latency)
            if (latency < min) min = latency
            if (latency > max) max = latency

            withContext(Dispatchers.Main) {
                series.appendData(DataPoint(i.toDouble(), latency), true, 50)
                val avg = latencies.average()
                statsView.text = String.format(
                    "Packets: %d Avg: %.2f ms Min: %.2f ms Max: %.2f ms",
                    latencies.size, avg, min, max
                )
            }
        }

        socket.close()
        val avg = latencies.average()
        return String.format(
            "Finished\nAvg: %.2f ms Min: %.2f ms Max: %.2f ms",
            avg, min, max
        )
    }
}