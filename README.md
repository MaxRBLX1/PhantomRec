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

| Windows Version | Capture Method | Recording FPS | Playback FPS |
|---|---|---|---|
| Windows 10 / 11 | GFX (GPU zero-copy) | 60 VFR | 60 |
| Windows 8 / 8.1 | DDAGrab (GPU) | 60 VFR | 60 |
| Windows 7 / Vista | GDI (CPU software) | 55 cap | 30 |

Windows 7/Vista lack GPU-accelerated capture APIs. PhantomRec still works — but your OS caps playback at 30 FPS.

**The fix is free:** Upgrade to Windows 8.1, 10, or 11 on the same hardware. Your CPU, RAM, and GPU haven't changed — but now the capture APIs exist, and PhantomRec uses them.

---

## Why Choose PhantomRec?

PhantomRec uses a **two-stage ghost pipeline** — the same architecture that made Fraps legendary, rebuilt for modern codecs.

**Stage 1 — Live Capture (mpeg4, 1 thread, ~5% CPU):**
The screen is captured and encoded with MPEG‑4 Part 2 — a lightweight codec that barely touches your CPU. Duplicate frames are skipped automatically. No GPU encoding means no stop-button freeze.

**Stage 2 — Post-Convert (x264, all cores, after you stop):**
When you press stop, PhantomRec converts the recording to x264 at high quality using all CPU cores — when your system is idle. You get NVENC-quality file sizes without needing a GPU.

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
| **Storage** | Any HDD or SSD |

*\*No dedicated GPU or GPU hardware encoding required.*

---

## Quick Start

1. Download the latest release and extract all files.
2. Make sure `maxsengine.exe` is in the same folder as `PhantomRec.exe`.
3. Double-click `PhantomRec.exe` — a small always-on-top window appears.
4. Press **F10** to start recording. Press **F10** again to stop.
5. Wait a few seconds for post-processing — your video opens automatically.

---

## What's New in v1.9.5

- **Pure C Core** — Capture engine rewritten in C11. No STL. No exceptions. Zero allocations during recording.
- **C++ UI** — Clean separation. Win32 + GDI+. Core and UI linked through `extern "C"`.
- **Smart Capture Fallback** — GFX → DDAGrab → GDI. Every method degrades gracefully if the OS doesn't support it.
- **Customization** — Custom backgrounds (PNG, JPG, BMP, GIF), custom fonts (TTF/OTF), font size, font color.
- **Animated GIF Backgrounds** — Smooth frame transitions, no ghosting.
- **INI Hot-Reload** — Edit `PhantomRec.ini` while running. Changes apply within 2 seconds.
- **No UI Stacking** — Text and backgrounds render clean on every update.

---

## Configuration

All settings in `PhantomRec.ini`. Edit while running — no restart needed.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
ConvertPreset=veryfast
CaptureMethod=auto
```

**CaptureMethod options:** `auto` (default), `gfx`, `ddagrab`, `gdi`
**ConvertPreset options:** `medium` (best quality), `veryfast` (balanced), `ultrafast` (fastest)

```ini
[Appearance]
Background=C:\path\to\image.png
Font=C:\path\to\font.ttf
FontSize=14
FontColor=16777215
```

## Building from Source

### Requirements
- MinGW-w64 (UCRT64)
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
    -ladvapi32 -lole32 -luuid -lksuser -lavrt -lgdiplus -lcomdlg32
```

⚠️ **`-D_WIN32_WINNT=0x0A00` is strictly required.** Without it, the binary targets XP compatibility and the recording pipeline fails with 0 FPS.

---

## Project Tree

| | |
|---|---|
| `PhantomRec/` | |
| `├── src/` | |
| `│   ├── PhantomRec.cpp` | C++ UI (Win32 + GDI+) |
| `│   ├── phantomrec_core.c` | Pure C core |
| `│   └── phantomrec_core.h` | Shared header (extern "C" bridge) |
And remaining or licence and Readme and .gitgnore

---

## What PhantomRec Does Not Do (Yet)

- **Streaming** — PhantomRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per-window capture** — PhantomRec captures the entire monitor.

---

## License & Credits

PhantomRec is free software. Use it, modify it, share it.

Built by **MaxRBLX1**.

**Max'sEngine™** powered by **FFmpeg** ([ffmpeg.org](https://ffmpeg.org)).

Audio capture based on Microsoft WASAPI sample code.
