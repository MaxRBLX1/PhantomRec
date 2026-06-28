# PhantomRec — "Record now. Encode later. Zero lag."

**Built by MaxRBLX1**

---

## What is PhantomRec?

PhantomRec is a **free, portable, and invisible screen recorder** for Windows. It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high-quality file — all without slowing down your computer.

No GPU? No problem. Old laptop? It works. PhantomRec is designed to run on **any Windows PC from Vista to Windows 11**, from a dual-core budget machine to a high-end workstation.

---

## Why Choose PhantomRec?

PhantomRec works differently from every other screen recorder. Instead of encoding everything in real time, it uses a **two-stage ghost pipeline**:

**Stage 1 — Live Capture (mpeg4, 1 thread, ~5% CPU):**
The screen is captured and encoded with MPEG‑4 Part 2 — a lightweight codec that barely touches your CPU. Duplicate frames are skipped automatically. No GPU encoding means no stop-button freeze.

**Stage 2 — Post-Convert (x264, all cores, after you stop):**
When you press stop, PhantomRec converts the recording to x264 at high quality using all CPU cores — when your system is idle. You get NVENC-quality file sizes without needing a GPU.

### The Result:

- Your GPU stays 100% dedicated to your game or desktop
- Recording uses ~5% CPU on any hardware
- Heavy compression happens when you're done recording
- **Instant stop** — no encoder queue drain freeze like GPU encoders
- Smooth 60 FPS output on any CPU from 2008 onward
- **No GPU required.** No GPU encoding used. No NVENC. No AMF. CPU only.

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

1. Download `PhantomRec_v1.7.0.zip` and extract all files to a folder.
2. Make sure `maxsengine.exe` is in the same folder as `PhantomRec.exe`.
3. Double-click **`PhantomRec.exe`** — a small always-on-top window appears.
4. Press **F10** to start recording.
5. Press **F10** again to stop.
6. Wait a few seconds for post-processing — your video opens automatically in Explorer.

---

## What's New in v1.7

- **Exclusive Fullscreen Capture** — Games in exclusive fullscreen mode are now captured correctly. PhantomRec keeps the DWM alive so `gfxcapture` never loses the signal.
- **Auto Game Detection** — Press F10 and PhantomRec automatically finds the game you're playing. No manual window selection needed.
- **Safe Window Handling** — Fragile game engines (Geometry Dash, older OpenGL titles) are automatically detected and protected from window modification.
- **Taskbar Anchor** — Uses the same DWM loophole as Xbox Game Bar to capture exclusive fullscreen games with zero performance loss.

---

## Configuration

All settings are stored in `PhantomRec.ini` next to the executable. You can edit it while PhantomRec is running — changes take effect within 2 seconds, no restart needed.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
ConvertPreset=medium
ConvertPreset options: medium (default, best quality), veryfast (balanced), ultrafast (fastest for older machines).
```

## Capture Methods

PhantomRec automatically selects the best capture method for your Windows version:

| Windows Version | Capture Method | Details |
|---|---|---|
| Windows 10 / 11 | `gfxcapture` (D3D11) | GPU zero-copy, cursor visible, 60 FPS VFR |
| Windows 8 / 8.1 | `ddagrab` (DirectDraw) | GPU DirectDraw, 60 FPS VFR |
| Windows 7 / Vista | `gdigrab` (GDI) | CPU software, 55 FPS cap |

---

## What PhantomRec Does Not Do (Yet)

- **Streaming** — PhantomRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per-window capture** — PhantomRec captures the entire monitor.

## Building from Source

### Requirements

- GCC (MinGW-w64 UCRT64)
- Windows SDK
- GDI+ and COMCTL32 development headers

### Linker Libraries
comctl32.lib, shell32.lib, user32.lib, gdi32.lib, kernel32.lib,
advapi32.lib, ole32.lib, uuid.lib, ksuser.lib, avrt.lib,
gdiplus.lib, comdlg32.lib

### Compile Command

```bash
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    -finput-charset=UTF-8 -fexec-charset=UTF-8 \
    -o PhantomRec.exe src/PhantomRec.cpp \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt -lgdiplus -lcomdlg32
⚠️ Important: The -D_WIN32_WINNT=0x0A00 flag is strictly required. Without it, the binary targets Windows XP compatibility, causing the recording pipeline to fail with 0 FPS.
```

## Project History

PhantomRec was originally released as **RetroRec** (v1.0 through v1.7). The name was changed in v1.8 to better reflect the software's modern capabilities and universal appeal. All recordings and settings from previous versions are fully compatible with PhantomRec.

---

## License & Credits

- PhantomRec is free software. Use it, modify it, and share it.
- Built by **MaxRBLX1**.
- **Max'sEngine™** powered by **FFmpeg** ([ffmpeg.org](https://ffmpeg.org)).
- Audio capture based on Microsoft WASAPI sample code.
