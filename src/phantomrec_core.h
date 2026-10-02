// phantomrec_core.h — PhantomRec v1.9.9 Core Interface
// "Every screen deserves to be recorded."
// Built by MaxRBLX1
//
// Stage 1: MaxRBLX1's Fastest MJPEG  (maxenc.exe primary, in-process fallback)
// Stage 2: x264 ultrafast post-convert.
//
// Huffyuv is retired. videoEncoder is locked to two values:
//   0 = MaxRBLX1's Fastest MJPEG via maxenc.exe  (default, preferred)
//   2 = MaxRBLX1's Fastest MJPEG in-process      (fallback if maxenc.exe missing)
// Value 1 is reserved legacy and is never set anywhere in the codebase.

#ifndef PHANTOMREC_CORE_H
#define PHANTOMREC_CORE_H

#include <windows.h>
#include <stdint.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PATH_LONG 1024
#define MAX_SEGMENTS  64

typedef enum {
    CAPTURE_AUTO = 0,
    CAPTURE_GFX,
    CAPTURE_DDAGRAB,
    CAPTURE_GDI
} CaptureMethod;

typedef struct {
    // ---- paths ----
    char maxsenginePath[MAX_PATH];
    char maxencPath[MAX_PATH];
    char maxsoundPath[MAX_PATH];
    char outputDir[MAX_PATH];

    // ---- recording state ----
    volatile LONG recording;
    volatile LONG paused;
    volatile LONG converting;
    volatile LONG audioCaptureActive; // never written to 1
    int           convertProgress;
    int           convertAfterRecording;

    // ---- capture ----
    int           captureMethod;      // 0=GFX, 1=DDAGRAB, 2=GDI
    int           screenWidth;
    int           screenHeight;

    // ---- pipeline config ----
    int           crf;
    int           maxrate;
    int           bufsize;
    int           pipeBufferSizeMB;
    int           dynamicThreads;
    int           cpuCoreCount;
    int           videoQueueSize;

    // ---- Stage 1 codec selection ----
    // 0 = MaxRBLX1's Fastest MJPEG via maxenc.exe (default, preferred)
    // 2 = MaxRBLX1's Fastest MJPEG in-process     (fallback if maxenc.exe missing)
    // 1 = RETIRED (Huffyuv). Never set. Present only for struct ABI stability.
    int           videoEncoder;
    int           mjpegQuality;       // 1-100, default 75

    // ---- segment tracking ----
    char          segmentBaseName[MAX_PATH];
    char          segmentFiles[MAX_SEGMENTS][MAX_PATH];
    int           segmentCount;
    int           pauseSegmentCount;
    char          tempFile[MAX_PATH];
    char          tempAudioFile[MAX_PATH];
    char          finalFile[MAX_PATH];

    // ---- timing ----
    LARGE_INTEGER recFreq;
    LARGE_INTEGER recStart;
    LARGE_INTEGER segmentStartTime;
    LARGE_INTEGER pauseTime;
    long long     segmentDurationMs;
    long long     totalPausedDurationMs;
    int           lastRecordingDurationMs;

    // Per-segment audio delay in ms, applied at mux time via -af adelay.
    // Set from maxenc's first-frame marker so audio t=0 aligns with
    // video t=0 instead of lagging by maxenc's warm-up delta.
    int           segmentAudioDelayMs[MAX_SEGMENTS];

    // QPC ticks from maxenc's <segment>.mkv.t0 and maxsound's
    // <segment>.wav.t0. Used at mux time to align audio with video.
    int64_t       segmentVideoT0[MAX_SEGMENTS];

    // ---- child processes ----
    PROCESS_INFORMATION ffmpegProcess;       // maxenc.exe or in-process (unused for in-process)
    PROCESS_INFORMATION ffmpegAudioProcess;  // WASAPI -> ffmpeg audio child

    // ---- audio ----
    IAudioClient*        audioClient; // always NULL
    IAudioCaptureClient* captureClient; // always NULL
    WAVEFORMATEX*        waveFormat; // always NULL
    HANDLE               hAudioReadyEvent; // always NULL
    HANDLE               hAudioThread; // always NULL
    HANDLE               hAudioPipeRead; // always NULL
    HANDLE               hAudioPipeWrite; // always NULL
    int                  audioActive;
    int                  audioAlwaysRunning; // always 0
    int                  audioBitsPerSample; // always 0
    char                 audioFormat[16]; // always empty
    int                  audioSampleRate; // always 0
    int                  audioChannels;  // always 0
	
    int64_t       segmentAudioT0[MAX_SEGMENTS]; // unused, reserved

    // ---- stats ----
    long long            totalBytes;
    int                  sessions;

    // ---- UI callbacks ----
    void (*onStatusUpdate)(const char* message);
    void (*onButtonUpdate)(const char* text);
    void (*onProgressUpdate)(int percent);
    void (*onConversionDone)(int success, const char* filePath);
} PhantomRecCore;

// ---- lifecycle ----
void Core_Init(PhantomRecCore* core, const char* maxsenginePath, const char* outputDir);
void Core_Shutdown(PhantomRecCore* core);
void Core_DetectResolution(PhantomRecCore* core);
void Core_ConfigurePipeline(PhantomRecCore* core);
void Core_SetCaptureMethod(PhantomRecCore* core);
void Core_SetCaptureMethodEx(PhantomRecCore* core, CaptureMethod method);
const char* Core_GetCaptureMethodDesc(const PhantomRecCore* core);
void Core_CleanupOrphanedTempFiles(PhantomRecCore* core);
void Core_ProbeAudio(PhantomRecCore* core);

// ---- engine discovery ----
int  Core_FindMaxsEngine(PhantomRecCore* core);

// ---- recording ----
int  Core_StartRecording(PhantomRecCore* core);
void Core_StopRecording(PhantomRecCore* core);
void Core_TogglePause(PhantomRecCore* core);

// ---- status ----
int  Core_IsRecording(const PhantomRecCore* core);
int  Core_IsPaused(const PhantomRecCore* core);
int  Core_IsConverting(const PhantomRecCore* core);
int  Core_GetProgress(const PhantomRecCore* core);
int  Core_GetAudioStatus(const PhantomRecCore* core);
int  Core_GetSegmentCount(const PhantomRecCore* core);
long long Core_GetTotalBytes(const PhantomRecCore* core);

// ---- utilities ----
int  Core_FileExists(const char* path);
long long Core_GetFileSize(const char* path);
void Core_FormatSize(long long bytes, char* buf, int bufsize);
void Core_FormatTime(int seconds, char* buf, int bufsize);
void Core_Timestamp(char* buf, int bufsize);
void Core_GetVideosFolder(char* buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif // PHANTOMREC_CORE_H
