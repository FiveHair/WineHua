/*
 * WineHua Audio Test v3 - WASAPI audio player
 *
 * Playback path (direct mmdevapi):
 *   IAudioClient -> mmdevapi -> wineohos.drv
 *     -> AudioBootstrapFd IPC -> AudioBroker -> OH_AudioRenderer -> Speaker
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -o winehua_audio_test.exe main.c \
 *       -lcomctl32 -lole32 -mwindows -static
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_IE 0x0500
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

/* -- Constants -- */
#define TARGET_SAMPLE_RATE   48000
#define TARGET_CHANNELS      2
#define TARGET_BITS          16
#define TARGET_BLOCK_ALIGN   (TARGET_CHANNELS * (TARGET_BITS / 8))
#define PLAY_BUFFER_US       30000000LL  /* 3s WASAPI buffer */

/* -- Control IDs -- */
#define IDC_FILE_LIST   1001
#define IDC_BTN_PLAY    1002
#define IDC_BTN_STOP    1003
#define IDC_STATUS      1004

/* -- UI handles -- */
static HWND g_hList = NULL, g_hStatus = NULL, g_hBtnPlay = NULL, g_hBtnStop = NULL;
static char g_musicDir[MAX_PATH];

/* -- Debug log -- */
static char g_logPath[MAX_PATH];

static void dbg_init(void)
{
    GetModuleFileNameA(NULL, g_logPath, sizeof(g_logPath));
    char *p = strrchr(g_logPath, '\\');
    if (p) p[1] = '\0';
    strncat(g_logPath, "winehua_audio_test.log",
            sizeof(g_logPath) - strlen(g_logPath) - 1);
    FILE *f = fopen(g_logPath, "w");
    if (f) { fprintf(f, "=== WineHua Audio Test v3 (WASAPI) ===\n"); fclose(f); }
}

static void dbg_log(const char *fmt, ...)
{
    va_list va; va_start(va, fmt);
    char buf[1024]; vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    FILE *f = fopen(g_logPath, "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

static const char *hr_name(HRESULT hr)
{
    switch (hr) {
    case S_OK: return "S_OK";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_INVALIDARG: return "E_INVALIDARG";
    case AUDCLNT_E_DEVICE_INVALIDATED: return "DEVICE_INVALIDATED";
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return "SERVICE_NOT_RUNNING";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "UNSUPPORTED_FORMAT";
    case AUDCLNT_E_DEVICE_IN_USE: return "DEVICE_IN_USE";
    default: return "";
    }
}

/* -- Playback state -- */
typedef enum { STATE_IDLE, STATE_PLAYING } PlayState;

static struct {
    PlayState         state;
    CRITICAL_SECTION  lock;
    HANDLE            thread;
    volatile LONG     stopRequest;
    short            *pcmData;
    DWORD             pcmFrames;
    char              currentFile[MAX_PATH];
} g_player;

/* ── Linear resample any rate/ch → 48000Hz stereo s16le ── */
static short *ResampleTo48000(const short *src, DWORD srcFrames,
                               int srcRate, int srcCh, DWORD *outFrames)
{
    if (srcRate == TARGET_SAMPLE_RATE && srcCh == TARGET_CHANNELS) {
        /* passthrough */
        short *dst = (short*)malloc(srcFrames * TARGET_BLOCK_ALIGN);
        if (!dst) return NULL;
        /* mono → stereo */
        if (srcCh == 1) {
            for (DWORD i = 0; i < srcFrames; i++) {
                dst[i * 2]     = src[i];
                dst[i * 2 + 1] = src[i];
            }
        } else {
            memcpy(dst, src, srcFrames * TARGET_BLOCK_ALIGN);
        }
        *outFrames = srcFrames;
        return dst;
    }

    double ratio = (double)TARGET_SAMPLE_RATE / (double)srcRate;
    DWORD dstFrames = (DWORD)((double)srcFrames * ratio) + 8;
    short *dst = (short*)calloc(dstFrames, TARGET_BLOCK_ALIGN);
    if (!dst) return NULL;

    for (DWORD i = 0; i < dstFrames; i++) {
        double srcPos = (double)i / ratio;
        DWORD idx = (DWORD)srcPos;
        double frac = srcPos - (double)idx;

        for (int ch = 0; ch < TARGET_CHANNELS; ch++) {
            int srcChIdx = (srcCh == 1) ? 0 : ch;
            short s0 = (idx < srcFrames)
                ? src[idx * srcCh + srcChIdx] : 0;
            short s1 = (idx + 1 < srcFrames)
                ? src[(idx + 1) * srcCh + srcChIdx] : s0;
            float v = (float)s0 + (float)frac * (float)(s1 - s0);
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            dst[i * TARGET_CHANNELS + ch] = (short)v;
        }
    }

    *outFrames = dstFrames;
    return dst;
}

/* ── WAV parser ── */
typedef struct {
    short *pcm;
    DWORD  frames;
    int    sampleRate;
    int    channels;
    int    bitsPerSample;
} WavData;

static BOOL ParseWav(const char *filePath, WavData *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(filePath, "rb");
    if (!f) return FALSE;

    /* RIFF header */
    char riff[4];
    DWORD fileSize;
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0)
        { fclose(f); return FALSE; }
    fread(&fileSize, 4, 1, f);
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "WAVE", 4) != 0)
        { fclose(f); return FALSE; }

    /* find fmt and data chunks */
    int fmtFound = 0, dataFound = 0;
    DWORD dataSize = 0;

    while (!feof(f)) {
        char id[4];
        DWORD size;
        if (fread(id, 1, 4, f) != 4) break;
        fread(&size, 4, 1, f);

        if (memcmp(id, "fmt ", 4) == 0) {
            WORD audioFormat, numChannels, bitsPerSample;
            DWORD sampleRate, byteRate;
            WORD blockAlign;
            fread(&audioFormat, 2, 1, f);
            fread(&numChannels, 2, 1, f);
            fread(&sampleRate, 4, 1, f);
            fread(&byteRate, 4, 1, f);
            fread(&blockAlign, 2, 1, f);
            fread(&bitsPerSample, 2, 1, f);
            if (size > 16) fseek(f, size - 16, SEEK_CUR);

            if (audioFormat != 1) { fclose(f); return FALSE; } /* PCM only */
            out->sampleRate    = (int)sampleRate;
            out->channels      = (int)numChannels;
            out->bitsPerSample = (int)bitsPerSample;
            fmtFound = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            dataSize = size;
            dataFound = 1;
            break;
        } else {
            fseek(f, size, SEEK_CUR);
        }
    }

    if (!fmtFound || !dataFound) { fclose(f); return FALSE; }

    /* read PCM */
    DWORD totalSamples = dataSize / (out->bitsPerSample / 8);
    short *pcm = (short*)malloc(totalSamples * sizeof(short));
    if (!pcm) { fclose(f); return FALSE; }

    /* convert to s16 */
    if (out->bitsPerSample == 16) {
        fread(pcm, sizeof(short), totalSamples, f);
    } else if (out->bitsPerSample == 8) {
        BYTE *raw = (BYTE*)malloc(totalSamples);
        fread(raw, 1, totalSamples, f);
        for (DWORD i = 0; i < totalSamples; i++)
            pcm[i] = ((short)raw[i] - 128) << 8;
        free(raw);
    }
    fclose(f);

    out->pcm    = pcm;
    out->frames = totalSamples / out->channels;
    return TRUE;
}

/* ── MP3 decoder (minimp3) ── */
static short *DecodeMp3(const char *filePath, DWORD *outFrames,
                         int *outRate, int *outCh)
{
    /* read whole file */
    FILE *f = fopen(filePath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char*)malloc(size);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, size, f);
    fclose(f);

    /* decode */
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    mp3dec_frame_info_t info;
    float pcmFloat[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int totalSamples = 0;

    /* pass 1: count */
    int remaining = (int)size;
    unsigned char *pos = data;
    while (remaining > 0) {
        int samples = mp3dec_decode_frame(&mp3d, pos, remaining,
                                          pcmFloat, &info);
        if (samples <= 0) break;
        totalSamples += samples * info.channels;
        pos += info.frame_bytes;
        remaining -= info.frame_bytes;
    }

    if (totalSamples == 0) { free(data); return NULL; }

    /* pass 2: decode to s16 */
    short *pcm = (short*)malloc(totalSamples * sizeof(short));
    if (!pcm) { free(data); return NULL; }

    mp3dec_init(&mp3d);
    remaining = (int)size;
    pos = data;
    int offset = 0;
    while (remaining > 0) {
        int samples = mp3dec_decode_frame(&mp3d, pos, remaining,
                                          pcmFloat, &info);
        if (samples <= 0) break;
        for (int i = 0; i < samples * info.channels; i++) {
            float val = pcmFloat[i] * 32767.0f;
            if (val > 32767.0f) val = 32767.0f;
            if (val < -32768.0f) val = -32768.0f;
            pcm[offset + i] = (short)val;
        }
        offset += samples * info.channels;
        pos += info.frame_bytes;
        remaining -= info.frame_bytes;
    }

    free(data);

    *outFrames = offset / info.channels;
    *outRate   = info.hz;
    *outCh     = info.channels;
    return pcm;
}

/* ── WASAPI playback thread ── */
static DWORD WINAPI PlaybackThread(LPVOID param)
{
    (void)param;
    HRESULT hr;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    UINT32 bufFrames = 0;
    DWORD cursor = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    dbg_log("  WASAPI: CoInitializeEx → 0x%08lx", hr);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        dbg_log("FAIL: CoCreateInstance(MMDeviceEnumerator) → 0x%08lx %s", hr, hr_name(hr));
        goto cleanup;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetDefaultAudioEndpoint → 0x%08lx %s", hr, hr_name(hr));
        goto cleanup;
    }

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&client);
    if (FAILED(hr)) {
        dbg_log("FAIL: Activate(IAudioClient) → 0x%08lx %s", hr, hr_name(hr));
        goto cleanup;
    }

    {
        WAVEFORMATEX wfx = {0};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = TARGET_CHANNELS;
        wfx.nSamplesPerSec = TARGET_SAMPLE_RATE;
        wfx.wBitsPerSample = TARGET_BITS;
        wfx.nBlockAlign = TARGET_BLOCK_ALIGN;
        wfx.nAvgBytesPerSec = TARGET_SAMPLE_RATE * TARGET_BLOCK_ALIGN;

        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                      0, PLAY_BUFFER_US, 0, &wfx, NULL);
        if (FAILED(hr)) {
            dbg_log("FAIL: IAudioClient_Initialize → 0x%08lx %s (rate=%lu ch=%d bits=%d)",
                    hr, hr_name(hr), TARGET_SAMPLE_RATE, TARGET_CHANNELS, TARGET_BITS);
            goto cleanup;
        }
    }

    hr = IAudioClient_GetBufferSize(client, &bufFrames);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetBufferSize → 0x%08lx", hr);
        goto cleanup;
    }
    dbg_log("  WASAPI: bufferSize=%u frames", bufFrames);

    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void**)&render);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetService(RenderClient) → 0x%08lx", hr);
        goto cleanup;
    }

    /* pre-fill */
    {
        BYTE *dst = NULL;
        UINT32 fill = bufFrames;
        EnterCriticalSection(&g_player.lock);
        DWORD rem = g_player.pcmFrames - cursor;
        if (fill > rem) fill = (UINT32)rem;
        LeaveCriticalSection(&g_player.lock);

        hr = IAudioRenderClient_GetBuffer(render, bufFrames, &dst);
        if (SUCCEEDED(hr)) {
            EnterCriticalSection(&g_player.lock);
            if (fill) memcpy(dst, g_player.pcmData + cursor * TARGET_CHANNELS, fill * TARGET_BLOCK_ALIGN);
            if (fill < bufFrames) memset(dst + fill * TARGET_BLOCK_ALIGN, 0, (bufFrames - fill) * TARGET_BLOCK_ALIGN);
            cursor += fill;
            LeaveCriticalSection(&g_player.lock);
            IAudioRenderClient_ReleaseBuffer(render, bufFrames, 0);
        }
    }

    hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        dbg_log("FAIL: IAudioClient_Start → 0x%08lx", hr);
        goto cleanup;
    }
    dbg_log("  WASAPI: started, totalFrames=%lu", g_player.pcmFrames);

    /* main loop: feed data, auto-stop when done+drained */
    int autoStop = 0;
    while (!g_player.stopRequest && !autoStop) {
        UINT32 padding = 0;
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr)) { dbg_log("  GetCurrentPadding failed -> 0x%08lx", hr); break; }

        if (padding >= bufFrames) { Sleep(5); continue; }

        UINT32 avail = bufFrames - padding;
        EnterCriticalSection(&g_player.lock);
        DWORD rem = g_player.pcmFrames - cursor;
        UINT32 chunk = (rem < avail) ? (UINT32)rem : avail;
        LeaveCriticalSection(&g_player.lock);

        if (!chunk) {
            /* all data fed - check if buffer is drained */
            if (padding == 0) autoStop = 1;
            else Sleep(20);
            continue;
        }

        BYTE *dst = NULL;
        hr = IAudioRenderClient_GetBuffer(render, chunk, &dst);
        if (FAILED(hr)) { dbg_log("  GetBuffer -> 0x%08lx", hr); break; }

        EnterCriticalSection(&g_player.lock);
        memcpy(dst, g_player.pcmData + cursor * TARGET_CHANNELS, chunk * TARGET_BLOCK_ALIGN);
        cursor += chunk;
        LeaveCriticalSection(&g_player.lock);

        IAudioRenderClient_ReleaseBuffer(render, chunk, 0);
    }

    /* drain */
    if (!g_player.stopRequest) {
        UINT32 padding = 1;
        for (int d = 0; padding && d < 200; d++) {
            Sleep(15);
            if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) break;
        }
        dbg_log("  WASAPI: drain done, padding=%u", padding);
    }

    IAudioClient_Stop(client);
    dbg_log("  WASAPI: stopped, %lu frames played", cursor);

cleanup:
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize();
    return 0;
}

/* ── Start WASAPI playback ── */
static void StopPlayback(void);

static BOOL StartPlayback(const char *fullPath)
{
    dbg_log("--- StartPlayback: %s ---", fullPath);
    StopPlayback();

    /* check file */
    {
        FILE *test = fopen(fullPath, "rb");
        if (!test) { dbg_log("FAIL: fopen (errno=%d)", errno); return FALSE; }
        fseek(test, 0, SEEK_END);
        dbg_log("  file size=%ld", ftell(test));
        fclose(test);
    }

    short *rawPcm = NULL;
    DWORD rawFrames = 0;
    int   rawRate = 0, rawCh = 0;
    const char *ext = strrchr(fullPath, '.');
    BOOL isMp3 = ext && (_stricmp(ext, ".mp3") == 0);

    if (isMp3) {
        dbg_log("  decoding MP3...");
        rawPcm = DecodeMp3(fullPath, &rawFrames, &rawRate, &rawCh);
        if (!rawPcm) { dbg_log("FAIL: DecodeMp3"); return FALSE; }
        dbg_log("  MP3: %lu frames %dHz/%dch", rawFrames, rawRate, rawCh);
    } else {
        dbg_log("  parsing WAV...");
        WavData wav;
        if (!ParseWav(fullPath, &wav)) { dbg_log("FAIL: ParseWav"); return FALSE; }
        rawPcm = wav.pcm; rawFrames = wav.frames;
        rawRate = wav.sampleRate; rawCh = wav.channels;
        dbg_log("  WAV: %lu frames %dHz/%dch/%dbit", rawFrames, rawRate, rawCh, wav.bitsPerSample);
    }

    if (!rawPcm) { dbg_log("FAIL: no PCM"); return FALSE; }

    /* resample */
    dbg_log("  resampling -> 48000Hz/2ch/s16le...");
    {
        short firstSample = rawPcm[0];
        short lastSample = rawPcm[rawFrames * rawCh - 1];
        dbg_log("  raw PCM: first=%d last=%d", (int)firstSample, (int)lastSample);
    }
    DWORD outFrames = 0;
    g_player.pcmData = ResampleTo48000(rawPcm, rawFrames, rawRate, rawCh, &outFrames);
    free(rawPcm);
    if (!g_player.pcmData) { dbg_log("FAIL: resample"); return FALSE; }
    g_player.pcmFrames = outFrames;
    dbg_log("  resampled: %lu frames (%.1fs)", outFrames, (double)outFrames / TARGET_SAMPLE_RATE);
    {
        short first = g_player.pcmData[0];
        short last = g_player.pcmData[(outFrames-1)*TARGET_CHANNELS];
        dbg_log("  resampled PCM: first=%d last=%d", (int)first, (int)last);
    }

    /* start WASAPI thread */
    g_player.stopRequest = FALSE;
    g_player.state = STATE_PLAYING;
    strncpy(g_player.currentFile, fullPath, sizeof(g_player.currentFile) - 1);
    g_player.thread = CreateThread(NULL, 0, PlaybackThread, NULL, 0, NULL);
    dbg_log("  thread started (WASAPI)");
    return g_player.thread != NULL;
}

/* ── Stop playback ── */
static void StopPlayback(void)
{
    if (g_player.state != STATE_PLAYING) return;

    InterlockedExchange(&g_player.stopRequest, TRUE);
    if (g_player.thread) {
        WaitForSingleObject(g_player.thread, 4000);
        CloseHandle(g_player.thread);
        g_player.thread = NULL;
    }

    free(g_player.pcmData);
    g_player.pcmData   = NULL;
    g_player.pcmFrames = 0;
    g_player.state     = STATE_IDLE;
}

/* ── UI helpers ── */
static void ScanMusicDir(void)
{
    ListView_DeleteAllItems(g_hList);

    const char *exts[] = { "*.mp3", "*.wav" };
    for (int e = 0; e < 2; e++) {
        char searchPath[MAX_PATH];
        WIN32_FIND_DATAA fd;
        snprintf(searchPath, sizeof(searchPath), "%s\\%s", g_musicDir, exts[e]);
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                LVITEMA lvi = {0};
                lvi.mask = LVIF_TEXT;
                lvi.iItem = 9999;
                lvi.pszText = fd.cFileName;
                ListView_InsertItem(g_hList, &lvi);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
}

/* get selected full path */
static BOOL GetSelectedFile(char *out, size_t outSize)
{
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return FALSE;
    char name[MAX_PATH] = {0};
    LVITEMA lvi = {0};
    lvi.iSubItem = 0;
    lvi.pszText = name;
    lvi.cchTextMax = sizeof(name);
    SendMessageA(g_hList, LVM_GETITEMTEXTA, (WPARAM)sel, (LPARAM)&lvi);
    snprintf(out, outSize, "%s\\%s", g_musicDir, name);
    return TRUE;
}

/* update status bar */
static void UpdateUI(const char *status)
{
    SetWindowTextA(g_hStatus, status);
    BOOL playing = (g_player.state == STATE_PLAYING);
    EnableWindow(g_hBtnPlay, !playing);
    EnableWindow(g_hBtnStop, playing);
}

/* play action */
static void DoPlay(void)
{
    char path[MAX_PATH];
    if (!GetSelectedFile(path, sizeof(path))) return;

    char buf[MAX_PATH + 64];
    const char *name = strrchr(path, '\\');
    snprintf(buf, sizeof(buf), "decoding: %s ...", name ? name + 1 : path);
    UpdateUI(buf);

    if (StartPlayback(path)) {
        snprintf(buf, sizeof(buf), "> PLAY: %s", name ? name + 1 : path);
        UpdateUI(buf);
    } else {
        UpdateUI("! FAILED - see winehua_audio_test.log");
    }
}

static void DoStop(void)
{
    StopPlayback();
    UpdateUI("STOPPED");
}

/* ── Window proc ── */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        /* ListView */
        g_hList = CreateWindowA(WC_LISTVIEWA, "",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                10, 10, 500, 280, hWnd,
                                (HMENU)IDC_FILE_LIST,
                                GetModuleHandle(NULL), NULL);
        LVCOLUMNA lvc = {0};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH;
        lvc.pszText = "File Name";
        lvc.cx = 480;
        ListView_InsertColumn(g_hList, 0, &lvc);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        /* buttons */
        g_hBtnPlay = CreateWindowA("BUTTON", "Play",
                                   WS_CHILD | WS_VISIBLE,
                                   10, 300, 100, 30, hWnd,
                                   (HMENU)IDC_BTN_PLAY,
                                   GetModuleHandle(NULL), NULL);
        g_hBtnStop = CreateWindowA("BUTTON", "Stop",
                                   WS_CHILD | WS_VISIBLE,
                                   120, 300, 100, 30, hWnd,
                                   (HMENU)IDC_BTN_STOP,
                                   GetModuleHandle(NULL), NULL);

        /* status bar */
        g_hStatus = CreateWindowA("STATIC", "Ready - double-click to play",
                                  WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                  230, 305, 280, 20, hWnd,
                                  (HMENU)IDC_STATUS,
                                  GetModuleHandle(NULL), NULL);

        /* music/ dir next to EXE */
        GetModuleFileNameA(NULL, g_musicDir, sizeof(g_musicDir));
        char *p = strrchr(g_musicDir, '\\');
        if (p) *p = '\0';
        strncat(g_musicDir, "\\music",
                sizeof(g_musicDir) - strlen(g_musicDir) - 1);

        ScanMusicDir();
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        MoveWindow(g_hList, 10, 10, w - 20, h - 60, TRUE);
        MoveWindow(g_hBtnPlay, 10, h - 45, 100, 30, TRUE);
        MoveWindow(g_hBtnStop, 120, h - 45, 100, 30, TRUE);
        MoveWindow(g_hStatus, 230, h - 40, w - 240, 20, TRUE);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_PLAY) DoPlay();
        else if (LOWORD(wParam) == IDC_BTN_STOP) DoStop();
        break;

    case WM_NOTIFY:
        if (((NMHDR*)lParam)->idFrom == IDC_FILE_LIST &&
            ((NMHDR*)lParam)->code == NM_DBLCLK) DoPlay();
        break;

    case WM_CLOSE:
        DoStop();
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

/* ── WinMain ── */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                    LPSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine;

    INITCOMMONCONTROLSEX icex = {
        sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icex);

    InitializeCriticalSection(&g_player.lock);
    g_player.state = STATE_IDLE;
    dbg_init();
    dbg_log("WineHua Audio Test v3 (WASAPI) starting...");

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WineHuaAudioTest";
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowA("WineHuaAudioTest",
                              "WineHua - Audio Test (WASAPI)",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              540, 400, NULL, NULL, hInst, NULL);

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    StopPlayback();
    DeleteCriticalSection(&g_player.lock);
    return (int)msg.wParam;
}