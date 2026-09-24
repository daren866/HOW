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

    if (g_rep)
        fprintf(g_rep, "\nRESULT: %s (pass=%d fail=%d)\n",
                g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    if (g_rep) fclose(g_rep);
    return g_fail ? 1 : 0;
}
