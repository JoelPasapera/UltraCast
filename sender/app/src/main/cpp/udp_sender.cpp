// udp_sender.cpp

#include <jni.h>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <android/log.h>
#include "packet.h"
#include "fec.h"
#include "feedback.h"

#define LOG_TAG "UltraCastNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// Reloj monotónico en microsegundos (uint32 con wrap ~71 min; sólo se usan
// diferencias, así que el wrap es inocuo). Para sellar el tiempo de envío.
static inline uint32_t mono_us() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL);
}

static const double kFecOverheadInit = 0.10; // arranque; luego se adapta solo
static const int    kFecMinParity    = 1;
static const double kOverheadMin     = 0.02; // suelo: siempre algo de paridad
static const double kOverheadMax     = 0.25; // techo (capado: con ABR, el bitrate es la
//   palanca principal ante pérdida sostenida;
//   el FEC sólo cubre la pérdida puntual y no
//   debe inflar el total y pelear con el ABR)

// EMA hacia un objetivo = pérdida medida con margen 2x + suelo.
static double fec_adapt(double cur, double loss) {
    double target = loss * 2.0 + kOverheadMin;
    if (target < kOverheadMin) target = kOverheadMin;
    if (target > kOverheadMax) target = kOverheadMax;
    return cur * 0.7 + target * 0.3;
}

// ---------------------------------------------------------------------------
//  Bitrate adaptativo (ABR) con control de congestión por RETARDO (estilo GCC).
//  El receptor distingue congestión (la cola crece -> el retardo sube) de
//  interferencia (pérdida con retardo plano) y manda ese estado en el feedback.
//  El controlador fusiona ambas señales:
//    - CONGESTIÓN  -> baja el bitrate (la causa real).
//    - INTERFERENCIA (pérdida, sin congestión) -> MANTIENE; el FEC la cubre.
//    - LIMPIO (poca pérdida, retardo plano/bajando) -> sube despacio.
//    - PÉRDIDA EXTREMA -> baja igual (respaldo de seguridad).
//  Kotlin sólo sondea el objetivo y se lo aplica al encoder en vivo. Sólo se
//  ajusta con feedback fresco: si el receptor calla, el bitrate se mantiene.
//  Ajustables:
//    - kLossCatastrophic: pérdida a partir de la cual baja aunque no haya
//      congestión (respaldo). Súbelo si en interferencia fuerte baja de más.
//    - kAbrUpStep: más alto = recupera nitidez más rápido al mejorar la red.
// ---------------------------------------------------------------------------
static const int    kCongOveruse      = 1;       // estado "congestión" que manda el receptor
static const double kLossCatastrophic = 0.20;    // pérdida extrema -> baja aunque el retardo no lo marque
static const double kAbrLossLow       = 0.01;    // pérdida (suavizada) para subir
static const int    kAbrUpStep        = 300000;  // +300 kbps por feedback (~600 kbps/s)
static const double kAbrDownFactor    = 0.80;    // x0.8 al bajar (multiplicativo)

// ---------------------------------------------------------------------------
//  Pacing: reparte en el tiempo los paquetes de un fotograma GRANDE para que la
//  ráfaga no sature el aire ni el buffer del receptor (clave en WiFi malo). Los
//  fotogramas chicos salen al instante, sin latencia añadida.
//  Si hace falta, ajusta kPaceBytesPerSec:
//    - aún se pierden keyframes en WiFi malo  -> BÁJALO (p.ej. 2.0e6 = 16 Mbps)
//    - notas un tirón al aparecer el keyframe y tu WiFi va bien -> SÚBELO
//  Y kPaceMinPackets: súbelo si se espacian demasiados fotogramas normales.
// ---------------------------------------------------------------------------
static const int    kPaceMinPackets  = 24;     // por debajo de esto: sin pacing
static const int    kPaceBatch       = 8;      // se mandan de a 8 antes de cada pausa
static const double kPaceBytesPerSec = 3.0e6;  // ~24 Mbps (~3x el bitrate de vídeo)
static const double kPaceMaxSeconds  = 0.040;  // un fotograma no se reparte en más de esto

// Espera (reloj monotónico) hasta que toque enviar este paquete según el ritmo.
static inline void pace_wait(const struct timespec& t0, long bytes_so_far, double rate) {
    double target_us = (double) bytes_so_far / rate * 1e6;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_us = (double) (now.tv_sec  - t0.tv_sec)  * 1e6
                        + (double) (now.tv_nsec - t0.tv_nsec) / 1e3;
    double wait_us = target_us - elapsed_us;
    if (wait_us > 60.0) {                         // ignora esperas minúsculas
        struct timespec ts;
        ts.tv_sec  = (time_t) (wait_us / 1e6);
        ts.tv_nsec = (long) ((wait_us - (double) ts.tv_sec * 1e6) * 1000.0);
        nanosleep(&ts, nullptr);
    }
}

struct Sender {
    int video_fd;
    int fb_fd;                       // socket de feedback (escucha al receptor)
    uint8_t* parity;
    std::atomic<double> overhead;    // sobrecarga FEC, ajustada por el hilo de feedback
    std::atomic<int>    target_bitrate; // bps; lo sondea Kotlin y se lo aplica al encoder
    int    br_min, br_max;           // límites del bitrate para el ABR
    double loss_ema;                 // pérdida suavizada (sólo lo toca el hilo de feedback)
    std::atomic<bool>   idr_requested; // pedido por el receptor, lo sondea Kotlin
    std::atomic<bool>   running;
    std::thread fb_thread;
};

// Hilo de feedback: nunca llama a Java; sólo toca estado nativo (atómicos).
static void feedback_loop(Sender* s) {
    int last_cong = -1;
    while (s->running.load()) {
        FeedbackPacket fb;
        ssize_t n = recv(s->fb_fd, &fb, sizeof(fb), 0); // con timeout para salir
        if (n < (ssize_t) sizeof(fb)) continue;
        if (ntohl(fb.magic) != kFeedbackMagic) continue;
        if (fb.request_idr) s->idr_requested.store(true);
        double loss = ntohs(fb.loss_x10) / 1000.0;
        int    cong = fb.cong_state;   // 0 normal, 1 congestión, 2 cola drenando
        if (cong != last_cong) {
            LOGI("Congestión: estado=%d (0=ok, 1=congestión, 2=drenando)", cong);
            last_cong = cong;
        }

        // ----- FEC coordinado con la CAUSA de la pérdida -----
        //   Congestión -> deja que el bitrate haga el trabajo: el FEC decae al
        //   mínimo (no inflar el total). Interferencia -> el FEC sube para tapar.
        double fec_loss = (cong == kCongOveruse) ? 0.0 : loss;
        s->overhead.store(fec_adapt(s->overhead.load(), fec_loss));

        // ----- ABR fusionado (retardo + pérdida) -----
        s->loss_ema = s->loss_ema * 0.6 + loss * 0.4;     // suaviza para subir con calma
        int br = s->target_bitrate.load();
        if (cong == kCongOveruse) {                       // CONGESTIÓN -> baja (la causa real)
            br = (int) ((double) br * kAbrDownFactor);
        } else if (loss > kLossCatastrophic) {            // pérdida extrema -> respaldo
            br = (int) ((double) br * kAbrDownFactor);
        } else if (s->loss_ema < kAbrLossLow) {           // limpio -> sube despacio
            br += kAbrUpStep;
        }                                                 // interferencia/zona muerta -> mantener
        if (br < s->br_min) br = s->br_min;
        if (br > s->br_max) br = s->br_max;
        s->target_bitrate.store(br);
    }
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_ultracast_sender_NativeBridge_init(
        JNIEnv* env, jobject /*thiz*/, jstring host, jint videoPort, jint feedbackPort,
        jint bitrateInit, jint bitrateMin, jint bitrateMax) {

    fec_init();
    const char* host_c = env->GetStringUTFChars(host, nullptr);

    // ----- Socket de vídeo (salida) -----
    int vfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (vfd < 0) { LOGE("socket vídeo"); env->ReleaseStringUTFChars(host, host_c); return 0; }
    int tos = 0xB8; setsockopt(vfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)); // DSCP EF
    int sndbuf = 1 << 20; setsockopt(vfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) videoPort);
    if (inet_pton(AF_INET, host_c, &addr.sin_addr) != 1) {
        LOGE("IP destino inválida: %s", host_c);
        env->ReleaseStringUTFChars(host, host_c); close(vfd); return 0;
    }
    env->ReleaseStringUTFChars(host, host_c);
    if (connect(vfd, (sockaddr*) &addr, sizeof(addr)) < 0) {
        LOGE("connect() vídeo"); close(vfd); return 0;
    }

    // ----- Socket de feedback (entrada del receptor) -----
    int ffd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ffd < 0) { LOGE("socket feedback"); close(vfd); return 0; }
    int one = 1; setsockopt(ffd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 500000;
    setsockopt(ffd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in fa{};
    fa.sin_family = AF_INET; fa.sin_addr.s_addr = INADDR_ANY;
    fa.sin_port = htons((uint16_t) feedbackPort);
    if (bind(ffd, (sockaddr*) &fa, sizeof(fa)) < 0) {
        LOGE("bind() feedback"); close(vfd); close(ffd); return 0;
    }

    Sender* s = new Sender();
    s->video_fd = vfd;
    s->fb_fd = ffd;
    s->parity = new uint8_t[(size_t) kFecMaxParity * kPayloadMax];
    s->overhead.store(kFecOverheadInit);
    s->target_bitrate.store(bitrateInit);
    s->br_min = bitrateMin;
    s->br_max = bitrateMax;
    s->loss_ema = 0.0;
    s->idr_requested.store(false);
    s->running.store(true);
    s->fb_thread = std::thread(feedback_loop, s);

    LOGI("Emisor listo: vídeo->%d, feedback<-%d, ABR %d-%d kbps",
         (int) videoPort, (int) feedbackPort, (int) (bitrateMin / 1000), (int) (bitrateMax / 1000));
    return reinterpret_cast<jlong>(s);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_ultracast_sender_NativeBridge_send(
        JNIEnv* env, jobject /*thiz*/,
        jlong handle, jobject buffer, jint offset, jint size,
        jboolean isConfig, jboolean isKeyFrame, jlong ptsUs, jint frameId) {

    Sender* s = reinterpret_cast<Sender*>(handle);
    if (s == nullptr || size <= 0) return;

    uint8_t* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    if (base == nullptr) { LOGE("ByteBuffer no directo"); return; }
    uint8_t* data = base + offset;

    const int P = kPayloadMax;
    int K = (size + P - 1) / P;

    // R con la sobrecarga ADAPTATIVA actual, acotada.
    double overhead = s->overhead.load();
    int R = (int) ((double) K * overhead + 0.999);
    if (R < kFecMinParity) R = kFecMinParity;
    if (R > kFecMaxParity) R = kFecMaxParity;
    if (R > 255 - K)       R = 255 - K;
    if (R < 0)             R = 0;

    // ----- Pacing del fotograma (solo si es grande) -----
    int  nPk  = K + (R > 0 ? R : 0);
    bool pace = (nPk >= kPaceMinPackets);
    long totalBytes = (long) K * kHeaderSize + size
                      + (R > 0 ? (long) R * (kHeaderSize + P) : 0);
    double paceRate = kPaceBytesPerSec;
    double minRate  = (double) totalBytes / kPaceMaxSeconds;   // respeta el tope
    if (paceRate < minRate) paceRate = minRate;
    struct timespec paceT0{};
    if (pace) clock_gettime(CLOCK_MONOTONIC, &paceT0);
    long bytesSent = 0;
    int  pktIndex  = 0;

    uint8_t flags = 0;
    if (isConfig)   flags |= FLAG_CONFIG;
    if (isKeyFrame) flags |= FLAG_KEYFRAME;

    // 1) Paquetes de DATOS.
    for (int i = 0; i < K; ++i) {
        int off = i * P;
        int chunk = size - off; if (chunk > P) chunk = P;

        PacketHeader h{};
        h.frame_id   = htonl((uint32_t) frameId);
        h.total_size = htonl((uint32_t) size);
        h.frag_index = htons((uint16_t) i);
        h.frag_count = htons((uint16_t) K);
        h.pts_us     = hton64((uint64_t) ptsUs);
        h.flags      = flags;
        h.fec_r      = (uint8_t) R;

        struct iovec iov[2];
        iov[0].iov_base = &h;         iov[0].iov_len = kHeaderSize;
        iov[1].iov_base = data + off; iov[1].iov_len = (size_t) chunk;
        struct msghdr msg{}; msg.msg_iov = iov; msg.msg_iovlen = 2;
        if (pace && (pktIndex % kPaceBatch == 0)) pace_wait(paceT0, bytesSent, paceRate);
        h.send_us = htonl(mono_us());              // sello de envío (después del pacing)
        if (sendmsg(s->video_fd, &msg, 0) < 0) LOGE("sendmsg datos %d", i);
        bytesSent += kHeaderSize + chunk;
        ++pktIndex;
    }

    // 2) Paquetes de PARIDAD (FEC).
    if (R > 0) {
        fec_encode(data, size, K, R, P, s->parity);
        uint8_t pflags = (uint8_t) (flags | FLAG_PARITY);
        for (int p = 0; p < R; ++p) {
            PacketHeader h{};
            h.frame_id   = htonl((uint32_t) frameId);
            h.total_size = htonl((uint32_t) size);
            h.frag_index = htons((uint16_t) p);
            h.frag_count = htons((uint16_t) K);
            h.pts_us     = hton64((uint64_t) ptsUs);
            h.flags      = pflags;
            h.fec_r      = (uint8_t) R;

            uint8_t* prow = s->parity + (size_t) p * P;
            struct iovec iov[2];
            iov[0].iov_base = &h;   iov[0].iov_len = kHeaderSize;
            iov[1].iov_base = prow; iov[1].iov_len = (size_t) P;
            struct msghdr msg{}; msg.msg_iov = iov; msg.msg_iovlen = 2;
            if (pace && (pktIndex % kPaceBatch == 0)) pace_wait(paceT0, bytesSent, paceRate);
            h.send_us = htonl(mono_us());          // sello de envío (después del pacing)
            if (sendmsg(s->video_fd, &msg, 0) < 0) LOGE("sendmsg paridad %d", p);
            bytesSent += kHeaderSize + P;
            ++pktIndex;
        }
    }
}

// Sondeado por Kotlin en cada fotograma: ¿el receptor pidió un IDR?
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_ultracast_sender_NativeBridge_pollKeyframeRequest(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Sender* s = reinterpret_cast<Sender*>(handle);
    if (s == nullptr) return JNI_FALSE;
    return s->idr_requested.exchange(false) ? JNI_TRUE : JNI_FALSE;
}

// Sondeado por Kotlin: bitrate objetivo actual del ABR (bps). Kotlin lo aplica
// al encoder con MediaCodec.setParameters cuando cambia.
extern "C"
JNIEXPORT jint JNICALL
Java_com_ultracast_sender_NativeBridge_pollTargetBitrate(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Sender* s = reinterpret_cast<Sender*>(handle);
    if (s == nullptr) return 0;
    return (jint) s->target_bitrate.load();
}

extern "C"
JNIEXPORT void JNICALL
Java_com_ultracast_sender_NativeBridge_close(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Sender* s = reinterpret_cast<Sender*>(handle);
    if (s == nullptr) return;
    s->running.store(false);
    if (s->fb_thread.joinable()) s->fb_thread.join();
    close(s->video_fd);
    close(s->fb_fd);
    delete[] s->parity;
    delete s;
}