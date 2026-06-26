package com.ultracast.sender

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.os.SystemClock
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.util.Collections

class MainActivity : AppCompatActivity() {

    companion object {
        const val DEFAULT_HOST = "10.18.147.181"
        const val CONTROL_PORT = 5002
        const val DISCOVERY_PORT = 5003
        const val DISCOVER_MSG = "ULTRACAST_DISCOVER"
        const val REPLY_PREFIX = "ULTRACAST:"
        private const val PREFS = "ultracast_prefs"
        private const val KEY_HOST = "target_host"
    }

    private val projectionManager by lazy {
        getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    }

    private lateinit var prefs: SharedPreferences
    private lateinit var statusText: TextView
    private lateinit var listContainer: LinearLayout
    private lateinit var hostInput: EditText
    private lateinit var manualBox: LinearLayout
    private var targetHost: String = DEFAULT_HOST
    private val seen = HashSet<String>()

    private val captureLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                startCapture(result.resultCode, result.data!!)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val savedHost = prefs.getString(KEY_HOST, DEFAULT_HOST) ?: DEFAULT_HOST

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 80, 48, 48)
        }

        val title = TextView(this).apply {
            text = "UltraCast"
            textSize = 24f
        }

        statusText = TextView(this).apply {
            text = "Buscando proyectores…"
            textSize = 15f
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 24
            layoutParams = p
        }

        listContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 16
            layoutParams = p
        }

        val rescanBtn = Button(this).apply {
            text = "Buscar de nuevo"
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 16
            layoutParams = p
        }
        rescanBtn.setOnClickListener { startDiscovery() }

        val manualToggle = Button(this).apply {
            text = "Escribir IP manualmente"
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 40
            layoutParams = p
        }

        hostInput = EditText(this).apply {
            setText(savedHost)
            hint = "10.18.147.181"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setSingleLine(true)
        }
        val connectManual = Button(this).apply { text = "Conectar a esta IP" }
        connectManual.setOnClickListener {
            val host = hostInput.text.toString().trim()
            if (host.isEmpty()) {
                Toast.makeText(this, "Escribe la IP del proyector", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            connectTo(host)
        }
        manualBox = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            addView(hostInput)
            addView(connectManual)
        }
        manualToggle.setOnClickListener {
            manualBox.visibility =
                if (manualBox.visibility == View.GONE) View.VISIBLE else View.GONE
        }

        val debugBtn = Button(this).apply {
            text = "Mostrar / ocultar depuración"
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 40
            layoutParams = p
        }
        debugBtn.setOnClickListener {
            val host = if (targetHost.isNotEmpty()) targetHost else hostInput.text.toString().trim()
            if (host.isEmpty()) {
                Toast.makeText(this, "Conéctate a un proyector primero", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            sendDebugToggle(host)
            Toast.makeText(this, "Comando enviado al proyector", Toast.LENGTH_SHORT).show()
        }

        root.addView(title)
        root.addView(statusText)
        root.addView(listContainer)
        root.addView(rescanBtn)
        root.addView(manualToggle)
        root.addView(manualBox)
        root.addView(debugBtn)
        setContentView(ScrollView(this).apply { addView(root) })

        startDiscovery()
    }

    private fun startDiscovery() {
        seen.clear()
        listContainer.removeAllViews()
        statusText.text = "Buscando proyectores…"
        Thread {
            try {
                val sock = DatagramSocket()
                sock.broadcast = true
                sock.soTimeout = 250
                val query = DISCOVER_MSG.toByteArray()
                val targets = broadcastTargets()

                fun sendQuery() {
                    for (t in targets) {
                        try { sock.send(DatagramPacket(query, query.size, t, DISCOVERY_PORT)) } catch (_: Exception) {}
                    }
                }

                sendQuery()
                val buf = ByteArray(128)
                val deadline = SystemClock.elapsedRealtime() + 2200
                var found = 0
                while (SystemClock.elapsedRealtime() < deadline) {
                    try {
                        val pkt = DatagramPacket(buf, buf.size)
                        sock.receive(pkt)
                        val msg = String(pkt.data, 0, pkt.length)
                        if (msg.startsWith(REPLY_PREFIX)) {
                            val ip = pkt.address.hostAddress ?: continue
                            val name = msg.removePrefix(REPLY_PREFIX).ifBlank { "Proyector" }
                            if (seen.add(ip)) {
                                found++
                                runOnUiThread { addProjector(name, ip) }
                            }
                        }
                    } catch (_: SocketTimeoutException) {
                        sendQuery()
                    }
                }
                sock.close()
                runOnUiThread {
                    statusText.text = if (found > 0)
                        "Toca un proyector para conectar"
                    else
                        "No se encontró ningún proyector. Prueba “Buscar de nuevo” o usa IP manual."
                }
            } catch (e: Exception) {
                runOnUiThread {
                    statusText.text = "Error buscando: ${e.message}. Usa IP manual."
                }
            }
        }.start()
    }

    private fun addProjector(name: String, ip: String) {
        val b = Button(this).apply {
            text = "$name\n$ip"
            textSize = 16f
            val p = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            p.topMargin = 12
            layoutParams = p
        }
        b.setOnClickListener { connectTo(ip) }
        listContainer.addView(b)
    }

    private fun broadcastTargets(): List<InetAddress> {
        val out = ArrayList<InetAddress>()
        try {
            for (ni in Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    ia.broadcast?.let { out.add(it) }
                }
            }
        } catch (_: Exception) {}
        try { out.add(InetAddress.getByName("255.255.255.255")) } catch (_: Exception) {}
        return out
    }

    private fun connectTo(host: String) {
        targetHost = host
        prefs.edit().putString(KEY_HOST, host).apply()
        captureLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    private fun startCapture(resultCode: Int, data: Intent) {
        val intent = Intent(this, ScreenCaptureService::class.java).apply {
            putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(ScreenCaptureService.EXTRA_TARGET_HOST, targetHost)
        }
        startForegroundService(intent)
    }

    private fun sendDebugToggle(host: String) {
        Thread {
            try {
                val socket = DatagramSocket()
                val addr = InetAddress.getByName(host)
                val data = byteArrayOf(1)
                socket.send(DatagramPacket(data, data.size, addr, CONTROL_PORT))
                socket.close()
            } catch (_: Exception) {
            }
        }.start()
    }
}