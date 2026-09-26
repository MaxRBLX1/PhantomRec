# PhantomRec — "Record with lightweight compression now. Encode with heavy compression later."

<img width="256" height="256" alt="Untitled" src="https://github.com/user-attachments/assets/9dec6e78-d9d8-4e4f-a491-fe130d9bc978" />  *The Icon Resembles "M" for MaxRBLX1*

**Built by MaxRBLX1 — v1.9.8**

## Project History

PhantomRec was originally released as RetroRec (v1.0 – v1.7). The name was changed in v1.8.
All recordings and settings from previous versions are fully compatible.

---

## What is PhantomRec?

PhantomRec is a free, portable, invisible screen recorder for Windows.
It captures your desktop with system audio, then converts the recording into a compact, high-quality file after you stop.

No GPU? No problem. Old laptop? It works.
PhantomRec runs on Windows 7 SP1 through Windows 11, from a dual-core budget machine to a high-end workstation.

The design principle is simple: **the GPU belongs to the game, not the recorder.** PhantomRec never calls NVENC, AMF, or QuickSync. Encoding happens on the CPU, which leaves the GPU encoder graph flat at 0% while you record.

---

## How PhantomRec Works — The Two-Stage Ghost Pipeline

### Stage 1 — Live Capture (MJPEG, CPU-only)

- **GFX Capture / DDAGrab** use the GPU's copy engine to read the framebuffer, then hand the raw pixels to the CPU. One blit per frame, no GPU-side format conversion.
- **MaxRBLX1's Fastest MJPEG** (`maxenc.exe`) encodes each frame with **libjpeg-turbo** (SIMD-accelerated JPEG) using a single thread on a pinned CPU core. This is a high-quality intermediate, not a lossless master.
- **GDI fallback** works on any Windows version and any GPU — including Microsoft Basic Display Adapter — at up to 30 FPS.
- The GPU encoder graph stays at **0%** because PhantomRec never touches it.

Every frame is timestamped with a wall-clock PTS from `QueryPerformanceCounter`, not a frame counter. That means the timing stays correct even when the capture source doesn't deliver exactly N frames per second — which it never does, especially during the first seconds of startup.

Encoded frames are handed to a **bounded writer ring** (64 slots). A separate thread muxes them to disk. A short disk stall — Defender scanning the growing file, an HDD seek, SMB latency — cannot blow the frame budget. On overflow, the oldest queued frame is dropped, which is the right policy for a screen recorder.

**Audio** is captured separately by `maxsound.exe` using WASAPI loopback. Video and audio each write a `.t0` marker file with the QPC tick at their first sample. At mux time, PhantomRec reads both markers and offsets the audio track by the exact measured delta, so audio t=0 lines up with video t=0.

### Stage 2 — Post-Convert (x264 ultrafast, when you stop)

When you press STOP, PhantomRec muxes the video and audio together, then encodes the result with **x264 ultrafast** on all available CPU cores — after the game or app you were recording has been closed. You get a compact, shareable file without the recorder ever competing for GPU time.

---

## The Result

- GPU encoder stays at 0% — no encoding on the GPU, ever.
- Recording uses roughly **half of one CPU core** on a modern 4-core machine (measured: `maxenc.exe` at ~14% of total CPU on 4 cores).
- Heavy compression happens after you stop, not during recording.
- No GPU encoder required. No NVENC. No AMF. No QuickSync. CPU only.
- Runs on hardware from 2 cores up — verified on an Intel Core 2 Quad Q9550 limited to 2 cores.

---

## System Requirements

PhantomRec is designed to work on **any PC from Windows 7 SP1 onward** — no GPU lockouts.

| Requirement | What You Actually Need |
| :--- | :--- |
| **CPU** | Any 64-bit x86 CPU with SSE2. The encoder is single-threaded per capture pipeline, so one usable core is the floor. |
| **RAM** | 4 GB. The audio pipe, JPEG buffers, and writer ring are bounded, so memory use is flat. |
| **OS** | Windows 7 SP1 through Windows 11. GFX and DDAGrab need newer Windows; GDI is the universal fallback. |
| **Storage** | Any HDD or SSD. The intermediate MJPEG stream is 30–50× smaller than a lossless codec, so even a slow mechanical drive can keep up. |
| **GPU** | None required. Any GPU works. See the table below. |

---

## What This Means for Your GPU

| GPU Type | What Happens | Locked Out? |
| :--- | :--- | :--- |
| **NVIDIA Kepler+ (GTX 600+)** | Full GFX/DDAGrab support. 60 FPS recording. | ❌ No |
| **AMD GCN 1.0+ (HD 7000+)** | Full GFX/DDAGrab support. 60 FPS recording. | ❌ No |
| **NVIDIA Tesla / Fermi (GTX 400/500)** | Capture still works via GDI fallback. GFX/DDAGrab may be slow on some titles because the GPU itself is old. | ❌ No |
| **AMD TeraScale (HD 6000 or older)** | Capture still works via GDI fallback. Same caveat as above. | ❌ No |
| **No GPU (Microsoft Basic Display Adapter)** | GDI capture + CPU encoding. Recording is smooth. | ❌ No |

**No one is locked out.** If you have a modern GPU, you get GFX/DDAGrab at 60 FPS. If you have an older one, PhantomRec falls back to GDI and still records. GDI is CPU-bound; if it drops frames, your CPU is the limit, not the GPU.

---

## A Note on Windows Versions

PhantomRec doesn't care what hardware you have — it cares about your OS, because that determines which capture APIs are available.

| Windows Version | Capture Method | Typical FPS |
| :--- | :--- | :--- |
| Windows 10 / 11 | GFX (Windows.Graphics.Capture, D3D11) | 60 FPS |
| Windows 8 / 8.1 | DDAGrab (DXGI Desktop Duplication) | 60 FPS |
| Windows 7 SP1 | GDI (CPU BitBlt) | Up to 30 FPS |

**Fallback chain:** GFX → DDAGrab → GDI. If a method isn't supported on your system, PhantomRec automatically drops to the next one. GDI is the universal fallback.

---

## What's New in v1.9.8

v1.9.8 is the biggest update in months. Two months of work, one goal: smooth recording on any PC.

### Architecture changes

- **Replaced ffvhuff with MaxRBLX1's Fastest MJPEG.** libjpeg-turbo on a single pinned core. Lower CPU cost, smaller intermediate files, and it stays out of the GPU's way completely.
- **Bounded writer ring.** Encoded frames go into a 64-slot ring, and a separate writer thread muxes them to disk. Disk stalls no longer drop frames.
- **`maxsound.exe` for audio.** Dedicated WASAPI loopback capture process instead of an in-process audio thread.
- **`.t0` marker-based A/V alignment.** Video and audio each write a QPC tick marker at their first sample. PhantomRec reads both and applies the exact measured offset with `adelay` or `atrim` at mux time.
- **Removed forced frame rate from Stage 2.** The output inherits its frame rate from the intermediate instead of being re-timed, which eliminates the A/V drift that older versions had.
- **Persistent `tjhandle` and `AVPacket`.** No per-frame allocation in the in-process MJPEG fallback.

### Bug fixes

- Fixed a crash on STOP where PhantomRec could receive its own `CTRL_BREAK_EVENT` and exit.
- Fixed the segment-0 overwrite in the standalone `maxenc.exe` pause path.
- Fixed the audio child's inherited handle list so ffmpeg's stderr no longer goes to a stale handle.
- Fixed `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` linkage on MSYS2 UCRT64.
- In-process capture thread now distinguishes `AVERROR(EAGAIN)` from EOF and no longer loops on permanent source failure.

### Known issues (targeted for 1.9.9)

- **Dual-core STOP and PAUSE can feel slow.** On 2-core systems, PhantomRec and its child processes currently share a CPU core, so the console `q` signal can take up to a second to be processed. The fix is child-process CPU affinity, which will land in 1.9.9. **Recording itself is unaffected.**
- **The UI thread blocks during Stage 2 conversion.** On long recordings, the window will be unresponsive while x264 runs. A worker-thread rewrite is planned for 1.9.9.

---

## What's New in v1.9.7

Polish release:

- 🧹 Removed power-plan management — no longer forces High Performance.
- 🎨 Fixed UI status flicker — no more black rectangles during updates.
- 📊 Progress bar corrected — proper 0–100 range, no `PBS_MARQUEE`.
- 🖼️ Added HD icon — 256×256 with 48, 32, and 16 px fallbacks.
- 📄 Version info embedded in the `.exe`.
- 🛠️ Verified build on MSYS2 UCRT64 with `windres`.

## What's New in v1.9.6

Bug-fix release:

- 🎯 Fixed CMD console window staying open after recording.
- 🔧 Fixed FFmpeg not exiting cleanly — process stops reliably every time.
- 🔇 Fixed audio thread hang — pipe write handle closed before waiting.
- 🔁 Fixed progress bar after pause/resume.
- 📦 Fixed fragmented files — segments always concatenate into a single file.
- 🧵 Fixed race conditions — all state flags use Interlocked operations.
- 🎨 Fixed font handle leaks.
- 🚀 Direct FFmpeg launch — no `cmd.exe` wrapper.

---

## Settings — How to Control

All settings are in `Settings.ini` (same folder as `PhantomRec.exe`).
Edit it while the program is running — hotkeys and appearance apply within 2 seconds. Capture method and conversion flag are deferred until idle.

```ini
[Settings]
Hotkey=F10
PauseHotkey=P
ConvertAfterRecording=yes
CaptureMethod=auto
MJPEGQuality=85

[Appearance]
Background=C:\path\to\image.png
Font=C:\path\to\font.ttf
FontSize=14
FontColor=16777215
```

| Setting | Description |
| :--- | :--- |
| `Hotkey` | `F1`–`F12` for function keys, or a single letter for `Ctrl+letter` (e.g. `R` = Ctrl+R). |
| `PauseHotkey` | Same format as `Hotkey`. |
| `ConvertAfterRecording` | `yes` = compress after recording (recommended). `no` = keep the high-quality intermediate file. |
| `CaptureMethod` | `auto` (default), `gfx`, `ddagrab`, `gdi`. |
| `MJPEGQuality` | 1–100. Default 85. Lower values = smaller intermediate files, faster encode, slightly softer image. Try 40–50 on very old hardware. |

---

## When `ConvertAfterRecording=no`

| Setting | Result |
| :--- | :--- |
| `ConvertAfterRecording=yes` | MJPEG intermediate → muxed with audio → x264 ultrafast → intermediate deleted. Final file: small, ready to share. |
| `ConvertAfterRecording=no` | MJPEG intermediate → muxed with audio → kept as-is. Final file: large, high quality, ideal for editing or re-encoding. |

### File Output

- **`yes` (default):** `PhantomRec_YYYYMMDD_HHMMSS.mkv` — compact x264, ready to share.
- **`no`:** `PhantomRec_YYYYMMDD_HHMMSS_lossless.mkv` — high-quality MJPEG intermediate, ideal for editing.

> **Note:** The `_lossless` filename is kept for backwards compatibility. The MJPEG intermediate is a high-quality file, not mathematically lossless. If you need mathematically lossless output for a specific workflow, run the intermediate through `maxsengine.exe` with an appropriate lossless codec before editing.

---

## Building from Source

### Requirements

- MSYS2 with the **UCRT64** toolchain
- MinGW-w64 (comes with MSYS2 UCRT64)
- FFmpeg development libraries (`libturbojpeg`, `libavformat`, `libavcodec`, `libavfilter`, `libavutil`, `libavdevice`)

### Compile

```bash
# Step 1: Build maxenc.exe (Stage 1 video)
gcc -std=c11 -O2 \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    src/maxenc.c -o maxenc.exe \
    -lturbojpeg -lavformat -lavcodec -lavfilter -lavutil -lavdevice \
    -lole32 -luuid -lwinmm

# Step 2: Build maxsound.exe (Stage 1b audio)
gcc -std=c11 -O2 \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    src/maxsound.c -o maxsound.exe \
    -lole32 -luuid -lavformat -lavcodec -lavutil

# Step 3: Build the C core
gcc -std=c11 -O2 \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    -c src/phantomrec_coreCopy.c -o phantomrec_core.o

# Step 4: Build the GUID definitions (WASAPI/KSDATAFORMAT symbols)
gcc -std=c11 -O2 \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    -c src/phantomrec_guids.c -o phantomrec_guids.o

# Step 5: Build the C++ UI
g++ -std=c++17 -O2 \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    -c src/phantomrecCopy.cpp -o phantomrec_ui.o

# Step 6: Compile the resource file (HD icon + version info)
windres src/resource.rc -O coff -o resource.o

# Step 7: Link with g++ (NOT gcc — the UI is C++)
g++ -O2 -mwindows \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
    phantomrec_core.o phantomrec_guids.o phantomrec_ui.o resource.o \
    -o PhantomRec.exe \
    -lturbojpeg \
    -lavformat -lavcodec -lavfilter -lavutil -lavdevice \
    -lcomctl32 -lshell32 -luser32 -lgdi32 -lkernel32 \
    -ladvapi32 -lole32 -luuid -lavrt \
    -lgdiplus -lcomdlg32 -lwindowscodecs \
    -static-libgcc -static-libstdc++
```

> ⚠️ `-D_WIN32_WINNT=0x0A00` (and matching `WINVER` and `NTDDI_VERSION`) is required on MSYS2 UCRT64. Without it, several Windows headers gate the WASAPI and process-creation declarations and the build will fail.

> ⚠️ The final link uses **`g++`**, not `gcc`. The UI is C++ and needs `libstdc++`. `-static-libgcc -static-libstdc++` bakes the runtime in so the `.exe` is self-contained.

---

## What PhantomRec Does Not Do (Yet)

- **Streaming** — PhantomRec is a recorder, not a streaming tool.
- **Webcam overlay** — Not supported.
- **Per-window capture** — PhantomRec captures the entire monitor.
- **Game capture via hooking** — Planned for a future release (exclusive fullscreen support).
- **GPU encoding** — Not now, not ever. That's the whole point.
- **Microphone handling** — Planned for future release

---

## On FRAPS

FRAPS hasn't been updated in over a decade. PhantomRec is the recorder that should have replaced it — same lightweight footprint on the same class of hardware, no game hooking, no anti-cheat conflicts, and a post-conversion stage that gives you compact files without stealing FPS during play.

PhantomRec doesn't remove FRAPS. It just makes FRAPS unnecessary.

---

## License & Credits

PhantomRec is free software. Use it, modify it, share it.

Built by a single developer: **MaxRBLX1**
Max'sEngine™ powered by FFmpeg (ffmpeg.org)
Audio capture based on Microsoft WASAPI sample code.

> "Every screen deserves to be recorded."
