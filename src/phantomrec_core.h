// phantomrec_core.h — PhantomRec v1.9.5 Pure C Core
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
// Max'sEngine™ | DXGI/GFX/DDAGrab/GDI Capture | MPEG-4 Stage 1 | x264 Stage 2 | WASAPI Audio

#ifndef PHANTOMREC_CORE_H
#define PHANTOMREC_CORE_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Capture Method Enum
// ============================================================================
typedef enum {
    CAPTURE_AUTO = 0,      // Auto-detect (DDAGrab preferred)
    CAPTURE_DDAGRAB = 1,   // Desktop Duplication (GPU, low latency, borderless)
    CAPTURE_GFX = 2,       // Graphics Capture (GPU, exclusive fullscreen)
    CAPTURE_GDI = 3        // GDI (CPU, software, Win7 compatible)
} CaptureMethod;

// ============================================================================
// Core State Structure
// ============================================================================
typedef struct PhantomRecCore {
    // Recording state
    volatile int recording;
    volatile int paused;
    volatile int converting;
    volatile int convertProgress;
    int convertAfterRecording;
	int captureFps; 
    
    // Capture method (0=GFX, 1=DDAGrab, 2=GDI)
    int captureMethod;
    
    // Screen dimensions
    int screenWidth;
    int screenHeight;
    
    // Pipeline configuration
    int cpuCoreCount;
    int dynamicThreads;
    int crf;
    int maxrate;
    int bufsize;
    int videoQueueSize;
    int pipeBufferSizeMB;
    int convertPreset;  // 0=ultrafast, 1=veryfast, 2=medium
    
    // Session stats
    int sessions;
    long long totalBytes;
    int lastRecordingDurationMs;
    
    // Audio format
    char audioFormat[16];
    int audioSampleRate;
    int audioChannels;
    int audioBitsPerSample;
    
    // Paths
    char maxsenginePath[MAX_PATH];
    char outputDir[MAX_PATH];
    char tempFile[MAX_PATH];
    char finalFile[MAX_PATH];
    char segmentBaseName[256];
    int pauseSegmentCount;
    char segmentFiles[64][MAX_PATH];
    int segmentCount;
    
    // WASAPI audio
    IAudioClient* audioClient;
    IAudioCaptureClient* captureClient;
    WAVEFORMATEX* waveFormat;
    int audioActive;
    HANDLE hAudioReadyEvent;
    HANDLE hAudioPipeWrite;
    HANDLE hAudioPipeRead;
    HANDLE hAudioSyncEvent;
	HANDLE hAudioThread;
    
    // FFmpeg process
    PROCESS_INFORMATION ffmpegProcess;
    
    // Timing
    LARGE_INTEGER recStart;
    LARGE_INTEGER recFreq;
    LARGE_INTEGER pauseTime;
    long long totalPausedDurationMs;
    
    // Power plan
    GUID originalPowerPlan;
    int powerPlanChanged;
    
    // Taskbar anchor / game window
    HWND g_SelectedGameWnd;
    LONG g_OriginalGameStyle;
    LONG g_OriginalGameExStyle;
    RECT g_OriginalGameRect;
    int g_GameStylesModified;
    
    // Callback for UI updates (called by core, handled by C++ UI)
    void (*onStatusUpdate)(const char* message);
    void (*onButtonUpdate)(const char* text);
    void (*onProgressUpdate)(int percent);
    void (*onConversionDone)(int success, const char* filePath);
    
} PhantomRecCore;

// ============================================================================
// Capture Method API (after struct declaration)
// ============================================================================
void Core_SetCaptureMethodEx(PhantomRecCore* core, CaptureMethod method);
const char* Core_GetCaptureMethodDesc(const PhantomRecCore* core);

// ============================================================================
// Core API — called by C++ UI
// ============================================================================

// Initialize the core with paths
void Core_Init(PhantomRecCore* core, const char* maxsenginePath, const char* outputDir);

// Recording control
int  Core_StartRecording(PhantomRecCore* core);
void Core_StopRecording(PhantomRecCore* core);
void Core_TogglePause(PhantomRecCore* core);

// Status queries
int  Core_IsRecording(const PhantomRecCore* core);
int  Core_IsPaused(const PhantomRecCore* core);
int  Core_IsConverting(const PhantomRecCore* core);
int  Core_GetProgress(const PhantomRecCore* core);

// Utility
void Core_DetectResolution(PhantomRecCore* core);
void Core_ConfigurePipeline(PhantomRecCore* core);
void Core_SetCaptureMethod(PhantomRecCore* core);
void Core_WarmEngine(PhantomRecCore* core);
void Core_CleanupOrphanedTempFiles(PhantomRecCore* core);

// File helpers
int  Core_FileExists(const char* path);
long long Core_GetFileSize(const char* path);
void Core_FormatSize(long long bytes, char* buf, int bufsize);
void Core_FormatTime(int seconds, char* buf, int bufsize);
void Core_Timestamp(char* buf, int bufsize);
void Core_GetVideosFolder(char* buf, int bufsize);
int  Core_FindMaxsEngine(PhantomRecCore* core);

#ifdef __cplusplus
}
#endif

#endif // PHANTOMREC_CORE_H
