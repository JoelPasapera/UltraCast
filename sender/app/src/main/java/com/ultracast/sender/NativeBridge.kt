package com.ultracast.sender

import java.nio.ByteBuffer

/**
 * Puente JNI hacia la capa nativa de red (C++).
 * El envío recibe un ByteBuffer DIRECTO para que C++ lea el puntero sin copiar.
 */
object NativeBridge {
    init {
        System.loadLibrary("udpsender")
    }

    /**
     * Abre el socket de vídeo (salida) y el de feedback (entrada del receptor).
     * bitrateInit/Min/Max definen el rango del ABR (controlador AIMD en el nativo).
     * Devuelve un handle (puntero nativo).
     */
    external fun init(
        host: String,
        videoPort: Int,
        feedbackPort: Int,
        bitrateInit: Int,
        bitrateMin: Int,
        bitrateMax: Int
    ): Long

    /** Envía una unidad NAL (el nativo la trocea en paquetes <= MTU + paridad FEC). */
    external fun send(
        handle: Long,
        buffer: ByteBuffer,
        offset: Int,
        size: Int,
        isConfig: Boolean,
        isKeyFrame: Boolean,
        ptsUs: Long,
        frameId: Int
    )

    /** ¿El receptor pidió un fotograma clave? (lo consume; true sólo una vez por pedido). */
    external fun pollKeyframeRequest(handle: Long): Boolean

    /** Bitrate objetivo actual del ABR (bps). Kotlin lo aplica al encoder si cambió. */
    external fun pollTargetBitrate(handle: Long): Int

    /** Cierra los sockets y libera el handle. */
    external fun close(handle: Long)
}