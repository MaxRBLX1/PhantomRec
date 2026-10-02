// maxsound.c — MaxRBLX1's Fastest Sound Capture
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
//
// Captures the default render endpoint via WASAPI loopback and writes a
// 32-bit float WAV. Video is owned by maxenc.exe. Muxing is owned by
// PhantomRec. This process does exactly one job: capture sound.
//
// Design:
//   - No FFmpeg. WASAPI hands us PCM. We write it straight to disk.
//     One WriteFile per packet, no intermediate buffers, no pipes.
//   - Sample-accurate t0. IAudioCaptureClient::GetBuffer returns a QPC
//     timestamp for each packet — the value of the performance counter
//     when the hardware sampled those bytes. On the first packet we write
//     that value as 8 little-endian bytes to <output.wav>.t0. The parent
//     compares it against maxenc's <output.mkv>.t0 to compute the exact
//     A/V offset.
//   - Event-driven. We block on the WASAPI ready event and wake only when
//     a buffer is available. No polling, no spin.
//   - AVRT priority. The main thread registers with MMCSS as "Audio" via
//     AvSetMmThreadCharacteristicsW so the scheduler treats it as a real
//     audio thread.
//   - File growth. NTFS extension is cheap; we let the file grow
//     naturally and flush on stop. We do NOT preallocate — that would
//     try to reserve gigabytes up front.
//
// Format:
//   32-bit float, whatever the mix format's channel count and sample rate
//   are. This is what WASAPI shared mode gives us. No conversion here.
//
// Stop: press 'q' in this console, or send CTRL_BREAK.
//
// Build: gcc -std=c11 -O2
//        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000
//        src/maxsound.c -o maxsound.exe
//        -lole32 -luuid -lavrt  
//
// Exit codes:
//   0 = normal
//   1 = fatal error
//   2 = no audio endpoint (WASAPI init failed)

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <process.h>
#include <conio.h>

// Force the float format GUID into this binary. Some MinGW-w64 ksmedia.h
// configurations gate it behind feature macros and never emit the symbol.
// phantomrec_guids.c carries the same line for the same reason.
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_LOCAL,
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// ============================================================================
// Globals
// ============================================================================
static volatile LONG  g_stopRequested = 0;

static IMMDeviceEnumerator* g_enum    = NULL;
static IMMDevice*           g_dev     = NULL;
static IAudioClient*        g_client  = NULL;
static IAudioCaptureClient* g_cap     = NULL;
static WAVEFORMATEX*        g_wf      = NULL;
static HANDLE               g_ready   = NULL;

static HANDLE  g_wav     = INVALID_HANDLE_VALUE;
static UINT64  g_bytes   = 0;   // total bytes written to the data chunk
static char    g_markerPath[MAX_PATH] = {0};

// ============================================================================
// Console control thread — watches for 'q'
// ============================================================================
static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    }
    return FALSE;
}

static unsigned int __stdcall ConsoleKeyThread(void* param) {
    (void)param;
    while (!InterlockedCompareExchange(&g_stopRequested, 1, 1)) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q' || c == 'Q') {
                InterlockedExchange(&g_stopRequested, 1);
                break;
            }
        }
        Sleep(50);
    }
    return 0;
}

// ============================================================================
// WAV header
//
// We write a WAVEFORMATEXTENSIBLE header unconditionally. It handles any
// channel count, any sample rate, and 32-bit float cleanly. Total header
// size is 68 bytes.
//
//   offset  0: "RIFF"
//   offset  4: fileSize - 8
//   offset  8: "WAVE"
//   offset 12: "fmt "
//   offset 16: 40
//   offset 20: WAVEFORMATEXTENSIBLE (40 bytes)
//   offset 60: "data"
//   offset 64: dataSize
//   offset 68: samples...
// ============================================================================
static void WriteWavHeader(HANDLE h, const WAVEFORMATEX* wf, UINT32 dataSize) {
    DWORD wrote = 0;
    const UINT32 fmtSize   = 40;
    const UINT32 fileSize  = 24 + fmtSize + dataSize;
    const UINT32 riffSize  = fileSize - 8;

    WriteFile(h, "RIFF", 4, &wrote, NULL);
    WriteFile(h, &riffSize, 4, &wrote, NULL);
    WriteFile(h, "WAVE", 4, &wrote, NULL);

    WriteFile(h, "fmt ", 4, &wrote, NULL);
    WriteFile(h, &fmtSize, 4, &wrote, NULL);

    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels       = wf->nChannels;
    wfx.Format.nSamplesPerSec  = wf->nSamplesPerSec;
    wfx.Format.wBitsPerSample  = wf->wBitsPerSample;
    wfx.Format.nBlockAlign     = wf->nBlockAlign;
    wfx.Format.nAvgBytesPerSec = wf->nAvgBytesPerSec;
    wfx.Format.cbSize          = 22;
    wfx.Samples.wValidBitsPerSample = wf->wBitsPerSample;

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)wf;
        wfx.dwChannelMask = ext->dwChannelMask;
    } else {
        switch (wf->nChannels) {
        case 1: wfx.dwChannelMask = 0x4;   break;  // SPEAKER_FRONT_CENTER
        case 2: wfx.dwChannelMask = 0x3;   break;  // FL | FR
        case 4: wfx.dwChannelMask = 0x33;  break;  // quad
        case 6: wfx.dwChannelMask = 0x3F;  break;  // 5.1
        case 8: wfx.dwChannelMask = 0x63F; break;  // 7.1
        default: wfx.dwChannelMask = 0;    break;
        }
    }

    memcpy(&wfx.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_LOCAL,
           sizeof(GUID));

    WriteFile(h, &wfx, sizeof(wfx), &wrote, NULL);

    WriteFile(h, "data", 4, &wrote, NULL);
    WriteFile(h, &dataSize, 4, &wrote, NULL);
}

static void RewriteWavSizes(HANDLE h) {
    DWORD wrote = 0;
    UINT32 dataSize  = (UINT32)(g_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : g_bytes);
    UINT32 fileSize  = 24 + 40 + dataSize;
    UINT32 riffSize  = fileSize - 8;

    SetFilePointer(h, 4, NULL, FILE_BEGIN);
    WriteFile(h, &riffSize, 4, &wrote, NULL);

    SetFilePointer(h, 64, NULL, FILE_BEGIN);
    WriteFile(h, &dataSize, 4, &wrote, NULL);

    // Trim the file to the real size.
    LARGE_INTEGER endPos;
    endPos.QuadPart = 68 + (LONGLONG)dataSize;
    SetFilePointerEx(h, endPos, NULL, FILE_BEGIN);
    SetEndOfFile(h);
}

// ============================================================================
// Marker file — 8 bytes, little-endian int64 QPC tick
// ============================================================================
static void WriteMarker(const char* path, int64_t tick) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, &tick, sizeof(tick), &wrote, NULL);
    CloseHandle(h);
}

// ============================================================================
// WASAPI init / teardown
// ============================================================================
static int InitWASAPI(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE is fine — someone already initialized this thread
    // as STA. We can still use the objects below.
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[maxsound] CoInitializeEx failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&g_enum);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] MMDeviceEnumerator failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = g_enum->lpVtbl->GetDefaultAudioEndpoint(g_enum, eRender, eConsole,
                                                 &g_dev);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] no default render endpoint: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = g_dev->lpVtbl->Activate(g_dev, &IID_IAudioClient, CLSCTX_ALL,
                                 NULL, (void**)&g_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] IAudioClient activate failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = g_client->lpVtbl->GetMixFormat(g_client, &g_wf);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] GetMixFormat failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    g_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ready) {
        fprintf(stderr, "[maxsound] CreateEvent failed\n");
        return 0;
    }

    hr = g_client->lpVtbl->Initialize(g_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, g_wf, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] IAudioClient Initialize failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = g_client->lpVtbl->SetEventHandle(g_client, g_ready);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] SetEventHandle failed: 0x%08lx\n",
                (unsigned long)hr);
        return 0;
    }

    hr = g_client->lpVtbl->GetService(g_client, &IID_IAudioCaptureClient,
                                      (void**)&g_cap);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] GetService(IAudioCaptureClient) "
                        "failed: 0x%08lx\n", (unsigned long)hr);
        return 0;
    }

    fprintf(stderr, "[maxsound] %u Hz, %u ch, %u-bit%s\n",
            g_wf->nSamplesPerSec, g_wf->nChannels, g_wf->wBitsPerSample,
            (g_wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&((WAVEFORMATEXTENSIBLE*)g_wf)->SubFormat,
                         &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_LOCAL))
                ? " float" : "");
    return 1;
}

static void CleanupWASAPI(void) {
    if (g_cap)    { g_cap->lpVtbl->Release(g_cap);    g_cap    = NULL; }
    if (g_client) { g_client->lpVtbl->Release(g_client); g_client = NULL; }
    if (g_dev)    { g_dev->lpVtbl->Release(g_dev);    g_dev    = NULL; }
    if (g_enum)   { g_enum->lpVtbl->Release(g_enum);  g_enum   = NULL; }
    if (g_wf)     { CoTaskMemFree(g_wf);              g_wf     = NULL; }
    if (g_ready)  { CloseHandle(g_ready);             g_ready  = NULL; }
}

// ============================================================================
// Probe mode — does this machine have a render endpoint we can capture?
// ============================================================================
static int ProbeOnly(void) {
    if (!InitWASAPI()) {
        CleanupWASAPI();
        return 2;
    }
    printf("%u %u %u\n",
           g_wf->nSamplesPerSec, g_wf->nChannels, g_wf->wBitsPerSample);
    CleanupWASAPI();
    return 0;
}

// ============================================================================
// Capture loop
// ============================================================================
static int CaptureLoop(const char* outPath) {
    // Open / create the WAV file.
    g_wav = CreateFileA(outPath, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                        NULL);
    if (g_wav == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[maxsound] cannot open %s (err %lu)\n",
                outPath, GetLastError());
        return 1;
    }

    // Write a placeholder header. We rewrite the sizes on stop.
    WriteWavHeader(g_wav, g_wf, 0);
    g_bytes = 0;

    // Set MMCSS "Audio" priority on this thread.
    DWORD taskIdx = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);

    HRESULT hr = g_client->lpVtbl->Start(g_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[maxsound] Start failed: 0x%08lx\n",
                (unsigned long)hr);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CloseHandle(g_wav);
        g_wav = INVALID_HANDLE_VALUE;
        return 1;
    }

    int firstPacket = 1;

    while (!InterlockedCompareExchange(&g_stopRequested, 1, 1)) {
        DWORD wr = WaitForSingleObject(g_ready, 100);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) break;

        UINT32 packetSize = 0;
        hr = g_cap->lpVtbl->GetNextPacketSize(g_cap, &packetSize);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            fprintf(stderr, "[maxsound] device invalidated\n");
            break;
        }
        if (FAILED(hr)) continue;

        while (packetSize > 0) {
            BYTE*  data    = NULL;
            UINT32 frames  = 0;
            DWORD  flags   = 0;
            UINT64 devPos  = 0, qpcPos = 0;

            hr = g_cap->lpVtbl->GetBuffer(g_cap, &data, &frames, &flags,
                                          &devPos, &qpcPos);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                fprintf(stderr, "[maxsound] device invalidated\n");
                break;
            }
            if (FAILED(hr)) break;

            UINT32 bytes = frames * g_wf->nBlockAlign;

            // First real packet: capture the QPC tick from the audio
            // engine. That value is the performance counter value at the
            // instant the hardware sampled those bytes — it's the only
            // timestamp that can be aligned to maxenc's frame QPC with
            // sub-millisecond precision.
            if (firstPacket && g_markerPath[0]) {
                WriteMarker(g_markerPath, (int64_t)qpcPos);
                firstPacket = 0;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Zero-fill a stack buffer and write in chunks.
                BYTE zero[4096];
                memset(zero, 0, sizeof(zero));
                UINT32 remaining = bytes;
                while (remaining > 0) {
                    DWORD chunk = remaining > sizeof(zero)
                                  ? (DWORD)sizeof(zero) : remaining;
                    DWORD wrote = 0;
                    if (!WriteFile(g_wav, zero, chunk, &wrote, NULL) ||
                        wrote != chunk) {
                        fprintf(stderr, "[maxsound] write failed (err %lu)\n",
                                GetLastError());
                        InterlockedExchange(&g_stopRequested, 1);
                        break;
                    }
                    remaining -= chunk;
                    g_bytes   += chunk;
                }
            } else {
                DWORD wrote = 0;
                if (!WriteFile(g_wav, data, bytes, &wrote, NULL) ||
                    wrote != bytes) {
                    fprintf(stderr, "[maxsound] write failed (err %lu)\n",
                            GetLastError());
                    InterlockedExchange(&g_stopRequested, 1);
                    g_cap->lpVtbl->ReleaseBuffer(g_cap, frames);
                    break;
                }
                g_bytes += bytes;
            }

            g_cap->lpVtbl->ReleaseBuffer(g_cap, frames);

            hr = g_cap->lpVtbl->GetNextPacketSize(g_cap, &packetSize);
            if (FAILED(hr)) break;
        }
    }

    g_client->lpVtbl->Stop(g_client);
    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);

    // Fix the header, flush, close.
    RewriteWavSizes(g_wav);
    FlushFileBuffers(g_wav);
    CloseHandle(g_wav);
    g_wav = INVALID_HANDLE_VALUE;

    fprintf(stderr, "[maxsound] done: %llu bytes written to %s\n",
            (unsigned long long)g_bytes, outPath);
    return 0;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <output.wav> [--marker <path>]\n"
            "       %s --probe-only\n"
            "  Stop: press 'q' in this console (or send CTRL_BREAK).\n",
            argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--probe-only") == 0) {
        return ProbeOnly();
    }

    const char* outPath = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--marker") == 0 && i + 1 < argc) {
            strncpy(g_markerPath, argv[++i], sizeof(g_markerPath) - 1);
            g_markerPath[sizeof(g_markerPath) - 1] = 0;
        }
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    uintptr_t keyThread = _beginthreadex(NULL, 0, ConsoleKeyThread, NULL, 0, NULL);

    if (!InitWASAPI()) {
        CleanupWASAPI();
        if (keyThread) CloseHandle((HANDLE)keyThread);
        return 2;
    }

    int rc = CaptureLoop(outPath);

    CleanupWASAPI();
    if (keyThread) CloseHandle((HANDLE)keyThread);
    return rc;
}
