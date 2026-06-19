**"Every screen deserves to be recorded."**  
*Built by MaxRBLX1*

---

## What is RetroRec?

RetroRec is a **free, portable, and invisible screen recorder** for Windows.  
It captures your desktop (or game) at a smooth 60 fps with system audio, then converts the recording into a compact, high‑quality file — all without slowing down your computer.

No GPU? No problem. Old laptop? It works.  
RetroRec is designed to run on **any Windows 10/11 PC**, from a dual‑core budget machine to a high‑end workstation.

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

RetroRec works differently from other recorders. Instead of encoding everything in real time, it:

1. **Records ultra‑fast MJPEG video + raw audio** – no heavy compression, almost zero CPU use.
2. **Skips duplicate frames** when your screen is static – saving disk space and CPU cycles.
3. **Converts to a polished x264 video after you press stop** – when your system is idle.

This two‑stage pipeline means **no lag while recording**, even on weak hardware.

---

## Features

- **60 fps smooth capture** using modern Windows Graphics Capture (or classic DDAGrab fallback).
- **System audio recording** via WASAPI loopback – no extra drivers needed.
- **Yellow recording border** shows you exactly when capture is active.
- **Zero‑config auto‑tuning** – RetroRec detects your CPU cores and adjusts quality automatically.
- **Portable** – runs from a USB stick, no installation.
- **Configurable hotkeys** (F1‑F12).
- **Open‑source** – you can read, modify, and share the code.

---

## System Requirements

| Component | Minimum |
|-----------|---------|
| **Operating System** | Windows 10 version 2004 or later / Windows 11 |
| **CPU** | Any dual‑core x86_64 processor |
| **RAM** | 4 GB |
| **GPU** | DirectX 11 capable integrated graphics |
| **Storage** | Any HDD or SSD (even slow drives work well) |

*No dedicated GPU required.*

---

## Configuration

All settings are stored in the **`RetroRec.ini`** file next to the executable.

```ini
[Settings]
; Hotkey to start/stop recording (F1 to F12)
Hotkey=F10

; Convert to a compact x264 file after recording (yes/no)
ConvertAfterRecording=yes

; x264 preset (fast = quicker, medium = slightly better quality)
ConvertPreset=fast

; Capture engine (gfx = modern, ddagrab = legacy fallback)
CaptureEngine=gfx

; Show a yellow border while recording (yes/no)
ShowYellowBorder=yes
You can open this file directly from the Settings button inside the app.

Recording Output
Recordings are saved in your Videos\RetroRec folder.
After stopping, you’ll find two files:

RetroRec_YYYYMMDD_HHMMSS.mkv – the final, compact video.

The intermediate temporary file is automatically deleted after conversion.

How to Use the Settings Button
Click the Settings button on the RetroRec window.

The RetroRec.ini file opens in Notepad.

Change any setting, save the file, and restart RetroRec (the new settings take effect on the next launch).

Troubleshooting
“ffmpeg.exe not found!”
Make sure ffmpeg.exe is in the same folder as RetroRec.exe.
You can download a static build from ffmpeg.org.

No audio in the recording?
Check that your system sound is not muted.

RetroRec uses the default playback device – make sure it’s set correctly in Windows Sound settings.

Recording is lagging or stuttering?
Lower the MJPEG quality by editing the -q:v value in the code (advanced).

Switch to CaptureEngine=ddagrab in the INI file if GFX capture isn’t working well on your system.

The yellow border doesn’t appear?
This may happen if you have an older Windows version. Switch to DDAGrab capture, or simply ignore it – the recording still works perfectly.

Building from Source
If you want to compile RetroRec yourself:

Requirements: GCC (MinGW‑w64) or MSVC, Windows SDK.

Libraries: avrt.lib, ole32.lib, comctl32.lib.

Compile:

bash
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    -o RetroRec.exe RetroRec.cpp \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt
License & Credits
RetroRec is free software. Use it, modify it, share it.
Built by MaxRBLX1 with the help of the community.

Special thanks to the FFmpeg project and Microsoft’s WASAPI sample code.

Enjoy the ghost. 👻
