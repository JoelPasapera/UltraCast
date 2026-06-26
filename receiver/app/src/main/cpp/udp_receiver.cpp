// file: udp_receiver.cpp
// path: app\src\main\cpp\udp_receiver.cpp

#include <jni.h>
#include <cstring>
#include <cstdint>
#include <vector>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <android/log.h>
#include "packet.h"
#include "fec.h"
#include "feedback.h"

#define LOG_TAG "UltraCastRecv"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

static const int kMaxFrameBytes = 4 * 1024 * 1024;
static const int kMaxFrags = (kMaxFrameBytes / kPayloadMax) + 1;
static const long kReportMs = 500;   // cadencia de informes de pérdida

// ---- sensado de congestión por retardo (estilo GCC) ----
static const int    kTrendWindow   = 25;     // grupos en la ventana de regresión
static const int    kTrendMin      = 10;     // mínimo de puntos antes de confiar
static const int    kBurstUs       = 5000;   // 5 ms: ventana de agrupado por tiempo de envío
static const int    kMaxGapUs      = 250000; // descarta saltos > 250 ms (stalls/reinicios)
static const double kSlopeThreshUs = 300.0;  // us de retardo/grupo que marca congestión
static const int    kOveruseGroups = 3;      // persistencia antes de declarar congestión

struct Receiver {
    int fd;
    volatile bool running;

    uint8_t* assembly;
    std::vector<uint8_t> data_present;
    uint8_t* parity;
    uint8_t  parity_present[kFecMaxParity];

    bool     have_frame, completed;
    uint32_t cur_frame_id;
    int      K, R, total_size;
    int      data_received, parity_received;
    uint8_t  cur_flags;
    uint64_t cur_pts;
    int      last_size; uint8_t last_flags; uint64_t last_pts;

    // ---- canal de vuelta ----
    int        feedback_port;
    sockaddr_in sender_addr;       // a dónde mandar feedback (IP del emisor : feedback_port)
    bool       sender_known;
    long       sum_expected;       // ventana de estimación de pérdida
    long       sum_received;
    long       last_report_ms;
    bool       pending_idr;        // hay que pedir un IDR en el próximo envío

    // ---- sensado de congestión por retardo (estilo GCC) ----
    bool     dl_grp_open;
    uint32_t dl_grp_first_send, dl_grp_last_send;  // grupo actual (tiempo de envío del emisor)
    int64_t  dl_grp_last_arr;                       // llegada (us, reloj receptor) del último
    bool     dl_have_prev;
    uint32_t dl_prev_send;                          // grupo previo (ya cerrado)
    int64_t  dl_prev_arr;
    long     dl_group_count;                        // eje x de la regresión (monotónico)
    double   dl_acc;                                // retardo acumulado (us)
    double   dl_x[kTrendWindow], dl_y[kTrendWindow];
    int      dl_n, dl_head, dl_overuse;
    int      cong_state;                            // 0 normal, 1 congestión, 2 drenando
    bool     cong_report_now;                       // avisar al emisor al entrar en congestión

    uint8_t  recvbuf[2048];
};

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Micros monotónicos en 64 bits (no desborda ni en builds de 32 bits).
static int64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static int loss_x10_of(Receiver* r) {
    if (r->sum_expected <= 0) return 0;
    long lost = r->sum_expected - r->sum_received;
    if (lost < 0) lost = 0;
    long x = lost * 1000 / r->sum_expected; // en 0.1%
    if (x > 1000) x = 1000;
    return (int) x;
}

static void send_feedback(Receiver* r, int request_idr, int loss_x10, int cong_state) {
    if (!r->sender_known) return;
    FeedbackPacket fb{};
    fb.magic       = htonl(kFeedbackMagic);
    fb.request_idr = (uint8_t) (request_idr ? 1 : 0);
    fb.cong_state  = (uint8_t) cong_state;
    fb.loss_x10    = htons((uint16_t) loss_x10);
    sendto(r->fd, &fb, sizeof(fb), 0,
           (sockaddr*) &r->sender_addr, sizeof(r->sender_addr));
}

// Sensado de congestión por retardo (estilo Google Congestion Control).
// Agrupa los paquetes por su tiempo de ENVÍO (~5 ms) y mide, por regresión, la
// PENDIENTE del retardo acumulado: si las llegadas se separan más que los envíos
// (pendiente +), la cola crece -> CONGESTIÓN. Plano (aunque haya pérdida) =
// interferencia. Como sólo se usan diferencias, el desfase entre relojes se
// cancela. No toca el camino de datos (assembly/FEC).
static void delay_sense(Receiver* r, uint32_t send_us, int64_t arr_us) {
    if (!r->dl_grp_open) {
        r->dl_grp_open = true;
        r->dl_grp_first_send = send_us;
        r->dl_grp_last_send  = send_us;
        r->dl_grp_last_arr   = arr_us;
        return;
    }
    int32_t since = (int32_t) (send_us - r->dl_grp_first_send);   // wrap-safe
    if (since >= 0 && since <= kBurstUs) {                        // mismo grupo: extender
        if ((int32_t) (send_us - r->dl_grp_last_send) >= 0) r->dl_grp_last_send = send_us;
        if (arr_us > r->dl_grp_last_arr) r->dl_grp_last_arr = arr_us;
        return;
    }
    if (since < 0) return;                                        // reordenado/viejo: ignorar

    // ---- cerrar el grupo actual y compararlo con el previo ----
    if (r->dl_have_prev) {
        int32_t d_send = (int32_t) (r->dl_grp_last_send - r->dl_prev_send);
        int64_t d_arr  = r->dl_grp_last_arr - r->dl_prev_arr;
        if (d_send > 0 && d_send < kMaxGapUs && d_arr >= 0 && d_arr < kMaxGapUs) {
            double delta = (double) d_arr - (double) d_send;     // variación de retardo del grupo
            r->dl_acc += delta;
            r->dl_group_count++;
            r->dl_y[r->dl_head] = r->dl_acc;
            r->dl_x[r->dl_head] = (double) r->dl_group_count;
            r->dl_head = (r->dl_head + 1) % kTrendWindow;
            if (r->dl_n < kTrendWindow) r->dl_n++;
            if (r->dl_n >= kTrendMin) {                          // pendiente por mínimos cuadrados
                double mx = 0, my = 0;
                for (int i = 0; i < r->dl_n; ++i) { mx += r->dl_x[i]; my += r->dl_y[i]; }
                mx /= r->dl_n; my /= r->dl_n;
                double num = 0, den = 0;
                for (int i = 0; i < r->dl_n; ++i) {
                    double dx = r->dl_x[i] - mx;
                    num += dx * (r->dl_y[i] - my);
                    den += dx * dx;
                }
                double slope = (den > 0) ? num / den : 0.0;      // us de retardo por grupo
                int prev = r->cong_state;
                if (slope > kSlopeThreshUs) {
                    if (++r->dl_overuse >= kOveruseGroups) r->cong_state = 1;  // congestión
                } else if (slope < -kSlopeThreshUs) {
                    r->cong_state = 2; r->dl_overuse = 0;                      // drenando
                } else {
                    r->cong_state = 0; r->dl_overuse = 0;                      // normal
                }
                if (r->cong_state == 1 && prev != 1) r->cong_report_now = true;
            }
        }
    }
    r->dl_prev_send = r->dl_grp_last_send;
    r->dl_prev_arr  = r->dl_grp_last_arr;
    r->dl_have_prev = true;
    r->dl_grp_first_send = send_us;                              // arrancar nuevo grupo
    r->dl_grp_last_send  = send_us;
    r->dl_grp_last_arr   = arr_us;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_ultracast_receiver_NativeBridge_init(
        JNIEnv* env, jobject /*thiz*/, jint port, jint feedbackPort) {

    fec_init();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOGE("socket"); return 0; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rcvbuf = 1 << 20; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t) port);
    if (bind(fd, (sockaddr*) &addr, sizeof(addr)) < 0) {
        LOGE("bind() %d", (int) port); close(fd); return 0;
    }

    Receiver* r = new Receiver();
    r->fd = fd; r->running = true;
    r->assembly = new uint8_t[kMaxFrameBytes];
    r->data_present.assign(kMaxFrags, 0);
    r->parity = new uint8_t[(size_t) kFecMaxParity * kPayloadMax];
    memset(r->parity_present, 0, sizeof(r->parity_present));
    r->have_frame = false; r->completed = false; r->cur_frame_id = 0;
    r->K = 0; r->R = 0; r->total_size = 0;
    r->data_received = 0; r->parity_received = 0;
    r->cur_flags = 0; r->cur_pts = 0;
    r->last_size = 0; r->last_flags = 0; r->last_pts = 0;

    r->feedback_port = feedbackPort;
    memset(&r->sender_addr, 0, sizeof(r->sender_addr));
    r->sender_known = false;
    r->sum_expected = 0; r->sum_received = 0;
    r->last_report_ms = now_ms();
    r->pending_idr = true;          // pediremos IDR en cuanto sepamos al emisor

    // estado del sensado de congestión por retardo
    r->dl_grp_open = false; r->dl_have_prev = false;
    r->dl_group_count = 0; r->dl_acc = 0.0;
    r->dl_n = 0; r->dl_head = 0; r->dl_overuse = 0;
    r->cong_state = 0; r->cong_report_now = false;

    LOGI("Receptor: vídeo<-%d, feedback->%d", (int) port, (int) feedbackPort);
    return reinterpret_cast<jlong>(r);
}

extern "C"
JNIEXPORT jobject JNICALL
Java_com_ultracast_receiver_NativeBridge_getBuffer(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Receiver* r = reinterpret_cast<Receiver*>(handle);
    if (r == nullptr) return nullptr;
    return env->NewDirectByteBuffer(r->assembly, kMaxFrameBytes);
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_ultracast_receiver_NativeBridge_receiveFrame(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {

    Receiver* r = reinterpret_cast<Receiver*>(handle);
    if (r == nullptr) return -1;
    const int P = kPayloadMax;

    while (r->running) {
        sockaddr_in src{}; socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(r->fd, r->recvbuf, sizeof(r->recvbuf), 0,
                             (sockaddr*) &src, &slen);
        if (n < (ssize_t) kHeaderSize) continue; // timeout o basura

        PacketHeader h;
        memcpy(&h, r->recvbuf, kHeaderSize);
        uint32_t frame_id   = ntohl(h.frame_id);
        int      tsize      = (int) ntohl(h.total_size);
        uint16_t frag_index = ntohs(h.frag_index);
        uint16_t frag_count = ntohs(h.frag_count);
        uint64_t pts        = ntoh64(h.pts_us);
        uint8_t  flags      = h.flags;
        int      Rhdr       = h.fec_r;
        uint32_t send_us    = ntohl(h.send_us);
        bool     is_parity  = (flags & FLAG_PARITY) != 0;

        int payload_len = (int) n - kHeaderSize;
        const uint8_t* payload = r->recvbuf + kHeaderSize;

        // Aprender al emisor en el primer paquete y pedir IDR ya (HELLO).
        if (!r->sender_known) {
            r->sender_addr = src;
            r->sender_addr.sin_port = htons((uint16_t) r->feedback_port);
            r->sender_known = true;
            send_feedback(r, 1, 0, r->cong_state);
            r->pending_idr = false;
        }

        if (frag_count == 0 || frag_count > kMaxFrags) continue;
        int Rcap = (Rhdr > kFecMaxParity) ? kFecMaxParity : Rhdr;

        // ---- Sensado de congestión por retardo (no toca el camino de datos) ----
        delay_sense(r, send_us, now_us());
        if (r->cong_report_now) {     // entró en congestión -> avisar cuanto antes
            send_feedback(r, 0, loss_x10_of(r), r->cong_state);
            r->cong_report_now = false;
        }

        // ---- ¿Fotograma nuevo? ----
        if (!r->have_frame || frame_id > r->cur_frame_id) {
            if (r->have_frame) {
                // Contabilizar el fotograma anterior para estimar la pérdida.
                r->sum_expected += (long) (r->K + r->R);
                r->sum_received += (long) (r->data_received + r->parity_received);
                if (!r->completed) {
                    // Se abandonó incompleto -> pedir IDR ya mismo.
                    r->pending_idr = true;
                    send_feedback(r, 1, loss_x10_of(r), r->cong_state);
                }
            }
            r->have_frame = true; r->completed = false;
            r->cur_frame_id = frame_id;
            r->K = frag_count; r->R = Rcap; r->total_size = tsize;
            r->data_received = 0; r->parity_received = 0;
            r->cur_flags = (uint8_t) (flags & ~FLAG_PARITY);
            r->cur_pts = pts;
            memset(r->data_present.data(), 0, (size_t) r->K);
            if (r->R > 0) memset(r->parity_present, 0, (size_t) r->R);
        } else if (frame_id < r->cur_frame_id) {
            continue;
        } else if (r->completed) {
            continue;
        }

        // ---- Informe periódico de pérdida ----
        long t = now_ms();
        if (t - r->last_report_ms >= kReportMs && r->sum_expected > 0) {
            send_feedback(r, r->pending_idr ? 1 : 0, loss_x10_of(r), r->cong_state);
            r->pending_idr = false;
            r->sum_expected = 0; r->sum_received = 0;
            r->last_report_ms = t;
        }

        // ---- Guardar el paquete ----
        if (!is_parity) {
            int i = frag_index;
            if (i >= r->K || r->data_present[i]) continue;
            int off = i * P;
            if (off + payload_len > kMaxFrameBytes) continue;
            memcpy(r->assembly + off, payload, (size_t) payload_len);
            if (payload_len < P)
                memset(r->assembly + off + payload_len, 0, (size_t) (P - payload_len));
            r->data_present[i] = 1; r->data_received++;
        } else {
            int p = frag_index;
            if (p >= r->R || r->parity_present[p]) continue;
            int plen = payload_len; if (plen > P) plen = P;
            uint8_t* prow = r->parity + (size_t) p * P;
            memcpy(prow, payload, (size_t) plen);
            if (plen < P) memset(prow + plen, 0, (size_t) (P - plen));
            r->parity_present[p] = 1; r->parity_received++;
        }

        // ---- ¿Fotograma listo? ----
        if (r->data_received == r->K) {
            r->completed = true;
            r->last_size = r->total_size; r->last_flags = r->cur_flags; r->last_pts = r->cur_pts;
            return r->last_size;
        }
        if (r->R > 0 && (r->data_received + r->parity_received) >= r->K
            && r->data_received < r->K) {
            int rc = fec_decode(r->assembly, P, r->K, r->R,
                                r->data_present.data(), r->parity_present, r->parity);
            if (rc == 0) {
                r->completed = true;
                r->last_size = r->total_size; r->last_flags = r->cur_flags; r->last_pts = r->cur_pts;
                return r->last_size;
            }
        }
    }
    return -1;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_ultracast_receiver_NativeBridge_getFlags(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Receiver* r = reinterpret_cast<Receiver*>(handle);
    return (r == nullptr) ? 0 : (jint) r->last_flags;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_ultracast_receiver_NativeBridge_getPts(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Receiver* r = reinterpret_cast<Receiver*>(handle);
    return (r == nullptr) ? 0 : (jlong) r->last_pts;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_ultracast_receiver_NativeBridge_stop(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Receiver* r = reinterpret_cast<Receiver*>(handle);
    if (r == nullptr) return;
    r->running = false;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_ultracast_receiver_NativeBridge_close(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    Receiver* r = reinterpret_cast<Receiver*>(handle);
    if (r == nullptr) return;
    close(r->fd);
    delete[] r->assembly;
    delete[] r->parity;
    delete r;
}