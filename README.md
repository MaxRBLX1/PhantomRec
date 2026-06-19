**"Every screen deserves to be recorded."**  
 *Built by MaxRBLX1*

---

## What is RetroRec?

RetroRec is a **free, portable, and invisible screen recorder** for Windows. It captures your desktop (or game) at a smooth 60 fps with system audio, then converts the recording into a compact, high‑quality file — all without slowing down your computer.

No GPU? No problem. Old laptop? It works. RetroRec is designed to run on **any Windows 10/11 PC**, from a dual‑core budget machine to a high‑end workstation.

---

## Quick Start

1. **Download** and extract the `RetroRec.zip` package.
2. Make sure `ffmpeg.exe` is in the same folder as `RetroRec.exe`.
3. Double‑click **`RetroRec.exe`** – a small always‑on‑top window appears.
4. Press your configured hotkey (default **F10**) to start recording.
5. Press the same hotkey again to stop.
6. Wait a few seconds for the post‑processing, and your video opens automatically in Explorer.

---

## The Ghost Philosophy

RetroRec works differently from other recorders. Instead of encoding everything in real time, it utilizes a two-stage pipeline to ensure **no lag while recording**, even on weak hardware:

1. **Captures the screen using Windows Graphics Capture** — zero‑copy GPU texture access, no performance hit.
2. **Encodes to MPEG‑4 Part 2** — a lightweight codec that uses minimal CPU and produces small files during capture.
3. **Skips duplicate frames** when your screen is static — saving disk space and CPU cycles.
4. **Converts to x264 after you press stop** — creating a compact, high‑quality file when your system is idle.

---

## 🎮 A Note on PC Gaming & The Road to v2.0

While RetroRec v1.3 is the ultimate master of desktop, browser, window, and emulator recording, **it is not optimized for heavy PC gaming yet.** Here is the technical reality:

* **The Exclusive Fullscreen Bottleneck:** RetroRec v1.3 captures frames via Windows Graphics Capture (`gfxcapture`), which hooks into the Desktop Window Manager (DWM). When a high-performance PC game runs in *Exclusive Fullscreen* mode, it completely bypasses the DWM to talk to your graphics card directly. Because the game hides from the Windows desktop compositor, v1.3 will often record a **black screen or a frozen frame**.
* **The Frame-Rate Tug-of-War:** If a game is maxing out your GPU to output ultra-high frame rates, it leaves zero processing queues open for standard Windows OS capture APIs, causing micro-stutters.

### 🚀 Coming in RetroRec v2.0: True Game Capture
To bypass this limitation entirely, **RetroRec v2.0** will introduce a dedicated **Graphics API Injection Engine**. Instead of asking Windows for the desktop, v2.0 will safely inject a lightweight hook into Direct3D 11/12 and Vulkan runtimes. This will capture frames directly from the game's internal swap chain before it hits the screen—delivering flawless, zero-lag 60 FPS recording for full-screen games, completely immune to hardware boundaries.

*Until v2.0 drops, you can record games in v1.3 by switching your game's video settings from "Exclusive Fullscreen" to **"Borderless Windowed"** mode!*

---

## Features

* **60 fps smooth capture** using Windows Graphics Capture (`gfxcapture`).
* **System audio recording** via WASAPI loopback with hardware QPC timestamps — perfect sync, no extra drivers.
* **MPEG‑4 Part 2 live encoding** — 76% smaller files than MJPEG, same CPU usage.
* **Variable Frame Rate (VFR)** — only encodes frames when the screen changes.
* **Zero‑config auto‑tuning** — RetroRec detects your CPU cores and adjusts quality automatically.
* **Live INI reload** — change hotkeys or settings while RetroRec is running.
* **Settings button** — opens the config file in Notepad.
* **Orphaned temp file cleanup** — removes leftover files from crashed recordings.
* **Portable** — runs from a USB stick, no installation.
* **Configurable hotkeys** (F1‑F12 or Ctrl+Letter).
* **Open‑source** — you can read, modify, and share the code.

---

## System Requirements

| Component | Minimum Requirement |
| :--- | :--- |
| **Operating System** | Windows 10 version 1803 or later / Windows 11 |
| **CPU** | Any dual‑core x86_64 processor |
| **RAM** | 4 GB |
| **GPU** | Any graphics adapter that displays Windows |
| **Storage** | Any HDD or SSD |

*\*No dedicated GPU or GPU hardware encoding required.*

---

## Configuration

All settings are stored in the **`RetroRec.ini`** file next to the executable. You can edit it while RetroRec is running — changes take effect immediately.

```ini
[Settings]
; Hotkey to start/stop recording (F1-F12, or a single letter for Ctrl+Letter)
Hotkey=F10

; Hotkey to pause/resume (same format as Hotkey)
; NOTE: Pause is currently disabled in v1.3 and will be implemented in v1.4
PauseHotkey=P

; Convert to a compact x264 file after recording (yes/no)
ConvertAfterRecording=yes

; x264 preset (fast = quicker, medium = slightly better quality)
ConvertPreset=fast
```

---

## Recording Output

Recordings are saved in your `Videos\RetroRec` folder by default. After stopping, you'll find:

* `RetroRec_YYYYMMDD_HHMMSS.mkv` — The final video.
* The intermediate temporary file (`_temp.mkv`) is automatically deleted after conversion.
* Orphaned temp files from crashes are cleaned up on the next launch.

---

## Troubleshooting

### "ffmpeg.exe not found!"
Make sure `ffmpeg.exe` is in the exact same folder as `RetroRec.exe`.

### No audio in the recording?
* Check that your system sound is not muted.
* RetroRec uses the default playback device — make sure it’s set correctly in Windows Sound settings.

### Recording is lagging or stuttering?
* RetroRec auto‑tunes for your CPU. On dual‑core systems, it intentionally uses 1 encoding thread — this is normal and should still maintain 55+ FPS.
* Close other CPU‑intensive applications while recording.

### The yellow border doesn’t appear?
This may happen on older Windows 10 builds. The recording still works — the border is a visual indicator only.

---

## Building from Source

If you want to compile RetroRec yourself:

### Requirements
* GCC (MinGW‑w64 UCRT64) or MSVC
* Windows SDK

### Linker Libraries
`avrt.lib`, `ole32.lib`, `comctl32.lib`, `ksuser.lib`

### Compile Command
```bash
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    -o RetroRec.exe src/RetroRec.cpp \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt
```

> ⚠️ **Important:** The `-D_WIN32_WINNT=0x0A00` flag is strictly required. Without it, the binary targets Windows XP compatibility, causing the recording pipeline to fail with 0 FPS.

---

## Technical Details

* **Capture:** Windows Graphics Capture (`gfxcapture`) with D3D11 hardware download.
* **Live Codec:** MPEG‑4 Part 2 (`mpeg4`), quality value 5, VFR mode.
* **Audio:** WASAPI loopback, event‑driven, MMCSS real‑time priority.
* **Post‑Convert:** x264 ultrafast preset, CRF auto‑tuned by CPU cores.
* **Container:** Matroska (`.mkv`) — crash‑safe.

---

## License & Credits

* RetroRec is free software. Use it, modify it, and share it.
* Built by **MaxRBLX1**.
* Powered by **FFmpeg**. Audio capture based on Microsoft WASAPI sample code.
