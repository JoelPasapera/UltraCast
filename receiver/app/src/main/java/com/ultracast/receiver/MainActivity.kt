// file: MainActivity.kt
// path: app\src\main\java\com\ultracast\receiver\MainActivity.kt

package com.ultracast.receiver

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.nio.ByteBuffer

class MainActivity : Activity() {

    companion object {
        const val LISTEN_PORT = 5000
        const val FEEDBACK_PORT = 5001
        const val CONTROL_PORT = 5002
        const val DISCOVERY_PORT = 5003
        const val STREAM_WIDTH = 1280
        const val STREAM_HEIGHT = 720
    }

    private lateinit var surfaceView: SurfaceView
    private lateinit var statsText: TextView
    private var decoder: VideoDecoder? = null
    private var nativeHandle: Long = 0L
    private var recvThread: Thread? = null
    private var controlThread: Thread? = null
    private var controlSocket: DatagramSocket? = null
    private var discoveryThread: Thread? = null
    private var discoverySocket: DatagramSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    @Volatile private var discoveryRunning = false
    @Volatile private var running = false
    @Volatile private var showOverlay = false

    @Volatile private var frameCount = 0
    @Volatile private var lastFrameSize = 0
    @Volatile private var minFrameSize = Int.MAX_VALUE
    @Volatile private var maxFrameSize = 0
    @Volatile private var totalBytes = 0L
    @Volatile private var lastPts = 0L
    @Volatile private var initOk = false
    @Volatile private var startMs = 0L
    @Volatile private var firstFrameMs = 0L
    @Volatile private var maxIdrSize = 0
    @Volatile private var firstIdr = "—"
    private val nalCounts = IntArray(32)

    private var spsBytes: ByteArray? = null
    private var ppsBytes: ByteArray? = null
    @Volatile private var gotIdr = false
    @Volatile private var preFeedDropped = 0
    @Volatile private var fecSimd = Int.MIN_VALUE   // estado NEON del FEC (sin leer aún)

    private val uiHandler = Handler(Looper.getMainLooper())
    private val statsRunnable = object : Runnable {
        override fun run() {
            val d = decoder
            val elapsed = if (startMs > 0) (SystemClock.elapsedRealtime() - startMs) / 1000.0 else 0.0
            val mn = if (minFrameSize == Int.MAX_VALUE) 0 else minFrameSize
            statsText.text = buildString {
                append("UltraCast — DIAGNÓSTICO COMPLETO\n")
                append("t=${"%.0f".format(elapsed)}s  Init:${if (initOk) "OK" else "..."}  Puerto:$LISTEN_PORT\n")
                append("── RED ──\n")
                append("Frames:$frameCount  Bytes:${"%.2f".format(totalBytes / 1_000_000.0)}MB\n")
                append("Tam: min $mn / máx $maxFrameSize / últ $lastFrameSize B\n")
                append("── NAL (1er tipo) ──\n")
                append("SPS7:${nalCounts[7]}  PPS8:${nalCounts[8]}  IDR5:${nalCounts[5]}  P1:${nalCounts[1]}  SEI6:${nalCounts[6]}\n")
                append("1er IDR: $firstIdr\n")
                append("── DECODER ──\n")
                append("Codec: ${d?.codecName ?: "—"}  iniciado:${if (d?.started == true) "Si" else "No"}\n")
                append("CSD: ${d?.csdInfo ?: "—"}\n")
                append("Input: encolados ${d?.inQueued ?: 0} / sin-buffer ${d?.inDropped ?: 0}\n")
                append("Output: DECODIFICADOS ${d?.decodedCount ?: 0}  formatChg:${d?.formatChangedCount ?: 0}\n")
                append("Formato salida: ${d?.outputFormat ?: "—"}\n")
                append("Feedback + FEC adaptativo: en nativo\n")
                append("FEC SIMD: ${fecSimdLabel()}\n")
                append("Error: ${d?.lastError ?: "—"}")
            }
            statsText.visibility = if (showOverlay) View.VISIBLE else View.GONE
            uiHandler.postDelayed(this, 500)
        }
    }

    private fun toggleOverlay() {
        showOverlay = !showOverlay
        uiHandler.post {
            statsText.visibility = if (showOverlay) View.VISIBLE else View.GONE
        }
    }

    // Lee (una sola vez) el estado de NEON del FEC nativo y lo cachea para el overlay.
    private fun fecSimdLabel(): String {
        if (fecSimd == Int.MIN_VALUE) {
            fecSimd = try { NativeBridge.fecSimd() } catch (_: Throwable) { -99 }
        }
        return when (fecSimd) {
            1 -> "NEON activo"
            0 -> "escalar (auto-test discrepó)"
            -1 -> "escalar (ABI 32-bit, sin NEON)"
            else -> "no disponible"
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
            keyCode == KeyEvent.KEYCODE_ENTER ||
            keyCode == KeyEvent.KEYCODE_BUTTON_A) {
            toggleOverlay()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        try {
            val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            multicastLock = wifi.createMulticastLock("ultracast-disc").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (_: Exception) {
        }

        surfaceView = SurfaceView(this)
        statsText = TextView(this).apply {
            setTextColor(Color.GREEN)
            textSize = 13f
            setBackgroundColor(Color.argb(195, 0, 0, 0))
            setPadding(20, 20, 20, 20)
            text = "UltraCast Receptor\nIniciando…"
            visibility = View.GONE
        }
        val root = FrameLayout(this).apply {
            isClickable = true
            isFocusable = true
            addView(
                surfaceView,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT
                )
            )
            addView(
                statsText,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.START
                )
            )
            setOnClickListener { toggleOverlay() }
        }
        setContentView(root)
        hideSystemBars()

        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) { startReceiving(holder) }
            override fun surfaceChanged(holder: SurfaceHolder, f: Int, w: Int, h: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) { stopReceiving() }
        })
        uiHandler.post(statsRunnable)
        startDiscoveryResponder()
    }

    private fun startReceiving(holder: SurfaceHolder) {
        if (running) return
        startMs = SystemClock.elapsedRealtime()
        spsBytes = null; ppsBytes = null; gotIdr = false; preFeedDropped = 0

        decoder = VideoDecoder(STREAM_WIDTH, STREAM_HEIGHT, holder.surface)

        nativeHandle = NativeBridge.init(LISTEN_PORT, FEEDBACK_PORT)
        initOk = nativeHandle != 0L
        val frameBuf: ByteBuffer = NativeBridge.getBuffer(nativeHandle)

        running = true
        recvThread = Thread {
            while (running) {
                val size = NativeBridge.receiveFrame(nativeHandle)
                if (size <= 0) continue
                frameCount++
                if (firstFrameMs == 0L) firstFrameMs = SystemClock.elapsedRealtime()
                lastFrameSize = size
                if (size < minFrameSize) minFrameSize = size
                if (size > maxFrameSize) maxFrameSize = size
                totalBytes += size

                val ptsUs = NativeBridge.getPts(nativeHandle)
                lastPts = ptsUs

                frameBuf.clear()
                frameBuf.limit(size)

                val nalType = if (size >= 5) (frameBuf.get(4).toInt() and 0x1F) else -1
                if (nalType in 0..31) nalCounts[nalType]++
                if (nalType == 5) {
                    if (size > maxIdrSize) maxIdrSize = size
                    if (firstIdr == "—") {
                        val n = minOf(12, size)
                        val sb = StringBuilder()
                        for (i in 0 until n) sb.append(String.format("%02X ", frameBuf.get(i)))
                        firstIdr = sb.toString().trim()
                    }
                }

                val dec = decoder ?: continue

                if (!dec.started) {
                    harvestCsd(frameBuf, size)
                    val sps = spsBytes
                    val pps = ppsBytes
                    if (sps != null && pps != null) {
                        dec.startWithConfig(sps, pps)
                    } else {
                        preFeedDropped++
                    }
                    continue
                }

                if (!gotIdr) {
                    if (nalType == 5) gotIdr = true
                    else { preFeedDropped++; continue }
                }
                if (nalType == 7 || nalType == 8) continue

                dec.feedFrame(frameBuf, size, ptsUs)
            }
        }.also { it.start() }

        startControlListener()
    }

    // Sólo para el botón de mostrar/ocultar depuración (el feedback de vídeo es nativo).
    private fun startControlListener() {
        controlThread = Thread {
            try {
                val sock = DatagramSocket(CONTROL_PORT)
                controlSocket = sock
                val buf = ByteArray(16)
                while (running) {
                    val pkt = DatagramPacket(buf, buf.size)
                    sock.receive(pkt)
                    toggleOverlay()
                }
            } catch (_: Exception) {
            }
        }.also { it.start() }
    }

    // Responde a los "¿hay proyector?" del celular con el nombre del dispositivo.
    // Si el socket falla, se reinicia solo (no se queda mudo).
    private fun startDiscoveryResponder() {
        if (discoveryRunning) return
        discoveryRunning = true
        discoveryThread = Thread {
            val name = "${Build.MANUFACTURER} ${Build.MODEL}".trim()
            val reply = ("ULTRACAST:" + name).toByteArray()
            while (discoveryRunning) {
                try {
                    val sock = DatagramSocket(null)
                    sock.reuseAddress = true
                    sock.bind(InetSocketAddress(DISCOVERY_PORT))
                    discoverySocket = sock
                    val buf = ByteArray(64)
                    while (discoveryRunning) {
                        val pkt = DatagramPacket(buf, buf.size)
                        sock.receive(pkt)
                        val msg = String(pkt.data, 0, pkt.length)
                        if (msg.startsWith("ULTRACAST_DISCOVER")) {
                            sock.send(DatagramPacket(reply, reply.size, pkt.address, pkt.port))
                        }
                    }
                } catch (_: Exception) {
                    try { discoverySocket?.close() } catch (_: Exception) {}
                    if (!discoveryRunning) break
                    try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
                }
            }
        }.also { it.start() }
    }

    private fun harvestCsd(buf: ByteBuffer, size: Int) {
        if (spsBytes != null && ppsBytes != null) return
        val pos = buf.position()
        val arr = ByteArray(size)
        for (k in 0 until size) arr[k] = buf.get(pos + k)

        val sc = ArrayList<IntArray>()
        var i = 0
        while (i + 2 < size) {
            if (arr[i].toInt() == 0 && arr[i + 1].toInt() == 0) {
                if (i + 3 < size && arr[i + 2].toInt() == 0 && arr[i + 3].toInt() == 1) {
                    sc.add(intArrayOf(i, i + 4)); i += 4; continue
                } else if (arr[i + 2].toInt() == 1) {
                    sc.add(intArrayOf(i, i + 3)); i += 3; continue
                }
            }
            i++
        }
        for (idx in sc.indices) {
            val payloadOff = sc[idx][1]
            if (payloadOff >= size) continue
            val nalType = arr[payloadOff].toInt() and 0x1F
            val scOff = sc[idx][0]
            val nextOff = if (idx + 1 < sc.size) sc[idx + 1][0] else size
            if (nalType == 7 && spsBytes == null) spsBytes = arr.copyOfRange(scOff, nextOff)
            if (nalType == 8 && ppsBytes == null) ppsBytes = arr.copyOfRange(scOff, nextOff)
        }
    }

    private fun stopReceiving() {
        running = false
        controlSocket?.close()
        if (nativeHandle != 0L) NativeBridge.stop(nativeHandle)
        try { recvThread?.join(1000) } catch (_: InterruptedException) {}
        try { controlThread?.join(500) } catch (_: InterruptedException) {}
        recvThread = null
        controlThread = null
        controlSocket = null
        decoder?.stop(); decoder = null
        if (nativeHandle != 0L) { NativeBridge.close(nativeHandle); nativeHandle = 0L }
    }

    override fun onDestroy() {
        super.onDestroy()
        discoveryRunning = false
        discoverySocket?.close()
        try { multicastLock?.release() } catch (_: Exception) {}
        try { discoveryThread?.join(500) } catch (_: InterruptedException) {}
        stopReceiving()
        uiHandler.removeCallbacks(statsRunnable)
    }

    @Suppress("DEPRECATION")
    private fun hideSystemBars() {
        window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        or View.SYSTEM_UI_FLAG_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
    }
}