/*
 * WineHua Graphics Diagnostic Test
 *
 * Purpose:
 *   Standalone Win32 diagnostic program for Wine + VirGL/OpenGL/WGL and optional D3D9/D3D11 checks.
 *   It is designed to validate implementation completeness, not to be a benchmark.
 *
 * Build examples:
 *   MinGW-w64:
 *     x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -o winehua_graphics_diag.exe winehua_graphics_diag.c -lopengl32 -lgdi32 -luser32
 *
 *   MinGW-w64 with D3D headers/libs available:
 *     x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -DWH_ENABLE_D3D=1 -o winehua_graphics_diag.exe winehua_graphics_diag.c -lopengl32 -lgdi32 -luser32 -ld3d9 -ld3d11 -ldxgi
 *
 *   MSVC Developer Prompt:
 *     cl /O2 /W3 /DCOBJMACROS /DWH_ENABLE_D3D=1 winehua_graphics_diag.c user32.lib gdi32.lib opengl32.lib d3d9.lib d3d11.lib dxgi.lib
 *
 * Runtime examples:
 *   wine winehua_graphics_diag.exe --visible --seconds 5
 *   wine winehua_graphics_diag.exe --quick --log diag.log
 *   wine winehua_graphics_diag.exe --stress --seconds 30
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

#ifndef WH_ENABLE_D3D
#define WH_ENABLE_D3D 1
#endif

#ifndef WH_HAS_INCLUDE
# if defined(__has_include)
#  define WH_HAS_INCLUDE(x) __has_include(x)
# else
#  define WH_HAS_INCLUDE(x) 0
# endif
#endif

#if WH_ENABLE_D3D && WH_HAS_INCLUDE(<d3d9.h>)
# include <d3d9.h>
# define WH_HAS_D3D9 1
#else
# define WH_HAS_D3D9 0
#endif

#if WH_ENABLE_D3D && WH_HAS_INCLUDE(<d3d11.h>) && WH_HAS_INCLUDE(<dxgi.h>)
# include <d3d11.h>
# include <dxgi.h>
# define WH_HAS_D3D11 1
#else
# define WH_HAS_D3D11 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- GL constants not guaranteed by Windows OpenGL 1.1 headers ---- */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS 0x84E2
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_FLAGS_ARB
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
#ifndef WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif
#ifndef WGL_CONTEXT_DEBUG_BIT_ARB
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x00000001
#endif
#ifndef WGL_DRAW_TO_WINDOW_ARB
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#endif
#ifndef WGL_ACCELERATION_ARB
#define WGL_ACCELERATION_ARB 0x2003
#endif
#ifndef WGL_SUPPORT_OPENGL_ARB
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#endif
#ifndef WGL_DOUBLE_BUFFER_ARB
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#endif
#ifndef WGL_PIXEL_TYPE_ARB
#define WGL_PIXEL_TYPE_ARB 0x2013
#endif
#ifndef WGL_COLOR_BITS_ARB
#define WGL_COLOR_BITS_ARB 0x2014
#endif
#ifndef WGL_ALPHA_BITS_ARB
#define WGL_ALPHA_BITS_ARB 0x201B
#endif
#ifndef WGL_DEPTH_BITS_ARB
#define WGL_DEPTH_BITS_ARB 0x2022
#endif
#ifndef WGL_STENCIL_BITS_ARB
#define WGL_STENCIL_BITS_ARB 0x2023
#endif
#ifndef WGL_FULL_ACCELERATION_ARB
#define WGL_FULL_ACCELERATION_ARB 0x2027
#endif
#ifndef WGL_TYPE_RGBA_ARB
#define WGL_TYPE_RGBA_ARB 0x202B
#endif
#ifndef WGL_SAMPLE_BUFFERS_ARB
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#endif
#ifndef WGL_SAMPLES_ARB
#define WGL_SAMPLES_ARB 0x2042
#endif

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

typedef ptrdiff_t GLsizeiptr_diag;
typedef char GLchar_diag;

typedef const char *(WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC_DIAG)(HDC hdc);
typedef const char *(WINAPI *PFNWGLGETEXTENSIONSSTRINGEXTPROC_DIAG)(void);
typedef BOOL (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC_DIAG)(HDC hdc, const int *piAttribIList, const FLOAT *pfAttribFList, UINT nMaxFormats, int *piFormats, UINT *nNumFormats);
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC_DIAG)(HDC hdc, HGLRC hShareContext, const int *attribList);
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC_DIAG)(int interval);
typedef int  (WINAPI *PFNWGLGETSWAPINTERVALEXTPROC_DIAG)(void);

typedef void (APIENTRYP PFNGLGENBUFFERSPROC_DIAG)(GLsizei n, GLuint *buffers);
typedef void (APIENTRYP PFNGLBINDBUFFERPROC_DIAG)(GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAPROC_DIAG)(GLenum target, GLsizeiptr_diag size, const void *data, GLenum usage);
typedef void (APIENTRYP PFNGLDELETEBUFFERSPROC_DIAG)(GLsizei n, const GLuint *buffers);
typedef void (APIENTRYP PFNGLGENVERTEXARRAYSPROC_DIAG)(GLsizei n, GLuint *arrays);
typedef void (APIENTRYP PFNGLBINDVERTEXARRAYPROC_DIAG)(GLuint array);
typedef void (APIENTRYP PFNGLDELETEVERTEXARRAYSPROC_DIAG)(GLsizei n, const GLuint *arrays);
typedef GLuint (APIENTRYP PFNGLCREATESHADERPROC_DIAG)(GLenum type);
typedef void (APIENTRYP PFNGLSHADERSOURCEPROC_DIAG)(GLuint shader, GLsizei count, const GLchar_diag *const *string, const GLint *length);
typedef void (APIENTRYP PFNGLCOMPILESHADERPROC_DIAG)(GLuint shader);
typedef void (APIENTRYP PFNGLGETSHADERIVPROC_DIAG)(GLuint shader, GLenum pname, GLint *params);
typedef void (APIENTRYP PFNGLGETSHADERINFOLOGPROC_DIAG)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar_diag *infoLog);
typedef void (APIENTRYP PFNGLDELETESHADERPROC_DIAG)(GLuint shader);
typedef GLuint (APIENTRYP PFNGLCREATEPROGRAMPROC_DIAG)(void);
typedef void (APIENTRYP PFNGLATTACHSHADERPROC_DIAG)(GLuint program, GLuint shader);
typedef void (APIENTRYP PFNGLLINKPROGRAMPROC_DIAG)(GLuint program);
typedef void (APIENTRYP PFNGLGETPROGRAMIVPROC_DIAG)(GLuint program, GLenum pname, GLint *params);
typedef void (APIENTRYP PFNGLGETPROGRAMINFOLOGPROC_DIAG)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar_diag *infoLog);
typedef void (APIENTRYP PFNGLUSEPROGRAMPROC_DIAG)(GLuint program);
typedef void (APIENTRYP PFNGLDELETEPROGRAMPROC_DIAG)(GLuint program);
typedef GLint (APIENTRYP PFNGLGETATTRIBLOCATIONPROC_DIAG)(GLuint program, const GLchar_diag *name);
typedef void (APIENTRYP PFNGLVERTEXATTRIBPOINTERPROC_DIAG)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
typedef void (APIENTRYP PFNGLENABLEVERTEXATTRIBARRAYPROC_DIAG)(GLuint index);
typedef void (APIENTRYP PFNGLDISABLEVERTEXATTRIBARRAYPROC_DIAG)(GLuint index);
typedef void (APIENTRYP PFNGLACTIVETEXTUREPROC_DIAG)(GLenum texture);
typedef void (APIENTRYP PFNGLCOMPRESSEDTEXIMAGE2DPROC_DIAG)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
typedef void (APIENTRYP PFNGLGENERATEMIPMAPPROC_DIAG)(GLenum target);
typedef void (APIENTRYP PFNGLGENFRAMEBUFFERSPROC_DIAG)(GLsizei n, GLuint *framebuffers);
typedef void (APIENTRYP PFNGLBINDFRAMEBUFFERPROC_DIAG)(GLenum target, GLuint framebuffer);
typedef void (APIENTRYP PFNGLDELETEFRAMEBUFFERSPROC_DIAG)(GLsizei n, const GLuint *framebuffers);
typedef void (APIENTRYP PFNGLFRAMEBUFFERTEXTURE2DPROC_DIAG)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRYP PFNGLCHECKFRAMEBUFFERSTATUSPROC_DIAG)(GLenum target);
typedef void (APIENTRYP PFNGLGENRENDERBUFFERSPROC_DIAG)(GLsizei n, GLuint *renderbuffers);
typedef void (APIENTRYP PFNGLBINDRENDERBUFFERPROC_DIAG)(GLenum target, GLuint renderbuffer);
typedef void (APIENTRYP PFNGLDELETERENDERBUFFERSPROC_DIAG)(GLsizei n, const GLuint *renderbuffers);
typedef void (APIENTRYP PFNGLRENDERBUFFERSTORAGEPROC_DIAG)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (APIENTRYP PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC_DIAG)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (APIENTRYP PFNGLFRAMEBUFFERRENDERBUFFERPROC_DIAG)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
typedef void (APIENTRYP PFNGLBLITFRAMEBUFFERPROC_DIAG)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

struct options {
    int visible;
    int quick;
    int stress;
    int seconds;
    const char *log_path;
};

struct window_ctx {
    HWND hwnd;
    HDC hdc;
    HGLRC rc;
    int width;
    int height;
};

struct diag_state {
    FILE *log;
    int pass_count;
    int warn_count;
    int fail_count;
    int running;
    struct options opt;
    HMODULE opengl32;

    PFNWGLGETEXTENSIONSSTRINGARBPROC_DIAG wglGetExtensionsStringARB_p;
    PFNWGLGETEXTENSIONSSTRINGEXTPROC_DIAG wglGetExtensionsStringEXT_p;
    PFNWGLCHOOSEPIXELFORMATARBPROC_DIAG wglChoosePixelFormatARB_p;
    PFNWGLCREATECONTEXTATTRIBSARBPROC_DIAG wglCreateContextAttribsARB_p;
    PFNWGLSWAPINTERVALEXTPROC_DIAG wglSwapIntervalEXT_p;
    PFNWGLGETSWAPINTERVALEXTPROC_DIAG wglGetSwapIntervalEXT_p;

    PFNGLGENBUFFERSPROC_DIAG glGenBuffers_p;
    PFNGLBINDBUFFERPROC_DIAG glBindBuffer_p;
    PFNGLBUFFERDATAPROC_DIAG glBufferData_p;
    PFNGLDELETEBUFFERSPROC_DIAG glDeleteBuffers_p;
    PFNGLGENVERTEXARRAYSPROC_DIAG glGenVertexArrays_p;
    PFNGLBINDVERTEXARRAYPROC_DIAG glBindVertexArray_p;
    PFNGLDELETEVERTEXARRAYSPROC_DIAG glDeleteVertexArrays_p;
    PFNGLCREATESHADERPROC_DIAG glCreateShader_p;
    PFNGLSHADERSOURCEPROC_DIAG glShaderSource_p;
    PFNGLCOMPILESHADERPROC_DIAG glCompileShader_p;
    PFNGLGETSHADERIVPROC_DIAG glGetShaderiv_p;
    PFNGLGETSHADERINFOLOGPROC_DIAG glGetShaderInfoLog_p;
    PFNGLDELETESHADERPROC_DIAG glDeleteShader_p;
    PFNGLCREATEPROGRAMPROC_DIAG glCreateProgram_p;
    PFNGLATTACHSHADERPROC_DIAG glAttachShader_p;
    PFNGLLINKPROGRAMPROC_DIAG glLinkProgram_p;
    PFNGLGETPROGRAMIVPROC_DIAG glGetProgramiv_p;
    PFNGLGETPROGRAMINFOLOGPROC_DIAG glGetProgramInfoLog_p;
    PFNGLUSEPROGRAMPROC_DIAG glUseProgram_p;
    PFNGLDELETEPROGRAMPROC_DIAG glDeleteProgram_p;
    PFNGLGETATTRIBLOCATIONPROC_DIAG glGetAttribLocation_p;
    PFNGLVERTEXATTRIBPOINTERPROC_DIAG glVertexAttribPointer_p;
    PFNGLENABLEVERTEXATTRIBARRAYPROC_DIAG glEnableVertexAttribArray_p;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC_DIAG glDisableVertexAttribArray_p;
    PFNGLACTIVETEXTUREPROC_DIAG glActiveTexture_p;
    PFNGLCOMPRESSEDTEXIMAGE2DPROC_DIAG glCompressedTexImage2D_p;
    PFNGLGENERATEMIPMAPPROC_DIAG glGenerateMipmap_p;
    PFNGLGENFRAMEBUFFERSPROC_DIAG glGenFramebuffers_p;
    PFNGLBINDFRAMEBUFFERPROC_DIAG glBindFramebuffer_p;
    PFNGLDELETEFRAMEBUFFERSPROC_DIAG glDeleteFramebuffers_p;
    PFNGLFRAMEBUFFERTEXTURE2DPROC_DIAG glFramebufferTexture2D_p;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC_DIAG glCheckFramebufferStatus_p;
    PFNGLGENRENDERBUFFERSPROC_DIAG glGenRenderbuffers_p;
    PFNGLBINDRENDERBUFFERPROC_DIAG glBindRenderbuffer_p;
    PFNGLDELETERENDERBUFFERSPROC_DIAG glDeleteRenderbuffers_p;
    PFNGLRENDERBUFFERSTORAGEPROC_DIAG glRenderbufferStorage_p;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC_DIAG glRenderbufferStorageMultisample_p;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC_DIAG glFramebufferRenderbuffer_p;
    PFNGLBLITFRAMEBUFFERPROC_DIAG glBlitFramebuffer_p;
};

static struct diag_state g_diag;

static void log_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    if (g_diag.log) {
        va_start(args, fmt);
        vfprintf(g_diag.log, fmt, args);
        fflush(g_diag.log);
        va_end(args);
    }
}

static void report_result(const char *level, const char *category, const char *name, const char *fmt, ...)
{
    char detail[2048];
    va_list args;

    if (!strcmp(level, "PASS")) g_diag.pass_count++;
    else if (!strcmp(level, "WARN")) g_diag.warn_count++;
    else if (!strcmp(level, "FAIL")) g_diag.fail_count++;

    detail[0] = 0;
    if (fmt && fmt[0]) {
        va_start(args, fmt);
        vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }

    log_printf("[%s] %-12s %-36s %s\n", level, category, name, detail);
}

#define PASS(cat, name, ...) report_result("PASS", cat, name, __VA_ARGS__)
#define WARN(cat, name, ...) report_result("WARN", cat, name, __VA_ARGS__)
#define FAIL(cat, name, ...) report_result("FAIL", cat, name, __VA_ARGS__)

static int contains_token(const char *list, const char *token)
{
    const char *p;
    size_t len;
    if (!list || !token || !token[0]) return 0;
    len = strlen(token);
    p = list;
    while ((p = strstr(p, token)) != NULL) {
        int left_ok = (p == list) || (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n');
        int right_ok = (p[len] == 0) || (p[len] == ' ' || p[len] == '\t' || p[len] == '\n');
        if (left_ok && right_ok) return 1;
        p += len;
    }
    return 0;
}

static int is_wine_runtime(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

static void *get_gl_proc(const char *name)
{
    PROC p = wglGetProcAddress(name);
    if (!p || p == (PROC)0x1 || p == (PROC)0x2 || p == (PROC)0x3 || p == (PROC)-1) {
        if (!g_diag.opengl32) g_diag.opengl32 = LoadLibraryA("opengl32.dll");
        if (g_diag.opengl32) p = GetProcAddress(g_diag.opengl32, name);
    }
    return (void *)p;
}

static GLenum gl_clear_error(void)
{
    GLenum err = GL_NO_ERROR;
    while ((err = glGetError()) != GL_NO_ERROR) { }
    return err;
}

static int gl_expect_ok(const char *category, const char *name)
{
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) {
        PASS(category, name, "GL_NO_ERROR");
        return 1;
    }
    FAIL(category, name, "glGetError=0x%04X", (unsigned)err);
    return 0;
}

static LRESULT CALLBACK diag_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct window_ctx *ctx = (struct window_ctx *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams);
        return TRUE;
    case WM_SIZE:
        if (ctx) {
            ctx->width = LOWORD(lparam);
            ctx->height = HIWORD(lparam);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

static int register_diag_class(void)
{
    static int done = 0;
    WNDCLASSEXW wc;
    if (done) return 1;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = diag_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"WineHuaGraphicsDiagWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        FAIL("WIN32", "RegisterClassExW", "GetLastError=%lu", GetLastError());
        return 0;
    }
    done = 1;
    PASS("WIN32", "RegisterClassExW", "ok");
    return 1;
}

static int create_diag_window(struct window_ctx *ctx, int visible, int width, int height)
{
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, width, height};
    ZeroMemory(ctx, sizeof(*ctx));
    ctx->width = width;
    ctx->height = height;
    if (!register_diag_class()) return 0;
    AdjustWindowRect(&r, style, FALSE);
    ctx->hwnd = CreateWindowExW(0, L"WineHuaGraphicsDiagWindow", L"WineHua Graphics Diagnostic",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, GetModuleHandleW(NULL), ctx);
    if (!ctx->hwnd) {
        FAIL("WIN32", "CreateWindowExW", "GetLastError=%lu", GetLastError());
        return 0;
    }
    if (visible) ShowWindow(ctx->hwnd, SW_SHOW);
    ctx->hdc = GetDC(ctx->hwnd);
    if (!ctx->hdc) {
        FAIL("WIN32", "GetDC", "GetLastError=%lu", GetLastError());
        return 0;
    }
    PASS("WIN32", "CreateWindow/GetDC", "hwnd=%p hdc=%p visible=%d", ctx->hwnd, ctx->hdc, visible);
    return 1;
}

static int set_basic_pixel_format(struct window_ctx *ctx)
{
    PIXELFORMATDESCRIPTOR pfd;
    PIXELFORMATDESCRIPTOR actual;
    int fmt;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    fmt = ChoosePixelFormat(ctx->hdc, &pfd);
    if (!fmt) {
        FAIL("WGL", "ChoosePixelFormat", "GetLastError=%lu", GetLastError());
        return 0;
    }
    PASS("WGL", "ChoosePixelFormat", "format=%d", fmt);
    if (!SetPixelFormat(ctx->hdc, fmt, &pfd)) {
        FAIL("WGL", "SetPixelFormat", "format=%d GetLastError=%lu", fmt, GetLastError());
        return 0;
    }
    ZeroMemory(&actual, sizeof(actual));
    DescribePixelFormat(ctx->hdc, fmt, sizeof(actual), &actual);
    PASS("WGL", "SetPixelFormat", "actual color=%d alpha=%d depth=%d stencil=%d flags=0x%08lX",
         actual.cColorBits, actual.cAlphaBits, actual.cDepthBits, actual.cStencilBits, actual.dwFlags);
    if (!(actual.dwFlags & PFD_GENERIC_FORMAT)) PASS("WGL", "PixelFormat acceleration", "not PFD_GENERIC_FORMAT");
    else WARN("WGL", "PixelFormat acceleration", "PFD_GENERIC_FORMAT set; may be software or wrapper fallback");
    return 1;
}

static int create_legacy_context(struct window_ctx *ctx)
{
    ctx->rc = wglCreateContext(ctx->hdc);
    if (!ctx->rc) {
        FAIL("WGL", "wglCreateContext", "GetLastError=%lu", GetLastError());
        return 0;
    }
    PASS("WGL", "wglCreateContext", "rc=%p", ctx->rc);
    if (!wglMakeCurrent(ctx->hdc, ctx->rc)) {
        FAIL("WGL", "wglMakeCurrent", "GetLastError=%lu", GetLastError());
        return 0;
    }
    PASS("WGL", "wglMakeCurrent", "legacy context current");
    return 1;
}

static void destroy_window_ctx(struct window_ctx *ctx)
{
    if (ctx->rc) {
        if (wglGetCurrentContext() == ctx->rc) wglMakeCurrent(NULL, NULL);
        wglDeleteContext(ctx->rc);
        ctx->rc = NULL;
    }
    if (ctx->hdc && ctx->hwnd) {
        ReleaseDC(ctx->hwnd, ctx->hdc);
        ctx->hdc = NULL;
    }
    if (ctx->hwnd) {
        DestroyWindow(ctx->hwnd);
        ctx->hwnd = NULL;
    }
}

static void load_wgl_extensions(void)
{
    g_diag.wglGetExtensionsStringARB_p = (PFNWGLGETEXTENSIONSSTRINGARBPROC_DIAG)get_gl_proc("wglGetExtensionsStringARB");
    g_diag.wglGetExtensionsStringEXT_p = (PFNWGLGETEXTENSIONSSTRINGEXTPROC_DIAG)get_gl_proc("wglGetExtensionsStringEXT");
    g_diag.wglChoosePixelFormatARB_p = (PFNWGLCHOOSEPIXELFORMATARBPROC_DIAG)get_gl_proc("wglChoosePixelFormatARB");
    g_diag.wglCreateContextAttribsARB_p = (PFNWGLCREATECONTEXTATTRIBSARBPROC_DIAG)get_gl_proc("wglCreateContextAttribsARB");
    g_diag.wglSwapIntervalEXT_p = (PFNWGLSWAPINTERVALEXTPROC_DIAG)get_gl_proc("wglSwapIntervalEXT");
    g_diag.wglGetSwapIntervalEXT_p = (PFNWGLGETSWAPINTERVALEXTPROC_DIAG)get_gl_proc("wglGetSwapIntervalEXT");

    if (g_diag.wglGetExtensionsStringARB_p || g_diag.wglGetExtensionsStringEXT_p) PASS("WGL", "WGL extension string", "available");
    else WARN("WGL", "WGL extension string", "not available");

    if (g_diag.wglChoosePixelFormatARB_p) PASS("WGL", "wglChoosePixelFormatARB", "available");
    else WARN("WGL", "wglChoosePixelFormatARB", "missing; SDL/engines may fall back to legacy ChoosePixelFormat");

    if (g_diag.wglCreateContextAttribsARB_p) PASS("WGL", "wglCreateContextAttribsARB", "available");
    else WARN("WGL", "wglCreateContextAttribsARB", "missing; GL 3.x/4.x core profile creation may fail");

    if (g_diag.wglSwapIntervalEXT_p) PASS("WGL", "wglSwapIntervalEXT", "available");
    else WARN("WGL", "wglSwapIntervalEXT", "missing; vsync control unavailable");
}

static const char *query_wgl_extensions(HDC hdc)
{
    const char *ext = NULL;
    if (g_diag.wglGetExtensionsStringARB_p) ext = g_diag.wglGetExtensionsStringARB_p(hdc);
    if (!ext && g_diag.wglGetExtensionsStringEXT_p) ext = g_diag.wglGetExtensionsStringEXT_p();
    return ext;
}

static void print_gl_info(HDC hdc)
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *wgl_ext;
    GLint max_tex = 0, max_units = 0, max_samples = 0;

    if (vendor && renderer && version) PASS("GL", "glGetString", "vendor='%s' renderer='%s' version='%s'", vendor, renderer, version);
    else FAIL("GL", "glGetString", "vendor=%p renderer=%p version=%p", vendor, renderer, version);
    if (glsl) PASS("GL", "GLSL version", "%s", glsl);
    else WARN("GL", "GLSL version", "not reported; shader path may be limited");

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &max_units);
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    PASS("GL", "Capability summary", "max_texture=%d fixed_texture_units=%d max_samples=%d", max_tex, max_units, max_samples);

    wgl_ext = query_wgl_extensions(hdc);
    if (wgl_ext) {
        PASS("WGL", "Extension probes", "ARB_pixel_format=%d ARB_create_context=%d EXT_swap_control=%d ARB_multisample=%d",
             contains_token(wgl_ext, "WGL_ARB_pixel_format"),
             contains_token(wgl_ext, "WGL_ARB_create_context"),
             contains_token(wgl_ext, "WGL_EXT_swap_control"),
             contains_token(wgl_ext, "WGL_ARB_multisample"));
        log_printf("[INFO] WGL_EXTENSIONS: %.1800s%s\n", wgl_ext, strlen(wgl_ext) > 1800 ? "..." : "");
    }

    if (extensions) {
        PASS("GL", "Extension probes", "S3TC=%d texture_sRGB=%d FBO_EXT=%d VAO=%d",
             contains_token(extensions, "GL_EXT_texture_compression_s3tc") || contains_token(extensions, "GL_S3_s3tc"),
             contains_token(extensions, "GL_EXT_texture_sRGB"),
             contains_token(extensions, "GL_EXT_framebuffer_object") || contains_token(extensions, "GL_ARB_framebuffer_object"),
             contains_token(extensions, "GL_ARB_vertex_array_object"));
        log_printf("[INFO] GL_EXTENSIONS: %.1800s%s\n", extensions, strlen(extensions) > 1800 ? "..." : "");
    }
}

static void load_gl_modern_functions(void)
{
#define LOAD_GL(field, type) do { g_diag.field##_p = (type)get_gl_proc(#field); } while (0)
    LOAD_GL(glGenBuffers, PFNGLGENBUFFERSPROC_DIAG);
    LOAD_GL(glBindBuffer, PFNGLBINDBUFFERPROC_DIAG);
    LOAD_GL(glBufferData, PFNGLBUFFERDATAPROC_DIAG);
    LOAD_GL(glDeleteBuffers, PFNGLDELETEBUFFERSPROC_DIAG);
    LOAD_GL(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC_DIAG);
    LOAD_GL(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC_DIAG);
    LOAD_GL(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC_DIAG);
    LOAD_GL(glCreateShader, PFNGLCREATESHADERPROC_DIAG);
    LOAD_GL(glShaderSource, PFNGLSHADERSOURCEPROC_DIAG);
    LOAD_GL(glCompileShader, PFNGLCOMPILESHADERPROC_DIAG);
    LOAD_GL(glGetShaderiv, PFNGLGETSHADERIVPROC_DIAG);
    LOAD_GL(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC_DIAG);
    LOAD_GL(glDeleteShader, PFNGLDELETESHADERPROC_DIAG);
    LOAD_GL(glCreateProgram, PFNGLCREATEPROGRAMPROC_DIAG);
    LOAD_GL(glAttachShader, PFNGLATTACHSHADERPROC_DIAG);
    LOAD_GL(glLinkProgram, PFNGLLINKPROGRAMPROC_DIAG);
    LOAD_GL(glGetProgramiv, PFNGLGETPROGRAMIVPROC_DIAG);
    LOAD_GL(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC_DIAG);
    LOAD_GL(glUseProgram, PFNGLUSEPROGRAMPROC_DIAG);
    LOAD_GL(glDeleteProgram, PFNGLDELETEPROGRAMPROC_DIAG);
    LOAD_GL(glGetAttribLocation, PFNGLGETATTRIBLOCATIONPROC_DIAG);
    LOAD_GL(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC_DIAG);
    LOAD_GL(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC_DIAG);
    LOAD_GL(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC_DIAG);
    LOAD_GL(glActiveTexture, PFNGLACTIVETEXTUREPROC_DIAG);
    LOAD_GL(glCompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC_DIAG);
    LOAD_GL(glGenerateMipmap, PFNGLGENERATEMIPMAPPROC_DIAG);
    LOAD_GL(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC_DIAG);
    LOAD_GL(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC_DIAG);
    LOAD_GL(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC_DIAG);
    LOAD_GL(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC_DIAG);
    LOAD_GL(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC_DIAG);
    LOAD_GL(glGenRenderbuffers, PFNGLGENRENDERBUFFERSPROC_DIAG);
    LOAD_GL(glBindRenderbuffer, PFNGLBINDRENDERBUFFERPROC_DIAG);
    LOAD_GL(glDeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC_DIAG);
    LOAD_GL(glRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC_DIAG);
    LOAD_GL(glRenderbufferStorageMultisample, PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC_DIAG);
    LOAD_GL(glFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC_DIAG);
    LOAD_GL(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC_DIAG);
#undef LOAD_GL

    if (g_diag.glGenBuffers_p && g_diag.glBindBuffer_p && g_diag.glBufferData_p)
        PASS("GLMODERN", "VBO functions", "available");
    else
        WARN("GLMODERN", "VBO functions", "missing/incomplete");

    if (g_diag.glCreateShader_p && g_diag.glCreateProgram_p && g_diag.glShaderSource_p)
        PASS("GLMODERN", "Shader functions", "available");
    else
        WARN("GLMODERN", "Shader functions", "missing/incomplete");

    if (g_diag.glGenFramebuffers_p && g_diag.glCheckFramebufferStatus_p)
        PASS("GLFBO", "FBO functions", "available");
    else
        WARN("GLFBO", "FBO functions", "missing/incomplete");
}

static int try_create_attrib_context(struct window_ctx *share_ctx, int major, int minor, int core)
{
    struct window_ctx ctx;
    int ok = 0;
    if (!g_diag.wglCreateContextAttribsARB_p) return 0;
    if (!create_diag_window(&ctx, 0, 320, 240)) return 0;

    if (g_diag.wglChoosePixelFormatARB_p) {
        int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB, TRUE,
            WGL_SUPPORT_OPENGL_ARB, TRUE,
            WGL_DOUBLE_BUFFER_ARB, TRUE,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB, 32,
            WGL_ALPHA_BITS_ARB, 8,
            WGL_DEPTH_BITS_ARB, 24,
            WGL_STENCIL_BITS_ARB, 8,
            0
        };
        int pf = 0;
        UINT count = 0;
        PIXELFORMATDESCRIPTOR pfd;
        if (g_diag.wglChoosePixelFormatARB_p(ctx.hdc, attribs, NULL, 1, &pf, &count) && count > 0) {
            ZeroMemory(&pfd, sizeof(pfd));
            DescribePixelFormat(ctx.hdc, pf, sizeof(pfd), &pfd);
            if (!SetPixelFormat(ctx.hdc, pf, &pfd)) {
                WARN("WGL", "ARB pixel format set", "failed GetLastError=%lu", GetLastError());
                destroy_window_ctx(&ctx);
                return 0;
            }
            PASS("WGL", "ARB pixel format", "format=%d", pf);
        } else {
            WARN("WGL", "ARB pixel format", "choose failed, fallback to legacy pixel format");
            if (!set_basic_pixel_format(&ctx)) {
                destroy_window_ctx(&ctx);
                return 0;
            }
        }
    } else if (!set_basic_pixel_format(&ctx)) {
        destroy_window_ctx(&ctx);
        return 0;
    }

    {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, major,
            WGL_CONTEXT_MINOR_VERSION_ARB, minor,
            WGL_CONTEXT_PROFILE_MASK_ARB, core ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
            0
        };
        ctx.rc = g_diag.wglCreateContextAttribsARB_p(ctx.hdc, share_ctx ? share_ctx->rc : NULL, attribs);
    }
    if (!ctx.rc) {
        WARN("WGL", "wglCreateContextAttribsARB", "OpenGL %d.%d %s failed GetLastError=%lu", major, minor, core ? "core" : "compat", GetLastError());
        destroy_window_ctx(&ctx);
        return 0;
    }
    if (!wglMakeCurrent(ctx.hdc, ctx.rc)) {
        WARN("WGL", "attrib context make current", "failed GetLastError=%lu", GetLastError());
        destroy_window_ctx(&ctx);
        return 0;
    }
    PASS("WGL", "wglCreateContextAttribsARB", "OpenGL %d.%d %s context ok, GL_VERSION=%s", major, minor, core ? "core" : "compat", glGetString(GL_VERSION));
    ok = 1;
    wglMakeCurrent(NULL, NULL);
    destroy_window_ctx(&ctx);
    return ok;
}

static void test_wgl_contexts(struct window_ctx *base)
{
    HGLRC share_rc;

    share_rc = wglCreateContext(base->hdc);
    if (share_rc) {
        if (wglShareLists(base->rc, share_rc)) PASS("WGL", "wglShareLists", "legacy context sharing ok");
        else WARN("WGL", "wglShareLists", "failed GetLastError=%lu", GetLastError());
        wglDeleteContext(share_rc);
    } else {
        WARN("WGL", "secondary legacy context", "wglCreateContext failed GetLastError=%lu", GetLastError());
    }

    if (g_diag.wglSwapIntervalEXT_p) {
        if (g_diag.wglSwapIntervalEXT_p(0)) PASS("WGL", "SwapInterval 0", "ok");
        else WARN("WGL", "SwapInterval 0", "failed");
        if (g_diag.wglSwapIntervalEXT_p(1)) PASS("WGL", "SwapInterval 1", "ok");
        else WARN("WGL", "SwapInterval 1", "failed");
        if (g_diag.wglGetSwapIntervalEXT_p) PASS("WGL", "GetSwapInterval", "interval=%d", g_diag.wglGetSwapIntervalEXT_p());
    }

    try_create_attrib_context(base, 3, 3, 1);
    try_create_attrib_context(base, 3, 0, 0);
}

static void test_textures(void)
{
    GLuint tex = 0;
    unsigned char data[4 * 4 * 4];
    int i;
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);

    for (i = 0; i < (int)sizeof(data); ++i) data[i] = (unsigned char)(i * 3);

    gl_clear_error();
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    gl_expect_ok("GLTEX", "RGBA8 texture");

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
    gl_expect_ok("GLTEX", "BGRA upload");

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 257, 129, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl_expect_ok("GLTEX", "NPOT texture 257x129");

    if (contains_token(extensions, "GL_EXT_texture_sRGB") || contains_token(extensions, "GL_ARB_framebuffer_sRGB")) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        gl_expect_ok("GLTEX", "sRGB texture");
    } else {
        WARN("GLTEX", "sRGB texture", "extension not advertised");
    }

    if (g_diag.glGenerateMipmap_p) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_diag.glGenerateMipmap_p(GL_TEXTURE_2D);
        gl_expect_ok("GLTEX", "GenerateMipmap");
    } else {
        WARN("GLTEX", "GenerateMipmap", "function missing");
    }

    if (g_diag.glCompressedTexImage2D_p && (contains_token(extensions, "GL_EXT_texture_compression_s3tc") || contains_token(extensions, "GL_S3_s3tc"))) {
        unsigned char dxt1[8] = {0};
        unsigned char dxt5[16] = {0};
        g_diag.glCompressedTexImage2D_p(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4, 0, sizeof(dxt1), dxt1);
        gl_expect_ok("GLTEX", "DXT1/BC1 compressed texture");
        g_diag.glCompressedTexImage2D_p(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 4, 4, 0, sizeof(dxt5), dxt5);
        gl_expect_ok("GLTEX", "DXT5/BC3 compressed texture");
    } else {
        WARN("GLTEX", "S3TC/DXT texture", "extension or function missing; many PC games may need this");
    }

    glDeleteTextures(1, &tex);
}

static GLuint compile_shader(GLenum type, const char *src, const char *name)
{
    GLuint shader;
    GLint ok = 0;
    char logbuf[1024];
    GLsizei len = 0;
    if (!g_diag.glCreateShader_p) return 0;
    shader = g_diag.glCreateShader_p(type);
    g_diag.glShaderSource_p(shader, 1, (const GLchar_diag *const *)&src, NULL);
    g_diag.glCompileShader_p(shader);
    g_diag.glGetShaderiv_p(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        if (g_diag.glGetShaderInfoLog_p) g_diag.glGetShaderInfoLog_p(shader, sizeof(logbuf), &len, logbuf);
        FAIL("GLSHADER", name, "compile failed: %s", logbuf);
        g_diag.glDeleteShader_p(shader);
        return 0;
    }
    PASS("GLSHADER", name, "compile ok");
    return shader;
}

static void test_shader_vbo(void)
{
    const char *vs_src =
        "#version 110\n"
        "attribute vec2 a_pos;\n"
        "attribute vec3 a_col;\n"
        "varying vec3 v_col;\n"
        "void main(){ v_col=a_col; gl_Position=vec4(a_pos,0.0,1.0); }\n";
    const char *fs_src =
        "#version 110\n"
        "varying vec3 v_col;\n"
        "void main(){ gl_FragColor=vec4(v_col,1.0); }\n";
    float verts[] = {
        -0.7f, -0.6f, 1.0f, 0.0f, 0.0f,
         0.7f, -0.6f, 0.0f, 1.0f, 0.0f,
         0.0f,  0.7f, 0.0f, 0.0f, 1.0f
    };
    GLuint vs = 0, fs = 0, prog = 0, vbo = 0, vao = 0;
    GLint ok = 0;
    GLint a_pos, a_col;
    char logbuf[1024];
    GLsizei len = 0;

    if (!(g_diag.glGenBuffers_p && g_diag.glBindBuffer_p && g_diag.glBufferData_p &&
          g_diag.glCreateShader_p && g_diag.glCreateProgram_p && g_diag.glVertexAttribPointer_p)) {
        WARN("GLSHADER", "Shader/VBO draw", "required functions missing");
        return;
    }

    gl_clear_error();
    if (g_diag.glGenVertexArrays_p && g_diag.glBindVertexArray_p) {
        g_diag.glGenVertexArrays_p(1, &vao);
        g_diag.glBindVertexArray_p(vao);
    } else {
        WARN("GLSHADER", "VAO", "not available; compatibility profile may still work");
    }

    vs = compile_shader(GL_VERTEX_SHADER, vs_src, "vertex shader");
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, "fragment shader");
    if (!vs || !fs) goto cleanup;

    prog = g_diag.glCreateProgram_p();
    g_diag.glAttachShader_p(prog, vs);
    g_diag.glAttachShader_p(prog, fs);
    g_diag.glLinkProgram_p(prog);
    g_diag.glGetProgramiv_p(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        logbuf[0] = 0;
        if (g_diag.glGetProgramInfoLog_p) g_diag.glGetProgramInfoLog_p(prog, sizeof(logbuf), &len, logbuf);
        FAIL("GLSHADER", "program link", "%s", logbuf);
        goto cleanup;
    }
    PASS("GLSHADER", "program link", "ok");

    g_diag.glGenBuffers_p(1, &vbo);
    g_diag.glBindBuffer_p(GL_ARRAY_BUFFER, vbo);
    g_diag.glBufferData_p(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    a_pos = g_diag.glGetAttribLocation_p(prog, "a_pos");
    a_col = g_diag.glGetAttribLocation_p(prog, "a_col");
    if (a_pos < 0 || a_col < 0) {
        FAIL("GLSHADER", "attribute locations", "a_pos=%d a_col=%d", a_pos, a_col);
        goto cleanup;
    }
    g_diag.glUseProgram_p(prog);
    g_diag.glEnableVertexAttribArray_p((GLuint)a_pos);
    g_diag.glEnableVertexAttribArray_p((GLuint)a_col);
    g_diag.glVertexAttribPointer_p((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    g_diag.glVertexAttribPointer_p((GLuint)a_col, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(2 * sizeof(float)));
    glViewport(0, 0, 320, 240);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    gl_expect_ok("GLSHADER", "VBO shader draw");

cleanup:
    if (a_pos >= 0 && g_diag.glDisableVertexAttribArray_p) g_diag.glDisableVertexAttribArray_p((GLuint)a_pos);
    if (a_col >= 0 && g_diag.glDisableVertexAttribArray_p) g_diag.glDisableVertexAttribArray_p((GLuint)a_col);
    if (g_diag.glUseProgram_p) g_diag.glUseProgram_p(0);
    if (vbo && g_diag.glDeleteBuffers_p) g_diag.glDeleteBuffers_p(1, &vbo);
    if (prog && g_diag.glDeleteProgram_p) g_diag.glDeleteProgram_p(prog);
    if (vs && g_diag.glDeleteShader_p) g_diag.glDeleteShader_p(vs);
    if (fs && g_diag.glDeleteShader_p) g_diag.glDeleteShader_p(fs);
    if (vao && g_diag.glDeleteVertexArrays_p) g_diag.glDeleteVertexArrays_p(1, &vao);
}

static void test_fbo(void)
{
    GLuint fbo = 0, color = 0, depth = 0;
    GLuint msaa_fbo = 0, msaa_color = 0, msaa_depth = 0;
    GLenum status;
    unsigned char pixel[4] = {0, 0, 0, 0};

    if (!(g_diag.glGenFramebuffers_p && g_diag.glBindFramebuffer_p && g_diag.glFramebufferTexture2D_p &&
          g_diag.glCheckFramebufferStatus_p && g_diag.glGenRenderbuffers_p && g_diag.glRenderbufferStorage_p &&
          g_diag.glFramebufferRenderbuffer_p)) {
        WARN("GLFBO", "FBO basic", "required functions missing");
        return;
    }

    gl_clear_error();
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    g_diag.glGenRenderbuffers_p(1, &depth);
    g_diag.glBindRenderbuffer_p(GL_RENDERBUFFER, depth);
    g_diag.glRenderbufferStorage_p(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 256, 256);

    g_diag.glGenFramebuffers_p(1, &fbo);
    g_diag.glBindFramebuffer_p(GL_FRAMEBUFFER, fbo);
    g_diag.glFramebufferTexture2D_p(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    g_diag.glFramebufferRenderbuffer_p(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    status = g_diag.glCheckFramebufferStatus_p(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) PASS("GLFBO", "RGBA8 + D24S8 FBO", "complete");
    else FAIL("GLFBO", "RGBA8 + D24S8 FBO", "status=0x%04X", (unsigned)status);

    glViewport(0, 0, 256, 256);
    glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (glGetError() == GL_NO_ERROR && pixel[3] > 0) PASS("GLFBO", "FBO clear/readback", "pixel=%u,%u,%u,%u", pixel[0], pixel[1], pixel[2], pixel[3]);
    else WARN("GLFBO", "FBO clear/readback", "gl error or alpha=0 pixel=%u,%u,%u,%u", pixel[0], pixel[1], pixel[2], pixel[3]);

    if (g_diag.glRenderbufferStorageMultisample_p) {
        g_diag.glGenRenderbuffers_p(1, &msaa_color);
        g_diag.glBindRenderbuffer_p(GL_RENDERBUFFER, msaa_color);
        g_diag.glRenderbufferStorageMultisample_p(GL_RENDERBUFFER, 4, GL_RGBA8, 256, 256);
        g_diag.glGenRenderbuffers_p(1, &msaa_depth);
        g_diag.glBindRenderbuffer_p(GL_RENDERBUFFER, msaa_depth);
        g_diag.glRenderbufferStorageMultisample_p(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, 256, 256);
        g_diag.glGenFramebuffers_p(1, &msaa_fbo);
        g_diag.glBindFramebuffer_p(GL_FRAMEBUFFER, msaa_fbo);
        g_diag.glFramebufferRenderbuffer_p(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_color);
        g_diag.glFramebufferRenderbuffer_p(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msaa_depth);
        status = g_diag.glCheckFramebufferStatus_p(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE) PASS("GLFBO", "MSAA 4x FBO", "complete");
        else WARN("GLFBO", "MSAA 4x FBO", "status=0x%04X", (unsigned)status);

        if (g_diag.glBlitFramebuffer_p && status == GL_FRAMEBUFFER_COMPLETE) {
            g_diag.glBindFramebuffer_p(GL_READ_FRAMEBUFFER, msaa_fbo);
            g_diag.glBindFramebuffer_p(GL_DRAW_FRAMEBUFFER, fbo);
            g_diag.glBlitFramebuffer_p(0, 0, 256, 256, 0, 0, 256, 256, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            gl_expect_ok("GLFBO", "MSAA blit/resolve");
        } else {
            WARN("GLFBO", "MSAA blit/resolve", "glBlitFramebuffer missing or MSAA incomplete");
        }
    } else {
        WARN("GLFBO", "MSAA FBO", "glRenderbufferStorageMultisample missing");
    }

    if (g_diag.glBindFramebuffer_p) g_diag.glBindFramebuffer_p(GL_FRAMEBUFFER, 0);
    if (msaa_fbo && g_diag.glDeleteFramebuffers_p) g_diag.glDeleteFramebuffers_p(1, &msaa_fbo);
    if (msaa_color && g_diag.glDeleteRenderbuffers_p) g_diag.glDeleteRenderbuffers_p(1, &msaa_color);
    if (msaa_depth && g_diag.glDeleteRenderbuffers_p) g_diag.glDeleteRenderbuffers_p(1, &msaa_depth);
    if (fbo && g_diag.glDeleteFramebuffers_p) g_diag.glDeleteFramebuffers_p(1, &fbo);
    if (depth && g_diag.glDeleteRenderbuffers_p) g_diag.glDeleteRenderbuffers_p(1, &depth);
    if (color) glDeleteTextures(1, &color);
}

static void draw_compat_scene(float angle)
{
    float a = angle * (float)(M_PI / 180.0);
    glViewport(0, 0, 640, 360);
    glClearColor(0.08f, 0.10f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.6, 1.6, -0.9, 0.9, 1.0, 20.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.0f);
    glRotatef(angle, 0.3f, 1.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(0, 1, 0);
    glColor3f(0, 1, 0); glVertex3f((float)cos(a), -1, (float)sin(a));
    glColor3f(0, 0, 1); glVertex3f(-1, -1, 0);
    glEnd();
}

static void test_present_loop(struct window_ctx *ctx)
{
    LARGE_INTEGER freq, start, now, last;
    int frames = 0;
    int seconds = g_diag.opt.seconds > 0 ? g_diag.opt.seconds : 3;
    MSG msg;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;

    while (1) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        if (((now.QuadPart - start.QuadPart) / (double)freq.QuadPart) >= seconds) break;

        if (frames == 30) SetWindowPos(ctx->hwnd, NULL, 0, 0, 800, 450, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (frames == 90) SetWindowPos(ctx->hwnd, NULL, 0, 0, 640, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        draw_compat_scene((float)(frames * 2.5f));
        if (!SwapBuffers(ctx->hdc)) {
            FAIL("PRESENT", "SwapBuffers", "failed at frame=%d GetLastError=%lu", frames, GetLastError());
            return;
        }
        frames++;
        Sleep(1);
    }
    QueryPerformanceCounter(&now);
    PASS("PRESENT", "SwapBuffers loop", "frames=%d seconds=%.2f fps=%.2f final_size=%dx%d",
         frames, (now.QuadPart - start.QuadPart) / (double)freq.QuadPart,
         frames / ((now.QuadPart - start.QuadPart) / (double)freq.QuadPart), ctx->width, ctx->height);
}

static void test_stress(void)
{
    int i, loops;
    GLuint tex = 0, buf = 0;
    unsigned char *data;

    if (!g_diag.opt.stress) return;
    loops = g_diag.opt.seconds > 0 ? g_diag.opt.seconds * 120 : 1200;
    data = (unsigned char *)malloc(256 * 256 * 4);
    if (!data) {
        WARN("STRESS", "allocation", "failed");
        return;
    }
    memset(data, 0x7F, 256 * 256 * 4);

    gl_clear_error();
    for (i = 0; i < loops; ++i) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 32, 32, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glDeleteTextures(1, &tex);
        tex = 0;

        if (g_diag.glGenBuffers_p && g_diag.glBufferData_p) {
            g_diag.glGenBuffers_p(1, &buf);
            g_diag.glBindBuffer_p(GL_ARRAY_BUFFER, buf);
            g_diag.glBufferData_p(GL_ARRAY_BUFFER, 64 * 1024, data, GL_DYNAMIC_DRAW);
            g_diag.glDeleteBuffers_p(1, &buf);
            buf = 0;
        }
        if ((i % 100) == 0 && glGetError() != GL_NO_ERROR) {
            FAIL("STRESS", "resource churn", "GL error at loop=%d", i);
            free(data);
            return;
        }
    }
    gl_expect_ok("STRESS", "resource churn");
    free(data);
}

#if WH_HAS_D3D9
typedef IDirect3D9 *(WINAPI *PFNDirect3DCreate9_DIAG)(UINT SDKVersion);
static void test_d3d9(HWND hwnd)
{
    HMODULE mod = LoadLibraryA("d3d9.dll");
    PFNDirect3DCreate9_DIAG Direct3DCreate9_p;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DTexture9 *tex = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DCAPS9 caps;
    HRESULT hr;

    if (!mod) {
        WARN("D3D9", "d3d9.dll", "not available");
        return;
    }
    Direct3DCreate9_p = (PFNDirect3DCreate9_DIAG)GetProcAddress(mod, "Direct3DCreate9");
    if (!Direct3DCreate9_p) {
        WARN("D3D9", "Direct3DCreate9", "missing");
        FreeLibrary(mod);
        return;
    }
    d3d = Direct3DCreate9_p(D3D_SDK_VERSION);
    if (!d3d) {
        FAIL("D3D9", "Direct3DCreate9", "returned NULL");
        FreeLibrary(mod);
        return;
    }
    PASS("D3D9", "Direct3DCreate9", "ok");

    hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    if (SUCCEEDED(hr)) PASS("D3D9", "GetDeviceCaps", "PixelShader=%u.%u VertexShader=%u.%u MaxTex=%u",
                            D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
                            D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
                            caps.MaxTextureWidth);
    else WARN("D3D9", "GetDeviceCaps", "hr=0x%08lX", (unsigned long)hr);

    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                 &pp, &dev);
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                     &pp, &dev);
    }
    if (SUCCEEDED(hr)) PASS("D3D9", "CreateDevice", "ok");
    else {
        FAIL("D3D9", "CreateDevice", "hr=0x%08lX", (unsigned long)hr);
        IDirect3D9_Release(d3d);
        FreeLibrary(mod);
        return;
    }

    hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF204060, 1.0f, 0);
    if (SUCCEEDED(hr)) PASS("D3D9", "Clear target/depth", "ok"); else WARN("D3D9", "Clear target/depth", "hr=0x%08lX", (unsigned long)hr);
    hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (SUCCEEDED(hr)) PASS("D3D9", "Present", "ok"); else WARN("D3D9", "Present", "hr=0x%08lX", (unsigned long)hr);

    hr = IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (SUCCEEDED(hr)) {
        PASS("D3D9", "A8R8G8B8 texture", "ok");
        IDirect3DTexture9_Release(tex);
        tex = NULL;
    } else WARN("D3D9", "A8R8G8B8 texture", "hr=0x%08lX", (unsigned long)hr);

    hr = IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, (D3DFORMAT)MAKEFOURCC('D','X','T','1'), D3DPOOL_DEFAULT, &tex, NULL);
    if (SUCCEEDED(hr)) {
        PASS("D3D9", "DXT1 texture", "ok");
        IDirect3DTexture9_Release(tex);
        tex = NULL;
    } else WARN("D3D9", "DXT1 texture", "hr=0x%08lX", (unsigned long)hr);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    FreeLibrary(mod);
}
#else
static void test_d3d9(HWND hwnd)
{
    (void)hwnd;
    WARN("D3D9", "compile option", "D3D9 headers not available or WH_ENABLE_D3D=0; skipped");
}
#endif

#if WH_HAS_D3D11
typedef HRESULT (WINAPI *PFND3D11CreateDeviceAndSwapChain_DIAG)(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext);
static const GUID WH_IID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static void test_d3d11(HWND hwnd)
{
    HMODULE mod = LoadLibraryA("d3d11.dll");
    PFND3D11CreateDeviceAndSwapChain_DIAG create_p;
    IDXGISwapChain *swap = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr;
    float clear[4] = {0.1f, 0.2f, 0.4f, 1.0f};

    if (!mod) {
        WARN("D3D11", "d3d11.dll", "not available");
        return;
    }
    create_p = (PFND3D11CreateDeviceAndSwapChain_DIAG)GetProcAddress(mod, "D3D11CreateDeviceAndSwapChain");
    if (!create_p) {
        WARN("D3D11", "D3D11CreateDeviceAndSwapChain", "missing");
        FreeLibrary(mod);
        return;
    }

    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 640;
    scd.BufferDesc.Height = 360;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = create_p(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3, D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
    if (FAILED(hr)) {
        hr = create_p(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 3, D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
    }
    if (FAILED(hr)) {
        FAIL("D3D11", "CreateDeviceAndSwapChain", "hr=0x%08lX", (unsigned long)hr);
        FreeLibrary(mod);
        return;
    }
    PASS("D3D11", "CreateDeviceAndSwapChain", "feature_level=0x%X", got);

    hr = IDXGISwapChain_GetBuffer(swap, 0, &WH_IID_ID3D11Texture2D, (void **)&back);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    }
    if (SUCCEEDED(hr)) {
        PASS("D3D11", "Backbuffer RTV", "ok");
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        hr = IDXGISwapChain_Present(swap, 0, 0);
        if (SUCCEEDED(hr)) PASS("D3D11", "Present", "ok"); else WARN("D3D11", "Present", "hr=0x%08lX", (unsigned long)hr);
    } else WARN("D3D11", "Backbuffer RTV", "hr=0x%08lX", (unsigned long)hr);

    {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = 64;
        td.Height = 64;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
        if (SUCCEEDED(hr)) {
            PASS("D3D11", "RGBA8 Texture2D", "ok");
            ID3D11Texture2D_Release(tex);
            tex = NULL;
        } else WARN("D3D11", "RGBA8 Texture2D", "hr=0x%08lX", (unsigned long)hr);

        td.Width = 4;
        td.Height = 4;
        td.Format = DXGI_FORMAT_BC1_UNORM;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        {
            unsigned char bc1[8] = {0};
            D3D11_SUBRESOURCE_DATA init;
            ZeroMemory(&init, sizeof(init));
            init.pSysMem = bc1;
            init.SysMemPitch = 8;
            hr = ID3D11Device_CreateTexture2D(dev, &td, &init, &tex);
        }
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
            if (SUCCEEDED(hr)) {
                PASS("D3D11", "BC1 SRV texture", "ok");
                ID3D11ShaderResourceView_Release(srv);
                srv = NULL;
            } else WARN("D3D11", "BC1 SRV texture", "SRV hr=0x%08lX", (unsigned long)hr);
            ID3D11Texture2D_Release(tex);
            tex = NULL;
        } else WARN("D3D11", "BC1 Texture2D", "hr=0x%08lX", (unsigned long)hr);
    }

    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (back) ID3D11Texture2D_Release(back);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    if (swap) IDXGISwapChain_Release(swap);
    FreeLibrary(mod);
}
#else
static void test_d3d11(HWND hwnd)
{
    (void)hwnd;
    WARN("D3D11", "compile option", "D3D11/DXGI headers not available or WH_ENABLE_D3D=0; skipped");
}
#endif

static void parse_args(int argc, char **argv)
{
    int i;
    g_diag.opt.seconds = 3;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--visible")) g_diag.opt.visible = 1;
        else if (!strcmp(argv[i], "--quick")) g_diag.opt.quick = 1;
        else if (!strcmp(argv[i], "--stress")) g_diag.opt.stress = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_diag.opt.seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) g_diag.opt.log_path = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("WineHua Graphics Diagnostic\n");
            printf("Options:\n");
            printf("  --visible         show diagnostic window\n");
            printf("  --quick           skip D3D and stress tests\n");
            printf("  --stress          run resource churn stress test\n");
            printf("  --seconds N       present/stress duration hint, default 3\n");
            printf("  --log PATH        write log file\n");
            exit(0);
        }
    }
    if (g_diag.opt.seconds <= 0) g_diag.opt.seconds = 3;
}

int main(int argc, char **argv)
{
    struct window_ctx win;
    int wine;

    ZeroMemory(&g_diag, sizeof(g_diag));
    parse_args(argc, argv);
    if (g_diag.opt.log_path) {
        g_diag.log = fopen(g_diag.opt.log_path, "wb");
        if (!g_diag.log) printf("[WARN] Cannot open log file: %s\n", g_diag.opt.log_path);
    }

    log_printf("WineHua Graphics Diagnostic starting\n");
    log_printf("Build: WH_HAS_D3D9=%d WH_HAS_D3D11=%d\n", WH_HAS_D3D9, WH_HAS_D3D11);
    wine = is_wine_runtime();
    if (wine) PASS("ENV", "Wine runtime", "detected");
    else WARN("ENV", "Wine runtime", "not detected; running as native Windows diagnostic");

    if (!create_diag_window(&win, g_diag.opt.visible, 640, 360)) goto done;
    if (!set_basic_pixel_format(&win)) goto done;
    if (!create_legacy_context(&win)) goto done;

    print_gl_info(win.hdc);
    load_wgl_extensions();
    test_wgl_contexts(&win);

    /* Make sure base context is current again after attribute context probing. */
    wglMakeCurrent(win.hdc, win.rc);
    load_gl_modern_functions();

    test_textures();
    test_shader_vbo();
    test_fbo();
    test_present_loop(&win);
    test_stress();

    if (!g_diag.opt.quick) {
        test_d3d9(win.hwnd);
        test_d3d11(win.hwnd);
    } else {
        WARN("D3D", "quick mode", "D3D9/D3D11 skipped");
    }

done:
    destroy_window_ctx(&win);
    log_printf("\nSUMMARY: PASS=%d WARN=%d FAIL=%d\n", g_diag.pass_count, g_diag.warn_count, g_diag.fail_count);
    if (g_diag.fail_count == 0) log_printf("RESULT: OK_WITH_WARNINGS_ALLOWED\n");
    else log_printf("RESULT: FAILED\n");
    if (g_diag.log) fclose(g_diag.log);
    return g_diag.fail_count ? 2 : 0;
}
