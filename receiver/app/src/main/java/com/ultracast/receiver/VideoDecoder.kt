package com.ultracast.receiver

import android.media.MediaCodec
import android.media.MediaFormat
import android.view.Surface
import java.nio.ByteBuffer

class VideoDecoder(
    private val width: Int,
    private val height: Int,
    private val surface: Surface
) {
    private lateinit var codec: MediaCodec
    private val info = MediaCodec.BufferInfo()

    @Volatile var started = false
    @Volatile var codecName = "—"
    @Volatile var csdInfo = "—"
    @Volatile var inQueued = 0
    @Volatile var inDropped = 0
    @Volatile var decodedCount = 0
    @Volatile var formatChangedCount = 0
    @Volatile var outputFormat = "—"
    @Volatile var lastError = "ninguno"

    fun startWithConfig(sps: ByteArray, pps: ByteArray) {
        if (started) return
        try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(sps))
            format.setByteBuffer("csd-1", ByteBuffer.wrap(pps))
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 1024 * 1024)
            csdInfo = "SPS ${sps.size}B + PPS ${pps.size}B"

            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            codecName = try { codec.name } catch (_: Exception) { "?" }
            codec.configure(format, surface, null, 0)
            codec.start()
            started = true
        } catch (e: Exception) {
            lastError = "configure: ${e.message}"
        }
    }

    fun feedFrame(nal: ByteBuffer, size: Int, ptsUs: Long) {
        if (!started) return
        try {
            val inIndex = codec.dequeueInputBuffer(10_000)
            if (inIndex >= 0) {
                val inBuf = codec.getInputBuffer(inIndex)!!
                inBuf.clear()
                inBuf.put(nal)
                codec.queueInputBuffer(inIndex, 0, size, ptsUs, 0)
                inQueued++
            } else {
                inDropped++
            }
            drainOutput()
        } catch (e: Exception) {
            lastError = "feed: ${e.message}"
        }
    }

    private fun drainOutput() {
        while (true) {
            val outIndex = codec.dequeueOutputBuffer(info, 0)
            when {
                outIndex >= 0 -> {
                    codec.releaseOutputBuffer(outIndex, true)
                    decodedCount++
                }
                outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    formatChangedCount++
                    try {
                        val f = codec.outputFormat
                        val w = f.getInteger(MediaFormat.KEY_WIDTH, -1)
                        val h = f.getInteger(MediaFormat.KEY_HEIGHT, -1)
                        outputFormat = "${w}x${h}"
                    } catch (_: Exception) {}
                }
                else -> break
            }
        }
    }

    fun stop() {
        try {
            if (started) { codec.stop(); codec.release() }
        } catch (_: Exception) {}
    }
}