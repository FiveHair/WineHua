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
#include <shellapi.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <wchar.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

/* -- Test framework includes ---------------------------------------- */
#include "test_common.h"

/* Test module entry points declared in test_wasapi.c, test_dsound.c,
 * test_waveout.c, test_mci.c, test_midi.c (built & linked separately). */
void run_wasapi_tests(LogFn log);
void run_dsound_tests(LogFn log);
void run_waveout_tests(LogFn log);
void run_mci_tests(LogFn log);
void run_midi_tests(LogFn log);

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

/* Test panel control IDs */
#define IDC_BTN_WASAPI  2001
#define IDC_BTN_DSOUND  2002
#define IDC_BTN_WAVEOUT 2003
#define IDC_BTN_MCI     2004
#define IDC_BTN_MIDI    2005
#define IDC_BTN_ALL     2006
#define IDC_LOG_EDIT    2007
#define IDC_RESULT_LABEL 2008

/* -- UI handles -- */
static HWND g_hList = NULL, g_hStatus = NULL, g_hBtnPlay = NULL, g_hBtnStop = NULL;
static char g_musicDir[MAX_PATH];

/* Test panel handles */
static HWND g_hLogEdit = NULL, g_hResultLabel = NULL;

/* Test counters (updated by log_callback via PostMessage) */
static volatile LONG g_testPass = 0;
static volatile LONG g_testFail = 0;
static volatile LONG g_testRunning = 0;
static HANDLE g_testProcess = NULL;

/* Custom window message for test log output (posted from worker threads) */
#define WM_TEST_LOG (WM_USER + 100)
#define WM_TEST_FINISHED (WM_USER + 101)

#define AUDIO_TEST_POLL_MS 100
#define AUDIO_TEST_TIMEOUT_MS 60000
#define AUDIO_TEST_TIMEOUT_ALL_MS 180000
#define AUDIO_TEST_PENDING_MAX 2048

/* -- Debug log -- */
static char g_logPath[MAX_PATH];

static void post_test_log_line(const char *msg)
{
    if (g_hLogEdit) {
        size_t len = strlen(msg) + 1;
        char *copy = (char *)malloc(len);
        if (copy) {
            memcpy(copy, msg, len);
            PostMessageA(GetParent(g_hLogEdit), WM_TEST_LOG,
                         (WPARAM)len, (LPARAM)copy);
        }
    }
}

/* -- Log callback for test modules ---------------------------------- */
static void test_log_callback(const char *msg)
{
    post_test_log_line(msg);
    FILE *f = fopen(g_logPath, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

static void dbg_init(void)
{
    if (!g_logPath[0]) {
        GetModuleFileNameA(NULL, g_logPath, sizeof(g_logPath));
        char *p = strrchr(g_logPath, '\\');
        if (p) p[1] = '\0';
        strncat(g_logPath, "winehua_audio_test.log",
                sizeof(g_logPath) - strlen(g_logPath) - 1);
    }
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

static void update_result_label(const char *override_text)
{
    char label[128];

    if (!g_hResultLabel) return;
    if (override_text) {
        SetWindowTextA(g_hResultLabel, override_text);
        return;
    }

    snprintf(label, sizeof(label), "Results: %ld Passed / %ld Failed",
             (long)g_testPass, (long)g_testFail);
    SetWindowTextA(g_hResultLabel, label);
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

/* -- Linear resample any rate/ch -> 48000Hz stereo s16le -- */
static short *ResampleTo48000(const short *src, DWORD srcFrames,
                               int srcRate, int srcCh, DWORD *outFrames)
{
    if (srcRate == TARGET_SAMPLE_RATE && srcCh == TARGET_CHANNELS) {
        /* passthrough */
        short *dst = (short*)malloc(srcFrames * TARGET_BLOCK_ALIGN);
        if (!dst) return NULL;
        /* mono -> stereo */
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

/* -- WAV parser -- */
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

/* -- MP3 decoder (minimp3) -- */
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

/* -- WASAPI playback thread -- */
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
    dbg_log("  WASAPI: CoInitializeEx -> 0x%08lx", hr);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        dbg_log("FAIL: CoCreateInstance(MMDeviceEnumerator) -> 0x%08lx %s", hr, hr_name(hr));
        goto cleanup;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetDefaultAudioEndpoint -> 0x%08lx %s", hr, hr_name(hr));
        goto cleanup;
    }

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&client);
    if (FAILED(hr)) {
        dbg_log("FAIL: Activate(IAudioClient) -> 0x%08lx %s", hr, hr_name(hr));
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
            dbg_log("FAIL: IAudioClient_Initialize -> 0x%08lx %s (rate=%lu ch=%d bits=%d)",
                    hr, hr_name(hr), TARGET_SAMPLE_RATE, TARGET_CHANNELS, TARGET_BITS);
            goto cleanup;
        }
    }

    hr = IAudioClient_GetBufferSize(client, &bufFrames);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetBufferSize -> 0x%08lx", hr);
        goto cleanup;
    }
    dbg_log("  WASAPI: bufferSize=%u frames", bufFrames);

    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void**)&render);
    if (FAILED(hr)) {
        dbg_log("FAIL: GetService(RenderClient) -> 0x%08lx", hr);
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
        dbg_log("FAIL: IAudioClient_Start -> 0x%08lx", hr);
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

/* -- Start WASAPI playback -- */
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
    snprintf(g_player.currentFile, sizeof(g_player.currentFile), "%s", fullPath);
    g_player.thread = CreateThread(NULL, 0, PlaybackThread, NULL, 0, NULL);
    dbg_log("  thread started (WASAPI)");
    return g_player.thread != NULL;
}

/* -- Stop playback -- */
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

/* -- UI helpers -- */
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
    {
        size_t dir_len = strlen(g_musicDir);
        size_t name_len = strlen(name);

        if (dir_len + 1 + name_len + 1 > outSize)
            return FALSE;

        memcpy(out, g_musicDir, dir_len);
        out[dir_len] = '\\';
        memcpy(out + dir_len + 1, name, name_len + 1);
    }
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

/* -- Test runner helper ----------------------------------------------
 * Risky callback-heavy tests are executed in a child process so the
 * GUI remains responsive even if the Wine audio stack deadlocks. */
typedef void (*TestFunc)(LogFn);

struct named_test {
    const char *display_name;
    const char *cli_name;
    TestFunc func;
    DWORD timeout_ms;
};

struct test_process_param {
    HWND owner;
    HANDLE process;
    char log_path[MAX_PATH];
    char test_name[32];
    DWORD timeout_ms;
    long read_offset;
    char pending[AUDIO_TEST_PENDING_MAX];
    size_t pending_len;
};

static const struct named_test g_all_tests[] = {
    { "WASAPI",      "wasapi", run_wasapi_tests, AUDIO_TEST_TIMEOUT_MS },
    { "DirectSound", "dsound", run_dsound_tests, AUDIO_TEST_TIMEOUT_MS },
    { "waveOut",     "waveout", run_waveout_tests, AUDIO_TEST_TIMEOUT_MS },
    { "MCI",         "mci", run_mci_tests, AUDIO_TEST_TIMEOUT_MS },
    { "MIDI",        "midi", run_midi_tests, AUDIO_TEST_TIMEOUT_MS },
};

static const struct named_test *find_named_test(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_all_tests) / sizeof(g_all_tests[0]); i++) {
        if (_stricmp(name, g_all_tests[i].cli_name) == 0 ||
            _stricmp(name, g_all_tests[i].display_name) == 0)
            return &g_all_tests[i];
    }
    return NULL;
}

static void append_test_banner(const char *display_name)
{
    test_log_callback("");
    test_log_callback("===========================================");
    test_log_callback(display_name);
    test_log_callback("===========================================");
}

static int run_named_test(const struct named_test *test)
{
    HRESULT hr;
    BOOL initialized = FALSE;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    initialized = SUCCEEDED(hr);

    append_test_banner(test->display_name);
    test->func(test_log_callback);
    test_log_callback("");
    test_log_callback("--- Done ---");

    if (initialized) CoUninitialize();
    return 0;
}

static int run_all_named_tests(void)
{
    HRESULT hr;
    BOOL initialized = FALSE;
    size_t i;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    initialized = SUCCEEDED(hr);

    for (i = 0; i < sizeof(g_all_tests) / sizeof(g_all_tests[0]); i++) {
        append_test_banner(g_all_tests[i].display_name);
        g_all_tests[i].func(test_log_callback);
    }

    test_log_callback("");
    test_log_callback("--- All Tests Done ---");
    if (initialized) CoUninitialize();
    return 0;
}

static int run_cli_tests(const char *test_name)
{
    const struct named_test *test;

    dbg_log("WineHua Audio Test child mode: %s", test_name);

    if (_stricmp(test_name, "all") == 0)
        return run_all_named_tests();

    test = find_named_test(test_name);
    if (!test) {
        test_log_callback("[FAIL] Unknown audio test name");
        return 2;
    }
    return run_named_test(test);
}

static void set_test_controls_enabled(HWND hWnd, BOOL enabled)
{
    static const int control_ids[] = {
        IDC_BTN_WASAPI, IDC_BTN_DSOUND, IDC_BTN_WAVEOUT,
        IDC_BTN_MCI, IDC_BTN_MIDI, IDC_BTN_ALL
    };
    size_t i;

    for (i = 0; i < sizeof(control_ids) / sizeof(control_ids[0]); i++) {
        HWND control = GetDlgItem(hWnd, control_ids[i]);
        if (control) EnableWindow(control, enabled);
    }
}

static void flush_monitor_pending_line(struct test_process_param *param)
{
    if (param->pending_len >= sizeof(param->pending))
        param->pending_len = sizeof(param->pending) - 1;

    param->pending[param->pending_len] = '\0';
    post_test_log_line(param->pending);
    param->pending_len = 0;
}

static void consume_monitor_bytes(struct test_process_param *param,
                                  const char *bytes, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        char ch = bytes[i];

        if (ch == '\r') continue;
        if (ch == '\n') {
            flush_monitor_pending_line(param);
            continue;
        }

        if (param->pending_len + 1 >= sizeof(param->pending))
            flush_monitor_pending_line(param);
        param->pending[param->pending_len++] = ch;
    }
}

static void pump_test_log_file(struct test_process_param *param, BOOL flush_partial)
{
    FILE *f;
    char buffer[512];
    size_t read_count;
    long pos;

    f = fopen(param->log_path, "rb");
    if (!f) {
        if (flush_partial && param->pending_len)
            flush_monitor_pending_line(param);
        return;
    }

    if (fseek(f, param->read_offset, SEEK_SET) != 0) {
        fclose(f);
        return;
    }

    while ((read_count = fread(buffer, 1, sizeof(buffer), f)) > 0)
        consume_monitor_bytes(param, buffer, read_count);

    pos = ftell(f);
    if (pos >= 0) param->read_offset = pos;
    fclose(f);

    if (flush_partial && param->pending_len)
        flush_monitor_pending_line(param);
}

static DWORD WINAPI MonitorTestProcessThread(LPVOID raw_param)
{
    struct test_process_param *param = (struct test_process_param *)raw_param;
    DWORD wait_result;
    DWORD exit_code = 0;
    DWORD started = GetTickCount();

    for (;;) {
        pump_test_log_file(param, FALSE);

        wait_result = WaitForSingleObject(param->process, AUDIO_TEST_POLL_MS);
        if (wait_result == WAIT_OBJECT_0) {
            GetExitCodeProcess(param->process, &exit_code);
            pump_test_log_file(param, TRUE);
            if (g_testProcess == param->process) g_testProcess = NULL;
            CloseHandle(param->process);
            PostMessageA(param->owner, WM_TEST_FINISHED, (WPARAM)exit_code, 0);
            free(param);
            return 0;
        }

        if (wait_result == WAIT_FAILED) {
            post_test_log_line("[FAIL] WaitForSingleObject(test process) failed");
            pump_test_log_file(param, TRUE);
            if (g_testProcess == param->process) g_testProcess = NULL;
            CloseHandle(param->process);
            PostMessageA(param->owner, WM_TEST_FINISHED,
                         (WPARAM)GetLastError(), 0);
            free(param);
            return 0;
        }

        if (GetTickCount() - started > param->timeout_ms) {
            char line[256];

            snprintf(line, sizeof(line),
                     "[FAIL] %s timed out after %lu ms; Wine callback path is likely stuck",
                     param->test_name, (unsigned long)param->timeout_ms);
            post_test_log_line(line);
            TerminateProcess(param->process, ERROR_TIMEOUT);
            WaitForSingleObject(param->process, 1000);
            pump_test_log_file(param, TRUE);
            if (g_testProcess == param->process) g_testProcess = NULL;
            CloseHandle(param->process);
            PostMessageA(param->owner, WM_TEST_FINISHED,
                         (WPARAM)ERROR_TIMEOUT, 1);
            free(param);
            return 0;
        }
    }
}

static BOOL create_temp_log_path(char *out, size_t out_size)
{
    char temp_path[MAX_PATH];
    char temp_file[MAX_PATH];

    if (!GetTempPathA(sizeof(temp_path), temp_path)) return FALSE;
    if (!GetTempFileNameA(temp_path, "wha", 0, temp_file)) return FALSE;

    snprintf(out, out_size, "%s", temp_file);
    return TRUE;
}

static BOOL begin_test_run(HWND hWnd)
{
    if (InterlockedCompareExchange(&g_testRunning, 1, 0) != 0) {
        test_log_callback("[INFO] Another audio test is already running");
        return FALSE;
    }

    if (g_hLogEdit) SetWindowTextA(g_hLogEdit, "");
    g_testPass = 0;
    g_testFail = 0;
    set_test_controls_enabled(hWnd, FALSE);
    update_result_label("Results: Running...");
    return TRUE;
}

static void finish_test_run(HWND hWnd, BOOL timed_out)
{
    InterlockedExchange(&g_testRunning, 0);
    set_test_controls_enabled(hWnd, TRUE);
    if (timed_out)
        update_result_label("Results: Timed Out");
    else if (g_testPass == 0 && g_testFail == 0)
        update_result_label("Results: Completed");
    else
        update_result_label(NULL);
}

static BOOL launch_test_process(HWND hWnd, const char *cli_name,
                                const char *display_name, DWORD timeout_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE monitor = NULL;
    struct test_process_param *param = NULL;
    char exe_path[MAX_PATH];
    char log_path[MAX_PATH];
    char command[MAX_PATH * 3];

    if (!GetModuleFileNameA(NULL, exe_path, sizeof(exe_path))) {
        test_log_callback("[FAIL] GetModuleFileName failed for test launcher");
        return FALSE;
    }

    if (!create_temp_log_path(log_path, sizeof(log_path))) {
        test_log_callback("[FAIL] Could not allocate temporary log path");
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    snprintf(command, sizeof(command),
             "\"%s\" --run-test %s --log-file \"%s\"",
             exe_path, cli_name, log_path);

    if (!CreateProcessA(exe_path, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        test_log_callback("[FAIL] CreateProcess for isolated audio test failed");
        return FALSE;
    }

    CloseHandle(pi.hThread);

    param = (struct test_process_param *)calloc(1, sizeof(*param));
    if (!param) {
        TerminateProcess(pi.hProcess, ERROR_OUTOFMEMORY);
        CloseHandle(pi.hProcess);
        test_log_callback("[FAIL] Out of memory while creating test monitor");
        return FALSE;
    }

    param->owner = hWnd;
    param->process = pi.hProcess;
    param->timeout_ms = timeout_ms;
    snprintf(param->log_path, sizeof(param->log_path), "%s", log_path);
    snprintf(param->test_name, sizeof(param->test_name), "%s", display_name);
    g_testProcess = pi.hProcess;

    monitor = CreateThread(NULL, 0, MonitorTestProcessThread, param, 0, NULL);
    if (!monitor) {
        TerminateProcess(pi.hProcess, ERROR_OUTOFMEMORY);
        CloseHandle(pi.hProcess);
        g_testProcess = NULL;
        free(param);
        test_log_callback("[FAIL] CreateThread for test monitor failed");
        return FALSE;
    }
    CloseHandle(monitor);

    {
        char line[128];
        snprintf(line, sizeof(line),
                 "[INFO] Running %s in isolated test process", display_name);
        post_test_log_line(line);
    }
    return TRUE;
}

static void RunTest(HWND hWnd, const struct named_test *test)
{
    if (!begin_test_run(hWnd)) return;
    if (!launch_test_process(hWnd, test->cli_name, test->display_name,
                             test->timeout_ms))
        finish_test_run(hWnd, FALSE);
}

static void RunAllTests(HWND hWnd)
{
    if (!begin_test_run(hWnd)) return;
    if (!launch_test_process(hWnd, "all", "Run All",
                             AUDIO_TEST_TIMEOUT_ALL_MS))
        finish_test_run(hWnd, FALSE);
}

static BOOL copy_wide_arg(const WCHAR *src, char *dst, size_t dst_size)
{
    int written;

    if (!dst || !dst_size) return FALSE;
    written = WideCharToMultiByte(CP_ACP, 0, src, -1, dst,
                                  (int)dst_size, NULL, NULL);
    if (written <= 0) {
        dst[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static BOOL parse_cli_option(int argc, WCHAR **argv, const WCHAR *option,
                             char *out, size_t out_size)
{
    size_t option_len = wcslen(option);
    int i;

    if (out && out_size) out[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], option) == 0) {
            if (i + 1 >= argc) return FALSE;
            return copy_wide_arg(argv[i + 1], out, out_size);
        }

        if (_wcsnicmp(argv[i], option, option_len) == 0 &&
            argv[i][option_len] == L'=') {
            return copy_wide_arg(argv[i] + option_len + 1, out, out_size);
        }
    }

    return FALSE;
}

/* -- Window proc -- */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE:
    {
        RECT rc;
        GetClientRect(hWnd, &rc);
        int W = rc.right - rc.left;

        /* -- Test panel (top section) -------------------------- */

        /* Test result log (multi-line read-only edit) */
        g_hLogEdit = CreateWindowA("EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | WS_VSCROLL,
            10, 10, W - 20, 260, hWnd,
            (HMENU)IDC_LOG_EDIT, GetModuleHandle(NULL), NULL);
        SendMessageA(g_hLogEdit, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
        SendMessageA(g_hLogEdit, EM_SETLIMITTEXT, 65536, 0);

        /* Test buttons - row 1 */
        int bx = 10, by = 275, bw = 100, bh = 28, gap = 5;
        CreateWindowA("BUTTON", "WASAPI Test",
            WS_CHILD | WS_VISIBLE,
            bx, by, bw, bh, hWnd,
            (HMENU)IDC_BTN_WASAPI, GetModuleHandle(NULL), NULL);
        bx += bw + gap;
        CreateWindowA("BUTTON", "DirectSound",
            WS_CHILD | WS_VISIBLE,
            bx, by, bw, bh, hWnd,
            (HMENU)IDC_BTN_DSOUND, GetModuleHandle(NULL), NULL);
        bx += bw + gap;
        CreateWindowA("BUTTON", "waveOut",
            WS_CHILD | WS_VISIBLE,
            bx, by, bw, bh, hWnd,
            (HMENU)IDC_BTN_WAVEOUT, GetModuleHandle(NULL), NULL);
        bx += bw + gap;
        CreateWindowA("BUTTON", "MCI",
            WS_CHILD | WS_VISIBLE,
            bx, by, bw, bh, hWnd,
            (HMENU)IDC_BTN_MCI, GetModuleHandle(NULL), NULL);
        bx += bw + gap;
        CreateWindowA("BUTTON", "MIDI",
            WS_CHILD | WS_VISIBLE,
            bx, by, bw, bh, hWnd,
            (HMENU)IDC_BTN_MIDI, GetModuleHandle(NULL), NULL);

        /* Row 2: "Run All" */
        int bx2 = 10;
        CreateWindowA("BUTTON", "Run All",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            bx2, by + bh + gap, 200, bh, hWnd,
            (HMENU)IDC_BTN_ALL, GetModuleHandle(NULL), NULL);

        /* Result summary label */
        g_hResultLabel = CreateWindowA("STATIC",
            "Results: Ready",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            bx2 + 205, by + bh + gap + 4, 300, 20, hWnd,
            (HMENU)IDC_RESULT_LABEL, GetModuleHandle(NULL), NULL);

        /* -- Music player panel (bottom section) --------------- */
        int py = by + bh + gap + bh + gap + 10;

        /* ListView */
        g_hList = CreateWindowA(WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            10, py, W - 20, 250, hWnd,
            (HMENU)IDC_FILE_LIST, GetModuleHandle(NULL), NULL);
        LVCOLUMNA lvc = {0};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH;
        lvc.pszText = "File Name";
        lvc.cx = 480;
        ListView_InsertColumn(g_hList, 0, &lvc);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        /* buttons */
        int pby = py + 255;
        g_hBtnPlay = CreateWindowA("BUTTON", "Play",
            WS_CHILD | WS_VISIBLE,
            10, pby, 100, 30, hWnd,
            (HMENU)IDC_BTN_PLAY, GetModuleHandle(NULL), NULL);
        g_hBtnStop = CreateWindowA("BUTTON", "Stop",
            WS_CHILD | WS_VISIBLE,
            120, pby, 100, 30, hWnd,
            (HMENU)IDC_BTN_STOP, GetModuleHandle(NULL), NULL);

        /* status bar */
        g_hStatus = CreateWindowA("STATIC", "Ready - double-click to play",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            230, pby + 5, 300, 20, hWnd,
            (HMENU)IDC_STATUS, GetModuleHandle(NULL), NULL);

        /* music/ dir next to EXE */
        GetModuleFileNameA(NULL, g_musicDir, sizeof(g_musicDir));
        char *p = strrchr(g_musicDir, '\\');
        if (p) *p = '\0';
        strncat(g_musicDir, "\\music",
                sizeof(g_musicDir) - strlen(g_musicDir) - 1);

        ScanMusicDir();
        break;
    }

    case WM_SIZE:
    {
        int w = LOWORD(lParam), h = HIWORD(lParam);

        /* Test panel */
        if (g_hLogEdit) MoveWindow(g_hLogEdit, 10, 10, w - 20, 260, TRUE);
        /* Buttons are fixed at y=275, no need to move on every resize */

        /* Music player panel - bottom half */
        int py = 337;
        int listH = h - py - 65;
        if (listH < 50) listH = 50;
        if (g_hList) MoveWindow(g_hList, 10, py, w - 20, listH, TRUE);
        int pby = py + listH + 5;
        if (g_hBtnPlay) MoveWindow(g_hBtnPlay, 10, pby, 100, 30, TRUE);
        if (g_hBtnStop) MoveWindow(g_hBtnStop, 120, pby, 100, 30, TRUE);
        if (g_hStatus) MoveWindow(g_hStatus, 230, pby + 5, w - 260, 20, TRUE);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_PLAY) DoPlay();
        else if (LOWORD(wParam) == IDC_BTN_STOP) DoStop();
        else if (LOWORD(wParam) == IDC_BTN_WASAPI) RunTest(hWnd, &g_all_tests[0]);
        else if (LOWORD(wParam) == IDC_BTN_DSOUND) RunTest(hWnd, &g_all_tests[1]);
        else if (LOWORD(wParam) == IDC_BTN_WAVEOUT) RunTest(hWnd, &g_all_tests[2]);
        else if (LOWORD(wParam) == IDC_BTN_MCI)    RunTest(hWnd, &g_all_tests[3]);
        else if (LOWORD(wParam) == IDC_BTN_MIDI)   RunTest(hWnd, &g_all_tests[4]);
        else if (LOWORD(wParam) == IDC_BTN_ALL)    RunAllTests(hWnd);
        break;

    /* -- WM_TEST_LOG: append text from test thread to log Edit -- */
    case WM_TEST_LOG:
    {
        char *msg = (char *)lParam;
        if (msg && g_hLogEdit) {
            /* Count PASS/FAIL for summary */
            if (strncmp(msg, "[PASS]", 6) == 0)
                InterlockedIncrement(&g_testPass);
            else if (strncmp(msg, "[FAIL]", 6) == 0)
                InterlockedIncrement(&g_testFail);

            /* Append to Edit */
            int len = GetWindowTextLengthA(g_hLogEdit);
            SendMessageA(g_hLogEdit, EM_SETSEL, (WPARAM)len, (WPARAM)len);
            SendMessageA(g_hLogEdit, EM_REPLACESEL, 0, (LPARAM)msg);
            SendMessageA(g_hLogEdit, EM_REPLACESEL, 0, (LPARAM)"\r\n");
            /* Scroll to bottom */
            SendMessageA(g_hLogEdit, EM_SCROLLCARET, 0, 0);

            /* Update result summary label */
            update_result_label(NULL);
        }
        free(msg);
        break;
    }

    case WM_TEST_FINISHED:
        finish_test_run(hWnd, !!lParam);
        break;

    case WM_NOTIFY:
        if (((NMHDR*)lParam)->idFrom == IDC_FILE_LIST &&
            ((NMHDR*)lParam)->code == NM_DBLCLK) DoPlay();
        break;

    case WM_CLOSE:
        if (g_testProcess)
            TerminateProcess(g_testProcess, 1);
        DoStop();
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        g_hLogEdit = NULL;
        g_hResultLabel = NULL;
        g_testProcess = NULL;
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

/* -- WinMain -- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                    LPSTR cmdLine, int nShow)
{
    int argc = 0;
    LPWSTR *argv = NULL;
    char cli_test_name[32] = {0};
    char cli_log_path[MAX_PATH] = {0};
    BOOL cli_mode = FALSE;

    (void)hPrev; (void)cmdLine;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        cli_mode = parse_cli_option(argc, argv, L"--run-test",
                                    cli_test_name, sizeof(cli_test_name));
        parse_cli_option(argc, argv, L"--log-file",
                         cli_log_path, sizeof(cli_log_path));
        LocalFree(argv);
    }

    if (cli_log_path[0]) {
        snprintf(g_logPath, sizeof(g_logPath), "%s", cli_log_path);
    }

    dbg_init();
    if (cli_mode)
        return run_cli_tests(cli_test_name);

    INITCOMMONCONTROLSEX icex = {
        sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icex);

    InitializeCriticalSection(&g_player.lock);
    g_player.state = STATE_IDLE;
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
                              "WineHua Audio Test -- Verification + Player",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              700, 760, NULL, NULL, hInst, NULL);

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
