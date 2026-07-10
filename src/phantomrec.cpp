// PhantomRec.cpp — PhantomRec v1.9.5 C++ UI
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Max'sEngine™ | Pure C Core + C++ UI

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <ctime>
#include <shlobj.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <fstream>
#include <powrprof.h>
#include <cstring>
 
using std::min;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "powrprof.lib")

extern "C" {
#include "phantomrec_core.h"
}

#define PHANTOMREC_VERSION "1.9.5"
#define ID_BTN_RECORD 1001
#define ID_BTN_SETTINGS 1002
#define ID_HOTKEY_RECORD 1
#define ID_HOTKEY_PAUSE 2
#define ID_TIMER_UPDATE 2001
#define ID_TIMER_INI_CHECK 2002
#define WM_APP_REFRESH_FONTS (WM_APP + 10)
#define ID_BTN_BROWSE_BG   1003
#define ID_BTN_BROWSE_FONT 1004
#define ID_BTN_APPLY       1005
#define ID_BTN_RESET_BG    1006
#define ID_EDIT_FONTSIZE   1007
#define ID_PREVIEW_BG      1008
#define ID_PREVIEW_FONT    1009
#define ID_BTN_COLOR       1010

// Custom messages for thread‑safe UI updates
#define WM_PR_STATUS     (WM_APP + 20)
#define WM_PR_BUTTON     (WM_APP + 21)
#define WM_PR_PROGRESS   (WM_APP + 22)
#define WM_PR_CONV_DONE  (WM_APP + 23)

// ============================================================================
// UI State
// ============================================================================
static PhantomRecCore g_Core;
static HWND g_hWnd = nullptr;
static HWND g_btnRecord = nullptr;
static HWND g_lblStatus = nullptr;
static HWND g_progressBar = nullptr;
static UINT g_recordHotkey = VK_F10;
static UINT g_pauseHotkey = 'P';
static HWND g_hSettingsWnd = nullptr;
static std::string g_iniPath;
static std::string g_customBackground;
static std::string g_customFont;
static int g_customFontSize = 14;
static COLORREF g_customColorRef = RGB(255, 255, 255);
static ULONG_PTR g_gdiplusToken = 0;
static bool g_backgroundIsGif = false;
static UINT_PTR g_gifTimerId = 0;
static Gdiplus::Image* g_gifImage = nullptr;
static UINT g_gifFrameCount = 0;
static UINT g_gifCurrentFrame = 0;
static std::string g_outputDir;
static std::string g_maxsenginePath;
static FILETIME g_iniLastWrite = {0};

// Thread‑safety: store UI thread ID
static DWORD g_mainThreadId = 0;

// Cached static background image (non‑GIF)
static Gdiplus::Image* g_cachedBgImage = nullptr;
static std::string g_cachedBgPath;

// Font handles for main window controls (for cleanup)
static HFONT g_hStatusFont = nullptr;
static HFONT g_hButtonFont = nullptr;

static void UpdateUI();
static void DoUpdateStatus(const char* message);
static void DoUpdateButton(const char* text);

// ============================================================================
// Helper: Read GIF frame delay from metadata
// ============================================================================
static UINT GetGifFrameDelay(Gdiplus::Image* image, UINT frameCount) {
    UINT frameDelay = 100; // default 100ms = 10 FPS
    UINT size = image->GetPropertyItemSize(PropertyTagFrameDelay);
    if (size > 0) {
        Gdiplus::PropertyItem* prop = (Gdiplus::PropertyItem*)malloc(size);
        if (prop && image->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Gdiplus::Ok) {
            long* delays = (long*)prop->value;
            if (frameCount > 0 && delays[0] > 0) {
                frameDelay = delays[0] * 10; // Convert 10ms units to ms
                if (frameDelay < 16) frameDelay = 16; // Minimum ~60 FPS
            }
        }
        free(prop);
    }
    return frameDelay;
}

// ============================================================================
// Thread‑safe core callbacks
// ============================================================================
static void OnStatusUpdate(const char* message) {
    if (GetCurrentThreadId() != g_mainThreadId) {
        // Post message to UI thread
        char* msgCopy = _strdup(message);
        PostMessageA(g_hWnd, WM_PR_STATUS, 0, (LPARAM)msgCopy);
    } else {
        DoUpdateStatus(message);
    }
}

static void OnButtonUpdate(const char* text) {
    if (GetCurrentThreadId() != g_mainThreadId) {
        char* txtCopy = _strdup(text);
        PostMessageA(g_hWnd, WM_PR_BUTTON, 0, (LPARAM)txtCopy);
    } else {
        DoUpdateButton(text);
    }
}

static void OnProgressUpdate(int percent) {
    if (GetCurrentThreadId() != g_mainThreadId) {
        PostMessageA(g_hWnd, WM_PR_PROGRESS, (WPARAM)percent, 0);
    } else {
        SendMessageA(g_progressBar, PBM_SETPOS, percent, 0);
        char buf[64];
        sprintf_s(buf, "Processing video... %d%%", percent);
        DoUpdateStatus(buf);
    }
}

static void OnConversionDone(int success, const char* filePath) {
    if (GetCurrentThreadId() != g_mainThreadId) {
        char* pathCopy = filePath ? _strdup(filePath) : nullptr;
        PostMessageA(g_hWnd, WM_PR_CONV_DONE, (WPARAM)success, (LPARAM)pathCopy);
    } else {
        SendMessageA(g_progressBar, PBM_SETPOS, 0, 0);
        if (success && filePath) {
            char buf[256];
            long long fs = Core_GetFileSize(filePath);
            Core_FormatSize(fs, buf, sizeof(buf));
            DoUpdateStatus(buf);
            ShellExecuteA(nullptr, "open", "explorer",
                ("/select,\"" + std::string(filePath) + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            DoUpdateStatus("Conversion failed");
        }
        DoUpdateButton("START");
    }
}

// Direct UI updaters (always called on UI thread)
static void DoUpdateStatus(const char* message) {
    if (g_lblStatus && IsWindow(g_lblStatus) && g_hWnd) {
        RECT rect;
        GetWindowRect(g_lblStatus, &rect);
        MapWindowPoints(HWND_DESKTOP, g_hWnd, (LPPOINT)&rect, 2);
        HDC hdc = GetDC(g_hWnd);
        HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0)); // default black background
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);
        ReleaseDC(g_hWnd, hdc);
        SetWindowTextA(g_lblStatus, message);
        InvalidateRect(g_hWnd, &rect, TRUE);
        UpdateWindow(g_hWnd);
    }
}

static void DoUpdateButton(const char* text) {
    if (g_btnRecord && IsWindow(g_btnRecord))
        SetWindowTextA(g_btnRecord, text);
}

// ============================================================================
// Helpers
// ============================================================================
static std::string GetExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    auto pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : ".";
}

static bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

static std::string GetVideosFolder() {
    char p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, p))) {
        std::string d = std::string(p) + "\\PhantomRec";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }
    return GetExeDir();
}

static UINT ParseHotkey(const std::string& key) {
    if (key == "F1")  return VK_F1;  if (key == "F2")  return VK_F2;
    if (key == "F3")  return VK_F3;  if (key == "F4")  return VK_F4;
    if (key == "F5")  return VK_F5;  if (key == "F6")  return VK_F6;
    if (key == "F7")  return VK_F7;  if (key == "F8")  return VK_F8;
    if (key == "F9")  return VK_F9;  if (key == "F10") return VK_F10;
    if (key == "F11") return VK_F11; if (key == "F12") return VK_F12;
    if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') return (UINT)key[0];
    if (key.length() == 1 && key[0] >= 'a' && key[0] <= 'z') return (UINT)(key[0] - 32);
    return 0;
}

static UINT GetHotkeyModifiers(UINT vk) {
    return (vk >= VK_F1 && vk <= VK_F12) ? MOD_NOREPEAT : (MOD_CONTROL | MOD_NOREPEAT);
}

static std::string GetHotkeyName(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z') return std::string("Ctrl+") + (char)vk;
    return "F10";
}

static void CreateDefaultIni() {
    std::ofstream ini(g_iniPath);
    ini << "; ========================================\r\n"
        << "; PhantomRec v1.9.5 Settings\r\n"
        << "; Made by MaxRBLX1\r\n"
        << "; Max'sEngine(tm) Powered by FFmpeg\r\n"
        << "; ========================================\r\n"
        << "; Stage 1: Ut Video lossless\r\n"
        << "; Stage 2: x264 Post-Convert (after recording)\r\n"
        << ";\r\n"
        << "; CaptureMethod: How PhantomRec captures your screen\r\n"
        << ";   auto    = PhantomRec picks the best method for your OS\r\n"
        << ";   ddagrab = DXGI Desktop Duplication (GPU, 60 FPS, Win8+)\r\n"
        << ";   gfx     = D3D11 Graphics Capture (GPU, 60 FPS, Win10+)\r\n"
        << ";   gdi     = CPU software capture (55 FPS, any Windows)\r\n"
        << ";\r\n"
        << "; Hotkey: F1-F12 for function keys\r\n"
        << ";         A-Z for Ctrl+Letter hotkeys (e.g., R = Ctrl+R)\r\n"
        << ";\r\n"
        << "; PauseHotkey: Same format as Hotkey\r\n"
        << ";\r\n"
        << "; ConvertAfterRecording: yes or no\r\n"
        << ";   yes = Automatically compress after recording (recommended)\r\n"
        << ";   no  = Keep the lossless temp file (very large)\r\n"
        << "; ========================================\r\n\r\n"
        << "[Settings]\r\n"
        << "Hotkey=F10\r\n"
        << "PauseHotkey=P\r\n"
        << "ConvertAfterRecording=yes\r\n"
        << "CaptureMethod=auto\r\n";
    ini.close();
}

static void LoadConfiguration() {
    char buf[32];
    GetPrivateProfileStringA("Settings", "Hotkey", "F10", buf, sizeof(buf), g_iniPath.c_str());
    g_recordHotkey = ParseHotkey(buf);
    if (g_recordHotkey == 0) g_recordHotkey = VK_F10;
    
    GetPrivateProfileStringA("Settings", "PauseHotkey", "P", buf, sizeof(buf), g_iniPath.c_str());
    g_pauseHotkey = ParseHotkey(buf);
    if (g_pauseHotkey == 0) g_pauseHotkey = 'P';
    
    GetPrivateProfileStringA("Settings", "ConvertAfterRecording", "yes", buf, sizeof(buf), g_iniPath.c_str());
    g_Core.convertAfterRecording = (strcmp(buf, "no") != 0);
    
    char capBuf[32];
    GetPrivateProfileStringA("Settings", "CaptureMethod", "auto", capBuf, sizeof(capBuf), g_iniPath.c_str());
    if (strcmp(capBuf, "ddagrab") == 0)
        Core_SetCaptureMethodEx(&g_Core, CAPTURE_DDAGRAB);
    else if (strcmp(capBuf, "gfx") == 0)
        Core_SetCaptureMethodEx(&g_Core, CAPTURE_GFX);
    else if (strcmp(capBuf, "gdi") == 0)
        Core_SetCaptureMethodEx(&g_Core, CAPTURE_GDI);
    else
        Core_SetCaptureMethodEx(&g_Core, CAPTURE_AUTO);
    
    HANDLE hFile = CreateFileA(g_iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        GetFileTime(hFile, nullptr, nullptr, &g_iniLastWrite);
        CloseHandle(hFile);
    }
}

static void ReloadIniIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &attr)) {
        if (CompareFileTime(&attr.ftLastWriteTime, &g_iniLastWrite) != 0) {
            g_iniLastWrite = attr.ftLastWriteTime;
            
            // --- Thread‑safety guard: never mutate capture method / conversion flag while recording
            bool wasRecording = Core_IsRecording(&g_Core);
            int oldMethod = g_Core.captureMethod;   // might not exist; we'll read via getter if available
            bool oldConvert = g_Core.convertAfterRecording;
            
            LoadConfiguration();   // modifies g_Core fields
            
            if (wasRecording) {
                // Revert unsafe changes
                g_Core.convertAfterRecording = oldConvert;
                Core_SetCaptureMethodEx(&g_Core, (CaptureMethod)oldMethod);
                // (If core stores method as enum, cast works; otherwise use the saved int)
            }
            
            // Hotkey rebinding is always safe
            UnregisterHotKey(g_hWnd, ID_HOTKEY_RECORD);
            UnregisterHotKey(g_hWnd, ID_HOTKEY_PAUSE);
            UINT recMod = GetHotkeyModifiers(g_recordHotkey);
            RegisterHotKey(g_hWnd, ID_HOTKEY_RECORD, recMod, g_recordHotkey);
            UINT pauseMod = GetHotkeyModifiers(g_pauseHotkey);
            RegisterHotKey(g_hWnd, ID_HOTKEY_PAUSE, pauseMod, g_pauseHotkey);
            UpdateUI();
        }
    }
}

// ============================================================================
// Customization – background caching
// ============================================================================
static void ClearCachedBackground() {
    if (g_cachedBgImage) {
        delete g_cachedBgImage;
        g_cachedBgImage = nullptr;
    }
    g_cachedBgPath.clear();
}

static void EnsureBackgroundCached() {
    if (g_customBackground.empty() || !FileExists(g_customBackground)) {
        ClearCachedBackground();
        return;
    }
    // Check if it's a GIF (handled separately)
    std::string ext = g_customBackground;
    auto dot = ext.find_last_of('.');
    if (dot != std::string::npos) {
        ext = ext.substr(dot);
        if (ext == ".gif" || ext == ".GIF") {
            ClearCachedBackground();
            return;
        }
    }
    if (g_cachedBgPath == g_customBackground && g_cachedBgImage)
        return;  // already cached
    ClearCachedBackground();
    std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
    g_cachedBgImage = new Gdiplus::Image(wpath.c_str());
    if (g_cachedBgImage->GetLastStatus() == Gdiplus::Ok) {
        g_cachedBgPath = g_customBackground;
    } else {
        delete g_cachedBgImage;
        g_cachedBgImage = nullptr;
    }
}

static void LoadCustomizations() {
    char buf[MAX_PATH] = {0};
    GetPrivateProfileStringA("Appearance", "Background", "", buf, sizeof(buf), g_iniPath.c_str());
    if (strlen(buf) > 0) {
        g_customBackground = buf;
        if (g_customBackground.find(":\\") == std::string::npos)
            g_customBackground = GetExeDir() + "\\" + g_customBackground;
    }
    memset(buf, 0, sizeof(buf));
    GetPrivateProfileStringA("Appearance", "Font", "", buf, sizeof(buf), g_iniPath.c_str());
    if (strlen(buf) > 0) {
        g_customFont = buf;
        if (g_customFont.find(":\\") == std::string::npos)
            g_customFont = GetExeDir() + "\\" + g_customFont;
        if (FileExists(g_customFont))
            AddFontResourceExA(g_customFont.c_str(), FR_PRIVATE, 0);
    }
    g_customFontSize = GetPrivateProfileIntA("Appearance", "FontSize", 14, g_iniPath.c_str());
    g_customColorRef = (COLORREF)GetPrivateProfileIntA("Appearance", "FontColor", RGB(255, 255, 255), g_iniPath.c_str());
}

static void SaveCustomizations() {
    WritePrivateProfileStringA("Appearance", "Background", g_customBackground.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "Font", g_customFont.c_str(), g_iniPath.c_str());
    char buf[16];
    sprintf_s(buf, "%d", g_customFontSize);
    WritePrivateProfileStringA("Appearance", "FontSize", buf, g_iniPath.c_str());
    sprintf_s(buf, "%d", (int)g_customColorRef);
    WritePrivateProfileStringA("Appearance", "FontColor", buf, g_iniPath.c_str());
}

// ============================================================================
// Settings Window (identical to v1.9, with fixes)
// ============================================================================
static void ApplyBackground(HWND previewWnd, const std::string& path) {
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

static void ApplyFont(HWND previewWnd, const std::string& path, int size) {
    if (!FileExists(path)) return;
    AddFontResourceExA(path.c_str(), FR_PRIVATE, 0);
    std::string fileName = path;
    auto pos = fileName.find_last_of("\\/");
    if (pos != std::string::npos) fileName = fileName.substr(pos + 1);
    std::string faceName;
    HDC hdc = GetDC(previewWnd);
    LOGFONTA lf = {0}; lf.lfCharSet = DEFAULT_CHARSET;
    strncpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), fileName.c_str(), _TRUNCATE);
    HFONT testFont = CreateFontA(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, fileName.c_str());
    if (testFont) { SelectObject(hdc, testFont); GetTextFaceA(hdc, LF_FACESIZE, lf.lfFaceName); faceName = lf.lfFaceName; DeleteObject(testFont); }
    ReleaseDC(previewWnd, hdc);
    if (faceName.empty()) { faceName = fileName; pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos); }
    HFONT hFont = CreateFontA(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
    if (hFont) {
        HFONT hOldFont = (HFONT)SendMessageA(previewWnd, WM_GETFONT, 0, 0);
        SendMessageA(previewWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hOldFont && hOldFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(hOldFont);
        InvalidateRect(previewWnd, nullptr, TRUE);
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HWND previewBg, previewFont, editFontSize;
    static std::string selectedBg, selectedFont;
    static int selectedFontSize;
    static COLORREF selectedColor;        // Fix 3: temp color until Apply
    static HFONT hPreviewFontHandle = nullptr;  // Fix 1: declared here, cleaned in WM_DESTROY

    switch (m) {
    case WM_CREATE: {
        selectedBg = g_customBackground;
        selectedFont = g_customFont;
        selectedFontSize = g_customFontSize;
        selectedColor = g_customColorRef;
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
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == previewFont) {
            SetTextColor((HDC)w, selectedColor);   // use temp color
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        return DefWindowProcA(h, m, w, l);
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
                auto pos = faceName.find_last_of("\\/"); if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                if (hPreviewFontHandle) DeleteObject(hPreviewFontHandle);
                hPreviewFontHandle = CreateFontA(selectedFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                if (hPreviewFontHandle) SendMessageA(previewFont, WM_SETFONT, (WPARAM)hPreviewFontHandle, TRUE);
                InvalidateRect(previewFont, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_COLOR) {
            CHOOSECOLORA cc = { sizeof(cc) };
            cc.hwndOwner = h; cc.rgbResult = selectedColor;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            static COLORREF acrCustClr[16] = {0}; cc.lpCustColors = acrCustClr;
            if (ChooseColorA(&cc)) {
                selectedColor = cc.rgbResult;
                InvalidateRect(previewFont, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_APPLY) {
            char sizeBuf[8]; GetWindowTextA(editFontSize, sizeBuf, sizeof(sizeBuf));
            int newSize = atoi(sizeBuf);
            if (newSize >= 8 && newSize <= 72) selectedFontSize = newSize;
            g_customBackground = selectedBg;
            g_customFont = selectedFont;
            g_customFontSize = selectedFontSize;
            g_customColorRef = selectedColor;   // Fix 3: apply temp color
            SaveCustomizations();
            // Reload background cache
            if (g_gifImage) { delete g_gifImage; g_gifImage = nullptr; }
            if (g_gifTimerId) { KillTimer(g_hWnd, g_gifTimerId); g_gifTimerId = 0; }
            g_backgroundIsGif = false;
            ClearCachedBackground();
            if (!g_customBackground.empty()) {
                std::string ext = g_customBackground;
                auto dot = ext.find_last_of('.');
                if (dot != std::string::npos) {
                    ext = ext.substr(dot);
                    if (ext == ".gif" || ext == ".GIF") {
                        g_backgroundIsGif = true;
                        std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
                        g_gifImage = new Gdiplus::Image(wpath.c_str());
                        if (g_gifImage->GetLastStatus() == Gdiplus::Ok) {
                            GUID pageGuid = Gdiplus::FrameDimensionTime;
                            g_gifFrameCount = g_gifImage->GetFrameCount(&pageGuid);
                            if (g_gifFrameCount > 1) {
                                UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                                g_gifTimerId = SetTimer(g_hWnd, 3001, frameDelay, nullptr);
                            }
                        }
                    }
                }
            }
            EnsureBackgroundCached(); // load static cache if needed
            InvalidateRect(g_hWnd, nullptr, TRUE);
            UpdateWindow(g_hWnd);
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); }
        return 0;
    }
    case WM_DESTROY:
        // Fix 1: clean up preview font handle
        if (hPreviewFontHandle) {
            DeleteObject(hPreviewFontHandle);
            hPreviewFontHandle = nullptr;
        }
        g_hSettingsWnd = nullptr;
        return 0;
    case WM_NCDESTROY:
        g_hSettingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void OpenSettingsWindow() {
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) { SetForegroundWindow(g_hSettingsWnd); return; }
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "PhantomRecSettings";
    RegisterClassExA(&wc);
    g_hSettingsWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        "PhantomRecSettings", "Customization",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (GetSystemMetrics(SM_CXSCREEN) - 640) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 480) / 2,
        640, 480, g_hWnd, nullptr, GetModuleHandle(nullptr), nullptr);
}

// ============================================================================
// UI Update
// ============================================================================
static void UpdateUI() {
    std::string hotkey = GetHotkeyName(g_recordHotkey);
    std::string pauseKey = GetHotkeyName(g_pauseHotkey);

    if (Core_IsConverting(&g_Core)) {
        DoUpdateButton("Processing...");
    } else if (Core_IsRecording(&g_Core) && Core_IsPaused(&g_Core)) {
        DoUpdateButton(("RESUME (" + pauseKey + ")").c_str());
        DoUpdateStatus("PAUSED");
    } else if (Core_IsRecording(&g_Core)) {
        DoUpdateButton(("STOP (" + hotkey + ")").c_str());
        DoUpdateStatus("Recording...");
    } else {
        DoUpdateButton(("START (" + hotkey + ")").c_str());
        std::string s = "Ready - " + hotkey + " to record\r\n";
        s += "Pause: " + pauseKey + "\r\n\r\n";
        s += "Cores: " + std::to_string(g_Core.cpuCoreCount);
        s += " | Threads: " + std::to_string(g_Core.dynamicThreads);
        s += "\r\nTarget CRF: " + std::to_string(g_Core.crf);
        s += "\r\n" + std::to_string(g_Core.screenWidth) + "x" + std::to_string(g_Core.screenHeight);
        s += "\r\nMax'sEngine(tm) Powered by FFmpeg\r\nBuilt by MaxRBLX1";
        DoUpdateStatus(s.c_str());
    }
}

// ============================================================================
// Window Procedure
// ============================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PR_STATUS: {
        char* msg = (char*)l;
        if (msg) {
            DoUpdateStatus(msg);
            free(msg);
        }
        return 0;
    }
    case WM_PR_BUTTON: {
        char* txt = (char*)l;
        if (txt) {
            DoUpdateButton(txt);
            free(txt);
        }
        return 0;
    }
    case WM_PR_PROGRESS: {
        SendMessageA(g_progressBar, PBM_SETPOS, (int)w, 0);
        char buf[64];
        sprintf_s(buf, "Processing video... %d%%", (int)w);
        DoUpdateStatus(buf);
        return 0;
    }
    case WM_PR_CONV_DONE: {
        SendMessageA(g_progressBar, PBM_SETPOS, 0, 0);
        int success = (int)w;
        char* filePath = (char*)l;
        if (success && filePath) {
            char buf[256];
            long long fs = Core_GetFileSize(filePath);
            Core_FormatSize(fs, buf, sizeof(buf));
            DoUpdateStatus(buf);
            ShellExecuteA(nullptr, "open", "explorer",
                ("/select,\"" + std::string(filePath) + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            DoUpdateStatus("Conversion failed");
        }
        DoUpdateButton("START");
        if (filePath) free(filePath);
        return 0;
    }
    case WM_APP_REFRESH_FONTS: {
        if (!g_customFont.empty() && FileExists(g_customFont)) {
            AddFontResourceExA(g_customFont.c_str(), FR_PRIVATE, 0);
            std::string faceName = g_customFont;
            auto pos = faceName.find_last_of("\\/");
            if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
            pos = faceName.find_last_of('.');
            if (pos != std::string::npos) faceName = faceName.substr(0, pos);
            HFONT hFreshFont = CreateFontA(g_customFontSize, 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
            if (hFreshFont) {
                if (g_lblStatus && IsWindow(g_lblStatus)) {
                    HFONT oldFont = (HFONT)SendMessageA(g_lblStatus, WM_SETFONT, (WPARAM)hFreshFont, TRUE);
                    if (g_hStatusFont && g_hStatusFont != oldFont && oldFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
                        DeleteObject(g_hStatusFont);
                    g_hStatusFont = hFreshFont;
                }
                if (g_btnRecord && IsWindow(g_btnRecord)) {
                    HFONT oldFont = (HFONT)SendMessageA(g_btnRecord, WM_SETFONT, (WPARAM)hFreshFont, TRUE);
                    if (g_hButtonFont && g_hButtonFont != oldFont && oldFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
                        DeleteObject(g_hButtonFont);
                    g_hButtonFont = hFreshFont;
                }
            }
        }
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);
        UpdateUI();
        return 0;
    }
    case WM_CREATE: {
        std::string startLabel = "START (" + GetHotkeyName(g_recordHotkey) + ")";
        g_btnRecord = CreateWindowA("BUTTON", startLabel.c_str(),
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 20, 340, 50, h, (HMENU)ID_BTN_RECORD, nullptr, nullptr);
        CreateWindowA("BUTTON", "", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 360, 5, 30, 30, h, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
        g_lblStatus = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_LEFT, 20, 120, 340, 130, h, nullptr, nullptr, nullptr);
        g_progressBar = CreateWindowA("msctls_progress32", "", WS_VISIBLE | WS_CHILD | PBS_MARQUEE, 20, 260, 340, 15, h, nullptr, nullptr, nullptr);
        UINT recMod = GetHotkeyModifiers(g_recordHotkey); RegisterHotKey(h, ID_HOTKEY_RECORD, recMod, g_recordHotkey);
        UINT pauseMod = GetHotkeyModifiers(g_pauseHotkey); RegisterHotKey(h, ID_HOTKEY_PAUSE, pauseMod, g_pauseHotkey);
        SetTimer(h, ID_TIMER_INI_CHECK, 2000, nullptr);
        LoadCustomizations();
        EnsureBackgroundCached();
        UpdateUI();
        if (!g_customBackground.empty() && FileExists(g_customBackground)) {
            std::string ext = g_customBackground;
            auto dot = ext.find_last_of('.');
            if (dot != std::string::npos) {
                ext = ext.substr(dot);
                if (ext == ".gif" || ext == ".GIF") {
                    g_backgroundIsGif = true;
                    std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
                    g_gifImage = new Gdiplus::Image(wpath.c_str());
                    if (g_gifImage->GetLastStatus() == Gdiplus::Ok) {
                        GUID pageGuid = Gdiplus::FrameDimensionTime;
                        g_gifFrameCount = g_gifImage->GetFrameCount(&pageGuid);
                        if (g_gifFrameCount > 1) {
                            UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                            g_gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
                        }
                    }
                }
            }
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)w;
        static HFONT hCurrentFont = nullptr;
        static std::string lastFontName = "";
        static int lastFontSize = 0;
        if (!g_customFont.empty()) {
            if (g_customFont != lastFontName || g_customFontSize != lastFontSize) {
                if (hCurrentFont) { SelectObject(hdcStatic, (HFONT)GetStockObject(SYSTEM_FONT)); DeleteObject(hCurrentFont); hCurrentFont = nullptr; }
                std::string faceName = g_customFont;
                auto pos = faceName.find_last_of("\\/"); if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hCurrentFont = CreateFontA(g_customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastFontName = g_customFont; lastFontSize = g_customFontSize;
            }
            if (hCurrentFont) SelectObject(hdcStatic, hCurrentFont);
        }
        SetBkMode(hdcStatic, TRANSPARENT);
        SetTextColor(hdcStatic, g_customColorRef);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLORBTN: {
        HDC hdcBtn = (HDC)w;
        static HFONT hBtnFont = nullptr;
        static std::string lastBtnFontName = "";
        static int lastBtnFontSize = 0;
        if (!g_customFont.empty()) {
            if (g_customFont != lastBtnFontName || g_customFontSize != lastBtnFontSize) {
                if (hBtnFont) DeleteObject(hBtnFont);
                std::string faceName = g_customFont;
                auto pos = faceName.find_last_of("\\/"); if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.'); if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hBtnFont = CreateFontA(g_customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastBtnFontName = g_customFont; lastBtnFontSize = g_customFontSize;
            }
            if (hBtnFont) SelectObject(hdcBtn, hBtnFont);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rect;
        GetClientRect(h, &rect);
        
        if (g_backgroundIsGif && g_gifImage) {
            Gdiplus::Graphics graphics(hdc);
            float imgW = (float)g_gifImage->GetWidth();
            float imgH = (float)g_gifImage->GetHeight();
            float winW = (float)(rect.right - rect.left);
            float winH = (float)(rect.bottom - rect.top);
            float scale = min(winW / imgW, winH / imgH);
            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float drawX = (winW - drawW) / 2.0f;
            float drawY = (winH - drawH) / 2.0f;
            Gdiplus::SolidBrush blackBrush(Gdiplus::Color(0, 0, 0));
            graphics.FillRectangle(&blackBrush, 0, 0, (int)winW, (int)winH);
            graphics.DrawImage(g_gifImage, (int)drawX, (int)drawY, (int)drawW, (int)drawH);
        } else if (g_cachedBgImage) {
            Gdiplus::Graphics graphics(hdc);
            float imgW = (float)g_cachedBgImage->GetWidth();
            float imgH = (float)g_cachedBgImage->GetHeight();
            float winW = (float)(rect.right - rect.left);
            float winH = (float)(rect.bottom - rect.top);
            float scale = min(winW / imgW, winH / imgH);
            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float drawX = (winW - drawW) / 2.0f;
            float drawY = (winH - drawH) / 2.0f;
            Gdiplus::SolidBrush blackBrush(Gdiplus::Color(0, 0, 0));
            graphics.FillRectangle(&blackBrush, 0, 0, (int)winW, (int)winH);
            graphics.DrawImage(g_cachedBgImage, (int)drawX, (int)drawY, (int)drawW, (int)drawH);
        } else {
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_RECORD) {
            if (Core_IsRecording(&g_Core))
                Core_StopRecording(&g_Core);
            else
                Core_StartRecording(&g_Core);
            UpdateUI();
        } else if (LOWORD(w) == ID_BTN_SETTINGS) {
            OpenSettingsWindow();
            InvalidateRect(h, nullptr, TRUE);
        }
        return 0;
    case WM_HOTKEY:
        if (w == ID_HOTKEY_RECORD) {
            if (Core_IsRecording(&g_Core))
                Core_StopRecording(&g_Core);
            else
                Core_StartRecording(&g_Core);
            UpdateUI();
        } else if (w == ID_HOTKEY_PAUSE) {
            Core_TogglePause(&g_Core);
            UpdateUI();
        }
        return 0;
    case WM_TIMER:
        if (w == ID_TIMER_UPDATE) UpdateUI();
        else if (w == ID_TIMER_INI_CHECK) ReloadIniIfChanged();
        else if (w == 3001 && g_gifImage && g_gifFrameCount > 1) {
            g_gifCurrentFrame = (g_gifCurrentFrame + 1) % g_gifFrameCount;
            GUID pageGuid = Gdiplus::FrameDimensionTime;
            g_gifImage->SelectActiveFrame(&pageGuid, g_gifCurrentFrame);
            InvalidateRect(h, nullptr, TRUE);
            UpdateWindow(h);
        }
        return 0;
    case WM_SYSCOMMAND: {
        UINT sysCmd = (w & 0xFFF0);
        if (sysCmd == SC_MINIMIZE) { if (g_gifTimerId) { KillTimer(h, g_gifTimerId); g_gifTimerId = 0; } }
        else if (sysCmd == SC_RESTORE) {
            if (g_backgroundIsGif && g_gifImage && g_gifFrameCount > 1 && !g_gifTimerId) {
                UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                g_gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
            }
            InvalidateRect(h, nullptr, TRUE);
        }
        return DefWindowProcA(h, m, w, l);
    }
    case WM_DESTROY: {
        // Fix 5: clean up font handles
        if (g_hStatusFont) DeleteObject(g_hStatusFont);
        if (g_hButtonFont) DeleteObject(g_hButtonFont);
        // Also the static cached fonts in WM_CTLCOLOR* can be cleaned (they are static but we can delete on exit)
        // However, those are local statics; we can simply not delete them (OS reclaims on process exit). That's acceptable.
        
        if (Core_IsRecording(&g_Core)) Core_StopRecording(&g_Core);
        if (g_gifImage) { delete g_gifImage; g_gifImage = nullptr; }
        if (g_gifTimerId) KillTimer(h, g_gifTimerId);
        ClearCachedBackground();
        UnregisterHotKey(h, ID_HOTKEY_RECORD);
        UnregisterHotKey(h, ID_HOTKEY_PAUSE);
        KillTimer(h, ID_TIMER_INI_CHECK);
        if (g_Core.powerPlanChanged) {
            PowerSetActiveScheme(NULL, &g_Core.originalPowerPlan);
            g_Core.powerPlanChanged = 0;
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

// ============================================================================
// Entry point
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    g_mainThreadId = GetCurrentThreadId();   // store UI thread
    
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
    
    // LOCK HIGH PERFORMANCE AT LAUNCH
    {
        GUID highPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        GUID* pOriginalGuid = NULL;
        if (PowerGetActiveScheme(NULL, &pOriginalGuid) == ERROR_SUCCESS) {
            g_Core.originalPowerPlan = *pOriginalGuid;
            LocalFree(pOriginalGuid);
            PowerSetActiveScheme(NULL, &highPerf);
            g_Core.powerPlanChanged = 1;
        }
    }
    
    g_outputDir = GetVideosFolder();
    g_iniPath = GetExeDir() + "\\Settings.ini";
    
    Core_Init(&g_Core, nullptr, g_outputDir.c_str());
    g_Core.onStatusUpdate = OnStatusUpdate;
    g_Core.onButtonUpdate = OnButtonUpdate;
    g_Core.onProgressUpdate = OnProgressUpdate;
    g_Core.onConversionDone = OnConversionDone;
    
    if (!Core_FindMaxsEngine(&g_Core)) {
        MessageBoxA(nullptr,
            "maxsengine.exe not found!\n\n"
            "Place maxsengine.exe (FFmpeg renamed) next to PhantomRec.exe\n"
            "or keep ffmpeg.exe for backward compatibility.\n\n"
            "Max'sEngine(tm) Powered by FFmpeg — ffmpeg.org",
            "PhantomRec v" PHANTOMREC_VERSION, MB_OK);
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 0;
    }
    
    Core_CleanupOrphanedTempFiles(&g_Core);
    
    if (!FileExists(g_iniPath)) {
        CreateDefaultIni();
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath.c_str());
    }
    
    LoadConfiguration();
    Core_DetectResolution(&g_Core);
    Core_ConfigurePipeline(&g_Core);
    Core_SetCaptureMethod(&g_Core);
    Core_WarmEngine(&g_Core);
    
    Sleep(500);
    MessageBeep(MB_ICONINFORMATION);
    
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = "PhantomRecWnd";
    RegisterClassExA(&wc);
    
    int w = 395, h = 340;
    g_hWnd = CreateWindowExA(WS_EX_TOPMOST | 0x02000000L, "PhantomRecWnd",
        "PhantomRec v" PHANTOMREC_VERSION " — Max'sEngine(tm)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        w, h, nullptr, nullptr, hInst, nullptr);
    
    if (!g_hWnd) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        return 1;
    }
    
    LoadCustomizations();
    EnsureBackgroundCached();
    
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    PostMessageA(g_hWnd, WM_APP_REFRESH_FONTS, 0, 0);
    
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return 0;
}
