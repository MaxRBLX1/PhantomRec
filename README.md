# PhantomRec — "Record with lightweight compression now. Encode with heavy compression later."

**Built by MaxRBLX1 — v1.9.5**

---

## Project History

PhantomRec was originally released as **RetroRec** (v1.0 – v1.7). The name was changed in **v1.8**.  
All recordings and settings from previous versions are fully compatible.

---

## What is PhantomRec?

PhantomRec is a **free, portable, invisible screen recorder** for Windows.  
It captures your desktop at a smooth 60 FPS with system audio, then converts the recording into a compact, high‑quality file — **all without slowing down your computer**.

**No GPU?** No problem. **Old laptop?** It works.  
PhantomRec runs on any Windows PC from **Vista to Windows 11**, from a dual‑core budget machine to a high‑end workstation.

---

## A Note on Windows Versions

PhantomRec doesn't care what hardware you have — it cares about your OS, because that determines which capture APIs are available.

| Windows Version | Capture Method | Typical FPS |
| :--- | :--- | :--- |
| Windows 10 / 11 | GFX (D3D11 zero‑copy) | 60 FPS |
| Windows 8 / 8.1 | DDAGrab (DXGI) | 60 FPS |
| Windows 7 / Vista | GDI (CPU software) | Up to 30 FPS (Lacks Things and Uses Bitblt) |

**Fallback chain:** GFX → DDAGrab → GDI. If a method isn't supported, PhantomRec automatically drops to the next best option. GDI is the universal fallback.

All capture methods write a **lossless master file** at the native frame rate. The final video is automatically converted to a constant **60 fps x264** file, regardless of the source frame rate.

---

## Why Choose PhantomRec?

PhantomRec uses a **two‑stage ghost pipeline** — the same architecture that made Fraps legendary, rebuilt for modern lossless codecs.

### Stage 1 — Live Capture (Ut Video lossless, ~5% CPU)

- The screen is captured and encoded with **Ut Video** — a mathematically lossless, intra‑frame codec.
- **Parallel encoding:** uses `-slices` equal to your CPU core count, keeping CPU usage flat at ~5% regardless of on‑screen action.
- **Duplicate frames** are skipped automatically.
- **Massive queue:** `-thread_queue_size 4096` for video and audio, plus `-max_muxing_queue_size 50000` to absorb HDD write pauses — zero lag, even on slow mechanical drives.
- **No GPU encoding** means the stop button responds instantly — no encoder queue drain freeze.

### Stage 2 — Post‑Convert (x264 ultrafast, after you stop)

When you press stop, PhantomRec converts the lossless master to a crisp, compact **x264** file at 60 FPS using all CPU cores — when your system is idle.  
You get **NVENC‑quality file sizes** without needing a GPU.

### The Result

- Your GPU stays **100% dedicated** to your game or desktop.
- Recording uses **~5% CPU** on any hardware.
- Heavy compression happens **when you're done** recording.
- **Instant stop** — no encoder queue drain freeze.
- Smooth output on any CPU from **2008 onward**.
- **No GPU required. No NVENC. No AMF. CPU only.**

---

## System Requirements

| Component | Minimum |
| :--- | :--- |
| OS | Windows Vista / 7 / 8 / 8.1 / 10 / 11 |
| CPU | Any dual‑core x86_64 with SSE2 |
| RAM | 4 GB |
| GPU | Any. Integrated. None. All work. |
| Storage | Any HDD or SSD with free space |

> *No dedicated GPU or GPU hardware encoding required.*

---

## Quick Start

1. Download the latest release and extract all files.
2. Make sure **`maxsengine.exe`** (our pre‑tuned FFmpeg 6.1+ build) is in the same folder as `PhantomRec.exe`.
3. Double‑click `PhantomRec.exe` — a small always‑on‑top window appears.
4. Press **F10** to start recording. Press **F10** again to stop.
5. Wait a few seconds for post‑processing — your video opens automatically in Explorer.

---

## What's New in v1.9.5 (Code‑Matched)

🎯 **All‑New Stage 1 — Ut Video Lossless with Parallel Slices**  
Replaced MPEG‑4 Part 2 with Ut Video. Uses `-slices` equal to your core count for efficient parallel frame encoding.  
Constant ~5% CPU, no matter what’s on screen. Writes a mathematically perfect master file.

🔧 **Hard‑Drive‑Friendly Pipeline**  
- Video `-thread_queue_size 4096` (was 2048).  
- Audio `-thread_queue_size 4096` (was 512).  
- Muxing queue expanded to **`50000`** (was 9096) — absorbs even the longest HDD write stalls.

🔇 **Perfect Audio/Video Sync**  
Audio and video now start at the exact same moment using a sync event (`hAudioStartEvent`).  
Works for initial recording and after every pause/resume.

⏸️ **Robust Pause/Resume with Segments**  
Pausing creates a new segment file. Resume continues on a fresh segment.  
Supports up to **64 segments** per recording session (prevents resource exhaustion).

⚡ **Power Plan Lock**  
Automatically switches to **High Performance** on launch and restores your original plan on exit — no dropped frames due to power throttling.

🖥️ **FFmpeg Console Fix**  
FFmpeg runs with `CREATE_NEW_CONSOLE` — guarantees proper CTRL_BREAK handling and eliminates the “accordion effect” timeline distortion.

🧹 **Clean Codebase**  
Removed dead variables and legacy MPEG‑4 quality settings. Pure C core + C++ UI with thread‑safe marshalling.

🎨 **UI Improvements**  
Custom backgrounds, fonts, animated GIFs, font colour/size — all hot‑reloaded from `Settings.ini` (appearance changes apply instantly; capture settings only apply when idle).  
Thread‑safe UI updates (core callbacks are marshalled to the main thread).

🛠️ **Stage 2 — Ultrafast CFR Conversion**  
Post‑processing uses `-preset ultrafast -r 60 -fps_mode cfr` for fast, consistent 60 FPS output.  
No manual thread count — FFmpeg auto‑detects optimal threading.

---

## Settings — How to Control

All settings are in **`Settings.ini`** (same folder as `PhantomRec.exe`).  
Edit it while the program is running — changes take effect within **2 seconds** (only hotkeys and appearance are applied mid‑recording; capture method and conversion flag are deferred until idle).

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
| Setting | Description |
| :--- | :--- |
| **Hotkey** | `F1`‑`F12` for function keys, or a single letter for `Ctrl+` (e.g. `R` = `Ctrl+R`). |
| **PauseHotkey** | Same format. |
| **ConvertAfterRecording** | `yes` = automatically compress after recording (**recommended**). <br> `no` = keep the raw segment files (`*_segN_temp.mkv` + `segments.txt`) in the output folder. You must manually concatenate or delete them. |
| **CaptureMethod** | `auto` (default), `gfx`, `ddagrab`, `gdi`. |

> **⚠️ Important: When `ConvertAfterRecording=no`**
>
> PhantomRec **does not** concatenate or delete the segment files. Instead, it leaves:
> - Multiple `*_segN_temp.mkv` files (each is a lossless chunk of your recording)
> - A `segments.txt` file that lists these chunks in the correct order
>
> To turn these chunks into a single playable video, you must manually concatenate them using `maxsengine.exe` (the included FFmpeg build).
>
> ### How to Concatenate Manually
>
> 1. Open the folder where PhantomRec saves your recordings.  
>    *(Default: `C:\Users\[YourUsername]\Videos\PhantomRec`)*
>
> 2. In that folder, locate the `segments.txt` file and all the `*_segN_temp.mkv` files.
>
> 3. **Open Command Prompt in that exact folder:**
>    - Hold `Shift` + right‑click inside the folder window.
>    - Select **"Open PowerShell window here"** or **"Open command window here"**.
>    - *(Alternatively, type `cmd` in the folder's address bar and press Enter.)*
>
> 4. Run this command:
>    ```cmd
>    maxsengine.exe -f concat -safe 0 -i segments.txt -c copy output.mkv
>    ```
>
> 5. Wait for the process to finish. You'll now have a single file named `output.mkv` — this is your complete recording.
>
> 6. You can safely delete the `*_segN_temp.mkv` files and `segments.txt` to free up space.
>
> > **💡 Tip:** Rename `output.mkv` to something meaningful (e.g., `my_gameplay.mkv`) before moving or sharing it.

---

## Building from Source

### Requirements
- **MinGW‑w64** (UCRT64)
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

> **⚠️ `-D_WIN32_WINNT=0x0A00` is strictly required.** Without it, the binary targets XP compatibility and the recording pipeline fails with 0 FPS.

---

## Project Tree

```text
PhantomRec/
├── src/
│   ├── PhantomRec.cpp        # C++ UI (Win32 + GDI+)
│   ├── phantomrec_core.c     # Pure C11 capture engine
│   └── phantomrec_core.h     # Shared header (extern "C" bridge)
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

PhantomRec is **free software**. Use it, modify it, share it.

**Built by MaxRBLX1.**  
**Max'sEngine™** powered by FFmpeg ([ffmpeg.org](https://ffmpeg.org)).  
Audio capture based on Microsoft WASAPI sample code.
