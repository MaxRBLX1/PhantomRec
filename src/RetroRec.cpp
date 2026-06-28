// PhantomRec v1.8 — Universal Ghost Screen Recorder
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Max'sEngine™ | GFX/DDAGrab/GDI Capture | x264 Post-Convert | WASAPI Audio | Zero Drops | Auto-CRF Matrix
// v1.6: Multi-API capture fallback + maxsengine.exe rename + GDI 55 FPS sweet spot

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
#include <gdiplus.h>
#include <algorithm>
#include <powrprof.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "powrprof.lib")

#define PHANTOMREC_VERSION "1.8.0"
#define ID_BTN_RECORD 1001
#define ID_BTN_SETTINGS 1002
#define ID_HOTKEY_RECORD 1
#define ID_HOTKEY_PAUSE 2
#define ID_TIMER_UPDATE 2001
#define ID_TIMER_INI_CHECK 2002
#define WM_CONVERSION_PROGRESS (WM_APP + 1)
#define WM_CONVERSION_DONE     (WM_APP + 2)
#define WM_INI_CHANGED         (WM_APP + 3)
#define ID_BTN_BROWSE_BG   1003
#define ID_BTN_BROWSE_FONT 1004
#define ID_BTN_APPLY       1005
#define ID_BTN_RESET_BG    1006
#define ID_EDIT_FONTSIZE   1007
#define ID_PREVIEW_BG      1008
#define ID_PREVIEW_FONT    1009
#define ID_BTN_COLOR 1010
#define WM_APP_REFRESH_FONTS (WM_APP + 10)

struct PhantomRec {
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
    std::string maxsenginePath;
    std::string outputDir;
    std::string iniPath;
    int pipeBufferSizeMB = 8;
    int screenWidth = 1920;
    int screenHeight = 1080;
    int cpuCoreCount = 4;
    int dynamicThreads = 1;
    int crf = 16;
    int maxrate = 4000;
    int videoQueueSize = 512;
    std::mutex mtx;
    std::chrono::steady_clock::time_point recStart;
    int sessions = 0;
    long long totalBytes = 0;
    int lastRecordingDurationMs = 0;
    int convertPreset = 2;
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
    HWND hSettingsWnd = nullptr;
    std::string customBackground;
    std::string customFont;
    int customFontSize = 14;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    bool backgroundIsGif = false;
    UINT_PTR gifTimerId = 0;
    Gdiplus::Image* gifImage = nullptr;
    UINT gifFrameCount = 0;
    UINT gifCurrentFrame = 0;
    int customFontColor = 0xFFFFFF;
    COLORREF customColorRef = RGB(255, 255, 255);
    int captureMethod = 0; // 0=gfx, 1=ddagrab, 2=gdi
    std::string audioFormat = "s16le";       // ADD THIS
    int audioSampleRate = 48000;             // ADD THIS
    int audioChannels = 2; 
    HWND g_SelectedGameWnd = nullptr;
    LONG g_OriginalGameStyle = 0;
    LONG g_OriginalGameExStyle = 0;
    RECT g_OriginalGameRect = {0};
    bool g_GameStylesModified = false;
	GUID originalPowerPlan;
    bool powerPlanChanged = false;
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
void SpawnWarmEngine();
void OpenSettingsWindow();
LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
void LoadCustomizations();
void SaveCustomizations();
void ApplyBackground(HWND previewWnd, const std::string& path);
void ApplyFont(HWND previewWnd, const std::string& path, int size);
std::string GetCaptureInput();
std::string GetCaptureFilter();
int GetCaptureQuality();
void ForceExclusiveTaskbarRender(HWND hGameWnd);
void RestoreGameWindow(HWND hGameWnd);

int GetWindowsVersion() {
    static int cached = -1;
    if (cached >= 0) return cached;
    
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) { cached = 0; return 0; }
    
    typedef LONG (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
    if (!RtlGetVersion) { cached = 0; return 0; }
    
    RTL_OSVERSIONINFOW osvi = { sizeof(osvi) };
    if (RtlGetVersion(&osvi) != 0) { cached = 0; return 0; }
    
    if (osvi.dwMajorVersion >= 10) cached = 10;
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion >= 2) cached = 8;
    else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) cached = 7;
    else cached = 0;
    
    return cached;
}

void AutoDetectGameWindow() {
    if (app.g_SelectedGameWnd && IsWindow(app.g_SelectedGameWnd)) return;
    
    HWND hForeground = GetForegroundWindow();
    if (!hForeground || hForeground == app.hwnd) return;
    
    char className[256];
    GetClassNameA(hForeground, className, sizeof(className));
    if (strcmp(className, "Shell_TrayWnd") == 0) return;
    if (strcmp(className, "Progman") == 0) return;
    if (strcmp(className, "WorkerW") == 0) return;
    
    app.g_SelectedGameWnd = hForeground;
}

// ── Reliable Windows version detection ────────────────────
std::string GetCaptureInput() {
    int winVer = GetWindowsVersion();
    if (winVer >= 10) {
        app.captureMethod = 0;
        
        // ALWAYS deploy taskbar anchor first — keeps DWM alive
        // even if game is already in exclusive fullscreen
        HWND hTaskbar = FindWindowA("Shell_TrayWnd", nullptr);
        if (hTaskbar) {
            SetWindowPos(hTaskbar, HWND_BOTTOM, 0, 0, 0, 0, 
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        
        if (app.g_SelectedGameWnd && IsWindow(app.g_SelectedGameWnd)) {
            ForceExclusiveTaskbarRender(app.g_SelectedGameWnd);
            
            DWORD_PTR result = 0;
            LRESULT isResponding = SendMessageTimeoutA(
                app.g_SelectedGameWnd, WM_NULL, 0, 0, 
                SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &result
            );
            
            if (isResponding == 0) {
                return " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1";
            } else {
                return " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1";
            }
        }
        
        return " -f lavfi -i gfxcapture=monitor_idx=0:capture_cursor=1";
    } else if (winVer >= 8) {
        app.captureMethod = 1;
        return " -f lavfi -i ddagrab=0:framerate=60";
    } else {
        app.captureMethod = 2;
        return " -f gdigrab -framerate 55 -i desktop";
    }
}

std::string GetCaptureFilter() {
    if (app.captureMethod == 0) {
        // gfxcapture: GPU zero-copy, hardware download
        return " -vf \"hwdownload,format=bgra,format=yuv420p\"";
    } else if (app.captureMethod == 1) {
        // ddagrab: DirectDraw, needs hwdownload
        return " -vf \"hwdownload,format=bgra,format=yuv420p\"";
    } else {
        // GDI: NO FILTERS. mpdecimate causes stutter.
        // Raw frames straight to mpeg4 — ultrafast preset keeps the pipe clear.
        return "";
    }
}

bool IsGameSafeForWindowMod(HWND hWnd) {
    if (!hWnd) return false;
    
    DWORD pid;
    GetWindowThreadProcessId(hWnd, &pid);
    
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return false;
    
    char procName[MAX_PATH];
    DWORD len = MAX_PATH;
    bool isSafe = true;
    
    if (QueryFullProcessImageNameA(hProcess, 0, procName, &len)) {
        std::string name(procName);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        
        // Games with fragile OpenGL/Cocos2d engines that crash on window mods
        if (name.find("geometrydash") != std::string::npos) isSafe = false;
        if (name.find("geometry dash") != std::string::npos) isSafe = false;
        // Add more sensitive games here as they're discovered
    }
    
    CloseHandle(hProcess);
    return isSafe;
}

void ForceExclusiveTaskbarRender(HWND hGameWnd) {
    if (!hGameWnd || !IsWindow(hGameWnd)) return;

    // Skip PhantomRec's own window
    if (hGameWnd == app.hwnd) return;

    bool canModify = IsGameSafeForWindowMod(hGameWnd);
    int scrWidth = GetSystemMetrics(SM_CXSCREEN);
    int scrHeight = GetSystemMetrics(SM_CYSCREEN);

    if (canModify) {
        // Save original styles for restoration
        app.g_OriginalGameStyle = GetWindowLongA(hGameWnd, GWL_STYLE);
        app.g_OriginalGameExStyle = GetWindowLongA(hGameWnd, GWL_EXSTYLE);
        GetWindowRect(hGameWnd, &app.g_OriginalGameRect);
        app.g_GameStylesModified = true;

        // Strip decorative borders
        LONG newStyle = app.g_OriginalGameStyle & 
            ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongA(hGameWnd, GWL_STYLE, newStyle);

        // Force game to fill screen as topmost layer
        SetWindowPos(hGameWnd, HWND_TOPMOST, 0, 0, scrWidth, scrHeight, 
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    // ALWAYS deploy the taskbar anchor — this is the Xbox Game Bar loophole
    // The taskbar keeps DWM awake, gfxcapture keeps reading frames
    // Works even when we can't touch the game window (Geometry Dash, etc.)
    HWND hTaskbar = FindWindowA("Shell_TrayWnd", nullptr);
    if (hTaskbar) {
        SetWindowPos(hTaskbar, HWND_BOTTOM, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void RestoreGameWindow(HWND hGameWnd) {
    // Only restore if we actually modified the window
    if (!app.g_GameStylesModified || !hGameWnd || !IsWindow(hGameWnd)) {
        // Still restore taskbar to normal
        HWND hTaskbar = FindWindowA("Shell_TrayWnd", nullptr);
        if (hTaskbar) {
            SetWindowPos(hTaskbar, HWND_TOP, 0, 0, 0, 0, 
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }
    
    // Restore original window styles
    SetWindowLongA(hGameWnd, GWL_STYLE, app.g_OriginalGameStyle);
    SetWindowLongA(hGameWnd, GWL_EXSTYLE, app.g_OriginalGameExStyle);
    
    // Restore original size and position
    int w = app.g_OriginalGameRect.right - app.g_OriginalGameRect.left;
    int h = app.g_OriginalGameRect.bottom - app.g_OriginalGameRect.top;
    SetWindowPos(hGameWnd, HWND_NOTOPMOST, 
        app.g_OriginalGameRect.left, app.g_OriginalGameRect.top, w, h,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    
    // Restore taskbar to normal position
    HWND hTaskbar = FindWindowA("Shell_TrayWnd", nullptr);
    if (hTaskbar) {
        SetWindowPos(hTaskbar, HWND_TOP, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    
    app.g_GameStylesModified = false;
}

int GetCaptureQuality() {
    if (app.captureMethod == 0) return 2; // gfxcapture: best quality
    if (app.captureMethod == 1) return 2; // ddagrab: same quality as gfx
    return 5; // GDI: lighter encode for older CPUs
}

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
    return "PhantomRec_" + std::string(b);
}

std::string GetVideosFolder() {
    char p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, p))) {
        std::string d = std::string(p) + "\\PhantomRec";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }
    return GetExeDir();
}

bool FindMaxsEngine() {
    // Only look in the same folder as PhantomRec.exe
    // No system PATH search — avoids false positives
    
    std::string local = GetExeDir() + "\\maxsengine.exe";
    if (FileExists(local)) { app.maxsenginePath = local; return true; }
    
    std::string legacy = GetExeDir() + "\\ffmpeg.exe";
    if (FileExists(legacy)) { app.maxsenginePath = legacy; return true; }
    
    return false;
}

void PlayNotificationSound() {
    MessageBeep(MB_ICONINFORMATION);
}

void CreateDefaultIni() {
    std::ofstream ini(app.iniPath);
    ini << "; ========================================\r\n"
        << "; PhantomRec v1.8 Configuration\r\n"
        << "; Made by MaxRBLX1\r\n"
        << "; Max'sEngine(tm) Powered by FFmpeg\r\n"
        << "; ========================================\r\n"
        << "; Capture: Auto-detected by Windows version\r\n"
        << ";   Win10+:  gfxcapture (D3D11 zero-copy)\r\n"
        << ";   Win8+:   ddagrab (DirectDraw)\r\n"
        << ";   Win7:    gdigrab (55 FPS software)\r\n"
        << "; Hotkey: F1-F12 for function keys\r\n"
        << ";         A-Z for Ctrl+Letter hotkeys\r\n"
        << "; PauseHotkey: Same format as Hotkey\r\n"
        << "; ConvertAfterRecording: yes or no\r\n"
        << "; ConvertPreset: ultrafast, veryfast, or medium\r\n"
        << "; ========================================\r\n\r\n"
        << "[Settings]\r\n"
        << "Hotkey=F10\r\n"
        << "PauseHotkey=P\r\n"
        << "ConvertAfterRecording=yes\r\n"
        << "ConvertPreset=veryfast\r\n";
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
    
    // Three presets: 0=ultrafast, 1=veryfast, 2=medium (default)
    GetPrivateProfileStringA("Settings", "ConvertPreset", "medium", buf, sizeof(buf), app.iniPath.c_str());
    if (strcmp(buf, "ultrafast") == 0) app.convertPreset = 0;
    else if (strcmp(buf, "veryfast") == 0) app.convertPreset = 1;
    else app.convertPreset = 2; // medium or anything else
    
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
            UINT oldRecord = app.recordHotkey;
            UINT oldPause = app.pauseHotkey;
            UnregisterHotKey(app.hwnd, ID_HOTKEY_RECORD);
            UnregisterHotKey(app.hwnd, ID_HOTKEY_PAUSE);
            UINT recMod = GetHotkeyModifiers(app.recordHotkey);
            RegisterHotKey(app.hwnd, ID_HOTKEY_RECORD, recMod, app.recordHotkey);
            UINT pauseMod = GetHotkeyModifiers(app.pauseHotkey);
            RegisterHotKey(app.hwnd, ID_HOTKEY_PAUSE, pauseMod, app.pauseHotkey);
            UpdateUI();
            if (oldRecord != app.recordHotkey || oldPause != app.pauseHotkey) {
                InvalidateRect(app.hwnd, nullptr, TRUE);
            }
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
    
    int winVer = GetWindowsVersion();
    
    if (winVer == 7) {
        // Windows 7 + GDI: light everything
        app.crf = 26; app.maxrate = 3000; app.bufsize = 6000;
        app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 2; app.videoQueueSize = 256;
        app.convertPreset = 0; // ultrafast
    } else if (winVer == 8) {
        // Windows 8 + ddagrab: medium-light
        app.crf = 26; app.maxrate = 4000; app.bufsize = 8000;
        app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 4; app.videoQueueSize = 512;
        app.convertPreset = 0; // ultrafast
    } else if (cores <= 2) {
        app.crf = 23; app.maxrate = 4000; app.bufsize = 8000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 4; app.videoQueueSize = 512;
    } else if (cores <= 4) {
        app.crf = 23; app.maxrate = 6000; app.bufsize = 12000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 8; app.videoQueueSize = 512;
    } else if (cores <= 8) {
        app.crf = 23; app.maxrate = 8000; app.bufsize = 16000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 16; app.videoQueueSize = 512;
    } else {
        app.crf = 23; app.maxrate = 12000; app.bufsize = 24000; app.dynamicThreads = 1;
        app.pipeBufferSizeMB = 32; app.videoQueueSize = 512;
    }
}

void UpdateLabelTransparent(HWND hwndParent, HWND hwndLabel, const std::string& newText) {
    RECT rect;
    GetWindowRect(hwndLabel, &rect);
    MapWindowPoints(HWND_DESKTOP, hwndParent, (LPPOINT)&rect, 2);
    InvalidateRect(hwndParent, &rect, FALSE);
    SetWindowTextA(hwndLabel, newText.c_str());
}

// ── UI helpers ────────────────────────────────────────────
void SetStatus(const std::string& t) {
    if (app.lblStatus && IsWindow(app.lblStatus) && app.hwnd) {
        UpdateLabelTransparent(app.hwnd, app.lblStatus, t);
    }
}
void SetButton(const std::string& t) {
    if (app.btnRecord && IsWindow(app.btnRecord)) SetWindowTextA(app.btnRecord, t.c_str());
}

std::string GetHotkeyName(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z') return std::string("Ctrl+") + (char)vk;
    return "F10";
}

std::string GetCaptureMethodName() {
    if (app.captureMethod == 0) return "GFX Capture (D3D11)";
    if (app.captureMethod == 1) return "DDAGrab (DirectDraw)";
    return "GDI Capture (55 FPS)";
}

void UpdateUI() {
    std::string hotkey = GetHotkeyName(app.recordHotkey);
    std::string pauseKey = GetHotkeyName(app.pauseHotkey);

    if (app.converting) {
        SetButton("Processing...");
        std::string s = "Processing video...\r\n";
        s += "Please wait - UI is responsive\r\n\r\n";
        s += "Progress: " + std::to_string(app.convertProgress.load()) + "%";
        SetStatus(s);
        SendMessage(app.progressBar, PBM_SETPOS, app.convertProgress.load(), 0);
    } else if (app.recording && app.paused) {
        SetButton("RESUME (" + pauseKey + ")");
        SetStatus("PAUSED\r\n" + GetCaptureMethodName() + " -> x264 Post-Convert");
        SendMessage(app.progressBar, PBM_SETMARQUEE, 0, 0);
    } else if (app.recording) {
        auto e = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - app.recStart).count();
        SetButton("STOP (" + hotkey + ")");
        std::string status = "REC " + FormatTime((int)e);
        if (app.audioActive) status += "\r\n[AUDIO] Pipe Active";
        status += "\r\n" + GetCaptureMethodName() + " -> x264 Post-Convert";
        SetStatus(status);
        SendMessage(app.progressBar, PBM_SETMARQUEE, 1, 0);
    } else {
        SetButton("START (" + hotkey + ")");
        std::string s = "Ready - " + hotkey + " to record\r\n";
        s += "Pause: " + pauseKey + "\r\n\r\n";
        s += "Cores: " + std::to_string(app.cpuCoreCount);
        s += " | Threads: " + std::to_string(app.dynamicThreads);
        s += "\r\nTarget CRF: " + std::to_string(app.crf);
        s += "\r\n" + std::to_string(app.screenWidth) + "x" + std::to_string(app.screenHeight);
        s += "\r\n" + GetCaptureMethodName() + " -> x264 Pipeline";
        s += "\r\nMax'sEngine(tm) Powered by FFmpeg";
        s += "\r\nBuilt by MaxRBLX1";
        if (app.sessions > 0) s += "\r\nSessions: " + std::to_string(app.sessions);
        SetStatus(s);
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
    }
}

// ── WASAPI Audio Capture ──────────────────────────────────
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

void SpawnWarmEngine() {
    // Pre-initialize capture pipeline based on detected method
    // Warms GPU driver cache for gfxcapture/ddagrab
    GetCaptureInput(); // Sets app.captureMethod
    
    std::string warmupCmd;
    if (app.captureMethod <= 1) {
        // gfxcapture or ddagrab — warm the D3D11/DirectDraw pipeline
        warmupCmd = 
            "cmd.exe /c \"\"" + app.maxsenginePath + "\" -y -hide_banner -loglevel error "
            + GetCaptureInput() + " "
            "-frames:v 1 -c:v mpeg4 -q:v 3 -f null NUL\"";
    } else {
        // GDI — just verify the engine runs
        warmupCmd = 
            "cmd.exe /c \"\"" + app.maxsenginePath + "\" -y -hide_banner -loglevel error "
            "-f gdigrab -framerate 1 -i desktop "
            "-frames:v 1 -c:v mpeg4 -q:v 5 -f null NUL\"";
    }
    
    STARTUPINFOA siWarm = { sizeof(siWarm) };
    siWarm.dwFlags = STARTF_USESHOWWINDOW;
    siWarm.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION piWarm = {0};
    std::vector<char> warmupCl(warmupCmd.begin(), warmupCmd.end());
    warmupCl.push_back('\0');
    if (CreateProcessA(nullptr, warmupCl.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &siWarm, &piWarm)) {
        WaitForSingleObject(piWarm.hProcess, 3000);
        CloseHandle(piWarm.hProcess);
        CloseHandle(piWarm.hThread);
    }
}

// ── Customization ─────────────────────────────────────────
void LoadCustomizations() {
    char buf[MAX_PATH] = {0};
    GetPrivateProfileStringA("Appearance", "Background", "", buf, sizeof(buf), app.iniPath.c_str());
    if (strlen(buf) > 0) {
        app.customBackground = buf;
        if (app.customBackground.find(":\\") == std::string::npos) {
            app.customBackground = GetExeDir() + "\\" + app.customBackground;
        }
    }
    memset(buf, 0, sizeof(buf));
    GetPrivateProfileStringA("Appearance", "Font", "", buf, sizeof(buf), app.iniPath.c_str());
    if (strlen(buf) > 0) {
        app.customFont = buf;
        if (app.customFont.find(":\\") == std::string::npos) {
            app.customFont = GetExeDir() + "\\" + app.customFont;
        }
        if (FileExists(app.customFont)) {
            AddFontResourceExA(app.customFont.c_str(), FR_PRIVATE, 0);
        }
    }
    app.customFontSize = GetPrivateProfileIntA("Appearance", "FontSize", 14, app.iniPath.c_str());
    app.customColorRef = (COLORREF)GetPrivateProfileIntA("Appearance", "FontColor", 
        RGB(255, 255, 255), app.iniPath.c_str());
}

void SaveCustomizations() {
    WritePrivateProfileStringA("Appearance", "Background", app.customBackground.c_str(), app.iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "Font", app.customFont.c_str(), app.iniPath.c_str());
    char sizeBuf[16]; sprintf_s(sizeBuf, "%d", app.customFontSize);
    WritePrivateProfileStringA("Appearance", "FontSize", sizeBuf, app.iniPath.c_str());
    char colorBuf[16]; sprintf_s(colorBuf, "%d", (int)app.customColorRef);
    WritePrivateProfileStringA("Appearance", "FontColor", colorBuf, app.iniPath.c_str());
}

void ApplyBackground(HWND previewWnd, const std::string& path) {
    if (!FileExists(path)) return;
    Gdiplus::Image image(std::wstring(path.begin(), path.end()).c_str());
    if (image.GetLastStatus() != Gdiplus::Ok) return;
    RECT rect; GetClientRect(previewWnd, &rect);
    HDC hdc = GetDC(previewWnd);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
    Gdiplus::Graphics graphics(memDC);
    graphics.DrawImage(&image, 0, 0, rect.right, rect.bottom);
    BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp); DeleteObject(memBmp); DeleteDC(memDC);
    ReleaseDC(previewWnd, hdc);
}

void ApplyFont(HWND previewWnd, const std::string& path, int size) {
    if (!FileExists(path)) return;
    int result = AddFontResourceExA(path.c_str(), FR_PRIVATE, 0);
    if (result == 0) return;
    std::string fileName = path;
    auto pos = fileName.find_last_of("\\/");
    if (pos != std::string::npos) fileName = fileName.substr(pos + 1);
    std::string faceName;
    HDC hdc = GetDC(previewWnd);
    LOGFONTA lf = {0}; lf.lfCharSet = DEFAULT_CHARSET;
    strncpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), fileName.c_str(), _TRUNCATE);
    HFONT testFont = CreateFontA(size, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fileName.c_str());
    if (testFont) {
        SelectObject(hdc, testFont);
        GetTextFaceA(hdc, LF_FACESIZE, lf.lfFaceName);
        faceName = lf.lfFaceName;
        DeleteObject(testFont);
    }
    ReleaseDC(previewWnd, hdc);
    if (faceName.empty()) {
        faceName = fileName;
        pos = faceName.find_last_of('.');
        if (pos != std::string::npos) faceName = faceName.substr(0, pos);
    }
    HFONT hFont = CreateFontA(size, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
    if (hFont) {
        HFONT hOldFont = (HFONT)SendMessage(previewWnd, WM_GETFONT, 0, 0);
        SendMessage(previewWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hOldFont && hOldFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(hOldFont);
        InvalidateRect(previewWnd, nullptr, TRUE);
    }
}

LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HWND previewBg, previewFont, editFontSize;
    static std::string selectedBg, selectedFont;
    static int selectedFontSize;
    switch (m) {
    case WM_CREATE: {
        selectedBg = app.customBackground;
        selectedFont = app.customFont;
        selectedFontSize = app.customFontSize;
        CreateWindowA("STATIC", "Background", WS_VISIBLE | WS_CHILD | SS_LEFT, 15, 10, 280, 20, h, nullptr, nullptr, nullptr);
        previewBg = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_BLACKRECT | SS_SUNKEN, 15, 35, 600, 180, h, (HMENU)ID_PREVIEW_BG, nullptr, nullptr);
        CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 15, 225, 80, 22, h, (HMENU)ID_BTN_BROWSE_BG, nullptr, nullptr);
        CreateWindowA("BUTTON", "Reset", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 105, 225, 80, 22, h, (HMENU)ID_BTN_RESET_BG, nullptr, nullptr);
        CreateWindowA("STATIC", "Font", WS_VISIBLE | WS_CHILD | SS_LEFT, 15, 260, 280, 20, h, nullptr, nullptr, nullptr);
        previewFont = CreateWindowA("STATIC", "MaxRBLX1", WS_VISIBLE | WS_CHILD | SS_CENTER | SS_SUNKEN, 15, 280, 600, 50, h, (HMENU)ID_PREVIEW_FONT, nullptr, nullptr);
        CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 15, 340, 80, 22, h, (HMENU)ID_BTN_BROWSE_FONT, nullptr, nullptr);
        CreateWindowA("STATIC", "Size:", WS_VISIBLE | WS_CHILD | SS_RIGHT, 90, 342, 40, 20, h, nullptr, nullptr, nullptr);
        editFontSize = CreateWindowA("EDIT", std::to_string(selectedFontSize).c_str(), WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 135, 340, 45, 22, h, (HMENU)ID_EDIT_FONTSIZE, nullptr, nullptr);
        CreateWindowA("STATIC", "Color:", WS_VISIBLE | WS_CHILD | SS_RIGHT, 180, 342, 50, 20, h, nullptr, nullptr, nullptr);
        CreateWindowA("BUTTON", "Pick Color", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 235, 340, 80, 22, h, (HMENU)ID_BTN_COLOR, nullptr, nullptr);
        CreateWindowA("BUTTON", "Apply", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 200, 390, 80, 28, h, (HMENU)ID_BTN_APPLY, nullptr, nullptr);
        CreateWindowA("BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 300, 390, 80, 28, h, (HMENU)IDCANCEL, nullptr, nullptr);
        if (!selectedBg.empty()) ApplyBackground(previewBg, selectedBg);
        if (!selectedFont.empty()) ApplyFont(previewFont, selectedFont, selectedFontSize);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        if ((HWND)l == previewFont) {
            SetTextColor((HDC)w, app.customColorRef);
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        return DefWindowProcA(h, m, w, l);
    }
    case WM_COMMAND: {
        if (LOWORD(w) == ID_BTN_BROWSE_BG) {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn = { sizeof(ofn) };
            ofn.hwndOwner = h;
            ofn.lpstrFilter = "Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) { selectedBg = file; ApplyBackground(previewBg, selectedBg); }
        } else if (LOWORD(w) == ID_BTN_RESET_BG) { selectedBg.clear(); InvalidateRect(previewBg, nullptr, TRUE); }
        else if (LOWORD(w) == ID_BTN_BROWSE_FONT) {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn = { sizeof(ofn) };
            ofn.hwndOwner = h;
            ofn.lpstrFilter = "Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                selectedFont = file;
                AddFontResourceExA(selectedFont.c_str(), FR_PRIVATE, 0);
                std::string faceName = selectedFont;
                auto pos = faceName.find_last_of("\\/");
                if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.');
                if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                static HFONT hPreviewFontHandle = nullptr;
                if (hPreviewFontHandle) DeleteObject(hPreviewFontHandle);
                hPreviewFontHandle = CreateFontA(selectedFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                if (hPreviewFontHandle) SendMessage(previewFont, WM_SETFONT, (WPARAM)hPreviewFontHandle, TRUE);
                InvalidateRect(previewFont, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_COLOR) {
            CHOOSECOLORA cc = { sizeof(cc) };
            cc.hwndOwner = h; cc.rgbResult = app.customColorRef;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            static COLORREF acrCustClr[16] = {0}; cc.lpCustColors = acrCustClr;
            if (ChooseColorA(&cc)) { app.customColorRef = cc.rgbResult; InvalidateRect(previewFont, nullptr, TRUE); }
        } else if (LOWORD(w) == ID_BTN_APPLY) {
            char sizeBuf[8]; GetWindowTextA(editFontSize, sizeBuf, sizeof(sizeBuf));
            int newSize = atoi(sizeBuf);
            if (newSize >= 8 && newSize <= 72) selectedFontSize = newSize;
            app.customBackground = selectedBg;
            app.customFont = selectedFont;
            app.customFontSize = selectedFontSize;
            SaveCustomizations();
            char colorBuf[16]; sprintf_s(colorBuf, "%d", (int)app.customColorRef);
            WritePrivateProfileStringA("Appearance", "FontColor", colorBuf, app.iniPath.c_str());
            if (app.gifImage) { delete app.gifImage; app.gifImage = nullptr; }
            if (app.gifTimerId) { KillTimer(app.hwnd, app.gifTimerId); app.gifTimerId = 0; }
            app.backgroundIsGif = false;
            if (!app.customBackground.empty()) {
                std::string ext = app.customBackground;
                auto dot = ext.find_last_of('.');
                if (dot != std::string::npos) {
                    ext = ext.substr(dot);
                    if (ext == ".gif" || ext == ".GIF") {
                        app.backgroundIsGif = true;
                        std::wstring wpath(app.customBackground.begin(), app.customBackground.end());
                        app.gifImage = new Gdiplus::Image(wpath.c_str());
                        if (app.gifImage->GetLastStatus() == Gdiplus::Ok) {
                            GUID pageGuid = Gdiplus::FrameDimensionTime;
                            app.gifFrameCount = app.gifImage->GetFrameCount(&pageGuid);
                            app.gifCurrentFrame = 0;
                            if (app.gifFrameCount > 1) {
                                UINT frameDelay = 100;
                                UINT size = app.gifImage->GetPropertyItemSize(PropertyTagFrameDelay);
                                if (size > 0) {
                                    Gdiplus::PropertyItem* prop = (Gdiplus::PropertyItem*)malloc(size);
                                    if (prop && app.gifImage->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
                                        long* delays = (long*)prop->value;
                                        if (app.gifFrameCount > 0 && delays[0] > 0) {
                                            frameDelay = delays[0] * 10;
                                            if (frameDelay < 16) frameDelay = 16;
                                        }
                                    }
                                    free(prop);
                                }
                                app.gifTimerId = SetTimer(app.hwnd, 3001, frameDelay, nullptr);
                            }
                        }
                    }
                }
            }
            InvalidateRect(app.hwnd, nullptr, TRUE);
            UpdateWindow(app.hwnd);
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); }
        return 0;
    }
    case WM_NCDESTROY: editFontSize = nullptr; previewBg = nullptr; previewFont = nullptr; app.hSettingsWnd = nullptr; return 0;
    case WM_DESTROY: app.hSettingsWnd = nullptr; return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int ScaleXForWindow(HWND hwnd, int x) {
    UINT dpi = 96;
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
        auto pGetDpiForWindow = (GetDpiForWindowProc)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pGetDpiForWindow && hwnd) dpi = pGetDpiForWindow(hwnd);
    }
    return MulDiv(x, dpi, 96);
}
int ScaleYForWindow(HWND hwnd, int y) { return ScaleXForWindow(hwnd, y); }

void OpenSettingsWindow() {
    if (app.hSettingsWnd && IsWindow(app.hSettingsWnd)) { SetForegroundWindow(app.hSettingsWnd); return; }
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "PhantomRecSettings"; RegisterClassExA(&wc);
    int scaledW = ScaleXForWindow(app.hwnd, 640);
    int scaledH = ScaleYForWindow(app.hwnd, 480);
    app.hSettingsWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        "PhantomRecSettings", "Customization",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (GetSystemMetrics(SM_CXSCREEN) - scaledW) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - scaledH) / 2,
        scaledW, scaledH, app.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
}

// ── Build FFmpeg capture command ──────────────────────────
std::string BuildCaptureCommand(const std::string& outputFile, bool hasAudio, HANDLE hAudioRead) {
    std::ostringstream c;
    c << "cmd.exe /c \""
      << "\"" << app.maxsenginePath << "\" -y -hide_banner -loglevel error";
    
    // Buffer size varies by capture method
    if (app.captureMethod == 2) {
        c << " -rtbufsize 500M"; // GDI: smaller buffer
    } else {
        c << " -rtbufsize 2000M"; // gfx/dda: large buffer
    }
    
    c << " -thread_queue_size " << app.videoQueueSize
      << GetCaptureInput();
    
    if (hasAudio) {
        c << " -thread_queue_size 512"
          << " -f " << app.audioFormat << " -ar " << app.audioSampleRate 
          << " -ac " << app.audioChannels << " -i pipe:0";
    }
    
    // Get the filter string (empty for GDI)
    std::string filterStr = GetCaptureFilter();
    
    c << " -max_muxing_queue_size 2048 -thread_queue_size 2048 -fps_mode vfr"
      << filterStr
      << " -c:v mpeg4 -q:v " << GetCaptureQuality();
    
    // GFX: veryfast (GPU zero-copy handles the heavy lifting)
    // DDAGrab + GDI: ultrafast (CPU-bound, need fastest encode)
    if (app.captureMethod == 0) {
        c << " -preset veryfast";
    } else {
        c << " -preset ultrafast";
    }
    
    c << " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    
    if (hasAudio) c << " -c:a copy";
    c << " -shortest -fflags +genpts"
      << " -threads " << app.dynamicThreads
      << " -f matroska \"" << outputFile << "\""
      << "\"";
    
    return c.str();
}

// ── Pause/Resume ──────────────────────────────────────────
void TogglePause() {
    if (!app.recording || app.converting) return;
    app.paused = !app.paused;
    if (app.paused) {
        app.pauseTime = std::chrono::steady_clock::now();
        app.recording = false;
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
        app.recording = true;
        UpdateUI();
    } else {
        auto now = std::chrono::steady_clock::now();
        app.totalPausedDuration += std::chrono::duration_cast<std::chrono::milliseconds>(now - app.pauseTime);
        app.pauseSegmentCount++;
        std::string segFile = app.outputDir + "\\" + app.segmentBaseName + "_seg" + 
                              std::to_string(app.pauseSegmentCount) + "_temp.mkv";
        app.segmentFiles.push_back(segFile);
        bool wasapiReady = InitializeWASAPI();
        if (wasapiReady) {
            SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
            CreatePipe(&app.hAudioPipeRead, &app.hAudioPipeWrite, &sa, app.pipeBufferSizeMB * 1024 * 1024);
            SetHandleInformation(app.hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
        }
        std::string cmdStr = BuildCaptureCommand(segFile, wasapiReady, app.hAudioPipeRead);
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        if (wasapiReady && app.hAudioPipeRead != nullptr) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = app.hAudioPipeRead;
        }
        si.wShowWindow = SW_HIDE;
        std::vector<char> cl(cmdStr.begin(), cmdStr.end());
        cl.push_back('\0');
        if (wasapiReady) app.audioThread = std::thread(AudioToPipeLoop);
        CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
            CREATE_NEW_PROCESS_GROUP | NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &si, &app.ffmpegProcess);
        if (app.hAudioPipeRead) { CloseHandle(app.hAudioPipeRead); app.hAudioPipeRead = nullptr; }
        UpdateUI();
    }
}

// ── Recording engine ──────────────────────────────────────
void StartRecording() {
    std::lock_guard<std::mutex> l(app.mtx);
    if (!app.powerPlanChanged) {
        GUID highPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        GUID* pOriginalGuid = nullptr;
        if (PowerGetActiveScheme(nullptr, &pOriginalGuid) == ERROR_SUCCESS) {
            app.originalPowerPlan = *pOriginalGuid;
            LocalFree(pOriginalGuid);
            PowerSetActiveScheme(nullptr, &highPerf);
            app.powerPlanChanged = true;
        }
    }
    if (app.recording || app.converting) return;
    if (app.maxsenginePath.empty() && !FindMaxsEngine()) {
        MessageBoxA(app.hwnd, "maxsengine.exe not found!\n\nPlace maxsengine.exe (FFmpeg) next to PhantomRec.exe\nPowered by FFmpeg — ffmpeg.org", "PhantomRec", MB_OK);
        return;
    }
    ReloadIniIfChanged();
    DetectCurrentResolution();
    ConfigureUniversalPipeline();
    AutoDetectGameWindow();  // ← Must run before GetCaptureInput
    GetCaptureInput();

    std::string ts = Timestamp();
    app.segmentBaseName = ts;
    app.pauseSegmentCount = 0;
    app.segmentFiles.clear();
    app.totalPausedDuration = std::chrono::milliseconds(0);
    app.paused = false;
    app.tempFile = app.outputDir + "\\" + ts + "_seg0_temp.mkv";
    app.segmentFiles.push_back(app.tempFile);
    app.finalFile = app.outputDir + "\\" + ts + ".mkv";

    bool wasapiReady = InitializeWASAPI();
    if (wasapiReady) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        CreatePipe(&app.hAudioPipeRead, &app.hAudioPipeWrite, &sa, app.pipeBufferSizeMB * 1024 * 1024);
        SetHandleInformation(app.hAudioPipeWrite, HANDLE_FLAG_INHERIT, 0);
    }
    
    app.audioFormat = "s16le"; app.audioSampleRate = 48000; app.audioChannels = 2;
    if (wasapiReady && app.waveFormat) {
        app.audioFormat = (app.audioBitsPerSample == 32) ? "f32le" : "s16le";
        app.audioSampleRate = app.waveFormat->nSamplesPerSec;
        app.audioChannels = app.waveFormat->nChannels;
    }

    app.recording = true;
    std::string cmdStr = BuildCaptureCommand(app.tempFile, wasapiReady, app.hAudioPipeRead);
    
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (wasapiReady && app.hAudioPipeRead != nullptr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = app.hAudioPipeRead;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_HIDE;

    std::vector<char> cl(cmdStr.begin(), cmdStr.end());
    cl.push_back('\0');

    if (wasapiReady) app.audioThread = std::thread(AudioToPipeLoop);

    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE,
        CREATE_NEW_PROCESS_GROUP | ABOVE_NORMAL_PRIORITY_CLASS,
        nullptr, nullptr, &si, &app.ffmpegProcess)) {
        app.recording = false;
        if (wasapiReady) {
            if (app.audioThread.joinable()) app.audioThread.join();
            CleanupWASAPI();
        }
        SetStatus("Failed"); return;
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
    SetStatus("Finalizing...");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - app.recStart).count();
    app.lastRecordingDurationMs = (int)elapsed;
	if (app.lastRecordingDurationMs < 1000) app.lastRecordingDurationMs = 1000;
    app.recording = false;
    if (app.powerPlanChanged) {
        PowerSetActiveScheme(nullptr, &app.originalPowerPlan);
        app.powerPlanChanged = false;
    }
    if (app.g_SelectedGameWnd) {
        RestoreGameWindow(app.g_SelectedGameWnd);
		app.g_SelectedGameWnd = nullptr;
    }
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
    Sleep(1500);

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

    if (validSegments == 0) {
        DeleteFileA(segmentsTxt.c_str());
        SetStatus("No recording data found");
        UpdateUI();
        return;
    }

    if (app.convertAfterRecording && app.lastRecordingDurationMs >= 1000) {
        app.converting = true;
        SetStatus("Processing video...");
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
        MessageBoxA(app.hwnd, 
            "Conversion started!\n\n"
            "The PhantomRec window will freeze during processing.\n"
            "This is normal — your recording is being compressed.\n\n"
            "Please wait...",
            "PhantomRec — Processing", MB_OK | MB_ICONINFORMATION);
        SYSTEM_INFO siCpu; GetSystemInfo(&siCpu);
        int threads = siCpu.dwNumberOfProcessors;
        if (threads < 2) threads = 2;
        
        // Use preset from config: ultrafast for ddagrab/GDI, veryfast for gfxcapture
        const char* presetStr;
        if (app.convertPreset == 0) presetStr = "ultrafast";
        else if (app.convertPreset == 1) presetStr = "veryfast";
        else presetStr = "medium";
        
        char cmdLine[4096];
        sprintf_s(cmdLine, sizeof(cmdLine),
            "cmd.exe /c \"\"%s\" -y -progress pipe:1 -loglevel error -f concat -safe 0 -i \"%s\" -fps_mode cfr -r 60 -c:v libx264 -preset %s -crf %d -c:a aac -b:a 128k -pix_fmt yuv420p -threads %d \"%s\"\"",
            app.maxsenginePath.c_str(), segmentsTxt.c_str(), presetStr, app.crf, threads, app.finalFile.c_str());
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
            CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS,
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
                            SetStatus("Processing video... " + std::to_string(percent) + "%");
                        } catch (...) {}
                    }
                }
            }
            CloseHandle(hRead);
            WaitForSingleObject(convertPI.hProcess, INFINITE);
            CloseHandle(convertPI.hProcess);
            CloseHandle(convertPI.hThread);
            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
            DeleteFileA(segmentsTxt.c_str());
            for (const auto& seg : app.segmentFiles) { if (FileExists(seg)) DeleteFileA(seg.c_str()); }
            app.segmentFiles.clear();
            app.converting = false;
            app.convertProgress = 0;
            SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
            long long fs = GetFileSize(app.finalFile);
            if (fs > 2048) {
                app.totalBytes += fs;
                SetStatus(" " + FormatSize(fs));
                ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + app.finalFile + "\"").c_str(), nullptr, SW_SHOWNORMAL);
            } else { SetStatus("Conversion failed"); }
            UpdateUI();
            return;
        } else { CloseHandle(hRead); CloseHandle(hWrite); }
    }
    app.converting = false; app.convertProgress = 0;
    SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
    long long fs = GetFileSize(app.finalFile);
    if (fs > 2048) {
        app.totalBytes += fs;
        SetStatus(" " + FormatSize(fs));
        ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + app.finalFile + "\"").c_str(), nullptr, SW_SHOWNORMAL);
    } else { SetStatus("File Error: Corrupted or Empty"); }
    UpdateUI();
}

// ── Window procedure ──────────────────────────────────────
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP_REFRESH_FONTS: {
        if (!app.customFont.empty() && FileExists(app.customFont)) {
            AddFontResourceExA(app.customFont.c_str(), FR_PRIVATE, 0);
            std::string faceName = app.customFont;
            auto pos = faceName.find_last_of("\\/");
            if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
            pos = faceName.find_last_of('.');
            if (pos != std::string::npos) faceName = faceName.substr(0, pos);
            HFONT hFreshFont = CreateFontA(app.customFontSize, 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
            if (hFreshFont) {
                if (app.lblStatus && IsWindow(app.lblStatus)) SendMessage(app.lblStatus, WM_SETFONT, (WPARAM)hFreshFont, TRUE);
                if (app.btnRecord && IsWindow(app.btnRecord)) SendMessage(app.btnRecord, WM_SETFONT, (WPARAM)hFreshFont, TRUE);
            }
        }
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);
        UpdateUI();
        return 0;
    }
    case WM_CREATE: {
        std::string startLabel = "START (" + GetHotkeyName(app.recordHotkey) + ")";
        app.btnRecord = CreateWindowA("BUTTON", startLabel.c_str(),
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 20, 340, 50, h, (HMENU)ID_BTN_RECORD, nullptr, nullptr);
        CreateWindowA("BUTTON", "", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 360, 5, 30, 30, h, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
        app.lblStatus = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_LEFT, 20, 120, 340, 130, h, nullptr, nullptr, nullptr);
        app.progressBar = CreateWindowA("msctls_progress32", "", WS_VISIBLE | WS_CHILD | PBS_MARQUEE, 20, 260, 340, 15, h, nullptr, nullptr, nullptr);
        UINT recMod = GetHotkeyModifiers(app.recordHotkey); RegisterHotKey(h, ID_HOTKEY_RECORD, recMod, app.recordHotkey);
        UINT pauseMod = GetHotkeyModifiers(app.pauseHotkey); RegisterHotKey(h, ID_HOTKEY_PAUSE, pauseMod, app.pauseHotkey);
        SetTimer(h, ID_TIMER_INI_CHECK, 2000, nullptr);
        LoadCustomizations();
        SetButton("START (" + GetHotkeyName(app.recordHotkey) + ")");
        GetCaptureInput(); UpdateUI();
        if (!app.customBackground.empty() && FileExists(app.customBackground)) {
            std::string ext = app.customBackground;
            auto dot = ext.find_last_of('.');
            if (dot != std::string::npos) {
                ext = ext.substr(dot);
                if (ext == ".gif" || ext == ".GIF") {
                    app.backgroundIsGif = true;
                    std::wstring wpath(app.customBackground.begin(), app.customBackground.end());
                    app.gifImage = new Gdiplus::Image(wpath.c_str());
                    if (app.gifImage->GetLastStatus() == Gdiplus::Ok) {
                        GUID pageGuid = Gdiplus::FrameDimensionTime;
                        app.gifFrameCount = app.gifImage->GetFrameCount(&pageGuid);
                        if (app.gifFrameCount > 1) {
                            UINT frameDelay = 100;
                            UINT size = app.gifImage->GetPropertyItemSize(PropertyTagFrameDelay);
                            if (size > 0) {
                                Gdiplus::PropertyItem* prop = (Gdiplus::PropertyItem*)malloc(size);
                                if (prop && app.gifImage->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
                                    long* delays = (long*)prop->value;
                                    if (app.gifFrameCount > 0 && delays[0] > 0) {
                                        frameDelay = delays[0] * 10;
                                        if (frameDelay < 16) frameDelay = 16;
                                    }
                                }
                                free(prop);
                            }
                            app.gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
                        }
                    }
                }
            }
        }
        return 0;
    }
    case WM_ACTIVATE: {
        if (LOWORD(w) == WA_INACTIVE) SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)w;
        static HFONT hCurrentFont = nullptr;
        static std::string lastFontName = "";
        static int lastFontSize = 0;
        if (!app.customFont.empty()) {
            if (app.customFont != lastFontName || app.customFontSize != lastFontSize) {
                if (hCurrentFont) { SelectObject(hdcStatic, (HFONT)GetStockObject(SYSTEM_FONT)); DeleteObject(hCurrentFont); hCurrentFont = nullptr; }
                std::string faceName = app.customFont;
                auto pos = faceName.find_last_of("\\/"); if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hCurrentFont = CreateFontA(app.customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastFontName = app.customFont; lastFontSize = app.customFontSize;
            }
            if (hCurrentFont) SelectObject(hdcStatic, hCurrentFont);
        }
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, app.customColorRef);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        RECT rect; GetClientRect(h, &rect);
        if (app.backgroundIsGif && app.gifImage) {
            Gdiplus::Graphics graphics(hdc);
            graphics.DrawImage(app.gifImage, 0, 0, rect.right, rect.bottom);
        } else if (!app.customBackground.empty() && FileExists(app.customBackground)) {
            Gdiplus::Image image(std::wstring(app.customBackground.begin(), app.customBackground.end()).c_str());
            if (image.GetLastStatus() == Gdiplus::Ok) {
                Gdiplus::Graphics graphics(hdc);
                graphics.DrawImage(&image, 0, 0, rect.right, rect.bottom);
            }
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORBTN: {
        HDC hdcBtn = (HDC)w;
        static HFONT hBtnFont = nullptr;
        static std::string lastBtnFontName = "";
        static int lastBtnFontSize = 0;
        if (!app.customFont.empty()) {
            if (app.customFont != lastBtnFontName || app.customFontSize != lastBtnFontSize) {
                if (hBtnFont) DeleteObject(hBtnFont);
                std::string faceName = app.customFont;
                auto pos = faceName.find_last_of("\\/"); if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hBtnFont = CreateFontA(app.customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastBtnFontName = app.customFont; lastBtnFontSize = app.customFontSize;
            }
            if (hBtnFont) SelectObject(hdcBtn, hBtnFont);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_RECORD) { if (app.recording) StopRecording(); else StartRecording(); }
        else if (LOWORD(w) == ID_BTN_SETTINGS) { OpenSettingsWindow(); InvalidateRect(h, nullptr, TRUE); }
        return 0;
    case WM_HOTKEY:
        if (w == ID_HOTKEY_RECORD) { if (app.recording) StopRecording(); else StartRecording(); }
        else if (w == ID_HOTKEY_PAUSE) { TogglePause(); }
        return 0;
    case WM_TIMER:
        if (w == ID_TIMER_UPDATE) { if (!IsIconic(h)) UpdateUI(); }
        else if (w == ID_TIMER_INI_CHECK) { if (!IsIconic(h)) ReloadIniIfChanged(); }
        else if (w == 3001 && app.gifImage && app.gifFrameCount > 1) {
            if (!IsIconic(h)) {
                app.gifCurrentFrame = (app.gifCurrentFrame + 1) % app.gifFrameCount;
                GUID pageGuid = Gdiplus::FrameDimensionTime;
                app.gifImage->SelectActiveFrame(&pageGuid, app.gifCurrentFrame);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_SYSCOMMAND: {
        UINT sysCmd = (w & 0xFFF0);
        if (sysCmd == SC_MINIMIZE) { if (app.gifTimerId) { KillTimer(h, app.gifTimerId); app.gifTimerId = 0; } }
        else if (sysCmd == SC_RESTORE) {
            if (app.backgroundIsGif && app.gifImage && app.gifFrameCount > 1 && !app.gifTimerId) {
                UINT frameDelay = 100;
                UINT size = app.gifImage->GetPropertyItemSize(PropertyTagFrameDelay);
                if (size > 0) {
                    Gdiplus::PropertyItem* prop = (Gdiplus::PropertyItem*)malloc(size);
                    if (prop && app.gifImage->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
                        long* delays = (long*)prop->value;
                        if (app.gifFrameCount > 0 && delays[0] > 0) { frameDelay = delays[0] * 10; if (frameDelay < 16) frameDelay = 16; }
                    }
                    free(prop);
                }
                app.gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
            }
            InvalidateRect(h, nullptr, TRUE);
        }
        return DefWindowProcA(h, m, w, l);
    }
    case WM_CONVERSION_PROGRESS:
        app.convertProgress = (int)w;
        SetStatus("Processing video... " + std::to_string((int)w) + "%");
        SendMessage(app.progressBar, PBM_SETPOS, w, 0);
        { RECT rc; GetWindowRect(app.lblStatus, &rc); MapWindowPoints(HWND_DESKTOP, h, (LPPOINT)&rc, 2); InvalidateRect(h, &rc, TRUE); }
        return 0;
    case WM_CONVERSION_DONE:
        app.converting = false; app.convertProgress = 0;
        SendMessage(app.progressBar, PBM_SETPOS, 0, 0);
        if (w == 1) {
            std::string* path = (std::string*)l;
            long long fs = GetFileSize(*path);
            if (fs > 2048) { app.totalBytes += fs; SetStatus(" " + FormatSize(fs));
                ShellExecuteA(nullptr, "open", "explorer", ("/select,\"" + *path + "\"").c_str(), nullptr, SW_SHOWNORMAL); }
            delete path;
        } else SetStatus("Conversion failed");
        UpdateUI(); InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_DESTROY:
        if (app.recording) StopRecording();
        if (!app.tempFile.empty() && FileExists(app.tempFile) && !app.converting) DeleteFileA(app.tempFile.c_str());
        if (app.gifImage) { delete app.gifImage; app.gifImage = nullptr; }
        if (app.gifTimerId) KillTimer(h, app.gifTimerId);
        UnregisterHotKey(h, ID_HOTKEY_RECORD); UnregisterHotKey(h, ID_HOTKEY_PAUSE);
        KillTimer(h, ID_TIMER_INI_CHECK);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// ── Entry point ───────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    Gdiplus::GdiplusStartup(&app.gdiplusToken, &app.gdiplusInput, nullptr);
    app.outputDir = GetVideosFolder();
    app.iniPath = GetExeDir() + "\\PhantomRec.ini";
    if (!FindMaxsEngine()) {
        MessageBoxA(nullptr, 
            "maxsengine.exe not found!\n\n"
            "Place maxsengine.exe (FFmpeg renamed) next to PhantomRec.exe\n"
            "or keep ffmpeg.exe for backward compatibility.\n\n"
            "Max'sEngine(tm) Powered by FFmpeg — ffmpeg.org",
            "PhantomRec v" PHANTOMREC_VERSION, MB_OK);
        Gdiplus::GdiplusShutdown(app.gdiplusToken);
        return 0;
    }
    CleanupOrphanedTempFiles();
    if (!FileExists(app.iniPath)) {
        CreateDefaultIni();
        // Flush Windows INI cache so LoadConfiguration reads real values
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, app.iniPath.c_str());
    }
    LoadConfiguration();
    ConfigureUniversalPipeline();
    SpawnWarmEngine();
    Sleep(500);
    PlayNotificationSound();
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; 
    wc.lpszClassName = "PhantomRecWnd"; RegisterClassExA(&wc);
    int w = 395, h = 340;
    app.hwnd = CreateWindowExA(WS_EX_TOPMOST | 0x02000000L, "PhantomRecWnd",
        "PhantomRec v" PHANTOMREC_VERSION " — Max'sEngine(tm)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        w, h, nullptr, nullptr, hInst, nullptr);
    if (!app.hwnd) { Gdiplus::GdiplusShutdown(app.gdiplusToken); return 1; }
    LoadCustomizations();
    if (app.gifImage) { delete app.gifImage; app.gifImage = nullptr; }
    if (app.gifTimerId) { KillTimer(app.hwnd, app.gifTimerId); app.gifTimerId = 0; }
    app.backgroundIsGif = false;
    if (!app.customBackground.empty()) {
        std::string ext = app.customBackground;
        auto dot = ext.find_last_of('.');
        if (dot != std::string::npos) {
            ext = ext.substr(dot);
            if (ext == ".gif" || ext == ".GIF") {
                app.backgroundIsGif = true;
                std::wstring wpath(app.customBackground.begin(), app.customBackground.end());
                app.gifImage = new Gdiplus::Image(wpath.c_str());
                if (app.gifImage->GetLastStatus() == Gdiplus::Ok) {
                    GUID pageGuid = Gdiplus::FrameDimensionTime;
                    app.gifFrameCount = app.gifImage->GetFrameCount(&pageGuid);
                    app.gifCurrentFrame = 0;
                    if (app.gifFrameCount > 1) {
                        UINT frameDelay = 100;
                        UINT size = app.gifImage->GetPropertyItemSize(PropertyTagFrameDelay);
                        if (size > 0) {
                            Gdiplus::PropertyItem* prop = (Gdiplus::PropertyItem*)malloc(size);
                            if (prop && app.gifImage->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
                                long* delays = (long*)prop->value;
                                if (app.gifFrameCount > 0 && delays[0] > 0) {
                                    frameDelay = delays[0] * 10;
                                    if (frameDelay < 16) frameDelay = 16;
                                }
                            }
                            free(prop);
                        }
                        app.gifTimerId = SetTimer(app.hwnd, 3001, frameDelay, nullptr);
                    }
                }
            }
        }
    }
    ShowWindow(app.hwnd, nCmdShow);
    UpdateWindow(app.hwnd);
    PostMessage(app.hwnd, WM_APP_REFRESH_FONTS, 0, 0);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    CleanupWASAPI();
    Gdiplus::GdiplusShutdown(app.gdiplusToken);
    return 0;
}
