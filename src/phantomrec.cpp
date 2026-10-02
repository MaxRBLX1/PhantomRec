// PhantomRec.cpp — PhantomRec v1.9.9 C++ UI
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Max'sEngine™ | Pure C Core + C++ UI
// v1.9.9: removed process affinity, universal 60fps Stage 2, tray notification.

#include "phantomrec_core.h"

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <ctime>
#include <shlobj.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <cstring>
#include "resource.h"

using std::min;
using Microsoft::WRL::ComPtr;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

#define PHANTOMREC_VERSION "1.9.9"
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
#define ID_BTN_STROKE_COLOR 1011

// Custom messages for thread-safe UI updates
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
static HWND g_btnSettings = nullptr;
static HWND g_progressBar = nullptr;
static UINT g_recordHotkey = VK_F10;
static UINT g_pauseHotkey = 'P';
static HWND g_hSettingsWnd = nullptr;
static std::string g_iniPath;
static std::string g_customBackground;
static std::string g_customFont;
static int g_customFontSize = 14;
static COLORREF g_customColorRef = RGB(255, 255, 255);
static COLORREF g_customStrokeColorRef = RGB(0, 0, 0);
static int g_customStrokeWidth = 2;
static ULONG_PTR g_gdiplusToken = 0;
static bool g_backgroundIsAnimated = false;
static bool g_backgroundIsGif = false;
static UINT_PTR g_gifTimerId = 0;
static Gdiplus::Image* g_gifImage = nullptr;
static UINT g_gifFrameCount = 0;
static UINT g_gifCurrentFrame = 0;
static std::string g_outputDir;
static FILETIME g_iniLastWrite = {0};

static DWORD g_mainThreadId = 0;

// Status text for GDI+ drawing
static std::string g_statusText = "Ready - F10 to record";

// Cached static background image (non-GIF)
static Gdiplus::Image* g_cachedBgImage = nullptr;
static std::string g_cachedBgPath;

// Font handles for main window controls
static HFONT g_hButtonFont = nullptr;

using namespace Gdiplus;

// ============================================================================
// GIF Preview State for Settings Window
// ============================================================================
struct GifPreviewState {
    Image* gifImage;
    UINT frameCount;
    UINT currentFrame;
    UINT_PTR timerId;
    UINT frameDelay;
    bool isGif;
};

// WIC animated background state
struct WicAnimatedBg {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> currentFrame;
    UINT frameCount = 0;
    UINT currentIndex = 0;
    UINT_PTR timerId = 0;
    UINT frameDelay = 100;
    bool isAnimated = false;
};
static WicAnimatedBg g_wicBg;

static void UpdateUI();
static void DoUpdateStatus(const char* message);
static void DoUpdateButton(const char* text);
static void ShowRecordingSavedNotification(const char* filePath);
// ============================================================================
// Helper: Draw text with stroke
// ============================================================================
static void DrawTextWithStroke(Graphics* graphics, const WCHAR* text,
                                const Font* font, const RectF& layoutRect,
                                const Color& textColor, const Color& strokeColor,
                                REAL strokeWidth) {
    GraphicsPath path;
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    FontFamily fontFamily;
    font->GetFamily(&fontFamily);

    path.AddString(text, -1, &fontFamily, font->GetStyle(),
                   font->GetSize(), layoutRect, &format);

    Pen strokePen(strokeColor, strokeWidth);
    strokePen.SetLineJoin(LineJoinRound);
    graphics->DrawPath(&strokePen, &path);

    SolidBrush textBrush(textColor);
    graphics->FillPath(&textBrush, &path);
}

// ============================================================================
// Helper: Read GIF frame delay
// ============================================================================
static UINT GetGifFrameDelay(Image* image, UINT frameCount) {
    UINT frameDelay = 100;
    UINT size = image->GetPropertyItemSize(PropertyTagFrameDelay);
    if (size > 0) {
        PropertyItem* prop = (PropertyItem*)malloc(size);
        if (prop && image->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Ok) {
            long* delays = (long*)prop->value;
            if (frameCount > 0 && delays[0] > 0) {
                frameDelay = delays[0] * 10;
                if (frameDelay < 16) frameDelay = 16;
            }
        }
        free(prop);
    }
    return frameDelay;
}

// ============================================================================
// Thread-safe core callbacks
// ============================================================================
static void OnStatusUpdate(const char* message) {
    if (GetCurrentThreadId() != g_mainThreadId) {
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
        ShowWindow(g_progressBar, SW_SHOW);
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
        ShowWindow(g_progressBar, SW_HIDE);
        if (success && filePath) {
            char buf[256];
            long long fs = Core_GetFileSize(filePath);
            Core_FormatSize(fs, buf, sizeof(buf));
            DoUpdateStatus(buf);
			ShowRecordingSavedNotification(filePath);
            ShellExecuteA(nullptr, "open", "explorer",
                ("/select,\"" + std::string(filePath) + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            DoUpdateStatus("Conversion failed");
        }
        DoUpdateButton("START");
    }
}

// ============================================================================
// Direct UI updaters
// ============================================================================
static void DoUpdateStatus(const char* message) {
    if (message) {
        g_statusText = message;
    }
    if (g_hWnd && IsWindow(g_hWnd)) {
        InvalidateRect(g_hWnd, nullptr, TRUE);
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
    ini << "; ============================================================\r\n"
        << "; PhantomRec v1.9.9 Settings\r\n"
        << "; Built by MaxRBLX1\r\n"
        << "; Max'sEngine(tm) Powered by FFmpeg\r\n"
        << "; ============================================================\r\n"
        << ";\r\n"
        << "; PhantomRec records in two stages, both automatic:\r\n"
        << ";\r\n"
        << ";   Stage 1 - LIVE CAPTURE (while you record)\r\n"
        << ";             MaxRBLX1's Fastest MJPEG.\r\n"
        << ";             Runs on one CPU core. Never touches your GPU encoder.\r\n"
        << ";\r\n"
        << ";   Stage 2 - POST-CONVERT (after you stop)\r\n"
        << ";             x264 ultrafast.\r\n"
        << ";             Compresses the master into a small final .mkv.\r\n"
        << ";\r\n"
        << "; There is no encoder or quality setting to change. Both stages\r\n"
        << "; pick the best options for your hardware automatically. If you\r\n"
        << "; are looking for a knob to turn, there isn't one, and that is\r\n"
        << "; deliberate.\r\n"
        << ";\r\n"
        << "; ============================================================\r\n"
        << "; Hotkey - start or stop a recording\r\n"
        << "; ------------------------------------------------------------\r\n"
        << ";   F1 - F12       Function keys\r\n"
        << ";   A - Z           Ctrl + letter (e.g. R = Ctrl+R)\r\n"
        << ";\r\n"
        << "; PauseHotkey - pause or resume while recording\r\n"
        << ";   Same format as Hotkey.\r\n"
        << ";\r\n"
        << "; ConvertAfterRecording - run Stage 2 when recording stops\r\n"
        << ";   yes   Compress into a small final .mkv  (recommended)\r\n"
        << ";   no    Keep the raw master file          (very large)\r\n"
        << ";\r\n"
        << "; CaptureMethod - how PhantomRec reads your screen\r\n"
        << ";   auto      Pick the best method for your Windows version\r\n"
        << ";   gfx       D3D11 Graphics Capture      (Win10+, 60 FPS)\r\n"
        << ";   ddagrab   DXGI Desktop Duplication    (Win8+, 60 FPS)\r\n"
        << ";   gdi       GDI software capture        (any Windows, up to 30 FPS)\r\n"
        << "; ============================================================\r\n"
        << "\r\n"
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
            bool wasRecording = Core_IsRecording(&g_Core);
            int oldMethod = g_Core.captureMethod;
            bool oldConvert = g_Core.convertAfterRecording;
            LoadConfiguration();
            if (wasRecording) {
                g_Core.convertAfterRecording = oldConvert;
                Core_SetCaptureMethodEx(&g_Core, (CaptureMethod)oldMethod);
            }
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
    std::string ext = g_customBackground;
    auto dot = ext.find_last_of('.');
    if (dot != std::string::npos) {
        ext = ext.substr(dot);
        if (ext == ".gif" || ext == ".GIF" || ext == ".webp" || ext == ".WEBP") {
            ClearCachedBackground();
            return;
        }
    }
    if (g_cachedBgPath == g_customBackground && g_cachedBgImage)
        return;
    ClearCachedBackground();
    std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
    g_cachedBgImage = new Image(wpath.c_str());
    if (g_cachedBgImage->GetLastStatus() == Ok) {
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
    g_customStrokeColorRef = (COLORREF)GetPrivateProfileIntA("Appearance", "StrokeColor", RGB(0, 0, 0), g_iniPath.c_str());
    g_customStrokeWidth = GetPrivateProfileIntA("Appearance", "StrokeWidth", 2, g_iniPath.c_str());
    if (g_customStrokeWidth < 0) g_customStrokeWidth = 0;
    if (g_customStrokeWidth > 10) g_customStrokeWidth = 10;
}

static void SaveCustomizations() {
    WritePrivateProfileStringA("Appearance", "Background", g_customBackground.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringA("Appearance", "Font", g_customFont.c_str(), g_iniPath.c_str());
    char buf[16];
    sprintf_s(buf, "%d", g_customFontSize);
    WritePrivateProfileStringA("Appearance", "FontSize", buf, g_iniPath.c_str());
    sprintf_s(buf, "%d", (int)g_customColorRef);
    WritePrivateProfileStringA("Appearance", "FontColor", buf, g_iniPath.c_str());
    sprintf_s(buf, "%d", (int)g_customStrokeColorRef);
    WritePrivateProfileStringA("Appearance", "StrokeColor", buf, g_iniPath.c_str());
    sprintf_s(buf, "%d", g_customStrokeWidth);
    WritePrivateProfileStringA("Appearance", "StrokeWidth", buf, g_iniPath.c_str());
}

// ============================================================================
// WIC Animated Background
// ============================================================================
static bool InitWicFactory() {
    if (!g_wicBg.factory) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wicBg.factory));
        return SUCCEEDED(hr);
    }
    return true;
}

static bool LoadWicAnimatedBackground(const std::string& path) {
    if (!InitWicFactory()) return false;
    if (g_wicBg.timerId) { KillTimer(g_hWnd, g_wicBg.timerId); g_wicBg.timerId = 0; }
    g_wicBg.decoder.Reset();
    g_wicBg.currentFrame.Reset();
    std::wstring wpath(path.begin(), path.end());
    HRESULT hr = g_wicBg.factory->CreateDecoderFromFilename(wpath.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnDemand, &g_wicBg.decoder);
    if (SUCCEEDED(hr)) {
        hr = g_wicBg.decoder->GetFrameCount(&g_wicBg.frameCount);
        if (SUCCEEDED(hr) && g_wicBg.frameCount > 1) {
            g_wicBg.isAnimated = true;
            g_wicBg.currentIndex = 0;
            g_wicBg.decoder->GetFrame(0, &g_wicBg.currentFrame);
            g_wicBg.frameDelay = 100;
            g_wicBg.timerId = SetTimer(g_hWnd, 3003, g_wicBg.frameDelay, nullptr);
            return true;
        } else {
            g_wicBg.isAnimated = false;
            g_wicBg.frameCount = 1;
            g_wicBg.decoder->GetFrame(0, &g_wicBg.currentFrame);
        }
    }
    return false;
}

static void DrawWicFrame(HDC hdc, const RECT& rect) {
    if (!g_wicBg.currentFrame) return;
    UINT width = 0, height = 0;
    g_wicBg.currentFrame->GetSize(&width, &height);
    HDC memDC = CreateCompatibleDC(hdc);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -((int)height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hBitmap && bits) {
        UINT stride = width * 4;
        g_wicBg.currentFrame->CopyPixels(nullptr, stride, stride * height, (BYTE*)bits);
        HGDIOBJ oldBitmap = SelectObject(memDC, hBitmap);
        int rectWidth = rect.right - rect.left;
        int rectHeight = rect.bottom - rect.top;
        float scale = min((float)rectWidth / width, (float)rectHeight / height);
        int drawW = (int)(width * scale), drawH = (int)(height * scale);
        int drawX = (rectWidth - drawW) / 2, drawY = (rectHeight - drawH) / 2;
        SetStretchBltMode(hdc, HALFTONE);
        StretchBlt(hdc, drawX, drawY, drawW, drawH, memDC, 0, 0, width, height, SRCCOPY);
        SelectObject(memDC, oldBitmap);
        DeleteObject(hBitmap);
    }
    DeleteDC(memDC);
}

// ============================================================================
// GIF Animation Helper for Settings Preview
// ============================================================================
static void StartGifAnimation(HWND hWnd, HWND previewWnd, const std::string& path, GifPreviewState* state) {
    if (state->timerId) {
        KillTimer(hWnd, state->timerId);
        state->timerId = 0;
    }
    if (state->gifImage) {
        delete state->gifImage;
        state->gifImage = nullptr;
    }

    std::wstring wpath(path.begin(), path.end());
    state->gifImage = new Image(wpath.c_str());

    if (state->gifImage->GetLastStatus() == Ok) {
        GUID pageGuid = FrameDimensionTime;
        state->frameCount = state->gifImage->GetFrameCount(&pageGuid);
        state->isGif = true;
        state->currentFrame = 0;

        if (state->frameCount > 1) {
            UINT size = state->gifImage->GetPropertyItemSize(PropertyTagFrameDelay);
            if (size > 0) {
                PropertyItem* prop = (PropertyItem*)malloc(size);
                if (prop && state->gifImage->GetPropertyItem(PropertyTagFrameDelay, size, prop) == Ok) {
                    long* delays = (long*)prop->value;
                    if (delays[0] > 0) {
                        state->frameDelay = delays[0] * 10;
                        if (state->frameDelay < 16) state->frameDelay = 16;
                    }
                }
                free(prop);
            }

            state->timerId = SetTimer(hWnd, 3002, state->frameDelay, nullptr);
        }
    }
}

// ============================================================================
// Helper: Apply background with aspect ratio preservation
// ============================================================================
static void ApplyBackground(HWND previewWnd, const std::string& path) {
    if (!FileExists(path)) return;
    InvalidateRect(previewWnd, nullptr, TRUE);
}

// ============================================================================
// Helper: Apply font with stroke support
// ============================================================================
static void ApplyFont(HWND previewWnd, const std::string& path, int size) {
    if (!FileExists(path)) return;
    AddFontResourceExA(path.c_str(), FR_PRIVATE, 0);
    std::string fileName = path;
    auto pos = fileName.find_last_of("\\/");
    if (pos != std::string::npos) fileName = fileName.substr(pos + 1);
    std::string faceName = fileName;
    pos = faceName.find_last_of('.');
    if (pos != std::string::npos) faceName = faceName.substr(0, pos);

    HFONT hFont = CreateFontA(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
    if (hFont) {
        HFONT hOldFont = (HFONT)SendMessageA(previewWnd, WM_GETFONT, 0, 0);
        SendMessageA(previewWnd, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (hOldFont && hOldFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(hOldFont);
        InvalidateRect(previewWnd, nullptr, TRUE);
    }
}

// ============================================================================
// Settings Window
// ============================================================================
static LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HWND previewBg, previewFont, editFontSize, editStrokeWidth;
    static std::string selectedBg, selectedFont;
    static int selectedFontSize;
    static COLORREF selectedColor;
    static COLORREF selectedStrokeColor;
    static int selectedStrokeWidth;
    static HFONT hPreviewFontHandle = nullptr;
    static GifPreviewState gifState = {0};

    switch (m) {
    case WM_CREATE: {
        selectedBg = g_customBackground;
        selectedFont = g_customFont;
        selectedFontSize = g_customFontSize;
        selectedColor = g_customColorRef;
        selectedStrokeColor = g_customStrokeColorRef;
        selectedStrokeWidth = g_customStrokeWidth;

        memset(&gifState, 0, sizeof(gifState));

        const int margin = 15;
        const int labelWidth = 80;
        const int ctrlWidth = 80;
        const int gap = 10;
        const int ctrlHeight = 22;

        CreateWindowA("STATIC", "Background", WS_VISIBLE | WS_CHILD | SS_LEFT,
                      margin, 10, 280, 20, h, nullptr, nullptr, nullptr);

        previewBg = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
                                  margin, 30, 600, 150, h, (HMENU)ID_PREVIEW_BG, nullptr, nullptr);

        CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      margin, 190, ctrlWidth, ctrlHeight, h, (HMENU)ID_BTN_BROWSE_BG, nullptr, nullptr);

        CreateWindowA("BUTTON", "Reset", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      margin + ctrlWidth + gap, 190, ctrlWidth, ctrlHeight, h, (HMENU)ID_BTN_RESET_BG, nullptr, nullptr);

        CreateWindowA("STATIC", "Font", WS_VISIBLE | WS_CHILD | SS_LEFT,
                      margin, 220, 280, 20, h, nullptr, nullptr, nullptr);

        previewFont = CreateWindowA("STATIC", "MaxRBLX1 Preview",
                                    WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
                                    margin, 240, 600, 50, h, (HMENU)ID_PREVIEW_FONT, nullptr, nullptr);

        int y3 = 300;
        CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      margin, y3, ctrlWidth, ctrlHeight, h, (HMENU)ID_BTN_BROWSE_FONT, nullptr, nullptr);

        CreateWindowA("STATIC", "Size:", WS_VISIBLE | WS_CHILD | SS_RIGHT,
                      margin + ctrlWidth + gap, y3 + 2, labelWidth, 20, h, nullptr, nullptr, nullptr);

        editFontSize = CreateWindowA("EDIT", std::to_string(selectedFontSize).c_str(),
                                     WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                                     margin + ctrlWidth + gap + labelWidth + gap, y3, 45, ctrlHeight, h, (HMENU)ID_EDIT_FONTSIZE, nullptr, nullptr);

        CreateWindowA("STATIC", "Text Color:", WS_VISIBLE | WS_CHILD | SS_RIGHT,
                      margin + ctrlWidth + gap + labelWidth + gap + 55, y3 + 2, 80, 20, h, nullptr, nullptr, nullptr);

        CreateWindowA("BUTTON", "Pick Color", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      margin + ctrlWidth + gap + labelWidth + gap + 145, y3, ctrlWidth, ctrlHeight, h, (HMENU)ID_BTN_COLOR, nullptr, nullptr);

        int y4 = 340;
        CreateWindowA("STATIC", "Stroke Color:", WS_VISIBLE | WS_CHILD | SS_RIGHT,
                      margin, y4 + 2, 90, 20, h, nullptr, nullptr, nullptr);

        CreateWindowA("BUTTON", "Pick Stroke", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      margin + 100, y4, ctrlWidth, ctrlHeight, h, (HMENU)ID_BTN_STROKE_COLOR, nullptr, nullptr);

        CreateWindowA("STATIC", "Stroke Width:", WS_VISIBLE | WS_CHILD | SS_RIGHT,
                      margin + 200, y4 + 2, 90, 20, h, nullptr, nullptr, nullptr);

        editStrokeWidth = CreateWindowA("EDIT", std::to_string(selectedStrokeWidth).c_str(),
                                        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
                                        margin + 300, y4, 45, ctrlHeight, h, nullptr, nullptr, nullptr);

        int y5 = 390;
        CreateWindowA("BUTTON", "Apply", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                      230, y5, 80, 28, h, (HMENU)ID_BTN_APPLY, nullptr, nullptr);

        CreateWindowA("BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      330, y5, 80, 28, h, (HMENU)IDCANCEL, nullptr, nullptr);

        if (!selectedBg.empty()) {
            ApplyBackground(previewBg, selectedBg);
            if (selectedBg.size() > 4) {
                std::string ext = selectedBg.substr(selectedBg.find_last_of('.') + 1);
                if (_stricmp(ext.c_str(), "gif") == 0) {
                    StartGifAnimation(h, previewBg, selectedBg, &gifState);
                }
            }
        }

        if (!selectedFont.empty()) {
            ApplyFont(previewFont, selectedFont, selectedFontSize);
        }

        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)l;
        if (lpDIS->hwndItem == previewBg) {
            RECT rect = lpDIS->rcItem;
            HDC hdc = lpDIS->hDC;

            HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, blackBrush);
            DeleteObject(blackBrush);

            if (!selectedBg.empty() && FileExists(selectedBg)) {
                std::wstring wpath(selectedBg.begin(), selectedBg.end());

                bool isGif = false;
                std::string ext = selectedBg.substr(selectedBg.find_last_of('.') + 1);
                if (_stricmp(ext.c_str(), "gif") == 0) {
                    isGif = true;
                }

                Image* bgImage = nullptr;
                if (isGif && gifState.gifImage) {
                    bgImage = gifState.gifImage;
                } else {
                    bgImage = new Image(wpath.c_str());
                }

                if (bgImage && bgImage->GetLastStatus() == Ok) {
                    Graphics graphics(hdc);
                    graphics.SetInterpolationMode(InterpolationModeHighQuality);

                    int imgWidth = bgImage->GetWidth();
                    int imgHeight = bgImage->GetHeight();
                    int rectWidth = rect.right - rect.left;
                    int rectHeight = rect.bottom - rect.top;

                    float scaleX = (float)rectWidth / imgWidth;
                    float scaleY = (float)rectHeight / imgHeight;
                    float scale = min(scaleX, scaleY);

                    int drawWidth = (int)(imgWidth * scale);
                    int drawHeight = (int)(imgHeight * scale);
                    int drawX = (rectWidth - drawWidth) / 2;
                    int drawY = (rectHeight - drawHeight) / 2;

                    graphics.DrawImage(bgImage, drawX, drawY, drawWidth, drawHeight);

                    if (!(isGif && gifState.gifImage)) {
                        delete bgImage;
                    }
                }
            }
            return TRUE;
        }

        if (lpDIS->hwndItem == previewFont) {
            RECT rect = lpDIS->rcItem;
            HDC hdc = lpDIS->hDC;

            HBRUSH clearBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            FillRect(hdc, &rect, clearBrush);
            DeleteObject(clearBrush);

            Graphics graphics(hdc);
            graphics.SetInterpolationMode(InterpolationModeHighQuality);
            graphics.SetSmoothingMode(SmoothingModeHighQuality);

            std::string faceName = selectedFont;
            auto pos = faceName.find_last_of("\\/");
            if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
            pos = faceName.find_last_of('.');
            if (pos != std::string::npos) faceName = faceName.substr(0, pos);

            std::wstring wfaceName(faceName.begin(), faceName.end());

            FontFamily fontFamily(wfaceName.c_str());
            Font font(&fontFamily, (REAL)selectedFontSize, FontStyleRegular, UnitPixel);

            const WCHAR* text = L"MaxRBLX1 Preview";

            RectF layoutRect((REAL)rect.left, (REAL)rect.top,
                           (REAL)(rect.right - rect.left),
                           (REAL)(rect.bottom - rect.top));

            Color textColor(GetRValue(selectedColor),
                           GetGValue(selectedColor),
                           GetBValue(selectedColor));

            Color strokeColor(GetRValue(selectedStrokeColor),
                             GetGValue(selectedStrokeColor),
                             GetBValue(selectedStrokeColor));

            DrawTextWithStroke(&graphics, text, &font, layoutRect,
                              textColor, strokeColor, (REAL)selectedStrokeWidth);

            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HWND hCtrl = (HWND)l;
        if (hCtrl != previewBg && hCtrl != previewFont) {
            return DefWindowProcA(h, m, w, l);
        }
        return DefWindowProcA(h, m, w, l);
    }

    case WM_COMMAND: {
        if (LOWORD(w) == ID_BTN_BROWSE_BG) {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn = { sizeof(ofn) };
            ofn.hwndOwner = h;
            ofn.lpstrFilter = "Animated Images (*.gif;*.png;*.webp)\0*.gif;*.png;*.webp\0All Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                selectedBg = file;
                if (gifState.timerId) {
                    KillTimer(h, gifState.timerId);
                    gifState.timerId = 0;
                }
                if (gifState.gifImage) {
                    delete gifState.gifImage;
                    gifState.gifImage = nullptr;
                    gifState.isGif = false;
                }
                std::string ext = selectedBg.substr(selectedBg.find_last_of('.') + 1);
                if (_stricmp(ext.c_str(), "gif") == 0) {
                    StartGifAnimation(h, previewBg, selectedBg, &gifState);
                }
                InvalidateRect(previewBg, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_RESET_BG) {
            selectedBg.clear();
            if (gifState.timerId) {
                KillTimer(h, gifState.timerId);
                gifState.timerId = 0;
            }
            if (gifState.gifImage) {
                delete gifState.gifImage;
                gifState.gifImage = nullptr;
                gifState.isGif = false;
            }
            InvalidateRect(previewBg, nullptr, TRUE);
        } else if (LOWORD(w) == ID_BTN_BROWSE_FONT) {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn = { sizeof(ofn) };
            ofn.hwndOwner = h;
            ofn.lpstrFilter = "Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                selectedFont = file;
                AddFontResourceExA(selectedFont.c_str(), FR_PRIVATE, 0);
                std::string faceName = selectedFont;
                auto pos = faceName.find_last_of("\\/");
                if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.');
                if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                if (hPreviewFontHandle) DeleteObject(hPreviewFontHandle);
                hPreviewFontHandle = CreateFontA(selectedFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                if (hPreviewFontHandle) {
                    SendMessageA(previewFont, WM_SETFONT, (WPARAM)hPreviewFontHandle, TRUE);
                }
                InvalidateRect(previewFont, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_COLOR) {
            CHOOSECOLORA cc = { sizeof(cc) };
            cc.hwndOwner = h;
            cc.rgbResult = selectedColor;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            static COLORREF acrCustClr[16] = {0};
            cc.lpCustColors = acrCustClr;
            if (ChooseColorA(&cc)) {
                selectedColor = cc.rgbResult;
                InvalidateRect(previewFont, nullptr, TRUE);
                UpdateWindow(previewFont);
            }
        } else if (LOWORD(w) == ID_BTN_STROKE_COLOR) {
            CHOOSECOLORA cc = { sizeof(cc) };
            cc.hwndOwner = h;
            cc.rgbResult = selectedStrokeColor;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            static COLORREF acrCustClr[16] = {0};
            cc.lpCustColors = acrCustClr;
            if (ChooseColorA(&cc)) {
                selectedStrokeColor = cc.rgbResult;
                InvalidateRect(previewFont, nullptr, TRUE);
                UpdateWindow(previewFont);
            }
        } else if (LOWORD(w) == ID_EDIT_FONTSIZE) {
            char sizeBuf[8];
            GetWindowTextA(editFontSize, sizeBuf, sizeof(sizeBuf));
            int newSize = atoi(sizeBuf);
            if (newSize >= 8 && newSize <= 72) {
                selectedFontSize = newSize;
                if (!selectedFont.empty()) {
                    std::string faceName = selectedFont;
                    auto pos = faceName.find_last_of("\\/");
                    if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                    pos = faceName.find_last_of('.');
                    if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                    if (hPreviewFontHandle) DeleteObject(hPreviewFontHandle);
                    hPreviewFontHandle = CreateFontA(selectedFontSize, 0, 0, 0, FW_NORMAL,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                    if (hPreviewFontHandle) {
                        SendMessageA(previewFont, WM_SETFONT, (WPARAM)hPreviewFontHandle, TRUE);
                    }
                }
                InvalidateRect(previewFont, nullptr, TRUE);
            }
        } else if (LOWORD(w) == ID_BTN_APPLY) {
            if (gifState.timerId) {
                KillTimer(h, gifState.timerId);
                gifState.timerId = 0;
            }

            char swBuf[8];
            GetWindowTextA(editStrokeWidth, swBuf, sizeof(swBuf));
            int newStrokeWidth = atoi(swBuf);
            if (newStrokeWidth >= 0 && newStrokeWidth <= 10) {
                selectedStrokeWidth = newStrokeWidth;
            }

            g_customBackground = selectedBg;
            g_customFont = selectedFont;
            g_customFontSize = selectedFontSize;
            g_customColorRef = selectedColor;
            g_customStrokeColorRef = selectedStrokeColor;
            g_customStrokeWidth = selectedStrokeWidth;
            SaveCustomizations();

            if (gifState.gifImage) {
                delete gifState.gifImage;
                gifState.gifImage = nullptr;
                gifState.isGif = false;
            }

            if (g_gifImage) {
                delete g_gifImage;
                g_gifImage = nullptr;
            }
            if (g_gifTimerId) {
                KillTimer(g_hWnd, g_gifTimerId);
                g_gifTimerId = 0;
            }
            if (g_wicBg.timerId) {
                KillTimer(g_hWnd, g_wicBg.timerId);
                g_wicBg.timerId = 0;
            }
            g_wicBg.decoder.Reset();
            g_wicBg.currentFrame.Reset();
            g_wicBg.isAnimated = false;
            g_backgroundIsGif = false;
            g_backgroundIsAnimated = false;
            ClearCachedBackground();

            if (!g_customBackground.empty() && FileExists(g_customBackground)) {
                std::string ext = g_customBackground;
                auto dot = ext.find_last_of('.');
                if (dot != std::string::npos) {
                    ext = ext.substr(dot);
                    if (_stricmp(ext.c_str(), ".gif") == 0) {
                        g_backgroundIsGif = true;
                        g_backgroundIsAnimated = true;
                        std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
                        g_gifImage = new Image(wpath.c_str());
                        if (g_gifImage->GetLastStatus() == Ok) {
                            GUID pageGuid = FrameDimensionTime;
                            g_gifFrameCount = g_gifImage->GetFrameCount(&pageGuid);
                            if (g_gifFrameCount > 1) {
                                UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                                g_gifTimerId = SetTimer(g_hWnd, 3001, frameDelay, nullptr);
                            }
                        }
                    } else if (_stricmp(ext.c_str(), ".webp") == 0 || _stricmp(ext.c_str(), ".png") == 0) {
                        if (LoadWicAnimatedBackground(g_customBackground)) {
                            g_backgroundIsAnimated = true;
                        }
                    }
                }
            }
            EnsureBackgroundCached();
            InvalidateRect(g_hWnd, nullptr, TRUE);
            UpdateWindow(g_hWnd);
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) {
            if (gifState.timerId) {
                KillTimer(h, gifState.timerId);
                gifState.timerId = 0;
            }
            if (gifState.gifImage) {
                delete gifState.gifImage;
                gifState.gifImage = nullptr;
                gifState.isGif = false;
            }
            DestroyWindow(h);
        }
        return 0;
    }

    case WM_TIMER:
        if (w == 3002 && gifState.isGif && gifState.gifImage && gifState.frameCount > 1) {
            gifState.currentFrame = (gifState.currentFrame + 1) % gifState.frameCount;
            GUID pageGuid = FrameDimensionTime;
            gifState.gifImage->SelectActiveFrame(&pageGuid, gifState.currentFrame);
            InvalidateRect(previewBg, nullptr, TRUE);
            UpdateWindow(previewBg);
        }
        return 0;

    case WM_DESTROY:
        if (gifState.timerId) {
            KillTimer(h, gifState.timerId);
            gifState.timerId = 0;
        }
        if (gifState.gifImage) {
            delete gifState.gifImage;
            gifState.gifImage = nullptr;
            gifState.isGif = false;
        }
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
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
        SetForegroundWindow(g_hSettingsWnd);
        return;
    }
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "PhantomRecSettings";
    RegisterClassExA(&wc);

    int settingsWidth = 650;
    int settingsHeight = 460;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - settingsWidth) / 2;
    int y = (screenHeight - settingsHeight) / 2;

    g_hSettingsWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        "PhantomRecSettings", "Customization",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y,
        settingsWidth, settingsHeight,
        g_hWnd, nullptr, GetModuleHandle(nullptr), nullptr);
}

// ============================================================================
// UI Update
// ============================================================================
static void UpdateUI() {
    std::string hotkey = GetHotkeyName(g_recordHotkey);
    std::string pauseKey = GetHotkeyName(g_pauseHotkey);

    int audioActive = Core_GetAudioStatus(&g_Core);
    int segmentCount = Core_GetSegmentCount(&g_Core);
    long long totalBytes = Core_GetTotalBytes(&g_Core);
    char fileSizeStr[64];
    Core_FormatSize(totalBytes, fileSizeStr, sizeof(fileSizeStr));

    int displayThreads = g_Core.dynamicThreads;
    bool isRecording = Core_IsRecording(&g_Core);

    if (Core_IsConverting(&g_Core)) {
        DoUpdateButton("Processing...");
        ShowWindow(g_progressBar, SW_SHOW);
        return;
    }

    if (g_progressBar && IsWindow(g_progressBar)) {
        ShowWindow(g_progressBar, SW_HIDE);
    }

    if (isRecording) {
        if (Core_IsPaused(&g_Core)) {
            DoUpdateButton(("RESUME (" + pauseKey + ")").c_str());
            char status[512];
            sprintf_s(status, sizeof(status),
                "PAUSED\r\n"
                "Segments: %d\r\n"
                "Audio: %s\r\n"
                "Total: %s",
                segmentCount,
                audioActive ? "ON" : "OFF",
                fileSizeStr);
            DoUpdateStatus(status);
        } else {
            DoUpdateButton(("STOP (" + hotkey + ")").c_str());
            char status[512];
            sprintf_s(status, sizeof(status),
                "Recording...\r\n"
                "Codec: %s\r\n"
                "Method: %s\r\n"
                "Resolution: %dx%d\r\n"
                "Segments: %d\r\n"
                "Audio: %s\r\n"
                "Total: %s",
                "MaxRBLX1's Fastest MJPEG",
                Core_GetCaptureMethodDesc(&g_Core),
                g_Core.screenWidth, g_Core.screenHeight,
                segmentCount,
                audioActive ? "ON" : "OFF",
                fileSizeStr);
            DoUpdateStatus(status);
        }
        return;
    }

    DoUpdateButton(("START (" + hotkey + ")").c_str());

    char status[512];
    sprintf_s(status, sizeof(status),
        "Ready - %s to record\r\n"
        "Pause: %s\r\n"
        "\r\n"
        "Cores: %d\r\n"
        "Threads: %d (conversion)\r\n"
        "Capture: %s\r\n"
        "Codec: %s\r\n"
        "CRF: %d\r\n"
        "Resolution: %dx%d\r\n"
        "Audio: %s (system loopback)\r\n"
        "Total recorded: %s\r\n"
        "\r\n"
        "Max'sEngine(tm) Powered by FFmpeg\r\n"
        "Built by MaxRBLX1",
        hotkey.c_str(),
        pauseKey.c_str(),
        g_Core.cpuCoreCount,
        displayThreads,
        Core_GetCaptureMethodDesc(&g_Core),
        "MaxRBLX1's Fastest MJPEG",
        g_Core.crf,
        g_Core.screenWidth, g_Core.screenHeight,
        audioActive ? "available" : "unavailable",
        fileSizeStr);

    DoUpdateStatus(status);
}

// ============================================================================
// Helper: Draw main UI text with stroke
// ============================================================================
static void DrawMainUITextWithStroke(HDC hdc, const RECT& rect, const std::string& text,
                                      COLORREF textColor, COLORREF strokeColor, int strokeWidth) {
    Graphics graphics(hdc);
    graphics.SetInterpolationMode(InterpolationModeHighQuality);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);

    std::wstring wtext(text.begin(), text.end());

    std::string faceName = g_customFont;
    if (!faceName.empty()) {
        auto pos = faceName.find_last_of("\\/");
        if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
        pos = faceName.find_last_of('.');
        if (pos != std::string::npos) faceName = faceName.substr(0, pos);
    } else {
        faceName = "Segoe UI";
    }

    std::wstring wfaceName(faceName.begin(), faceName.end());
    FontFamily fontFamily(wfaceName.c_str());
    Font font(&fontFamily, (REAL)g_customFontSize, FontStyleRegular, UnitPixel);

    RectF layoutRect((REAL)rect.left, (REAL)rect.top,
                     (REAL)(rect.right - rect.left),
                     (REAL)(rect.bottom - rect.top));

    Color colorText(GetRValue(textColor), GetGValue(textColor), GetBValue(textColor));
    Color colorStroke(GetRValue(strokeColor), GetGValue(strokeColor), GetBValue(strokeColor));

    DrawTextWithStroke(&graphics, wtext.c_str(), &font, layoutRect,
                       colorText, colorStroke, (REAL)strokeWidth);
}

// ============================================================================
// Recording-saved tray notification
// ============================================================================
// Uses Shell_NotifyIcon, which is the one notification API that renders
// natively on every Windows version PhantomRec supports:
//   Windows 7 SP1 / 8 / 8.1 -> classic balloon tip
//   Windows 10 / 11         -> toast in the Action Center
// The tray icon is added on demand and removed 10 seconds later.

#define WM_TRAY_NOTIFY        (WM_APP + 30)
#define TRAY_ICON_ID          1
#define TRAY_CLEANUP_TIMER    5001

static NOTIFYICONDATAW g_nid = {0};
static bool            g_trayIconAdded = false;
static std::string     g_lastRecordingPath;

static void TrayNotifyAddIfNeeded() {
    if (g_trayIconAdded) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hWnd;
    g_nid.uID    = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY_NOTIFY;

    g_nid.hIcon = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                    MAKEINTRESOURCE(IDI_MAIN_ICON),
                                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    wcscpy(g_nid.szTip, L"PhantomRec");

    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayIconAdded = true;
    }
}

static void TrayNotifyRemove() {
    if (!g_trayIconAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIconAdded = false;
}

static void ShowRecordingSavedNotification(const char* filePath) {
    if (!g_hWnd || !IsWindow(g_hWnd)) return;
    if (!filePath || !filePath[0]) return;

    g_lastRecordingPath = filePath;
    TrayNotifyAddIfNeeded();

    const char* fileName = strrchr(filePath, '\\');
    fileName = fileName ? fileName + 1 : filePath;

    wchar_t wFileName[MAX_PATH] = {0};
    MultiByteToWideChar(CP_ACP, 0, fileName, -1, wFileName, MAX_PATH);

    wcscpy(g_nid.szInfoTitle, L"PhantomRec \u2014 Recording saved");
    wcscpy(g_nid.szInfo, L"Your recording is ready. Click to open the folder.\n");
    size_t used = wcslen(g_nid.szInfo);
    if (used < 250) {
        wcsncpy(g_nid.szInfo + used, wFileName, 255 - used);
        g_nid.szInfo[255] = 0;
    }

    g_nid.uFlags      = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    g_nid.uTimeout    = 6000;

    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    KillTimer(g_hWnd, TRAY_CLEANUP_TIMER);
    SetTimer(g_hWnd, TRAY_CLEANUP_TIMER, 10000, NULL);
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
    case WM_TRAY_NOTIFY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == NIN_BALLOONUSERCLICK) {
            if (!g_lastRecordingPath.empty()) {
                ShellExecuteA(nullptr, "open", "explorer",
                    ("/select,\"" + g_lastRecordingPath + "\"").c_str(),
                    nullptr, SW_SHOWNORMAL);
            }
        }
        return 0;
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

        g_btnSettings = CreateWindowA("BUTTON", "", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            360, 5, 30, 30, h, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);

        g_progressBar = CreateWindowA("msctls_progress32", "", WS_CHILD,
            20, 260, 340, 15, h, nullptr, nullptr, nullptr);
        SendMessageA(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        ShowWindow(g_progressBar, SW_HIDE);

        UINT recMod = GetHotkeyModifiers(g_recordHotkey);
        RegisterHotKey(h, ID_HOTKEY_RECORD, recMod, g_recordHotkey);
        UINT pauseMod = GetHotkeyModifiers(g_pauseHotkey);
        RegisterHotKey(h, ID_HOTKEY_PAUSE, pauseMod, g_pauseHotkey);

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
                    g_backgroundIsAnimated = true;
                    std::wstring wpath(g_customBackground.begin(), g_customBackground.end());
                    g_gifImage = new Image(wpath.c_str());
                    if (g_gifImage->GetLastStatus() == Ok) {
                        GUID pageGuid = FrameDimensionTime;
                        g_gifFrameCount = g_gifImage->GetFrameCount(&pageGuid);
                        if (g_gifFrameCount > 1) {
                            UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                            g_gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
                        }
                    }
                } else if (ext == ".webp" || ext == ".WEBP" || ext == ".png" || ext == ".PNG") {
                    if (LoadWicAnimatedBackground(g_customBackground)) {
                        g_backgroundIsAnimated = true;
                    }
                }
            }
        }
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)l;
        if (lpDIS->hwndItem == g_btnSettings) {
            Graphics graphics(lpDIS->hDC);
            graphics.SetSmoothingMode(SmoothingModeHighQuality);
            SolidBrush bgBrush(Color(45, 45, 48));
            graphics.FillRectangle(&bgBrush, 0, 0, 30, 30);
            FontFamily fontFamily(L"Segoe UI Emoji");
            Font font(&fontFamily, 16, FontStyleRegular, UnitPixel);
            SolidBrush textBrush(Color(255, 255, 255));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF rect(0, 0, 30, 30);
            graphics.DrawString(L"⚙", -1, &font, rect, &format, &textBrush);
            return TRUE;
        }
        break;
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
                if (hCurrentFont) {
                    SelectObject(hdcStatic, (HFONT)GetStockObject(SYSTEM_FONT));
                    DeleteObject(hCurrentFont);
                    hCurrentFont = nullptr;
                }
                std::string faceName = g_customFont;
                auto pos = faceName.find_last_of("\\/");
                if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.');
                if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hCurrentFont = CreateFontA(g_customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastFontName = g_customFont;
                lastFontSize = g_customFontSize;
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
                if (hBtnFont) {
                    DeleteObject(hBtnFont);
                    hBtnFont = nullptr;
                }
                std::string faceName = g_customFont;
                auto pos = faceName.find_last_of("\\/");
                if (pos != std::string::npos) faceName = faceName.substr(pos + 1);
                pos = faceName.find_last_of('.');
                if (pos != std::string::npos) faceName = faceName.substr(0, pos);
                hBtnFont = CreateFontA(g_customFontSize, 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName.c_str());
                lastBtnFontName = g_customFont;
                lastBtnFontSize = g_customFontSize;
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

        if (g_backgroundIsAnimated && g_wicBg.currentFrame) {
            DrawWicFrame(hdc, rect);
        } else if (g_backgroundIsGif && g_gifImage) {
            Graphics graphics(hdc);
            float imgW = (float)g_gifImage->GetWidth();
            float imgH = (float)g_gifImage->GetHeight();
            float winW = (float)(rect.right - rect.left);
            float winH = (float)(rect.bottom - rect.top);
            float scale = min(winW / imgW, winH / imgH);
            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float drawX = (winW - drawW) / 2.0f;
            float drawY = (winH - drawH) / 2.0f;
            SolidBrush blackBrush(Color(0, 0, 0));
            graphics.FillRectangle(&blackBrush, 0, 0, (int)winW, (int)winH);
            graphics.DrawImage(g_gifImage, (int)drawX, (int)drawY, (int)drawW, (int)drawH);
        } else if (g_cachedBgImage) {
            Graphics graphics(hdc);
            float imgW = (float)g_cachedBgImage->GetWidth();
            float imgH = (float)g_cachedBgImage->GetHeight();
            float winW = (float)(rect.right - rect.left);
            float winH = (float)(rect.bottom - rect.top);
            float scale = min(winW / imgW, winH / imgH);
            float drawW = imgW * scale;
            float drawH = imgH * scale;
            float drawX = (winW - drawW) / 2.0f;
            float drawY = (winH - drawH) / 2.0f;
            SolidBrush blackBrush(Color(0, 0, 0));
            graphics.FillRectangle(&blackBrush, 0, 0, (int)winW, (int)winH);
            graphics.DrawImage(g_cachedBgImage, (int)drawX, (int)drawY, (int)drawW, (int)drawH);
        } else {
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
        }

        RECT statusRect;
        statusRect.left = 20;
        statusRect.top = 120;
        statusRect.right = 360;
        statusRect.bottom = 250;

        DrawMainUITextWithStroke(hdc, statusRect, g_statusText,
                                 g_customColorRef, g_customStrokeColorRef, g_customStrokeWidth);

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
        else if (w == TRAY_CLEANUP_TIMER) {
            KillTimer(h, TRAY_CLEANUP_TIMER);
            TrayNotifyRemove();
        }
        else if (w == 3001 && g_gifImage && g_gifFrameCount > 1) {
            g_gifCurrentFrame = (g_gifCurrentFrame + 1) % g_gifFrameCount;
            GUID pageGuid = FrameDimensionTime;
            g_gifImage->SelectActiveFrame(&pageGuid, g_gifCurrentFrame);
            InvalidateRect(h, nullptr, TRUE);
            UpdateWindow(h);
        }
        else if (w == 3003 && g_wicBg.isAnimated && g_wicBg.decoder) {
            g_wicBg.currentIndex = (g_wicBg.currentIndex + 1) % g_wicBg.frameCount;
            g_wicBg.decoder->GetFrame(g_wicBg.currentIndex, &g_wicBg.currentFrame);
            InvalidateRect(h, nullptr, TRUE);
            UpdateWindow(h);
        }
        return 0;

    case WM_SYSCOMMAND: {
        UINT sysCmd = (w & 0xFFF0);
        if (sysCmd == SC_MINIMIZE) {
            if (g_gifTimerId) { KillTimer(h, g_gifTimerId); g_gifTimerId = 0; }
            if (g_wicBg.timerId) { KillTimer(h, g_wicBg.timerId); g_wicBg.timerId = 0; }
        }
        else if (sysCmd == SC_RESTORE) {
            if (g_backgroundIsGif && g_gifImage && g_gifFrameCount > 1 && !g_gifTimerId) {
                UINT frameDelay = GetGifFrameDelay(g_gifImage, g_gifFrameCount);
                g_gifTimerId = SetTimer(h, 3001, frameDelay, nullptr);
            }
            if (g_wicBg.isAnimated && g_wicBg.decoder && !g_wicBg.timerId) {
                g_wicBg.timerId = SetTimer(h, 3003, g_wicBg.frameDelay, nullptr);
            }
            InvalidateRect(h, nullptr, TRUE);
        }
        return DefWindowProcA(h, m, w, l);
    }

    case WM_DESTROY: {
		TrayNotifyRemove(); 
        if (g_hButtonFont) DeleteObject(g_hButtonFont);

        if (Core_IsRecording(&g_Core)) Core_StopRecording(&g_Core);

        if (g_gifImage) { delete g_gifImage; g_gifImage = nullptr; }
        if (g_gifTimerId) KillTimer(h, g_gifTimerId);
        if (g_wicBg.timerId) KillTimer(h, g_wicBg.timerId);
        g_wicBg.decoder.Reset();
        g_wicBg.currentFrame.Reset();
        ClearCachedBackground();
        UnregisterHotKey(h, ID_HOTKEY_RECORD);
        UnregisterHotKey(h, ID_HOTKEY_PAUSE);
        KillTimer(h, ID_TIMER_INI_CHECK);

        Core_Shutdown(&g_Core);

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
    g_mainThreadId = GetCurrentThreadId();

    GdiplusStartupInput gdiplusInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);

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
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }
	
	Core_ProbeAudio(&g_Core);
	
    if (!g_Core.audioActive && g_Core.maxsoundPath[0] == '\0') {
        // Only warn if maxsound.exe is missing entirely. If it's present
        // but the probe failed, that usually means no audio device, which
        // is normal on headless machines and doesn't need a popup.
        MessageBoxA(nullptr,
            "maxsound.exe not found.\n\n"
            "Recordings will be video-only.\n\n"
            "Place maxsound.exe next to PhantomRec.exe to capture "
            "system audio.",
            "PhantomRec v" PHANTOMREC_VERSION, MB_OK | MB_ICONINFORMATION);
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

    Sleep(500);
    MessageBeep(MB_ICONINFORMATION);

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = "PhantomRecWnd";
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    wc.hIconSm = LoadIconA(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    RegisterClassExA(&wc);

    int w = 395, h = 340;
    g_hWnd = CreateWindowExA(WS_EX_TOPMOST | 0x02000000L, "PhantomRecWnd",
        "PhantomRec v" PHANTOMREC_VERSION " - Max'sEngine",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        w, h, nullptr, nullptr, hInst, nullptr);

    if (!g_hWnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    HICON hIcon = LoadIconA(hInst, MAKEINTRESOURCE(IDI_MAIN_ICON));
    if (hIcon) {
        SendMessageA(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageA(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
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

    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
