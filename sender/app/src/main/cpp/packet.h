// file: packet.h
// path: app\src\main\cpp\packet.h

#pragma once
#include <cstdint>
#include <arpa/inet.h>

// ===========================================================================
//  Formato de paquete COMPARTIDO por emisor y receptor.
//  Si cambias algo aquí, recompila AMBOS módulos.
// ===========================================================================

static const int kPacketMax = 1400;          // MTU conservadora (cabecera + payload)

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t frame_id;     // id incremental por CADA NAL (datos y paridad de un
    //   fotograma comparten frame_id)
    uint32_t total_size;   // bytes del NAL completo (para recortar y para FEC).
    //   Va en todos los paquetes -> el receptor lo sabe
    //   aunque pierda algunos.
    uint16_t frag_index;   // datos: 0..K-1   |   paridad: 0..R-1
    uint16_t frag_count;   // K = nº de fragmentos de DATOS del fotograma
    uint64_t pts_us;       // timestamp de presentación (microsegundos)
    uint8_t  flags;        // bit0 = config (SPS/PPS), bit1 = keyframe, bit2 = PARIDAD
    uint8_t  fec_r;        // R = nº de paquetes de paridad del fotograma
    uint32_t send_us;      // reloj monotónico del EMISOR al enviar (us, uint32 con
    //   wrap ~71 min). Sólo se usan DIFERENCIAS, así que el
    //   desfase entre relojes se cancela. Para el sensado de
    //   congestión por retardo (estilo GCC) en el receptor.
};
#pragma pack(pop)

static const int kHeaderSize = (int) sizeof(PacketHeader);   // 26 bytes
static const int kPayloadMax = kPacketMax - kHeaderSize;     // 1374 bytes

static const uint8_t FLAG_CONFIG   = 0x01;
static const uint8_t FLAG_KEYFRAME = 0x02;
static const uint8_t FLAG_PARITY   = 0x04;

// htonll / ntohll portables (Android es little-endian, pero lo dejamos correcto).
static inline uint64_t hton64(uint64_t v) {
    uint32_t hi = htonl((uint32_t) (v >> 32));
    uint32_t lo = htonl((uint32_t) (v & 0xFFFFFFFFULL));
    return ((uint64_t) lo << 32) | hi;
}
static inline uint64_t ntoh64(uint64_t v) { return hton64(v); }