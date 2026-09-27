/* host_linux.c - HOW Linux x64 宿主（X11/Xft）
 *
 * 与 Windows 侧（main.c/compat.c/store.c/selftest.c/arkrt.c）对等的平台层：
 *   标题 “HOW - x64转译arm模式”，中部为已安装 HAP 应用列表，
 *   底部 “安装hap” 按钮 → 选择 .hap → 确认 → 安装；双击列表项打开兼容层窗口。
 *   兼容层窗口 “{App name} 兼容层”：工具栏 + Flex 页面（ui_render 复用）+ 日志面板。
 *
 * 复用（零改动）：howcore VM（abc/json/ui_parse）+ ui_render.c 布局绘制引擎
 *                （经 plat_linux.h 的 GDI shim）。
 * 本文件实现：Linux 版存储（~/.local/share/HOW/apps）、arkrt 探测执行
 *                （ark/linux/ark_js_vm 直接 fork/exec）、X11 主窗口与兼容层
 *                窗口、headless selftest（CI 门禁，像素采样验证渲染）。
 *
 * 命令行：
 *   HOW                       GUI 主窗口（无 DISPLAY 时报错退出）
 *   HOW --selftest            无界面自测门禁（CI；有 DISPLAY 时附加渲染验证）
 *   HOW --install x.hap       无界面安装（可选 --yes 跳过确认语义一致性）
 */
#include "how.h"
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>

#define TOOLBAR_H 44
#define LOG_H     128

#define IDC_NONE   0
#define IDC_BTN    1002
#define IDC_LIST   1001

#define MAX_APPS 64

/* ================= Linux 版存储层（对等 store.c） ================= */

void store_apps_dir(wchar_t *out, int cap) {
    char base[700] = "";
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(base, sizeof(base), "%s/HOW/apps", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(base, sizeof(base), "%s/.local/share/HOW/apps", home);
    }
    u8w(base, out, cap);
    /* 逐级建目录 */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", base);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void store_dir_from_bundle(const char *bundleU8, char *out, int cap) {
    int j = 0;
    for (int i = 0; bundleU8[i] && j < cap - 1; i++) {
        char c = bundleU8[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[j++] = c;
        else out[j++] = '_';
    }
    out[j] = 0;
}

/* zip 内条目读取（对等 store.c 的 zip_read_entry） */
#include "miniz.h"

static char *zip_read_entry(mz_zip_archive *z, const char *entry, long *outLen) {
    int idx = mz_zip_reader_locate_file(z, entry, NULL, 0);
    if (idx < 0) return NULL;
    size_t len = 0;
    void *buf = mz_zip_reader_extract_to_heap(z, (mz_uint)idx, &len, 0);
    if (!buf) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) { mz_free(buf); return NULL; }
    memcpy(out, buf, len);
    out[len] = 0;
    mz_free(buf);
    if (outLen) *outLen = (long)len;
    return out;
}

int hap_probe(const wchar_t *hapPath, char *nameU8, int nameCap,
              char *bundleU8, int bCap, char *verU8, int vCap) {
    char pathA[1024];
    w2u8(hapPath, pathA, 1024);
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_file(&z, pathA, 0)) return 0;
    int ok = 0;
    long len = 0;
    char *data = zip_read_entry(&z, "app.json5", &len);
    if (data) {
        JV *j = json5_parse(data);
        if (j) {
            JV *app = jv_get(j, "app");
            const char *bn = jv_str(jv_get(app, "bundleName"), NULL);
            const char *lb = jv_str(jv_get(app, "label"), NULL);
            const char *vn = jv_str(jv_get(app, "versionName"), "");
            if (bn && *bn) {
                snprintf(bundleU8, (size_t)bCap, "%s", bn);
                ok = 1;
            }
            if (lb && *lb && strncmp(lb, "$string:", 8) != 0)
                snprintf(nameU8, (size_t)nameCap, "%s", lb);
            if (vn) snprintf(verU8, (size_t)vCap, "%s", vn);
            jv_free(j);
        }
        free(data);
    }
    if (ok && (!nameU8[0])) {
        long plen = 0;
        char *pack = zip_read_entry(&z, "pack.info", &plen);
        if (pack) {
            JV *j = json5_parse(pack);
            JV *s = jv_get(j, "summary");
            JV *a = jv_get(s, "app");
            const char *lb = jv_str(jv_get(a, "label"), NULL);
            if (lb && *lb) snprintf(nameU8, (size_t)nameCap, "%s", lb);
            jv_free(j);
            free(pack);
        }
    }
    mz_zip_reader_end(&z);
    return ok;
}

static int entry_unsafe(const char *name) {
    if (!name || !name[0]) return 1;
    if (name[0] == '/' || name[0] == '\\') return 1;
    if (strstr(name, "..")) return 1;
    if (strchr(name, ':')) return 1;
    return 0;
}

static void mkdirs_u8(const char *pathU8) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", pathU8);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_install_json(const char *dirU8, const char *nameU8,
                               const char *bundleU8, const char *verU8) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/install.json", dirU8);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    fprintf(f,
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"bundle\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"installed\": \"%04d-%02d-%02d %02d:%02d:%02d\"\n"
        "}\n",
        nameU8, bundleU8, verU8 ? verU8 : "",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    fclose(f);
}

int store_install(const wchar_t *hapPath, const char *bundleU8,
                  const char *nameU8, const char *verU8) {
    char pathA[1024];
    w2u8(hapPath, pathA, 1024);
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_file(&z, pathA, 0)) return -1;

    char dirU8[1024];
    {
        char appsU8[768];
        wchar_t appsW[512];
        store_apps_dir(appsW, 512);
        w2u8(appsW, appsU8, 768);
        char sub[160];
        store_dir_from_bundle(bundleU8, sub, 160);
        snprintf(dirU8, sizeof(dirU8), "%s/%s", appsU8, sub);
    }
    mkdirs_u8(dirU8);

    unsigned n = mz_zip_reader_get_num_files(&z);
    unsigned installed = 0;
    for (unsigned i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        if (entry_unsafe(st.m_filename)) continue;
        size_t L = strlen(st.m_filename);
        if (L > 0 && st.m_filename[L - 1] == '/') continue;
        char destA[1300];
        snprintf(destA, sizeof(destA), "%s/%s", dirU8, st.m_filename);
        /* 建父目录 */
        {
            char parent[1300];
            snprintf(parent, sizeof(parent), "%s", destA);
            char *cut = strrchr(parent, '/');
            if (cut) { *cut = 0; mkdirs_u8(parent); }
        }
        if (mz_zip_reader_extract_to_file(&z, i, destA, 0)) installed++;
    }
    mz_zip_reader_end(&z);
    if (installed == 0) return -2;
    write_install_json(dirU8, nameU8, bundleU8, verU8);
    return 0;
}

int store_list(AppInfo *arr, int max) {
    char appsU8[768];
    wchar_t appsW[512];
    store_apps_dir(appsW, 512);
    w2u8(appsW, appsU8, 768);
    DIR *d = opendir(appsU8);
    if (!d) return 0;
    int cnt = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && cnt < max) {
        if (de->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", appsU8, de->d_name);
        struct stat stt;
        if (stat(sub, &stt) != 0 || !S_ISDIR(stt.st_mode)) continue;
        char ij[1200];
        snprintf(ij, sizeof(ij), "%s/install.json", sub);
        FILE *f = fopen(ij, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 65536) { fclose(f); continue; }
        char *buf = (char *)malloc((size_t)sz + 1);
        size_t rd = fread(buf, 1, (size_t)sz, f);
        buf[rd] = 0;
        fclose(f);
        JV *j = json5_parse(buf);
        free(buf);
        if (!j) continue;
        AppInfo *ai = &arr[cnt];
        memset(ai, 0, sizeof(AppInfo));
        u8w(jv_str(jv_get(j, "name"), ""), ai->name, 128);
        u8w(jv_str(jv_get(j, "bundle"), ""), ai->bundle, 128);
        u8w(jv_str(jv_get(j, "version"), ""), ai->version, 64);
        if (!ai->name[0]) u8w(de->d_name, ai->name, 128);
        snprintf(ai->dir, sizeof(ai->dir), "%s", de->d_name);
        cnt++;
        jv_free(j);
    }
    closedir(d);
    return cnt;
}

/* ================= Linux 版 arkrt（对等 arkrt.c） ================= */

static int file_exists_u8(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void exe_dir_u8(char *out, int cap) {
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) {
        if (getcwd(out, (size_t)cap)) return;
        out[0] = 0;
        return;
    }
    out[n] = 0;
    char *cut = strrchr(out, '/');
    if (cut) *cut = 0;
}

int arkrt_probe(ArkRtProbe *out) {
    memset(out, 0, sizeof(*out));
    out->type = ENG_HOWVM;
    /* Linux 布局：exe 同目录 ark/linux/ark_js_vm（CI ark-runtime job 产物） */
    char exedir[1024], cand[1200];
    exe_dir_u8(exedir, 1024);
    snprintf(cand, sizeof(cand), "%s/ark/linux/ark_js_vm", exedir);
    if (!file_exists_u8(cand)) snprintf(cand, sizeof(cand), "ark/linux/ark_js_vm");
    if (!file_exists_u8(cand)) return 0;
    out->type = ENG_ARK_NATIVE;
    u8w(cand, out->exePath, 1024);
    u8w("上游 arkcompiler 构建产物（Linux 原生）", out->detail, 192);
    return 1;
}

int arkrt_exec(ArkRtProbe *p, const wchar_t *abcPathW, char *outBuf, int outCap) {
    if (!p || p->type != ENG_ARK_NATIVE) return -1;
    char exeU8[1100], abcU8[1100];
    w2u8(p->exePath, exeU8, 1100);
    w2u8(abcPathW, abcU8, 1100);
    if (!file_exists_u8(exeU8)) return -2;
    /* 捕获 stdout+stderr 的真实进程执行（20s 超时，对齐 Windows 版） */
    int fds[2];
    if (pipe(fds) != 0) return -3;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -3; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execl(exeU8, exeU8, abcU8, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    /* 非阻塞读 + 轮询超时 */
    int total = 0, done = 0;
    for (;;) {
        struct pollfd pf = { fds[0], POLLIN, 0 };
        int pr = poll(&pf, 1, 200);
        if (pr > 0) {
            ssize_t n = read(fds[0], outBuf + total, (size_t)(outCap - 1 - total));
            if (n > 0) {
                total += (int)n;
                if (total >= outCap - 1) { total = outCap - 1; break; }
                continue;
            }
            if (n == 0) break;   /* EOF：子进程关闭输出 */
        }
        /* 检查是否退出 */
        int st = 0;
        pid_t wr = waitpid(pid, &st, WNOHANG);
        if (wr == pid) {
            /* 收尾读取剩余输出 */
            while (total < outCap - 1) {
                ssize_t n = read(fds[0], outBuf + total, (size_t)(outCap - 1 - total));
                if (n <= 0) break;
                total += (int)n;
            }
            done = 1;
            break;
        }
        static int waited = 0;
        waited += 200;
        if (waited > 20000) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            done = 0;
            break;
        }
    }
    close(fds[0]);
    outBuf[total] = 0;
    if (!done) {
        /* 超时分支：确保进程已回收 */
        int st;
        waitpid(pid, &st, WNOHANG);
        return -4;
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -5;
    if (WIFEXITED(st)) {
        int rc = WEXITSTATUS(st);
        return rc == 127 ? -6 : rc;
    }
    return -7;
}

const wchar_t *arkrt_engine_name(HowEngine t) {
    switch (t) {
    case ENG_ARK_NATIVE:
        return L"真 Ark 运行时 \u00b7 ark_js_vm (Linux)";
    case ENG_ARK_WSL:
        return L"真 Ark 运行时 \u00b7 ark_js_vm (WSL)";
    default:
        return L"HOW Runtime \u00b7 HOWVM \u5f15\u64ce";
    }
}

/* ================= X11 基础设施 ================= */

typedef struct XWin XWin;

struct XWin {
    Window   w;
    RtState *rt;             /* 兼容层窗口持有；主窗口为 NULL */
    wchar_t  title[192];
    int      wpx, hpx;
    Pixmap   px;             /* 双缓冲 */
    GC       gc;
    int      needRepaint;
    /* 主窗口状态 */
    AppInfo *apps;
    int      napps;
    int      hoverList, selList;
    int      hoverBtn;
    int      clickCandidate;
    int      lastClickIdx;
    DWORD    lastClickTick;
    wchar_t  status[256];
    HFONT    fNorm, fBig, fBtn, fMono;
    HFONT    fonts[24];      /* 兼容层字体缓存（rt_getfont 由 ui_parse 管理） */
    int      nf;
};

static Display *g_dpy;
static int      g_scr;
static Window   g_root;
static Atom     g_wmDelete;
static Atom     g_wmDeleteProto;
static XWin    *g_wins[64];
static int      g_nwins;

static XWin *win_new(void) {
    if (g_nwins >= 64) return NULL;
    XWin *xw = (XWin *)calloc(1, sizeof(XWin));
    g_wins[g_nwins++] = xw;
    return xw;
}

static void win_destroy(XWin *xw) {
    if (xw->px) XFreePixmap(g_dpy, xw->px);
    if (xw->gc) XFreeGC(g_dpy, xw->gc);
    for (int i = 0; i < 24; i++) if (xw->fonts[i]) DeleteObject(xw->fonts[i]);
    if (xw->fNorm) DeleteObject(xw->fNorm);
    if (xw->fBig) DeleteObject(xw->fBig);
    if (xw->fBtn) DeleteObject(xw->fBtn);
    if (xw->fMono) DeleteObject(xw->fMono);
    if (xw->rt) rt_destroy(xw->rt);
    free(xw->apps);
    /* 从注册表移除 */
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i] == xw) {
            g_wins[i] = g_wins[g_nwins - 1];
            g_nwins--;
            break;
        }
    free(xw);
}

static XWin *win_find(Window w) {
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i]->w == w) return g_wins[i];
    return NULL;
}

static HFONT ui_font(XWin *xw, int size, int bold) {
    /* 复用窗口字体缓存（等同 rt_getfont 语义） */
    int key = (size - 9) * 2 + (bold ? 1 : 0);
    if (key >= 0 && key < 24) {
        if (!xw->fonts[key]) {
            xw->fonts[key] = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                                         0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_DONTCARE, L"sans");
        }
        return xw->fonts[key];
    }
    return CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"sans");
}

/* DC 构造：目标为窗口或离屏 Pixmap 的绘图面（isWin 区分 Drawable 种类） */
static HDC dc_for(Drawable d, int isWin, int w, int h) {
    HDC hdc = (HDC)calloc(1, sizeof(HOWDC_));
    hdc->dpy = g_dpy;
    hdc->draw = XftDrawCreate(g_dpy, d, DefaultVisual(g_dpy, g_scr),
                              DefaultColormap(g_dpy, g_scr));
    XGCValues gv;
    hdc->gc = XCreateGC(g_dpy, d, 0, &gv);
    hdc->w = w;
    hdc->h = h;
    hdc->px = isWin ? 0 : d;
    hdc->win = isWin ? d : 0;
    hdc->bkMode = TRANSPARENT;
    return hdc;
}

static void dc_free(HDC hdc) {
    if (!hdc) return;
    if (hdc->draw) XftDrawDestroy(hdc->draw);
    if (hdc->gc) XFreeGC(g_dpy, hdc->gc);
    free(hdc);
}


/* ================= 兼容层窗口绘制（对等 compat.c） ================= */

static void draw_toolbar(HDC hdc, RECT rcT, const wchar_t *title,
                         const wchar_t *engineTag, HFONT fT, HFONT fTag) {
    HBRUSH bg = CreateSolidBrush(RGB(0x18, 0x24, 0x31));
    FillRect(hdc, &rcT, bg);
    DeleteObject(bg);
    HBRUSH dot = CreateSolidBrush(RGB(0x34, 0xC7, 0x59));
    XSetForeground(hdc->dpy, hdc->gc, dot->pixel);
    XFillArc(hdc->dpy, hdc->px ? hdc->px : hdc->win, hdc->gc,
             (int)rcT.left + 16, (int)rcT.top + 18, 10, 10, 0, 360 * 64);
    DeleteObject(dot);
    SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
    SelectObject(hdc, fT);
    RECT tr = rcT;
    tr.left += 36;
    DrawTextW(hdc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, RGB(0x8F, 0xA3, 0xB8));
    SelectObject(hdc, fTag);
    RECT gr = rcT;
    gr.right -= 14;
    DrawTextW(hdc, engineTag ? engineTag : L"HOW Runtime", -1, &gr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void draw_logpane(HDC hdc, RECT rcL, RtState *rt, HFONT fLog) {
    HBRUSH br = CreateSolidBrush(RGB(0x0C, 0x11, 0x16));
    FillRect(hdc, &rcL, br);
    DeleteObject(br);
    RECT line = rcL;
    line.left += 12;
    line.top += 8;
    line.bottom = line.top + 18;
    SelectObject(hdc, fLog);
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
    int maxLines = (int)(rcL.bottom - rcL.top - 16) / 18;
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
}

/* 兼容层一帧完整绘制到窗口的离屏 Pixmap 并翻页 */
static void compat_paint(XWin *xw) {
    RtState *rt = xw->rt;
    if (!rt) return;
    int pw = xw->wpx, ph = xw->hpx;
    if (!xw->px) {
        xw->px = XCreatePixmap(g_dpy, xw->w, (unsigned)pw, (unsigned)ph,
                               (unsigned)DefaultDepth(g_dpy, g_scr));
        XGCValues gv;
        xw->gc = XCreateGC(g_dpy, xw->px, 0, &gv);
    }
    HDC mem = dc_for(xw->px, 0, pw, ph);

    RECT rcT = { 0, 0, pw, TOOLBAR_H };
    RECT rcPage = { 0, TOOLBAR_H, pw, ph - LOG_H };
    RECT rcLog = { 0, ph - LOG_H, pw, ph };
    draw_toolbar(mem, rcT, xw->title,
                 rt->engineTag[0] ? rt->engineTag : NULL,
                 ui_font(xw, 16, 1), ui_font(xw, 12, 0));
    ui_render(rt, mem, rcPage);
    draw_logpane(mem, rcLog, rt, ui_font(xw, 13, 0));

    XCopyArea(g_dpy, xw->px, xw->w, mem->gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
    dc_free(mem);
    XFlush(g_dpy);
}

/* 兼容层窗口创建（对等 compat.c 的 compat_open） */
static void compat_open(XWin *owner, AppInfo *app) {
    RtState *rt = rt_create();
    wchar_t apps[512], path[1024], dirW[160];
    store_apps_dir(apps, 512);
    u8w(app->dir, dirW, 160);
    _snwprintf(path, 1024, L"%s/%s/ets/modules.abc", apps, dirW);
    rt_log(rt, "[HOW Runtime] 启动 \u00b7 兼容层: %ls", app->name);
    if (access(w2u8(path, (char[1100]){0}, 1100), F_OK) == 0) {
        char p8[1100];
        w2u8(path, p8, 1100);
        rt_load_abc(rt, p8);
    } else {
        rt_log(rt, "[abc] 未找到 ets/modules.abc，仅加载 UI 模式");
    }
    _snwprintf(path, 1024, L"%s/%s/pages/index.json", apps, dirW);
    {
        FILE *f = fopen(w2u8(path, (char[1100]){0}, 1100), "rb");
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
                    rt_log(rt, "[ui] pages/index.json 解析成功 \u00b7 节点 %d 个", nodes);
                else
                    rt_log(rt, "[ui] pages/index.json 解析失败 (%d)", nodes);
            }
            fclose(f);
        } else {
            rt_log(rt, "[ui] 未找到 pages/index.json");
        }
    }
    rt_log(rt, "[render] 后端: Xft/X11 双缓冲软渲染 \u00b7 Flex 布局引擎");
    rt_log(rt, "[input] 事件链: ButtonPress -> ui_click 命中分发");

    /* 真 Ark 运行时接入（Linux 版） */
    {
        ArkRtProbe ap;
        int hasReal = arkrt_probe(&ap);
        rt->engine = (int)ap.type;
        _snwprintf(rt->engineTag, 96, L"%ls", arkrt_engine_name(ap.type));
        if (hasReal) {
            rt_log(rt, "[arkrt] 发现真 Ark 运行时组件: %ls", ap.exePath);
            wchar_t abcW[1100];
            _snwprintf(abcW, 1100, L"%s/%s/ets/modules.abc", apps, dirW);
            char outb[4096];
            int erc = arkrt_exec(&ap, abcW, outb, 4096);
            if (erc >= 0) {
                rt_log(rt, "[arkrt] 上游 ark_js_vm 真实加载执行 modules.abc \u00b7 退出码 %d", erc);
                char *line = outb;
                int shown = 0;
                for (char *q = outb; *q && shown < 3; q++) {
                    if (*q == '\n' || *(q + 1) == 0) {
                        int len = (int)(q - line) + (*q != '\n' ? 1 : 0);
                        if (len > 0) {
                            char tmp[256];
                            int cp = len > 200 ? 200 : len;
                            memcpy(tmp, line, (size_t)cp);
                            tmp[cp] = 0;
                            for (char *t = tmp; *t; t++) if (*t == '\r') *t = ' ';
                            rt_log(rt, "[arkvm] %s", tmp);
                            shown++;
                        }
                        line = q + 1;
                    }
                }
            } else {
                rt_log(rt, "[arkrt] 真运行时启动失败(%d) —— UI 动作由 HOWVM 引擎继续承担", erc);
                _snwprintf(rt->engineTag, 96, L"%ls", arkrt_engine_name(ENG_HOWVM));
                rt->engine = (int)ENG_HOWVM;
            }
        } else {
            rt_log(rt, "[arkrt] 未发现 ark/ 真运行时组件，使用内置 HOWVM 引擎");
            _snwprintf(rt->engineTag, 96, L"%ls", arkrt_engine_name(ENG_HOWVM));
        }
    }

    /* 竖屏窗口（手机形态），限制在工作区内 */
    int sw = DisplayWidth(g_dpy, g_scr), sh = DisplayHeight(g_dpy, g_scr);
    int w = 440, h = 760;
    if (h > sh - 60) h = sh - 60;
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;

    XWin *xw = win_new();
    if (!xw) { rt_destroy(rt); return; }
    xw->rt = rt;
    xw->wpx = w;
    xw->hpx = h;
    _snwprintf(xw->title, 192, L"%ls 兼容层", app->name);

    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = WhitePixel(g_dpy, g_scr);
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;
    xw->w = XCreateWindow(g_dpy, g_root, x, y, (unsigned)w, (unsigned)h, 1,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWBackPixel | CWEventMask, &attr);
    char titleU8[400];
    w2u8(xw->title, titleU8, 400);
    XStoreName(g_dpy, xw->w, titleU8);
    XSetWMProtocols(g_dpy, xw->w, &g_wmDelete, 1);
    XMapWindow(g_dpy, xw->w);
    XFlush(g_dpy);
}

/* ================= 主窗口（对等 main.c） ================= */

static void refresh_list(XWin *xw) {
    if (xw->apps) free(xw->apps);
    xw->apps = (AppInfo *)calloc(MAX_APPS, sizeof(AppInfo));
    xw->napps = store_list(xw->apps, MAX_APPS);
    xw->selList = -1;
    xw->hoverList = -1;
}

static void main_layout(XWin *xw, int *rcList, int *rcBtn) {
    int w = xw->wpx, h = xw->hpx;
    rcList[0] = 18; rcList[1] = 74; rcList[2] = w - 18; rcList[3] = h - 74 - 66;
    rcBtn[0] = (w - 170) / 2; rcBtn[1] = h - 56; rcBtn[2] = rcBtn[0] + 170; rcBtn[3] = h - 18;
}

static void main_paint(XWin *xw) {
    int pw = xw->wpx, ph = xw->hpx;
    if (!xw->px) {
        xw->px = XCreatePixmap(g_dpy, xw->w, (unsigned)pw, (unsigned)ph,
                               (unsigned)DefaultDepth(g_dpy, g_scr));
        XGCValues gv;
        xw->gc = XCreateGC(g_dpy, xw->px, 0, &gv);
    }
    HDC mem = dc_for(xw->px, 0, pw, ph);
    /* 背景（COLOR_BTNFACE 近似） */
    HBRUSH bg = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
    RECT all = { 0, 0, pw, ph };
    FillRect(mem, &all, bg);
    DeleteObject(bg);

    wchar_t hdr[128];
    _snwprintf(hdr, 128, L"已安装应用（%d）", xw->napps);
    RECT rH = { 18, 14, pw - 18, 44 };
    SetTextColor(mem, RGB(0x10, 0x18, 0x28));
    SelectObject(mem, ui_font(xw, 18, 1));
    DrawTextW(mem, hdr, -1, &rH, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    RECT rHint = { 18, 46, pw - 18, 70 };
    SetTextColor(mem, RGB(0x5A, 0x66, 0x74));
    SelectObject(mem, ui_font(xw, 15, 0));
    DrawTextW(mem, L"双击应用打开兼容层窗口 \u00b7 HOW Runtime 加载 .abc 并渲染",
              -1, &rHint, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    /* 列表区 */
    int rl[4], rb[4];
    main_layout(xw, rl, rb);
    RECT rL = { rl[0], rl[1], rl[2], rl[3] };
    HBRUSH listBg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(mem, &rL, listBg);
    DeleteObject(listBg);
    /* 边框 */
    XSetForeground(g_dpy, mem->gc, 0x8C8C8C);
    XDrawRectangle(g_dpy, xw->px, mem->gc, rl[0], rl[1],
                   (unsigned)(rl[2] - rl[0] - 1), (unsigned)(rl[3] - rl[1] - 1));
    int rowH = 34;
    int maxRows = (rl[3] - rl[1] - 8) / rowH;
    if (maxRows < 0) maxRows = 0;
    SelectObject(mem, ui_font(xw, 15, 0));
    for (int i = 0; i < xw->napps && i < maxRows; i++) {
        RECT row = { rl[0] + 1, rl[1] + 1 + i * rowH, rl[2] - 1, rl[1] + 1 + (i + 1) * rowH };
        if (i == xw->selList) {
            HBRUSH sel = CreateSolidBrush(RGB(0xC7, 0xE0, 0xF8));
            FillRect(mem, &row, sel);
            DeleteObject(sel);
        } else if (i == xw->hoverList) {
            HBRUSH hov = CreateSolidBrush(RGB(0xE9, 0xF3, 0xFB));
            FillRect(mem, &row, hov);
            DeleteObject(hov);
        }
        RECT rt2 = row;
        rt2.left += 14;
        SetTextColor(mem, RGB(0x1B, 0x1B, 0x1B));
        DrawTextW(mem, xw->apps[i].name, -1, &rt2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (xw->napps == 0) {
        RECT empty = rL;
        SetTextColor(mem, RGB(0x90, 0x99, 0xA3));
        SelectObject(mem, ui_font(xw, 14, 0));
        DrawTextW(mem, L"暂无已安装应用 —— 点击下方“安装hap”", -1, &empty,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* 状态栏 */
    RECT rS = { 18, ph - 92, pw - 18, ph - 70 };
    SetTextColor(mem, RGB(0x2A, 0x56, 0x86));
    SelectObject(mem, ui_font(xw, 14, 0));
    DrawTextW(mem, xw->status[0] ? xw->status : L"", -1, &rS,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    /* 按钮 */
    RECT rB = { rb[0], rb[1], rb[2], rb[3] };
    HBRUSH btn = CreateSolidBrush(xw->hoverBtn ? RGB(0x2B, 0x88, 0xD8) : RGB(0x00, 0x78, 0xD7));
    RoundRect(mem, rb[0], rb[1], rb[2], rb[3], 8, 8);
    DeleteObject(btn);
    RECT rBT = rB;
    SetTextColor(mem, RGB(0xFF, 0xFF, 0xFF));
    SelectObject(mem, ui_font(xw, 16, 0));
    DrawTextW(mem, BTN_HAPW, -1, &rBT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    XCopyArea(g_dpy, xw->px, xw->w, mem->gc, 0, 0, (unsigned)pw, (unsigned)ph, 0, 0);
    dc_free(mem);
    XFlush(g_dpy);
}

/* 安装流程：优先 zenity 文件选择，headless 环境提示命令行 */
static void do_install(XWin *xw) {
    char sel[1024] = "";
    /* zenity 可用性 */
    if (access("/usr/bin/zenity", X_OK) == 0) {
        FILE *pp = popen("zenity --file-selection --title='选择要安装的 HAP 应用包' "
                         "--file-filter='HAP 应用包 | *.hap *.HAP' 2>/dev/null", "r");
        if (pp) {
            if (!fgets(sel, sizeof(sel), pp)) sel[0] = 0;
            pclose(pp);
            size_t L = strlen(sel);
            while (L && (sel[L - 1] == '\n' || sel[L - 1] == '\r')) sel[--L] = 0;
        }
    }
    const char *env = getenv("HOW_HAP_PATH");   /* headless 兜底：环境变量 */
    if (!sel[0] && env && *env) snprintf(sel, sizeof(sel), "%s", env);
    if (!sel[0]) {
        _snwprintf(xw->status, 256,
                   L"未找到文件选择器：请用命令行安装 HOW --install 文件.hap");
        xw->needRepaint = 1;
        return;
    }
    struct stat st;
    if (stat(sel, &st) != 0 || !S_ISREG(st.st_mode)) {
        _snwprintf(xw->status, 256, L"文件不存在: %s", sel);
        xw->needRepaint = 1;
        return;
    }
    /* 探测与安装（对等 main.c 的 do_install） */
    char nameU8[128] = "", bundleU8[128] = "", verU8[64] = "";
    wchar_t fileW[1100];
    u8w(sel, fileW, 1100);
    hap_probe(fileW, nameU8, 128, bundleU8, 128, verU8, 64);
    if (!nameU8[0]) {
        const char *slash = strrchr(sel, '/');
        wchar_t nm[128];
        u8w(slash ? slash + 1 : sel, nm, 128);
        wchar_t *dot = wcsrchr(nm, L'.');
        if (dot) *dot = 0;
        w2u8(nm, nameU8, 128);
    }
    if (!bundleU8[0]) snprintf(bundleU8, 128, "unknown.%u", (unsigned)(time(NULL) % 100000));

    wchar_t wname[128], wbundle[128], wver[64];
    u8w(nameU8, wname, 128);
    u8w(bundleU8, wbundle, 128);
    u8w(verU8, wver, 64);
    /* GUI 模式下 zenity 确认；headless 直接继续（--install 已显式表达意图）。
     * 文本经环境变量传递规避 shell 引号注入。 */
    if (access("/usr/bin/zenity", X_OK) == 0) {
        char text[1200];
        snprintf(text, sizeof(text),
                 "是否安装该应用？\n\n应用名称：%s\n包名：%s\n版本：%s",
                 nameU8, bundleU8, verU8[0] ? verU8 : "-");
        setenv("HOW_CONFIRM_TEXT", text, 1);
        int rc = system("zenity --question --title='安装确认' --width=420 "
                        "--no-wrap --text=\"$HOW_CONFIRM_TEXT\" 2>/dev/null");
        unsetenv("HOW_CONFIRM_TEXT");
        if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
            _snwprintf(xw->status, 256, L"已取消安装");
            xw->needRepaint = 1;
            return;
        }
    }
    int rc = store_install(fileW, bundleU8, nameU8, verU8);
    if (rc == 0) {
        _snwprintf(xw->status, 256, L"“%ls”安装成功", wname);
        refresh_list(xw);
    } else {
        _snwprintf(xw->status, 256, L"安装失败（rc=%d）：无法解压 HAP 包", rc);
    }
    xw->needRepaint = 1;
}

/* ================= 主窗口交互 ================= */

static void main_button(XWin *xw, int x, int y, int pressed) {
    int rl[4], rb[4];
    main_layout(xw, rl, rb);
    /* 列表区命中 */
    if (x >= rl[0] && x < rl[2] && y >= rl[1] && y < rl[3]) {
        int rowH = 34;
        int idx = (y - rl[1] - 1) / rowH;
        int maxRows = (rl[3] - rl[1] - 8) / rowH;
        if (idx < 0 || idx >= xw->napps || idx >= maxRows) idx = -1;
        xw->hoverList = idx;
        if (pressed) {
            if (xw->selList != idx) {
                xw->selList = idx;      /* 单击仅选中 */
                xw->needRepaint = 1;
            }
            xw->clickCandidate = idx;   /* 供双击判定 */
        }
        return;
    }
    xw->hoverList = -1;
    /* 按钮命中 */
    if (x >= rb[0] && x < rb[2] && y >= rb[1] && y < rb[3]) {
        xw->hoverBtn = 1;
        if (pressed) do_install(xw);
    } else {
        xw->hoverBtn = 0;
    }
    xw->needRepaint = 1;
}

/* 双击判定（对等 Windows LBN_DBLCLK：双击间隔内同一行） */
static int main_dblclick(XWin *xw, int idx) {
    if (idx < 0 || idx >= xw->napps) return 0;
    if (xw->lastClickIdx == idx &&
        GetTickCount() - xw->lastClickTick < 500) {
        xw->lastClickIdx = -1;
        return 1;
    }
    xw->lastClickIdx = idx;
    xw->lastClickTick = GetTickCount();
    return 0;
}

/* ================= selftest（对等 selftest.c，Linux 门禁） ================= */

static FILE *g_rep;
static int g_fail, g_pass;

static void rep_line(const char *tag, const char *name) {
    if (g_rep) {
        fprintf(g_rep, "[%s] %s\n", tag, name);
        fflush(g_rep);
        printf("[%s] %s\n", tag, name);
        fflush(stdout);
    }
}

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; rep_line("PASS", name); } \
    else { g_fail++; rep_line("FAIL", name); } } while (0)

static char *read_file_u8c(const char *path, long *outLen) {
    FILE *f = fopen(path, "rb");
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

/* 渲染像素门禁：离屏 Pixmap 渲染页面并采样关键位置（需 DISPLAY） */
static int selftest_render_pixels(RtState *rt, int *outSamples) {
    int pw = 400, ph = 640;
    Pixmap px = XCreatePixmap(g_dpy, g_root, (unsigned)pw, (unsigned)ph,
                              (unsigned)DefaultDepth(g_dpy, g_scr));
    HDC hdc = dc_for(px, 0, pw, ph);
    RECT page = { 0, 0, pw, ph };
    ui_render(rt, hdc, page);
    XSync(g_dpy, False);
    XImage *img = XGetImage(g_dpy, px, 0, 0, (unsigned)pw, (unsigned)ph, AllPlanes, ZPixmap);
    int distinct = 0;
    if (img) {
        /* 统计非背景色像素比例（页面底色 0xF1F3F5 或 root bg） */
        COLORREF pageBg = rt->root && rt->root->hasBg
                              ? rt->root->bg : RGB(0xF1, 0xF3, 0xF5);
        unsigned long bgPixel =
            ((unsigned long)GetRValue(pageBg) << 16) |
            ((unsigned long)GetGValue(pageBg) << 8) |
             (unsigned long)GetBValue(pageBg);
        /* X 像素格式：采样实际图像与 bgPixel 比较按掩码归一 */
        long non = 0;
        for (int y = 0; y < ph; y += 4)
            for (int x = 0; x < pw; x += 4) {
                unsigned long p = XGetPixel(img, x, y);
                if (p != bgPixel) non++;
            }
        distinct = (int)(non * 100 / ((pw / 4) * (ph / 4)));
        *outSamples = distinct;
        XDestroyImage(img);
    }
    dc_free(hdc);
    XFreePixmap(g_dpy, px);
    return distinct;
}

static int run_selftest(void) {
    char exedir[1024];
    exe_dir_u8(exedir, 1024);
    char rep[1100];
    snprintf(rep, sizeof(rep), "%s/selftest-report.txt", exedir);
    g_rep = fopen(rep, "wb");
    if (!g_rep) g_rep = fopen("selftest-report.txt", "wb");
    if (g_rep) {
        setvbuf(g_rep, NULL, _IONBF, 0);
        fprintf(g_rep, "HOW Runtime selftest (Linux x64)\n====================\n");
        fprintf(g_rep, "exe_dir: %s\ncwd: DISPLAY=%s\n", exedir,
                getenv("DISPLAY") ? getenv("DISPLAY") : "(none)");
    }

    /* 1. 定位样例 HAP */
    char hap[1200] = "";
    int found = 0;
    {
        const char *cands[6] = {
            "samples/MyFirstDemo.hap", "MyFirstDemo.hap",
        };
        for (int i = 0; i < 2 && !found; i++) {
            snprintf(hap, sizeof(hap), "%s/%s", exedir, cands[i]);
            if (file_exists_u8(hap)) { found = 1; break; }
            snprintf(hap, sizeof(hap), "%s", cands[i]);
            if (file_exists_u8(hap)) found = 1;
        }
    }
    CHECK(found, "定位样例 HAP (MyFirstDemo.hap)");

    /* 2. hap_probe */
    char nameU8[128] = "", bundleU8[128] = "", verU8[64] = "";
    int ok = 0;
    if (found) {
        wchar_t hapW[1200];
        u8w(hap, hapW, 1200);
        ok = hap_probe(hapW, nameU8, 128, bundleU8, 128, verU8, 64);
    }
    CHECK(ok && strcmp(bundleU8, "com.example.hello") == 0,
          "hap_probe: app.json5 解析与包名匹配");
    CHECK(ok && nameU8[0] != 0, "hap_probe: 应用名称非空");

    /* 3. 安装 */
    int rc = -9;
    if (ok) {
        wchar_t hapW[1200];
        u8w(hap, hapW, 1200);
        rc = store_install(hapW, bundleU8, nameU8, verU8);
    }
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
        char appsU8[768], p8[1100];
        wchar_t appsW[512];
        store_apps_dir(appsW, 512);
        w2u8(appsW, appsU8, 768);
        snprintf(p8, sizeof(p8), "%s/%s/ets/modules.abc", appsU8, apps[hit].dir);
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
        snprintf(p8, sizeof(p8), "%s/%s/pages/index.json", appsU8, apps[hit].dir);
        long jl = 0;
        char *json = read_file_u8c(p8, &jl);
        int nodes = json ? rt_load_ui(rt, json) : -1;
        free(json);
        CHECK(nodes > 0, "rt_load_ui: ArkUI 声明式组件树解析");
    } else {
        rep_line("FAIL", "运行时管线跳过（安装失败）");
        g_fail += 4;
    }

    /* 6. 渲染 + 像素采样（有 DISPLAY 时执行完整门禁，无则跳过） */
    if (g_dpy) {
        int samples = 0;
        int distinct = selftest_render_pixels(rt, &samples);
        CHECK(distinct > 3,
              "ui_render: Xft/X11 布局+绘制 像素门禁（非背景占比>3%）");
        char info[160];
        snprintf(info, sizeof(info),
                 "ui_render 像素统计: %d%% 采样点含内容", distinct);
        rep_line("INFO", info);
    } else {
        rep_line("SKIP", "ui_render（无 DISPLAY；CI 用 xvfb-run 执行完整门禁）");
    }
    rt_destroy(rt);

    /* 7. 真 Ark 运行时接入层 */
    {
        ArkRtProbe ap;
        int has = arkrt_probe(&ap);
        if (has) {
            CHECK(ap.type == ENG_ARK_NATIVE,
                  "arkrt_probe: 发现真 Ark 运行时组件(ark/linux)");
            if (hit >= 0) {
                char appsU8[768];
                wchar_t appsW[512], abcW[1100];
                store_apps_dir(appsW, 512);
                w2u8(appsW, appsU8, 768);
                char abcp[1100];
                snprintf(abcp, sizeof(abcp), "%s/%s/ets/modules.abc",
                         appsU8, apps[hit].dir);
                u8w(abcp, abcW, 1100);
                char outb[4096];
                int erc = arkrt_exec(&ap, abcW, outb, 4096);
                CHECK(erc >= 0, "arkrt_exec: 真 ark_js_vm 进程已实际启动");
                if (erc >= 0) {
                    char brief[256];
                    int k = 0;
                    for (; outb[k] && k < 160 && outb[k] != '\n'; k++)
                        brief[k] = (outb[k] == '\r') ? ' ' : outb[k];
                    brief[k] = 0;
                    char info[320];
                    snprintf(info, sizeof(info),
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
    printf("\nRESULT: %s (pass=%d fail=%d)\n",
           g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    if (g_rep) fclose(g_rep);
    return g_fail ? 1 : 0;
}

/* ================= 事件循环与入口 ================= */

static void event_loop(void) {
    for (;;) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        if (ev.type == g_wmDeleteProto) {
            XWin *xw = win_find(ev.xclient.window);
            if (xw) {
                if (xw->rt) { win_destroy(xw); continue; }
                break;   /* 主窗口关闭 → 退出 */
            }
            continue;
        }
        XWin *xw = win_find(ev.xany.window);
        if (!xw) continue;
        switch (ev.type) {
        case Expose:
            xw->needRepaint = 1;
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != xw->wpx || ev.xconfigure.height != xw->hpx) {
                xw->wpx = ev.xconfigure.width;
                xw->hpx = ev.xconfigure.height;
                if (xw->px) XFreePixmap(g_dpy, xw->px);
                xw->px = 0;
                xw->needRepaint = 1;
            }
            break;
        case ButtonPress:
        case ButtonRelease: {
            int pressed = (ev.type == ButtonPress);
            if (ev.xbutton.button == Button1) {
                if (xw->rt) {
                    if (pressed) {
                        RECT rc = { 0, TOOLBAR_H, xw->wpx, xw->hpx - LOG_H };
                        POINT pt = { ev.xbutton.x, ev.xbutton.y - TOOLBAR_H };
                        if (PtInRect(&rc, pt)) {
                            ui_click(xw->rt, (int)pt.x, (int)pt.y);
                            xw->needRepaint = 1;
                        }
                    }
                } else if (pressed) {
                    /* 双击判定：与上次同行的快速连击打开兼容层 */
                    int rl[4];
                    main_layout(xw, rl, (int[4]){0});
                    if (ev.xbutton.x >= rl[0] && ev.xbutton.x < rl[2] &&
                        ev.xbutton.y >= rl[1] && ev.xbutton.y < rl[3]) {
                        int rowH = 34;
                        int idx = (ev.xbutton.y - rl[1] - 1) / rowH;
                        int maxRows = (rl[3] - rl[1] - 8) / rowH;
                        if (idx < 0 || idx >= xw->napps || idx >= maxRows) idx = -1;
                        if (main_dblclick(xw, idx) && idx >= 0) {
                            compat_open(xw, &xw->apps[idx]);   /* 双击打开 */
                            xw->needRepaint = 1;
                        } else {
                            main_button(xw, ev.xbutton.x, ev.xbutton.y, 1);
                        }
                    } else {
                        main_button(xw, ev.xbutton.x, ev.xbutton.y, 1);
                    }
                }
            }
            break;
        }
        case MotionNotify:
            if (!xw->rt) {
                int oldH = xw->hoverList, oldB = xw->hoverBtn;
                xw->hoverBtn = 0;
                int rl[4], rb[4];
                main_layout(xw, rl, rb);
                if (ev.xbutton.x >= rl[0] && ev.xbutton.x < rl[2] &&
                    ev.xbutton.y >= rl[1] && ev.xbutton.y < rl[3]) {
                    int rowH = 34;
                    int idx = (ev.xbutton.y - rl[1] - 1) / rowH;
                    int maxRows = (rl[3] - rl[1] - 8) / rowH;
                    xw->hoverList = (idx >= 0 && idx < xw->napps && idx < maxRows) ? idx : -1;
                } else {
                    xw->hoverList = -1;
                    if (ev.xbutton.x >= rb[0] && ev.xbutton.x < rb[2] &&
                        ev.xbutton.y >= rb[1] && ev.xbutton.y < rb[3])
                        xw->hoverBtn = 1;
                }
                if (oldH != xw->hoverList || oldB != xw->hoverBtn) xw->needRepaint = 1;
            }
            break;
        default:
            break;
        }
        /* toast 衰减重绘（对等 WM_TIMER） */
        for (int i = 0; i < g_nwins; i++) {
            XWin *ww = g_wins[i];
            if (ww->rt && ww->rt->toastOn &&
                GetTickCount() - ww->rt->toastTick >= 2000) {
                ww->rt->toastOn = 0;
                ww->needRepaint = 1;
            }
            if (ww->needRepaint) {
                ww->needRepaint = 0;
                if (ww->rt) compat_paint(ww);
                else main_paint(ww);
            }
        }
    }
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    /* 命令行模式 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            /* selftest 尽量带 DISPLAY（CI: xvfb-run HOW --selftest） */
            g_dpy = XOpenDisplay(NULL);
            if (g_dpy) {
                g_scr = DefaultScreen(g_dpy);
                g_root = DefaultRootWindow(g_dpy);
            }
            int rc = run_selftest();
            if (g_dpy) XCloseDisplay(g_dpy);
            return rc;
        }
        if (strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            const char *hap = argv[i + 1];
            if (!file_exists_u8(hap)) {
                fprintf(stderr, "HAP 文件不存在: %s\n", hap);
                return 2;
            }
            wchar_t hapW[1200];
            u8w(hap, hapW, 1200);
            char nameU8[128] = "", bundleU8[128] = "", verU8[64] = "";
            hap_probe(hapW, nameU8, 128, bundleU8, 128, verU8, 64);
            if (!nameU8[0]) {
                const char *slash = strrchr(hap, '/');
                wchar_t nm[128];
                u8w(slash ? slash + 1 : hap, nm, 128);
                wchar_t *dot = wcsrchr(nm, L'.');
                if (dot) *dot = 0;
                w2u8(nm, nameU8, 128);
            }
            if (!bundleU8[0]) snprintf(bundleU8, 128, "unknown.%u", (unsigned)(time(NULL) % 100000));
            int rc = store_install(hapW, bundleU8, nameU8, verU8);
            if (rc == 0) printf("已安装: %s (%s)\n", nameU8, bundleU8);
            else { fprintf(stderr, "安装失败 rc=%d\n", rc); return 3; }
            return 0;
        }
    }

    /* GUI 模式 */
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "无法打开 DISPLAY（无 X 环境）。"
                        "headless 模式: HOW --selftest / HOW --install x.hap\n");
        return 1;
    }
    g_scr = DefaultScreen(g_dpy);
    g_root = DefaultRootWindow(g_dpy);
    g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    g_wmDeleteProto = XInternAtom(g_dpy, "WM_PROTOCOLS", False);

    XWin *xw = win_new();
    if (!xw) return 1;
    xw->wpx = 760;
    xw->hpx = 560;
    wcscpy(xw->title, APP_TITLEW);
    xw->selList = -1;
    xw->hoverList = -1;
    xw->lastClickIdx = -1;

    int sw = DisplayWidth(g_dpy, g_scr), sh = DisplayHeight(g_dpy, g_scr);
    int x = (sw - 760) / 2, y = (sh - 560) / 2;
    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = 0xF0F0F0;
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | StructureNotifyMask;
    xw->w = XCreateWindow(g_dpy, g_root, x, y, 760, 560, 1,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWBackPixel | CWEventMask, &attr);
    {
        char t[400];
        w2u8(APP_TITLEW, t, 400);
        XStoreName(g_dpy, xw->w, t);
    }
    XSetWMProtocols(g_dpy, xw->w, &g_wmDelete, 1);
    XSizeHints *shints = XAllocSizeHints();
    shints->flags = PMinSize;
    shints->min_width = 560;
    shints->min_height = 430;
    XSetWMNormalHints(g_dpy, xw->w, shints);
    XFree(shints);
    refresh_list(xw);
    XMapWindow(g_dpy, xw->w);
    XFlush(g_dpy);

    event_loop();

    /* 清理全部窗口 */
    while (g_nwins > 0) win_destroy(g_wins[0]);
    XCloseDisplay(g_dpy);
    return 0;
}
