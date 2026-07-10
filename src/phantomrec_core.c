// phantomrec_core.c — PhantomRec v1.9.5 Pure C Core
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Max'sEngine™ | DXGI/GFX/DDAGrab/GDI Capture | UTVideo Lossless | x264 Stage 2 | WASAPI Audio
//
// Compile as part of PhantomRec.exe (link with C++ UI):
//   gcc -std=c11 -O2 -c phantomrec_core.c -o phantomrec_core.o
//   g++ -std=c++17 -O2 phantomrec_core.o PhantomRec.cpp -o PhantomRec.exe ...

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
        // GPU paths: hwdownload + convert to yuv420p (halves chroma, HDD-safe)
        strncpy_s(buf, bufsize, " -vf \"hwdownload,format=bgra,format=yuv420p\"", _TRUNCATE);
    } else {
        // GDI: already in system memory, just convert to yuv420p
        strncpy_s(buf, bufsize, " -vf \"format=yuv420p\"", _TRUNCATE);
    }
}


// ============================================================================
// Capture Method Management
// ============================================================================

static CaptureMethod g_UserCaptureMethod = CAPTURE_AUTO;

void Core_SetCaptureMethodEx(PhantomRecCore* core, CaptureMethod method) {
    g_UserCaptureMethod = method;
    // Re-apply the capture input string
    Core_SetCaptureMethod(core);
}

const char* Core_GetCaptureMethodDesc(const PhantomRecCore* core) {
    switch (core->captureMethod) {
    case 0: return "GFX Capture (D3D11, GPU, 60 FPS)";
    case 1: return "DDAGrab (DXGI, GPU, 60 FPS)";
    case 2: return "GDI (CPU, 55 FPS, software)";
    default: return "Unknown";
    }
}

static void GetCaptureInput(PhantomRecCore* core, char* buf, int bufsize) {
    int winVer = GetWindowsVersion();
    
    // Determine which method to use
    CaptureMethod method = g_UserCaptureMethod;
    
    if (method == CAPTURE_AUTO) {
        // Auto: pick the best method available for this OS
        if (winVer >= 10) {
            method = CAPTURE_GFX;        // Win10/11: GPU zero-copy, best quality
        } else if (winVer >= 8) {
            method = CAPTURE_DDAGRAB;    // Win8/8.1: GPU DirectDraw, 60 FPS
        } else {
            method = CAPTURE_GDI;        // Win7/Vista: CPU software, 55 FPS sweet spot
        }
    }
    
    // SAFETY NET: Graceful fallback chain — every method degrades if OS doesn't support it
    // GFX requires Win10+ — fallback to DDAGrab
    if (method == CAPTURE_GFX && winVer < 10) {
        method = CAPTURE_DDAGRAB;
    }
    // DDAGrab requires Win8+ — fallback to GDI
    if (method == CAPTURE_DDAGRAB && winVer < 8) {
        method = CAPTURE_GDI;
    }
    // GDI is the universal floor — works on Vista through Win11, no fallback needed
    
    switch (method) {
    case CAPTURE_GFX:
        core->captureMethod = 0;
        // Explicit framerate ensures VFR recording as promised
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
// Audio capture thread (launched internally)
// ============================================================================

static unsigned int __stdcall AudioToPipeThread(void* param) {
    PhantomRecCore* core = (PhantomRecCore*)param;
    
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    
	WaitForSingleObject(core->hAudioStartEvent, INFINITE);
    UINT32 packetLength = 0;
    // FIX: removed dead firstPacketSent / hAudioSyncEvent code
    
    while (core->recording) {
        while (core->paused && core->recording) { Sleep(100); }
        if (!core->recording) break;
        
        DWORD waitResult = WaitForSingleObject(core->hAudioReadyEvent, 1000);
        if (waitResult != WAIT_OBJECT_0) {
            if (!core->recording) break;
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
// Build FFmpeg command line
// ============================================================================

static void BuildCaptureCommand(PhantomRecCore* core, const char* outputFile, int hasAudio, char* cmdLine, int cmdSize) {
    char captureInput[512];
    char captureFilter[256];
    
    GetCaptureInput(core, captureInput, sizeof(captureInput));
    GetCaptureFilter(core, captureFilter, sizeof(captureFilter));
    
    const char* rtbufsize = (core->captureMethod == 2) ? "500M" : "2000M";
    
    int offset = sprintf_s(cmdLine, cmdSize,
        "cmd.exe /c \"\"%s\" -y -hide_banner -loglevel error"
        " -rtbufsize %s"
        " -thread_queue_size 2048"
        "%s",
        core->maxsenginePath, rtbufsize, captureInput);
    
    if (hasAudio) {
        offset += sprintf_s(cmdLine + offset, cmdSize - offset,
            " -thread_queue_size 512 -f %s -ar %d -ac %d -i pipe:0",
            core->audioFormat, core->audioSampleRate, core->audioChannels);
    }
    
    offset += sprintf_s(cmdLine + offset, cmdSize - offset,
        "%s"
        " -max_muxing_queue_size 9096"
        " -c:v utvideo -pred median -threads 1"
        " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv",
        captureFilter);
    
    if (hasAudio) {
        offset += sprintf_s(cmdLine + offset, cmdSize - offset, " -c:a copy");
    }
    
    sprintf_s(cmdLine + offset, cmdSize - offset,
        " -shortest -fflags +genpts -f matroska \"%s\"\"",
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
    QueryPerformanceFrequency(&core->recFreq);
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
    
    int winVer = GetWindowsVersion();
    
    if (winVer == 7) {
        core->crf = 26; core->maxrate = 3000; core->bufsize = 6000;
        core->pipeBufferSizeMB = 2;
    } else if (winVer == 8) {
        core->crf = 26; core->maxrate = 4000; core->bufsize = 8000;
        core->pipeBufferSizeMB = 4; 
    } else if (cores <= 2) {
        core->crf = 23; core->maxrate = 4000; core->bufsize = 8000;
        core->pipeBufferSizeMB = 4;
    } else if (cores <= 4) {
        core->crf = 23; core->maxrate = 6000; core->bufsize = 12000;
        core->pipeBufferSizeMB = 8;
    } else if (cores <= 8) {
        core->crf = 23; core->maxrate = 8000; core->bufsize = 16000;
        core->pipeBufferSizeMB = 16;
    } else {
        core->crf = 23; core->maxrate = 12000; core->bufsize = 24000;
        core->pipeBufferSizeMB = 32;
    }
}

void Core_SetCaptureMethod(PhantomRecCore* core) {
    char captureInput[512];
    GetCaptureInput(core, captureInput, sizeof(captureInput));
}

void Core_WarmEngine(PhantomRecCore* core) {
    char captureInput[512];
    GetCaptureInput(core, captureInput, sizeof(captureInput));
    
    char warmupCmd[1024];
    if (core->captureMethod <= 1) {
        sprintf_s(warmupCmd, sizeof(warmupCmd),
            "cmd.exe /c \"\"%s\" -y -hide_banner -loglevel error %s -frames:v 1 -c:v utvideo -pred median -threads 1 -f null NUL\"",
            core->maxsenginePath, captureInput);
    } else {
        sprintf_s(warmupCmd, sizeof(warmupCmd),
            "cmd.exe /c \"\"%s\" -y -hide_banner -loglevel error -f gdigrab -framerate 1 -i desktop -frames:v 1 -c:v utvideo -pred median -threads 1 -f null NUL\"",
            core->maxsenginePath);
    }
    
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    
    if (CreateProcessA(NULL, warmupCmd, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
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

// ============================================================================
// Start / Stop Recording
// ============================================================================

int Core_StartRecording(PhantomRecCore* core) {
    if (core->recording || core->converting) return 0;
    
    // High performance power plan
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
    
    // Setup paths
    char ts[64];
    Core_Timestamp(ts, sizeof(ts));
    strncpy_s(core->segmentBaseName, sizeof(core->segmentBaseName), ts, _TRUNCATE);
    core->pauseSegmentCount = 0;
    core->segmentCount = 1;
    core->totalPausedDurationMs = 0;
    core->paused = 0;
    
    sprintf_s(core->tempFile, MAX_PATH, "%s\\%s_seg0_temp.mkv", core->outputDir, ts);
    sprintf_s(core->finalFile, MAX_PATH, "%s\\%s.mkv", core->outputDir, ts);
    strncpy_s(core->segmentFiles[0], MAX_PATH, core->tempFile, _TRUNCATE);
    
    // Initialize audio
    int wasapiReady = InitializeWASAPI(core);
    if (wasapiReady) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        CreatePipe(&core->hAudioPipeRead, &core->hAudioPipeWrite, &sa, core->pipeBufferSizeMB * 1024 * 1024);
        SetHandleInformation(core->hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
    }
    
    // Set audio format
    strncpy_s(core->audioFormat, sizeof(core->audioFormat), "s16le", _TRUNCATE);
    core->audioSampleRate = 48000;
    core->audioChannels = 2;
    if (wasapiReady && core->waveFormat) {
        strncpy_s(core->audioFormat, sizeof(core->audioFormat),
            (core->audioBitsPerSample == 32) ? "f32le" : "s16le", _TRUNCATE);
        core->audioSampleRate = core->waveFormat->nSamplesPerSec;
        core->audioChannels = core->waveFormat->nChannels;
    }
    
    // Build FFmpeg command
    core->recording = 1;
    char cmdLine[8196];
    BuildCaptureCommand(core, core->tempFile, wasapiReady, cmdLine, sizeof(cmdLine));


    // Launch audio thread (will block on hAudioStartEvent)
    if (wasapiReady) {
        if (core->hAudioStartEvent) CloseHandle(core->hAudioStartEvent);
        core->hAudioStartEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
        core->hAudioThread = (HANDLE)_beginthreadex(NULL, 0, AudioToPipeThread, core, 0, NULL);
    }
    
    // Launch FFmpeg
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (wasapiReady && core->hAudioPipeRead) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = core->hAudioPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_MINIMIZE;
    
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, &core->ffmpegProcess)) {
        core->recording = 0;
		
	    // Unblock the audio thread waiting on hAudioStartEvent
        if (core->hAudioStartEvent) {
            SetEvent(core->hAudioStartEvent);
            CloseHandle(core->hAudioStartEvent);
            core->hAudioStartEvent = NULL;
        }

        // Wake up the audio thread if it already passed the start event
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
	
    // FFmpeg started – release the audio thread so they begin together
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
    if (!core->recording) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    core->lastRecordingDurationMs = (int)((now.QuadPart - core->recStart.QuadPart) * 1000 / core->recFreq.QuadPart);
    if (core->lastRecordingDurationMs < 1000) core->lastRecordingDurationMs = 1000;

    core->recording = 0;               // Signal audio thread to exit
    core->paused = 0;                  // Clear pause flag (if any)

    // Restore power plan
    if (core->powerPlanChanged) {
        PowerSetActiveScheme(NULL, &core->originalPowerPlan);
        core->powerPlanChanged = 0;
    }
	
    // 1. Wake up the audio thread so it can see recording == 0 and exit
    if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
	
    // 2. Wait for the audio thread to actually finish
    if (core->hAudioThread) {
        WaitForSingleObject(core->hAudioThread, 5000);
        CloseHandle(core->hAudioThread);
        core->hAudioThread = NULL;
    }

    // 3. Now safe to close the pipe and clean up WASAPI
    if (core->hAudioPipeWrite) {
        CloseHandle(core->hAudioPipeWrite);
        core->hAudioPipeWrite = NULL;
    }
    CleanupWASAPI(core);

    // 4. Stop FFmpeg
    if (core->ffmpegProcess.hProcess) {
        SetConsoleCtrlHandler(NULL, TRUE);
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, core->ffmpegProcess.dwProcessId);
        DWORD wr = WaitForSingleObject(core->ffmpegProcess.hProcess, 6000);
        if (wr == WAIT_TIMEOUT) TerminateProcess(core->ffmpegProcess.hProcess, 0);
        SetConsoleCtrlHandler(NULL, FALSE);
        CloseHandle(core->ffmpegProcess.hProcess);
        CloseHandle(core->ffmpegProcess.hThread);
        memset(&core->ffmpegProcess, 0, sizeof(core->ffmpegProcess));
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    Sleep(1500);

    // ----- Segment list & Stage 2 (FIXED) -----
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

    if (core->convertAfterRecording && core->lastRecordingDurationMs >= 1000) {
        core->converting = 1;
        core->convertProgress = 0;

        if (core->onStatusUpdate) core->onStatusUpdate("Processing video...");
        if (core->onButtonUpdate) core->onButtonUpdate("Processing...");

        char cmdLine[8196];
        
        sprintf_s(cmdLine, sizeof(cmdLine),
            "cmd.exe /c \"\"%s\" -y -progress pipe:1 -loglevel error -f concat -safe 0 -i \"%s\" "
            "-c:v libx264 -preset ultrafast -crf %d -c:a aac -b:a 128k "
            "-pix_fmt yuv420p -r 60 -fps_mode cfr \"%s\"\"",
            core->maxsenginePath, segmentsTxt, core->crf, core->finalFile);

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

            DeleteFileA(segmentsTxt);
            for (int i = 0; i < core->segmentCount; i++) {
                if (Core_FileExists(core->segmentFiles[i])) DeleteFileA(core->segmentFiles[i]);
            }

            core->converting = 0;
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
    }
}

void Core_TogglePause(PhantomRecCore* core) {
    if (!core->recording || core->converting) return;
    
    core->paused = !core->paused;
    
    if (core->paused) {
        // PAUSE — Stop FFmpeg and audio, same as v1.5
        QueryPerformanceCounter(&core->pauseTime);
        
        // Stop audio thread
        if (core->hAudioReadyEvent) SetEvent(core->hAudioReadyEvent);
		
        // 2. Wait for it to finish
        if (core->hAudioThread) {
            WaitForSingleObject(core->hAudioThread, 5000);
            CloseHandle(core->hAudioThread);
            core->hAudioThread = NULL;
        }
		
        if (core->hAudioPipeWrite) { CloseHandle(core->hAudioPipeWrite); core->hAudioPipeWrite = NULL; }
        CleanupWASAPI(core);
        
        // Stop FFmpeg
        if (core->ffmpegProcess.hProcess) {
            SetConsoleCtrlHandler(NULL, TRUE);
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, core->ffmpegProcess.dwProcessId);
            DWORD wr = WaitForSingleObject(core->ffmpegProcess.hProcess, 6000);
            if (wr == WAIT_TIMEOUT) TerminateProcess(core->ffmpegProcess.hProcess, 0);
            SetConsoleCtrlHandler(NULL, FALSE);
            CloseHandle(core->ffmpegProcess.hProcess);
            CloseHandle(core->ffmpegProcess.hThread);
            memset(&core->ffmpegProcess, 0, sizeof(core->ffmpegProcess));
        }
        
        if (core->onStatusUpdate) core->onStatusUpdate("PAUSED");
        if (core->onButtonUpdate) core->onButtonUpdate("RESUME");
        
    } else {
        // RESUME — Calculate pause duration, create new segment
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        core->totalPausedDurationMs += (long long)((now.QuadPart - core->pauseTime.QuadPart) * 1000 / core->recFreq.QuadPart);
        
        core->pauseSegmentCount++;
        sprintf_s(core->segmentFiles[core->segmentCount], MAX_PATH,
            "%s\\%s_seg%d_temp.mkv",
            core->outputDir, core->segmentBaseName, core->pauseSegmentCount);
        core->segmentCount++;
        
        // Initialize audio for new segment
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
        
        // Build and launch new FFmpeg for new segment
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
        si.wShowWindow = SW_MINIMIZE;
        
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
        NULL, NULL, &si, &core->ffmpegProcess)) {
        // Unblock the audio thread waiting on hAudioStartEvent
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
        return;
    }
    
    // FFmpeg started – release the audio thread so they begin together
    if (core->hAudioStartEvent) {
        SetEvent(core->hAudioStartEvent);
        CloseHandle(core->hAudioStartEvent);
        core->hAudioStartEvent = NULL;
    }

    if (core->hAudioPipeRead) {
        CloseHandle(core->hAudioPipeRead);
        core->hAudioPipeRead = NULL;
    }
        
        if (core->onStatusUpdate) core->onStatusUpdate("Recording...");
        if (core->onButtonUpdate) core->onButtonUpdate("STOP");
    }
}

// ============================================================================
// Status queries
// ============================================================================

int Core_IsRecording(const PhantomRecCore* core) { return core->recording; }
int Core_IsPaused(const PhantomRecCore* core) { return core->paused; }
int Core_IsConverting(const PhantomRecCore* core) { return core->converting; }
int Core_GetProgress(const PhantomRecCore* core) { return core->convertProgress; }
