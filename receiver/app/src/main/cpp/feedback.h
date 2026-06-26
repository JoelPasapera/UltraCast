// file: feedback.h
// path: app\src\main\cpp\feedback.h

#pragma once
#include <cstdint>
#include <arpa/inet.h>

// ===========================================================================
//  Paquete de FEEDBACK: del receptor al emisor (sentido inverso al vídeo).
//  COMPARTIDO por emisor y receptor.
// ===========================================================================

static const uint32_t kFeedbackMagic = 0xFEEDBAC1u; // para filtrar basura

#pragma pack(push, 1)
struct FeedbackPacket {
    uint32_t magic;        // kFeedbackMagic
    uint8_t  request_idr;  // 1 = envía un fotograma clave (IDR) YA
    uint8_t  cong_state;   // sensado de congestión por retardo (estilo GCC):
    //   0 = normal, 1 = congestión (retardo subiendo, la
    //   cola crece), 2 = la cola se está drenando.
    uint16_t loss_x10;     // pérdida medida en 0.1% (0..1000 = 0..100%)
};
#pragma pack(pop)