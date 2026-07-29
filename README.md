# PhantomRec — "Record with lightweight compression now. Encode with heavy compression later."
## Built by MaxRBLX1 — v1.9.6

## Project History
PhantomRec was originally released as RetroRec (v1.0 – v1.7). The name was changed in v1.8.
All recordings and settings from previous versions are fully compatible.

## A Note on Zero Performance Impact
When you record using DDAGRAB or GFX Capture, it will steal FPS from your game because it makes your GPU do the heavy lifting — even if you don't have a GPU or have a GPU with no encoder. If you want no performance impact, use GDIGrab. This works in system memory, meaning your GPU is 100% focused on the game, but it gives a 30fps recording.

## What is PhantomRec?
PhantomRec is a free, portable, invisible screen recorder for Windows.
It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high‑quality file.

## No GPU? No problem. Old laptop? It works.
## PhantomRec runs on any Windows PC from Vista to Windows 11, from a dual‑core budget machine to a high‑end workstation.

## A Note on Windows Versions
PhantomRec doesn't care what hardware you have — it cares about your OS, because that determines which capture APIs are available.

## Windows Version	Capture Method	Typical FPS
1. Windows 10 / 11	GFX (D3D11 zero‑copy)	60 FPS
2. Windows 8 / 8.1	DDAGrab (DXGI)	60 FPS
3. Windows 7 / Vista	GDI (CPU software)	Up to 30 FPS (BitBlt)
### Fallback chain: GFX → DDAGrab → GDI. If a method isn't supported, PhantomRec automatically drops to the next best option. GDI is the universal fallback.

## All capture methods write a lossless master file at the native frame rate. The final video is automatically converted to a constant 60 fps x264 file, regardless of the source frame rate.

## Why Choose PhantomRec?
PhantomRec uses a two‑stage ghost pipeline — the same architecture that made Fraps legendary, rebuilt for modern lossless codecs.

Stage 1 — Live Capture (Ut Video lossless, ~5% CPU)
The screen is captured and encoded with Ut Video — a mathematically lossless, intra‑frame codec.

Parallel encoding: uses -slices equal to your CPU core count (1 slice for dual‑core systems to avoid thread trashing), keeping CPU usage flat at ~5% regardless of on‑screen action.

Duplicate frames are skipped automatically.

Massive queues: video -thread_queue_size 4096, audio -thread_queue_size 4096, and -max_muxing_queue_size 50000 to absorb HDD write pauses — zero lag even on slow mechanical drives.

No GPU encoding means the stop button responds instantly — no encoder queue drain freeze.

Stage 2 — Post‑Convert (x264 ultrafast, after you stop)
When you press stop, PhantomRec converts the lossless master to a crisp, compact x264 file at 60 FPS using all CPU cores — when your system is idle.
You get NVENC‑quality file sizes without needing a GPU.

The Result
Your GPU stays 100% dedicated to your game or desktop.

Recording uses ~5% CPU on any hardware.

Heavy compression happens when you're done recording.

Instant stop — no encoder queue drain freeze.

Smooth output on any CPU from 2008 onward.

No GPU required. No NVENC. No AMF. CPU only.

## What's New in v1.9.6
> This is a bug‑fix release — the most critical issues from v1.9.5 have been resolved:

1. 🎯 Fixed CMD console window staying open — the console closes automatically after FFmpeg exits.

2. 🔧 Fixed FFmpeg not exiting cleanly — the process now stops reliably every time (attaches console, sends CTRL_BREAK/CTRL_C, force‑kills if needed).

3. 🔇 Fixed audio thread hang — pipe write handle is closed before waiting for the audio thread.

4. 🔁 Fixed progress bar after pause/resume — duration now correctly subtracts paused time.

5. 📦 Fixed fragmented files — segments are always concatenated into a single lossless file when ConvertAfterRecording=no.

6. 🧵 Fixed race conditions — all state flags now use Interlocked operations.

7. 🎨 Fixed font handle leaks — UI resources are properly cleaned up in WM_DESTROY.

8. ⚡ Added dual‑core optimization — -slices 1 is used for CPUs with 2 or fewer cores to eliminate thread trashing.

9. 🚀 Direct FFmpeg launch — no cmd.exe wrapper, giving full control over the FFmpeg process.

## Settings — How to Control
All settings are in Settings.ini (same folder as PhantomRec.exe).
Edit it while the program is running — changes take effect within 2 seconds (only hotkeys and appearance are applied mid‑recording; capture method and conversion flag are deferred until idle).

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
## Setting	Description
1. Hotkey	F1‑F12 for function keys, or a single letter for Ctrl+ (e.g. R = Ctrl+R).
PauseHotkey	Same format.
2. ConvertAfterRecording	yes = automatically compress after recording (recommended).
no = keep the raw segment files (*_segN_temp.mkv + segments.txt) in the output folder. You must manually concatenate or delete them.
3. CaptureMethod	auto (default), gfx, ddagrab, gdi.

##⚠️ Important: When ConvertAfterRecording=no
> PhantomRec does not concatenate or delete the segment files. Instead, it leaves:

> Multiple *_segN_temp.mkv files (each is a lossless chunk of your recording)

> A segments.txt file that lists these chunks in the correct order

> To turn these chunks into a single playable video, you must manually concatenate them using maxsengine.exe (the included FFmpeg build).

## How to Concatenate Manually
1. Open the folder where PhantomRec saves your recordings.
(Default: C:\Users\[YourUsername]\Videos\PhantomRec)

2. In that folder, locate the segments.txt file and all the *_segN_temp.mkv files.

3. Open Command Prompt in that exact folder:

4. Hold Shift + right‑click inside the folder window.

5. Select "Open PowerShell window here" or "Open command window here".

> (Alternatively, type cmd in the folder's address bar and press Enter.)

Run this command:

```cmd
maxsengine.exe -f concat -safe 0 -i segments.txt -c copy output.mkv
```
> Wait for the process to finish. You'll now have a single file named output.mkv — this is your complete recording.

> You can safely delete the *_segN_temp.mkv files and segments.txt to free up space.

**💡 Tip: Rename output.mkv to something meaningful (e.g., my_gameplay.mkv) before moving or sharing it.**

## Building from Source
> Requirements
> MinGW‑w64 (UCRT64)
> Windows SDK

Compile
bash
```
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
**⚠️ -D_WIN32_WINNT=0x0A00 is strictly required. Without it, the binary targets XP compatibility and the recording pipeline fails with 0 FPS.**

## What PhantomRec Does Not Do (Yet)
> Streaming — PhantomRec is a recorder, not a streaming tool.
> Webcam overlay — Not supported.
> Per‑window capture — PhantomRec captures the entire monitor.

## License & Credits
PhantomRec is free software. Use it, modify it, share it.

Built by a single developer: MaxRBLX1.
Max'sEngine™ powered by FFmpeg (ffmpeg.org).
Audio capture based on Microsoft WASAPI sample code.

"Every screen deserves to be recorded."
