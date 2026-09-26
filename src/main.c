/* main.c - HOW 宿主主窗口
 *  标题 “HOW - x64转译arm模式”，中部为已安装 HAP 应用列表，
 *  底部 “安装hap” 按钮 → 选择 .hap → 确认 → 安装；双击列表项打开兼容层窗口。
 *  另支持 --show <bundle>：直接打开指定应用的兼容层窗口（GUI 自动化验证用）。
 */
#include "how.h"
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#define IDC_LIST   1001
#define IDC_BTN    1002
#define IDC_HEADER 1003
#define IDC_HINT   1004
#define IDC_STATUS 1005

#define MAX_APPS 64

static struct {
    AppInfo apps[MAX_APPS];
    int n;
    HFONT font, fontBig;
} g;

static HFONT make_font(int h, int bold) {
    return CreateFontW(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static void refresh_list(HWND hwnd) {
    HWND lb = GetDlgItem(hwnd, IDC_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    g.n = store_list(g.apps, MAX_APPS);
    for (int i = 0; i < g.n; i++)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)g.apps[i].name);
    wchar_t hdr[128];
    _snwprintf(hdr, 128, L"\u5df2\u5b89\u88c5\u5e94\u7528\uff08%d\uff09", g.n);
    SetDlgItemTextW(hwnd, IDC_HEADER, hdr);
}

static void layout_controls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    MoveWindow(GetDlgItem(hwnd, IDC_HEADER), 18, 14, w - 36, 30, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_HINT), 18, 46, w - 36, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_LIST), 18, 74, w - 36, h - 74 - 66, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_BTN), (w - 170) / 2, h - 56, 170, 38, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STATUS), 18, h - 92, w - 36, 22, TRUE);
}

static void do_install(HWND hwnd) {
    wchar_t file[4096];
    file[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"HAP \u5e94\u7528\u5305 (*.hap)\0*.hap\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"\u9009\u62e9\u8981\u5b89\u88c5\u7684 HAP \u5e94\u7528\u5305";
    if (!GetOpenFileNameW(&ofn)) return;

    /* 探测包信息 */
    char nameU8[128] = "", bundleU8[128] = "", verU8[64] = "";
    hap_probe(file, nameU8, 128, bundleU8, 128, verU8, 64);
    if (!nameU8[0]) {
        /* 用文件名兜底 */
        wchar_t nm[128];
        wchar_t *slash = wcsrchr(file, L'\\');
        wcsncpy(nm, slash ? slash + 1 : file, 127);
        nm[127] = 0;
        wchar_t *dot = wcsrchr(nm, L'.');
        if (dot) *dot = 0;
        w2u8(nm, nameU8, 128);
    }
    if (!bundleU8[0]) snprintf(bundleU8, 128, "unknown.%lu", (unsigned long)(GetTickCount() % 100000));

    wchar_t msg[1024], wname[128], wbundle[128], wver[64];
    u8w(nameU8, wname, 128);
    u8w(bundleU8, wbundle, 128);
    u8w(verU8, wver, 64);
    _snwprintf(msg, 1024,
               L"\u662f\u5426\u5b89\u88c5\u8be5\u5e94\u7528\uff1f\n\n"
               L"\u5e94\u7528\u540d\u79f0\uff1a%ls\n"
               L"\u5305\u540d\uff1a%ls\n"
               L"\u7248\u672c\uff1a%ls",
               wname, wbundle, wver);
    if (MessageBoxW(hwnd, msg, L"\u5b89\u88c5\u786e\u8ba4",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        SetDlgItemTextW(hwnd, IDC_STATUS, L"\u5df2\u53d6\u6d88\u5b89\u88c5");
        return;
    }
    int rc = store_install(file, bundleU8, nameU8, verU8);
    if (rc == 0) {
        wchar_t ok[256];
        _snwprintf(ok, 256, L"\u201c%ls\u201d\u5b89\u88c5\u6210\u529f", wname);
        SetDlgItemTextW(hwnd, IDC_STATUS, ok);
        refresh_list(hwnd);
        MessageBoxW(hwnd, ok, L"HOW", MB_OK | MB_ICONINFORMATION);
    } else {
        wchar_t bad[256];
        _snwprintf(bad, 256, L"\u5b89\u88c5\u5931\u8d25\uff08rc=%d\uff09\uff1a\u65e0\u6cd5\u89e3\u538b HAP \u5305", rc);
        SetDlgItemTextW(hwnd, IDC_STATUS, bad);
        MessageBoxW(hwnd, bad, L"HOW", MB_OK | MB_ICONERROR);
    }
}

static void open_compat(HWND hwnd, int idx) {
    if (idx < 0 || idx >= g.n) return;
    compat_open(GetModuleHandleW(NULL), &g.apps[idx]);
}

/* --show <bundle>：直接打开指定应用的兼容层窗口（GUI 自动化验证用）。
 * 窗口关闭后进程退出。返回码：0 正常；2 未找到指定应用。 */
static int run_show_mode(HINSTANCE hInst, const wchar_t *bundleW) {
    compat_register(hInst);
    AppInfo apps[MAX_APPS];
    int n = store_list(apps, MAX_APPS);
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (_wcsicmp(apps[i].bundle, bundleW) == 0) { hit = i; break; }
    if (hit < 0) {
        MessageBoxW(NULL, L"\u672a\u627e\u5230\u6307\u5b9a\u5e94\u7528\uff08\u8bf7\u5148\u5b89\u88c5 hap \u6216\u68c0\u67e5 bundle \u540d\uff09",
                    L"HOW --show", MB_OK | MB_ICONERROR);
        return 2;
    }
    compat_set_quit_on_close(1);
    compat_open(hInst, &apps[hit]);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT f = make_font(15, 0);
        g.font = f;
        g.fontBig = make_font(18, 1);
        HWND h;
        h = CreateWindowExW(0, L"STATIC", L"\u5df2\u5b89\u88c5\u5e94\u7528\uff080\uff09",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                            hwnd, (HMENU)(INT_PTR)IDC_HEADER, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)g.fontBig, TRUE);
        h = CreateWindowExW(0, L"STATIC",
                            L"\u53cc\u51fb\u5e94\u7528\u6253\u5f00\u517c\u5bb9\u5c42\u7a97\u53e3 \u00b7 HOW Runtime \u52a0\u8f7d .abc \u5e76\u6e32\u67d3",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                            hwnd, (HMENU)(INT_PTR)IDC_HINT, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
        h = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
        h = CreateWindowExW(0, L"BUTTON", BTN_HAPW,
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                            hwnd, (HMENU)(INT_PTR)IDC_BTN, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)make_font(15, 0), TRUE);
        h = CreateWindowExW(0, L"STATIC", L"",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                            hwnd, (HMENU)(INT_PTR)IDC_STATUS, NULL, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
        refresh_list(hwnd);
        return 0;
    }
    case WM_SIZE:
        layout_controls(hwnd);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_BTN && code == BN_CLICKED) do_install(hwnd);
        else if (id == IDC_LIST && code == LBN_DBLCLK) {
            /* 双击列表项打开兼容层窗口（单击仅选中，不再弹窗） */
            int sel = (int)SendMessageW(GetDlgItem(hwnd, IDC_LIST), LB_GETCURSEL, 0, 0);
            open_compat(hwnd, sel);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 560;
        mmi->ptMinTrackSize.y = 430;
        return 0;
    }
    case WM_DESTROY:
        if (g.font) DeleteObject(g.font);
        if (g.fontBig) DeleteObject(g.fontBig);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev;
    /* 无界面自测模式：HOW.exe --selftest（CI 兼容层运行验证门禁） */
    if (wcsstr(GetCommandLineW(), L"--selftest"))
        return run_selftest(hInst);
    /* GUI 自动化验证模式：HOW.exe --show <bundle> —— 直接打开兼容层窗口 */
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 3 && wcscmp(argv[1], L"--show") == 0) {
            int rc = run_show_mode(hInst, argv[2]);
            LocalFree(argv);
            return rc;
        }
        if (argv) LocalFree(argv);
    }
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"HOW_MAIN_WND";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) return 1;
    compat_register(hInst);

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 760, h = 560;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    HWND hwnd = CreateWindowExW(0, L"HOW_MAIN_WND", APP_TITLEW,
                                WS_OVERLAPPEDWINDOW, x, y, w, h,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
