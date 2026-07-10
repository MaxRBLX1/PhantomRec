# PhantomRec — "Record now. Encode later. Zero lag."

**Built by MaxRBLX1**
**v1.9.5**

---

## Project History

PhantomRec was originally released as **RetroRec** (v1.0 through v1.7). The name was changed in v1.8. All recordings and settings from previous versions are fully compatible.

---

## What is PhantomRec?

PhantomRec is a **free, portable, and invisible screen recorder** for Windows. It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high-quality file — all without slowing down your computer.

No GPU? No problem. Old laptop? It works. PhantomRec is designed to run on **any Windows PC from Vista to Windows 11**, from a dual-core budget machine to a high-end workstation.

---

### A Note on Windows Versions

PhantomRec doesn't care what hardware you have. It cares what OS you run — because your OS determines what capture APIs are available.

| Windows Version | Capture Method | Typical Recording FPS |
|---|---|---|
| Windows 10 / 11 | GFX (GPU zero‑copy) | 60 FPS |
| Windows 8 / 8.1 | DDAGrab (GPU) | 60 FPS |
| Windows 7 / Vista | GDI (CPU software) | Up to 60 FPS (hardware dependent) |

All capture methods write a lossless master file at the native frame rate. The final video is automatically converted to a constant 60 fps x264 file, regardless of the source frame rate.

---

## Why Choose PhantomRec?

PhantomRec uses a **two-stage ghost pipeline** — the same architecture that made Fraps legendary, rebuilt for modern lossless codecs.

**Stage 1 — Live Capture (Ut Video lossless, 1 thread, ~5% CPU):**  
The screen is captured and encoded with **Ut Video**, a mathematically lossless codec that processes each frame independently. CPU usage stays flat at ~5% regardless of on-screen action. Duplicate frames are skipped automatically. No GPU encoding means no stop‑button freeze.

**Stage 2 — Post‑Convert (x264 ultrafast, all cores, after you stop):**  
When you press stop, PhantomRec converts the lossless master to a crisp, compact x264 file at 60 fps using all CPU cores — when your system is idle. You get NVENC‑quality file sizes without needing a GPU.

### The Result:
- Your GPU stays 100% dedicated to your game or desktop
- Recording uses ~5% CPU on any hardware
- Heavy compression happens when you're done recording
- **Instant stop** — no encoder queue drain freeze
- Smooth output on any CPU from 2008 onward
- **No GPU required. No NVENC. No AMF. CPU only.**

---

## System Requirements

| Component | Minimum |
|---|---|
| **OS** | Windows Vista / 7 / 8 / 8.1 / 10 / 11 |
| **CPU** | Any dual-core x86_64 (SSE2) |
| **RAM** | 4 GB |
| **GPU** | Any. Integrated. None. All work. |
| **Storage** | Any HDD or SSD with free space |

*\*No dedicated GPU or GPU hardware encoding required.*

---

## Quick Start

1. Download the latest release and extract all files.
2. Make sure `maxsengine.exe` is in the same folder as `PhantomRec.exe`.
3. Double-click `PhantomRec.exe` — a small always‑on‑top window appears.
4. Press **F10** to start recording. Press **F10** again to stop.
5. Wait a few seconds for post‑processing — your video opens automatically.

---

## What's New in v1.9.5

### 🎯 All‑New Stage 1 — Ut Video Lossless
- Replaced MPEG‑4 Part 2 with **Ut Video** — a modern, single‑thread lossless codec that eliminates CPU spikes during heavy motion.
- Constant ~5% CPU, no matter what’s on screen (explosions, racing, desktop idle).
- Writes a mathematically perfect master file that uses ~30% less disk space than the old lossless pipeline.

### 🔧 Hard‑drive‑Friendly Pipeline
- Added `-max_muxing_queue_size 9096` to absorb disk pauses — recordings stay smooth even on slow mechanical hard drives.
- Video input queue tuned to **2048** (recommended maximum for live capture), with audio queue at **512**.
- All tested on a real 200 GB HDD with zero lag spikes.

### 🔇 Perfect Audio/Video Sync
- Audio and video now start at the exact same moment (using a sync event), removing any permanent offset.
- Works for both initial recording and after a pause/resume.

### 🖥️ FFmpeg Console Fix
- FFmpeg now runs with its own minimized console (`CREATE_NEW_CONSOLE`) — this guarantees proper `CTRL_BREAK` handling and eliminates the “accordion effect” timeline distortion that caused stuttering in earlier versions.

### 🧹 Clean, Understandable Codebase
- Removed dead variables and functions (legacy MPEG‑4 quality settings, unused thread count parameters).
- The source is now easy to read and contribute to.

### 🎨 UI Improvements (from previous patches)
- Custom backgrounds, fonts, animated GIFs, font colour/size — all hot‑reloaded from `Settings.ini`.
- No more UI text ghosting or stacking.
- Thread‑safe UI updates (core callbacks are marshalled to the main thread).

### 🛠️ Stage 2 — Ultrafast CFR Conversion
- Post‑processing now uses `-preset ultrafast -r 60 -fps_mode cfr` for fast, consistent 60 fps output.
- No manual thread count — FFmpeg auto‑detects optimal threading.

---

## Settings How to control

All settings are in **`Settings.ini`** (in the same folder as `PhantomRec.exe`). Edit it while the program is running — changes take effect within 2 seconds.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
CaptureMethod=auto
```
- Hotkey – F1‑F12 for function keys, or a single letter for Ctrl+<letter> (e.g. R = Ctrl+R).

- PauseHotkey – same format.

- ConvertAfterRecording – yes (recommended) or no (keeps the huge temporary file).

- CaptureMethod – auto (default, auto‑detects best method), gfx, ddagrab, gdi.


## Configuration

All settings are in **`Settings.ini`** (in the same folder as `PhantomRec.exe`).  
Edit it while the program is running — changes take effect within 2 seconds.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
CaptureMethod=auto

[Appearance]
Background=C:\path\to\image.png
Font=C:\path\to\font.ttf
FontSize=14
FontColor=16777215
```
Note: The ConvertPreset option from older versions is ignored in v1.9.5 — Stage 2 always uses ultrafast for the quickest possible conversion.


## Building from Source

### Requirements
- MinGW‑w64 (UCRT64)
- Windows SDK

### Compile

```bash
# Step 1: Compile the pure C core
gcc -std=c11 -O2 -c src/phantomrec_core.c -o phantomrec_core.o

# Step 2: Link with C++ UI
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    phantomrec_core.o src/PhantomRec.cpp \
    -o PhantomRec.exe \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt -lgdiplus -lcomdlg32 \
    -lpowrprof
```
## ⚠️ -D_WIN32_WINNT=0x0A00 is strictly required. Without it, the binary targets XP compatibility and the recording pipeline fails with 0 FPS.

## Project Tree
```
PhantomRec/
├── src/
│ ├── PhantomRec.cpp # C++ UI (Win32 + GDI+)
│ ├── phantomrec_core.c # Pure C11 capture engine
│ └── phantomrec_core.h # Shared header (extern "C" bridge)
├── .gitignore
├── LICENSE
└── README.md
```
---

## What PhantomRec Does Not Do (Yet)

- **Streaming** — PhantomRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per‑window capture** — PhantomRec captures the entire monitor.

---

## License & Credits

PhantomRec is free software. Use it, modify it, share it.

Built by **MaxRBLX1**.

**Max'sEngine™** powered by **FFmpeg** ([ffmpeg.org](https://ffmpeg.org)).

Audio capture based on Microsoft WASAPI sample code.
