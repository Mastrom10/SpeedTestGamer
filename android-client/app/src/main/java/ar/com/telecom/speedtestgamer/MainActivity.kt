package ar.com.telecom.speedtestgamer

import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.jjoe64.graphview.GraphView
import com.jjoe64.graphview.series.DataPoint
import com.jjoe64.graphview.series.LineGraphSeries
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
    private lateinit var spinnerCount: Spinner
    private lateinit var spinnerTick: Spinner
    private lateinit var spinnerPayload: Spinner
    private lateinit var statsView: TextView
    private lateinit var graph: GraphView
    private val series = LineGraphSeries<DataPoint>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        editIp = findViewById(R.id.editIp)
        editPort = findViewById(R.id.editPort)
        startButton = findViewById(R.id.buttonStart)
        spinnerCount = findViewById(R.id.spinnerCount)
        spinnerTick = findViewById(R.id.spinnerTick)
        spinnerPayload = findViewById(R.id.spinnerPayload)
        statsView = findViewById(R.id.textStats)
        graph = findViewById(R.id.graph)
        graph.addSeries(series)

        ArrayAdapter.createFromResource(
            this,
            R.array.packet_counts,
            android.R.layout.simple_spinner_item
        ).also { adapter ->
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerCount.adapter = adapter
        }

        ArrayAdapter.createFromResource(
            this,
            R.array.tick_values,
            android.R.layout.simple_spinner_item
        ).also { adapter ->
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerTick.adapter = adapter
        }

        ArrayAdapter.createFromResource(
            this,
            R.array.payload_sizes,
            android.R.layout.simple_spinner_item
        ).also { adapter ->
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerPayload.adapter = adapter
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
        val count = spinnerCount.selectedItem.toString().toInt()
        val tickMs = spinnerTick.selectedItem.toString().toInt()
        val payloadSize = spinnerPayload.selectedItem.toString().toInt()

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
        val bufReq = ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
        bufReq.putInt(count)
        val sendTime = System.nanoTime()
        bufReq.putLong(sendTime)
        bufReq.putInt(0) // client_id
        bufReq.putInt(payloadSize)
        bufReq.putInt(tickMs)
        val reqData = bufReq.array()
        socket.send(DatagramPacket(reqData, reqData.size, server, port))

        // receive sync
        val syncBuf = ByteArray(16)
        val syncPacket = DatagramPacket(syncBuf, syncBuf.size)
        socket.receive(syncPacket)
        val recvTime = System.nanoTime()
        val sync = ByteBuffer.wrap(syncBuf).order(ByteOrder.LITTLE_ENDIAN)
        val serverTime = sync.long
        sync.int // server_id
        val usedTick = sync.int
        val offset = serverTime - (sendTime + (recvTime - sendTime) / 2)

        val latencies = mutableListOf<Double>()
        var min = Double.MAX_VALUE
        var max = 0.0
        val packetBuf = ByteArray(1500)
        for (i in 0 until count) {
            val pkt = DatagramPacket(packetBuf, packetBuf.size)
            socket.receive(pkt)
            val now = System.nanoTime()
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
                series.appendData(DataPoint(i.toDouble(), latency), true, count)
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