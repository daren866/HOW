/* compat.c - “{App name} 兼容层”窗口：装载 HAP 内 .abc 与 UI 模式并渲染 */
#include "how.h"
#include <windowsx.h>

#define TOOLBAR_H 44
#define LOG_H 128

static const wchar_t WC_COMPAT[] = L"HOW_COMPAT_WND";

static void draw_toolbar(HDC hdc, RECT rcT, const wchar_t *title) {
    HBRUSH br = CreateSolidBrush(RGB(0x18, 0x24, 0x31));
    FillRect(hdc, &rcT, br);
    DeleteObject(br);
    /* 状态指示灯 */
    HBRUSH dot = CreateSolidBrush(RGB(0x34, 0xC7, 0x59));
    HBRUSH od = (HBRUSH)SelectObject(hdc, dot);
    HPEN op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, rcT.left + 16, rcT.top + 18, rcT.left + 26, rcT.top + 28);
    SelectObject(hdc, od);
    SelectObject(hdc, op);
    DeleteObject(dot);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
    HFONT f = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT of = (HFONT)SelectObject(hdc, f);
    RECT tr = rcT;
    tr.left += 36;
    DrawTextW(hdc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, RGB(0x8F, 0xA3, 0xB8));
    HFONT f2 = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    SelectObject(hdc, f2);
    RECT gr = rcT;
    gr.right -= 14;
    DrawTextW(hdc, L"HOW Runtime · GDI \u8f6f\u6e32\u67d3", -1, &gr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
    DeleteObject(f);
    DeleteObject(f2);
}

static void draw_logpane(HDC hdc, RECT rcL, RtState *rt) {
    HBRUSH br = CreateSolidBrush(RGB(0x0C, 0x11, 0x16));
    FillRect(hdc, &rcL, br);
    DeleteObject(br);
    RECT line = rcL;
    line.left += 12;
    line.top += 8;
    line.bottom = line.top + 18;
    HFONT f = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT of = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    /* 取日志尾部若干行，自下而上绘制 */
    const wchar_t *log = rt_logbuf(rt);
    int total = (int)wcslen(log);
    int lines[9];
    int nl = 0;
    int end = total;
    while (end > 0 && nl < 8) {
        int start = end - 1;
        while (start > 0 && log[start - 1] != L'\n') start--;
        lines[nl++] = start;
        end = start - 1;
    }
    int maxLines = (rcL.bottom - rcL.top - 16) / 18;
    if (nl > maxLines) nl = maxLines;
    SetTextColor(hdc, RGB(0xA6, 0xD3, 0xA0));
    for (int i = nl - 1; i >= 0; i--) {
        wchar_t buf[256];
        int len = 0;
        for (int k = lines[i]; log[k] && log[k] != L'\n' && len < 200; k++)
            buf[len++] = log[k];
        buf[len] = 0;
        DrawTextW(hdc, buf, -1, &line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        line.top += 18;
        line.bottom += 18;
    }
    SelectObject(hdc, of);
    DeleteObject(f);
}

static LRESULT CALLBACK compat_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RtState *rt = (RtState *)GetPropW(hwnd, L"HOW_RT");
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetPropW(hwnd, L"HOW_RT", (HANDLE)cs->lpCreateParams);
        SetTimer(hwnd, 1, 500, NULL); /* toast 衰减检查 */
        return 0;
    }
    case WM_PAINT: {
        if (!rt) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int pw = rc.right - rc.left, ph = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bm = CreateCompatibleBitmap(hdc, pw, ph);
        HBITMAP ob = (HBITMAP)SelectObject(mem, bm);
        /* 区域划分：工具栏 / 页面 / 日志 */
        RECT rcT = {0, 0, pw, TOOLBAR_H};
        RECT rcPage = {0, TOOLBAR_H, pw, ph - LOG_H};
        RECT rcLog = {0, ph - LOG_H, pw, ph};
        wchar_t title[192];
        GetWindowTextW(hwnd, title, 192);
        draw_toolbar(mem, rcT, title);
        ui_render(rt, mem, rcPage);
        draw_logpane(mem, rcLog, rt);
        BitBlt(hdc, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!rt) break;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int ph = rc.bottom - rc.top;
        int y = GET_Y_LPARAM(lp), x = GET_X_LPARAM(lp);
        if (y > TOOLBAR_H && y < ph - LOG_H) {
            ui_click(rt, x, y - TOOLBAR_H);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (rt && rt->toastOn) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (rt) { rt_destroy(rt); RemovePropW(hwnd, L"HOW_RT"); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void compat_register(HINSTANCE hInst) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = compat_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = WC_COMPAT;
    RegisterClassW(&wc);
}

void compat_open(HINSTANCE hInst, AppInfo *app) {
    /* 组装运行时状态 */
    RtState *rt = rt_create();
    wchar_t apps[512], path[1024], dirW[160];
    store_apps_dir(apps, 512);
    u8w(app->dir, dirW, 160);
    _snwprintf(path, 1024, L"%s\\%s\\ets\\modules.abc", apps, dirW);
    rt_log(rt, "[HOW Runtime] \u542f\u52a8 \u00b7 \u517c\u5bb9\u5c42: %ls", app->name);
    /* .abc（真实 Panda 容器解析） */
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        char p8[1024];
        w2u8(path, p8, 1024);
        rt_load_abc(rt, p8);
    } else {
        rt_log(rt, "[abc] \u672a\u627e\u5230 ets/modules.abc\uff0c\u4ec5\u52a0\u8f7d UI \u6a21\u5f0f");
    }
    /* UI 模式 */
    _snwprintf(path, 1024, L"%s\\%s\\pages\\index.json", apps, dirW);
    attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        FILE *f = _wfopen(path, L"rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 262144) {
                char *buf = (char *)malloc((size_t)sz + 1);
                size_t rd = fread(buf, 1, (size_t)sz, f);
                buf[rd] = 0;
                int nodes = rt_load_ui(rt, buf);
                free(buf);
                if (nodes > 0)
                    rt_log(rt, "[ui] pages/index.json \u89e3\u6790\u6210\u529f \u00b7 \u8282\u70b9 %d \u4e2a", nodes);
                else
                    rt_log(rt, "[ui] pages/index.json \u89e3\u6790\u5931\u8d25 (%d)", nodes);
            }
            fclose(f);
        }
    } else {
        rt_log(rt, "[ui] \u672a\u627e\u5230 pages/index.json");
    }
    rt_log(rt, "[render] \u540e\u7aef: GDI \u53cc\u7f13\u51b2\u8f6f\u6e32\u67d3 \u00b7 Flex \u5e03\u5c40\u5f15\u64ce");
    rt_log(rt, "[input] \u4e8b\u4ef6\u94fe: WM_LBUTTONUP -> ui_click \u547d\u4e2d\u5206\u53d1");

    /* 竖屏窗口（手机形态），限制在可视工作区内 */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 440, h = 760;
    if (h > wa.bottom - wa.top - 60) h = wa.bottom - wa.top - 60;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    wchar_t title[192];
    _snwprintf(title, 192, L"%ls \u517c\u5bb9\u5c42", app->name);
    HWND hwnd = CreateWindowExW(0, WC_COMPAT, title,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                x, y, w, h, NULL, NULL, hInst, rt);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}
