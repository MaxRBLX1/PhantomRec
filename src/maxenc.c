// maxenc.c — MaxRBLX1's Fastest MJPEG encoder subprocess
// Reads the screen with gfxcapture / ddagrab / gdigrab, compresses to JPEG
// with libjpeg-turbo, and muxes to MKV. Video only — audio is owned by the
// parent process (PhantomRec), which spawns its own audio child separately
// and muxes the two streams together after recording ends.
//
// Stop by pressing 'q' (or sending CTRL_BREAK) in this process's console.
// Pause by pressing 'p'. Pausing ends the current segment; resuming starts a
// new one. On stop, all segments are concatenated losslessly into <output>.mkv.
//
// Pixel format:
//   Windows screen capture produces BGRX — Blue, Green, Red, and an ignored
//   pad byte. It is NOT BGRA; the fourth byte is undefined padding, not an
//   alpha channel. The format filter requests `bgra` (same 4-byte memory
//   layout the driver produces; the fourth byte is ignored) and
//   libjpeg-turbo's TJPF_BGRX entry point reads the B, G, R channels and
//   ignores the pad byte.
//
// PTS strategy:
//   Every frame gets a wall-clock PTS from QueryPerformanceCounter. No frame
//   counter, no drift check, no drift threshold. PTS is monotonic within a
//   segment (lastPtsUs clamp). The segment start is taken lazily on the first
//   real frame.
//
// First-frame marker:
//   On the first real frame of segment 0, maxenc writes <output.mkv>.t0
//   containing the QPC tick at that instant. PhantomRec polls for this file
//   so it can start audio capture at the same real-world moment, and uses
//   the delta at mux time to apply -af adelay so audio t=0 aligns with
//   video t=0.
//
// Writer thread:
//   The capture loop never calls av_write_frame directly. It hands encoded
//   JPEG buffers to a bounded ring; a separate writer thread muxes them.
//   This decouples the 16.6 ms frame budget from disk I/O stalls (Defender,
//   HDD seeks, SMB latency) and eliminates the corresponding frame drops.
//   On overflow, the OLDEST queued frame is dropped — the right policy for
//   a screen recorder.
//
// Build: gcc -std=c11 -O2
//        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000
//        src/maxenc.c -o maxenc.exe
//        -lturbojpeg -lavformat -lavcodec -lavfilter -lavutil -lavdevice
//        -lole32 -luuid -lwinmm
//
// Exit codes: 0 = normal, 1 = fatal error, 2 = stop requested before any frames.

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <turbojpeg.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <process.h>
#include <conio.h>

// ============================================================================
// Config
// ============================================================================
typedef enum { API_AUTO, API_GFX, API_DDAGRAB, API_GDI } CaptureAPI;

static volatile LONG g_stopRequested = 0;
static volatile LONG g_pauseRequested = 0;

static LARGE_INTEGER g_qpcFreq = {0};

static tjhandle g_tjc = NULL;

static int g_boostConsolePriority = 0;

static const char* ApiName(CaptureAPI api) {
    switch (api) {
    case API_GFX:     return "gfxcapture";
    case API_DDAGRAB: return "ddagrab";
    case API_GDI:     return "gdigrab";
    default:          return "auto";
    }
}

static int GetTargetFPS(CaptureAPI api) {
    if (api == API_GDI) return 30;
    return 60;
}

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

static int FilterExists(const char* name) {
    return avfilter_get_by_name(name) != NULL;
}

static CaptureAPI PickAPI(CaptureAPI requested) {
    int winVer = GetWindowsVersion();

    if (requested == API_AUTO) {
        if (winVer >= 10 && FilterExists("gfxcapture"))  return API_GFX;
        if (winVer >= 8  && FilterExists("ddagrab"))     return API_DDAGRAB;
        return API_GDI;
    }

    if (requested == API_GFX && !FilterExists("gfxcapture")) {
        fprintf(stderr, "[maxenc] gfxcapture not available, falling back\n");
        if (winVer >= 8 && FilterExists("ddagrab")) return API_DDAGRAB;
        return API_GDI;
    }
    if (requested == API_DDAGRAB && !FilterExists("ddagrab")) {
        fprintf(stderr, "[maxenc] ddagrab not available, falling back to gdigrab\n");
        return API_GDI;
    }
    if (requested == API_GFX && winVer < 10) {
        fprintf(stderr, "[maxenc] gfxcapture requires Win10+, falling back\n");
        if (winVer >= 8 && FilterExists("ddagrab")) return API_DDAGRAB;
        return API_GDI;
    }
    if (requested == API_DDAGRAB && winVer < 8) {
        fprintf(stderr, "[maxenc] ddagrab requires Win8+, falling back to gdigrab\n");
        return API_GDI;
    }
    return requested;
}

// ============================================================================
// Console control thread — watches for 'q' (stop) and 'p' (pause)
// ============================================================================
static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    }
    return FALSE;
}

static unsigned int __stdcall ConsoleKeyThread(void* param) {
    (void)param;

    if (g_boostConsolePriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    while (!InterlockedCompareExchange(&g_stopRequested, 1, 1)) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q' || c == 'Q') {
                InterlockedExchange(&g_stopRequested, 1);
                break;
            } else if (c == 'p' || c == 'P') {
                InterlockedExchange(&g_pauseRequested, 1);
            }
        }
        Sleep(50);
    }
    return 0;
}

// ============================================================================
// MJPEG encode helper
// ============================================================================
static unsigned char* EncodeFrameToMJPEG(tjhandle tjc,
                                         const unsigned char* bgrx,
                                         int w, int h, int stride,
                                         int quality, unsigned long* out_size) {
    unsigned char* jpeg = NULL;
    int ret = tjCompress2(tjc, (unsigned char*)bgrx, w, stride, h,
                          TJPF_BGRX, &jpeg, out_size,
                          TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (ret != 0 || !jpeg) { *out_size = 0; return NULL; }
    return jpeg;
}

// ============================================================================
// Bounded writer ring — decouples capture from disk I/O
// ============================================================================
#define WRITER_RING_SIZE 64

typedef struct {
    unsigned char* data;
    int            size;
    int64_t        pts;
} WriterSlot;

static WriterSlot        g_wslot[WRITER_RING_SIZE];
static int               g_wHead = 0;
static int               g_wTail = 0;
static int               g_wCount = 0;
static CRITICAL_SECTION  g_wLock;
static HANDLE            g_wNotEmpty   = NULL;
static HANDLE            g_wWriterThr  = NULL;
static volatile LONG     g_wRunning    = 0;
static volatile LONG     g_wDrops      = 0;
static volatile LONG     g_wWriteError = 0;
static volatile LONG     g_wInFlight   = 0;
static volatile LONG     g_wRingInited = 0;

static AVFormatContext*  g_wFmt    = NULL;
static AVStream*         g_wStream = NULL;

static void WriterRingInit(void) {
    if (InterlockedCompareExchange(&g_wRingInited, 1, 0) != 0) return;
    InitializeCriticalSection(&g_wLock);
    g_wNotEmpty = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_wHead = g_wTail = g_wCount = 0;
    memset(g_wslot, 0, sizeof(g_wslot));
}

static void WriterRingFree(void) {
    if (InterlockedCompareExchange(&g_wRingInited, 0, 1) != 1) return;
    EnterCriticalSection(&g_wLock);
    for (int i = 0; i < g_wCount; i++) {
        int idx = (g_wHead + i) % WRITER_RING_SIZE;
        if (g_wslot[idx].data) { tjFree(g_wslot[idx].data); g_wslot[idx].data = NULL; }
    }
    g_wHead = g_wTail = g_wCount = 0;
    LeaveCriticalSection(&g_wLock);
    if (g_wNotEmpty) { CloseHandle(g_wNotEmpty); g_wNotEmpty = NULL; }
    DeleteCriticalSection(&g_wLock);
}

static void WriterRingPush(unsigned char* jpeg, int size, int64_t pts) {
    EnterCriticalSection(&g_wLock);
    if (g_wCount == WRITER_RING_SIZE) {
        if (g_wslot[g_wHead].data) tjFree(g_wslot[g_wHead].data);
        g_wslot[g_wHead].data = NULL;
        g_wHead = (g_wHead + 1) % WRITER_RING_SIZE;
        g_wCount--;
        InterlockedIncrement(&g_wDrops);
    }
    g_wslot[g_wTail].data = jpeg;
    g_wslot[g_wTail].size = size;
    g_wslot[g_wTail].pts  = pts;
    g_wTail = (g_wTail + 1) % WRITER_RING_SIZE;
    g_wCount++;
    LeaveCriticalSection(&g_wLock);
    SetEvent(g_wNotEmpty);
}

static void WriterRingDrain(void) {
    for (;;) {
        EnterCriticalSection(&g_wLock);
        int empty = (g_wCount == 0);
        LeaveCriticalSection(&g_wLock);
        if (empty && InterlockedCompareExchange(&g_wInFlight, 0, 0) == 0) return;
        Sleep(1);
    }
}

static unsigned int __stdcall WriterThreadProc(void* param) {
    (void)param;
    AVPacket* pkt = av_packet_alloc();

    while (InterlockedCompareExchange(&g_wRunning, 1, 1)) {
        DWORD wr = WaitForSingleObject(g_wNotEmpty, 50);
        if (wr == WAIT_TIMEOUT) continue;

        WriterSlot s;
        EnterCriticalSection(&g_wLock);
        if (g_wCount == 0) { LeaveCriticalSection(&g_wLock); continue; }
        s = g_wslot[g_wHead];
        g_wslot[g_wHead].data = NULL;
        g_wHead = (g_wHead + 1) % WRITER_RING_SIZE;
        g_wCount--;
        LeaveCriticalSection(&g_wLock);

        if (g_wFmt && g_wStream && s.data && pkt) {
            InterlockedExchange(&g_wInFlight, 1);
            av_packet_unref(pkt);
            pkt->data = s.data;
            pkt->size = s.size;
            pkt->pts  = s.pts;
            pkt->dts  = s.pts;
            pkt->stream_index = g_wStream->index;
            av_packet_rescale_ts(pkt, (AVRational){1, 1000000},
                                 g_wStream->time_base);
            if (av_write_frame(g_wFmt, pkt) < 0) {
                InterlockedExchange(&g_wWriteError, 1);
            }
            av_packet_unref(pkt);
            InterlockedExchange(&g_wInFlight, 0);
        }
        if (s.data) tjFree(s.data);
    }

    EnterCriticalSection(&g_wLock);
    for (int i = 0; i < g_wCount; i++) {
        int idx = (g_wHead + i) % WRITER_RING_SIZE;
        if (g_wslot[idx].data) tjFree(g_wslot[idx].data);
    }
    g_wHead = g_wTail = g_wCount = 0;
    LeaveCriticalSection(&g_wLock);

    if (pkt) av_packet_free(&pkt);
    return 0;
}

// ============================================================================
// Filter graph builder (gfx / ddagrab)
// ============================================================================
static int BuildFilterGraph(CaptureAPI api, int fps,
                            AVFilterGraph** out_graph, AVFilterContext** out_sink)
{
    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) return -1;

    const AVFilter* src  = NULL;
    const AVFilter* hwdl = avfilter_get_by_name("hwdownload");
    const AVFilter* fmt  = avfilter_get_by_name("format");
    const AVFilter* sink = avfilter_get_by_name("buffersink");
    if (!hwdl || !fmt || !sink) { avfilter_graph_free(&graph); return -1; }

    char src_args[256];
    if (api == API_GFX) {
        src = avfilter_get_by_name("gfxcapture");
        snprintf(src_args, sizeof(src_args),
                 "monitor_idx=0:capture_cursor=1:max_framerate=%d", fps);
    } else if (api == API_DDAGRAB) {
        src = avfilter_get_by_name("ddagrab");
        snprintf(src_args, sizeof(src_args),
                 "output_idx=0:draw_mouse=1:framerate=%d", fps);
    } else {
        avfilter_graph_free(&graph);
        return -1;
    }
    if (!src) { avfilter_graph_free(&graph); return -1; }

    AVFilterContext *src_ctx = NULL, *hwdl_ctx = NULL, *fmt_ctx = NULL, *sink_ctx = NULL;
    if (avfilter_graph_create_filter(&src_ctx, src, "src", src_args, NULL, graph) < 0) goto fail;
    if (avfilter_graph_create_filter(&hwdl_ctx, hwdl, "hwdl", NULL, NULL, graph) < 0) goto fail;
    if (avfilter_graph_create_filter(&fmt_ctx, fmt, "fmt", "pix_fmts=bgra", NULL, graph) < 0) goto fail;
    if (avfilter_graph_create_filter(&sink_ctx, sink, "sink", NULL, NULL, graph) < 0) goto fail;

    if (avfilter_link(src_ctx,  0, hwdl_ctx, 0) < 0) goto fail;
    if (avfilter_link(hwdl_ctx, 0, fmt_ctx,  0) < 0) goto fail;
    if (avfilter_link(fmt_ctx,  0, sink_ctx, 0) < 0) goto fail;
    if (avfilter_graph_config(graph, NULL) < 0) goto fail;

    *out_graph = graph;
    *out_sink  = sink_ctx;
    return 0;
fail:
    avfilter_graph_free(&graph);
    return -1;
}

// ============================================================================
// One capture segment: MKV with a single MJPEG video stream
// ============================================================================
typedef struct {
    AVFormatContext* fmtout;
    AVStream*        st;
    int              w, h, fps;
} Segment;

static int OpenSegment(Segment* seg, const char* path, int w, int h, int fps) {
    memset(seg, 0, sizeof(*seg));
    seg->w = w; seg->h = h; seg->fps = fps;

    if (avformat_alloc_output_context2(&seg->fmtout, NULL, "matroska", path) < 0)
        return -1;
    seg->st = avformat_new_stream(seg->fmtout, NULL);
    seg->st->codecpar->codec_id   = AV_CODEC_ID_MJPEG;
    seg->st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    seg->st->codecpar->width      = w;
    seg->st->codecpar->height     = h;
    seg->st->codecpar->format     = AV_PIX_FMT_YUV420P;

    seg->st->time_base            = (AVRational){1, 1000000};
    seg->st->avg_frame_rate       = (AVRational){fps, 1};

    if (avio_open(&seg->fmtout->pb, path, AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(seg->fmtout);
        seg->fmtout = NULL;
        return -1;
    }
    if (avformat_write_header(seg->fmtout, NULL) < 0) {
        avio_closep(&seg->fmtout->pb);
        avformat_free_context(seg->fmtout);
        seg->fmtout = NULL;
        return -1;
    }
    return 0;
}

static void CloseSegment(Segment* seg) {
    if (!seg->fmtout) return;
    av_write_trailer(seg->fmtout);
    if (seg->fmtout->pb) avio_closep(&seg->fmtout->pb);
    avformat_free_context(seg->fmtout);
    seg->fmtout = NULL;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    // Raise the scheduler tick to 1 ms for the life of this process. Without
    // this, Sleep(1) is 15.6 ms and the capture loop can blow the 16.6 ms
    // frame budget on Balanced power plans.
    timeBeginPeriod(1);

    avdevice_register_all();

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <output.mkv> [quality=85] [api=auto] [ffmpeg-exe]\n"
            "  api = auto | gfx | ddagrab | gdi\n"
            "  Stop: press 'q' in this console (or send CTRL_BREAK).\n"
            "  Pause: press 'p'.\n"
            "  This build is video-only. Audio is owned by the parent process.\n",
            argv[0]);
        timeEndPeriod(1);
        return 1;
    }

    const char* outfile  = argv[1];
    int quality          = (argc > 2) ? atoi(argv[2]) : 85;
    const char* apiArg   = (argc > 3) ? argv[3]    : "auto";
    const char* ffmpegEx = (argc > 4) ? argv[4]    : "maxsengine.exe";

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    CaptureAPI requested = API_AUTO;
    if      (strcmp(apiArg, "gfx")     == 0) requested = API_GFX;
    else if (strcmp(apiArg, "ddagrab") == 0) requested = API_DDAGRAB;
    else if (strcmp(apiArg, "gdi")     == 0) requested = API_GDI;

    CaptureAPI api = PickAPI(requested);
    int fps = GetTargetFPS(api);

    QueryPerformanceFrequency(&g_qpcFreq);
	
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (si.dwNumberOfProcessors <= 2) {
            g_boostConsolePriority = 1;
        }
    }

    g_tjc = tjInitCompress();
    if (!g_tjc) {
        fprintf(stderr, "[maxenc] tjInitCompress failed\n");
        timeEndPeriod(1);
        return 1;
    }

    // The main thread is the capture thread. Nudge it above normal so the
    // OS scheduler does not preempt it for >16.6 ms under load.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    uintptr_t keyThread = _beginthreadex(NULL, 0, ConsoleKeyThread, NULL, 0, NULL);

    char basePath[MAX_PATH];
    strncpy(basePath, outfile, sizeof(basePath) - 1);
    basePath[sizeof(basePath) - 1] = 0;
    size_t bl = strlen(basePath);
    if (bl > 4 && _stricmp(basePath + bl - 4, ".mkv") == 0) basePath[bl - 4] = 0;

    // ---- Capture side (filter graph or GDI demuxer) ----
    AVFilterGraph*   graph     = NULL;
    AVFilterContext* sink_ctx  = NULL;
    AVFrame*         frame     = NULL;
    AVFormatContext* input_ctx = NULL;
    AVCodecContext*  bmp_dec   = NULL;
    AVFrame*         bmp_frame = NULL;
    AVPacket*        in_pkt    = NULL;
    int w = 0, h = 0;

    if (api == API_GDI) {
        const AVInputFormat* ifmt = av_find_input_format("gdigrab");
        if (!ifmt) { fprintf(stderr, "[maxenc] gdigrab not available\n"); goto fatal; }

        AVDictionary* opts = NULL;
        char fpsStr[8];
        snprintf(fpsStr, sizeof(fpsStr), "%d", fps);
        av_dict_set(&opts, "framerate", fpsStr, 0);
        av_dict_set(&opts, "draw_mouse", "1", 0);
        if (avformat_open_input(&input_ctx, "desktop", ifmt, &opts) < 0) {
            fprintf(stderr, "[maxenc] gdigrab open failed\n");
            av_dict_free(&opts);
            goto fatal;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(input_ctx, NULL) < 0) goto fatal;

        int v_idx = -1;
        for (unsigned i = 0; i < input_ctx->nb_streams; i++) {
            if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                v_idx = i; break;
            }
        }
        if (v_idx < 0) goto fatal;

        AVCodecParameters* par = input_ctx->streams[v_idx]->codecpar;
        w = par->width; h = par->height;

        const AVCodec* dec_codec = avcodec_find_decoder(par->codec_id);
        if (!dec_codec) goto fatal;
        bmp_dec = avcodec_alloc_context3(dec_codec);
        if (!bmp_dec) goto fatal;
        avcodec_parameters_to_context(bmp_dec, par);
        if (avcodec_open2(bmp_dec, dec_codec, NULL) < 0) goto fatal;
        bmp_frame = av_frame_alloc();
        in_pkt    = av_packet_alloc();
        if (!bmp_frame || !in_pkt) goto fatal;
    } else {
        if (BuildFilterGraph(api, fps, &graph, &sink_ctx) < 0) {
            fprintf(stderr, "[maxenc] filter graph build failed for %s\n", ApiName(api));
            goto fatal;
        }
        frame = av_frame_alloc();
        if (!frame) goto fatal;
        if (av_buffersink_get_frame(sink_ctx, frame) < 0) goto fatal;
        w = frame->width; h = frame->height;
        av_frame_unref(frame);
    }

    fprintf(stderr, "[maxenc] api=%s, %dx%d @ %d fps (wall-clock PTS, BGRX, q=%d)\n",
            ApiName(api), w, h, fps, quality);

    // ---- Writer thread: decouples capture from disk I/O ----
    WriterRingInit();
    InterlockedExchange(&g_wRunning, 1);
    g_wWriterThr = (HANDLE)_beginthreadex(NULL, 0, WriterThreadProc, NULL, 0, NULL);
    if (!g_wWriterThr) {
        fprintf(stderr, "[maxenc] writer thread start failed\n");
        goto fatal;
    }

    // ---- Segment loop ----
    int segmentIndex = 0;
    int anyFrameWritten = 0;
    int64_t globalFrameCounter = 0;

    while (!InterlockedCompareExchange(&g_stopRequested, 1, 1)) {
        Segment seg;
        char segPath[MAX_PATH];
        if (segmentIndex == 0) {
            snprintf(segPath, sizeof(segPath), "%s", outfile);
        } else {
            snprintf(segPath, sizeof(segPath), "%s_seg%d.mkv", basePath, segmentIndex);
        }
        if (OpenSegment(&seg, segPath, w, h, fps) < 0) {
            fprintf(stderr, "[maxenc] segment open failed: %s\n", segPath);
            break;
        }
        fprintf(stderr, "[maxenc] segment %d -> %s\n", segmentIndex, segPath);

        g_wFmt    = seg.fmtout;
        g_wStream = seg.st;

        InterlockedExchange(&g_pauseRequested, 0);

        int64_t segT0     = -1;
        int64_t lastPtsUs = -1;
        int localFrameIndex = 0;

        while (!InterlockedCompareExchange(&g_stopRequested, 1, 1)) {
            if (InterlockedCompareExchange(&g_pauseRequested, 1, 1)) {
                break;
            }

            const unsigned char* bgrx = NULL;
            int stride = 0;
            int bw = w, bh = h;

            if (api == API_GDI) {
                if (av_read_frame(input_ctx, in_pkt) < 0) { Sleep(1); continue; }
                int got = 0;
                if (avcodec_send_packet(bmp_dec, in_pkt) == 0) {
                    if (avcodec_receive_frame(bmp_dec, bmp_frame) == 0) {
                        bgrx = bmp_frame->data[0];
                        stride = bmp_frame->linesize[0];
                        bw = bmp_frame->width;
                        bh = bmp_frame->height;
                        got = 1;
                    }
                }
                av_packet_unref(in_pkt);
                if (!got) { Sleep(1); continue; }
            } else {
                if (av_buffersink_get_frame(sink_ctx, frame) < 0) { Sleep(1); continue; }
                bgrx = frame->data[0];
                stride = frame->linesize[0];
                bw = frame->width;
                bh = frame->height;
            }

            unsigned long jpegSize = 0;
            unsigned char* jpeg = EncodeFrameToMJPEG(g_tjc, bgrx, bw, bh, stride, quality, &jpegSize);

            if (api == API_GDI) av_frame_unref(bmp_frame);
            else                av_frame_unref(frame);

            if (!jpeg || jpegSize == 0) continue;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (segT0 < 0) segT0 = now.QuadPart;

            int64_t pts_us = (now.QuadPart - segT0)
                             * 1000000 / g_qpcFreq.QuadPart;

            if (pts_us <= lastPtsUs) pts_us = lastPtsUs + 1;
            lastPtsUs = pts_us;

            // First real frame of segment 0: write the t0 marker so the
            // parent can start audio capture at the same real-world moment.
            if (segmentIndex == 0 && localFrameIndex == 0) {
                char markerPath[MAX_PATH];
                snprintf(markerPath, sizeof(markerPath), "%s.t0", outfile);
                FILE* mf = NULL;
                fopen_s(&mf, markerPath, "wb");
                if (mf) {
                    long long tick = (long long)now.QuadPart;
                    fwrite(&tick, sizeof(tick), 1, mf);
                    fclose(mf);
                }
            }

            // Hand off to writer thread; ownership of jpeg transfers.
            WriterRingPush(jpeg, (int)jpegSize, pts_us);

            if (InterlockedCompareExchange(&g_wWriteError, 1, 1)) {
                fprintf(stderr, "[maxenc] writer reported a write error\n");
                InterlockedExchange(&g_stopRequested, 1);
                break;
            }
            anyFrameWritten = 1;
            localFrameIndex++;
            globalFrameCounter++;
        }

        // Flush queued frames for this segment before freeing its context.
        WriterRingDrain();
        g_wFmt    = NULL;
        g_wStream = NULL;

        CloseSegment(&seg);

        if (InterlockedCompareExchange(&g_stopRequested, 1, 1)) break;
        if (InterlockedCompareExchange(&g_pauseRequested, 1, 1)) {
            segmentIndex++;
            continue;
        }
    }

done:
    InterlockedExchange(&g_stopRequested, 1);

    // Stop writer thread and release anything still queued.
    InterlockedExchange(&g_wRunning, 0);
    if (g_wWriterThr) {
        SetEvent(g_wNotEmpty);
        WaitForSingleObject(g_wWriterThr, 5000);
        CloseHandle(g_wWriterThr);
        g_wWriterThr = NULL;
    }
    WriterRingFree();

    if (g_wDrops > 0) {
        fprintf(stderr, "[maxenc] %ld frames dropped (writer ring overflow)\n",
                (long)g_wDrops);
    }

    // ---- Concat segments into the final output ----
    if (!anyFrameWritten) {
        fprintf(stderr, "[maxenc] no frames were written\n");
        goto fatal;
    }

    if (segmentIndex == 0) {
        // Segment 0 already wrote directly to outfile. Nothing to move.
    } else {
        char listPath[MAX_PATH];
        snprintf(listPath, sizeof(listPath), "%s_segments.txt", basePath);
        FILE* f = NULL;
        fopen_s(&f, listPath, "w");
        if (!f) { fprintf(stderr, "[maxenc] concat list failed\n"); goto fatal; }

        // Segment 0 wrote directly to outfile (no suffix). Additional
        // segments from 'p' wrote to basePath_segN.mkv for N >= 1.
        fprintf(f, "file '%s'\r\n", outfile);
        for (int i = 1; i <= segmentIndex; i++) {
            char segPath[MAX_PATH];
            snprintf(segPath, sizeof(segPath), "%s_seg%d.mkv", basePath, i);
            if (GetFileAttributesA(segPath) != INVALID_FILE_ATTRIBUTES)
                fprintf(f, "file '%s'\r\n", segPath);
        }
        fclose(f);

        // Concat output goes to a temp path because outfile is an input.
        char concatOut[MAX_PATH];
        snprintf(concatOut, sizeof(concatOut), "%s_concat.mkv", basePath);

        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
            "\"%s\" -y -hide_banner -loglevel error -f concat -safe 0 -i \"%s\" -c copy \"%s\"",
            ffmpegEx, listPath, concatOut);

        STARTUPINFOA csi = { sizeof(csi) };
        csi.dwFlags = STARTF_USESHOWWINDOW;
        csi.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION cpi = {0};
        BOOL ok = FALSE;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &csi, &cpi)) {
            WaitForSingleObject(cpi.hProcess, INFINITE);
            DWORD ec = 0;
            GetExitCodeProcess(cpi.hProcess, &ec);
            ok = (ec == 0);
            CloseHandle(cpi.hProcess);
            CloseHandle(cpi.hThread);
        }

        if (ok) {
            DeleteFileA(outfile);
            if (!MoveFileA(concatOut, outfile)) {
                fprintf(stderr, "[maxenc] failed to move concat output over %s\n", outfile);
            }
        } else {
            fprintf(stderr, "[maxenc] concat failed, keeping original segment 0\n");
            DeleteFileA(concatOut);
        }

        for (int i = 1; i <= segmentIndex; i++) {
            char segPath[MAX_PATH];
            snprintf(segPath, sizeof(segPath), "%s_seg%d.mkv", basePath, i);
            DeleteFileA(segPath);
        }
        DeleteFileA(listPath);
    }

    fprintf(stderr, "[maxenc] done: %lld frames written to %s\n",
            (long long)globalFrameCounter, outfile);
    if (g_tjc) { tjDestroy(g_tjc); g_tjc = NULL; }
    if (keyThread) CloseHandle((HANDLE)keyThread);
    timeEndPeriod(1);
    return 0;

fatal:
    InterlockedExchange(&g_stopRequested, 1);

    InterlockedExchange(&g_wRunning, 0);
    if (g_wWriterThr) {
        SetEvent(g_wNotEmpty);
        WaitForSingleObject(g_wWriterThr, 5000);
        CloseHandle(g_wWriterThr);
        g_wWriterThr = NULL;
    }
    WriterRingFree();

    if (frame)     av_frame_free(&frame);
    if (bmp_frame) av_frame_free(&bmp_frame);
    if (in_pkt)    av_packet_free(&in_pkt);
    if (bmp_dec)   avcodec_free_context(&bmp_dec);
    if (input_ctx) avformat_close_input(&input_ctx);
    if (graph)     avfilter_graph_free(&graph);
    if (g_tjc)     { tjDestroy(g_tjc); g_tjc = NULL; }
    if (keyThread) CloseHandle((HANDLE)keyThread);
    timeEndPeriod(1);
    return 1;
}
