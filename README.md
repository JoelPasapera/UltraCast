# UltraCast

**Ultra-low-latency wireless screen mirroring for cheap Android projectors — built because Miracast wasn't good enough.**

**English** · **[Español](README.es.md)**

This project is distributed under the terms of the [`LICENSE`](LICENSE) file in the repository root.

![platform](https://img.shields.io/badge/platform-Android-green)
![language](https://img.shields.io/badge/native-C%2B%2B%20%2F%20Kotlin-blue)
<!-- Add a license badge here once you pick one, e.g. ![license](https://img.shields.io/badge/license-MIT-lightgrey) -->

UltraCast mirrors an Android phone's screen to a cheap Android projector (or any second Android device) over your local Wi-Fi, using a purpose-built low-latency video pipeline that keeps working on weak or congested networks where Miracast stutters, freezes, or drops the connection.

It is **two apps** — a **sender** (the phone) and a **receiver** (the projector) — talking over a custom UDP protocol with forward error correction and WebRTC-style congestion control.

> **Why this exists.** Cheap Android projectors (the HY300 and its many clones) ship with a Miracast implementation that falls apart on real-world Wi-Fi: lag, freezes, and dropped sessions. UltraCast was built to fix exactly that one problem — and do it better than the built-in option.

---

## Table of contents

- [Demo](#demo)
- [Features](#features)
- [How it works](#how-it-works)
  - [Architecture](#architecture)
  - [The pipeline](#the-pipeline)
  - [Wire protocol](#wire-protocol)
  - [Forward error correction (FEC)](#forward-error-correction-fec)
  - [Congestion control and adaptive bitrate](#congestion-control-and-adaptive-bitrate)
- [Requirements](#requirements)
- [Building](#building)
- [Installing](#installing)
  - [Sender (phone)](#sender-phone)
  - [Receiver (projector) — the important part](#receiver-projector--the-important-part)
- [Usage](#usage)
- [The diagnostic HUD](#the-diagnostic-hud)
- [Configuration and tuning](#configuration-and-tuning)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)

---

## Demo

> 📹 **Add a 30–60 second side-by-side clip here: UltraCast vs Miracast on the same Wi-Fi, mirroring the same content.**
>
> This is the single most important thing for adoption. A screen-mirroring tool with no visible proof of being smoother gets ignored. Film both screens with a phone in slow motion, or screen-record both. One good clip is worth more than this entire README.

---

## Features

- **Low-latency UDP transport.** No TCP head-of-line blocking — a single lost packet never stalls the stream. Built for "show it now," not "deliver it perfectly, eventually."
- **Reed–Solomon forward error correction.** Recovers lost packets *without* retransmission (Cauchy-matrix RS over GF(2⁸)), with the amount of parity adapting to the measured loss. **NEON-accelerated** on 64-bit ARM, with an automatic scalar fallback that is verified bit-identical at startup.
- **Delay-based congestion control (GCC-style).** The receiver measures the trend of the one-way delay and distinguishes **congestion** (the queue is filling → reduce bitrate) from **interference** (random radio loss with flat delay → hold the bitrate and let FEC cover it). This is the same core idea WebRTC uses, and it stops the stream from needlessly dropping quality on a noisy-but-uncongested link.
- **Adaptive bitrate (ABR).** AIMD bitrate control driven by the receiver's feedback, applied to the hardware encoder **live, without restarting** the stream.
- **Content-adaptive framerate.** Mirrors a *screen*, so static content (a slide, a desktop) costs almost nothing, while motion runs at full frame rate — with a low heartbeat that keeps a frozen screen recoverable if a packet is lost.
- **Packet pacing.** Large keyframes are spread over a few milliseconds so the burst doesn't overflow the air or the receiver's buffer (critical on weak Wi-Fi).
- **Auto-discovery.** The phone finds the projector on the LAN by itself — no typing IP addresses (optional; manual IP also works).
- **On-screen diagnostic HUD.** Live fps, bitrate, packet loss, jitter, FEC overhead, congestion state, and per-frame FEC recovery counts, toggled on the projector.

---

## How it works

### Architecture

Two native (C++) UDP engines wrapped by thin Kotlin apps. All the latency-critical work — fragmentation, FEC, the feedback loop, and congestion sensing — happens in C++; Kotlin only drives `MediaProjection` / `MediaCodec` and the UI.

```
                          PHONE (sender)                                          PROJECTOR (receiver)
  ┌────────────────────────────────────────────────┐            ┌──────────────────────────────────────────────────┐
  │  MediaProjection ─▶ VirtualDisplay              │            │   libudpreceiver (C++)                            │
  │        │                                        │            │     UDP :5000 ─▶ reassemble by frame_id           │
  │        ▼ (Surface, zero-copy)                   │            │         │             ─▶ FEC decode (if needed)   │
  │  MediaCodec H.264 encoder (low-latency CBR)     │   Wi-Fi    │         ▼                                         │
  │        │  encoded NAL (no copy)                 │   (LAN)    │   MediaCodec H.264 decoder                        │
  │        ▼                                        │  ───────▶  │         │                                         │
  │  libudpsender (C++)                             │   :5000    │         ▼                                         │
  │     fragment ─▶ Reed–Solomon parity ─▶ pacing   │            │   fullscreen SurfaceView                          │
  │     ─▶ UDP :5000                                │            │                                                   │
  │                                                 │  ◀───────  │   feedback: request IDR / loss% / congestion      │
  │  feedback loop ◀── :5001 ── ABR + FEC + GCC     │   :5001    │                                                   │
  └────────────────────────────────────────────────┘            └──────────────────────────────────────────────────┘
```

### The pipeline

**Sender**
1. `MediaProjection` renders the screen into the encoder's input `Surface` (no CPU copy of pixels).
2. A hardware `MediaCodec` H.264 encoder produces NAL units in low-latency CBR mode.
3. The native engine fragments each NAL into ≤1400-byte UDP packets, appends Reed–Solomon parity packets, and **paces** large frames over time.
4. A background thread listens for receiver feedback and adjusts bitrate (ABR), FEC overhead, and keyframe requests.

**Receiver**
1. The native engine receives UDP packets, groups them by `frame_id`, and reassembles each frame.
2. If data packets are missing but enough parity arrived, it **FEC-decodes** the frame.
3. Complete frames are fed to a hardware `MediaCodec` H.264 decoder and rendered fullscreen.
4. It continuously measures loss and the one-way-delay trend, and reports both back to the sender; it also requests a keyframe (IDR) when it loses a frame it can't recover.

### Wire protocol

Four UDP ports, all on the local network:

| Port | Direction | Purpose |
|-----:|-----------|---------|
| 5000 | phone → projector | Video (fragments + FEC parity) |
| 5001 | projector → phone | Feedback (loss, congestion state, IDR requests) |
| 5002 | phone → projector | Control (toggle the on-screen debug HUD) |
| 5003 | broadcast | Discovery (`ULTRACAST_DISCOVER` → `ULTRACAST:<device name>`) |

**Packet header** (26 bytes, big-endian, packed) prepended to every video packet:

| Field | Type | Meaning |
|-------|------|---------|
| `frame_id` | `uint32` | Per-NAL id; data and parity of one frame share it |
| `total_size` | `uint32` | Size of the complete NAL (for trimming and FEC) |
| `frag_index` | `uint16` | Data: `0..K-1`; parity: `0..R-1` |
| `frag_count` | `uint16` | `K` = number of data fragments |
| `pts_us` | `uint64` | Presentation timestamp (µs) |
| `flags` | `uint8` | bit0 = config (SPS/PPS), bit1 = keyframe, bit2 = parity |
| `fec_r` | `uint8` | `R` = number of parity packets for this frame |
| `send_us` | `uint32` | Sender monotonic send time (µs) — used for delay-based congestion sensing |

**Feedback packet** (8 bytes, projector → phone):

| Field | Type | Meaning |
|-------|------|---------|
| `magic` | `uint32` | `0xFEEDBAC1` (filters stray packets) |
| `request_idr` | `uint8` | 1 = send a keyframe now |
| `cong_state` | `uint8` | 0 = normal, 1 = congestion (delay rising), 2 = queue draining |
| `loss_x10` | `uint16` | Measured loss in 0.1% units (0–1000) |

> The `send_us` / arrival comparison only ever uses **differences**, so the two devices' clocks don't need to be synchronized — the offset cancels out.

### Forward error correction (FEC)

UltraCast uses a Cauchy-matrix Reed–Solomon code over GF(2⁸) (primitive polynomial `0x11D`). For a frame split into `K` data packets it sends `R` parity packets; the receiver can reconstruct the frame as long as it receives **any** `K` of the `K+R` packets. No round-trips, so loss recovery costs no latency.

- Parity overhead **adapts** to measured loss, clamped between ~2% and ~25% (up to 20 parity packets per frame).
- The GF(2⁸) multiply is **vectorized with ARM NEON** on `arm64-v8a`. At startup the NEON path is checked against the scalar path on random data; if they ever disagree, it falls back to scalar automatically. On 32-bit ABIs it runs the scalar path (still correct, just slower). The current mode is shown in the HUD.
- **Note:** RS over GF(2⁸) is limited to `K + R ≤ 255` packets per frame. At 720p this is not reached in practice, but very large keyframes (e.g. at higher resolutions) would exceed it and go unprotected — see [Limitations](#limitations).

### Congestion control and adaptive bitrate

The bitrate controller fuses two signals from the receiver's feedback:

- **Congestion** (`cong_state = 1`): the receiver's delay-trend estimator (a least-squares slope over packet groups) sees the one-way delay rising — the link is queuing — so the sender **lowers** the bitrate.
- **Interference** (loss with a flat delay trend): random radio loss without queuing. The sender **holds** the bitrate and lets FEC recover the lost packets, instead of needlessly dropping quality. FEC overhead rises here.
- **Clean** (low loss, flat/falling delay): the sender **probes upward** in small steps.
- **Catastrophic loss** (very high loss regardless of delay): a safety backstop still reduces bitrate.

The encoder bitrate is changed live via `MediaCodec.setParameters` — no stream restart, no glitch.

---

## Requirements

**Hardware**
- Two Android devices on the **same Wi-Fi / LAN** (ideally the same access point). One is the sender (a phone), the other is the receiver (a projector or any Android device with a screen).
- 64-bit ARM (`arm64-v8a`) is recommended for NEON-accelerated FEC. 32-bit works with scalar FEC.
- The **receiver device must allow installing apps from outside the Play Store** (sideloading). On most cheap projectors this means enabling "install unknown apps" for a file manager — see [Installing the receiver](#receiver-projector--the-important-part).

**Software (to build)**
- Android Studio (recent version).
- Android **SDK** and **NDK + CMake** (the apps contain native C++ — the NDK is required). Install both from *Android Studio → SDK Manager → SDK Tools*.
- The projects target **compileSdk/targetSdk 36** and **minSdk 29 (Android 10)**. Confirm these match your local SDK and adjust in each `build.gradle` if needed.

---

## Building

The repository contains two independent Android Studio projects:

```
UltraCast/
├── sender/      ← the phone app  (package com.ultracast.sender)
└── receiver/    ← the projector app (package com.ultracast.receiver)
```

Build each one separately:

1. **Install the NDK and CMake.** In Android Studio: *SDK Manager → SDK Tools → check "NDK (Side by side)" and "CMake" → Apply*. Without these, the native libraries (`libudpsender` / `libudpreceiver`) won't compile.
2. **Open the project.** *File → Open* and select the `sender/` folder (then repeat for `receiver/`). Let Gradle sync finish.
3. **Build the APK.** *Build → Build Bundle(s) / APK(s) → Build APK(s)*. The result lands in `app/build/outputs/apk/debug/app-debug.apk`.
4. Repeat for the other project.

> If Gradle sync complains about the SDK or NDK version, open *File → Project Structure* (or the module `build.gradle`) and point it at the versions you actually have installed.

---

## Installing

### Sender (phone)

Install like any normal app:
- Plug the phone into your computer and use *Run ▶* in Android Studio, **or**
- Copy `app-debug.apk` to the phone and open it (you may need to allow "install unknown apps" for your file manager / browser once).

### Receiver (projector) — the important part

This is the step that trips people up. Most cheap Android projectors (HY300 and clones) **cannot enable Developer Options / ADB**, so you can't install over USB the normal way. Instead:

1. Build the receiver APK (`receiver/app/build/outputs/apk/debug/app-debug.apk`).
2. Copy it to a **USB flash drive**.
3. Plug the drive into the projector.
4. On the projector, open its **file manager**, find the APK on the drive, and select it.
5. The projector will ask to **allow installation from unknown sources** — approve it (you usually only do this once).
6. Install, then launch **UltraCast Receiver**. Leave it running; it goes fullscreen and waits for the phone.

> If your projector *can* enable Developer Options and ADB, you can instead `adb install app-debug.apk` over USB or Wi-Fi — much faster for iterating.

---

## Usage

1. Put both devices on the **same Wi-Fi network**.
2. **Launch the receiver** on the projector and leave it on the waiting screen.
3. **Launch the sender** on the phone:
   - Let it **auto-discover** the projector on the LAN, **or** enter the projector's IP address manually.
   - Android will show a **screen-capture consent dialog** — accept it. (On Android 13+ you may also be asked for notification permission, used for the persistent "casting" notification.)
4. Your phone screen now appears on the projector.

To stop, stop the cast from the phone (or close the sender app).

> **Default IP:** the sender ships with a hard-coded fallback IP for quick testing. Use auto-discovery, or change `DEFAULT_HOST` in `ScreenCaptureService.kt`, or enter the IP in the app, to match your projector.

---

## The diagnostic HUD

The receiver has a built-in overlay that shows exactly what the link is doing — invaluable for tuning and for proving the system works.

**Toggle it on the projector** with the remote's **OK / center / Enter** button (or **A** on a gamepad), or by tapping the screen.

The **QUALITY** block shows:

- **fps / bitrate** — frames per second and video throughput actually received.
- **Loss %** — measured packet loss.
- **Jitter (ms)** — variation in one-way delay.
- **FEC overhead %** — how much parity is currently being sent (derived from each packet's `K`/`R`).
- **Congestion** — `ok` / `CONGESTION` / `draining`, straight from the delay-trend estimator.
- **Frames OK / FEC-recovered / Lost** — how many frames arrived clean, how many FEC rescued, and how many were lost outright. **This is the most useful line:** a rising "FEC-recovered" with "Lost" near zero means FEC is doing its job.

---

## Configuration and tuning

The system runs well out of the box, but every knob is a named constant. The most useful ones:

| Constant | File | Default | What it does |
|----------|------|--------:|--------------|
| `STREAM_WIDTH` / `STREAM_HEIGHT` | `ScreenCaptureService.kt` | 1280×720 | Capture/stream resolution |
| `STREAM_FPS` | `ScreenCaptureService.kt` | 60 | Max frame rate |
| `STREAM_BITRATE` | `ScreenCaptureService.kt` | 8 Mbps | Starting bitrate |
| `BITRATE_MIN` / `BITRATE_MAX` | `ScreenCaptureService.kt` | 1.5 / 10 Mbps | ABR floor and ceiling |
| `IDR_INTERVAL_SEC` | `VideoEncoder.kt` | 3 | Keyframe interval. Higher = less bandwidth on static content; slightly slower late-join |
| `REPEAT_FRAME_AFTER_US` | `VideoEncoder.kt` | 250 ms | Heartbeat on static content; lets a requested keyframe emit even when the screen is frozen |
| `kSlopeThreshUs` | `udp_receiver.cpp` | 300 | Delay slope that flags congestion. Lower = more sensitive |
| `kLossCatastrophic` | `udp_sender.cpp` | 0.20 | Loss above which bitrate drops even without a congestion signal |
| `kPaceBytesPerSec` | `udp_sender.cpp` | ~24 Mbps | Pacing rate for large frames |
| FEC overhead cap | `udp_sender.cpp` | 25% | Maximum parity overhead |

**Use the HUD to tune.** For example: if "Lost" climbs while Congestion stays `ok`, you're seeing interference — raise the FEC cap or `kLossCatastrophic`. If the picture looks soft on a still slide, your keyframe interval or bitrate is too low.

---

## Troubleshooting

- **Projector shows nothing / stays on the waiting screen.** Make sure both devices are on the same Wi-Fi (and that the network isn't using client isolation / "AP isolation," which blocks device-to-device traffic). Try entering the projector's IP manually instead of relying on discovery.
- **Connects but the image is frozen or garbled.** Usually packet loss with no recovery. Check the HUD's Loss and FEC lines; move closer to the access point or lower `BITRATE_MAX`.
- **Choppy / micro-stutter even on good Wi-Fi.** Network jitter. Watch the HUD's Jitter value.
- **The projector won't install the APK.** You need to allow "install unknown apps" for the file manager you're using. Some projectors hide this under *Settings → Apps → Special access*.
- **Build fails on the native step.** The NDK and CMake aren't installed — add them in the SDK Manager.

---

## Limitations

Honest list — this is a focused tool, not a finished product:

- **Tested on a single hardware pair** so far (the projector + phone it was built on). Different encoders/decoders behave differently; broad device testing is the biggest open task.
- **H.264 only.** No HEVC or AV1 yet.
- **LAN only.** No NAT traversal / internet streaming.
- **No audio.** Video only (use a Bluetooth speaker in the meantime).
- **No encryption.** Fine for a home LAN; not suitable for untrusted networks.
- **Very large keyframes are unprotected.** Reed–Solomon over GF(2⁸) caps at 255 packets per frame; a keyframe larger than that (possible at higher resolutions) gets no FEC.
- **Shared C++ files are duplicated by hand** between the two apps (`packet.h`, `feedback.h`, `fec.*`).

## Roadmap

- A reproducible **benchmark harness** (UltraCast vs Miracast vs Moonlight under `netem`-controlled loss/jitter/bandwidth) with real glass-to-glass latency measurement.
- **Audio** with lip-sync.
- **HEVC / AV1** support.
- **Multi-block FEC** for large keyframes.
- **Encryption** (DTLS-style).
- De-duplicate the shared native sources.
- Broader **device matrix** testing.

---

## Contributing

Contributions are very welcome — especially **device reports**. The single most valuable thing you can do is try UltraCast on *your* phone and *your* projector and open an issue with:

- Phone model + Android version, projector model + Android version.
- Whether it worked, and a photo/recording of the HUD (fps, loss, jitter, FEC, congestion).
- Any device-specific install quirks.

Bug reports, tuning findings, and pull requests for the roadmap items are all appreciated. Please keep changes focused and describe what you tested.

---

## License

> **Choose a license before promoting the project.** Two common options:
> - **MIT / Apache-2.0** (permissive) — maximizes adoption; anyone can use it, including commercially. (scrcpy uses Apache-2.0.)
> - **GPL-3.0** (copyleft) — anyone who distributes a modified version must also open their source. (Moonlight uses GPL-3.0.)
>
> Add the chosen `LICENSE` file to the repository root and update this section.

---

## Acknowledgments

- The **congestion-control design** follows the ideas behind Google Congestion Control (GCC), the algorithm used in WebRTC.
- Kindred open-source projects that proved low-latency mirroring/streaming can be excellent: **scrcpy** and **Moonlight / Sunshine**.
- Built out of frustration with Miracast on cheap Android projectors — and a refusal to accept that it had to be that bad.
