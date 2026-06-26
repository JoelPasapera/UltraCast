package com.ultracast.sender

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager

class ScreenCaptureService : Service() {

    companion object {
        const val TAG = "UltraCast"
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        const val EXTRA_TARGET_HOST = "target_host"
        private const val CHANNEL_ID = "ultracast"
        private const val NOTIF_ID = 1

        const val DEFAULT_HOST = "10.18.147.181"
        const val TARGET_PORT = 5000     // vídeo (= receptor)
        const val FEEDBACK_PORT = 5001   // feedback del receptor (lo escucha el nativo)
        const val POLL_MS = 50L
        const val KF_THROTTLE_MS = 250L

        const val STREAM_WIDTH = 1280
        const val STREAM_HEIGHT = 720
        const val STREAM_FPS = 60
        const val STREAM_BITRATE = 8_000_000   // inicial (el ABR sube/baja desde aquí)
        const val BITRATE_MIN = 1_500_000      // ABR: piso para WiFi malo
        const val BITRATE_MAX = 10_000_000     // ABR: techo para WiFi excelente
        const val ABR_POLL_MS = 500L           // cada cuánto revisar/aplicar el bitrate
    }

    private var targetHost: String = DEFAULT_HOST
    private var projection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var encoder: VideoEncoder? = null
    private var nativeHandle: Long = 0L

    @Volatile private var running = false
    private var pollThread: Thread? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d(TAG, "Service onStartCommand")
        if (intent == null) {
            Log.e(TAG, "intent == null")
            return START_NOT_STICKY
        }

        try {
            startAsForeground()
            Log.d(TAG, "Foreground + notificacion OK")
        } catch (e: Throwable) {
            Log.e(TAG, "startForeground FALLO", e)
            stopSelf()
            return START_NOT_STICKY
        }

        targetHost = intent.getStringExtra(EXTRA_TARGET_HOST) ?: DEFAULT_HOST
        Log.d(TAG, "targetHost = $targetHost")

        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
        val data: Intent? =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
            else
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(EXTRA_RESULT_DATA)

        if (resultCode != Activity.RESULT_OK || data == null) {
            Log.e(TAG, "resultCode/data invalidos: code=$resultCode data=${data != null}")
            stopSelf()
            return START_NOT_STICKY
        }

        try {
            startPipeline(resultCode, data)
            Log.d(TAG, "Pipeline arrancado OK")
        } catch (e: Throwable) {
            Log.e(TAG, "Pipeline FALLO", e)
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    private fun startPipeline(resultCode: Int, data: Intent) {
        Log.d(TAG, "init nativo host=$targetHost video=$TARGET_PORT feedback=$FEEDBACK_PORT")
        nativeHandle = NativeBridge.init(
            targetHost, TARGET_PORT, FEEDBACK_PORT,
            STREAM_BITRATE, BITRATE_MIN, BITRATE_MAX
        )
        Log.d(TAG, "nativeHandle=$nativeHandle")

        encoder = VideoEncoder(
            width = STREAM_WIDTH,
            height = STREAM_HEIGHT,
            fps = STREAM_FPS,
            bitrate = STREAM_BITRATE,
            handle = nativeHandle
        ).also { it.start() }
        Log.d(TAG, "encoder iniciado")

        val mgr = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mgr.getMediaProjection(resultCode, data)?.apply {
            registerCallback(object : MediaProjection.Callback() {
                override fun onStop() {
                    Log.d(TAG, "MediaProjection onStop")
                    stopSelf()
                }
            }, null)
        }
        Log.d(TAG, "projection=${projection != null}")

        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        (getSystemService(Context.WINDOW_SERVICE) as WindowManager)
            .defaultDisplay.getMetrics(metrics)

        virtualDisplay = projection?.createVirtualDisplay(
            "UltraCast",
            STREAM_WIDTH, STREAM_HEIGHT, metrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC,
            encoder!!.inputSurface, null, null
        )
        Log.d(TAG, "virtualDisplay=${virtualDisplay != null}")

        // El nativo recibe el feedback y marca cuándo el receptor pide un IDR.
        // Aquí sólo sondeamos esa marca y se lo decimos al encoder.
        running = true
        startKeyframePoll()
    }

    private fun startKeyframePoll() {
        pollThread = Thread {
            var lastKf = 0L
            var lastAbr = 0L
            var appliedBitrate = STREAM_BITRATE
            while (running) {
                try {
                    val h = nativeHandle
                    if (h != 0L && NativeBridge.pollKeyframeRequest(h)) {
                        val now = SystemClock.elapsedRealtime()
                        if (now - lastKf >= KF_THROTTLE_MS) {
                            lastKf = now
                            encoder?.requestKeyFrame()
                            Log.d(TAG, "Keyframe forzado por feedback del receptor")
                        }
                    }
                    // ABR: leer el bitrate objetivo del nativo y aplicarlo si cambió.
                    if (h != 0L) {
                        val now = SystemClock.elapsedRealtime()
                        if (now - lastAbr >= ABR_POLL_MS) {
                            lastAbr = now
                            val target = NativeBridge.pollTargetBitrate(h)
                            if (target > 0 && kotlin.math.abs(target - appliedBitrate) >= 100_000) {
                                appliedBitrate = target
                                encoder?.setBitrate(target)
                                Log.d(TAG, "ABR: bitrate -> ${target / 1000} kbps")
                            }
                        }
                    }
                    Thread.sleep(POLL_MS)
                } catch (_: InterruptedException) {
                    break
                } catch (_: Exception) {
                }
            }
        }.also { it.start() }
    }

    private fun startAsForeground() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "UltraCast", NotificationManager.IMPORTANCE_LOW)
        )
        val notif: Notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("UltraCast")
            .setContentText("Transmitiendo pantalla…")
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .build()
        startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "Service onDestroy")
        running = false
        pollThread?.interrupt()
        try { pollThread?.join(500) } catch (_: InterruptedException) {}
        pollThread = null
        virtualDisplay?.release()
        encoder?.stop()
        projection?.stop()
        if (nativeHandle != 0L) NativeBridge.close(nativeHandle)
        nativeHandle = 0L
    }
}