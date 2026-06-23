// RetroRec v1.4 — Universal Ghost Screen Recorder
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// GFX Live Capture | x264 Post-Convert | WASAPI Audio | Zero Drops | Auto-CRF Matrix
// v1.4: Persistent warm FFmpeg + Async conversion + Pause/Resume + Audio‑first startup

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <atomic>
#include <mutex>
#include <ctime>
#include <sstream>
#include <shlobj.h>
#include <chrono>
#include <vector>
#include <thread>
#include <fstream>
#include <cstdint>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <commctrl.h>
#include <avrt.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

#define RETROREC_VERSION "1.4.0"
#define ID_BTN_RECORD 1001
#define ID_BTN_SETTINGS 1002
#define ID_HOTKEY_RECORD 1
#define ID_HOTKEY_PAUSE 2
#define ID_TIMER_UPDATE 2001
#define ID_TIMER_INI_CHECK 2002
#define WM_CONVERSION_PROGRESS (WM_APP + 1)
#define WM_CONVERSION_DONE     (WM_APP + 2)
#define WM_INI_CHANGED         (WM_APP + 3)



struct RetroRec {
    bool convertAfterRecording = true;
    std::atomic<bool> recording{false};
    std::atomic<bool> paused{false};
	std::chrono::steady_clock::time_point pauseTime;
	std::chrono::milliseconds totalPausedDuration{0};
	std::vector<std::string> segmentFiles;
	int pauseSegmentCount = 0;
	std::string segmentBaseName;
    std::atomic<bool> converting{false};
    std::atomic<int> convertProgress{0};
    int bufsize = 8000;
    HWND hwnd = nullptr;
    HWND btnRecord = nullptr;
    UINT recordHotkey = VK_F10;
    UINT pauseHotkey = 'P';
    HWND lblStatus = nullptr;
    HWND progressBar = nullptr;
    PROCESS_INFORMATION ffmpegProcess = {0};
    std::string finalFile;
    std::string tempFile;
    std::string ffmpegPath;
    std::string outputDir;
    std::string iniPath;
    int pipeBufferSizeMB = 8;
    int screenWidth = 1920;
    int screenHeight = 1080;
    int cpuCoreCount = 4;
    int dynamicThreads = 1;
    int crf = 22;
    int maxrate = 4000;
    int videoQueueSize = 512;
    std::mutex mtx;
    std::chrono::steady_clock::time_point recStart;
    int sessions = 0;
    long long totalBytes = 0;
    int lastRecordingDurationMs = 0;
    int convertPreset = 0;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    std::thread audioThread;
    bool audioActive = false;
    int audioBitsPerSample = 16;
    HANDLE hAudioPipeWrite = nullptr;
    HANDLE hAudioPipeRead = nullptr;
    HANDLE hAudioSyncEvent = nullptr;
    HANDLE hAudioReadyEvent = nullptr;
    FILETIME iniLastWrite = {0};
} app;

// ── Function declarations ─────────────────────────────────
bool InitializeWASAPI();
void CleanupWASAPI();
void AudioToPipeLoop();
void UpdateUI();
void SetStatus(const std::string& t);
void SetButton(const std::string& t);
void LoadConfiguration();
void CleanupOrphanedTempFiles();
void CreateDefaultIni();
void ReloadIniIfChanged();
void SpawnWarmFFmpeg();

// ── Utility functions ─────────────────────────────────────
std::string GetExeDir() {
    char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : ".";
}

bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

long long GetFileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) {
        LARGE_INTEGER s; s.HighPart = d.nFileSizeHigh; s.LowPart = d.nFileSizeLow;
        return s.QuadPart;
    }
    return -1;
}

std::string FormatSize(long long b) {
    if (b < 0) return "Unknown";
    if (b < 1024) return std::to_string(b) + " B";
    double kb = b / 1024.0;
    if (kb < 1048576) { char x[32]; sprintf_s(x, "%.1f KB", kb); return x; }
    double mb = kb / 1024.0;
    if (mb < 1024) { char x[32]; sprintf_s(x, "%.1f MB", mb); return x; }
    char x[32]; sprintf_s(x, "%.2f GB", mb / 1024.0); return x;
}

std::string FormatTime(int s) {
    char b[32]; sprintf_s(b, "%02d:%02d", s/60, s%60); return b;
}

std::string Timestamp() {
    time_t n = time(nullptr); tm t;
    localtime_s(&t, &n);
    char b[64]; strftime(b, sizeof(b), "%Y%m%d_%H%M%S", &t);
    return "RetroRec_" + std::string(b);
}

std::string GetVideosFolder() {
    char p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, p))) {
        std::string d = std::string(p) + "\\RetroRec";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }
    return GetExeDir();
}

bool FindFFmpeg() {
    std::string local = GetExeDir() + "\\ffmpeg.exe";
    if (FileExists(local)) { app.ffmpegPath = local; return true; }
    char b[MAX_PATH];
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, b, nullptr) > 0) {
        app.ffmpegPath = b;
        return true;
    }
    return false;
}

void PlayNotificationSound() {
    // Play the Windows "Notification" system sound
    // This tells the user: "RetroRec is ready, not broken"
    MessageBeep(MB_ICONINFORMATION);
    // Alternative: Play Windows default beep
    // Beep(800, 200);
}

// ── INI file helpers ──────────────────────────────────────
void CreateDefaultIni() {
    std::ofstream ini(app.iniPath);
    ini << "; ========================================\r\n"
        << "; RetroRec v1.4 Configuration\r\n"
        << "; Made by MaxRBLX1\r\n"
        << "; ========================================\r\n"
        << "; Hotkey: F1-F12 for function keys\r\n"
        << ";         A-Z for Ctrl+Letter hotkeys\r\n"
        << "; PauseHotkey: Same format as Hotkey\r\n"
        << "; ConvertAfterRecording: yes or no\r\n"
        << "; ConvertPreset: fast or medium\r\n"
        << "; ========================================\r\n\r\n"
        << "[Settings]\r\n"
        << "Hotkey=F10\r\n"
        << "PauseHotkey=P\r\n"
        << "ConvertAfterRecording=yes\r\n"
        << "ConvertPreset=fast\r\n";
    ini.close();
}

void CleanupOrphanedTempFiles() {
    std::string searchPath = app.outputDir + "\\*_temp.mkv";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string fullPath = app.outputDir + "\\" + findData.cFileName;
            DeleteFileA(fullPath.c_str());
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
}

UINT ParseHotkey(const std::string& key) {
    if (key == "F1")  return VK_F1;
    if (key == "F2")  return VK_F2;
    if (key == "F3")  return VK_F3;
    if (key == "F4")  return VK_F4;
    if (key == "F5")  return VK_F5;
    if (key == "F6")  return VK_F6;
    if (key == "F7")  return VK_F7;
    if (key == "F8")  return VK_F8;
    if (key == "F9")  return VK_F9;
    if (key == "F10") return VK_F10;
    if (key == "F11") return VK_F11;
    if (key == "F12") return VK_F12;
    if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') return (UINT)key[0];
    if (key.length() == 1 && key[0] >= 'a' && key[0] <= 'z') return (UINT)(key[0] - 32);
    return 0;
}

UINT GetHotkeyModifiers(UINT vk) {
    return (vk >= VK_F1 && vk <= VK_F12) ? (MOD_NOREPEAT) : (MOD_CONTROL | MOD_NOREPEAT);
}

void LoadConfiguration() {
    char buf[32];

    GetPrivateProfileStringA("Settings", "Hotkey", "F10", buf, sizeof(buf), app.iniPath.c_str());
    app.recordHotkey = ParseHotkey(buf);
    if (app.recordHotkey == 0) app.recordHotkey = VK_F10;

    GetPrivateProfileStringA("Settings", "PauseHotkey", "P", buf, sizeof(buf), app.iniPath.c_str());
    app.pauseHotkey = ParseHotkey(buf);
    if (app.pauseHotkey == 0) app.pauseHotkey = 'P';

    GetPrivateProfileStringA("Settings", "ConvertAfterRecording", "yes", buf, sizeof(buf), app.iniPath.c_str());
    app.convertAfterRecording = (strcmp(buf, "no") != 0);

    GetPrivateProfileStringA("Settings", "ConvertPreset", "fast", buf, sizeof(buf), app.iniPath.c_str());
    app.convertPreset = (strcmp(buf, "medium") == 0) ? 1 : 0;

    HANDLE hFile = CreateFileA(app.iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        GetFileTime(hFile, nullptr, nullptr, &app.iniLastWrite);
        CloseHandle(hFile);
    }
}

void ReloadIniIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExA(app.iniPath.c_str(), GetFileExInfoStandard, &attr)) {
        if (CompareFileTime(&attr.ftLastWriteTime, &app.iniLastWrite) != 0) {
            app.iniLastWrite = attr.ftLastWriteTime;
            LoadConfiguration();

            UnregisterHotKey(app.hwnd, ID_HOTKEY_RECORD);
            UnregisterHotKey(app.hwnd, ID_HOTKEY_PAUSE);

            UINT recMod = GetHotkeyModifiers(app.recordHotkey);
            RegisterHotKey(app.hwnd, ID_HOTKEY_RECORD, recMod, app.recordHotkey);

            UINT pauseMod = GetHotkeyModifiers(app.pauseHotkey);
            RegisterHotKey(app.hwnd, ID_HOTKEY_PAUSE, pauseMod, app.pauseHotkey);

            UpdateUI();
        }
    }
}

// ── Configuration ─────────────────────────────────────────
void DetectCurrentResolution() {
    app.screenWidth = GetSystemMetrics(SM_CXSCREEN);
    app.screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void ConfigureUniversalPipeline() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    int cores = si.dwNumberOfProcessors;
    app.cpuCoreCount = cores;
    if (cores <= 2) {
        app.crf = 22; app.maxrate = 4000; app.bufsize = 8000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 4; app.videoQueueSize = 512;
    } else if (cores <= 4) {
        app.crf = 22; app.maxrate = 6000; app.bufsize = 12000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 8; app.videoQueueSize = 512;
    } else if (cores <= 8) {
        app.crf = 22; app.maxrate = 8000; app.bufsize = 16000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 16; app.videoQueueSize = 512;
    } else {
        app.crf = 22; app.maxrate = 12000; app.bufsize = 24000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 32; app.videoQueueSize = 512;
    }
}

// ── UI helpers ────────────────────────────────────────────
void SetStatus(const std::string& t) {
    if (app.lblStatus && IsWindow(app.lblStatus)) SetWindowTextA(app.lblStatus, t.c_str());
}
void SetButton(const std::string& t) {
    if (app.btnRecord && IsWindow(app.btnRecord)) SetWindowTextA(app.btnRecord, t.c_str());
}

std::string GetHotkeyName(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z') return std::string("Ctrl+") + (char)vk;
    return "F10";
}

void UpdateUI() {
    std::string hotkey = GetHotkeyName(app.recordHotkey);
    std::string pauseKey = GetHotkeyName(app.pauseHotkey);

    if (app.converting) {
        SetButton("⏳ Processing...");
        std::string s = "⏳ Processing video...\r\n";
        s += "Please wait — UI is responsive\r\n\r\n";
        s += "Progress: " + std::to_string(app.convertProgress.load()) + "%";
        SetStatus(s);
        SendMessage(app.progressBar, PBM_SETPOS, app.convertProgress.load(), 0);
    } else if (app.recording && app.paused) {
        SetButton("▶ RESUME (" + pauseKey + ")");
        SetStatus("⏸ PAUSED\r\nGFX + MPEG-4 Capture -> x264 Post-Convert");
        SendMessage(app.progressBar, PBM_SETMARQUEE, 0, 0);
    } else if (app.recording) {
        auto e = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - app.recStart).count();
        SetButton("■ STOP (" + hotkey + ")");
        std::string status = "● REC " + FormatTime((int)e);
        if (app.audioActive) status += "\r\n[AUDIO] Pipe Active";
        status += "\r\nGFX + MPEG-4 Capture -> x264 Post-Convert";
        SetStatus(status);
        SendMessage(app.progressBar, PBM_SETMARQUEE, 1, 0);
    } else {
        SetButton("▶ START (" + hotkey + ")");
        std::string s = "✓ Ready — " + hotkey + " to record\r\n";
        s += "Pause: " + pauseKey + "\r\n\r\n";
        s += "Cores: " + std::to_string(app.cpuCoreCount);
        s += " | Threads: " + std::to_string(app.dynamicThreads);
        s += "\r\nTarget CRF: " + std::to_string(app.crf);
        s += " | Bitrate: " + std::to_string(app.maxrate) + "k";
        s += "\r\n" + std::to_string(app.screenWidth) + "x" + std::to_string(app.screenHeight);
        s += "\r\nGFX Capture -> x264 Pipeline | Live WASAPI Pipe";
        s += "\r\nBuilt by MaxRBLX1";
        if (app.sessions > 0) s += "\r\nSessions: " + std::to_string(app.sessions);
        SetStatus(s);
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
    }
}

// ── WASAPI Audio Capture (Microsoft event‑driven pattern) ──
bool InitializeWASAPI() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE) return false;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) { CoUninitialize(); return false; }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { enumerator->Release(); CoUninitialize(); return false; }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&app.audioClient);
    device->Release(); enumerator->Release();
    if (FAILED(hr)) { CoUninitialize(); return false; }

    hr = app.audioClient->GetMixFormat(&app.waveFormat);
    if (FAILED(hr)) { app.audioClient->Release(); app.audioClient = nullptr; CoUninitialize(); return false; }

    app.hAudioReadyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!app.hAudioReadyEvent) {
        app.audioClient->Release(); app.audioClient = nullptr;
        CoUninitialize(); return false;
    }

    hr = app.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        0, 0, app.waveFormat, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(app.waveFormat); app.waveFormat = nullptr;
        app.audioClient->Release(); app.audioClient = nullptr;
        CloseHandle(app.hAudioReadyEvent); app.hAudioReadyEvent = nullptr;
        CoUninitialize(); return false;
    }

    hr = app.audioClient->SetEventHandle(app.hAudioReadyEvent);
    if (FAILED(hr)) {
        CoTaskMemFree(app.waveFormat); app.waveFormat = nullptr;
        app.audioClient->Release(); app.audioClient = nullptr;
        CloseHandle(app.hAudioReadyEvent); app.hAudioReadyEvent = nullptr;
        CoUninitialize(); return false;
    }

    hr = app.audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&app.captureClient);
    if (FAILED(hr)) {
        CoTaskMemFree(app.waveFormat); app.waveFormat = nullptr;
        app.audioClient->Release(); app.audioClient = nullptr;
        CloseHandle(app.hAudioReadyEvent); app.hAudioReadyEvent = nullptr;
        CoUninitialize(); return false;
    }

    if (app.waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ex = (WAVEFORMATEXTENSIBLE*)app.waveFormat;
        app.audioBitsPerSample = (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 32 : 16;
    } else if (app.waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        app.audioBitsPerSample = 32;
    }

    app.audioClient->Start();
    app.audioActive = true;
    return true;
}

void AudioToPipeLoop() {
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    UINT32 packetLength = 0;
    bool firstPacketSent = false;

    while (app.recording) {
        while (app.paused && app.recording) { Sleep(100); }
        if (!app.recording) break;

        DWORD waitResult = WaitForSingleObject(app.hAudioReadyEvent, 1000);
        if (waitResult != WAIT_OBJECT_0) {
            if (!app.recording) break;
            continue;
        }

        HRESULT hr = app.captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength > 0) {
            BYTE* data; UINT32 frames; DWORD flags;
            UINT64 devicePosition = 0, qpcTimestamp = 0;
            hr = app.captureClient->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcTimestamp);
            if (SUCCEEDED(hr)) {
                size_t size = frames * app.waveFormat->nBlockAlign;
                BYTE* writeData = data;
                std::vector<BYTE> silence;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(size, 0);
                    writeData = silence.data();
                }
                DWORD written = 0;
                WriteFile(app.hAudioPipeWrite, writeData, (DWORD)size, &written, nullptr);
                app.captureClient->ReleaseBuffer(frames);
                if (!firstPacketSent && written > 0) {
                    if (app.hAudioSyncEvent) SetEvent(app.hAudioSyncEvent);
                    firstPacketSent = true;
                }
            }
            hr = app.captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }
    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
}

void CleanupWASAPI() {
    if (app.audioActive) {
        app.audioClient->Stop();
        if (app.captureClient) { app.captureClient->Release(); app.captureClient = nullptr; }
        if (app.audioClient) { app.audioClient->Release(); app.audioClient = nullptr; }
        if (app.waveFormat) { CoTaskMemFree(app.waveFormat); app.waveFormat = nullptr; }
        if (app.hAudioReadyEvent) { CloseHandle(app.hAudioReadyEvent); app.hAudioReadyEvent = nullptr; }
        app.audioActive = false;
        CoUninitialize();
    }
}


void SpawnWarmFFmpeg() {
    // Capture 1 frame, encode with mpeg4, discard output, exit
    // This fully initializes D3D11 + gfxcapture + encoder in the GPU driver cache
    // The real FFmpeg on F10 will use the cached resources — instant start
    
    std::string warmupCmd = 
        "cmd.exe /c \"\"" + app.ffmpegPath + "\" -y -hide_banner -loglevel error "
        "-f lavfi -i gfxcapture=monitor_idx=0 "
        "-frames:v 1 -c:v mpeg4 -q:v 3 -f null NUL\"";
    
    STARTUPINFOA siWarm = { sizeof(siWarm) };
    siWarm.dwFlags = STARTF_USESHOWWINDOW;
    siWarm.wShowWindow = SW_HIDE;
    
    PROCESS_INFORMATION piWarm = {0};
    std::vector<char> warmupCl(warmupCmd.begin(), warmupCmd.end());
    warmupCl.push_back('\0');
    
    if (CreateProcessA(nullptr, warmupCl.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &siWarm, &piWarm)) {
        // Wait for it to finish (captures 1 frame, encodes, exits)
        WaitForSingleObject(piWarm.hProcess, 3000);
        CloseHandle(piWarm.hProcess);
        CloseHandle(piWarm.hThread);
    }
}

// ── Pause/Resume — Full NtSuspendProcess/NtResumeProcess ──
void TogglePause() {
    if (!app.recording || app.converting) return;
    
    app.paused = !app.paused;

    if (app.paused) {
        // ── PAUSE ──
        app.pauseTime = std::chrono::steady_clock::now();
        
        // Stop audio thread
        app.recording = false;
        if (app.hAudioReadyEvent) SetEvent(app.hAudioReadyEvent);
        if (app.audioThread.joinable()) app.audioThread.join();
        if (app.hAudioPipeWrite) { CloseHandle(app.hAudioPipeWrite); app.hAudioPipeWrite = nullptr; }
        CleanupWASAPI();
        
        // Stop FFmpeg gracefully — finalizes current segment
        if (app.ffmpegProcess.hProcess) {
            SetConsoleCtrlHandler(nullptr, TRUE);
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, app.ffmpegProcess.dwProcessId);
            DWORD wr = WaitForSingleObject(app.ffmpegProcess.hProcess, 6000);
            if (wr == WAIT_TIMEOUT) TerminateProcess(app.ffmpegProcess.hProcess, 0);
            SetConsoleCtrlHandler(nullptr, FALSE);
            CloseHandle(app.ffmpegProcess.hProcess);
            CloseHandle(app.ffmpegProcess.hThread);
            app.ffmpegProcess.hProcess = nullptr;
            app.ffmpegProcess.hThread = nullptr;
        }
        
        app.recording = true;  // Still in recording session, just paused
        UpdateUI();
        
    } else {
        // ── RESUME ──
        auto now = std::chrono::steady_clock::now();
        auto pauseDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - app.pauseTime);
        app.totalPausedDuration += pauseDuration;
        
        // Create new segment filename
        app.pauseSegmentCount++;
        std::string segFile = app.outputDir + "\\" + app.segmentBaseName + "_seg" + 
                              std::to_string(app.pauseSegmentCount) + "_temp.mkv";
        app.segmentFiles.push_back(segFile);
        
        // Re-initialize WASAPI
        bool wasapiReady = InitializeWASAPI();
        if (wasapiReady) {
            SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
            CreatePipe(&app.hAudioPipeRead, &app.hAudioPipeWrite, &sa, app.pipeBufferSizeMB * 1024 * 1024);
            SetHandleInformation(app.hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
        }
        
        std::string audioFormat = "s16le";
        int sampleRate = 48000, channels = 2;
        if (wasapiReady && app.waveFormat) {
            audioFormat = (app.audioBitsPerSample == 32) ? "f32le" : "s16le";
            sampleRate = app.waveFormat->nSamplesPerSec;
            channels = app.waveFormat->nChannels;
        }
        
        // Build FFmpeg command
        std::ostringstream c;
        c << "cmd.exe /c \""
          << "\"" << app.ffmpegPath << "\" -y -hide_banner -loglevel error -rtbufsize 2000M"
          << " -thread_queue_size " << app.videoQueueSize
          << " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1";
        if (wasapiReady) {
            c << " -thread_queue_size 512"
              << " -f " << audioFormat << " -ar " << sampleRate << " -ac " << channels << " -i pipe:0";
        }
        c << " -max_muxing_queue_size 2048 -thread_queue_size 2048 -fps_mode vfr"
          << " -vf \"hwdownload,format=bgra,mpdecimate,format=yuv420p\""
          << " -c:v mpeg4 -q:v 3 -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
        if (wasapiReady) c << " -c:a copy";
        c << " -shortest -fflags +genpts -threads " << app.dynamicThreads
          << " -f matroska \"" << segFile << "\""
          << "\"";
        
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        if (wasapiReady && app.hAudioPipeRead != nullptr) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = app.hAudioPipeRead;
        }
        si.wShowWindow = SW_HIDE;
        
        std::string cmdStr = c.str();
        std::vector<char> cl(cmdStr.begin(), cmdStr.end());
        cl.push_back('\0');
        
        // Start audio and FFmpeg at the EXACT SAME TIME — no drift
        if (wasapiReady) {
            app.audioThread = std::thread(AudioToPipeLoop);
        }
        
        CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
            CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &si, &app.ffmpegProcess);
        
        if (app.hAudioPipeRead) { CloseHandle(app.hAudioPipeRead); app.hAudioPipeRead = nullptr; }
        
        UpdateUI();
    }
}

// ── Recording engine (audio‑first, stable) ────────────────
void StartRecording() {
    std::lock_guard<std::mutex> l(app.mtx);
    if (app.recording || app.converting) return;
    if (app.ffmpegPath.empty() && !FindFFmpeg()) {
        MessageBoxA(app.hwnd, "ffmpeg.exe not found!", "RetroRec", MB_OK);
        return;
    }
    ReloadIniIfChanged();
    DetectCurrentResolution();
    ConfigureUniversalPipeline();

    std::string ts = Timestamp();
    app.segmentBaseName = ts;
    app.pauseSegmentCount = 0;
    app.segmentFiles.clear();
    app.totalPausedDuration = std::chrono::milliseconds(0);
    app.paused = false;
    
    // First segment
    app.tempFile = app.outputDir + "\\" + ts + "_seg0_temp.mkv";
    app.segmentFiles.push_back(app.tempFile);
    app.finalFile = app.outputDir + "\\" + ts + ".mkv";

    bool wasapiReady = InitializeWASAPI();
    if (wasapiReady) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        CreatePipe(&app.hAudioPipeRead, &app.hAudioPipeWrite, &sa, app.pipeBufferSizeMB * 1024 * 1024);
        SetHandleInformation(app.hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
    }
    std::string audioFormat = "s16le";
    int sampleRate = 48000, channels = 2;
    if (wasapiReady && app.waveFormat) {
        audioFormat = (app.audioBitsPerSample == 32) ? "f32le" : "s16le";
        sampleRate = app.waveFormat->nSamplesPerSec;
        channels = app.waveFormat->nChannels;
    }

    app.recording = true;

    // Build FFmpeg command FIRST
    std::ostringstream c;
    c << "cmd.exe /c \""
      << "\"" << app.ffmpegPath << "\" -y -hide_banner -loglevel error -rtbufsize 2000M"
      << " -thread_queue_size " << app.videoQueueSize
      << " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1";
    if (wasapiReady) {
        c << " -thread_queue_size 512"
          << " -f " << audioFormat << " -ar " << sampleRate << " -ac " << channels << " -i pipe:0";
    }
    c << " -max_muxing_queue_size 2048 -thread_queue_size 2048 -fps_mode vfr"
      << " -vf \"hwdownload,format=bgra,mpdecimate,format=yuv420p\""
      << " -c:v mpeg4 -q:v 3 -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    if (wasapiReady) c << " -c:a copy";
    c << " -shortest -fflags +genpts"
      << " -threads " << app.dynamicThreads
      << " -f matroska \"" << app.tempFile << "\""
      << "\"";

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (wasapiReady && app.hAudioPipeRead != nullptr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = app.hAudioPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_HIDE;

    std::string cmdStr = c.str();
    std::vector<char> cl(cmdStr.begin(), cmdStr.end());
    cl.push_back('\0');

    // Start audio and FFmpeg at the EXACT SAME TIME
    if (wasapiReady) {
        app.audioThread = std::thread(AudioToPipeLoop);
    }

    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
        CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
        nullptr, nullptr, &si, &app.ffmpegProcess)) {
        app.recording = false;
        if (wasapiReady) {
            if (app.audioThread.joinable()) app.audioThread.join();
            CleanupWASAPI();
        }
        SetStatus("✗ Failed"); return;
    }

    if (app.hAudioPipeRead) { CloseHandle(app.hAudioPipeRead); app.hAudioPipeRead = nullptr; }

    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    app.recStart = std::chrono::steady_clock::now();
    app.sessions++;
    SetTimer(app.hwnd, ID_TIMER_UPDATE, 1000, nullptr);
    UpdateUI();
}

void StopRecording() {
    std::lock_guard<std::mutex> l(app.mtx);
	if (!app.recording || app.paused) return;

    KillTimer(app.hwnd, ID_TIMER_UPDATE);
    SendMessage(app.progressBar, PBM_SETMARQUEE, 0, 0);
    SetStatus("⏳ Finalizing...");

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - app.recStart).count();
    app.lastRecordingDurationMs = (int)elapsed;

    app.recording = false;

    // If paused, audio/FFmpeg already stopped by TogglePause()
    if (app.paused) {
        app.paused = false;
    } else {
        if (app.hAudioReadyEvent) SetEvent(app.hAudioReadyEvent);
        if (app.audioThread.joinable()) app.audioThread.join();
        if (app.hAudioPipeWrite) { CloseHandle(app.hAudioPipeWrite); app.hAudioPipeWrite = nullptr; }
        CleanupWASAPI();

        if (app.ffmpegProcess.hProcess) {
            SetConsoleCtrlHandler(nullptr, TRUE);
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, app.ffmpegProcess.dwProcessId);
            DWORD wr = WaitForSingleObject(app.ffmpegProcess.hProcess, 6000);
            if (wr == WAIT_TIMEOUT) TerminateProcess(app.ffmpegProcess.hProcess, 0);
            SetConsoleCtrlHandler(nullptr, FALSE);
            CloseHandle(app.ffmpegProcess.hProcess);
            CloseHandle(app.ffmpegProcess.hThread);
            app.ffmpegProcess.hProcess = nullptr;
            app.ffmpegProcess.hThread = nullptr;
        }
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    Sleep(500);

    // Build segments.txt
    std::string segmentsTxt = app.outputDir + "\\segments.txt";
    std::ofstream segFile(segmentsTxt);
    int validSegments = 0;
    for (const auto& seg : app.segmentFiles) {
        if (FileExists(seg) && GetFileSize(seg) > 2048) {
            segFile << "file '" << seg << "'\r\n";
            validSegments++;
        }
    }
    segFile.close();

    // No valid segments — nothing to convert
    if (validSegments == 0) {
        DeleteFileA(segmentsTxt.c_str());
        SetStatus("✗ No recording data found");
        UpdateUI();
        return;
    }

    // ── CONVERSION BUILT INLINE ──
    if (app.convertAfterRecording) {
        app.converting = true;
        SetStatus("⏳ Processing video...");
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
	
        // Tell user the GUI will freeze during conversion
        MessageBoxA(app.hwnd, 
            "Conversion started!\n\n"
            "The RetroRec window will freeze during processing.\n"
            "This is normal — your recording is being compressed.\n\n"
            "Please wait...",
            "RetroRec — Processing",
            MB_OK | MB_ICONINFORMATION);

        SYSTEM_INFO siCpu; GetSystemInfo(&siCpu);
        int threads = siCpu.dwNumberOfProcessors;
        if (threads < 2) threads = 2;

        // Build conversion command
        char cmdLine[4096];
        sprintf_s(cmdLine, sizeof(cmdLine),
            "cmd.exe /c \"\"%s\" -y -progress pipe:1 -loglevel error -f concat -safe 0 -i \"%s\" -fps_mode cfr -r 60 -c:v libx264 -preset ultrafast -tune zerolatency -crf %d -maxrate %dk -bufsize %dk -c:a aac -b:a 128k -pix_fmt yuv420p -threads %d \"%s\"\"",
            app.ffmpegPath.c_str(), segmentsTxt.c_str(), app.crf, app.maxrate, app.bufsize, threads, app.finalFile.c_str());

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
        std::vector<char> cl(cmdLine, cmdLine + strlen(cmdLine) + 1);
		
		SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

        if (CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &siConv, &convertPI)) {

            CloseHandle(hWrite);
            char buf[512];
            std::string lineBuffer;
            DWORD bytesRead;
            long long totalDurationUs = (long long)app.lastRecordingDurationMs * 1000;

            while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buf[bytesRead] = '\0';
                lineBuffer += buf;
                size_t pos;
                while ((pos = lineBuffer.find('\n')) != std::string::npos) {
                    std::string oneLine = lineBuffer.substr(0, pos);
                    lineBuffer = lineBuffer.substr(pos + 1);
                    if (!oneLine.empty() && oneLine.back() == '\r') oneLine.pop_back();
                    if (oneLine.find("out_time_ms=") == 0) {
                        try {
                            long long timeUs = std::stoll(oneLine.substr(12));
                            int percent = (totalDurationUs > 0) ? (int)((timeUs * 100) / totalDurationUs) : 0;
                            if (percent > 100) percent = 100;
                            app.convertProgress = percent;
                            SendMessage(app.progressBar, PBM_SETPOS, percent, 0);
                            SetStatus("⏳ Processing video... " + std::to_string(percent) + "%");
                        } catch (...) {}
                    }
                }
            }
            CloseHandle(hRead);
            WaitForSingleObject(convertPI.hProcess, INFINITE);
            CloseHandle(convertPI.hProcess);
            CloseHandle(convertPI.hThread);
			
			SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

            // Clean up
            DeleteFileA(segmentsTxt.c_str());
            for (const auto& seg : app.segmentFiles) {
                if (FileExists(seg)) DeleteFileA(seg.c_str());
            }
            app.segmentFiles.clear();

            app.converting = false;
            app.convertProgress = 0;
            SendMessage(app.progressBar, PBM_SETPOS, 0, 0);

            long long fs = GetFileSize(app.finalFile);
            if (fs > 2048) {
                app.totalBytes += fs;
                SetStatus("✓ " + FormatSize(fs));
                ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + app.finalFile + "\"").c_str(), nullptr, SW_SHOWNORMAL);
            } else {
                SetStatus("✗ Conversion failed");
            }
            UpdateUI();
            return;
        } else {
            CloseHandle(hRead);
            CloseHandle(hWrite);
        }
    }

    // Fallback: no conversion
    app.converting = false;
    app.convertProgress = 0;
    SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
    
    long long fs = GetFileSize(app.finalFile);
    if (fs > 2048) {
        app.totalBytes += fs;
        SetStatus("✓ " + FormatSize(fs));
        ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + app.finalFile + "\"").c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        SetStatus("✗ File Error: Corrupted or Empty");
    }
    UpdateUI();
}

// ── Window procedure ──────────────────────────────────────
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        std::string startLabel = "▶ START (" + GetHotkeyName(app.recordHotkey) + ")";
        app.btnRecord = CreateWindowA("BUTTON", startLabel.c_str(),
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, 20, 340, 50, h, (HMENU)ID_BTN_RECORD, nullptr, nullptr);
        CreateWindowA("BUTTON", "Settings",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            20, 80, 160, 30, h, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
        app.lblStatus = CreateWindowA("STATIC", "",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            20, 120, 340, 130, h, nullptr, nullptr, nullptr);
        app.progressBar = CreateWindowA("msctls_progress32", "",
            WS_VISIBLE | WS_CHILD | PBS_MARQUEE,
            20, 260, 340, 15, h, nullptr, nullptr, nullptr);
        UINT recMod = GetHotkeyModifiers(app.recordHotkey);
        RegisterHotKey(h, ID_HOTKEY_RECORD, recMod, app.recordHotkey);
        UINT pauseMod = GetHotkeyModifiers(app.pauseHotkey);
        RegisterHotKey(h, ID_HOTKEY_PAUSE, pauseMod, app.pauseHotkey);
        SetTimer(h, ID_TIMER_INI_CHECK, 2000, nullptr);
        SetButton("▶ START (" + GetHotkeyName(app.recordHotkey) + ")");
        UpdateUI();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_RECORD) {
            if (app.recording) StopRecording(); else StartRecording();
        } else if (LOWORD(w) == ID_BTN_SETTINGS) {
            if (!FileExists(app.iniPath)) CreateDefaultIni();
            ShellExecuteA(h, "open", app.iniPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    case WM_HOTKEY:
        if (w == ID_HOTKEY_RECORD) {
            if (app.recording) StopRecording(); else StartRecording();
        } else if (w == ID_HOTKEY_PAUSE) {
            TogglePause();
        }
        return 0;
    case WM_TIMER:
        if (w == ID_TIMER_UPDATE) UpdateUI();
        else if (w == ID_TIMER_INI_CHECK) ReloadIniIfChanged();
        return 0;
    case WM_CONVERSION_PROGRESS:
        app.convertProgress = (int)w;
        SetStatus("⏳ Processing video... " + std::to_string((int)w) + "%");
        SendMessage(app.progressBar, PBM_SETPOS, w, 0);
        return 0;
    case WM_CONVERSION_DONE:
        app.converting = false;
        app.convertProgress = 0;
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
        if (w == 1) {
            std::string* path = (std::string*)l;
            long long fs = GetFileSize(*path);
            if (fs > 2048) {
                app.totalBytes += fs;
                SetStatus("✓ " + FormatSize(fs));
                ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + *path + "\"").c_str(), nullptr, SW_SHOWNORMAL);
            }
            delete path;
        } else {
            SetStatus("✗ Conversion failed");
        }
		
        UpdateUI();
        return 0;
    case WM_DESTROY:
        if (app.recording) StopRecording();
        if (!app.tempFile.empty() && FileExists(app.tempFile) && !app.converting) {
            DeleteFileA(app.tempFile.c_str());
        }
        UnregisterHotKey(h, ID_HOTKEY_RECORD);
        UnregisterHotKey(h, ID_HOTKEY_PAUSE);
        KillTimer(h, ID_TIMER_INI_CHECK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// ── Entry point ───────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    app.outputDir = GetVideosFolder();
    app.iniPath = GetExeDir() + "\\RetroRec.ini";

    if (!FindFFmpeg()) {
        MessageBoxA(nullptr, "Place ffmpeg.exe next to RetroRec.exe", "RetroRec", MB_OK);
        return 0;
    }

    CleanupOrphanedTempFiles();
    if (!FileExists(app.iniPath)) CreateDefaultIni();
    LoadConfiguration();
    ConfigureUniversalPipeline();

    // Spawn persistent warm FFmpeg at startup
    SpawnWarmFFmpeg();
	
	Sleep(500);
    PlayNotificationSound();

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "RetroRecWnd";
    RegisterClassExA(&wc);

    int w = 395, h = 340;
    app.hwnd = CreateWindowExA(WS_EX_TOPMOST, "RetroRecWnd",
        "RetroRec v" RETROREC_VERSION,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        w, h, nullptr, nullptr, hInst, nullptr);
    if (!app.hwnd) return 1;
    ShowWindow(app.hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    CleanupWASAPI();
    return 0;
}
