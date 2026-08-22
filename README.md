# PhantomRec — "Record with lightweight compression now. Encode with heavy compression later."

<img width="256" height="256" alt="Untitled" src="https://github.com/user-attachments/assets/9dec6e78-d9d8-4e4f-a491-fe130d9bc978" />  *The Icon Resembles "M" for MaxRBLX1*

**Built by MaxRBLX1 — v1.9.7**

## Project History

PhantomRec was originally released as RetroRec (v1.0 – v1.7). The name was changed in v1.8.
All recordings and settings from previous versions are fully compatible.

## Zero Performance Impact – How PhantomRec Works

PhantomRec uses a **two-stage pipeline** that keeps your GPU 100% focused on your game.

### Stage 1: Capture (GPU copy + CPU lossless encoding)

- **GFX / DDAGrab** use the GPU's **copy engine** to read the framebuffer – lightweight, minimal impact.
- **ffvhuff** (CPU, ~5%) encodes losslessly – **no GPU encoding** means zero FPS theft.
- **GDIGrab** (CPU fallback) works on any system, but is slower (30 FPS).

### Stage 2: Compression (after recording)

- Heavy **x264** compression runs **when you're idle** – not during gameplay.
- The stop button responds instantly – no encoder queue drain freeze.

### Future: Game Capture via Hooking

For **exclusive fullscreen games**, PhantomRec will eventually capture raw frames by hooking the game's `Present()` call. This bypasses the Desktop Window Manager (DWM) entirely – **zero added latency, zero FPS drop**, and perfect frame pacing for every game, from retro classics to modern AAA titles.

- **No DWM overhead** – the game talks directly to the display.
- **No added input lag** – the hook never blocks the render loop.
- **Works on all APIs** – DirectX 9, 10, 11, 12, OpenGL, Vulkan (via runtime detection, no hardcoding).
- **Legacy support** – DirectX 7/8 games captured via wrapper libraries (e.g., `d3d8to9`).

### The Result

- GPU stays 100% dedicated to your game.
- Recording uses ~5% CPU on any hardware.
- No GPU encoder required – works on any PC from 2008 onward.
- **Future:** Universal game capture for every exclusive fullscreen title, old or new.

## System Requirements

PhantomRec is designed to work on **any PC** — no exceptions, no lockouts
- CPU: Any 64-bit CPU
- GPU: **Kepler** or newer (Tesla and fermi works but games will lag) for AMD **AMD GCN 1.0+** or newer (AMD TeraScale HD 6000 or older works but games will lag)
- RAM: 4 GB minimum because of thread queue size 4096
- OS: Windows Vista to Windows 11
- Storage: Any HDD or SSD

---

### What This Means for Your GPU

| GPU Type | What Happens | Locked Out? |
| :--- | :--- | :--- |
| **NVIDIA Kepler+ (GTX 600+)** | ✅ Full DDAGrab/GFX support. 60 FPS recording. | ❌ No |
| **AMD GCN 1.0+ (HD 7000+)** | ✅ Full DDAGrab/GFX support. 60 FPS recording. | ❌ No |
| **NVIDIA Tesla/Fermi (GTX 400/500)** | ⚠️ All APIs work, but games will lag because the GPU itself is too old to run modern API that PhantomRec uses properly. Recording still works via GDI fallback. | ❌ No |
| **AMD TeraScale (HD 6000 or older)** | ⚠️ All APIs work, but games will lag because the GPU itself is too old to run modern modern API that PhantomRec uses properly. Recording still works via GDI fallback. | ❌ No |
| **No GPU (Microsoft Basic Display Adapter)** | ✅ PhantomRec uses GDI + CPU encoding. Recording is still smooth. | ❌ No |

---

### The Honest Explanation

> **All GPUs work with PhantomRec — no one is locked out.**

- **If you have a modern GPU** (NVIDIA Kepler+, AMD GCN 1.0+), you get **60 FPS** recording with DDAGrab/GFX. These APIs are fast and efficient.

- **If you have an older GPU** (NVIDIA Tesla/Fermi, AMD TeraScale), PhantomRec **still uses all capture APIs** but your games will lag because the GPU itself is too old to run the modern APIs that PhantomRec uses properly. Recording still works via GDI fallback. If you experience lag while using GDI, your CPU is the bottleneck, not the GPU.

- **If you have no GPU** (Microsoft Basic Display Adapter), PhantomRec uses **GDI + CPU encoding**. Recording is still smooth.

---

### Why These Requirements?

| Requirement | Why It's There |
| :--- | :--- |
| **Any 64-bit CPU** | PhantomRec uses the CPU for encoding — not the GPU. No special instructions required. |
| **Kepler or newer GPU** | Kepler and GCN 1.0 are the baseline for modern APIs like DDAGrab/GFX. Older GPUs (Tesla/Fermi/TeraScale) still work, but modern games will lag because the hardware is too old. |
| **4 GB RAM** | PhantomRec uses large thread queues (`4096`) to absorb HDD write pauses. You need enough memory to buffer frames and audio. |
| **Windows Vista to 11** | GDI capture works on all Windows versions from Vista onward. GFX and DDAGrab work on newer versions. |
| **Any HDD or SSD** | The pipeline is designed to handle slow mechanical drives. You don't need an SSD to record smoothly. |

---

### No Minimum. No Maximum. No Lockouts.

PhantomRec works on **what you already have**.

- No expensive GPU required.
- No forced upgrades.
- No artificial limits.

## What is PhantomRec?

PhantomRec is a free, portable, invisible screen recorder for Windows.
It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high‑quality file.

No GPU? No problem. Old laptop? It works.
PhantomRec runs on any Windows PC from Vista to Windows 11, from a dual‑core budget machine to a high‑end workstation.

## A Note on Windows Versions

PhantomRec doesn't care what hardware you have — it cares about your OS, because that determines which capture APIs are available.

| Windows Version | Capture Method | Typical FPS |
|-----------------|----------------|-------------|
| Windows 10 / 11 | GFX (D3D11 zero‑copy) | 60 FPS |
| Windows 8 / 8.1 | DDAGrab (DXGI) | 60 FPS |
| Windows 7 / Vista | GDI (CPU software) | Up to 30 FPS (BitBlt) |

Fallback chain: GFX → DDAGrab → GDI. If a method isn't supported, PhantomRec automatically drops to the next best option. GDI is the universal fallback.

All capture methods write a lossless master file at the native frame rate. The final video is automatically converted to a constant 60 fps x264 file, regardless of the source frame rate.

## Why Choose PhantomRec?

PhantomRec uses a two‑stage ghost pipeline — this is new for some recorders


### Stage 1 — Live Capture (ffvhuff lossless, ~5% CPU)

The screen is captured and encoded with ffvhuff — a mathematically lossless, intra‑frame codec.

- **Parallel encoding:** uses `-slices` equal to your CPU core count (1 slice for dual‑core systems to avoid thread trashing), keeping CPU usage flat at ~5% regardless of on‑screen action.
- **Duplicate frames** are skipped automatically.
- **Massive queues:** video `-thread_queue_size 4096`, audio `-thread_queue_size 4096`, and `-max_muxing_queue_size 50000` to absorb HDD write pauses — zero lag even on slow mechanical drives.
- **No GPU encoding** means the stop button responds instantly — no encoder queue drain freeze.

### Stage 2 — Post‑Convert (x264 ultrafast, after you stop)

When you press stop, PhantomRec converts the lossless master to a crisp, compact x264 file at 60 FPS using all CPU cores — when your system is idle.
You get NVENC‑quality file sizes without needing a GPU.

### The Result

- Your GPU stays 100% dedicated to your game or desktop.
- Recording uses ~5% CPU on any hardware.
- Heavy compression happens when you're done recording.
- Instant stop — no encoder queue drain freeze.
- Smooth output on any CPU from 2008 onward.
- No GPU required. No NVENC. No AMF. CPU only.

## What's New in v1.9.7

v1.9.7 is a polish release — building on v1.9.6 with user‑facing improvements and a cleaner experience:

- 🧹 **Removed power‑plan management** – no longer forces High Performance; respects user's power settings and avoids potential issues.
- 🎨 **Fixed UI status flicker** – status updates no longer draw black rectangles, giving a smooth, flicker‑free display.
- 📊 **Progress bar corrected** – removed PBS_MARQUEE and added proper 0–100 range, now correctly shows conversion progress.
- 🖼️ **Added HD icon** – a 256×256 icon (with 48, 32, and 16px fallbacks) embedded via resource.rc, giving a crisp, professional look in taskbar, Start Menu, Alt+Tab, and File Explorer.
- 📄 **Version info embedded** – the .exe now shows FileVersion, ProductName, Copyright, etc. in its Properties dialog.
- 🛠️ **Full UCRT64 compatibility** – verified build process with MSYS2 UCRT64 and windres.

## What's New in v1.9.6

This was a bug‑fix release — the most critical issues from v1.9.5 were resolved:

- 🎯 **Fixed CMD console window staying open** — the console closes automatically after FFmpeg exits.
- 🔧 **Fixed FFmpeg not exiting cleanly** — the process now stops reliably every time (attaches console, sends CTRL_BREAK/CTRL_C, force‑kills if needed).
- 🔇 **Fixed audio thread hang** — pipe write handle is closed before waiting for the audio thread.
- 🔁 **Fixed progress bar after pause/resume** — duration now correctly subtracts paused time.
- 📦 **Fixed fragmented files** — segments are always concatenated into a single lossless file when ConvertAfterRecording=no.
- 🧵 **Fixed race conditions** — all state flags now use Interlocked operations.
- 🎨 **Fixed font handle leaks** — UI resources are properly cleaned up in WM_DESTROY.
- ⚡ **Added dual‑core optimization** — `-slices 1` is used for CPUs with 2 or fewer cores to eliminate thread trashing.
- 🚀 **Direct FFmpeg launch** — no cmd.exe wrapper, giving full control over the FFmpeg process.

## Settings — How to Control

All settings are in `Settings.ini` (same folder as PhantomRec.exe).
Edit it while the program is running — changes take effect within 2 seconds (only hotkeys and appearance are applied mid‑recording; capture method and conversion flag are deferred until idle).

```
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
| Setting | Description |
|---------|-------------|
| `Hotkey` | F1‑F12 for function keys, or a single letter for Ctrl+ (e.g. R = Ctrl+R). |
| `PauseHotkey` | Same format as Hotkey. |
| `ConvertAfterRecording` | `yes` = automatically compress after recording (recommended).<br>`no` = keep the raw segment files (`*_segN_temp.mkv` + `segments.txt`) in the output folder. You must manually concatenate or delete them. |
| `CaptureMethod` | `auto` (default), `gfx`, `ddagrab`, `gdi`. |

## ⚠️ Important: When `ConvertAfterRecording=no`

When `ConvertAfterRecording=no`, PhantomRec **keeps the lossless master file** instead of compressing it to x264.

| Setting | Result |
| :--- | :--- |
| `ConvertAfterRecording=yes` | Lossless capture (`ffvhuff`) → compressed to x264 → lossless file deleted. Final file: small, shareable. |
| `ConvertAfterRecording=no` | Lossless capture (`ffvhuff`) → lossless file kept. Final file: large, perfect quality. |

### File Output

- **`yes` (default):** `PhantomRec_YYYYMMDD_HHMMSS.mkv` – compact x264, ready to share.
- **`no`:** `PhantomRec_YYYYMMDD_HHMMSS_lossless.mkv` – lossless ffvhuff, ideal for editing or re‑encoding later.

> 💡 **Tip:** The lossless file is mathematically identical to what was captured. Use it if you plan to edit or re‑encode later. For everyday sharing, the compressed file is best.

### No Manual Work Needed

PhantomRec **automatically concatenates all segments** into a single file – you never need to manually merge segments or delete temporary files.

## Building from Source

### Requirements

- MinGW‑w64 (UCRT64)
- Windows SDK (included with MinGW)

### Compile

```
# Step 1: Compile the pure C core
gcc -std=c11 -O2 -c src/phantomrec_core.c -o phantomrec_core.o

# Step 2: Compile the resource file (for HD icon & version info)
windres src/resource.rc -O coff -o resource.o

# Step 3: Link with C++ UI
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 \
    phantomrec_core.o src/PhantomRec.cpp resource.o \
    -o PhantomRec.exe \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lksuser -lavrt -lgdiplus -lcomdlg32
```
> ⚠️ `-D_WIN32_WINNT=0x0A00` is strictly required.
> Without it, the binary targets XP compatibility and the recording pipeline fails with 0 FPS.

> **Note:** There is no `-mwindows` flag — the console is intentionally kept visible for transparency, debugging, and to show FFmpeg logs.

## What PhantomRec Does Not Do (Yet)

- **Streaming** — PhantomRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per‑window capture** — PhantomRec captures the entire monitor.
- **Game capture via hooking** — Planned for a future release (exclusive fullscreen support).

## License & Credits

PhantomRec is free software. Use it, modify it, share it.

Built by a single developer: **MaxRBLX1**
Max'sEngine™ powered by FFmpeg (ffmpeg.org)
Audio capture based on Microsoft WASAPI sample code.

> "Every screen deserves to be recorded."
