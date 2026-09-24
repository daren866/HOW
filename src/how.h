/* how.h - HOW 宿主程序公共头（Windows 层） */
#ifndef HOW_H
#define HOW_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "howcore.h"

#define APP_TITLEW L"HOW - x64\u8f6c\u8bd1arm\u6a21\u5f0f"   /* HOW - x64转译arm模式 */
#define BTN_HAPW   L"\u5b89\u88c5hap"                        /* 安装hap */

/* UTF-8 <-> UTF-16 转换（调用者提供缓冲） */
wchar_t *u8w(const char *u8, wchar_t *buf, int cap);
char    *w2u8(const wchar_t *w, char *buf, int cap);
char    *w2acp(const wchar_t *w, char *buf, int cap);   /* ANSI 代码页（miniz 文件名用） */

/* ---------------- HAP 包与已装应用存储 ---------------- */
typedef struct {
    wchar_t name[128];      /* 显示名称 */
    wchar_t bundle[128];    /* bundleName */
    wchar_t version[64];
    char    dir[128];       /* 安装子目录名（bundle 的安全形式） */
} AppInfo;

void     store_apps_dir(wchar_t *out, int cap);
int      store_list(AppInfo *arr, int max);
int      hap_probe(const wchar_t *hapPath, char *nameU8, int nameCap,
                   char *bundleU8, int bCap, char *verU8, int vCap);
int      store_install(const wchar_t *hapPath, const char *bundleU8,
                       const char *nameU8, const char *verU8);
void     store_dir_from_bundle(const char *bundleU8, char *out, int cap);

/* ---------------- UI 模式（ArkUI 声明式描述子集） ---------------- */
typedef enum { ACT_NONE, ACT_INC, ACT_SET, ACT_VMCALL, ACT_TOAST } ActType;

typedef struct {
    ActType type;
    char    var[64];
    double  delta, value;
    char    chunk[64];
    char    msg[256];
} Action;

typedef struct UINode UINode;
struct UINode {
    char type[16];          /* column row text button spacer image */
    int  hasW, w, wPct;
    int  hasH, h, hPct;
    int  pad, margin, space, radius, fontSize, bold, textAlign;
    int  hasBg;
    COLORREF bg, fg;
    char justify[16], align[16];
    char text[512], var[64];
    Action act;
    UINode **ch;
    int  nch;
    int  id;                /* 渲染矩形索引 */
};

typedef struct RtState RtState;

struct RtState {
    UINode *root;
    Var vars[24];
    int nvars;
    AbcInfo abc;
    wchar_t toast[256];
    int toastOn;
    DWORD toastTick;
    wchar_t *log;
    int logLen, logCap;
    HFONT fonts[24];
    int nfonts;
    RECT *rects;
    int nrects;
};

RtState *rt_create(void);
void     rt_destroy(RtState *rt);
void     free_node(UINode *nd);
int      rt_load_ui(RtState *rt, const char *jsonU8);       /* 返回节点数 */
int      rt_load_abc(RtState *rt, const char *pathU8);      /* 返回 1 成功 */
void     rt_log(RtState *rt, const char *fmtU8, ...);
void     rt_seed_state(RtState *rt, JV *pageObj);
void     ui_click(RtState *rt, int x, int y);               /* 命中并执行动作 */
void     ui_render(RtState *rt, HDC hdc, RECT rcPage);      /* GDI 双缓冲软渲染 */
HFONT    rt_getfont(RtState *rt, int size, int bold);
const wchar_t *rt_logbuf(RtState *rt);
void     rt_set_toast(RtState *rt, const char *msgU8);

/* ---------------- 兼容层窗口 ---------------- */
void compat_register(HINSTANCE hInst);
void compat_open(HINSTANCE hInst, AppInfo *app);

#endif
