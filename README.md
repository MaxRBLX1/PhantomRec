# RetroRec v1.6 — "Every screen deserves to be recorded."

**Built by MaxRBLX1**

---

## What is RetroRec?

RetroRec is a **free, portable, and invisible screen recorder** for Windows. It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high-quality file — all without slowing down your computer.

No GPU? No problem. Old laptop? It works. RetroRec is designed to run on **any Windows PC from Vista to Windows 11**, from a dual-core budget machine to a high-end workstation.

---

## Why Choose RetroRec?

RetroRec works differently from every other screen recorder. Instead of encoding everything in real time, it uses a **two-stage ghost pipeline**:

**Stage 1 — Live Capture (mpeg4, 1 thread, ~5% CPU):**
The screen is captured and encoded with MPEG‑4 Part 2 — a lightweight codec that barely touches your CPU. Duplicate frames are skipped automatically.

**Stage 2 — Post-Convert (x264, all cores, after you stop):**
When you press stop, RetroRec converts the recording to x264 at high quality using all CPU cores — when your system is idle.

### The Result:

- Your GPU stays 100% dedicated to your game or desktop
- Recording uses ~5% CPU on any hardware
- Heavy compression happens when you're done recording
- Instant stop — no encoder queue drain freeze like GPU encoders
- Smooth 60 FPS output on any CPU from 2008 onward
- No GPU required. No GPU encoding used. No NVENC. No AMF. CPU only.

---

## System Requirements

| Component | Minimum |
|---|---|
| **Operating System** | Windows 7 SP1 / Server 2008 R2 or later |
| **CPU** | Any dual-core x86_64 processor (SSE2) |
| **RAM** | 4 GB |
| **GPU** | Any. Integrated. None. All work. |
| **Storage** | Any HDD or SSD |

*\*No dedicated GPU or GPU hardware encoding required.*

---

## Quick Start

1. Download `RetroRec_v1.6.0.zip` and extract all files to a folder.
2. Make sure `maxsengine.exe` is in the same folder as `RetroRec.exe`.
3. Double-click **`RetroRec.exe`** — a small always-on-top window appears.
4. Press **F10** to start recording.
5. Press **F10** again to stop.
6. Wait a few seconds for post-processing — your video opens automatically in Explorer.

---

## Configuration

All settings are stored in `RetroRec.ini` next to the executable. You can edit it while RetroRec is running — changes take effect within 2 seconds, no restart needed.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
ConvertPreset=medium
ConvertPreset options: medium (default, best quality), veryfast (balanced), ultrafast (fastest for older machines).
```

## Capture Methods

RetroRec automatically selects the best capture method for your Windows version:

| Windows Version | Capture Method | Details |
|---|---|---|
| Windows 10 / 11 | gfxcapture (D3D11) | GPU zero-copy, cursor visible, 60 FPS VFR |
| Windows 8 / 8.1 | ddagrab (DirectDraw) | GPU DirectDraw, 60 FPS VFR |
| Windows 7 / Vista | gdigrab (GDI) | CPU software, 55 FPS cap |

---

## What RetroRec Does Not Do (Yet)

- **Exclusive fullscreen game capture** — RetroRec captures via the Desktop Window Manager. Games running in *Exclusive Fullscreen* mode bypass the DWM and may record as a black screen. Use **Borderless Windowed** mode as a workaround.
- **GPU hardware encoding** — RetroRec intentionally uses CPU-only encoding to avoid the stop-button freeze and hardware compatibility issues that plague GPU encoders.
- **Streaming** — RetroRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per-window capture** — RetroRec captures the entire monitor.

---

## Building from Source

### Requirements

- GCC (MinGW-w64 UCRT64)
- Windows SDK
- GDI+ and COMCTL32 development headers

### Linker Libraries

```
comctl32.lib, shell32.lib, user32.lib, gdi32.lib, kernel32.lib,
advapi32.lib, ole32.lib, uuid.lib, ksuser.lib, avrt.lib,
gdiplus.lib, comdlg32.lib
```

### Compile Command

```bash
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    -finput-charset=UTF-8 -fexec-charset=UTF-8 \
    -o RetroRec.exe src/RetroRec.cpp \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt -lgdiplus -lcomdlg32
```

> ⚠️ **Important:** The `-D_WIN32_WINNT=0x0A00` flag is strictly required. Without it, the binary targets Windows XP compatibility, causing the recording pipeline to fail with 0 FPS.

---

## License & Credits

- RetroRec is free software. Use it, modify it, and share it.
- Built by **MaxRBLX1**.
- **Max'sEngine™** powered by **FFmpeg** ([ffmpeg.org](https://ffmpeg.org)).
- Audio capture based on Microsoft WASAPI sample code.
```
