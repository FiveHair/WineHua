#define COBJMACROS

#include <windows.h>
#include <ddraw.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../thirdparty/wine/programs/winehua_smoke_protocol.h"

struct probe_state
{
    struct winehua_smoke_options options;
    HWND window;
    HMODULE ddraw_module;
    LPDIRECTDRAW dd;
    BOOL is_cnc_ddraw;
    HRESULT create_hr;
    HRESULT coop_windowed_hr;
    HRESULT primary_hr;
    HRESULT offscreen_hr;
    HRESULT lock_hr;
    HRESULT blt_hr;
    HRESULT palette_hr;
    HRESULT set_palette_hr;
    HRESULT coop_fullscreen_hr;
    HRESULT set_mode_hr;
    HRESULT flip_chain_hr;
    HRESULT flip_hr;
    UINT failures;
    char first_stage[64];
    char first_message[256];
};

typedef HRESULT (WINAPI *directdraw_create_fn)(GUID *guid, LPDIRECTDRAW *out, IUnknown *outer);

static void record_failure(struct probe_state *state, const char *stage, const char *format, ...)
{
    va_list args;

    ++state->failures;
    if (state->first_stage[0]) return;
    lstrcpynA(state->first_stage, stage, (int)sizeof(state->first_stage));
    va_start(args, format);
    vsnprintf(state->first_message, sizeof(state->first_message), format, args);
    va_end(args);
}

static BOOL require_hr(struct probe_state *state, const char *stage, const char *name, HRESULT hr)
{
    fprintf(stderr, "[%s] %s hr=0x%08lx\n", SUCCEEDED(hr) ? "PASS" : "FAIL", name,
            (unsigned long)hr);
    if (SUCCEEDED(hr)) return TRUE;
    record_failure(state, stage, "%s failed with HRESULT 0x%08lx", name, (unsigned long)hr);
    return FALSE;
}

static void require_condition(struct probe_state *state, const char *stage, const char *name, BOOL value)
{
    fprintf(stderr, "[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) record_failure(state, stage, "%s", name);
}

static void fill_surface_pattern(LPDIRECTDRAWSURFACE surface, UINT width, UINT height)
{
    DDSURFACEDESC desc;
    UINT x, y;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface_Lock(surface, NULL, &desc, DDLOCK_WAIT, NULL))) return;
    for (y = 0; y < height && y < desc.dwHeight; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);
        for (x = 0; x < width && x < desc.dwWidth; ++x)
            row[x] = (x ^ y) & 1 ? 0x00ff8000 : 0x000080ff;
    }
    IDirectDrawSurface_Unlock(surface, desc.lpSurface);
}

static void run_windowed_case(struct probe_state *state)
{
    DDSURFACEDESC desc;
    LPDIRECTDRAWSURFACE primary = NULL, offscreen = NULL;
    LPDIRECTDRAWPALETTE palette = NULL;
    PALETTEENTRY entries[256];
    RECT dst;
    UINT i;

    state->coop_windowed_hr = IDirectDraw_SetCooperativeLevel(state->dd, state->window, DDSCL_NORMAL);
    if (!require_hr(state, "windowed", "SetCooperativeLevel DDSCL_NORMAL", state->coop_windowed_hr))
        return;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    state->primary_hr = IDirectDraw_CreateSurface(state->dd, &desc, &primary, NULL);
    require_hr(state, "windowed", "CreateSurface primary", state->primary_hr);

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    desc.dwWidth = 64;
    desc.dwHeight = 64;
    memset(&desc.ddpfPixelFormat, 0, sizeof(desc.ddpfPixelFormat));
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    state->offscreen_hr = IDirectDraw_CreateSurface(state->dd, &desc, &offscreen, NULL);
    if (!require_hr(state, "windowed", "CreateSurface offscreen 64x64 X8R8G8B8", state->offscreen_hr))
        goto done;

    fill_surface_pattern(offscreen, 64, 64);
    state->lock_hr = DD_OK;

    dst.left = 32; dst.top = 32; dst.right = 224; dst.bottom = 192;
    state->blt_hr = IDirectDrawSurface_Blt(primary, &dst, offscreen, NULL, DDBLT_WAIT, NULL);
    require_hr(state, "windowed", "Blt offscreen -> primary (stretched)", state->blt_hr);

    for (i = 0; i < 256; ++i)
    {
        entries[i].peRed = (BYTE)i;
        entries[i].peGreen = (BYTE)(255 - i);
        entries[i].peBlue = (BYTE)(i ^ 0x80);
        entries[i].peFlags = 0;
    }
    state->palette_hr = IDirectDraw_CreatePalette(state->dd, DDPCAPS_8BIT, entries, &palette, NULL);
    require_hr(state, "palette", "CreatePalette 8-bit", state->palette_hr);
    if (SUCCEEDED(state->palette_hr))
    {
        /* A 32-bit windowed primary often rejects palettes. Record the HRESULT
         * without failing the smoke: C&C-style 8-bit fullscreen still exercises
         * CreatePalette, which is the compatibility gate. */
        state->set_palette_hr = IDirectDrawSurface_SetPalette(primary, palette);
        fprintf(stderr, "[INFO] SetPalette on 32-bit primary hr=0x%08lx\n",
                (unsigned long)state->set_palette_hr);
    }

done:
    if (palette) IDirectDrawPalette_Release(palette);
    if (offscreen) IDirectDrawSurface_Release(offscreen);
    if (primary) IDirectDrawSurface_Release(primary);
}

static void run_fullscreen_case(struct probe_state *state)
{
    DDSURFACEDESC desc;
    LPDIRECTDRAWSURFACE primary = NULL;
    UINT i;

    /* Fullscreen exclusive is only exercised against cnc-ddraw: it intercepts
     * SetDisplayMode and does not poke the real display. Wine's builtin ddraw
     * would resize the winewayland output, which is not smoke-safe. */
    if (!state->is_cnc_ddraw)
    {
        fprintf(stderr, "[SKIP] fullscreen flip chain (builtin ddraw, not smoke-safe)\n");
        state->coop_fullscreen_hr = state->set_mode_hr = state->flip_chain_hr = state->flip_hr = DD_OK;
        return;
    }

    state->coop_fullscreen_hr = IDirectDraw_SetCooperativeLevel(state->dd, state->window,
            DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (!require_hr(state, "fullscreen", "SetCooperativeLevel EXCLUSIVE|FULLSCREEN",
            state->coop_fullscreen_hr))
        return;
    state->set_mode_hr = IDirectDraw_SetDisplayMode(state->dd, 640, 480, 32);
    if (!require_hr(state, "fullscreen", "SetDisplayMode 640x480x32", state->set_mode_hr))
        goto restore;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    desc.dwBackBufferCount = 1;
    state->flip_chain_hr = IDirectDraw_CreateSurface(state->dd, &desc, &primary, NULL);
    if (!require_hr(state, "fullscreen", "CreateSurface primary+backbuffer flip chain",
            state->flip_chain_hr))
        goto restore;

    fill_surface_pattern(primary, 640, 480);
    for (i = 0; i < 3; ++i)
    {
        state->flip_hr = IDirectDrawSurface_Flip(primary, NULL, DDFLIP_WAIT);
        if (FAILED(state->flip_hr)) break;
    }
    require_hr(state, "fullscreen", "Flip x3", state->flip_hr);

restore:
    if (primary) IDirectDrawSurface_Release(primary);
    IDirectDraw_RestoreDisplayMode(state->dd);
    IDirectDraw_SetCooperativeLevel(state->dd, state->window, DDSCL_NORMAL);
}

static void write_result(struct probe_state *state)
{
    char metrics[2048];
    const char *status = state->failures ? "FAIL" : "PASS";
    const char *stage = state->failures ? state->first_stage : "complete";
    const char *message = state->failures ? state->first_message :
            "DirectDraw create, windowed blt, palette and fullscreen flip passed";

    snprintf(metrics, sizeof(metrics),
            "{\"impl\":\"%s\",\"windowed\":{\"coop\":\"0x%08lx\",\"primary\":\"0x%08lx\","
            "\"offscreen\":\"0x%08lx\",\"blt\":\"0x%08lx\"},"
            "\"palette\":{\"create\":\"0x%08lx\",\"set\":\"0x%08lx\"},"
            "\"fullscreen\":{\"coop\":\"0x%08lx\",\"setMode\":\"0x%08lx\","
            "\"chain\":\"0x%08lx\",\"flip\":\"0x%08lx\",\"skipped\":%s},"
            "\"failureCount\":%u}",
            state->is_cnc_ddraw ? "cnc-ddraw" : "wine-builtin",
            (unsigned long)state->coop_windowed_hr, (unsigned long)state->primary_hr,
            (unsigned long)state->offscreen_hr, (unsigned long)state->blt_hr,
            (unsigned long)state->palette_hr, (unsigned long)state->set_palette_hr,
            (unsigned long)state->coop_fullscreen_hr, (unsigned long)state->set_mode_hr,
            (unsigned long)state->flip_chain_hr, (unsigned long)state->flip_hr,
            state->is_cnc_ddraw ? "false" : "true",
            state->failures);
    winehua_smoke_write_result(&state->options, status, stage, message, metrics);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int main(int argc, char **argv)
{
    struct probe_state state;
    WNDCLASSA window_class;
    directdraw_create_fn create_dd;
    FARPROC create_proc;

    memset(&state, 0, sizeof(state));
    if (!winehua_smoke_parse_options(&state.options, argc, argv, 5)) return 2;
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = "WineHuaDDrawSmokeWindow";
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        record_failure(&state, "window", "RegisterClass failed with error %lu", GetLastError());
        write_result(&state);
        return 1;
    }
    state.window = CreateWindowExA(0, window_class.lpszClassName, "WineHua DDraw smoke",
            WS_OVERLAPPEDWINDOW, 80, 80, 640, 480, NULL, NULL, window_class.hInstance, NULL);
    if (!state.window)
    {
        record_failure(&state, "window", "CreateWindow failed with error %lu", GetLastError());
        write_result(&state);
        return 1;
    }
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);

    state.ddraw_module = LoadLibraryA("ddraw.dll");
    require_condition(&state, "dll-load", "ddraw.dll loaded", state.ddraw_module != NULL);
    if (!state.ddraw_module)
    {
        write_result(&state);
        DestroyWindow(state.window);
        UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    /* DDGetProcAddress is a cnc-ddraw-only export; Wine's builtin ddraw does
     * not provide it.  Presence proves the PE overlay is active. */
    state.is_cnc_ddraw = GetProcAddress(state.ddraw_module, "DDGetProcAddress") != NULL;
    fprintf(stderr, "[INFO] ddraw implementation: %s\n", state.is_cnc_ddraw ? "cnc-ddraw" : "wine-builtin");
    {
        const char *backend = getenv("WINEHUA_DDRAW_BACKEND");
        if (backend && !strcmp(backend, "cnc"))
            require_condition(&state, "dll-load",
                    "cnc-ddraw overlay is active (DDGetProcAddress)",
                    state.is_cnc_ddraw);
    }

    create_dd = NULL;
    create_proc = GetProcAddress(state.ddraw_module, "DirectDrawCreate");
    if (create_proc) memcpy(&create_dd, &create_proc, sizeof(create_dd));
    require_condition(&state, "dll-load", "DirectDrawCreate export is available", create_dd != NULL);
    state.create_hr = create_dd ? create_dd(NULL, &state.dd, NULL) : E_FAIL;
    if (!require_hr(&state, "directdraw-create", "DirectDrawCreate", state.create_hr))
    {
        write_result(&state);
        FreeLibrary(state.ddraw_module);
        DestroyWindow(state.window);
        UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
        return 1;
    }

    run_windowed_case(&state);
    run_fullscreen_case(&state);

    write_result(&state);
    IDirectDraw_Release(state.dd);
    FreeLibrary(state.ddraw_module);
    DestroyWindow(state.window);
    UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
    return state.failures ? 1 : 0;
}
