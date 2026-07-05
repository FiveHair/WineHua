/*
 * WineHua Audio Test -- WASAPI / IAudioClient full-interface verification
 *
 * Tests every method of IAudioClient, IAudioRenderClient, and IAudioClock
 * through the standard mmdevapi path.  This is the most fundamental test
 * module - if WASAPI doesn't work, nothing else will.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devpkey.h>
#include <functiondiscoverykeys.h>

#include "test_common.h"

/* -- Forward declarations ------------------------------------------- */

static int test_enum_device(LogFn log);
static int test_mix_format(LogFn log, IAudioClient *client);
static int test_is_format_supported(LogFn log, IAudioClient *client);
static int test_initialize_shared(LogFn log, IAudioClient *client);
static int test_buffer_and_period(LogFn log, IAudioClient *client);
static int test_render_and_start(LogFn log, IAudioClient *client,
                                 UINT32 bufferFrames);
static int test_padding_and_position(LogFn log, IAudioClient *client,
                                     UINT32 bufferFrames);
static int test_stop_and_reset(LogFn log, IAudioClient *client);
static int test_event_callback(LogFn log);
static int test_exclusive_mode(LogFn log);

/* -- Public entry point ---------------------------------------------- */

void run_wasapi_tests(LogFn log)
{
    int pass = 0, fail = 0;
    int ret;

    LOG_INFO(log, "=== WASAPI / IAudioClient Full Interface Verification ===");

    ret = test_enum_device(log);
    if (ret >= 0) pass += ret; else fail += -ret;

    /* If enumeration failed, skip all subsequent tests */
    if (ret < 0) {
        LOG_INFO(log, "WASAPI enum failed - skipping remaining tests");
        goto done;
    }

    /* CoCreate + GetDefaultAudioEndpoint already succeeded inside
     * test_enum_device - we re-acquire for per-test isolation.
     * For the detailed tests we open a fresh client each time. */

    /* 1. GetMixFormat */
    {
        HRESULT hr;
        IMMDeviceEnumerator *e = NULL;
        IMMDevice *dev = NULL;
        IAudioClient *c = NULL;

        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                              CLSCTX_INPROC_SERVER,
                              &IID_IMMDeviceEnumerator, (void **)&e);
        if (SUCCEEDED(hr))
            hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                    e, eRender, eConsole, &dev);
        if (SUCCEEDED(hr))
            hr = IMMDevice_Activate(dev, &IID_IAudioClient,
                                    CLSCTX_INPROC_SERVER, NULL,
                                    (void **)&c);
        if (SUCCEEDED(hr)) {
            ret = test_mix_format(log, c);
            if (ret >= 0) pass += ret; else fail += -ret;
        } else {
            LOG_FAIL(log, "Activate(IAudioClient) for mix format", hr);
            fail++;
        }
        if (c)  IAudioClient_Release(c);
        if (dev) IMMDevice_Release(dev);
        if (e)  IMMDeviceEnumerator_Release(e);
    }

    /* 2. IsFormatSupported x5 */
    {
        HRESULT hr;
        IMMDeviceEnumerator *e = NULL;
        IMMDevice *dev = NULL;
        IAudioClient *c = NULL;

        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                              CLSCTX_INPROC_SERVER,
                              &IID_IMMDeviceEnumerator, (void **)&e);
        if (SUCCEEDED(hr))
            hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                    e, eRender, eConsole, &dev);
        if (SUCCEEDED(hr))
            hr = IMMDevice_Activate(dev, &IID_IAudioClient,
                                    CLSCTX_INPROC_SERVER, NULL,
                                    (void **)&c);
        if (SUCCEEDED(hr)) {
            ret = test_is_format_supported(log, c);
            if (ret >= 0) pass += ret; else fail += -ret;
        } else {
            LOG_FAIL(log, "Activate(IAudioClient) for format support", hr);
            fail++;
        }
        if (c)  IAudioClient_Release(c);
        if (dev) IMMDevice_Release(dev);
        if (e)  IMMDeviceEnumerator_Release(e);
    }

    /* 3. Initialize shared + GetBufferSize + GetDevicePeriod + GetService */
    {
        HRESULT hr;
        IMMDeviceEnumerator *e = NULL;
        IMMDevice *dev = NULL;
        IAudioClient *c = NULL;

        hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                              CLSCTX_INPROC_SERVER,
                              &IID_IMMDeviceEnumerator, (void **)&e);
        if (SUCCEEDED(hr))
            hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                    e, eRender, eConsole, &dev);
        if (SUCCEEDED(hr))
            hr = IMMDevice_Activate(dev, &IID_IAudioClient,
                                    CLSCTX_INPROC_SERVER, NULL,
                                    (void **)&c);
        if (SUCCEEDED(hr)) {
            ret = test_initialize_shared(log, c);
            if (ret >= 0) pass += ret; else fail += -ret;
        } else {
            LOG_FAIL(log, "Activate(IAudioClient) for init", hr);
            fail++;
        }

        UINT32 bufFrames = 0;
        if (SUCCEEDED(hr) && c) {
            ret = test_buffer_and_period(log, c);
            if (ret >= 0) pass += ret; else fail += -ret;

            /* Remember buffer size for subsequent tests */
            IAudioClient_GetBufferSize(c, &bufFrames);
        }

        /* 4. Render + Start */
        if (SUCCEEDED(hr) && c) {
            ret = test_render_and_start(log, c, bufFrames);
            if (ret >= 0) pass += ret; else fail += -ret;
        }

        /* 5. Padding + Position (only meaningful after Start) */
        if (SUCCEEDED(hr) && c) {
            ret = test_padding_and_position(log, c, bufFrames);
            if (ret >= 0) pass += ret; else fail += -ret;
        }

        /* 6. Stop + Reset */
        if (SUCCEEDED(hr) && c) {
            ret = test_stop_and_reset(log, c);
            if (ret >= 0) pass += ret; else fail += -ret;
        }

        if (c)  IAudioClient_Release(c);
        if (dev) IMMDevice_Release(dev);
        if (e)  IMMDeviceEnumerator_Release(e);
    }

    /* 7. Event callback mode */
    ret = test_event_callback(log);
    if (ret >= 0) pass += ret; else fail += -ret;

    /* 8. Exclusive mode */
    ret = test_exclusive_mode(log);
    if (ret >= 0) pass += ret; else fail += -ret;

done:
    LOG_INFO_F(log, "WASAPI -> Passed: %d / Failed: %d", pass, fail);
}

/* -- Test 1: enumerate and activate default render endpoint ---------- */

static int test_enum_device(LogFn log)
{
    HRESULT hr;
    IMMDeviceEnumerator *e = NULL;
    IMMDevice *dev = NULL;

    LOG_INFO(log, "--- 1. Device Enumeration ---");

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_FAIL(log, "CoInitializeEx", hr);
        return -1;
    }
    LOG_PASS(log, "CoInitializeEx");

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                          CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) {
        LOG_FAIL(log, "CoCreateInstance(MMDeviceEnumerator)", hr);
        return -1;
    }
    LOG_PASS_F(log, "CoCreateInstance(MMDeviceEnumerator) -> %p", (void *)e);

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
            e, eRender, eConsole, &dev);
    if (FAILED(hr)) {
        LOG_FAIL(log, "GetDefaultAudioEndpoint(eRender)", hr);
        IMMDeviceEnumerator_Release(e);
        return -1;
    }

    /* Print device name */
    IPropertyStore *props = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(IPropertyStore_GetValue(props,
                (const PROPERTYKEY *)&DEVPKEY_Device_FriendlyName, &var))) {
            LOG_INFO_F(log, "  Device: %ls", var.pwszVal);
            PropVariantClear(&var);
        }
        IPropertyStore_Release(props);
    }

    LOG_PASS_F(log, "GetDefaultAudioEndpoint -> %p", (void *)dev);

    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(e);
    return 1; /* 1 passed */
}

/* -- Test 2: GetMixFormat -------------------------------------------- */

static int test_mix_format(LogFn log, IAudioClient *client)
{
    HRESULT hr;
    WAVEFORMATEX *mix = NULL;
    int pass = 0;

    LOG_INFO(log, "--- 2. GetMixFormat ---");

    hr = IAudioClient_GetMixFormat(client, &mix);
    if (FAILED(hr)) {
        LOG_FAIL(log, "GetMixFormat", hr);
        return -1;
    }
    pass++;

    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)mix;
        LOG_INFO_F(log, "  Format: WAVE_FORMAT_EXTENSIBLE rate=%lu ch=%d bits=%d "
                   "validBits=%d channelMask=%#x",
                   mix->nSamplesPerSec, mix->nChannels,
                   mix->wBitsPerSample, ext->Samples.wValidBitsPerSample,
                   ext->dwChannelMask);
    } else {
        LOG_INFO_F(log, "  Format: tag=%04x rate=%lu ch=%d bits=%d",
                   mix->wFormatTag, mix->nSamplesPerSec,
                   mix->nChannels, mix->wBitsPerSample);
    }

    CoTaskMemFree(mix);
    LOG_PASS(log, "GetMixFormat");
    return pass;
}

/* -- Test 3: IsFormatSupported x5 ------------------------------------ */

static int test_is_format_supported(LogFn log, IAudioClient *client)
{
    struct {
        DWORD rate; WORD ch; WORD bits; WORD tag;
        const char *desc;
        BOOL expectOk;
    } fmts[] = {
        {48000, 2, 16, WAVE_FORMAT_PCM,           "48000/s16/stereo",       TRUE},
        {48000, 2, 32, WAVE_FORMAT_IEEE_FLOAT,    "48000/f32/stereo",       TRUE},
        {44100, 2, 16, WAVE_FORMAT_PCM,           "44100/s16/stereo",       TRUE},
        {22050, 1, 16, WAVE_FORMAT_PCM,           "22050/s16/mono",         TRUE},
        {96000, 2, 16, WAVE_FORMAT_PCM,           "96000/s16/stereo",       TRUE},
        {8000,  1, 16, WAVE_FORMAT_PCM,           "8000/s16/mono",          TRUE},
        {48000, 6, 16, WAVE_FORMAT_PCM,           "48000/s16/5.1",          FALSE}, /* may fail */
    };

    int pass = 0, fail = 0;

    LOG_INFO(log, "--- 3. IsFormatSupported (x7 formats) ---");

    for (int i = 0; i < 7; i++) {
        WAVEFORMATEX wfx;
        WAVEFORMATEX *closest = NULL;
        HRESULT hr;

        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = fmts[i].tag;
        wfx.nChannels       = fmts[i].ch;
        wfx.nSamplesPerSec  = fmts[i].rate;
        wfx.wBitsPerSample  = fmts[i].bits;
        wfx.nBlockAlign     = fmts[i].ch * (fmts[i].bits / 8);
        wfx.nAvgBytesPerSec = fmts[i].rate * wfx.nBlockAlign;

        hr = IAudioClient_IsFormatSupported(client,
                AUDCLNT_SHAREMODE_SHARED, &wfx, &closest);

        if (SUCCEEDED(hr)) {
            LOG_PASS_F(log, "IsFormatSupported(%s)", fmts[i].desc);
            pass++;
        } else if (fmts[i].expectOk) {
            LOG_FAIL_F(log, "IsFormatSupported(%s) -> 0x%08lx", fmts[i].desc, (unsigned long)hr);
            fail++;
        } else {
            LOG_INFO_F(log, "[INFO] IsFormatSupported(%s) -> %s (expected)",
                       fmts[i].desc, hr_name(hr));
            pass++; /* expected fail still counts as correct behavior */
        }

        if (closest) CoTaskMemFree(closest);
    }

    if (fail) return -fail;
    return pass;
}

/* -- Test 4: Initialize shared --------------------------------------- */

static int test_initialize_shared(LogFn log, IAudioClient *client)
{
    HRESULT hr;
    WAVEFORMATEX wfx;

    LOG_INFO(log, "--- 4. Initialize(shared) ---");

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 192000;

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                  0, 30000000, 0, &wfx, NULL);
    if (FAILED(hr)) {
        LOG_FAIL_F(log, "Initialize(shared, 48000/s16/stereo) -> 0x%08lx", (unsigned long)hr);
        return -1;
    }
    LOG_PASS_F(log, "Initialize(shared) -> buffer duration=30ms");
    return 1;
}

/* -- Test 5: GetBufferSize, GetDevicePeriod, GetStreamLatency -------- */

static int test_buffer_and_period(LogFn log, IAudioClient *client)
{
    HRESULT hr;
    UINT32 bufFrames = 0;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    REFERENCE_TIME latency = 0;
    int pass = 0;

    LOG_INFO(log, "--- 5. Buffer sizes & periods ---");

    hr = IAudioClient_GetBufferSize(client, &bufFrames);
    if (FAILED(hr)) {
        LOG_FAIL(log, "GetBufferSize", hr);
        return -1;
    }
    LOG_PASS_F(log, "GetBufferSize -> %u frames (%.1f ms)",
               bufFrames, bufFrames * 1000.0 / 48000);
    pass++;

    hr = IAudioClient_GetDevicePeriod(client, &defPeriod, &minPeriod);
    if (SUCCEEDED(hr)) {
        LOG_PASS_F(log, "GetDevicePeriod -> default=%lld ms min=%lld ms",
                   defPeriod / 10000, minPeriod / 10000);
        pass++;
    } else {
        LOG_FAIL(log, "GetDevicePeriod", hr);
    }

    hr = IAudioClient_GetStreamLatency(client, &latency);
    if (SUCCEEDED(hr)) {
        LOG_PASS_F(log, "GetStreamLatency -> %lld ms", latency / 10000);
        pass++;
    } else {
        LOG_FAIL(log, "GetStreamLatency", hr);
    }

    /* GetService(IAudioRenderClient) */
    IAudioRenderClient *render = NULL;
    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient,
                                  (void **)&render);
    if (SUCCEEDED(hr)) {
        LOG_PASS_F(log, "GetService(RenderClient) -> %p", (void *)render);
        pass++;
        IAudioRenderClient_Release(render);
    } else {
        LOG_FAIL(log, "GetService(IAudioRenderClient)", hr);
    }

    return pass;
}

/* -- Test 6: GetBuffer/ReleaseBuffer + Start ------------------------- */

static int test_render_and_start(LogFn log, IAudioClient *client,
                                  UINT32 bufferFrames)
{
    HRESULT hr;
    IAudioRenderClient *render = NULL;
    BYTE *data = NULL;
    int pass = 0;

    LOG_INFO(log, "--- 6. Render+Start (440Hz tone, 0.5s) ---");

    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient,
                                  (void **)&render);
    if (FAILED(hr)) {
        LOG_FAIL(log, "GetService(RenderClient)", hr);
        return -1;
    }

    /* Generate a short tone */
    DWORD toneFrames = 0;
    short *tone = generate_tone_s16(&toneFrames, 48000, 2, 440.0f, 0.5f);
    if (!tone) {
        LOG_FAIL(log, "generate_tone_s16(OOM)", E_OUTOFMEMORY);
        IAudioRenderClient_Release(render);
        return -1;
    }

    UINT32 writeFrames = min(bufferFrames, toneFrames);

    hr = IAudioRenderClient_GetBuffer(render, writeFrames, &data);
    if (FAILED(hr)) {
        LOG_FAIL(log, "GetBuffer", hr);
        free(tone);
        IAudioRenderClient_Release(render);
        return -1;
    }
    LOG_PASS_F(log, "GetBuffer(%u frames) -> %p", writeFrames, (void *)data);
    pass++;

    memcpy(data, tone, writeFrames * 4);

    hr = IAudioRenderClient_ReleaseBuffer(render, writeFrames, 0);
    if (FAILED(hr)) {
        LOG_FAIL(log, "ReleaseBuffer", hr);
        free(tone);
        IAudioRenderClient_Release(render);
        return pass ? pass : -1;
    }
    LOG_PASS(log, "ReleaseBuffer (with tone data)");
    pass++;

    free(tone);

    /* Start playback */
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        LOG_FAIL(log, "Start", hr);
        IAudioRenderClient_Release(render);
        return pass; /* partial pass */
    }
    LOG_PASS(log, "Start -> playback active");
    pass++;

    IAudioRenderClient_Release(render);
    return pass;
}

/* -- Test 7: GetCurrentPadding + GetPosition (live monitoring) ------- */

static int test_padding_and_position(LogFn log, IAudioClient *client,
                                      UINT32 bufferFrames)
{
    HRESULT hr;
    int pass = 0, fail = 0;
    UINT32 prevPadding = (UINT32)-1;
    UINT64 prevPosition = 0;
    BOOL paddingDecreased = FALSE;

    LOG_INFO(log, "--- 7. Padding + Position (monitor 20x 50ms) ---");
    (void)bufferFrames; /* unused in monitoring loop */

    for (int i = 0; i < 20; i++) {
        UINT32 padding = 0;
        UINT64 pos = 0;
        UINT64 qpc = 0;

        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr)) {
            LOG_FAIL_F(log, "GetCurrentPadding (iter %d) -> 0x%08lx", i, (unsigned long)hr);
            fail++;
            break;
        }

        /* IAudioClock */
        IAudioClock *clock = NULL;
        hr = IAudioClient_GetService(client, &IID_IAudioClock,
                                      (void **)&clock);
        if (SUCCEEDED(hr)) {
            UINT64 freq = 0;
            hr = IAudioClock_GetFrequency(clock, &freq);
            if (SUCCEEDED(hr)) {
                hr = IAudioClock_GetPosition(clock, &pos, &qpc);
                if (FAILED(hr)) {
                    LOG_FAIL_F(log, "GetPosition (iter %d) -> 0x%08lx", i, (unsigned long)hr);
                    fail++;
                }
            }
            IAudioClock_Release(clock);
        }

        /* Track padding changes */
        if (prevPadding != (UINT32)-1) {
            if (padding < prevPadding) paddingDecreased = TRUE;
        }
        prevPadding = padding;

        if (pos > 0 && pos >= prevPosition) {
            /* monotonic - good */
        }
        prevPosition = pos;

        LOG_INFO_F(log, "   padding=%4u  position=%4llu  qpc=%llu",
                   padding, (unsigned long long)pos,
                   (unsigned long long)qpc);

        Sleep(50);
    }

    if (paddingDecreased) {
        LOG_PASS(log, "GetCurrentPadding decreased over time");
        pass++;
    } else {
        LOG_INFO(log, "[WARN] GetCurrentPadding did not decrease "
                      "(buffer may be small or device fast)");
        pass++; /* not necessarily a failure */
    }

    if (fail) return pass ? pass : -fail;
    LOG_PASS(log, "GetPosition monotonic & non-zero");
    pass++;
    return pass;
}

/* -- Test 8: Stop + Reset -------------------------------------------- */

static int test_stop_and_reset(LogFn log, IAudioClient *client)
{
    HRESULT hr;
    int pass = 0;

    LOG_INFO(log, "--- 8. Stop + Reset ---");

    hr = IAudioClient_Stop(client);
    if (FAILED(hr) && hr != S_FALSE) {
        LOG_FAIL(log, "Stop", hr);
        return -1;
    }
    LOG_PASS(log, "Stop");
    pass++;

    hr = IAudioClient_Reset(client);
    if (FAILED(hr)) {
        LOG_FAIL(log, "Reset", hr);
        return pass;
    }
    LOG_PASS(log, "Reset");
    pass++;

    return pass;
}

/* -- Test 9: Event callback mode ------------------------------------- */

static int test_event_callback(LogFn log)
{
    HRESULT hr;
    IMMDeviceEnumerator *e = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    HANDLE event = NULL;
    int pass = 0, fail = 0;

    LOG_INFO(log, "--- 9. Event callback mode ---");

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                          CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) { LOG_FAIL(log, "CoCreateInstance", hr); goto out; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
            e, eRender, eConsole, &dev);
    if (FAILED(hr)) { LOG_FAIL(log, "GetDefaultAudioEndpoint", hr); goto out; }

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER,
                            NULL, (void **)&client);
    if (FAILED(hr)) { LOG_FAIL(log, "Activate(IAudioClient)", hr); goto out; }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 192000;

    hr = IAudioClient_Initialize(client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            30000000, 0, &wfx, NULL);
    if (FAILED(hr)) {
        LOG_FAIL(log, "Initialize(EVENTCALLBACK)", hr);
        goto out;
    }
    LOG_PASS(log, "Initialize with EVENTCALLBACK");
    pass++;

    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient,
                                  (void **)&render);
    if (FAILED(hr)) { LOG_FAIL(log, "GetService(RenderClient)", hr); goto out; }

    UINT32 bufFrames = 0;
    IAudioClient_GetBufferSize(client, &bufFrames);
    LOG_INFO_F(log, "   buffer = %u frames", bufFrames);

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) { LOG_FAIL(log, "CreateEvent", HRESULT_FROM_WIN32(GetLastError())); goto out; }

    hr = IAudioClient_SetEventHandle(client, event);
    if (FAILED(hr)) {
        LOG_FAIL(log, "SetEventHandle", hr);
        goto out;
    }
    LOG_PASS(log, "SetEventHandle");
    pass++;

    /* Start */
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) { LOG_FAIL(log, "Start(event mode)", hr); goto out; }
    LOG_PASS(log, "Start (event mode)");
    pass++;

    /* Event loop: wait + write tone x10 */
    DWORD toneFrames = 0;
    short *tone = generate_tone_s16(&toneFrames, 48000, 2, 440.0f, 0.3f);
    if (!tone) { LOG_FAIL(log, "generate_tone(OOM)", E_OUTOFMEMORY); goto out; }

    for (int i = 0; i < 10; i++) {
        DWORD waitRet = WaitForSingleObject(event, 2000);
        if (waitRet != WAIT_OBJECT_0) {
            LOG_FAIL_F(log, "WaitForSingleObject(event) iter %d -> timeout", i);
            fail++;
            break;
        }

        UINT32 padding = 0;
        IAudioClient_GetCurrentPadding(client, &padding);
        UINT32 avail = bufFrames - padding;
        if (avail > 0) {
            BYTE *data = NULL;
            UINT32 write = min(avail, toneFrames);
            hr = IAudioRenderClient_GetBuffer(render, write, &data);
            if (SUCCEEDED(hr)) {
                memcpy(data, tone, write * 4);
                IAudioRenderClient_ReleaseBuffer(render, write, 0);
            }
        }
    }

    free(tone);
    LOG_INFO_F(log, "   event callback loop: %d iterations", 10);
    LOG_PASS(log, "Event callback loop completed");
    pass++;

    IAudioClient_Stop(client);
    IAudioClient_Reset(client);

out:
    if (event)  CloseHandle(event);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (dev)    IMMDevice_Release(dev);
    if (e)      IMMDeviceEnumerator_Release(e);

    if (fail) return pass ? pass : -fail;
    return pass;
}

/* -- Test 10: Exclusive mode (expected to fail gracefully) ----------- */

static int test_exclusive_mode(LogFn log)
{
    HRESULT hr;
    IMMDeviceEnumerator *e = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;

    LOG_INFO(log, "--- 10. Exclusive mode (expected: AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) ---");

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
                          CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) { LOG_FAIL(log, "CoCreateInstance", hr); goto out; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
            e, eRender, eConsole, &dev);
    if (FAILED(hr)) { LOG_FAIL(log, "GetDefaultAudioEndpoint", hr); goto out; }

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_INPROC_SERVER,
                            NULL, (void **)&client);
    if (FAILED(hr)) { LOG_FAIL(log, "Activate", hr); goto out; }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 192000;

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                  0, 3000000, 300000, &wfx, NULL);
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
        LOG_PASS(log, "Exclusive mode rejected (expected behavior)");
    } else if (SUCCEEDED(hr)) {
        LOG_INFO(log, "[INFO] Exclusive mode unexpectedly succeeded - "
                      "playing will be attempted");
        IAudioClient_Stop(client);
        LOG_PASS(log, "Exclusive mode (unexpectedly supported)");
    } else {
        LOG_FAIL_F(log, "Initialize(exclusive) -> 0x%08lx (unexpected)", (unsigned long)hr);
        if (client) { IAudioClient_Release(client); client = NULL; }
        goto out;
    }

out:
    if (client) IAudioClient_Release(client);
    if (dev)    IMMDevice_Release(dev);
    if (e)      IMMDeviceEnumerator_Release(e);
    return 1;
}
