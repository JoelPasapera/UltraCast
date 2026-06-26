package com.ultracast.receiver

import java.nio.ByteBuffer

/**
 * Puente JNI hacia el receptor UDP nativo (C++). El reensamblado, el FEC y el
 * canal de vuelta (pedir IDR / reportar pérdida) ocurren todos en C++.
 */
object NativeBridge {
    init {
        System.loadLibrary("udpreceiver")
    }

    /**
     * Enlaza el socket de vídeo en [port] y prepara el feedback hacia el emisor
     * en [feedbackPort]. Devuelve un handle (puntero nativo).
     */
    external fun init(port: Int, feedbackPort: Int): Long

    /** ByteBuffer DIRECTO sobre el buffer de reensamblado. Llamar UNA vez y cachear. */
    external fun getBuffer(handle: Long): ByteBuffer

    /** Bloquea hasta tener un fotograma completo (reconstruyendo con FEC si hace falta). */
    external fun receiveFrame(handle: Long): Int

    /** Flags del último fotograma (bit0=config, bit1=keyframe). */
    external fun getFlags(handle: Long): Int

    /** Timestamp de presentación (µs) del último fotograma. */
    external fun getPts(handle: Long): Long

    /**
     * Estado de la aceleración SIMD del FEC (no necesita handle):
     *   1 = NEON activo, 0 = NEON desactivado por auto-test, -1 = ABI sin NEON.
     */
    external fun fecSimd(): Int

    /** Desbloquea el receptor para poder cerrarlo. */
    external fun stop(handle: Long)

    /** Cierra el socket y libera el handle. */
    external fun close(handle: Long)
}