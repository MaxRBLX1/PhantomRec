// phantomrec_core.c — PhantomRec v1.9.6 Pure C Core
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Direct FFmpeg launch (no cmd.exe wrapper) for full control.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <powrprof.h>
#include <avrt.h>
#include <process.h>
#include <initguid.h>

#include "phantomrec_core.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "powrprof.lib")

// ============================================================================
// Internal helpers
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

static void GetCaptureFilter(const PhantomRecCore* core, char* buf, int bufsize) {
    if (core->captureMethod == 0 || core->captureMethod == 1) {
        strncpy_s(buf, bufsize, " -vf \"hwdownload,format=bgra,format=yuv420p\"", _TRUNCATE);
    } else {
        strncpy_s(buf, bufsize, " -vf \"format=yuv420p\"", _TRUNCATE);
    }
}

// ============================================================================
// Helper: close console window of given process ID
// ============================================================================

static BOOL CALLBACK CloseConsoleWindowEnumProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = (DWORD)lParam;
    DWORD windowPid;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == pid) {
        char className[64];
        GetClassNameA(hwnd, className, sizeof(className));
        if (strcmp(className, "ConsoleWindowClass") == 0) {
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return FALSE;
        }
    }
    return TRUE;
}

static void CloseConsoleWindow(DWORD pid) {
    if (pid) EnumWindows(CloseConsoleWindowEnumProc, (LPARAM)pid);
}

// ============================================================================
// Hard Stop FFmpeg Process (v1.9.6 - Bulletproof with timeout)
// ============================================================================

static void StopFFmpegProcess(PhantomRecCore* core, DWORD timeoutMs) {
    if (!core->ffmpegProcess.hProcess) return;

    DWORD pid = core->ffmpegProcess.dwProcessId;
    HANDLE hProcess = core->ffmpegProcess.hProcess;

    // Step 1: Attach and send CTRL_BREAK
    if (AttachConsole(pid)) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
        Sleep(timeoutMs);
        FreeConsole();
    }

    // Step 2: If still alive, try CTRL_C
    DWORD exitCode;
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
        if (AttachConsole(pid)) {
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
            Sleep(timeoutMs);
            FreeConsole();
        }
    }

    // Step 3: Still alive? Terminate.
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
        TerminateProcess(hProcess, 0);
        WaitForSingleObject(hProcess, 1000);
    }

    // Step 4: Close handles
    CloseHandle(hProcess);
    CloseHandle(core->ffmpegProcess.hThread);
    memset(&core->ffmpegProcess, 0, sizeof(core->ffmpegProcess));
}

// ============================================================================
// Capture Method Management
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
        if (winVer >= 10) {
            method = (core->cpuCoreCount >= 8) ? CAPTURE_GFX : CAPTURE_DDAGRAB;
        } else if (winVer >= 8) {
            method = CAPTURE_DDAGRAB;
        } else {
            method = CAPTURE_GDI;
        }
    }
    // fallback chain
    if (method == CAPTURE_GFX && winVer < 10) method = CAPTURE_DDAGRAB;
    if (method == CAPTURE_DDAGRAB && winVer < 8) method = CAPTURE_GDI;

    switch (method) {
    case CAPTURE_GFX:
        core->captureMethod = 0;
        strncpy_s(buf, bufsize, " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1", _TRUNCATE);
        break;
    case CAPTURE_DDAGRAB:
        core->captureMethod = 1;
        strncpy_s(buf, bufsize, " -f lavfi -i ddagrab=0:framerate=60", _TRUNCATE);
        break;
    case CAPTURE_GDI:
    default:
        core->captureMethod = 2;
        strncpy_s(buf, bufsize, " -f gdigrab -framerate 60 -i desktop", _TRUNCATE);
        break;
    }
}

// ============================================================================
// Utility functions
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
        return 1;
    }
    sprintf_s(testPath, MAX_PATH, "%s\\ffmpeg.exe", local);
    if (Core_FileExists(testPath)) {
        strncpy_s(core->maxsenginePath, MAX_PATH, testPath, _TRUNCATE);
        return 1;
    }
    return 0;
}

// ============================================================================
// WASAPI Audio Capture
// ============================================================================

static int InitializeWASAPI(PhantomRecCore* core) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE) return 0;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) { CoUninitialize(); return 0; }
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) { enumerator->lpVtbl->Release(enumerator); CoUninitialize(); return 0; }
    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&core->audioClient);
    device->lpVtbl->Release(device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) { CoUninitialize(); return 0; }
    hr = core->audioClient->lpVtbl->GetMixFormat(core->audioClient, &core->waveFormat);
    if (FAILED(hr)) {
        core->audioClient->lpVtbl->Release(core->audioClient);
        core->audioClient = NULL;
        CoUninitialize();
        return 0;
    }
    core->hAudioReadyEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!core->hAudioReadyEvent) {
        CoTaskMemFree(core->waveFormat);
        core->waveFormat = NULL;
        core->audioClient->lpVtbl->Release(core->audioClient);
        core->audioClient = NULL;
        CoUninitialize();
        return 0;
    }
    hr = core->audioClient->lpVtbl->Initialize(core->audioClient,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, core->waveFormat, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(core->waveFormat); core->waveFormat = NULL;
        core->audioClient->lpVtbl->Release(core->audioClient); core->audioClient = NULL;
        CloseHandle(core->hAudioReadyEvent); core->hAudioReadyEvent = NULL;
        CoUninitialize();
        return 0;
    }
    core->audioClient->lpVtbl->SetEventHandle(core->audioClient, core->hAudioReadyEvent);
    core->audioClient->lpVtbl->GetService(core->audioClient, &IID_IAudioCaptureClient, (void**)&core->captureClient);
    if (core->waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ex = (WAVEFORMATEXTENSIBLE*)core->waveFormat;
        core->audioBitsPerSample = (IsEqualGUID(&ex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) ? 32 : 16;
    } else if (core->waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        core->audioBitsPerSample = 32;
    } else {
        core->audioBitsPerSample = 16;
    }
    core->audioClient->lpVtbl->Start(core->audioClient);
    core->audioActive = 1;
    return 1;
}

static void CleanupWASAPI(PhantomRecCore* core) {
    if (core->audioActive) {
        core->audioClient->lpVtbl->Stop(core->audioClient);
        if (core->captureClient) { core->captureClient->lpVtbl->Release(core->captureClient); core->captureClient = NULL; }
        if (core->audioClient) { core->audioClient->lpVtbl->Release(core->audioClient); core->audioClient = NULL; }
        if (core->waveFormat) { CoTaskMemFree(core->waveFormat); core->waveFormat = NULL; }
        if (core->hAudioReadyEvent) { CloseHandle(core->hAudioReadyEvent); core->hAudioReadyEvent = NULL; }
        core->audioActive = 0;
        CoUninitialize();
    }
}

// ============================================================================
// Audio capture thread
// ============================================================================

static unsigned int __stdcall AudioToPipeThread(void* param) {
    PhantomRecCore* core = (PhantomRecCore*)param;
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    WaitForSingleObject(core->hAudioStartEvent, INFINITE);
    UINT32 packetLength = 0;
    while (InterlockedCompareExchange(&core->recording, 1, 1) == 1) {
        while (InterlockedCompareExchange(&core->paused, 1, 1) == 1 && InterlockedCompareExchange(&core->recording, 1, 1) == 1) { Sleep(100); }
        if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) break;
        DWORD waitResult = WaitForSingleObject(core->hAudioReadyEvent, 1000);
        if (waitResult != WAIT_OBJECT_0) {
            if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) break;
            continue;
        }
        HRESULT hr = core->captureClient->lpVtbl->GetNextPacketSize(core->captureClient, &packetLength);
        if (FAILED(hr)) break;
        while (packetLength > 0) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            UINT64 devicePosition = 0, qpcTimestamp = 0;
            hr = core->captureClient->lpVtbl->GetBuffer(core->captureClient, &data, &frames, &flags, &devicePosition, &qpcTimestamp);
            if (SUCCEEDED(hr)) {
                size_t size = frames * core->waveFormat->nBlockAlign;
                BYTE* writeData = data;
                BYTE* silenceBuf = NULL;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silenceBuf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
                    if (silenceBuf) {
                        memset(silenceBuf, 0, size);
                        writeData = silenceBuf;
                    }
                }
                DWORD written = 0;
                WriteFile(core->hAudioPipeWrite, writeData, (DWORD)size, &written, NULL);
                core->captureClient->lpVtbl->ReleaseBuffer(core->captureClient, frames);
                if (silenceBuf) HeapFree(GetProcessHeap(), 0, silenceBuf);
            }
            hr = core->captureClient->lpVtbl->GetNextPacketSize(core->captureClient, &packetLength);
            if (FAILED(hr)) break;
        }
    }
    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    return 0;
}

// ============================================================================
// Build FFmpeg command line (NO cmd.exe wrapper)
// ============================================================================

static void BuildCaptureCommand(PhantomRecCore* core, const char* outputFile, int hasAudio, char* cmdLine, int cmdSize) {
    char captureInput[512];
    char captureFilter[256];
    GetCaptureInput(core, captureInput, sizeof(captureInput));
    GetCaptureFilter(core, captureFilter, sizeof(captureFilter));
    const char* rtbufsize = "2048M";
    int encoderSlices = (core->cpuCoreCount <= 2) ? 1 : core->cpuCoreCount;
    if (encoderSlices < 1) encoderSlices = 1;
    core->dynamicThreads = encoderSlices;

    int vq = (core->videoQueueSize > 0) ? core->videoQueueSize : 4096;

    // Start with the executable path (quoted)
    int offset = sprintf_s(cmdLine, cmdSize,
        "\"%s\" -y -hide_banner -loglevel error"
        " -rtbufsize %s"
        " -thread_queue_size %d"
        "%s",
        core->maxsenginePath, rtbufsize, vq, captureInput);

    if (hasAudio) {
        offset += sprintf_s(cmdLine + offset, cmdSize - offset,
            " -itsoffset 0.0 -thread_queue_size %d -f %s -ar %d -ac %d -i pipe:0",
            vq, core->audioFormat, core->audioSampleRate, core->audioChannels);
    }

    offset += sprintf_s(cmdLine + offset, cmdSize - offset,
        "%s"
        " -max_muxing_queue_size 50000"
        " -c:v utvideo -pred median -slices %d"
        " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv",
        captureFilter, encoderSlices);

    if (hasAudio) {
        offset += sprintf_s(cmdLine + offset, cmdSize - offset, " -c:a copy");
    }

    // Output file (quoted)
    sprintf_s(cmdLine + offset, cmdSize - offset,
        " -fflags +genpts -f matroska \"%s\"",
        outputFile);
}

// ============================================================================
// Recording engine
// ============================================================================

void Core_Init(PhantomRecCore* core, const char* maxsenginePath, const char* outputDir) {
    memset(core, 0, sizeof(PhantomRecCore));
    if (maxsenginePath) strncpy_s(core->maxsenginePath, MAX_PATH, maxsenginePath, _TRUNCATE);
    if (outputDir) strncpy_s(core->outputDir, MAX_PATH, outputDir, _TRUNCATE);
    core->convertAfterRecording = 1;
    core->pipeBufferSizeMB = 8;
    core->dynamicThreads = 1;
    core->hAudioThread = NULL;
    core->hAudioStartEvent = NULL;
    core->videoQueueSize = 4096;
    QueryPerformanceFrequency(&core->recFreq);
    InterlockedExchange(&core->recording, 0);
    InterlockedExchange(&core->paused, 0);
    InterlockedExchange(&core->converting, 0);
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
        core->crf = 26; core->maxrate = 3000; core->bufsize = 6000; core->pipeBufferSizeMB = 2;
    } else if (winVer == 8) {
        core->crf = 26; core->maxrate = 4000; core->bufsize = 8000; core->pipeBufferSizeMB = 4;
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

void Core_WarmEngine(PhantomRecCore* core) {
    char captureInput[512];
    GetCaptureInput(core, captureInput, sizeof(captureInput));
    int warmupSlices = (core->cpuCoreCount <= 2) ? 1 : core->cpuCoreCount; // Dual-core optimization
    if (warmupSlices < 1) warmupSlices = 1;
    char warmupCmd[1024];
    if (core->captureMethod <= 1) {
        sprintf_s(warmupCmd, sizeof(warmupCmd),
            "\"%s\" -y -hide_banner -loglevel error %s -frames:v 3 -c:v utvideo -pred median -slices %d -f null NUL",
            core->maxsenginePath, captureInput, warmupSlices);
    } else {
        sprintf_s(warmupCmd, sizeof(warmupCmd),
            "\"%s\" -y -hide_banner -loglevel error -f gdigrab -framerate 60 -i desktop -frames:v 3 -c:v utvideo -pred median -slices %d -f null NUL",
            core->maxsenginePath, warmupSlices);
    }
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, warmupCmd, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
        NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
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
}

void Core_Shutdown(PhantomRecCore* core) {
    // stub – cleanup handled in WinMain
}

// ============================================================================
// Start / Stop Recording
// ============================================================================

int Core_StartRecording(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 1) return 0;
    if (InterlockedCompareExchange(&core->converting, 1, 1) == 1) return 0;

    if (!core->powerPlanChanged) {
        GUID highPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        GUID* pOriginalGuid = NULL;
        if (PowerGetActiveScheme(NULL, &pOriginalGuid) == ERROR_SUCCESS) {
            core->originalPowerPlan = *pOriginalGuid;
            LocalFree(pOriginalGuid);
            PowerSetActiveScheme(NULL, &highPerf);
            core->powerPlanChanged = 1;
        }
    }

    char ts[64];
    Core_Timestamp(ts, sizeof(ts));
    strncpy_s(core->segmentBaseName, sizeof(core->segmentBaseName), ts, _TRUNCATE);
    core->pauseSegmentCount = 0;
    core->segmentCount = 1;
    core->totalPausedDurationMs = 0;
    InterlockedExchange(&core->paused, 0);

    sprintf_s(core->tempFile, MAX_PATH, "%s\\%s_seg0_temp.mkv", core->outputDir, ts);
    sprintf_s(core->finalFile, MAX_PATH, "%s\\%s.mkv", core->outputDir, ts);
    strncpy_s(core->segmentFiles[0], MAX_PATH, core->tempFile, _TRUNCATE);

    int wasapiReady = InitializeWASAPI(core);
    if (wasapiReady) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        CreatePipe(&core->hAudioPipeRead, &core->hAudioPipeWrite, &sa, core->pipeBufferSizeMB * 1024 * 1024);
        SetHandleInformation(core->hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
    }

    strncpy_s(core->audioFormat, sizeof(core->audioFormat), "s16le", _TRUNCATE);
    core->audioSampleRate = 48000;
    core->audioChannels = 2;
    if (wasapiReady && core->waveFormat) {
        strncpy_s(core->audioFormat, sizeof(core->audioFormat),
            (core->audioBitsPerSample == 32) ? "f32le" : "s16le", _TRUNCATE);
        core->audioSampleRate = core->waveFormat->nSamplesPerSec;
        core->audioChannels = core->waveFormat->nChannels;
    }

    InterlockedExchange(&core->recording, 1);
    char cmdLine[8196];
    BuildCaptureCommand(core, core->tempFile, wasapiReady, cmdLine, sizeof(cmdLine));

    if (wasapiReady) {
        if (core->hAudioStartEvent) CloseHandle(core->hAudioStartEvent);
        core->hAudioStartEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        core->hAudioThread = (HANDLE)_beginthreadex(NULL, 0, AudioToPipeThread, core, 0, NULL);
    }

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (wasapiReady && core->hAudioPipeRead) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = core->hAudioPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_HIDE;  // hide the console window

    // Launch FFmpeg directly (no cmd.exe)
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, &core->ffmpegProcess)) {
        InterlockedExchange(&core->recording, 0);
        if (core->hAudioStartEvent) {
            SetEvent(core->hAudioStartEvent);
            CloseHandle(core->hAudioStartEvent);
            core->hAudioStartEvent = NULL;
        }
        if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
        if (core->hAudioThread) {
            WaitForSingleObject(core->hAudioThread, 5000);
            CloseHandle(core->hAudioThread);
            core->hAudioThread = NULL;
        }
        if (core->hAudioPipeWrite) { CloseHandle(core->hAudioPipeWrite); core->hAudioPipeWrite = NULL; }
        CleanupWASAPI(core);
        return 0;
    }

    if (core->hAudioStartEvent) {
        SetEvent(core->hAudioStartEvent);
        CloseHandle(core->hAudioStartEvent);
        core->hAudioStartEvent = NULL;
    }
    if (core->hAudioPipeRead) {
        CloseHandle(core->hAudioPipeRead);
        core->hAudioPipeRead = NULL;
    }

    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    QueryPerformanceCounter(&core->recStart);
    core->sessions++;

    if (core->onStatusUpdate) core->onStatusUpdate("Recording...");
    if (core->onButtonUpdate) core->onButtonUpdate("STOP");
    return 1;
}

void Core_StopRecording(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long totalElapsedMs = (now.QuadPart - core->recStart.QuadPart) * 1000 / core->recFreq.QuadPart;
    long long actualDurationMs = totalElapsedMs - core->totalPausedDurationMs;
    if (actualDurationMs < 1000) actualDurationMs = 1000;
    core->lastRecordingDurationMs = (int)actualDurationMs;

    InterlockedExchange(&core->recording, 0);
    InterlockedExchange(&core->paused, 0);

    if (core->powerPlanChanged) {
        PowerSetActiveScheme(NULL, &core->originalPowerPlan);
        core->powerPlanChanged = 0;
    }

    // Close pipe write BEFORE waiting for audio thread
    if (core->hAudioPipeWrite) {
        CloseHandle(core->hAudioPipeWrite);
        core->hAudioPipeWrite = NULL;
    }
    if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
    if (core->hAudioThread) {
        WaitForSingleObject(core->hAudioThread, 5000);
        CloseHandle(core->hAudioThread);
        core->hAudioThread = NULL;
    }
    CleanupWASAPI(core);

    // Save PID before stopping FFmpeg (will be zeroed)
    DWORD pid = core->ffmpegProcess.dwProcessId;
    // Stop FFmpeg (2 seconds timeout)
    StopFFmpegProcess(core, 2000);

    // Close the console window that FFmpeg was running in
    if (pid) {
        CloseConsoleWindow(pid);
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    Sleep(1500);

    // ----- Segment list and concatenation -----
    char segmentsTxt[MAX_PATH];
    sprintf_s(segmentsTxt, MAX_PATH, "%s\\segments.txt", core->outputDir);
    FILE* segFile = NULL;
    fopen_s(&segFile, segmentsTxt, "w");
    int validSegments = 0;
    for (int i = 0; i < core->segmentCount; i++) {
        if (Core_FileExists(core->segmentFiles[i]) && Core_GetFileSize(core->segmentFiles[i]) > 2048) {
            fprintf(segFile, "file '%s'\r\n", core->segmentFiles[i]);
            validSegments++;
        }
    }
    if (segFile) fclose(segFile);

    if (validSegments == 0) {
        DeleteFileA(segmentsTxt);
        if (core->onStatusUpdate) core->onStatusUpdate("No recording data found");
        return;
    }

    // Always concatenate segments into a single lossless file
    char losslessFile[MAX_PATH];
    sprintf_s(losslessFile, MAX_PATH, "%s\\%s_lossless.mkv", core->outputDir, core->segmentBaseName);

    char concatCmd[8196];
    // Concatenation – we still use cmd for simplicity, but it's a one‑time operation.
    sprintf_s(concatCmd, sizeof(concatCmd),
        "cmd.exe /c \"\"%s\" -y -loglevel error -f concat -safe 0 -i \"%s\" -c copy \"%s\"\"",
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
        DeleteFileA(segmentsTxt);
        for (int i = 0; i < core->segmentCount; i++) {
            if (Core_FileExists(core->segmentFiles[i])) DeleteFileA(core->segmentFiles[i]);
        }
    } else {
        DeleteFileA(segmentsTxt);
        if (core->onStatusUpdate) core->onStatusUpdate("Concatenation failed");
        return;
    }

    // Now decide: compress or keep lossless?
    if (core->convertAfterRecording && core->lastRecordingDurationMs >= 1000) {
        InterlockedExchange(&core->converting, 1);
        core->convertProgress = 0;
        if (core->onStatusUpdate) core->onStatusUpdate("Processing video...");
        if (core->onButtonUpdate) core->onButtonUpdate("Processing...");

        char cmdLine[8196];
        // Compression – directly launch FFmpeg
        sprintf_s(cmdLine, sizeof(cmdLine),
            "\"%s\" -y -progress pipe:1 -loglevel error -i \"%s\" "
            "-c:v libx264 -preset ultrafast -crf %d -c:a aac -b:a 128k -async 1 "
            "-pix_fmt yuv420p -r 60 -fps_mode cfr \"%s\"",
            core->maxsenginePath, losslessFile, core->crf, core->finalFile);

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
        SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

        if (CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
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
            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

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
        // keep lossless
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

void Core_TogglePause(PhantomRecCore* core) {
    if (InterlockedCompareExchange(&core->recording, 1, 1) == 0) return;
    if (InterlockedCompareExchange(&core->converting, 1, 1) == 1) return;

    int currentPause = InterlockedCompareExchange(&core->paused, 1, 1);
    if (currentPause == 0) {
        // Pause
        InterlockedExchange(&core->paused, 1);
        QueryPerformanceCounter(&core->pauseTime);

        // close pipe write before stopping audio thread
        if (core->hAudioPipeWrite) {
            CloseHandle(core->hAudioPipeWrite);
            core->hAudioPipeWrite = NULL;
        }
        if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
        if (core->hAudioThread) {
            WaitForSingleObject(core->hAudioThread, 100);
            CloseHandle(core->hAudioThread);
            core->hAudioThread = NULL;
        }
        CleanupWASAPI(core);

        // Save PID before stopping FFmpeg
        DWORD pid = core->ffmpegProcess.dwProcessId;
        // Stop FFmpeg with a short 500ms timeout
        StopFFmpegProcess(core, 500);
        // Close the console window
        if (pid) {
            CloseConsoleWindow(pid);
        }

        if (core->onStatusUpdate) core->onStatusUpdate("PAUSED");
        if (core->onButtonUpdate) core->onButtonUpdate("RESUME");

    } else {
        // Resume
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        core->totalPausedDurationMs += (long long)((now.QuadPart - core->pauseTime.QuadPart) * 1000 / core->recFreq.QuadPart);

        if (core->segmentCount >= 64) {
            if (core->onStatusUpdate) core->onStatusUpdate("Too many segments – stop and restart");
            return;
        }

        core->pauseSegmentCount++;
        sprintf_s(core->segmentFiles[core->segmentCount], MAX_PATH,
            "%s\\%s_seg%d_temp.mkv",
            core->outputDir, core->segmentBaseName, core->pauseSegmentCount);
        core->segmentCount++;

        int wasapiReady = InitializeWASAPI(core);
        if (wasapiReady) {
            SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
            CreatePipe(&core->hAudioPipeRead, &core->hAudioPipeWrite, &sa, core->pipeBufferSizeMB * 1024 * 1024);
            SetHandleInformation(core->hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
        }

        if (wasapiReady) {
            if (core->hAudioStartEvent) CloseHandle(core->hAudioStartEvent);
            core->hAudioStartEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
            core->hAudioThread = (HANDLE)_beginthreadex(NULL, 0, AudioToPipeThread, core, 0, NULL);
        }

        char cmdLine[8196];
        BuildCaptureCommand(core, core->segmentFiles[core->segmentCount - 1], wasapiReady, cmdLine, sizeof(cmdLine));

        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        if (wasapiReady && core->hAudioPipeRead) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = core->hAudioPipeRead;
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        }
        si.wShowWindow = SW_HIDE;

        if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
            CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
            NULL, NULL, &si, &core->ffmpegProcess)) {
            // Resume failed – stop entirely
            InterlockedExchange(&core->recording, 0);
            InterlockedExchange(&core->paused, 0);

            if (core->hAudioStartEvent) {
                SetEvent(core->hAudioStartEvent);
                CloseHandle(core->hAudioStartEvent);
                core->hAudioStartEvent = NULL;
            }
            if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
            if (core->hAudioThread) {
                WaitForSingleObject(core->hAudioThread, 100);
                CloseHandle(core->hAudioThread);
                core->hAudioThread = NULL;
            }
            if (core->hAudioPipeWrite) {
                CloseHandle(core->hAudioPipeWrite);
                core->hAudioPipeWrite = NULL;
            }
            CleanupWASAPI(core);

            if (core->powerPlanChanged) {
                PowerSetActiveScheme(NULL, &core->originalPowerPlan);
                core->powerPlanChanged = 0;
            }

            if (core->onStatusUpdate) core->onStatusUpdate("Resume failed – recording stopped");
            if (core->onButtonUpdate) core->onButtonUpdate("START");
            return;
        }

        if (core->hAudioStartEvent) {
            SetEvent(core->hAudioStartEvent);
            CloseHandle(core->hAudioStartEvent);
            core->hAudioStartEvent = NULL;
        }
        if (core->hAudioPipeRead) {
            CloseHandle(core->hAudioPipeRead);
            core->hAudioPipeRead = NULL;
        }

        InterlockedExchange(&core->paused, 0);
        if (core->onStatusUpdate) core->onStatusUpdate("Recording...");
        if (core->onButtonUpdate) core->onButtonUpdate("STOP");
    }
}

// ============================================================================
// Status queries (use Interlocked to read)
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
