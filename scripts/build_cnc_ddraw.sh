#!/bin/bash
# Build cnc-ddraw (DirectDraw compatibility layer) PE overlay for WineHua.
#
# The upstream submodule (thirdparty/cnc-ddraw) stays pinned and clean; the
# build runs on an isolated copy because cnc-ddraw's Makefile generates
# inc/git.h and object files inside the source tree.
#
# Primary artifact is the 32-bit ddraw.dll. cnc-ddraw is a Win32 wrapper
# (GWL_WNDPROC); an x64 MinGW build fails, and classic DDraw games are x86.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

CNC_SRC="$ROOT/thirdparty/cnc-ddraw"
CNC_BUILD_ROOT="$BUILD_DIR/cnc-ddraw"
SOURCE_COPY="$CNC_BUILD_ROOT/source"
OUTPUT_ROOT="$CNC_BUILD_ROOT/dist"
EXPECTED_BASE_COMMIT="541b5de"
CNC_TAG="v7.1.0.0"

[ -f "$CNC_SRC/Makefile" ] || err "cnc-ddraw submodule missing: $CNC_SRC (run: git submodule update --init thirdparty/cnc-ddraw)"

if git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" rev-parse HEAD >/dev/null 2>&1; then
    base_commit="$(git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" rev-parse --short=7 HEAD)"
    [ -z "$(git -c safe.directory="$CNC_SRC" -C "$CNC_SRC" status --porcelain)" ] || \
        err "cnc-ddraw submodule is dirty; keep modifications out of the submodule"
else
    base_commit="unknown"
fi
case "$base_commit" in
    "$EXPECTED_BASE_COMMIT"*) ;;
    *) err "cnc-ddraw base drifted: expected $EXPECTED_BASE_COMMIT ($CNC_TAG), got $base_commit" ;;
esac

command -v i686-w64-mingw32-gcc >/dev/null 2>&1 || \
    err "i686-w64-mingw32-gcc missing (install gcc-mingw-w64-i686; the Docker buildenv already provides it)"
command -v i686-w64-mingw32-windres >/dev/null 2>&1 || \
    err "i686-w64-mingw32-windres missing (binutils-mingw-w64-i686)"

rm -rf "$SOURCE_COPY"
mkdir -p "$SOURCE_COPY" "$OUTPUT_ROOT/x86"
tar -C "$CNC_SRC" --exclude='.git' -cf - . | tar -C "$SOURCE_COPY" -xf -

python3 - "$SOURCE_COPY" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
ogl = (root / "src/render_ogl.c").read_text()
dd = (root / "src/dd.c").read_text()
if '#include <stdio.h>' not in dd:
    dd = dd.replace('#include <windows.h>\n', '#include <windows.h>\n#include <stdio.h>\n', 1)

dbg = (root / "inc/debug.h").read_text()
if 'winehua_logf' not in dbg:
    needle = '#include <intrin.h>\n'
    if needle not in dbg:
        raise SystemExit("debug.h intrin include site not found")
    dbg = dbg.replace(needle, needle + """#include <stdio.h>
#include <stdarg.h>

/* Wow64 32-bit stderr often never reaches wine_stderr. Duplicate to a
 * DOS path the PE can actually create. */
static inline void winehua_logf(const char *fmt, ...)
{
    va_list ap;
    FILE *fp;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    fp = fopen("C:\\\\windows\\\\temp\\\\cnc-ddraw.log", "a");
    if (fp)
    {
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fclose(fp);
    }
}

""", 1)
    (root / "inc/debug.h").write_text(dbg)

if '#include "versionhelpers.h"' not in ogl:
    needle = '#include "debug.h"\n'
    if needle not in ogl:
        raise SystemExit("debug.h include site not found in render_ogl.c")
    ogl = ogl.replace(needle, needle + '#include "versionhelpers.h"\n', 1)

old = """static BOOL ogl_texture_upload_test()
{
    if (g_ogl.surface_tex_width > 4096 || g_ogl.surface_tex_height > 4096)
        return TRUE;
"""
new = """static BOOL ogl_texture_upload_test()
{
    /* WineHua/virpipe: glGetTexImage does not round-trip a just-uploaded
     * texture. Treating that as a renderer failure poisons the HWND with
     * SetPixelFormat and then draws GDI into a blank GL window. */
    if (IsWine())
    {
        fprintf(stderr, "[cnc-ddraw] skip glGetTexImage upload probe on Wine\\n");
        fflush(stderr);
        return TRUE;
    }
    if (g_ogl.surface_tex_width > 4096 || g_ogl.surface_tex_height > 4096)
        return TRUE;
"""
if old not in ogl:
    raise SystemExit("ogl_texture_upload_test() site not found")
ogl = ogl.replace(old, new, 1)

old = """    ogl_release();

    g_ogl.context = xwglCreateContext(g_ddraw.render.hdc);
"""
new = """    ogl_release();

    if (!g_ddraw.render.hdc)
        g_ddraw.render.hdc = GetDC(g_ddraw.hwnd);
    if (g_ddraw.render.hdc && GetPixelFormat(g_ddraw.render.hdc) == 0)
    {
        PIXELFORMATDESCRIPTOR pfd;
        int pixel_format;
        memset(&pfd, 0, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.iLayerType = PFD_MAIN_PLANE;
        pixel_format = ChoosePixelFormat(g_ddraw.render.hdc, &pfd);
        if (!pixel_format || !SetPixelFormat(g_ddraw.render.hdc, pixel_format, &pfd))
        {
            fprintf(stderr, "[cnc-ddraw] SetPixelFormat failed err=%lu\\n", GetLastError());
            fflush(stderr);
            return FALSE;
        }
    }

    g_ogl.context = xwglCreateContext(g_ddraw.render.hdc);
    if (!g_ogl.context)
    {
        fprintf(stderr, "[cnc-ddraw] wglCreateContext failed err=%lu hwnd=%p hdc=%p size=%ux%u bpp=%u\\n",
                GetLastError(), g_ddraw.hwnd, g_ddraw.render.hdc,
                g_ddraw.width, g_ddraw.height, g_ddraw.bpp);
        fflush(stderr);
    }
"""
if old not in ogl:
    raise SystemExit("ogl_create SetPixelFormat site not found")
ogl = ogl.replace(old, new, 1)

old = """BOOL ogl_create()
{
    if (g_ogl.hwnd == g_ddraw.hwnd && g_ogl.hdc == g_ddraw.render.hdc && g_ogl.context)
    {
        return TRUE;
    }
"""
new = """BOOL ogl_create()
{
    fprintf(stderr, "[cnc-ddraw] ogl_create enter hwnd=%p hdc=%p ctx=%p\\n",
            g_ddraw.hwnd, g_ddraw.render.hdc, g_ogl.context);
    fflush(stderr);
    if (g_ogl.hwnd == g_ddraw.hwnd && g_ogl.hdc == g_ddraw.render.hdc && g_ogl.context)
    {
        return TRUE;
    }
"""
if old not in ogl:
    raise SystemExit("ogl_create enter site not found")
ogl = ogl.replace(old, new, 1)

old = """    else
    {
        TRACE("OpenGL error %08x, GetLastError %lu (xwglMakeCurrent())\\n", err, GetLastError());
        ogl_check_error("xwglMakeCurrent()");
    }

    for (int i = 0; i < 5; i++)
    {
        if (xwglMakeCurrent(NULL, NULL))
            break;

        Sleep(50);
    }
    
    if (!g_ogl.use_opengl)
"""
new = """    else
    {
        fprintf(stderr, "[cnc-ddraw] ogl_render_main MakeCurrent failed err=%08x last=%lu\\n",
                err, GetLastError());
        fflush(stderr);
        TRACE("OpenGL error %08x, GetLastError %lu (xwglMakeCurrent())\\n", err, GetLastError());
        ogl_check_error("xwglMakeCurrent()");
    }

    for (int i = 0; i < 5; i++)
    {
        if (xwglMakeCurrent(NULL, NULL))
            break;

        Sleep(50);
    }
    
    if (!g_ogl.use_opengl)
"""
if old not in ogl:
    raise SystemExit("ogl_render_main MakeCurrent else site not found")
ogl = ogl.replace(old, new, 1)

old = """DWORD WINAPI ogl_render_main(void)
{
    Sleep(250);
    g_ogl.got_error = g_ogl.use_opengl = FALSE;
    GLenum err = GL_NO_ERROR;
    BOOL made_current = FALSE;

    for (int i = 0; i < 5; i++)
    {
        if ((made_current = xwglMakeCurrent(g_ogl.hdc, g_ogl.context)))
            break;

        Sleep(50);
    }
"""
new = """DWORD WINAPI ogl_render_main(void)
{
    if (!IsWine())
        Sleep(250);
    g_ogl.got_error = g_ogl.use_opengl = FALSE;
    GLenum err = GL_NO_ERROR;
    BOOL made_current = FALSE;
    int make_tries = IsWine() ? 20 : 5;

    for (int i = 0; i < make_tries; i++)
    {
        if ((made_current = xwglMakeCurrent(g_ogl.hdc, g_ogl.context)))
            break;

        Sleep(IsWine() ? 100 : 50);
    }
"""
if old not in ogl:
    raise SystemExit("ogl_render_main MakeCurrent retry site not found")
ogl = ogl.replace(old, new, 1)

old = """    if (!g_ogl.use_opengl)
    {
        g_ddraw.show_driver_warning = TRUE;
        g_ddraw.renderer = gdi_render_main;
        gdi_render_main();
    }

    return 0;
}
"""
new = """    if (!g_ogl.use_opengl)
    {
        /* GDI StretchBlt onto a WGL HWND is a black client plus the
         * "slow software rendering ()" banner. Keep the OpenGL path. */
        if (IsWine())
        {
            fprintf(stderr, "[cnc-ddraw] refusing GDI fallback on Wine context=%p hdc=%p pf=%d\\n",
                    g_ogl.context, g_ogl.hdc, GetPixelFormat(g_ogl.hdc ? g_ogl.hdc : g_ddraw.render.hdc));
            fflush(stderr);
        }
        else
        {
            g_ddraw.show_driver_warning = TRUE;
            g_ddraw.renderer = gdi_render_main;
            gdi_render_main();
        }
    }

    return 0;
}
"""
if old not in ogl:
    raise SystemExit("ogl_render_main GDI fallback site not found")
ogl = ogl.replace(old, new, 1)

old = """        g_ogl.use_opengl = (g_ogl.main_program || g_ddraw.bpp == 16 || g_ddraw.bpp == 32) && !g_ogl.got_error;

        GL_CHECK(ogl_render());
"""
new = """        g_ogl.use_opengl = (g_ogl.main_program || g_ddraw.bpp == 16 || g_ddraw.bpp == 32) && !g_ogl.got_error;
        if (IsWine() && g_ogl.context)
        {
            if (!g_ogl.use_opengl)
                fprintf(stderr, "[cnc-ddraw] keeping OpenGL on Wine despite probe errors (got_error=%d program=%d bpp=%d)\\n",
                        (int)g_ogl.got_error, (int)g_ogl.main_program, g_ddraw.bpp);
            g_ogl.use_opengl = TRUE;
        }
        fprintf(stderr, "[cnc-ddraw] GL init use_opengl=%d version=%s renderer=%s\\n",
                (int)g_ogl.use_opengl,
                glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "?",
                glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
        fflush(stderr);

        GL_CHECK(ogl_render());
"""
if old not in ogl:
    raise SystemExit("use_opengl assignment site not found")
ogl = ogl.replace(old, new, 1)

old = """                if (err != GL_NO_ERROR && err != GL_INVALID_FRAMEBUFFER_OPERATION)
                {
                    g_ogl.use_opengl = FALSE;

                    TRACE("OpenGL error %08x (ogl_render())\\n", err);
                    ogl_check_error("ogl_render()");
                }
"""
new = """                if (err != GL_NO_ERROR && err != GL_INVALID_FRAMEBUFFER_OPERATION)
                {
                    /* virpipe raises benign errors on the first uploads.
                     * Dropping OpenGL here falls through to GDI on an HWND
                     * that already has a GL pixel format — a white window. */
                    if (IsWine())
                    {
                        static int wine_gl_err_logged;
                        if (!wine_gl_err_logged)
                        {
                            wine_gl_err_logged = 1;
                            fprintf(stderr, "[cnc-ddraw] ignore GL error %08x in render loop on Wine\\n", err);
                            fflush(stderr);
                        }
                    }
                    else
                    {
                        g_ogl.use_opengl = FALSE;
                        TRACE("OpenGL error %08x (ogl_render())\\n", err);
                        ogl_check_error("ogl_render()");
                    }
                }
"""
if old not in ogl:
    raise SystemExit("ogl_render error_check_count site not found")
ogl = ogl.replace(old, new, 1)

old = """            if (g_ddraw.renderer == ogl_render_main)
            {
                PIXELFORMATDESCRIPTOR pfd;
                memset(&pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
                pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);

                pfd.nVersion = 1;
                pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = g_ddraw.mode.dmBitsPerPel;
                pfd.iLayerType = PFD_MAIN_PLANE;

                SetPixelFormat(g_ddraw.render.hdc, ChoosePixelFormat(g_ddraw.render.hdc, &pfd), &pfd);
            }
"""
new = """            /* WineHua: SetPixelFormat lives in ogl_create() so a failed GL
             * init does not poison the HWND before GDI can still blit. */
"""
if old not in dd:
    raise SystemExit("dd.c SetPixelFormat site not found")
dd = dd.replace(old, new, 1)

old = """        LONG ref = InterlockedDecrement(&g_ddraw.ref);
"""
new = """        fprintf(stderr, "[cnc-ddraw] selected renderer=%s ogl=%d gdi=%d d3d9=%d\\n",
                g_config.renderer,
                g_ddraw.renderer == ogl_render_main,
                g_ddraw.renderer == gdi_render_main,
                g_ddraw.renderer == d3d9_render_main);
        fflush(stderr);

        LONG ref = InterlockedDecrement(&g_ddraw.ref);
"""
if old not in dd:
    raise SystemExit("dd.c renderer log site not found")
dd = dd.replace(old, new, 1)

old = """        lpDDDriverCaps->dwCaps =
            DDCAPS_BLT | 
            DDCAPS_PALETTE | 
            DDCAPS_BLTCOLORFILL | 
            DDCAPS_BLTSTRETCH | 
            DDCAPS_CANCLIP | 
            DDCAPS_CANBLTSYSMEM |
            DDCAPS_CANCLIPSTRETCHED | 
            DDCAPS_COLORKEY;
"""
new = """        lpDDDriverCaps->dwCaps =
            DDCAPS_3D |
            DDCAPS_BLT | 
            DDCAPS_PALETTE | 
            DDCAPS_BLTCOLORFILL | 
            DDCAPS_BLTSTRETCH | 
            DDCAPS_CANCLIP | 
            DDCAPS_CANBLTSYSMEM |
            DDCAPS_CANCLIPSTRETCHED | 
            DDCAPS_COLORKEY;
"""
if old not in dd:
    raise SystemExit("dd.c GetCaps DDCAPS site not found")
dd = dd.replace(old, new, 1)

old = """    if (IsEqualGUID(&IID_IDirectDraw7, riid))
    {
        memset(pDDDI, 0, sizeof(DDDEVICEIDENTIFIER2));
    }
    else
    {
        memset(pDDDI, 0, sizeof(DDDEVICEIDENTIFIER));
    }
    
    return DD_OK;
"""
new = """    if (IsEqualGUID(&IID_IDirectDraw7, riid))
        memset(pDDDI, 0, sizeof(DDDEVICEIDENTIFIER2));
    else
        memset(pDDDI, 0, sizeof(DDDEVICEIDENTIFIER));

    /* Westwood RA/RA2 reject an all-zero identifier as "no graphics card". */
    strncpy(pDDDI->szDriver, "vga.dll", sizeof(pDDDI->szDriver) - 1);
    strncpy(pDDDI->szDescription, "DirectDraw HAL", sizeof(pDDDI->szDescription) - 1);
    pDDDI->liDriverVersion.HighPart = 0x00010001;
    pDDDI->liDriverVersion.LowPart = 0x00010001;
    pDDDI->dwVendorId = 0x10DE;
    pDDDI->dwDeviceId = 0x1C20;
    pDDDI->guidDeviceIdentifier.Data1 = 0xa8373c10;
    pDDDI->guidDeviceIdentifier.Data2 = 0x7ac4;
    pDDDI->guidDeviceIdentifier.Data3 = 0x4deb;
    pDDDI->guidDeviceIdentifier.Data4[0] = 0x84;
    pDDDI->guidDeviceIdentifier.Data4[1] = 0x9a;
    pDDDI->guidDeviceIdentifier.Data4[2] = 0x00;
    pDDDI->guidDeviceIdentifier.Data4[3] = 0x98;
    pDDDI->guidDeviceIdentifier.Data4[4] = 0x44;
    pDDDI->guidDeviceIdentifier.Data4[5] = 0xd0;
    pDDDI->guidDeviceIdentifier.Data4[6] = 0x8b;
    pDDDI->guidDeviceIdentifier.Data4[7] = 0x2d;
    return DD_OK;
"""
if old not in dd:
    raise SystemExit("dd.c GetDeviceIdentifier site not found")
dd = dd.replace(old, new, 1)

old = """HRESULT dd_SetDisplayMode(DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwFlags)
{
    if (!dwWidth)
        dwWidth = g_ddraw.width ? g_ddraw.width : 800;
"""
new = """HRESULT dd_SetDisplayMode(DWORD dwWidth, DWORD dwHeight, DWORD dwBPP, DWORD dwFlags)
{
    fprintf(stderr, "[cnc-ddraw] SetDisplayMode %ux%u %u bpp windowed=%d\\n",
            (unsigned)dwWidth, (unsigned)dwHeight, (unsigned)dwBPP, g_config.windowed);
    fflush(stderr);

    if (!dwWidth)
        dwWidth = g_ddraw.width ? g_ddraw.width : 800;
"""
if old not in dd:
    raise SystemExit("dd.c SetDisplayMode log site not found")
dd = dd.replace(old, new, 1)

old = """            if (!ogl_create())
            {
                ogl_release();
                g_ddraw.show_driver_warning = TRUE;
                g_ddraw.renderer = gdi_render_main;
            }
"""
new = """            if (!ogl_create())
            {
                fprintf(stderr, "[cnc-ddraw] ogl_create failed hwnd=%p %ux%u bpp=%u\\n",
                        g_ddraw.hwnd, g_ddraw.width, g_ddraw.height, g_ddraw.bpp);
                fflush(stderr);
                if (IsWine())
                {
                    fprintf(stderr, "[cnc-ddraw] refusing GDI fallback after ogl_create\\n");
                    fflush(stderr);
                }
                else
                {
                    ogl_release();
                    g_ddraw.show_driver_warning = TRUE;
                    g_ddraw.renderer = gdi_render_main;
                }
            }
"""
if old not in dd:
    raise SystemExit("dd.c ogl_create GDI fallback site not found")
dd = dd.replace(old, new)

(root / "src/render_ogl.c").write_text(ogl)
(root / "src/dd.c").write_text(dd)

dllmain = (root / "src/dllmain.c").read_text()
old = """HMODULE g_ddraw_module;
static BOOL g_screensaver_disabled;
"""
new = """HMODULE g_ddraw_module;
static BOOL g_screensaver_disabled;
/* Same GUID Wine's ddraw reports so games that skip the NULL HEL device
 * still see a hardware adapter (RA2 launchers: "找不到图形卡"). */
static GUID winehua_dd_hal_guid =
    {0xa8373c10, 0x7ac4, 0x4deb, {0x84, 0x9a, 0x00, 0x98, 0x44, 0xd0, 0x8b, 0x2d}};
"""
if old not in dllmain:
    raise SystemExit("dllmain.c guid site not found")
dllmain = dllmain.replace(old, new, 1)

old = """        g_ddraw_module = hDll;

        verhelp_init();
"""
new = """        g_ddraw_module = hDll;
        {
            char mod[MAX_PATH];
            mod[0] = 0;
            GetModuleFileNameA(hDll, mod, sizeof(mod) - 1);
            fprintf(stderr, "[cnc-ddraw] DllMain attach pid=%lu module=%s\\n",
                    GetCurrentProcessId(), mod);
            fflush(stderr);
        }

        verhelp_init();
"""
if old not in dllmain:
    raise SystemExit("dllmain.c DllMain attach log site not found")
dllmain = dllmain.replace(old, new, 1)

old = """HRESULT WINAPI DirectDrawCreate(GUID FAR* lpGUID, LPDIRECTDRAW FAR* lplpDD, IUnknown FAR* pUnkOuter)
{
    TRACE("-> %s(lpGUID=%p, lplpDD=%p, pUnkOuter=%p) [%p]\\n", __FUNCTION__, lpGUID, lplpDD, pUnkOuter, _ReturnAddress());
"""
new = """HRESULT WINAPI DirectDrawCreate(GUID FAR* lpGUID, LPDIRECTDRAW FAR* lplpDD, IUnknown FAR* pUnkOuter)
{
    fprintf(stderr, "[cnc-ddraw] DirectDrawCreate guid=%p\\n", lpGUID);
    fflush(stderr);
    TRACE("-> %s(lpGUID=%p, lplpDD=%p, pUnkOuter=%p) [%p]\\n", __FUNCTION__, lpGUID, lplpDD, pUnkOuter, _ReturnAddress());
"""
if old not in dllmain:
    raise SystemExit("dllmain.c DirectDrawCreate log site not found")
dllmain = dllmain.replace(old, new, 1)

old = """HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACK lpCallback, LPVOID lpContext)
{
    TRACE("-> %s(lpCallback=%p, lpContext=%p) [%p]\\n", __FUNCTION__, lpCallback, lpContext, _ReturnAddress());

    if (lpCallback)
        lpCallback(NULL, "Primary Display Driver", "display", lpContext);

    TRACE("<- %s\\n", __FUNCTION__);
    return DD_OK;
}
"""
new = """HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACK lpCallback, LPVOID lpContext)
{
    TRACE("-> %s(lpCallback=%p, lpContext=%p) [%p]\\n", __FUNCTION__, lpCallback, lpContext, _ReturnAddress());

    fprintf(stderr, "[cnc-ddraw] DirectDrawEnumerateA\\n");
    fflush(stderr);
    if (lpCallback)
    {
        if (lpCallback(NULL, "DirectDraw HAL", "display", lpContext))
            lpCallback(&winehua_dd_hal_guid, "WineHua Primary Display Driver", "display", lpContext);
    }

    TRACE("<- %s\\n", __FUNCTION__);
    return DD_OK;
}
"""
if old not in dllmain:
    raise SystemExit("dllmain.c DirectDrawEnumerateA site not found")
dllmain = dllmain.replace(old, new, 1)

old = """    if (lpCallback)
        lpCallback(NULL, "Primary Display Driver", "display", lpContext, NULL);

    TRACE("<- %s\\n", __FUNCTION__);
    return DD_OK;
}
"""
new = """    if (lpCallback)
    {
        if (lpCallback(NULL, "DirectDraw HAL", "display", lpContext, NULL))
            lpCallback(&winehua_dd_hal_guid, "WineHua Primary Display Driver", "display", lpContext, NULL);
    }

    TRACE("<- %s\\n", __FUNCTION__);
    return DD_OK;
}
"""
if old not in dllmain:
    raise SystemExit("dllmain.c DirectDrawEnumerateExA site not found")
dllmain = dllmain.replace(old, new, 1)
(root / "src/dllmain.c").write_text(dllmain)

cfg = (root / "src/config.c").read_text()
old = """    GET_STRING("renderer", "auto", g_config.renderer, sizeof(g_config.renderer));
    GET_BOOL(g_config.devmode, "devmode", FALSE);
"""
new = """    GET_STRING("renderer", "auto", g_config.renderer, sizeof(g_config.renderer));
    {
        const char *cnc_renderer = getenv("WINEHUA_CNC_RENDERER");
        if (cnc_renderer && cnc_renderer[0] && lstrcmpiA(cnc_renderer, "0") != 0)
        {
            strncpy(g_config.renderer, cnc_renderer, sizeof(g_config.renderer) - 1);
            g_config.renderer[sizeof(g_config.renderer) - 1] = 0;
            winehua_logf("[cnc-ddraw] WINEHUA_CNC_RENDERER=%s\\n", g_config.renderer);
        }
    }
    GET_BOOL(g_config.devmode, "devmode", FALSE);
"""
if old not in cfg:
    raise SystemExit("config.c WINEHUA_CNC_RENDERER site not found")
cfg = cfg.replace(old, new, 1)
old = """    if (GetFileAttributes(g_config.ini_path) == INVALID_FILE_ATTRIBUTES)
    {
        cfg_create_ini();
    }

    ini_create(&g_config.ini, g_config.ini_path);
"""
new = """    if (GetFileAttributes(g_config.ini_path) == INVALID_FILE_ATTRIBUTES)
    {
        /* WineHua sets CNC_DDRAW_CONFIG_FILE to a Unix path. Win32
         * GetFileAttributes/GetPrivateProfile cannot open it, so fall back
         * to the staged syswow64 copy. */
        static const char kDosIni[] = "C:\\\\windows\\\\syswow64\\\\ddraw.ini";
        if (g_config.ini_path[0] == '/' &&
            GetFileAttributesA(kDosIni) != INVALID_FILE_ATTRIBUTES)
        {
            strncpy(g_config.ini_path, kDosIni, sizeof(g_config.ini_path) - 1);
            g_config.ini_path[sizeof(g_config.ini_path) - 1] = 0;
        }
        else
        {
            cfg_create_ini();
        }
    }
    if (GetFileAttributes(g_config.ini_path) == INVALID_FILE_ATTRIBUTES)
        cfg_create_ini();

    fprintf(stderr, "[cnc-ddraw] config ini=%s attrs=%lu\\n",
            g_config.ini_path, GetFileAttributes(g_config.ini_path));
    fflush(stderr);

    ini_create(&g_config.ini, g_config.ini_path);
"""
if old not in cfg:
    raise SystemExit("config.c ini fallback site not found")
cfg = cfg.replace(old, new, 1)
(root / "src/config.c").write_text(cfg)

hooks = (root / "src/winapi_hooks.c").read_text()
old = """BOOL WINAPI fake_EnumDisplaySettingsA(LPCSTR lpszDeviceName, DWORD iModeNum, DEVMODEA* lpDevMode)
{
    BOOL result = real_EnumDisplaySettingsA(lpszDeviceName, iModeNum, lpDevMode);

    if (result && !lpszDeviceName && lpDevMode && iModeNum == ENUM_CURRENT_SETTINGS)
    {
        if (g_ddraw.ref && g_ddraw.width)
        {
            lpDevMode->dmPelsWidth = g_ddraw.width;
            lpDevMode->dmPelsHeight = g_ddraw.height;
            lpDevMode->dmBitsPerPel = g_ddraw.bpp;
        }
        else if (g_config.fake_mode[0])
        {
            char* e = &g_config.fake_mode[0];

            lpDevMode->dmPelsWidth = strtoul(e, &e, 0);
            lpDevMode->dmPelsHeight = strtoul(e + 1, &e, 0);
            lpDevMode->dmBitsPerPel = strtoul(e + 1, &e, 0);
        }
        else
        {
            lpDevMode->dmPelsWidth = 1024;
            lpDevMode->dmPelsHeight = 768;
            lpDevMode->dmBitsPerPel = 16;
        }

        lpDevMode->dmDisplayFrequency = 60;
    }

    if (result && !lpszDeviceName && lpDevMode && iModeNum != ENUM_CURRENT_SETTINGS)
    {
        //lpDevMode->dmBitsPerPel = 16;
    }

    return result;
}
"""
new = """BOOL WINAPI fake_EnumDisplaySettingsA(LPCSTR lpszDeviceName, DWORD iModeNum, DEVMODEA* lpDevMode)
{
    static const struct { DWORD w, h, bpp; } extra[] = {
        {640, 480, 8}, {640, 480, 16}, {640, 480, 32},
        {800, 600, 8}, {800, 600, 16}, {800, 600, 32},
        {1024, 768, 16}, {1024, 768, 32},
        {1280, 720, 16}, {1280, 720, 32},
        {1280, 800, 16}, {1280, 800, 32},
    };
    BOOL result = real_EnumDisplaySettingsA(lpszDeviceName, iModeNum, lpDevMode);

    if (result && !lpszDeviceName && lpDevMode && iModeNum == ENUM_CURRENT_SETTINGS)
    {
        if (g_ddraw.ref && g_ddraw.width)
        {
            lpDevMode->dmPelsWidth = g_ddraw.width;
            lpDevMode->dmPelsHeight = g_ddraw.height;
            lpDevMode->dmBitsPerPel = g_ddraw.bpp;
        }
        else if (g_config.fake_mode[0])
        {
            char* e = &g_config.fake_mode[0];

            lpDevMode->dmPelsWidth = strtoul(e, &e, 0);
            lpDevMode->dmPelsHeight = strtoul(e + 1, &e, 0);
            lpDevMode->dmBitsPerPel = strtoul(e + 1, &e, 0);
        }
        else
        {
            lpDevMode->dmPelsWidth = 1024;
            lpDevMode->dmPelsHeight = 768;
            lpDevMode->dmBitsPerPel = 16;
        }

        lpDevMode->dmDisplayFrequency = 60;
        return TRUE;
    }

    if (result)
        return TRUE;

    if (iModeNum == ENUM_CURRENT_SETTINGS || iModeNum == ENUM_REGISTRY_SETTINGS || !lpDevMode)
        return FALSE;

    {
        DWORD real_count = 0;
        DEVMODEA tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.dmSize = sizeof(tmp);
        while (real_EnumDisplaySettingsA(lpszDeviceName, real_count, &tmp))
        {
            real_count++;
            memset(&tmp, 0, sizeof(tmp));
            tmp.dmSize = sizeof(tmp);
        }
        if (iModeNum >= real_count)
        {
            DWORD extra_i = iModeNum - real_count;
            if (extra_i < (DWORD)(sizeof(extra) / sizeof(extra[0])))
            {
                memset(lpDevMode, 0, lpDevMode->dmSize ? lpDevMode->dmSize : sizeof(*lpDevMode));
                lpDevMode->dmSize = sizeof(DEVMODEA);
                lpDevMode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
                lpDevMode->dmPelsWidth = extra[extra_i].w;
                lpDevMode->dmPelsHeight = extra[extra_i].h;
                lpDevMode->dmBitsPerPel = extra[extra_i].bpp;
                lpDevMode->dmDisplayFrequency = 60;
                return TRUE;
            }
        }
    }

    return FALSE;
}
"""
if old not in hooks:
    raise SystemExit("winapi_hooks.c EnumDisplaySettings site not found")
hooks = hooks.replace(old, new, 1)
(root / "src/winapi_hooks.c").write_text(hooks)

d3dh = (root / "inc/IDirect3D.h").read_text()
old = """DEFINE_GUID(IID_IDirect3DNullDevice, 0x8767df22, 0xbacc, 0x11d1, 0x89, 0x69, 0x0, 0xa0, 0xc9, 0x6, 0x29, 0xa8);
"""
new = """DEFINE_GUID(IID_IDirect3DNullDevice, 0x8767df22, 0xbacc, 0x11d1, 0x89, 0x69, 0x0, 0xa0, 0xc9, 0x6, 0x29, 0xa8);
DEFINE_GUID(IID_IDirect3DHALDevice, 0x84E63dE0, 0x46AA, 0x11CF, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E);
DEFINE_GUID(IID_IDirect3DTnLHalDevice, 0xf5049e78, 0x4861, 0x11d2, 0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8);
"""
if old not in d3dh:
    raise SystemExit("IDirect3D.h HAL guid site not found")
d3dh = d3dh.replace(old, new, 1)
(root / "inc/IDirect3D.h").write_text(d3dh)

d3d7 = (root / "src/IDirect3D/IDirect3D7.c").read_text()
old = """HRESULT __stdcall IDirect3D7__EnumDevices(
    IDirect3D7Impl* This,
    LPD3DENUMDEVICESCALLBACK7 lpEnumDevicesCallback,
    LPVOID lpUserArg)
{
    TRACE("NOT_IMPLEMENTED -> %s(This=%p) [%p]\\n", __FUNCTION__, This, _ReturnAddress());
    HRESULT ret = S_OK;

    if (lpEnumDevicesCallback)
    {
        //D3DDEVICEDESC7 desc = { 0 };
        //lpEnumDevicesCallback("NULL", "NULL", &desc, lpUserArg);
    }

    TRACE("NOT_IMPLEMENTED <- %s\\n", __FUNCTION__);
    return ret;
}
"""
new = """static void winehua_fill_d3d7_hal(D3DDEVICEDESC7* desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->dwDevCaps =
        D3DDEVCAPS_FLOATTLVERTEX |
        D3DDEVCAPS_EXECUTESYSTEMMEMORY |
        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
        D3DDEVCAPS_TEXTURESYSTEMMEMORY |
        D3DDEVCAPS_TEXTUREVIDEOMEMORY |
        D3DDEVCAPS_DRAWPRIMTLVERTEX |
        D3DDEVCAPS_CANRENDERAFTERFLIP |
        D3DDEVCAPS_DRAWPRIMITIVES2 |
        D3DDEVCAPS_DRAWPRIMITIVES2EX |
        D3DDEVCAPS_HWRASTERIZATION |
        D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    desc->dpcLineCaps.dwSize = sizeof(desc->dpcLineCaps);
    desc->dpcTriCaps.dwSize = sizeof(desc->dpcTriCaps);
    desc->dpcTriCaps.dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    desc->dpcTriCaps.dwRasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX;
    desc->dpcTriCaps.dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB;
    desc->dpcTriCaps.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE;
    desc->dpcLineCaps = desc->dpcTriCaps;
    desc->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    desc->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_32;
    desc->dwMinTextureWidth = 1;
    desc->dwMinTextureHeight = 1;
    desc->dwMaxTextureWidth = 2048;
    desc->dwMaxTextureHeight = 2048;
    desc->dwMaxTextureRepeat = 2048;
    desc->wMaxTextureBlendStages = 8;
    desc->wMaxSimultaneousTextures = 8;
    desc->dwMaxActiveLights = 8;
    desc->deviceGUID = IID_IDirect3DHALDevice;
}

HRESULT __stdcall IDirect3D7__EnumDevices(
    IDirect3D7Impl* This,
    LPD3DENUMDEVICESCALLBACK7 lpEnumDevicesCallback,
    LPVOID lpUserArg)
{
    D3DDEVICEDESC7 desc;
    TRACE("-> %s(This=%p) [%p]\\n", __FUNCTION__, This, _ReturnAddress());
    fprintf(stderr, "[cnc-ddraw] IDirect3D7::EnumDevices\\n");
    fflush(stderr);

    if (lpEnumDevicesCallback)
    {
        winehua_fill_d3d7_hal(&desc);
        lpEnumDevicesCallback("WineHua Direct3D HAL", "Direct3D HAL", &desc, lpUserArg);
    }

    TRACE("<- %s\\n", __FUNCTION__);
    return S_OK;
}
"""
if old not in d3d7:
    raise SystemExit("IDirect3D7.c EnumDevices site not found")
d3d7 = d3d7.replace(old, new, 1)
if '#include <stdio.h>' not in d3d7:
    d3d7 = '#include <stdio.h>\n#include <string.h>\n' + d3d7
(root / "src/IDirect3D/IDirect3D7.c").write_text(d3d7)

for rel in (
    "src/render_ogl.c",
    "src/dd.c",
    "src/config.c",
    "src/dllmain.c",
    "src/IDirect3D/IDirect3D7.c",
):
    p = root / rel
    text = p.read_text()
    if 'fprintf(stderr, "[cnc-ddraw]' in text:
        p.write_text(text.replace('fprintf(stderr, "[cnc-ddraw]', 'winehua_logf("[cnc-ddraw]'))
PY
if ! grep -q 'skip glGetTexImage upload probe on Wine' "$SOURCE_COPY/src/render_ogl.c"; then
    err "cnc-ddraw WineHua GetTexImage skip did not apply"
fi
if ! grep -q 'ignore GL error' "$SOURCE_COPY/src/render_ogl.c"; then
    err "cnc-ddraw WineHua render-loop GL error skip did not apply"
fi
if ! grep -q 'SetPixelFormat lives in ogl_create' "$SOURCE_COPY/src/dd.c"; then
    err "cnc-ddraw WineHua SetPixelFormat move did not apply"
fi
if ! grep -q 'WineHua Primary Display Driver' "$SOURCE_COPY/src/dllmain.c"; then
    err "cnc-ddraw WineHua DirectDrawEnumerate HAL did not apply"
fi
if ! grep -q 'WINEHUA_CNC_RENDERER' "$SOURCE_COPY/src/config.c"; then
    err "cnc-ddraw WineHua WINEHUA_CNC_RENDERER did not apply"
fi
if ! grep -q 'C:\\\\windows\\\\syswow64\\\\ddraw.ini' "$SOURCE_COPY/src/config.c"; then
    err "cnc-ddraw WineHua DOS ini fallback did not apply"
fi
if ! grep -q 'IDirect3D7::EnumDevices' "$SOURCE_COPY/src/IDirect3D/IDirect3D7.c"; then
    err "cnc-ddraw WineHua D3D7 EnumDevices did not apply"
fi
if ! grep -q '{640, 480, 8}' "$SOURCE_COPY/src/winapi_hooks.c"; then
    err "cnc-ddraw WineHua EnumDisplaySettings extras did not apply"
fi
if ! grep -q 'DDCAPS_3D' "$SOURCE_COPY/src/dd.c"; then
    err "cnc-ddraw WineHua DDCAPS_3D did not apply"
fi
if ! grep -q 'winehua_logf' "$SOURCE_COPY/inc/debug.h"; then
    err "cnc-ddraw WineHua file logger did not apply"
fi
if ! grep -q 'DllMain attach' "$SOURCE_COPY/src/dllmain.c"; then
    err "cnc-ddraw WineHua DllMain attach log did not apply"
fi
if ! grep -q 'refusing GDI fallback' "$SOURCE_COPY/src/render_ogl.c"; then
    err "cnc-ddraw WineHua GDI refuse did not apply"
fi

log "=== Build cnc-ddraw $CNC_TAG (x86) ==="
make -C "$SOURCE_COPY" clean >/dev/null 2>&1 || true
make -C "$SOURCE_COPY" -j"$(nproc)" TARGET=ddraw.dll
[ -s "$SOURCE_COPY/ddraw.dll" ] || err "cnc-ddraw x86 build produced no ddraw.dll"
cp "$SOURCE_COPY/ddraw.dll" "$OUTPUT_ROOT/x86/ddraw.dll"
if [ -d "$SOURCE_COPY/Shaders" ]; then
    mkdir -p "$OUTPUT_ROOT/Shaders"
    cp -a "$SOURCE_COPY/Shaders/." "$OUTPUT_ROOT/Shaders/"
fi
x86_sha="$(sha256sum "$OUTPUT_ROOT/x86/ddraw.dll" | awk '{print $1}')"
log "  x86 ddraw.dll sha256=$x86_sha"

x64_sha=""

# WineHua overlay defaults: OpenGL via Wine WGL → guest Mesa virpipe →
# winehua_gl_present_bridge. Borderless (windowed+fullscreen) stretches to
# the desktop without exclusive ChangeDisplaySettings.
write_winehua_ini() {
    cat > "$1" <<'INI'
; WineHua cnc-ddraw overlay defaults. Games may override via
; CNC_DDRAW_CONFIG_FILE or a ddraw.ini next to the game executable.
[ddraw]
renderer=opengl
; Zink-on-Venus: WINEHUA_GL_BACKEND=zink sets WINEHUA_CNC_RENDERER=openglcore.
; Leave renderer=opengl for virpipe (GLES host has no reliable core profile).
windowed=true
fullscreen=false
border=true
nonexclusive=true
vsync=false
maxfps=0
shader=Bilinear
hook=4
no_compat_warning=true
savesettings=0
resolutions=2
fake_mode=800x600x16

; Westwood TS/RA2/YR. Overlay [ddraw] is windowed OpenGL; without these
; sections tshack stays false and YR presents a black primary plus the
; cnc-ddraw GDI "slow software rendering" banner.
[gamemd]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1
maintas=false
boxing=false

[gamemd/wine]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true
no_compat_warning=true

[ra2md]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1
maintas=false
boxing=false

[ra2md/wine]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true

[ra2]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1

[game]
windowed=true
fullscreen=false
nonexclusive=true
tshack=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1

; StarCraft (classic). Same DDraw HAL / windowed OpenGL overlay as RA2.
[StarCraft]
windowed=true
fullscreen=false
nonexclusive=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1

[starcraft]
windowed=true
fullscreen=false
nonexclusive=true
noactivateapp=true
no_compat_warning=true
maxfps=60
minfps=-1
INI
}
write_winehua_ini "$OUTPUT_ROOT/x86/ddraw.ini"
[ -d "$OUTPUT_ROOT/x64" ] && write_winehua_ini "$OUTPUT_ROOT/x64/ddraw.ini"
write_winehua_ini "$OUTPUT_ROOT/ddraw.ini"
ini_sha="$(sha256sum "$OUTPUT_ROOT/ddraw.ini" | awk '{print $1}')"

x64_manifest=""
[ -n "$x64_sha" ] && x64_manifest=",
    \"x64\": {\"ddraw.dll\": \"$x64_sha\"}"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": 1,
  "backend": "cnc-ddraw",
  "upstream": "https://github.com/FunkyFr3sh/cnc-ddraw",
  "tag": "$CNC_TAG",
  "commit": "$base_commit",
  "runtimeRoot": "cnc-ddraw",
  "files": {
    "ddraw.ini": "$ini_sha",
    "x86": {"ddraw.dll": "$x86_sha"}$x64_manifest
  }
}
EOF

log "cnc-ddraw dist → $OUTPUT_ROOT"
