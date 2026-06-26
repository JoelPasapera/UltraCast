#include "fec.h"
#include <cstring>
#include <utility>
#include <android/log.h>

#define FEC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "UltraCastFEC", __VA_ARGS__)

// vqtbl1q_u8 es exclusivo de AArch64 (ARMv8 64-bit). En esa ABI NEON es base.
// Cualquier otra ABI (armeabi-v7a 32-bit, x86, etc.) usa la ruta escalar.
#if defined(__aarch64__)
#  include <arm_neon.h>
#  define FEC_HAVE_NEON 1
#else
#  define FEC_HAVE_NEON 0
#endif

// ---------------------------------------------------------------------------
//  Aritmética en GF(2^8)
//  Polinomio primitivo 0x11D (x^8 + x^4 + x^3 + x^2 + 1), generador 0x02.
// ---------------------------------------------------------------------------
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool    g_neon_ok = false;   // true solo si NEON se verificó bit-a-bit

static inline uint8_t gmul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}
static inline uint8_t ginv(uint8_t a) {           // a != 0
    return gf_exp[255 - gf_log[a]];
}

// ---------------------------------------------------------------------------
//  Primitiva núcleo:  dst[0..len) ^= c * src[0..len)   en GF(2^8)
//  Es exactamente la operación que repiten el emisor (paridad) y el receptor
//  (reconstrucción). Vectorizarla acelera AMBOS.
// ---------------------------------------------------------------------------

// Referencia escalar: siempre correcta, sirve de respaldo y de patrón de test.
static void muladd_scalar(uint8_t* dst, const uint8_t* src, uint8_t c, int len) {
    if (c == 0) return;
    if (c == 1) { for (int j = 0; j < len; ++j) dst[j] = (uint8_t)(dst[j] ^ src[j]); return; }
    for (int j = 0; j < len; ++j) dst[j] = (uint8_t)(dst[j] ^ gmul(c, src[j]));
}

#if FEC_HAVE_NEON
// NEON: 16 bytes por iteración usando tablas de medio-byte (nibble) + vqtbl1q_u8.
//   Para un byte b = (hi<<4) ^ lo (los nibbles no comparten bits):
//     c*b = c*(hi<<4) ^ c*lo = thi[hi] ^ tlo[lo]
//   con tlo[i] = c*i  y  thi[i] = c*(i<<4),  i = 0..15.
static void muladd_neon(uint8_t* dst, const uint8_t* src, uint8_t c, int len) {
    if (c == 0) return;
    int j = 0;
    if (c == 1) {                                   // c==1  ->  XOR puro
        for (; j + 16 <= len; j += 16) {
            uint8x16_t s = vld1q_u8(src + j);
            uint8x16_t d = vld1q_u8(dst + j);
            vst1q_u8(dst + j, veorq_u8(d, s));
        }
        for (; j < len; ++j) dst[j] = (uint8_t)(dst[j] ^ src[j]);
        return;
    }
    uint8_t lo[16], hi[16];
    for (int i = 0; i < 16; ++i) {
        lo[i] = gmul(c, (uint8_t) i);
        hi[i] = gmul(c, (uint8_t) (i << 4));
    }
    const uint8x16_t tlo  = vld1q_u8(lo);
    const uint8x16_t thi  = vld1q_u8(hi);
    const uint8x16_t mask = vdupq_n_u8(0x0F);
    for (; j + 16 <= len; j += 16) {
        uint8x16_t v    = vld1q_u8(src + j);
        uint8x16_t vl   = vandq_u8(v, mask);        // nibble bajo
        uint8x16_t vh   = vshrq_n_u8(v, 4);         // nibble alto
        uint8x16_t prod = veorq_u8(vqtbl1q_u8(tlo, vl), vqtbl1q_u8(thi, vh));
        uint8x16_t d    = vld1q_u8(dst + j);
        vst1q_u8(dst + j, veorq_u8(d, prod));
    }
    for (; j < len; ++j) dst[j] = (uint8_t)(dst[j] ^ gmul(c, src[j]));  // cola < 16
}
#endif

// Despacho: NEON si quedó verificado; si no, escalar. Mismo resultado siempre.
static inline void muladd(uint8_t* dst, const uint8_t* src, uint8_t c, int len) {
#if FEC_HAVE_NEON
    if (g_neon_ok) { muladd_neon(dst, src, c, len); return; }
#endif
    muladd_scalar(dst, src, c, len);
}

// Auto-verificación (una sola vez): NEON debe coincidir EXACTAMENTE con escalar
// sobre datos aleatorios y longitudes variadas (incluida la cola < 16 bytes).
// Si discrepa aunque sea en un byte, NEON queda desactivado -> nunca corrompe.
static void fec_selftest() {
#if FEC_HAVE_NEON
    const int N = 256;
    uint8_t a[N], ref[N], got[N];
    uint32_t seed = 0x1234567u;
    auto rnd = [&]() -> uint8_t { seed = seed * 1664525u + 1013904223u; return (uint8_t)(seed >> 24); };
    bool ok = true;
    for (int t = 0; t < 64 && ok; ++t) {
        uint8_t c = rnd();
        for (int i = 0; i < N; ++i) { a[i] = rnd(); uint8_t base = rnd(); ref[i] = base; got[i] = base; }
        int len = 1 + (int)(rnd() % (N - 1));       // longitud variable -> ejercita la cola
        muladd_scalar(ref, a, c, len);
        muladd_neon  (got, a, c, len);
        for (int i = 0; i < len; ++i) if (ref[i] != got[i]) { ok = false; break; }
    }
    g_neon_ok = ok;
    FEC_LOG("%s", ok ? "NEON verificado y ACTIVO" : "NEON discrepa -> uso ESCALAR");
#else
    g_neon_ok = false;
    FEC_LOG("Sin NEON en esta ABI -> uso ESCALAR");
#endif
}

// Construcción de tablas + auto-test. Se ejecuta UNA sola vez.
static bool fec_build_once() {
    int x = 1;
    for (int i = 0; i < 255; ++i) {
        gf_exp[i] = (uint8_t) x;
        gf_log[x] = (uint8_t) i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; ++i) gf_exp[i] = gf_exp[i - 255]; // sin módulo en mul
    gf_log[0] = 0; // no se usa
    fec_selftest();
    return true;
}

void fec_init() {
    // Inicialización segura entre hilos (garantía de C++11 para estáticos locales):
    // ahora fec_init lo pueden llamar a la vez el hilo de red y el de la interfaz.
    [[maybe_unused]] static bool ready = fec_build_once();
}

uint8_t fec_coef(int p, int d, int K) {
    // Matriz de Cauchy: x_p = K+p, y_d = d (todos distintos, K+R <= 255).
    // C[p][d] = 1 / (x_p XOR y_d). El XOR nunca es 0 porque K+p >= K > d.
    uint8_t v = (uint8_t) ((K + p) ^ d);
    return ginv(v);
}

// ---------------------------------------------------------------------------
//  EMISOR: paridad = matriz de Cauchy * datos
// ---------------------------------------------------------------------------
void fec_encode(const uint8_t* data, int size, int K, int R, int P, uint8_t* parity) {
    fec_init();
    memset(parity, 0, (size_t) R * P);
    for (int p = 0; p < R; ++p) {
        uint8_t* prow = parity + (size_t) p * P;
        for (int d = 0; d < K; ++d) {
            uint8_t c = fec_coef(p, d, K);
            if (c == 0) continue;
            int avail = size - d * P;            // bytes reales de este fragmento
            if (avail > P) avail = P;
            if (avail <= 0) continue;            // (no debería pasar para d < K)
            // prow ^= c * data[d]; los bytes más allá de `avail` son 0 (no se tocan).
            muladd(prow, data + (size_t) d * P, c, avail);
        }
    }
}

// ---------------------------------------------------------------------------
//  RECEPTOR: reconstrucción por borrado
//  Sabemos QUÉ fragmentos faltan (no son errores, son borrones), así que basta
//  resolver un sistema lineal m x m sobre GF(2^8), con m = nº de datos perdidos.
// ---------------------------------------------------------------------------
int fec_decode(uint8_t* assembly, int P, int K, int R,
               const uint8_t* data_present,
               const uint8_t* parity_present,
               const uint8_t* parity) {
    fec_init();
    if (K > 255 || R > kFecMaxParity) return -1; // fuera del rango soportado

    // 1) Índices de datos que faltan.
    int missing[256]; int m = 0;
    for (int d = 0; d < K; ++d)
        if (!data_present[d]) missing[m++] = d;
    if (m == 0) return 0;                 // nada que reconstruir
    if (m > kFecMaxParity) return -1;

    // 2) Paquetes de paridad disponibles: necesitamos al menos m.
    int pidx[kFecMaxParity]; int pc = 0;
    for (int p = 0; p < R && pc < m; ++p)
        if (parity_present[p]) pidx[pc++] = p;
    if (pc < m) return -1;                // pérdidas > paridad: irrecuperable

    // 3) Precalcular coeficientes de las m filas de paridad elegidas (m x K).
    static const int MP = kFecMaxParity;
    uint8_t coefMat[MP * 256];
    for (int i = 0; i < m; ++i)
        for (int d = 0; d < K; ++d)
            coefMat[i * K + d] = fec_coef(pidx[i], d, K);

    // 4) Matriz A (m x m): A[i][j] = coef(parity_i, missing_j).
    uint8_t A[MP * MP];
    uint8_t inv[MP * MP];
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < m; ++j) {
            A[i * m + j]   = coefMat[i * K + missing[j]];
            inv[i * m + j] = (i == j) ? 1 : 0;
        }
    }

    // 5) Invertir A por Gauss-Jordan sobre GF(2^8).
    //    m <= 20  ->  O(m^3) ~ unos pocos miles de ops UNA vez por frame: trivial,
    //    se deja escalar. El coste real está en el paso 6 (m^2 * P), ese va con NEON.
    for (int col = 0; col < m; ++col) {
        int piv = -1;
        for (int r = col; r < m; ++r) if (A[r * m + col] != 0) { piv = r; break; }
        if (piv < 0) return -1;           // singular (no ocurre con Cauchy)
        if (piv != col) {
            for (int j = 0; j < m; ++j) {
                std::swap(A[col * m + j],   A[piv * m + j]);
                std::swap(inv[col * m + j], inv[piv * m + j]);
            }
        }
        uint8_t ipv = ginv(A[col * m + col]);
        for (int j = 0; j < m; ++j) {
            A[col * m + j]   = gmul(A[col * m + j],   ipv);
            inv[col * m + j] = gmul(inv[col * m + j], ipv);
        }
        for (int r = 0; r < m; ++r) {
            if (r == col) continue;
            uint8_t f = A[r * m + col];
            if (f == 0) continue;
            for (int j = 0; j < m; ++j) {
                A[r * m + j]   = (uint8_t) (A[r * m + j]   ^ gmul(f, A[col * m + j]));
                inv[r * m + j] = (uint8_t) (inv[r * m + j] ^ gmul(f, inv[col * m + j]));
            }
        }
    }
    // inv = A^{-1}

    // 6) Reconstrucción vectorizada, por BLOQUES de columnas (cache-friendly en
    //    la L1 del A35). Matemáticamente idéntico al bucle byte-a-byte original:
    //      rhs[i] = parity[pidx[i]]  XOR  sum_{d presente} coef(i,d) * assembly[d]
    //      assembly[missing[k]] = sum_i inv[k][i] * rhs[i]
    //    s_rhs vive en BSS (no es asignación dinámica en el hot path) y el decode
    //    es de un solo hilo, así que es seguro reutilizarlo.
    static const int TILE = 512;
    static uint8_t s_rhs[MP * TILE];

    for (int c0 = 0; c0 < P; c0 += TILE) {
        int w = (P - c0 < TILE) ? (P - c0) : TILE;          // ancho del bloque

        // A) filas RHS de este bloque
        for (int i = 0; i < m; ++i) {
            uint8_t* rr = s_rhs + (size_t) i * TILE;
            memcpy(rr, parity + (size_t) pidx[i] * P + c0, (size_t) w);
            for (int d = 0; d < K; ++d) {
                if (!data_present[d]) continue;
                uint8_t coef = coefMat[i * K + d];
                if (coef == 0) continue;
                muladd(rr, assembly + (size_t) d * P + c0, coef, w);
            }
        }

        // B) resolver los fragmentos perdidos de este bloque
        for (int k = 0; k < m; ++k) {
            uint8_t* out = assembly + (size_t) missing[k] * P + c0;
            memset(out, 0, (size_t) w);
            for (int i = 0; i < m; ++i) {
                uint8_t coef = inv[k * m + i];
                if (coef == 0) continue;
                muladd(out, s_rhs + (size_t) i * TILE, coef, w);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  Estado de SIMD, para mostrarlo en pantalla en el receptor (sin ADB).
//    1  = NEON verificado y activo
//    0  = NEON presente pero desactivado por el auto-test (cae a escalar)
//   -1  = esta ABI no trae NEON (p.ej. compilación 32-bit) -> escalar
// ---------------------------------------------------------------------------
int fec_simd_status() {
    fec_init();   // asegura que el auto-test ya corrió
#if FEC_HAVE_NEON
    return g_neon_ok ? 1 : 0;
#else
    return -1;
#endif
}

// Puente JNI directo (solo en Android). Lo usa el overlay del RECEPTOR. En el
// emisor este símbolo queda sin usar (inofensivo), porque fec.cpp es compartido.
#if defined(__ANDROID__)
#include <jni.h>
extern "C" JNIEXPORT jint JNICALL
Java_com_ultracast_receiver_NativeBridge_fecSimd(JNIEnv*, jobject) {
    return (jint) fec_simd_status();
}
#endif