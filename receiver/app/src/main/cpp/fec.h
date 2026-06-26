// file: fec.h
// path: app\src\main\cpp\fec.h

#pragma once
#include <cstdint>

// ===========================================================================
//  Reed-Solomon de borrado (erasure coding) sobre GF(2^8) con matriz de Cauchy.
//  COMPARTIDO por emisor (codifica paridad) y receptor (reconstruye pérdidas).
// ===========================================================================

// Tope de paquetes de paridad por fotograma (acota CPU, ancho de banda y buffers).
static const int kFecMaxParity = 20;

// Construye las tablas de GF(2^8). Idempotente; las funciones lo llaman solas.
void fec_init();

// Coeficiente Cauchy de la fila de paridad p y la columna de datos d, para K
// fragmentos de datos. C[p][d] = 1 / ((K+p) XOR d) en GF(2^8).
uint8_t fec_coef(int p, int d, int K);

// EMISOR: calcula R paquetes de paridad (cada uno de P bytes) a partir de los
// K fragmentos de datos contenidos en `data` (size bytes en total; el último
// fragmento se trata como P bytes rellenando con ceros).
//   parity: buffer de salida de R*P bytes (fila p en parity + p*P).
void fec_encode(const uint8_t* data, int size, int K, int R, int P, uint8_t* parity);

// RECEPTOR: reconstruye los fragmentos de datos que falten, escribiéndolos en
// `assembly` (fragmento i en assembly + i*P). Cada símbolo es de P bytes.
//   data_present[K]    : 1 si llegó el fragmento de datos i
//   parity_present[R]  : 1 si llegó el paquete de paridad p
//   parity             : R*P bytes (fila p en parity + p*P)
// Devuelve 0 si reconstruyó todo lo que faltaba, -1 si no hay paridad suficiente.
int fec_decode(uint8_t* assembly, int P, int K, int R,
               const uint8_t* data_present,
               const uint8_t* parity_present,
               const uint8_t* parity);
