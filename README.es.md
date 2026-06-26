# UltraCast

**Espejado de pantalla inalámbrico de ultra baja latencia para proyectores Android baratos — hecho porque Miracast no daba la talla.**

**[English](README.md)** · **Español**

Este proyecto se distribuye bajo los términos de la [`LICENSE`](LICENSE) archivo situado en la raíz del repositorio.

![platform](https://img.shields.io/badge/platform-Android-green)
![language](https://img.shields.io/badge/native-C%2B%2B%20%2F%20Kotlin-blue)
<!-- Agregá una insignia de licencia aquí, p. ej. ![license](https://img.shields.io/badge/license-MIT-lightgrey) -->

UltraCast refleja la pantalla de un celular Android en un proyector Android barato (o en cualquier segundo dispositivo Android) a través de tu red Wi-Fi local, usando un pipeline de vídeo de baja latencia hecho a medida que sigue funcionando en redes débiles o congestionadas, donde Miracast tartamudea, se congela o corta la conexión.

Son **dos apps** — un **emisor** (el celular) y un **receptor** (el proyector) — que se comunican mediante un protocolo UDP propio con corrección de errores hacia adelante (FEC) y control de congestión al estilo WebRTC.

> **Por qué existe.** Los proyectores Android baratos (el HY300 y sus muchos clones) traen una implementación de Miracast que se desarma con el Wi-Fi del mundo real: lag, congelados y sesiones que se caen. UltraCast se hizo para resolver exactamente ese problema — y hacerlo mejor que la opción de fábrica.

---

## Tabla de contenidos

- [Demo](#demo)
- [Características](#características)
- [Cómo funciona](#cómo-funciona)
  - [Arquitectura](#arquitectura)
  - [El pipeline](#el-pipeline)
  - [Protocolo de red](#protocolo-de-red)
  - [Corrección de errores (FEC)](#corrección-de-errores-fec)
  - [Control de congestión y bitrate adaptativo](#control-de-congestión-y-bitrate-adaptativo)
- [Requisitos](#requisitos)
- [Compilación](#compilación)
- [Instalación](#instalación)
  - [Emisor (celular)](#emisor-celular)
  - [Receptor (proyector) — la parte importante](#receptor-proyector--la-parte-importante)
- [Uso](#uso)
- [El HUD de diagnóstico](#el-hud-de-diagnóstico)
- [Configuración y ajustes](#configuración-y-ajustes)
- [Solución de problemas](#solución-de-problemas)
- [Limitaciones](#limitaciones)
- [Hoja de ruta](#hoja-de-ruta)
- [Cómo contribuir](#cómo-contribuir)
- [Licencia](#licencia)
- [Agradecimientos](#agradecimientos)

---

## Demo

> 📹 **Agregar aquí un clip de 30–60 segundos lado a lado: UltraCast vs Miracast en el mismo Wi-Fi, espejando el mismo contenido.**
>
> Es lo más importante para la adopción, por encima de todo este README. Una herramienta de espejado sin una prueba visible de ser más fluida pasa desapercibida. Filmá ambas pantallas con un celular en cámara lenta, o grabá la pantalla de las dos. Un buen clip vale más que todo este README.

---

## Características

- **Transporte UDP de baja latencia.** Sin bloqueo de cabeza de línea como en TCP — un único paquete perdido nunca frena el flujo. Pensado para "mostralo ya", no para "entregalo perfecto, pero más tarde".
- **Corrección de errores Reed–Solomon.** Recupera paquetes perdidos *sin* retransmisión (Reed–Solomon con matriz de Cauchy sobre GF(2⁸)), y la cantidad de paridad se adapta a la pérdida medida. **Acelerado con NEON** en ARM de 64 bits, con un camino escalar de respaldo que se verifica bit a bit al arrancar.
- **Control de congestión por retardo (estilo GCC).** El receptor mide la tendencia del retardo de ida y distingue **congestión** (la cola se está llenando → bajar el bitrate) de **interferencia** (pérdida aleatoria de radio con retardo plano → mantener el bitrate y dejar que el FEC la cubra). Es la misma idea de fondo que usa WebRTC, y evita que el sistema baje la calidad sin necesidad en un enlace ruidoso pero no congestionado.
- **Bitrate adaptativo (ABR).** Control de bitrate AIMD guiado por el feedback del receptor, aplicado al codificador por hardware **en vivo, sin reiniciar** el flujo.
- **Framerate adaptativo al contenido.** Espeja una *pantalla*, así que el contenido estático (una diapositiva, un escritorio) casi no cuesta nada, mientras que el movimiento corre a fotogramas completos — con un latido mínimo que mantiene recuperable una pantalla congelada si se pierde un paquete.
- **Pacing de paquetes.** Los fotogramas clave grandes se reparten a lo largo de unos milisegundos para que la ráfaga no desborde el aire ni el búfer del receptor (clave en Wi-Fi débil).
- **Autodescubrimiento.** El celular encuentra el proyector en la red por sí mismo — sin escribir direcciones IP (opcional; también funciona con IP manual).
- **HUD de diagnóstico en pantalla.** fps, bitrate, pérdida de paquetes, jitter, overhead del FEC, estado de congestión y conteo de recuperación por fotograma, en vivo, alternable en el proyector.

---

## Cómo funciona

### Arquitectura

Dos motores UDP nativos (en C++) envueltos por apps Kotlin delgadas. Todo el trabajo crítico para la latencia — la fragmentación, el FEC, el lazo de feedback y el sensado de congestión — ocurre en C++; Kotlin solo maneja `MediaProjection` / `MediaCodec` y la interfaz.

```
                          CELULAR (emisor)                                       PROYECTOR (receptor)
  ┌────────────────────────────────────────────────┐            ┌──────────────────────────────────────────────────┐
  │  MediaProjection ─▶ VirtualDisplay              │            │   libudpreceiver (C++)                            │
  │        │                                        │            │     UDP :5000 ─▶ reensamblar por frame_id         │
  │        ▼ (Surface, sin copia)                   │            │         │             ─▶ decodificar FEC (si hace falta)
  │  Codificador MediaCodec H.264 (baja latencia)   │   Wi-Fi    │         ▼                                         │
  │        │  NAL codificado (sin copia)            │   (LAN)    │   Decodificador MediaCodec H.264                  │
  │        ▼                                        │  ───────▶  │         │                                         │
  │  libudpsender (C++)                             │   :5000    │         ▼                                         │
  │     fragmentar ─▶ paridad Reed–Solomon ─▶ pacing│            │   SurfaceView a pantalla completa                 │
  │     ─▶ UDP :5000                                │            │                                                   │
  │                                                 │  ◀───────  │   feedback: pedir IDR / pérdida% / congestión     │
  │  lazo de feedback ◀── :5001 ── ABR + FEC + GCC  │   :5001    │                                                   │
  └────────────────────────────────────────────────┘            └──────────────────────────────────────────────────┘
```

### El pipeline

**Emisor**
1. `MediaProjection` dibuja la pantalla directamente en el `Surface` de entrada del codificador (sin copia de píxeles por CPU).
2. Un codificador `MediaCodec` H.264 por hardware produce unidades NAL en modo de baja latencia (CBR).
3. El motor nativo fragmenta cada NAL en paquetes UDP de ≤1400 bytes, agrega paquetes de paridad Reed–Solomon y aplica **pacing** a los fotogramas grandes.
4. Un hilo en segundo plano escucha el feedback del receptor y ajusta el bitrate (ABR), el overhead del FEC y los pedidos de fotograma clave.

**Receptor**
1. El motor nativo recibe los paquetes UDP, los agrupa por `frame_id` y reensambla cada fotograma.
2. Si faltan paquetes de datos pero llegó suficiente paridad, **decodifica con FEC** el fotograma.
3. Los fotogramas completos se entregan a un decodificador `MediaCodec` H.264 por hardware y se muestran a pantalla completa.
4. Mide de forma continua la pérdida y la tendencia del retardo de ida, y reporta ambas al emisor; además pide un fotograma clave (IDR) cuando pierde un fotograma que no puede recuperar.

### Protocolo de red

Cuatro puertos UDP, todos en la red local:

| Puerto | Sentido | Función |
|-------:|---------|---------|
| 5000 | celular → proyector | Vídeo (fragmentos + paridad FEC) |
| 5001 | proyector → celular | Feedback (pérdida, estado de congestión, pedidos de IDR) |
| 5002 | celular → proyector | Control (mostrar/ocultar el HUD de depuración) |
| 5003 | broadcast | Descubrimiento (`ULTRACAST_DISCOVER` → `ULTRACAST:<nombre del dispositivo>`) |

**Cabecera del paquete** (26 bytes, big-endian, empaquetada) al inicio de cada paquete de vídeo:

| Campo | Tipo | Significado |
|-------|------|-------------|
| `frame_id` | `uint32` | Id por NAL; los datos y la paridad de un fotograma lo comparten |
| `total_size` | `uint32` | Tamaño del NAL completo (para recortar y para el FEC) |
| `frag_index` | `uint16` | Datos: `0..K-1`; paridad: `0..R-1` |
| `frag_count` | `uint16` | `K` = número de fragmentos de datos |
| `pts_us` | `uint64` | Timestamp de presentación (µs) |
| `flags` | `uint8` | bit0 = config (SPS/PPS), bit1 = fotograma clave, bit2 = paridad |
| `fec_r` | `uint8` | `R` = número de paquetes de paridad de este fotograma |
| `send_us` | `uint32` | Tiempo de envío del emisor, reloj monotónico (µs) — para el sensado de congestión por retardo |

**Paquete de feedback** (8 bytes, proyector → celular):

| Campo | Tipo | Significado |
|-------|------|-------------|
| `magic` | `uint32` | `0xFEEDBAC1` (filtra paquetes ajenos) |
| `request_idr` | `uint8` | 1 = enviar un fotograma clave ya |
| `cong_state` | `uint8` | 0 = normal, 1 = congestión (retardo subiendo), 2 = la cola se está drenando |
| `loss_x10` | `uint16` | Pérdida medida en unidades de 0.1% (0–1000) |

> La comparación entre `send_us` y la llegada solo usa **diferencias**, así que los relojes de los dos dispositivos no necesitan estar sincronizados — el desfase se cancela.

### Corrección de errores (FEC)

UltraCast usa un código Reed–Solomon con matriz de Cauchy sobre GF(2⁸) (polinomio primitivo `0x11D`). Para un fotograma dividido en `K` paquetes de datos envía `R` paquetes de paridad; el receptor puede reconstruir el fotograma mientras reciba **cualquier** combinación de `K` de los `K+R` paquetes. Sin ida y vuelta, así que la recuperación de pérdida no cuesta latencia.

- El overhead de paridad se **adapta** a la pérdida medida, acotado entre ~2% y ~25% (hasta 20 paquetes de paridad por fotograma).
- La multiplicación en GF(2⁸) está **vectorizada con ARM NEON** en `arm64-v8a`. Al arrancar, el camino NEON se compara contra el escalar sobre datos aleatorios; si alguna vez difieren, cae al escalar automáticamente. En ABIs de 32 bits corre el camino escalar (igual de correcto, solo más lento). El modo actual se muestra en el HUD.
- **Nota:** Reed–Solomon sobre GF(2⁸) está limitado a `K + R ≤ 255` paquetes por fotograma. A 720p eso no se alcanza en la práctica, pero los fotogramas clave muy grandes (posibles a resoluciones más altas) lo superarían y quedarían sin protección — ver [Limitaciones](#limitaciones).

### Control de congestión y bitrate adaptativo

El controlador de bitrate fusiona dos señales del feedback del receptor:

- **Congestión** (`cong_state = 1`): el estimador de tendencia del retardo del receptor (una pendiente por mínimos cuadrados sobre grupos de paquetes) ve subir el retardo de ida — el enlace está encolando — así que el emisor **baja** el bitrate.
- **Interferencia** (pérdida con tendencia del retardo plana): pérdida aleatoria de radio sin encolamiento. El emisor **mantiene** el bitrate y deja que el FEC recupere los paquetes perdidos, en vez de bajar la calidad sin necesidad. Acá el overhead del FEC sube.
- **Limpio** (poca pérdida, retardo plano o bajando): el emisor **sube despacio**, en pasos pequeños.
- **Pérdida catastrófica** (pérdida muy alta sin importar el retardo): un respaldo de seguridad baja igual el bitrate.

El bitrate del codificador se cambia en vivo con `MediaCodec.setParameters` — sin reiniciar el flujo, sin glitch.

---

## Requisitos

**Hardware**
- Dos dispositivos Android en la **misma red Wi-Fi / LAN** (idealmente el mismo punto de acceso). Uno es el emisor (un celular) y el otro el receptor (un proyector o cualquier Android con pantalla).
- Se recomienda ARM de 64 bits (`arm64-v8a`) para el FEC acelerado con NEON. En 32 bits funciona con el FEC escalar.
- El **dispositivo receptor debe permitir instalar apps fuera de la Play Store** (sideload). En la mayoría de los proyectores baratos esto significa habilitar "instalar apps desconocidas" para un explorador de archivos — ver [Instalación del receptor](#receptor-proyector--la-parte-importante).

**Software (para compilar)**
- Android Studio (versión reciente).
- **SDK** de Android y **NDK + CMake** (las apps contienen código nativo en C++ — el NDK es obligatorio). Instalá ambos desde *Android Studio → SDK Manager → SDK Tools*.
- Los proyectos apuntan a **compileSdk/targetSdk 36** y **minSdk 29 (Android 10)**. Verificá que coincidan con tu SDK local y ajustá cada `build.gradle` si hace falta.

---

## Compilación

El repositorio contiene dos proyectos de Android Studio independientes:

```
UltraCast/
├── sender/      ← la app del celular  (paquete com.ultracast.sender)
└── receiver/    ← la app del proyector (paquete com.ultracast.receiver)
```

Compilá cada uno por separado:

1. **Instalar el NDK y CMake.** En Android Studio: *SDK Manager → SDK Tools → marcar "NDK (Side by side)" y "CMake" → Apply*. Sin esto, las librerías nativas (`libudpsender` / `libudpreceiver`) no compilan.
2. **Abrir el proyecto.** *File → Open* y elegir la carpeta `sender/` (después repetir con `receiver/`). Esperar a que termine la sincronización de Gradle.
3. **Compilar el APK.** *Build → Build Bundle(s) / APK(s) → Build APK(s)*. El resultado queda en `app/build/outputs/apk/debug/app-debug.apk`.
4. Repetir para el otro proyecto.

> Si la sincronización de Gradle se queja por la versión del SDK o del NDK, abrí *File → Project Structure* (o el `build.gradle` del módulo) y apuntá a las versiones que tengas instaladas.

---

## Instalación

### Emisor (celular)

Se instala como cualquier app normal:
- Conectá el celular a la computadora y usá *Run ▶* en Android Studio, **o**
- Copiá `app-debug.apk` al celular y abrilo (puede que tengas que permitir "instalar apps desconocidas" para tu explorador o navegador una vez).

### Receptor (proyector) — la parte importante

Este es el paso que más cuesta. La mayoría de los proyectores Android baratos (el HY300 y sus clones) **no pueden habilitar Opciones de desarrollador / ADB**, así que no se puede instalar por USB de la forma normal. En su lugar:

1. Compilá el APK del receptor (`receiver/app/build/outputs/apk/debug/app-debug.apk`).
2. Copialo a un **pendrive USB**.
3. Conectá el pendrive al proyector.
4. En el proyector, abrí su **explorador de archivos**, buscá el APK en el pendrive y seleccionalo.
5. El proyector va a pedir **permitir la instalación desde fuentes desconocidas** — aprobalo (normalmente se hace una sola vez).
6. Instalá y abrí **UltraCast Receiver**. Dejalo corriendo; pasa a pantalla completa y queda esperando al celular.

> Si tu proyector *sí* puede habilitar Opciones de desarrollador y ADB, podés usar `adb install app-debug.apk` por USB o Wi-Fi — mucho más rápido para iterar.

---

## Uso

1. Poné ambos dispositivos en la **misma red Wi-Fi**.
2. **Abrí el receptor** en el proyector y dejalo en la pantalla de espera.
3. **Abrí el emisor** en el celular:
   - Dejá que **descubra** el proyector en la red, **o** ingresá la dirección IP del proyector a mano.
   - Android va a mostrar un **diálogo de consentimiento para capturar la pantalla** — aceptalo. (En Android 13+ también puede pedirte permiso de notificaciones, para la notificación persistente de "transmitiendo".)
4. Tu pantalla del celular ya aparece en el proyector.

Para detener, cortá la transmisión desde el celular (o cerrá la app del emisor).

> **IP por defecto:** el emisor trae una IP fija de respaldo para pruebas rápidas. Usá el autodescubrimiento, o cambiá `DEFAULT_HOST` en `ScreenCaptureService.kt`, o ingresá la IP en la app, para que coincida con tu proyector.

---

## El HUD de diagnóstico

El receptor tiene una superposición integrada que muestra exactamente qué está haciendo el enlace — invaluable para ajustar y para demostrar que el sistema funciona.

**Alternalo en el proyector** con el botón **OK / central / Enter** del control remoto (o **A** en un gamepad), o tocando la pantalla.

El bloque **CALIDAD** muestra:

- **fps / bitrate** — fotogramas por segundo y throughput de vídeo realmente recibido.
- **Pérdida %** — pérdida de paquetes medida.
- **Jitter (ms)** — variación del retardo de ida.
- **Overhead FEC %** — cuánta paridad se está enviando ahora (deducida del `K`/`R` de cada paquete).
- **Congestión** — `ok` / `CONGESTIÓN` / `drenando`, directo desde el estimador de tendencia del retardo.
- **Frames OK / rescatados por FEC / perdidos** — cuántos fotogramas llegaron limpios, cuántos rescató el FEC y cuántos se perdieron del todo. **Esta es la línea más útil:** un "rescatados por FEC" que sube con "perdidos" cerca de cero significa que el FEC está haciendo su trabajo.

---

## Configuración y ajustes

El sistema anda bien tal cual, pero cada perilla es una constante con nombre. Las más útiles:

| Constante | Archivo | Valor | Qué hace |
|-----------|---------|------:|----------|
| `STREAM_WIDTH` / `STREAM_HEIGHT` | `ScreenCaptureService.kt` | 1280×720 | Resolución de captura/transmisión |
| `STREAM_FPS` | `ScreenCaptureService.kt` | 60 | Framerate máximo |
| `STREAM_BITRATE` | `ScreenCaptureService.kt` | 8 Mbps | Bitrate inicial |
| `BITRATE_MIN` / `BITRATE_MAX` | `ScreenCaptureService.kt` | 1.5 / 10 Mbps | Piso y techo del ABR |
| `IDR_INTERVAL_SEC` | `VideoEncoder.kt` | 3 | Intervalo entre fotogramas clave. Más alto = menos ancho de banda en estático; late-join un poco más lento |
| `REPEAT_FRAME_AFTER_US` | `VideoEncoder.kt` | 250 ms | Latido en contenido estático; permite emitir un fotograma clave pedido aunque la pantalla esté congelada |
| `kSlopeThreshUs` | `udp_receiver.cpp` | 300 | Pendiente del retardo que marca congestión. Más bajo = más sensible |
| `kLossCatastrophic` | `udp_sender.cpp` | 0.20 | Pérdida a partir de la cual el bitrate baja aun sin señal de congestión |
| `kPaceBytesPerSec` | `udp_sender.cpp` | ~24 Mbps | Ritmo del pacing para fotogramas grandes |
| Tope de overhead FEC | `udp_sender.cpp` | 25% | Overhead máximo de paridad |

**Usá el HUD para ajustar.** Por ejemplo: si "perdidos" sube mientras Congestión sigue en `ok`, estás viendo interferencia — subí el tope del FEC o `kLossCatastrophic`. Si la imagen se ve blanda en una diapositiva fija, tu intervalo de fotograma clave o tu bitrate están demasiado bajos.

---

## Solución de problemas

- **El proyector no muestra nada / se queda en la pantalla de espera.** Asegurate de que ambos dispositivos estén en la misma Wi-Fi (y que la red no use aislamiento de clientes / "AP isolation", que bloquea el tráfico entre dispositivos). Probá ingresar la IP del proyector a mano en vez de depender del descubrimiento.
- **Conecta pero la imagen se congela o se corrompe.** Suele ser pérdida de paquetes sin recuperación. Mirá las líneas de Pérdida y FEC del HUD; acercate al punto de acceso o bajá `BITRATE_MAX`.
- **Cortes / micro-tirones incluso con buen Wi-Fi.** Jitter de red. Mirá el valor de Jitter en el HUD.
- **El proyector no instala el APK.** Tenés que permitir "instalar apps desconocidas" para el explorador de archivos que estés usando. Algunos proyectores lo esconden en *Ajustes → Apps → Acceso especial*.
- **La compilación falla en el paso nativo.** El NDK y CMake no están instalados — agregalos en el SDK Manager.

---

## Limitaciones

Lista honesta — esto es una herramienta enfocada, no un producto terminado:

- **Probado en un solo par de hardware** hasta ahora (el proyector + el celular en los que se construyó). Distintos codificadores/decodificadores se comportan distinto; probar en muchos dispositivos es la tarea abierta más grande.
- **Solo H.264.** Todavía no hay HEVC ni AV1.
- **Solo LAN.** Sin travesía de NAT / transmisión por internet.
- **Sin audio.** Solo vídeo (por ahora, usá un parlante Bluetooth).
- **Sin cifrado.** Está bien para una LAN doméstica; no es apto para redes no confiables.
- **Los fotogramas clave muy grandes quedan sin protección.** Reed–Solomon sobre GF(2⁸) topa en 255 paquetes por fotograma; un fotograma clave más grande que eso (posible a resoluciones más altas) no lleva FEC.
- **Los archivos C++ compartidos están duplicados a mano** entre las dos apps (`packet.h`, `feedback.h`, `fec.*`).

## Hoja de ruta

- Un **banco de pruebas reproducible** (UltraCast vs Miracast vs Moonlight bajo pérdida/jitter/ancho de banda controlados con `netem`) con medición real de latencia pantalla-a-pantalla.
- **Audio** con sincronía labial.
- Soporte de **HEVC / AV1**.
- **FEC multi-bloque** para fotogramas clave grandes.
- **Cifrado** (estilo DTLS).
- Dejar de duplicar las fuentes nativas compartidas.
- Pruebas en una **matriz de dispositivos** más amplia.

---

## Cómo contribuir

Las contribuciones son muy bienvenidas — sobre todo los **reportes de dispositivos**. Lo más valioso que podés hacer es probar UltraCast en *tu* celular y *tu* proyector y abrir un issue con:

- Modelo del celular + versión de Android, modelo del proyector + versión de Android.
- Si funcionó, y una foto/grabación del HUD (fps, pérdida, jitter, FEC, congestión).
- Cualquier particularidad de instalación específica del dispositivo.

Los reportes de bugs, los hallazgos de ajuste y los pull requests para los puntos de la hoja de ruta se agradecen. Por favor, mantené los cambios enfocados y contá qué probaste.

---

## Licencia

Este proyecto se distribuye bajo los términos del archivo [`LICENSE`](LICENSE) en la raíz del repositorio.

---

## Agradecimientos

- El **diseño del control de congestión** sigue las ideas detrás de Google Congestion Control (GCC), el algoritmo que usa WebRTC.
- Proyectos open-source hermanos que demostraron que el espejado/streaming de baja latencia puede ser excelente: **scrcpy** y **Moonlight / Sunshine**.
- Hecho por frustración con Miracast en proyectores Android baratos — y por negarse a aceptar que tenía que ser tan malo.
