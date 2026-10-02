// phantomrec_core.c — PhantomRec v1.9.9 Pure C Core
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
//
// Stage 1: MaxRBLX1's Fastest MJPEG  (maxenc.exe primary, in-process fallback)
// Stage 1b: MaxRBLX1's Fastest Sound  (maxsound.exe, WASAPI loopback)
// Stage 2: x264 ultrafast post-convert.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <avrt.h>
#include <process.h>
#include <turbojpeg.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "phantomrec_coreCopy.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "advapi32.lib")

// ============================================================================
// Globals
// ============================================================================
static AVFormatContext* g_fmt_ctx       = NULL;
static AVStream*        g_video_stream  = NULL;
static CRITICAL_SECTION g_muxer_lock;
static HANDLE           g_hCaptureThread = NULL;
static volatile int     g_captureRunning = 0;
static tjhandle         g_tjc            = NULL;
static AVPacket*        g_mjpeg_pkt      = NULL;

// ============================================================================
// Forward declarations
// ============================================================================
static BOOL SendKeyToProcess(DWORD pid, WORD vk, char ch);
static BOOL WINAPI PhantomCtrlHandler(DWORD type);

// ============================================================================
// Windows version detection
// ============================================================================
static int GetWindowsVersion(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return 0;
    typedef LONG (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
    if (!RtlGetVersion) return 0;
    RTL_OSVERSIONINFOW osvi = { sizeof(osvi) };
    if (RtlGetVersion(&osvi) != 0) return 0;
    if (osvi.dwMajorVersion >= 10) return 10;
    if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion >= 2) return 8;
    if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) return 7;
    return 0;
}

// ============================================================================
// In-process MJPEG encode + mux
// ============================================================================
static unsigned char* EncodeFrameToMJPEG(tjhandle tjc,
                                         const unsigned char* bgrx,
                                         int w, int h, int stride,
                                         int quality, unsigned long* out_size)
{
    unsigned char* jpeg = NULL;
    int ret = tjCompress2(tjc, (unsigned char*)bgrx, w, stride, h,
                          TJPF_BGRX, &jpeg, out_size,
                          TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (ret != 0 || !jpeg) { *out_size = 0; return NULL; }
    return jpeg;
}

static int MuxJPEGFrame(PhantomRecCore* core, const unsigned char* bgrx,
                        int w, int h, int stride)
{
    if (!g_tjc || !g_fmt_ctx || !g_video_stream || !g_mjpeg_pkt) return -1;
    if (InterlockedCompareExchange(&core->paused, 1, 1) == 1) return 0;

    int quality = (core->mjpegQuality >= 1 && core->mjpegQuality <= 100)
                    ? core->mjpegQuality : 75;

    unsigned long jpeg_size = 0;
    unsigned char* jpeg = EncodeFrameToMJPEG(g_tjc, bgrx, w, h, stride, quality, &jpeg_size);
    if (!jpeg || jpeg_size == 0) return -1;

    AVPacket* pkt = g_mjpeg_pkt;
    av_packet_unref(pkt);

    pkt->data = jpeg;
    pkt->size = (int)jpeg_size;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t elapsed_us = (int64_t)((now.QuadPart - core->segmentStartTime.QuadPart)
                                   * 1000000 / core->recFreq.QuadPart);

    pkt->pts = elapsed_us;
    pkt->dts = elapsed_us;
    pkt->stream_index = g_video_stream->index;
    av_packet_rescale_ts(pkt, (AVRational){1, 1000000}, g_video_stream->time_base);

    EnterCriticalSection(&g_muxer_lock);
    av_interleaved_write_frame(g_fmt_ctx, pkt);
    LeaveCriticalSection(&g_muxer_lock);

    tjFree(jpeg);
    av_packet_unref(pkt);
    return 0;
}

// ============================================================================
// Keyboard send to console child
// ============================================================================
static BOOL SendKeyToProcess(DWORD pid, WORD vk, char ch) {
    FreeConsole();
    if (!AttachConsole(pid)) return FALSE;
    BOOL ok = FALSE;
    HANDLE hIn = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (hIn != INVALID_HANDLE_VALUE) {
        INPUT_RECORD ir[2] = {0};
        ir[0].EventType = KEY_EVENT;
        ir[0].Event.KeyEvent.bKeyDown = TRUE;
        ir[0].Event.KeyEvent.wRepeatCount = 1;
        ir[0].Event.KeyEvent.wVirtualKeyCode = vk;
        ir[0].Event.KeyEvent.uChar.AsciiChar = ch;
        ir[1].EventType = KEY_EVENT;
        ir[1].Event.KeyEvent.bKeyDown = TRUE;
        ir[1].Event.KeyEvent.wRepeatCount = 1;
        ir[1].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        ir[1].Event.KeyEvent.uChar.AsciiChar = '\r';
        DWORD written = 0;
        ok = WriteConsoleInputA(hIn, ir, 2, &written) != 0;
        CloseHandle(hIn);
    }
    FreeConsole();
    return ok;
}

// ============================================================================
// Video child spawn / stop (maxenc.exe)
// ============================================================================
static BOOL SpawnVideoChild(PhantomRecCore* core, const char* cmdline,
                            PROCESS_INFORMATION* pi) {
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char* buf = _strdup(cmdline);
    if (!buf) return FALSE;

    BOOL ok = CreateProcessA(
        NULL, buf, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | ABOVE_NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, pi);

    free(buf);
    return ok;
}

static void StopVideoChild(PROCESS_INFORMATION* pi, DWORD timeoutMs) {
    if (!pi->hProcess) return;

    SendKeyToProcess(pi->dwProcessId, 'Q', 'q');
    if (WaitForSingleObject(pi->hProcess, timeoutMs) == WAIT_OBJECT_0) {
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        memset(pi, 0, sizeof(*pi));
        return;
    }

    if (AttachConsole(pi->dwProcessId)) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi->dwProcessId);
        FreeConsole();
    }
    if (WaitForSingleObject(pi->hProcess, 800) == WAIT_OBJECT_0) {
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        memset(pi, 0, sizeof(*pi));
        return;
    }

    if (AttachConsole(pi->dwProcessId)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, pi->dwProcessId);
        FreeConsole();
    }
    if (WaitForSingleObject(pi->hProcess, 800) == WAIT_OBJECT_0) {
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        memset(pi, 0, sizeof(*pi));
        return;
    }

    TerminateProcess(pi->hProcess, 0);
    WaitForSingleObject(pi->hProcess, 500);
    CloseHandle(pi->hProcess);
    CloseHandle(pi->hThread);
    memset(pi, 0, sizeof(*pi));
}

// ============================================================================
// maxsound.exe child — spawn / stop
// ============================================================================
static BOOL SpawnMaxSound(PhantomRecCore* core, const char* wavPath,
                          const char* markerPath, PROCESS_INFORMATION* pi) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" \"%s\" --marker \"%s\"",
             core->maxsoundPath, wavPath, markerPath);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char* buf = _strdup(cmd);
    if (!buf) return FALSE;

    BOOL ok = CreateProcessA(
        NULL, buf, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | ABOVE_NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, pi);

    free(buf);
    return ok;
}

static void StopMaxSound(PROCESS_INFORMATION* pi, DWORD timeoutMs) {
    if (!pi->hProcess) return;

    SendKeyToProcess(pi->dwProcessId, 'Q', 'q');
    if (WaitForSingleObject(pi->hProcess, timeoutMs) == WAIT_OBJECT_0) {
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        memset(pi, 0, sizeof(*pi));
        return;
    }

    if (AttachConsole(pi->dwProcessId)) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi->dwProcessId);
        FreeConsole();
    }
    if (WaitForSingleObject(pi->hProcess, 800) == WAIT_OBJECT_0) {
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        memset(pi, 0, sizeof(*pi));
        return;
    }

    TerminateProcess(pi->hProcess, 0);
    WaitForSingleObject(pi->hProcess, 500);
    CloseHandle(pi->hProcess);
    CloseHandle(pi->hThread);
    memset(pi, 0, sizeof(*pi));
}

static void DeleteFileWithRetry(const char* path) {
    for (int attempt = 0; attempt < 20; attempt++) {
        if (DeleteFileA(path)) return;
        Sleep(100);
    }
}

// ============================================================================
// .t0 marker helpers
// ============================================================================
static int64_t ReadT0Marker(const char* file) {
    char markerPath[MAX_PATH];
    snprintf(markerPath, sizeof(markerPath), "%s.t0", file);

    HANDLE h = CreateFileA(markerPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int64_t tick = 0;
    DWORD got = 0;
    ReadFile(h, &tick, sizeof(tick), &got, NULL);
    CloseHandle(h);
    if (got != sizeof(tick)) return 0;
    return tick;
}

static int64_t WaitT0Marker(const char* file, DWORD timeoutMs) {
    DWORD elapsed = 0;
    while (elapsed < timeoutMs) {
        int64_t t = ReadT0Marker(file);
        if (t != 0) return t;
        Sleep(20);
        elapsed += 20;
    }
    return 0;
}

static int QpcDeltaToMs(PhantomRecCore* core, int64_t audioTick, int64_t videoTick) {
    if (core->recFreq.QuadPart == 0) return 0;
    LONGLONG delta = (LONGLONG)(audioTick - videoTick);
    LONGLONG ms = delta * 1000 / core->recFreq.QuadPart;
    if (ms >  30000) ms =  30000;
    if (ms < -30000) ms = -30000;
    return (int)ms;
}

// ============================================================================
// Capture method
// ============================================================================
static CaptureMethod g_UserCaptureMethod = CAPTURE_AUTO;

void Core_SetCaptureMethodEx(PhantomRecCore* core, CaptureMethod method) {
    g_UserCaptureMethod = method;
    Core_SetCaptureMethod(core);
}

const char* Core_GetCaptureMethodDesc(const PhantomRecCore* core) {
    switch (core->captureMethod) {
    case 0: return "GFX Capture (D3D11, GPU, 60 FPS)";
    case 1: return "DDAGrab (DXGI, GPU, 60 FPS)";
    case 2: return "GDI (CPU, up to 30 FPS, software)";
    default: return "Unknown";
    }
}

static void GetCaptureInput(PhantomRecCore* core, char* buf, int bufsize) {
    int winVer = GetWindowsVersion();
    CaptureMethod method = g_UserCaptureMethod;
    if (method == CAPTURE_AUTO) {
        if (winVer >= 10) method = (core->cpuCoreCount >= 8) ? CAPTURE_GFX : CAPTURE_DDAGRAB;
        else if (winVer >= 8) method = CAPTURE_DDAGRAB;
        else method = CAPTURE_GDI;
    }
    if (method == CAPTURE_GFX && winVer < 10) method = CAPTURE_DDAGRAB;
    if (method == CAPTURE_DDAGRAB && winVer < 8) method = CAPTURE_GDI;

    int captureFPS = (core->cpuCoreCount >= 4) ? 60 : 30;

    switch (method) {
    case CAPTURE_GFX:
        core->captureMethod = 0;
        strncpy_s(buf, bufsize, " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1", _TRUNCATE);
        break;
    case CAPTURE_DDAGRAB:
        core->captureMethod = 1;
        sprintf_s(buf, bufsize, " -f lavfi -i ddagrab=0:framerate=%d", captureFPS);
        break;
    case CAPTURE_GDI:
    default:
        core->captureMethod = 2;
        sprintf_s(buf, bufsize, " -f gdigrab -framerate %d -i desktop", captureFPS);
        break;
    }
}

// ============================================================================
// Utility
// ============================================================================
int Core_FileExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

long long Core_GetFileSize(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        LARGE_INTEGER s;
        s.HighPart = d.nFileSizeHigh;
        s.LowPart = d.nFileSizeLow;
        return s.QuadPart;
    }
    return -1;
}

void Core_FormatSize(long long bytes, char* buf, int bufsize) {
    if (bytes < 0) { strncpy_s(buf, bufsize, "Unknown", _TRUNCATE); return; }
    if (bytes < 1024) { sprintf_s(buf, bufsize, "%lld B", bytes); return; }
    double kb = bytes / 1024.0;
    if (kb < 1048576.0) { sprintf_s(buf, bufsize, "%.1f KB", kb); return; }
    double mb = kb / 1024.0;
    if (mb < 1024.0) { sprintf_s(buf, bufsize, "%.1f MB", mb); return; }
    sprintf_s(buf, bufsize, "%.2f GB", mb / 1024.0);
}

void Core_FormatTime(int seconds, char* buf, int bufsize) {
    sprintf_s(buf, bufsize, "%02d:%02d", seconds / 60, seconds % 60);
}

void Core_Timestamp(char* buf, int bufsize) {
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    strftime(buf, bufsize, "PhantomRec_%Y%m%d_%H%M%S", &t);
}

void Core_GetVideosFolder(char* buf, int bufsize) {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, path))) {
        sprintf_s(buf, bufsize, "%s\\PhantomRec", path);
        CreateDirectoryA(buf, NULL);
    } else {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char* lastSep = strrchr(path, '\\');
        if (lastSep) *lastSep = '\0';
        strncpy_s(buf, bufsize, path, _TRUNCATE);
    }
}

int Core_FindMaxsEngine(PhantomRecCore* core) {
    char local[MAX_PATH];
    GetModuleFileNameA(NULL, local, MAX_PATH);
    char* lastSep = strrchr(local, '\\');
    if (lastSep) *lastSep = '\0';
    char testPath[MAX_PATH];

    sprintf_s(testPath, MAX_PATH, "%s\\maxsengine.exe", local);
    if (Core_FileExists(testPath)) {
        strncpy_s(core->maxsenginePath, MAX_PATH, testPath, _TRUNCATE);
    } else {
        sprintf_s(testPath, MAX_PATH, "%s\\ffmpeg.exe", local);
        if (Core_FileExists(testPath)) {
            strncpy_s(core->maxsenginePath, MAX_PATH, testPath, _TRUNCATE);
        } else {
            return 0;
        }
    }

    sprintf_s(testPath, MAX_PATH, "%s\\maxenc.exe", local);
    if (Core_FileExists(testPath)) {
        strncpy_s(core->maxencPath, MAX_PATH, testPath, _TRUNCATE);
    } else {
        core->maxencPath[0] = '\0';
    }

    sprintf_s(testPath, MAX_PATH, "%s\\maxsound.exe", local);
    if (Core_FileExists(testPath)) {
        strncpy_s(core->maxsoundPath, MAX_PATH, testPath, _TRUNCATE);
    } else {
        core->maxsoundPath[0] = '\0';
    }

    return 1;
}

// ============================================================================
// Command builders
// ============================================================================
static void BuildMaxEncCommand(PhantomRecCore* core, const char* outputFile,
                               char* cmdLine, int cmdSize) {
    const char* apiStr = "auto";
    switch (core->captureMethod) {
    case 0: apiStr = "gfx";     break;
    case 1: apiStr = "ddagrab"; break;
    case 2: apiStr = "gdi";     break;
    default: apiStr = "auto";   break;
    }
    int quality = (core->mjpegQuality >= 1 && core->mjpegQuality <= 100)
                    ? core->mjpegQuality : 75;

    sprintf_s(cmdLine, cmdSize,
        "\"%s\" \"%s\" %d %s \"%s\"",
        core->maxencPath, outputFile, quality, apiStr, core->maxsenginePath);
}

// ============================================================================
// Core lifecycle
// ============================================================================
void Core_Init(PhantomRecCore* core, const char* maxsenginePath, const char* outputDir) {
	SetConsoleCtrlHandler(PhantomCtrlHandler, TRUE);
    memset(core, 0, sizeof(PhantomRecCore));
    if (maxsenginePath) strncpy_s(core->maxsenginePath, MAX_PATH, maxsenginePath, _TRUNCATE);
    if (outputDir) strncpy_s(core->outputDir, MAX_PATH, outputDir, _TRUNCATE);
    core->convertAfterRecording = 1;
    core->pipeBufferSizeMB = 8;
    core->dynamicThreads = 1;
    core->videoQueueSize = 4096;

    core->videoEncoder = 0;
    core->mjpegQuality = 85;

    InitializeCriticalSection(&g_muxer_lock);

    g_tjc = tjInitCompress();
    g_mjpeg_pkt = av_packet_alloc();

    QueryPerformanceFrequency(&core->recFreq);
    InterlockedExchange(&core->recording, 0);
    InterlockedExchange(&core->paused, 0);
    InterlockedExchange(&core->converting, 0);
}

static BOOL WINAPI PhantomCtrlHandler(DWORD type) {
    // PhantomRec attaches to maxenc/maxsound's console to send 'q'. If the
    // 'q' path times out, it sends CTRL_BREAK — which, with process group 0,
    // is delivered to every process on that console, including PhantomRec.
    // Without this handler the default action is ExitProcess. Swallow it.
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        return TRUE;
    }
    return FALSE;
}

void Core_DetectResolution(PhantomRecCore* core) {
    core->screenWidth = GetSystemMetrics(SM_CXSCREEN);
    core->screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void Core_ConfigurePipeline(PhantomRecCore* core) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = si.dwNumberOfProcessors;
    core->cpuCoreCount = cores;
    core->dynamicThreads = cores;
    if (core->dynamicThreads < 1) core->dynamicThreads = 1;

    int winVer = GetWindowsVersion();
    if (winVer == 7) {
        core->crf = 23; core->maxrate = 3000; core->bufsize = 6000; core->pipeBufferSizeMB = 2;
    } else if (winVer == 8) {
        core->crf = 23; core->maxrate = 4000; core->bufsize = 8000; core->pipeBufferSizeMB = 4;
    } else if (cores <= 2) {
        core->crf = 23; core->maxrate = 4000; core->bufsize = 8000; core->pipeBufferSizeMB = 4;
    } else if (cores <= 4) {
        core->crf = 23; core->maxrate = 6000; core->bufsize = 12000; core->pipeBufferSizeMB = 8;
    } else if (cores <= 8) {
        core->crf = 23; core->maxrate = 8000; core->bufsize = 16000; core->pipeBufferSizeMB = 16;
    } else {
        core->crf = 23; core->maxrate = 12000; core->bufsize = 24000; core->pipeBufferSizeMB = 32;
    }
}

void Core_SetCaptureMethod(PhantomRecCore* core) {
    char captureInput[512];
    GetCaptureInput(core, captureInput, sizeof(captureInput));
}

void Core_CleanupOrphanedTempFiles(PhantomRecCore* core) {
    char searchPath[MAX_PATH];
    sprintf_s(searchPath, MAX_PATH, "%s\\*_temp.mkv", core->outputDir);
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char fullPath[MAX_PATH];
            sprintf_s(fullPath, MAX_PATH, "%s\\%s", core->outputDir, findData.cFileName);
            DeleteFileA(fullPath);
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
    sprintf_s(searchPath, MAX_PATH, "%s\\*_temp.wav", core->outputDir);
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char fullPath[MAX_PATH];
            sprintf_s(fullPath, MAX_PATH, "%s\\%s", core->outputDir, findData.cFileName);
            DeleteFileA(fullPath);
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
    // Clean up leftover .t0 marker files too
    sprintf_s(searchPath, MAX_PATH, "%s\\*.t0", core->outputDir);
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char fullPath[MAX_PATH];
            sprintf_s(fullPath, MAX_PATH, "%s\\%s", core->outputDir, findData.cFileName);
            DeleteFileA(fullPath);
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
}

// ============================================================================
// Core shutdown
// ============================================================================
void Core_Shutdown(PhantomRecCore* core) {
    (void)core;
    if (g_fmt_ctx) {
        if (g_fmt_ctx->pb) avio_close(g_fmt_ctx->pb);
        avformat_free_context(g_fmt_ctx);
        g_fmt_ctx = NULL;
    }
    g_video_stream = NULL;
    DeleteCriticalSection(&g_muxer_lock);

    if (g_tjc) { tjDestroy(g_tjc); g_tjc = NULL; }
    if (g_mjpeg_pkt) { av_packet_free(&g_mjpeg_pkt); g_mjpeg_pkt = NULL; }
}

// ============================================================================
// In-process MJPEG capture thread (videoEncoder == 2 only)
// ============================================================================
static unsigned int __stdcall CaptureThreadMJPEG(void* param) {
    PhantomRecCore* core = (PhantomRecCore*)param;
    int targetFPS = (core->cpuCoreCount >= 4) ? 60 : 30;

    AVFormatContext* input_ctx = NULL;
    AVCodecContext*  bmp_dec   = NULL;
    AVFrame*         bmp_frame = NULL;
    AVPacket*        in_pkt    = NULL;

    AVFilterGraph*   graph     = NULL;
    AVFilterContext* sink_ctx  = NULL;
    AVFrame*         frame     = NULL;

    int w = 0, h = 0;

    if (core->captureMethod == 2) {
        avdevice_register_all();
        const AVInputFormat* ifmt = av_find_input_format("gdigrab");
        if (!ifmt) return 1;
        AVDictionary* opts = NULL;
        char fpsStr[8];
        snprintf(fpsStr, sizeof(fpsStr), "%d", targetFPS);
        av_dict_set(&opts, "framerate", fpsStr, 0);
        av_dict_set(&opts, "draw_mouse", "1", 0);
        if (avformat_open_input(&input_ctx, "desktop", ifmt, &opts) < 0) {
            av_dict_free(&opts);
            return 1;
        }
        av_dict_free(&opts);
        if (avformat_find_stream_info(input_ctx, NULL) < 0) goto done;

        int v_idx = -1;
        for (unsigned i = 0; i < input_ctx->nb_streams; i++) {
            if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                v_idx = i; break;
            }
        }
        if (v_idx < 0) goto done;
        AVCodecParameters* par = input_ctx->streams[v_idx]->codecpar;
        w = par->width; h = par->height;
        const AVCodec* dec_codec = avcodec_find_decoder(par->codec_id);
        if (!dec_codec) goto done;
        bmp_dec = avcodec_alloc_context3(dec_codec);
        if (!bmp_dec) goto done;
        avcodec_parameters_to_context(bmp_dec, par);
        if (avcodec_open2(bmp_dec, dec_codec, NULL) < 0) goto done;
        bmp_frame = av_frame_alloc();
        in_pkt    = av_packet_alloc();
        if (!bmp_frame || !in_pkt) goto done;
    } else {
        const AVFilter* src  = NULL;
        const AVFilter* hwdl = avfilter_get_by_name("hwdownload");
        const AVFilter* fmt  = avfilter_get_by_name("format");
        const AVFilter* sink = avfilter_get_by_name("buffersink");
        if (!hwdl || !fmt || !sink) return 1;

        graph = avfilter_graph_alloc();
        if (!graph) return 1;

        char src_args[256];
        if (core->captureMethod == 0) {
            src = avfilter_get_by_name("gfxcapture");
            snprintf(src_args, sizeof(src_args),
                     "monitor_idx=0:capture_cursor=1:max_framerate=%d", targetFPS);
        } else {
            src = avfilter_get_by_name("ddagrab");
            snprintf(src_args, sizeof(src_args),
                     "output_idx=0:draw_mouse=1:framerate=%d", targetFPS);
        }
        if (!src) { avfilter_graph_free(&graph); return 1; }

        AVFilterContext *src_ctx = NULL, *hwdl_ctx = NULL, *fmt_ctx = NULL;
        if (avfilter_graph_create_filter(&src_ctx, src, "src", src_args, NULL, graph) < 0) goto done_graph;
        if (avfilter_graph_create_filter(&hwdl_ctx, hwdl, "hwdl", NULL, NULL, graph) < 0) goto done_graph;
        if (avfilter_graph_create_filter(&fmt_ctx, fmt, "fmt", "pix_fmts=bgra", NULL, graph) < 0) goto done_graph;
        if (avfilter_graph_create_filter(&sink_ctx, sink, "sink", NULL, NULL, graph) < 0) goto done_graph;
        if (avfilter_link(src_ctx,  0, hwdl_ctx, 0) < 0) goto done_graph;
        if (avfilter_link(hwdl_ctx, 0, fmt_ctx,  0) < 0) goto done_graph;
        if (avfilter_link(fmt_ctx,  0, sink_ctx, 0) < 0) goto done_graph;
        if (avfilter_graph_config(graph, NULL) < 0) goto done_graph;

        frame = av_frame_alloc();
        if (!frame) goto done_graph;
        if (av_buffersink_get_frame(sink_ctx, frame) < 0) goto done_graph;
        w = frame->width; h = frame->height;
        av_frame_unref(frame);
    }

    while (g_captureRunning && InterlockedCompareExchange(&core->recording, 1, 1) == 1) {
        if (InterlockedCompareExchange(&core->paused, 1, 1) == 1) {
            Sleep(10);
            continue;
        }
        if (core->captureMethod == 2) {
            int rr = av_read_frame(input_ctx, in_pkt);
            if (rr == AVERROR(EAGAIN)) { Sleep(1); continue; }
            if (rr < 0) break;
            if (avcodec_send_packet(bmp_dec, in_pkt) == 0) {
                if (avcodec_receive_frame(bmp_dec, bmp_frame) == 0) {
                    MuxJPEGFrame(core, bmp_frame->data[0], bmp_frame->width,
                                 bmp_frame->height, bmp_frame->linesize[0]);
                    av_frame_unref(bmp_frame);
                }
            }
            av_packet_unref(in_pkt);
        } else {
            int rr = av_buffersink_get_frame(sink_ctx, frame);
            if (rr == AVERROR(EAGAIN)) { Sleep(1); continue; }
            if (rr < 0) break;
            MuxJPEGFrame(core, frame->data[0], frame->width, frame->height, frame->linesize[0]);
            av_frame_unref(frame);
        }
    }

done_graph:
    if (frame)     av_frame_free(&frame);
    if (graph)     avfilter_graph_free(&graph);

done:
    if (in_pkt)    av_packet_free(&in_pkt);
    if (bmp_frame) av_frame_free(&bmp_frame);
    if (bmp_dec)   avcodec_free_context(&bmp_dec);
    if (input_ctx) avformat_close_input(&input_ctx);
    return 0;
}

// ============================================================================
// External child idle thread
// ============================================================================
static unsigned int __stdcall CaptureThreadExternal(void* param) {
    PhantomRecCore* core = (PhantomRecCore*)param;
    while (g_captureRunning && InterlockedCompareExchange(&core->recording, 1, 1) == 1) {
        Sleep(50);
    }
    return 0;
}

// ============================================================================
// Probe for maxsound
// ============================================================================
void Core_ProbeAudio(PhantomRecCore* core) {
    core->audioActive = 0;
    if (core->maxsoundPath[0] == '\0') return;
    if (!Core_FileExists(core->maxsoundPath)) return;

    char probeCmd[1024];
    snprintf(probeCmd, sizeof(probeCmd), "\"%s\" --probe-only", core->maxsoundPath);

    STARTUPINFOA siP = { sizeof(siP) };
    siP.dwFlags = STARTF_USESHOWWINDOW;
    siP.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION piP = {0};

    if (CreateProcessA(NULL, probeCmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &siP, &piP)) {
        WaitForSingleObject(piP.hProcess, 3000);
        DWORD ec = 1;
        GetExitCodeProcess(piP.hProcess, &ec);
        core->audioActive = (ec == 0) ? 1 : 0;
        CloseHandle(piP.hProcess);
        CloseHandle(piP.hThread);
    }
}

// ============================================================================
// Start
// ============================================================================
int Core_StartRecording(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 1) return 0;
    if (InterlockedCompareExchange(&core->converting, 1, 1) == 1) return 0;

    // Tell the user we're preparing. This is the window during which
    // maxenc.exe is starting up and nothing is being captured yet.
    // Without this message the UI looks frozen.
    if (core->onStatusUpdate) core->onStatusUpdate("Arming capture...");
    if (core->onButtonUpdate) core->onButtonUpdate("ARMING...");

    Core_DetectResolution(core);

    char ts[64];
    Core_Timestamp(ts, sizeof(ts));
    strncpy_s(core->segmentBaseName, sizeof(core->segmentBaseName), ts, _TRUNCATE);
    core->pauseSegmentCount = 0;
    core->segmentCount = 1;
    core->totalPausedDurationMs = 0;
    memset(core->segmentAudioDelayMs, 0, sizeof(core->segmentAudioDelayMs));
    memset(core->segmentVideoT0, 0, sizeof(core->segmentVideoT0));
    memset(core->segmentAudioT0, 0, sizeof(core->segmentAudioT0));
    InterlockedExchange(&core->paused, 0);

    sprintf_s(core->tempFile, MAX_PATH, "%s\\%s.mkv", core->outputDir, ts);
    sprintf_s(core->tempAudioFile, MAX_PATH, "%s\\%s_seg0_temp.wav", core->outputDir, ts);
    sprintf_s(core->finalFile, MAX_PATH, "%s\\%s.mkv", core->outputDir, ts);
    strncpy_s(core->segmentFiles[0], MAX_PATH, core->tempFile, _TRUNCATE);

    int wasapiReady = core->audioActive ? 1 : 0;

    InterlockedExchange(&core->recording, 1);

    int useMaxEncChild = (core->maxencPath[0] != '\0' && Core_FileExists(core->maxencPath));
    int useMaxSound    = (core->maxsoundPath[0] != '\0' && Core_FileExists(core->maxsoundPath));
    if (!useMaxSound) wasapiReady = 0;

    if (!useMaxEncChild) {
        if (core->onStatusUpdate) core->onStatusUpdate("maxenc.exe missing - using in-process MJPEG");
        core->videoEncoder = 2;
    } else {
        core->videoEncoder = 0;
    }

    if (useMaxEncChild) {
        char videoCmd[8196];
        BuildMaxEncCommand(core, core->tempFile, videoCmd, sizeof(videoCmd));
        if (!SpawnVideoChild(core, videoCmd, &core->ffmpegProcess)) {
            if (core->onStatusUpdate) core->onStatusUpdate("maxenc.exe spawn failed - using in-process MJPEG");
            core->videoEncoder = 2;
            useMaxEncChild = 0;
        } else {
            // Wait for maxenc's first captured frame so recStart reflects
            // the actual start of the video timeline, not process spawn time.
            // 2 seconds is generous; on every machine tested so far the
            // marker appears within 200-800ms.
            int64_t videoT0 = WaitT0Marker(core->tempFile, 2000);
            core->segmentVideoT0[0] = videoT0;

            if (videoT0 != 0) {
                core->segmentStartTime.QuadPart = videoT0;
                core->recStart.QuadPart = videoT0;
            } else {
                // Fallback: marker never appeared. Fall back to spawn time.
                QueryPerformanceCounter(&core->segmentStartTime);
                QueryPerformanceCounter(&core->recStart);
            }

            core->sessions++;
            g_captureRunning = 1;
            g_hCaptureThread = (HANDLE)_beginthreadex(NULL, 0, CaptureThreadExternal, core, 0, NULL);
            if (!g_hCaptureThread) { g_captureRunning = 0; goto fail; }
        }
    }

    if (!useMaxEncChild) {
        if (avformat_alloc_output_context2(&g_fmt_ctx, NULL, "matroska", core->segmentFiles[0]) < 0) {
            if (core->onStatusUpdate) core->onStatusUpdate("MJPEG: alloc output failed");
            goto fail;
        }
        av_opt_set_int(g_fmt_ctx, "max_muxing_queue_size", 8192, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(g_fmt_ctx, "max_interleave_delta", 1000000, AV_OPT_SEARCH_CHILDREN);
        if (avio_open(&g_fmt_ctx->pb, core->segmentFiles[0], AVIO_FLAG_WRITE) < 0) {
            if (core->onStatusUpdate) core->onStatusUpdate("MJPEG: avio_open failed");
            goto fail;
        }
        g_video_stream = avformat_new_stream(g_fmt_ctx, NULL);
        if (!g_video_stream) goto fail;
        g_video_stream->codecpar->codec_id   = AV_CODEC_ID_MJPEG;
        g_video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        g_video_stream->codecpar->width      = core->screenWidth;
        g_video_stream->codecpar->height     = core->screenHeight;
        g_video_stream->codecpar->format     = AV_PIX_FMT_YUV420P;
        g_video_stream->time_base            = (AVRational){1, 1000000};
        g_video_stream->avg_frame_rate       = (AVRational){60, 1};
        if (avformat_write_header(g_fmt_ctx, NULL) < 0) {
            if (core->onStatusUpdate) core->onStatusUpdate("MJPEG: write_header failed");
            goto fail;
        }
        QueryPerformanceCounter(&core->segmentStartTime);
        QueryPerformanceCounter(&core->recStart);
        core->sessions++;
        g_captureRunning = 1;
        g_hCaptureThread = (HANDLE)_beginthreadex(NULL, 0, CaptureThreadMJPEG, core, 0, NULL);
        if (!g_hCaptureThread) { g_captureRunning = 0; goto fail; }
    }

    if (wasapiReady) {
        int64_t videoT0 = WaitT0Marker(core->tempFile, 500);
        core->segmentVideoT0[0] = videoT0;

        char audioMarkerPath[MAX_PATH];
        snprintf(audioMarkerPath, sizeof(audioMarkerPath), "%s.t0", core->tempAudioFile);

        if (!SpawnMaxSound(core, core->tempAudioFile, audioMarkerPath,
                           &core->ffmpegAudioProcess)) {
            if (core->onStatusUpdate) core->onStatusUpdate("maxsound spawn failed - video only");
        }
    }

    if (core->onStatusUpdate) core->onStatusUpdate("Recording...");
    if (core->onButtonUpdate) core->onButtonUpdate("STOP");
    return 1;

fail:
    InterlockedExchange(&core->recording, 0);
    if (g_fmt_ctx) {
        if (g_fmt_ctx->pb) avio_close(g_fmt_ctx->pb);
        avformat_free_context(g_fmt_ctx);
        g_fmt_ctx = NULL;
    }
    g_video_stream = NULL;
    return 0;
}

// ============================================================================
// Stop
// ============================================================================
void Core_StopRecording(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    core->segmentDurationMs = (now.QuadPart - core->segmentStartTime.QuadPart) * 1000 / core->recFreq.QuadPart;

    long long totalElapsedMs = (now.QuadPart - core->recStart.QuadPart) * 1000 / core->recFreq.QuadPart;
    long long actualDurationMs = totalElapsedMs - core->totalPausedDurationMs;
    if (actualDurationMs < 1000) actualDurationMs = 1000;
    core->lastRecordingDurationMs = (int)actualDurationMs;

    InterlockedExchange(&core->recording, 0);
    InterlockedExchange(&core->paused, 0);

    if (core->videoEncoder == 2) {
        g_captureRunning = 0;
        if (g_hCaptureThread) {
            WaitForSingleObject(g_hCaptureThread, 500);
            CloseHandle(g_hCaptureThread);
            g_hCaptureThread = NULL;
        }
        EnterCriticalSection(&g_muxer_lock);
        if (g_fmt_ctx) {
            av_write_trailer(g_fmt_ctx);
            avio_close(g_fmt_ctx->pb);
            avformat_free_context(g_fmt_ctx);
            g_fmt_ctx = NULL;
        }
        g_video_stream = NULL;
        LeaveCriticalSection(&g_muxer_lock);
    } else {
        g_captureRunning = 0;
        if (g_hCaptureThread) {
            WaitForSingleObject(g_hCaptureThread, 500);
            CloseHandle(g_hCaptureThread);
            g_hCaptureThread = NULL;
        }
        StopVideoChild(&core->ffmpegProcess, 1500);
    }

    StopMaxSound(&core->ffmpegAudioProcess, 1500);

    char losslessFile[MAX_PATH];
    sprintf_s(losslessFile, MAX_PATH, "%s\\%s_lossless.mkv", core->outputDir, core->segmentBaseName);

    if (core->segmentCount == 1) {
        if (Core_FileExists(core->tempAudioFile) && Core_GetFileSize(core->tempAudioFile) > 2048) {
            int64_t vT0 = core->segmentVideoT0[0];
            if (vT0 == 0) vT0 = ReadT0Marker(core->segmentFiles[0]);
            int64_t aT0 = ReadT0Marker(core->tempAudioFile);
            int aDelayMs = 0;
            if (vT0 != 0 && aT0 != 0) {
                aDelayMs = QpcDeltaToMs(core, aT0, vT0);
            }

            char muxCmd[8196];
            if (aDelayMs >= 0) {
                int useD = aDelayMs > 0 ? aDelayMs : 1;
                sprintf_s(muxCmd, sizeof(muxCmd),
                    "\"%s\" -y -loglevel error -i \"%s\" -i \"%s\" "
                    "-c:v copy -c:a pcm_s16le -af adelay=%d:all=1 \"%s\"",
                    core->maxsenginePath, core->segmentFiles[0], core->tempAudioFile,
                    useD, losslessFile);
            } else {
                double trimS = (-aDelayMs) / 1000.0;
                sprintf_s(muxCmd, sizeof(muxCmd),
                    "\"%s\" -y -loglevel error -i \"%s\" -i \"%s\" "
                    "-c:v copy -c:a pcm_s16le "
                    "-af atrim=start=%.3f,asetpts=PTS-STARTPTS \"%s\"",
                    core->maxsenginePath, core->segmentFiles[0], core->tempAudioFile,
                    trimS, losslessFile);
            }

            STARTUPINFOA siMux = { sizeof(siMux) };
            siMux.dwFlags = STARTF_USESHOWWINDOW;
            siMux.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION piMux = {0};

            if (CreateProcessA(NULL, muxCmd, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &siMux, &piMux)) {
                WaitForSingleObject(piMux.hProcess, INFINITE);
                CloseHandle(piMux.hProcess);
                CloseHandle(piMux.hThread);
                // Delete the .t0 markers now that the mux is done with them.
                char vM[MAX_PATH], aM[MAX_PATH];
                snprintf(vM, sizeof(vM), "%s.t0", core->segmentFiles[0]);
                snprintf(aM, sizeof(aM), "%s.t0", core->tempAudioFile);
                DeleteFileWithRetry(vM);
                DeleteFileWithRetry(aM);
                DeleteFileWithRetry(core->segmentFiles[0]);
                DeleteFileWithRetry(core->tempAudioFile);
            } else {
                CopyFileA(core->segmentFiles[0], losslessFile, FALSE);
            }
        } else {
            CopyFileA(core->segmentFiles[0], losslessFile, FALSE);
            DeleteFileWithRetry(core->segmentFiles[0]);
        }
    } else {
        char segmentsTxt[MAX_PATH];
        sprintf_s(segmentsTxt, MAX_PATH, "%s\\segments_%s.txt", core->outputDir, core->segmentBaseName);
        FILE* segFile = NULL;
        fopen_s(&segFile, segmentsTxt, "w");
        int validSegments = 0;

        for (int i = 0; i < core->segmentCount; i++) {
            char segmentAudioFile[MAX_PATH];
            sprintf_s(segmentAudioFile, MAX_PATH, "%s\\%s_seg%d_temp.wav",
                core->outputDir, core->segmentBaseName, i);
            char mergedSegmentFile[MAX_PATH];
            sprintf_s(mergedSegmentFile, MAX_PATH, "%s\\%s_seg%d_merged.mkv",
                core->outputDir, core->segmentBaseName, i);

            if (Core_FileExists(core->segmentFiles[i]) && Core_GetFileSize(core->segmentFiles[i]) > 2048) {
                if (Core_FileExists(segmentAudioFile) && Core_GetFileSize(segmentAudioFile) > 2048) {
                    int64_t vT0 = core->segmentVideoT0[i];
                    if (vT0 == 0) vT0 = ReadT0Marker(core->segmentFiles[i]);
                    int64_t aT0 = ReadT0Marker(segmentAudioFile);
                    int aDelayMs = 0;
                    if (vT0 != 0 && aT0 != 0) {
                        aDelayMs = QpcDeltaToMs(core, aT0, vT0);
                    }

                    char muxCmd[8196];
                    if (aDelayMs >= 0) {
                        int useD = aDelayMs > 0 ? aDelayMs : 1;
                        sprintf_s(muxCmd, sizeof(muxCmd),
                            "\"%s\" -y -loglevel error -i \"%s\" -i \"%s\" "
                            "-c:v copy -c:a pcm_s16le -af adelay=%d:all=1 \"%s\"",
                            core->maxsenginePath, core->segmentFiles[i], segmentAudioFile,
                            useD, mergedSegmentFile);
                    } else {
                        double trimS = (-aDelayMs) / 1000.0;
                        sprintf_s(muxCmd, sizeof(muxCmd),
                            "\"%s\" -y -loglevel error -i \"%s\" -i \"%s\" "
                            "-c:v copy -c:a pcm_s16le "
                            "-af atrim=start=%.3f,asetpts=PTS-STARTPTS \"%s\"",
                            core->maxsenginePath, core->segmentFiles[i], segmentAudioFile,
                            trimS, mergedSegmentFile);
                    }

                    STARTUPINFOA siMux = { sizeof(siMux) };
                    siMux.dwFlags = STARTF_USESHOWWINDOW;
                    siMux.wShowWindow = SW_HIDE;
                    PROCESS_INFORMATION piMux = {0};

                    if (CreateProcessA(NULL, muxCmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &siMux, &piMux)) {
                        WaitForSingleObject(piMux.hProcess, INFINITE);
                        CloseHandle(piMux.hProcess);
                        CloseHandle(piMux.hThread);
                        // Delete the .t0 markers now that the mux is done with them.
                        char vM[MAX_PATH], aM[MAX_PATH];
                        snprintf(vM, sizeof(vM), "%s.t0", core->segmentFiles[i]);
                        snprintf(aM, sizeof(aM), "%s.t0", segmentAudioFile);
                        DeleteFileWithRetry(vM);
                        DeleteFileWithRetry(aM);
                        if (segFile) fprintf(segFile, "file '%s'\r\n", mergedSegmentFile);
                        validSegments++;
                        DeleteFileWithRetry(core->segmentFiles[i]);
                        DeleteFileWithRetry(segmentAudioFile);
                    }
                } else {
                    CopyFileA(core->segmentFiles[i], mergedSegmentFile, FALSE);
                    if (segFile) fprintf(segFile, "file '%s'\r\n", mergedSegmentFile);
                    validSegments++;
                    DeleteFileWithRetry(core->segmentFiles[i]);
                }
            }
        }
        if (segFile) fclose(segFile);

        if (validSegments > 1) {
            char concatCmd[8196];
            sprintf_s(concatCmd, sizeof(concatCmd),
                "\"%s\" -y -loglevel error -f concat -safe 0 -i \"%s\" -c copy \"%s\"",
                core->maxsenginePath, segmentsTxt, losslessFile);
            STARTUPINFOA siConcat = { sizeof(siConcat) };
            siConcat.dwFlags = STARTF_USESHOWWINDOW;
            siConcat.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION piConcat = {0};

            if (CreateProcessA(NULL, concatCmd, NULL, NULL, FALSE,
                CREATE_NO_WINDOW, NULL, NULL, &siConcat, &piConcat)) {
                WaitForSingleObject(piConcat.hProcess, INFINITE);
                CloseHandle(piConcat.hProcess);
                CloseHandle(piConcat.hThread);
                for (int i = 0; i < core->segmentCount; i++) {
                    char mergedFile[MAX_PATH];
                    sprintf_s(mergedFile, MAX_PATH, "%s\\%s_seg%d_merged.mkv",
                        core->outputDir, core->segmentBaseName, i);
                    DeleteFileWithRetry(mergedFile);
                }
                DeleteFileWithRetry(segmentsTxt);
            }
        } else if (validSegments == 1) {
            for (int i = 0; i < core->segmentCount; i++) {
                char mergedFile[MAX_PATH];
                sprintf_s(mergedFile, MAX_PATH, "%s\\%s_seg%d_merged.mkv",
                    core->outputDir, core->segmentBaseName, i);
                if (Core_FileExists(mergedFile)) {
                    CopyFileA(mergedFile, losslessFile, FALSE);
                    DeleteFileWithRetry(mergedFile);
                    break;
                }
            }
            DeleteFileWithRetry(segmentsTxt);
        }
    }

    // Stage 2: x264 conversion
    if (core->convertAfterRecording && core->lastRecordingDurationMs >= 1000) {
        InterlockedExchange(&core->converting, 1);
        core->convertProgress = 0;
        if (core->onStatusUpdate) core->onStatusUpdate("Processing video...");
        if (core->onButtonUpdate) core->onButtonUpdate("Processing...");

        int x264Threads = core->cpuCoreCount / 2;
        if (x264Threads < 1) x264Threads = 1;

        // Universal 60fps CFR output on every capture path. GFX and
        // DDAGrab already deliver 60fps; GDI delivers 30fps natively
        // and Stage 2 duplicates each frame to fill the 60fps timeline.
        // Same output format regardless of which capture API ran.
        char cmdLine[8196];
        sprintf_s(cmdLine, sizeof(cmdLine),
            "\"%s\" -y -progress pipe:1 -loglevel error -i \"%s\" "
            "-r 60 -vsync cfr "
            "-c:v libx264 -preset ultrafast -crf %d -color_range tv "
            "-c:a aac -b:a 96k -af aresample=async=1 "
            "-pix_fmt yuv420p -threads %d "
            "-movflags +faststart \"%s\"",
            core->maxsenginePath, losslessFile,
            core->crf, x264Threads, core->finalFile);

        HANDLE hRead, hWrite;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        CreatePipe(&hRead, &hWrite, &sa, 0);
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA siConv = { sizeof(siConv) };
        siConv.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        siConv.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        siConv.hStdOutput = hWrite;
        siConv.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        siConv.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION convertPI = {0};

        if (CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            NULL, NULL, &siConv, &convertPI)) {
            CloseHandle(hWrite);
            char buf[512];
            char lineBuffer[4096] = {0};
            int lineLen = 0;
            DWORD bytesRead;
            long long totalDurationUs = (long long)core->lastRecordingDurationMs * 1000;

            while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buf[bytesRead] = '\0';
                for (DWORD i = 0; i < bytesRead; i++) {
                    if (buf[i] == '\n') {
                        lineBuffer[lineLen] = '\0';
                        if (strncmp(lineBuffer, "out_time_ms=", 12) == 0) {
                            long long timeUs = _atoi64(lineBuffer + 12);
                            int percent = (totalDurationUs > 0) ? (int)((timeUs * 100) / totalDurationUs) : 0;
                            if (percent > 100) percent = 100;
                            core->convertProgress = percent;
                            if (core->onProgressUpdate) core->onProgressUpdate(percent);
                        }
                        lineLen = 0;
                    } else if (buf[i] != '\r') {
                        if (lineLen < (int)sizeof(lineBuffer) - 1)
                            lineBuffer[lineLen++] = buf[i];
                    }
                }
            }
            CloseHandle(hRead);
            WaitForSingleObject(convertPI.hProcess, INFINITE);
            CloseHandle(convertPI.hProcess);
            CloseHandle(convertPI.hThread);

            DeleteFileA(losslessFile);

            InterlockedExchange(&core->converting, 0);
            core->convertProgress = 0;

            long long fs = Core_GetFileSize(core->finalFile);
            if (fs > 2048) {
                core->totalBytes += fs;
                if (core->onConversionDone) core->onConversionDone(1, core->finalFile);
            } else {
                if (core->onConversionDone) core->onConversionDone(0, NULL);
            }
        } else {
            CloseHandle(hRead);
            CloseHandle(hWrite);
        }
    } else {
        strncpy_s(core->finalFile, MAX_PATH, losslessFile, _TRUNCATE);
        long long fs = Core_GetFileSize(core->finalFile);
        if (fs > 2048) {
            core->totalBytes += fs;
            if (core->onConversionDone) core->onConversionDone(1, core->finalFile);
        } else {
            if (core->onConversionDone) core->onConversionDone(0, NULL);
        }
    }
}

// ============================================================================
// Pause / resume
// ============================================================================
void Core_TogglePause(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) return;
    if (InterlockedCompareExchange(&core->converting, 1, 1) == 1) return;

    int currentPause = InterlockedCompareExchange(&core->paused, 1, 1);

    if (currentPause == 0) {
        // ---------- PAUSE ----------
        InterlockedExchange(&core->paused, 1);
        QueryPerformanceCounter(&core->pauseTime);

        if (core->videoEncoder == 2) {
            g_captureRunning = 0;
            if (g_hCaptureThread) {
                WaitForSingleObject(g_hCaptureThread, 500);
                CloseHandle(g_hCaptureThread);
                g_hCaptureThread = NULL;
            }
            EnterCriticalSection(&g_muxer_lock);
            if (g_fmt_ctx) {
                av_write_trailer(g_fmt_ctx);
                avio_close(g_fmt_ctx->pb);
                avformat_free_context(g_fmt_ctx);
                g_fmt_ctx = NULL;
            }
            g_video_stream = NULL;
            LeaveCriticalSection(&g_muxer_lock);
        } else {
            g_captureRunning = 0;
            if (g_hCaptureThread) {
                WaitForSingleObject(g_hCaptureThread, 500);
                CloseHandle(g_hCaptureThread);
                g_hCaptureThread = NULL;
            }
            StopVideoChild(&core->ffmpegProcess, 1500);
        }

        StopMaxSound(&core->ffmpegAudioProcess, 1500);

        if (core->onStatusUpdate) core->onStatusUpdate("PAUSED");
        if (core->onButtonUpdate) core->onButtonUpdate("RESUME");
        return;
    }

    // ---------- RESUME ----------
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    core->totalPausedDurationMs += (long long)((now.QuadPart - core->pauseTime.QuadPart) * 1000 / core->recFreq.QuadPart);

    if (core->segmentCount >= MAX_SEGMENTS) {
        if (core->onStatusUpdate) core->onStatusUpdate("Too many segments - stop and restart");
        return;
    }

    core->pauseSegmentCount++;
    int newIdx = core->segmentCount;
    sprintf_s(core->segmentFiles[newIdx], MAX_PATH,
        "%s\\%s_seg%d_temp.mkv", core->outputDir, core->segmentBaseName, core->pauseSegmentCount);
    sprintf_s(core->tempAudioFile, MAX_PATH,
        "%s\\%s_seg%d_temp.wav", core->outputDir, core->segmentBaseName, core->pauseSegmentCount);
    core->segmentCount++;

    const char* newSeg = core->segmentFiles[newIdx];

    if (core->videoEncoder == 0) {
        char videoCmd[8196];
        BuildMaxEncCommand(core, newSeg, videoCmd, sizeof(videoCmd));
        if (!SpawnVideoChild(core, videoCmd, &core->ffmpegProcess)) goto resume_fail;
    } else {
        if (avformat_alloc_output_context2(&g_fmt_ctx, NULL, "matroska", newSeg) < 0) goto resume_fail;
        av_opt_set_int(g_fmt_ctx, "max_muxing_queue_size", 8192, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(g_fmt_ctx, "max_interleave_delta", 1000000, AV_OPT_SEARCH_CHILDREN);
        if (avio_open(&g_fmt_ctx->pb, newSeg, AVIO_FLAG_WRITE) < 0) goto resume_fail;
        g_video_stream = avformat_new_stream(g_fmt_ctx, NULL);
        if (!g_video_stream) goto resume_fail;
        g_video_stream->codecpar->codec_id   = AV_CODEC_ID_MJPEG;
        g_video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        g_video_stream->codecpar->width      = core->screenWidth;
        g_video_stream->codecpar->height     = core->screenHeight;
        g_video_stream->codecpar->format     = AV_PIX_FMT_YUV420P;
        g_video_stream->time_base            = (AVRational){1, 1000000};
        g_video_stream->avg_frame_rate       = (AVRational){60, 1};
        if (avformat_write_header(g_fmt_ctx, NULL) < 0) goto resume_fail;
    }

    // Wait for the new segment's video t0, then spawn maxsound.
    int wasapiReady = core->audioActive ? 1 : 0;
    if (wasapiReady) {
        int64_t videoT0 = WaitT0Marker(newSeg, 500);
        core->segmentVideoT0[newIdx] = videoT0;

        char audioMarkerPath[MAX_PATH];
        snprintf(audioMarkerPath, sizeof(audioMarkerPath), "%s.t0", core->tempAudioFile);

        if (!SpawnMaxSound(core, core->tempAudioFile, audioMarkerPath,
                           &core->ffmpegAudioProcess)) {
            if (core->onStatusUpdate) core->onStatusUpdate("Resume: maxsound failed - video only");
        }
    }

    QueryPerformanceCounter(&core->segmentStartTime);
    g_captureRunning = 1;
    if (core->videoEncoder == 2) {
        g_hCaptureThread = (HANDLE)_beginthreadex(NULL, 0, CaptureThreadMJPEG, core, 0, NULL);
    } else {
        g_hCaptureThread = (HANDLE)_beginthreadex(NULL, 0, CaptureThreadExternal, core, 0, NULL);
    }
    if (!g_hCaptureThread) goto resume_fail;

    InterlockedExchange(&core->paused, 0);
    if (core->onStatusUpdate) core->onStatusUpdate("Recording...");
    if (core->onButtonUpdate) core->onButtonUpdate("STOP");
    return;

resume_fail:
    if (g_hCaptureThread) { g_captureRunning = 0; WaitForSingleObject(g_hCaptureThread, 500); CloseHandle(g_hCaptureThread); g_hCaptureThread = NULL; }
    if (g_fmt_ctx) {
        if (g_fmt_ctx->pb) avio_close(g_fmt_ctx->pb);
        avformat_free_context(g_fmt_ctx);
        g_fmt_ctx = NULL;
    }
    g_video_stream = NULL;
    InterlockedExchange(&core->recording, 0);
    InterlockedExchange(&core->paused, 0);
    if (core->onStatusUpdate) core->onStatusUpdate("Resume failed - recording stopped");
    if (core->onButtonUpdate) core->onButtonUpdate("START");
}

// ============================================================================
// Status queries
// ============================================================================
int Core_IsRecording(const PhantomRecCore* core) {
    return InterlockedCompareExchange((LONG*)&core->recording, 1, 1);
}
int Core_IsPaused(const PhantomRecCore* core) {
    return InterlockedCompareExchange((LONG*)&core->paused, 1, 1);
}
int Core_IsConverting(const PhantomRecCore* core) {
    return InterlockedCompareExchange((LONG*)&core->converting, 1, 1);
}
int Core_GetProgress(const PhantomRecCore* core) {
    return core->convertProgress;
}
int Core_GetAudioStatus(const PhantomRecCore* core) {
    return core->audioActive;
}
int Core_GetSegmentCount(const PhantomRecCore* core) {
    return core->segmentCount;
}
long long Core_GetTotalBytes(const PhantomRecCore* core) {
    return core->totalBytes;
}
