/*
 * WineHua Graphics Diagnostic GUI Frontend
 *
 * This file provides a clickable Win32 GUI wrapper around winehua_graphics_diag.c.
 * Keep winehua_graphics_diag.c in the same directory when building this file.
 *
 * Build examples:
 *   MinGW-w64:
 *     x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -DWH_ENABLE_D3D=1 \
 *       -mwindows -o winehua_graphics_diag_gui.exe winehua_graphics_diag_gui.c \
 *       -lopengl32 -lgdi32 -luser32 -lshell32 -ld3d9 -ld3d11 -ldxgi
 *
 *   MinGW-w64 without D3D libs:
 *     x86_64-w64-mingw32-gcc -O2 -Wall -DCOBJMACROS -DWH_ENABLE_D3D=0 \
 *       -mwindows -o winehua_graphics_diag_gui.exe winehua_graphics_diag_gui.c \
 *       -lopengl32 -lgdi32 -luser32 -lshell32
 *
 *   MSVC Developer Prompt:
 *     cl /O2 /W3 /DCOBJMACROS /DWH_ENABLE_D3D=1 /Fe:winehua_graphics_diag_gui.exe \
 *       winehua_graphics_diag_gui.c user32.lib gdi32.lib opengl32.lib shell32.lib d3d9.lib d3d11.lib dxgi.lib /link /SUBSYSTEM:WINDOWS
 *
 * Runtime:
 *   Double-click winehua_graphics_diag_gui.exe, then press Quick / Full / Stress.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reuse the CLI diagnostic implementation directly.
 * The CLI main() is renamed so this GUI can call it from a worker thread.
 */
#define main winehua_graphics_diag_cli_main
#include "winehua_graphics_diag.c"
#undef main

#define IDC_BTN_QUICK   1001
#define IDC_BTN_FULL    1002
#define IDC_BTN_STRESS  1003
#define IDC_BTN_CLEAR   1004
#define IDC_BTN_OPENLOG 1005
#define IDC_BTN_EXIT    1006
#define IDC_EDIT_LOG    2001
#define IDC_STATIC_SUM  2002

#define WM_DIAG_DONE    (WM_APP + 10)
#define DIAG_TIMER_ID   1

struct gui_app_state {
    HWND hwnd;
    HWND edit;
    HWND summary;
    HWND btn_quick;
    HWND btn_full;
    HWND btn_stress;
    HWND btn_clear;
    HWND btn_openlog;
    HWND btn_exit;
    HANDLE worker;
    DWORD worker_id;
    int running;
    int last_exit_code;
    char mode[32];
    char log_path[MAX_PATH];
};

static struct gui_app_state g_gui;

static void gui_set_buttons_enabled(int enabled)
{
    EnableWindow(g_gui.btn_quick, enabled);
    EnableWindow(g_gui.btn_full, enabled);
    EnableWindow(g_gui.btn_stress, enabled);
    EnableWindow(g_gui.btn_clear, enabled);
    EnableWindow(g_gui.btn_exit, enabled);
    EnableWindow(g_gui.btn_openlog, !g_gui.running);
}

static void gui_set_status(const char *text)
{
    if (g_gui.summary) SetWindowTextA(g_gui.summary, text ? text : "");
}

static char *normalize_newlines(const char *src, DWORD len)
{
    DWORD i;
    DWORD extra = 0;
    char *dst;
    char *p;
    if (!src) return NULL;
    for (i = 0; i < len; ++i) {
        if (src[i] == '\n' && (i == 0 || src[i - 1] != '\r')) extra++;
    }
    dst = (char *)malloc((size_t)len + extra + 1);
    if (!dst) return NULL;
    p = dst;
    for (i = 0; i < len; ++i) {
        if (src[i] == '\n' && (i == 0 || src[i - 1] != '\r')) *p++ = '\r';
        *p++ = src[i];
    }
    *p = 0;
    return dst;
}

static void gui_load_log_file(void)
{
    HANDLE h;
    DWORD size, read_bytes;
    char *buf;
    char *text;

    h = CreateFileA(g_gui.log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (g_gui.edit && !g_gui.running) SetWindowTextA(g_gui.edit, "No log file yet. Click Quick Test / Full Test / Stress Test.\r\n");
        return;
    }

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(h);
        return;
    }

    /* Avoid very large edit-control updates. Keep the tail when stress logs grow. */
    if (size > 512 * 1024) size = 512 * 1024;
    buf = (char *)malloc(size + 1);
    if (!buf) {
        CloseHandle(h);
        return;
    }
    SetFilePointer(h, -(LONG)size, NULL, FILE_END);
    if (!ReadFile(h, buf, size, &read_bytes, NULL)) read_bytes = 0;
    CloseHandle(h);
    buf[read_bytes] = 0;

    text = normalize_newlines(buf, read_bytes);
    free(buf);
    if (text) {
        SetWindowTextA(g_gui.edit, text);
        SendMessageA(g_gui.edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_gui.edit, EM_SCROLLCARET, 0, 0);
        free(text);
    }
}

static DWORD WINAPI gui_worker_thread(LPVOID param)
{
    const char *mode = (const char *)param;
    int ret;

    if (!strcmp(mode, "quick")) {
        char *argv[] = {
            "winehua_graphics_diag_gui.exe", "--visible", "--quick", "--seconds", "5", "--log", g_gui.log_path, NULL
        };
        ret = winehua_graphics_diag_cli_main(7, argv);
    } else if (!strcmp(mode, "stress")) {
        char *argv[] = {
            "winehua_graphics_diag_gui.exe", "--visible", "--stress", "--seconds", "30", "--log", g_gui.log_path, NULL
        };
        ret = winehua_graphics_diag_cli_main(7, argv);
    } else {
        char *argv[] = {
            "winehua_graphics_diag_gui.exe", "--visible", "--seconds", "8", "--log", g_gui.log_path, NULL
        };
        ret = winehua_graphics_diag_cli_main(6, argv);
    }

    g_gui.last_exit_code = ret;
    PostMessageA(g_gui.hwnd, WM_DIAG_DONE, (WPARAM)ret, 0);
    return (DWORD)ret;
}

static void gui_start_run(const char *mode)
{
    char status[256];

    if (g_gui.running) return;
    DeleteFileA(g_gui.log_path);
    SetWindowTextA(g_gui.edit, "Starting diagnostic...\r\nA small render window may appear during testing.\r\n");

    lstrcpynA(g_gui.mode, mode, sizeof(g_gui.mode));
    g_gui.running = 1;
    gui_set_buttons_enabled(0);
    wsprintfA(status, "Running %s test...", mode);
    gui_set_status(status);
    SetTimer(g_gui.hwnd, DIAG_TIMER_ID, 500, NULL);

    g_gui.worker = CreateThread(NULL, 0, gui_worker_thread, (LPVOID)g_gui.mode, 0, &g_gui.worker_id);
    if (!g_gui.worker) {
        g_gui.running = 0;
        KillTimer(g_gui.hwnd, DIAG_TIMER_ID);
        gui_set_buttons_enabled(1);
        gui_set_status("Failed to start diagnostic worker thread.");
    }
}

static void gui_on_done(int exit_code)
{
    char status[256];
    g_gui.running = 0;
    KillTimer(g_gui.hwnd, DIAG_TIMER_ID);
    if (g_gui.worker) {
        CloseHandle(g_gui.worker);
        g_gui.worker = NULL;
    }
    gui_load_log_file();
    wsprintfA(status, "Finished %s test. ExitCode=%d. See PASS/WARN/FAIL summary below.", g_gui.mode, exit_code);
    gui_set_status(status);
    gui_set_buttons_enabled(1);
}

static void gui_open_log(void)
{
    HANDLE h = CreateFileA(g_gui.log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        MessageBoxA(g_gui.hwnd, "No log file found. Run a test first.", "WineHua Graphics Diagnostic", MB_OK | MB_ICONINFORMATION);
        return;
    }
    CloseHandle(h);
    ShellExecuteA(g_gui.hwnd, "open", "notepad.exe", g_gui.log_path, NULL, SW_SHOWNORMAL);
}

static HWND gui_create_button(HWND parent, const char *text, int id)
{
    return CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           0, 0, 100, 28, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
}

static void gui_layout(HWND hwnd)
{
    RECT rc;
    int w, h;
    int margin = 10;
    int left_w = 130;
    int btn_h = 32;
    int y;

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    y = margin;

    MoveWindow(g_gui.btn_quick, margin, y, left_w, btn_h, TRUE); y += btn_h + 8;
    MoveWindow(g_gui.btn_full, margin, y, left_w, btn_h, TRUE); y += btn_h + 8;
    MoveWindow(g_gui.btn_stress, margin, y, left_w, btn_h, TRUE); y += btn_h + 16;
    MoveWindow(g_gui.btn_clear, margin, y, left_w, btn_h, TRUE); y += btn_h + 8;
    MoveWindow(g_gui.btn_openlog, margin, y, left_w, btn_h, TRUE); y += btn_h + 8;
    MoveWindow(g_gui.btn_exit, margin, h - margin - btn_h, left_w, btn_h, TRUE);

    MoveWindow(g_gui.summary, margin * 2 + left_w, margin, w - left_w - margin * 3, 24, TRUE);
    MoveWindow(g_gui.edit, margin * 2 + left_w, margin + 30,
               w - left_w - margin * 3, h - margin * 2 - 30, TRUE);
}

static LRESULT CALLBACK gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
    {
        char temp[MAX_PATH];
        GetTempPathA(sizeof(temp), temp);
        wsprintfA(g_gui.log_path, "%swinehua_graphics_diag_gui.log", temp);

        g_gui.hwnd = hwnd;
        g_gui.btn_quick = gui_create_button(hwnd, "Quick Test", IDC_BTN_QUICK);
        g_gui.btn_full = gui_create_button(hwnd, "Full Test", IDC_BTN_FULL);
        g_gui.btn_stress = gui_create_button(hwnd, "Stress Test", IDC_BTN_STRESS);
        g_gui.btn_clear = gui_create_button(hwnd, "Clear Log", IDC_BTN_CLEAR);
        g_gui.btn_openlog = gui_create_button(hwnd, "Open Log", IDC_BTN_OPENLOG);
        g_gui.btn_exit = gui_create_button(hwnd, "Exit", IDC_BTN_EXIT);
        g_gui.summary = CreateWindowExA(0, "STATIC", "Ready. Click a test button. Log is written to %TEMP%.",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        0, 0, 100, 24, hwnd, (HMENU)IDC_STATIC_SUM, GetModuleHandleA(NULL), NULL);
        g_gui.edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                     ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
                                     0, 0, 100, 100, hwnd, (HMENU)IDC_EDIT_LOG, GetModuleHandleA(NULL), NULL);
        SendMessageA(g_gui.edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        gui_layout(hwnd);
        gui_load_log_file();
        return 0;
    }
    case WM_SIZE:
        gui_layout(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == DIAG_TIMER_ID) gui_load_log_file();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BTN_QUICK:
            gui_start_run("quick");
            return 0;
        case IDC_BTN_FULL:
            gui_start_run("full");
            return 0;
        case IDC_BTN_STRESS:
            gui_start_run("stress");
            return 0;
        case IDC_BTN_CLEAR:
            if (!g_gui.running) {
                DeleteFileA(g_gui.log_path);
                SetWindowTextA(g_gui.edit, "");
                gui_set_status("Log cleared.");
            }
            return 0;
        case IDC_BTN_OPENLOG:
            gui_open_log();
            return 0;
        case IDC_BTN_EXIT:
            if (!g_gui.running) DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DIAG_DONE:
        gui_on_done((int)wparam);
        return 0;
    case WM_CLOSE:
        if (g_gui.running) {
            MessageBoxA(hwnd, "A diagnostic is running. Please wait until it finishes.",
                        "WineHua Graphics Diagnostic", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmdline, int show)
{
    WNDCLASSEXA wc;
    HWND hwnd;
    MSG msg;
    (void)hprev;
    (void)cmdline;

    ZeroMemory(&g_gui, sizeof(g_gui));
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = gui_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WineHuaGraphicsDiagGui";

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "RegisterClassEx failed.", "WineHua Graphics Diagnostic", MB_OK | MB_ICONERROR);
        return 1;
    }

    hwnd = CreateWindowExA(0, wc.lpszClassName, "WineHua Graphics Diagnostic GUI",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           980, 680, NULL, NULL, hinst, NULL);
    if (!hwnd) {
        MessageBoxA(NULL, "CreateWindow failed.", "WineHua Graphics Diagnostic", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
