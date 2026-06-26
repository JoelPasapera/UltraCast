package com.ultracast.sender

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Codificador H.264 por hardware en modo baja latencia.
 *  - Entrada = un Surface (la pantalla se dibuja ahí sola, sin pasar por la CPU).
 *  - Salida  = NAL -> capa nativa por un ByteBuffer DIRECTO (sin copias).
 *
 * Para que un receptor que se conecta TARDE pueda engancharse, cacheamos el
 * config (SPS/PPS) y lo reenviamos justo antes de cada fotograma clave (salen
 * cada 1s). Así el receptor recibe el "manual" + un IDR cada segundo.
 */
class VideoEncoder(
    private val width: Int,
    private val height: Int,
    private val fps: Int,
    private val bitrate: Int,
    private val handle: Long
) {
    lateinit var inputSurface: Surface
        private set

    private lateinit var codec: MediaCodec
    private var frameId: Int = 0
    private var cachedConfig: ByteBuffer? = null

    fun start() {
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE,
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
        }

        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)

        codec.setCallback(object : MediaCodec.Callback() {
            override fun onOutputBufferAvailable(
                mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo
            ) {
                val buf: ByteBuffer? = mc.getOutputBuffer(index)
                if (buf != null && info.size > 0) {
                    val isConfig = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                    val isKey = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0

                    if (isConfig) {
                        // Guardar el config (SPS/PPS) en un buffer DIRECTO para reenviarlo.
                        val copy = ByteBuffer.allocateDirect(info.size)
                        val src = buf.duplicate()
                        src.position(info.offset)
                        src.limit(info.offset + info.size)
                        copy.put(src)
                        copy.position(0)
                        cachedConfig = copy
                    } else if (isKey) {
                        // Reenviar el config justo antes de cada keyframe (para late-join).
                        cachedConfig?.let { cfg ->
                            cfg.position(0)
                            NativeBridge.send(
                                handle, cfg, 0, cfg.capacity(),
                                true, false, info.presentationTimeUs, frameId
                            )
                            frameId++
                        }
                    }

                    NativeBridge.send(
                        handle, buf, info.offset, info.size,
                        isConfig, isKey, info.presentationTimeUs, frameId
                    )
                    frameId++
                }
                mc.releaseOutputBuffer(index, false)
            }

            override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {}

            override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
                e.printStackTrace()
            }

            override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {}
        })

        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = codec.createInputSurface()
        codec.start()
    }

    fun requestKeyFrame() {
        val params = Bundle().apply {
            putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
        }
        codec.setParameters(params)
    }

    /**
     * Cambia el bitrate objetivo del encoder EN CALIENTE, sin reiniciar (lo usa el
     * ABR). MediaCodec ajusta la calidad a partir del siguiente fotograma; no hay
     * corte ni glitch.
     */
    fun setBitrate(bps: Int) {
        try {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps)
            }
            codec.setParameters(params)
        } catch (_: Exception) {}
    }

    fun stop() {
        try {
            codec.stop()
            codec.release()
        } catch (_: Exception) {}
        try { inputSurface.release() } catch (_: Exception) {}
    }
}