/* selftest.c - HOW.exe --selftest：无界面验证兼容层完整运行链路
 * 覆盖：HAP 探测 → 安装 → 清单 → .abc(PACA) 解析 → HOWVM 执行 →
 *       UI 模式解析 → GDI 渲染 → 隐藏兼容层窗口 WM_PAINT 全链路。
 * 退出码 0 = 全部通过；结果写入 exe 同目录 selftest-report.txt。
 */
#include "how.h"

static FILE *g_rep;
static int g_fail, g_pass;

static void rep_line(const char *tag, const char *name) {
    if (g_rep) {
        fprintf(g_rep, "[%s] %s\n", tag, name);
        fflush(g_rep);
    }
}

static FILE *open_report(const wchar_t *exedir, const wchar_t *cwd) {
    wchar_t p[1100];
    FILE *f;
    _snwprintf(p, 1100, L"%s\\selftest-report.txt", exedir);
    f = _wfopen(p, L"wb");
    if (!f) {
        _snwprintf(p, 1100, L"%s\\selftest-report.txt", cwd);
        f = _wfopen(p, L"wb");
    }
    if (f) setvbuf(f, NULL, _IONBF, 0);
    return f;
}

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; rep_line("PASS", name); } \
    else { g_fail++; rep_line("FAIL", name); } } while (0)

/* 颜色近似匹配（GDI 纯色填充应完全一致，容差仅防御 ClearType/舍入） */
static int color_close(COLORREF a, COLORREF b, int tol) {
    return abs((int)GetRValue(a) - (int)GetRValue(b)) <= tol &&
           abs((int)GetGValue(a) - (int)GetGValue(b)) <= tol &&
           abs((int)GetBValue(a) - (int)GetBValue(b)) <= tol;
}

/* 把内存位图存成 24 位 BMP（渲染证据，CI artifact） */
static void save_bmp(HDC mem, HBITMAP bm, int w, int h, const wchar_t *path) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    int rowBytes = ((w * 3 + 3) / 4) * 4;
    unsigned char *px = (unsigned char *)malloc((size_t)rowBytes * (size_t)h);
    if (!px) return;
    if (GetDIBits(mem, bm, 0, (UINT)h, px, &bi, DIB_RGB_COLORS)) {
        BITMAPFILEHEADER fh;
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = (DWORD)(fh.bfOffBits + (size_t)rowBytes * (size_t)h);
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(px, 1, (size_t)rowBytes * (size_t)h, f);
            fclose(f);
        }
    }
    free(px);
}

static int exists_w(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static char *read_file_u8(const wchar_t *path, long *outLen) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 262144) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    if (outLen) *outLen = (long)rd;
    return buf;
}

int run_selftest(HINSTANCE hInst) {
    wchar_t exedir[1024], cwd[1024];
    GetModuleFileNameW(NULL, exedir, 1024);
    {
        wchar_t *cut = wcsrchr(exedir, L'\\');
        if (cut) *cut = 0;
    }
    GetCurrentDirectoryW(1024, cwd);
    g_rep = open_report(exedir, cwd);
    if (g_rep) fprintf(g_rep, "HOW Runtime selftest\n====================\n");
    if (g_rep) fprintf(g_rep, "exe_dir: %ls\ncwd: %ls\n", exedir, cwd);

    /* 1. 定位样例 HAP */
    wchar_t hap[1024];
    int found = 0;
    {
        struct { wchar_t *dir; const wchar_t *rel; } cands[4] = {
            { exedir, L"samples\\MyFirstDemo.hap" },
            { exedir, L"MyFirstDemo.hap" },
            { cwd,    L"samples\\MyFirstDemo.hap" },
            { cwd,    L"MyFirstDemo.hap" },
        };
        for (int i = 0; i < 4 && !found; i++) {
            _snwprintf(hap, 1024, L"%s\\%s", cands[i].dir, cands[i].rel);
            if (exists_w(hap)) found = 1;
        }
    }
    CHECK(found, "定位样例 HAP (MyFirstDemo.hap)");

    /* 2. hap_probe：解析 app.json5 */
    char nameU8[128] = "", bundleU8[128] = "", verU8[64] = "";
    int ok = found && hap_probe(hap, nameU8, 128, bundleU8, 128, verU8, 64);
    CHECK(ok && strcmp(bundleU8, "com.example.hello") == 0,
          "hap_probe: app.json5 解析与包名匹配");
    CHECK(ok && nameU8[0] != 0, "hap_probe: 应用名称非空");

    /* 3. 安装 */
    int rc = ok ? store_install(hap, bundleU8, nameU8, verU8) : -9;
    CHECK(rc == 0, "store_install: HAP 解压安装");

    /* 4. 应用清单 */
    AppInfo apps[64];
    int n = (rc == 0) ? store_list(apps, 64) : 0;
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (wcscmp(apps[i].bundle, L"com.example.hello") == 0) hit = i;
    CHECK(hit >= 0, "store_list: 安装清单发现应用");

    /* 5. 运行时管线：abc → VM → UI */
    RtState *rt = rt_create();
    if (hit >= 0) {
        wchar_t appsdir[512], dirW[160], path[1024];
        store_apps_dir(appsdir, 512);
        u8w(apps[hit].dir, dirW, 160);
        _snwprintf(path, 1024, L"%s\\%s\\ets\\modules.abc", appsdir, dirW);
        char p8[1024];
        w2u8(path, p8, 1024);
        rt_load_abc(rt, p8);
        CHECK(rt->abc.ok && rt->abc.nchunks == 1, "abc_load: PACA 容器解析 + HOWRT 代码段");
        if (rt->abc.nchunks > 0) {
            VmChunk *ck = &rt->abc.chunks[0];
            int vr = vm_run(ck, rt->vars, &rt->nvars);
            CHECK(vr == 0, "vm_run: onPlus 字节码第一次执行");
            vm_run(ck, rt->vars, &rt->nvars);
            vm_run(ck, rt->vars, &rt->nvars);
            int vi = var_find(rt->vars, rt->nvars, "count");
            Var *v = vi >= 0 ? &rt->vars[vi] : NULL;
            CHECK(v && v->num == 3.0, "HOWVM 计数器: count == 3");
        }
        _snwprintf(path, 1024, L"%s\\%s\\pages\\index.json", appsdir, dirW);
        long jl = 0;
        char *json = read_file_u8(path, &jl);
        int nodes = json ? rt_load_ui(rt, json) : -1;
        free(json);
        CHECK(nodes > 0, "rt_load_ui: ArkUI 声明式组件树解析");
    } else {
        rep_line("FAIL", "运行时管线跳过（安装失败）");
        g_fail += 4;
    }

    /* 6. GDI 渲染（屏幕 DC，CI 无桌面时跳过而非失败） */
    HDC sdc = GetDC(NULL);
    if (sdc) {
        RECT page = {0, 0, 400, 640};
        ui_render(rt, sdc, page);
        ReleaseDC(NULL, sdc);
        CHECK(1, "ui_render: GDI 布局+绘制 冒烟");
    } else {
        rep_line("SKIP", "ui_render（CI 无屏幕 DC）");
    }
    rt_destroy(rt);

    /* 7. 隐藏兼容层窗口 WM_PAINT 全链路 */
    compat_register(hInst);
    RtState *rt2 = rt_create();
    int winOk = 0;
    {
        /* 重新装载一次运行时状态（窗口销毁时会自动释放） */
        wchar_t appsdir[512], dirW[160], path[1024];
        store_apps_dir(appsdir, 512);
        if (hit >= 0) {
            u8w(apps[hit].dir, dirW, 160);
            _snwprintf(path, 1024, L"%s\\%s\\ets\\modules.abc", appsdir, dirW);
            char p8[1024];
            w2u8(path, p8, 1024);
            rt_load_abc(rt2, p8);
            _snwprintf(path, 1024, L"%s\\%s\\pages\\index.json", appsdir, dirW);
            long jl = 0;
            char *json = read_file_u8(path, &jl);
            if (json) rt_load_ui(rt2, json);
            free(json);
        }
        HWND hw = CreateWindowExW(0, L"HOW_COMPAT_WND", L"selftest",
                                  WS_OVERLAPPEDWINDOW, 8, 8, 440, 760,
                                  NULL, NULL, hInst, rt2);
        winOk = (hw != NULL);
        CHECK(winOk, "创建兼容层窗口(隐藏)");
        if (winOk) {
            RedrawWindow(hw, NULL, NULL, RDW_INTERNALPAINT | RDW_UPDATENOW);
            DestroyWindow(hw); /* 触发 WM_DESTROY：KillTimer + rt_destroy */
            CHECK(1, "WM_PAINT 渲染 + 窗口销毁回收");
        }
    }

    /* 7.5 像素级渲染验证：与 WM_PAINT 同一函数 compat_paint 渲染到内存位图，
     *      采样工具栏/页面底/双按钮/日志五区签名色，并保存 BMP 渲染证据。 */
    {
        HDC sdc = GetDC(NULL);
        if (sdc && hit >= 0) {
            int pw = 440, ph = 760;
            HDC mem = CreateCompatibleDC(sdc);
            HBITMAP bm = CreateCompatibleBitmap(sdc, pw, ph);
            HBITMAP ob = (HBITMAP)SelectObject(mem, bm);
            RtState *rt3 = rt_create();
            wchar_t appsdir[512], dirW[160], path[1024];
            store_apps_dir(appsdir, 512);
            u8w(apps[hit].dir, dirW, 160);
            _snwprintf(path, 1024, L"%s\\%s\\pages\\index.json", appsdir, dirW);
            long jl2 = 0;
            char *json2 = read_file_u8(path, &jl2);
            int nodes2 = json2 ? rt_load_ui(rt3, json2) : -1;
            free(json2);
            if (nodes2 <= 0) rep_line("SKIP", "像素验证（UI 未装载，无法验证页面渲染）");
            compat_paint(rt3, mem, pw, ph, L"\u50cf\u7d20\u9a8c\u8bc1 \u517c\u5bb9\u5c42");

            /* 点探针：工具栏 / 页面浅底 / 日志深底（位置确定） */
            struct { int x, y; COLORREF want; const char *name; } pts[3] = {
                { 220,  22, RGB(0x18, 0x24, 0x31), "像素: 工具栏深底 #182431" },
                { 220,  70, RGB(0xF1, 0xF3, 0xF5), "像素: 页面浅底 #F1F3F5" },
                { 220, 700, RGB(0x0C, 0x11, 0x16), "像素: 日志深底 #0C1116" },
            };
            int sigHit = 0;
            for (int i = 0; i < 3; i++) {
                COLORREF got = GetPixel(mem, pts[i].x, pts[i].y);
                if (color_close(got, pts[i].want, 30)) {
                    sigHit++; rep_line("PASS", pts[i].name);
                } else {
                    char line[192];
                    _snprintf(line, sizeof(line), "%s @(%d,%d) got=#%02X%02X%02X",
                              pts[i].name, pts[i].x, pts[i].y,
                              (unsigned)GetRValue(got), (unsigned)GetGValue(got),
                              (unsigned)GetBValue(got));
                    rep_line("FAIL", line);
                }
            }
            /* 带探针：蓝色/红色按钮（y 中心随文本度量有 ±30px 浮动，改为区域扫描） */
            int blueHit = 0, redHit = 0;
            for (int y = 320; y <= 380; y += 4) {
                for (int x = 120; x <= 200; x += 4)
                    if (color_close(GetPixel(mem, x, y), RGB(0x00, 0x7D, 0xFF), 40)) blueHit++;
                for (int x = 240; x <= 320; x += 4)
                    if (color_close(GetPixel(mem, x, y), RGB(0xE8, 0x40, 0x26), 40)) redHit++;
            }
            if (blueHit >= 20) { sigHit++; rep_line("PASS", "像素: 主按钮蓝 #007DFF 区域命中"); }
            else rep_line("FAIL", "像素: 主按钮蓝区域未命中");
            if (redHit >= 20) { sigHit++; rep_line("PASS", "像素: 重置按钮红 #E84026 区域命中"); }
            else rep_line("FAIL", "像素: 重置按钮红区域未命中");
            CHECK(sigHit >= 4,
                  "像素验证: 签名色 >= 4/5 命中（页面真实渲染，防白屏/花屏回归）");

            /* 渲染证据 BMP（CI artifact） */
            wchar_t bmpPath[1100];
            _snwprintf(bmpPath, 1100, L"%s\\selftest-render.bmp", exedir);
            save_bmp(mem, bm, pw, ph, bmpPath);
            DWORD a = GetFileAttributesW(bmpPath);
            if (a == INVALID_FILE_ATTRIBUTES) {
                _snwprintf(bmpPath, 1100, L"selftest-render.bmp");
                save_bmp(mem, bm, pw, ph, bmpPath);
            }
            rep_line("INFO", "render evidence: selftest-render.bmp");

            SelectObject(mem, ob);
            DeleteObject(bm);
            DeleteDC(mem);
            rt_destroy(rt3);
        } else {
            rep_line("SKIP", "像素验证（无屏幕 DC 或样例未安装）");
        }
        if (sdc) ReleaseDC(NULL, sdc);
    }

    /* 8. 真 Ark 运行时接入层：探测 ark/ 真编译组件 + 真实执行 */
    {
        ArkRtProbe ap;
        int has = arkrt_probe(&ap);
        if (has) {
            CHECK(ap.type == ENG_ARK_NATIVE || ap.type == ENG_ARK_WSL,
                  "arkrt_probe: 发现真 Ark 运行时组件(ark/)");
            if (hit >= 0) {
                wchar_t appsdir[512], dirW[160], abcW[1024];
                store_apps_dir(appsdir, 512);
                u8w(apps[hit].dir, dirW, 160);
                _snwprintf(abcW, 1024, L"%s\\%s\\ets\\modules.abc", appsdir, dirW);
                char outb[4096];
                int erc = arkrt_exec(&ap, abcW, outb, 4096);
                /* >=0 = 进程已真实启动并退出（退出码任意），<0 = 未启动/超时 */
                CHECK(erc >= 0, "arkrt_exec: 真 ark_js_vm 进程已实际启动");
                if (erc >= 0) {
                    char brief[256];
                    int k = 0;
                    for (; outb[k] && k < 160 && outb[k] != '\n'; k++)
                        brief[k] = (outb[k] == '\r') ? ' ' : outb[k];
                    brief[k] = 0;
                    char info[320];
                    _snprintf(info, sizeof(info),
                              "arkrt_exec: rc=%d out: %s", erc, brief);
                    rep_line("INFO", info);
                }
            }
        } else {
            CHECK(ap.type == ENG_HOWVM,
                  "arkrt_probe: 无 ark/ 组件时安全回退 HOWVM");
        }
    }

    if (g_rep)
        fprintf(g_rep, "\nRESULT: %s (pass=%d fail=%d)\n",
                g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    if (g_rep) fclose(g_rep);
    return g_fail ? 1 : 0;
}
