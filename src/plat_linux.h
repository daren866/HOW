/* plat_linux.h - Linux/X11 平台 shim：提供 how.h 所需的 Win32 GDI 子集
 *
 * 目标：ui_parse.c / ui_render.c（布局与绘制引擎）在 Linux 上零改动复用。
 * 本头只在非 _WIN32 构建时被 how.h 引入，Windows 侧完全不受影响。
 *
 * 覆盖的 Win32 子集：
 *   类型:  COLORREF RECT POINT HBRUSH HPEN DWORD
 *          HFONT  -> Xft 字体包装（含尺寸/粗细）
 *          HDC    -> XftDraw + Pixmap 双缓冲绘图面（成员公开供 host_linux.c 构造）
 *   常量:  RGB PtInRect NULL_PEN TRANSPARENT DT_* FW_*
 *   函数:  CreateFontW SelectObject DeleteObject GetStockObject
 *          CreateSolidBrush FillRect Rectangle RoundRect
 *          SetTextColor SetBkMode DrawTextW GetTickCount
 *   编码:  u8w / w2u8 / w2acp（UTF-8 <-> UCS-4；Linux wchar_t 为 32 位）
 */
#ifndef HOW_PLAT_LINUX_H
#define HOW_PLAT_LINUX_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlocale.h>
#include <X11/Xft/Xft.h>

#include <stdint.h>
#include <wchar.h>
#include <stdarg.h>

typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef unsigned long COLORREF;   /* 0x00BBGGRR，同 Win32 */
typedef void *HINSTANCE;          /* 仅满足 how.h 声明；Linux 侧不使用 */

typedef struct { long left, top, right, bottom; } RECT;
typedef struct { long x, y; } POINT;

/* ---- 字体 ---- */
typedef struct HOWFONT_ {
    int tag;       /* PLAT_OBJ_FONT */
    Display *dpy;
    XftFont *xf;
    int size;      /* 像素字高（正数语义，对应 CreateFontW 的 -height） */
    int bold;
} HOWFONT_;
typedef struct HOWFONT_ *HFONT;

/* ---- 笔刷 ---- */
typedef struct HOWBRUSH_ {
    int tag;       /* PLAT_OBJ_BRUSH */
    Display *dpy;
    unsigned long pixel;    /* X11 像素值 */
} HOWBRUSH_;
typedef struct HOWBRUSH_ *HBRUSH;
typedef void *HPEN;

#define PLAT_OBJ_FONT  0x464F4E54u   /* 'FONT' */
#define PLAT_OBJ_BRUSH 0x42525348u   /* 'BRSH' */

/* ---- 绘图面 ---- */
typedef struct HOWDC_ {
    Display  *dpy;
    XftDraw  *draw;      /* 绘制目标（可 NULL：纯测量 DC） */
    GC        gc;        /* 矩形填充用 */
    Pixmap    px;        /* 0 = 无离屏位图（测量/屏幕直绘） */
    Window    win;       /* 0 = 非窗口直绘 */
    int       w, h;
    unsigned long textColor;
    int       bkMode;
    void     *curBrush;  /* SelectObject(brush) 绑定的当前笔刷（GDI Rectangle 语义） */
} HOWDC_;
typedef struct HOWDC_ *HDC;

/* ---- 常量 ---- */
#define RGB(r,g,b) ((COLORREF)(((uint8_t)(r)) | (((uint32_t)(uint8_t)(g)) << 8) | (((uint32_t)(uint8_t)(b)) << 16)))
#define GetRValue(c)  ((int)(((c))      & 0xFF))
#define GetGValue(c)  ((int)((((c))>>8) & 0xFF))
#define GetBValue(c)  ((int)((((c))>>16)& 0xFF))

#define NULL_PEN   5    /* Win32 stock 对象 ID 语义 */
#define TRANSPARENT 1
#define OPAQUE      2

#define DT_TOP            0x00000000
#define DT_LEFT           0x00000000
#define DT_CENTER         0x00000001
#define DT_RIGHT          0x00000002
#define DT_VCENTER        0x00000004
#define DT_SINGLELINE     0x00000020
#define DT_NOPREFIX       0x00000800
#define DT_WORDBREAK      0x00000010
#define DT_END_ELLIPSIS   0x00008000
#define DT_CALCRECT       0x00000400
#define DT_EXTERNALLEADING 0x00000200

#define FW_NORMAL    400
#define FW_SEMIBOLD  600
#define FW_BOLD      700

#define DEFAULT_CHARSET      0
#define OUT_DEFAULT_PRECIS   0
#define CLIP_DEFAULT_PRECIS  0
#define CLEARTYPE_QUALITY    5
#define DEFAULT_PITCH        0
#define FF_DONTCARE          0

/* glibc 无 _snwprintf；swprintf(buf,cap,fmt,...) 签名等价 */
#define _snwprintf swprintf

/* ---- PtInRect（Win32 语义：左/上含，右/下不含） ---- */
static inline int PtInRect(const RECT *rc, POINT pt) {
    return pt.x >= rc->left && pt.x < rc->right && pt.y >= rc->top && pt.y < rc->bottom;
}

/* ---- GDI 函数 ---- */
HFONT   CreateFontW(int height, int width, int esc, int orient, int weight,
                    int italic, int underline, int strike, int charset,
                    int outp, int clipp, int qual, int pitchfam, const wchar_t *face);
HBRUSH  CreateSolidBrush(COLORREF c);
void   *SelectObject(HDC hdc, void *obj);   /* HFONT/HBRUSH 通用；旧对象由调用方保存 */
int     DeleteObject(void *obj);
HPEN    GetStockObject(int which);
int     FillRect(HDC hdc, const RECT *rc, HBRUSH br);
int     Rectangle(HDC hdc, int l, int t, int r, int b);
int     RoundRect(HDC hdc, int l, int t, int r, int b, int rw, int rh);
COLORREF SetTextColor(HDC hdc, COLORREF c);
int     SetBkMode(HDC hdc, int mode);
int     DrawTextW(HDC hdc, const wchar_t *text, int len, RECT *r, UINT flags);

/* ---- 工具 ---- */
DWORD GetTickCount(void);   /* 毫秒（monotonic） */

/* 与 Windows 侧 how.h 相同签名的编码转换（实现在 plat_linux.c） */
wchar_t *u8w(const char *u8, wchar_t *buf, int cap);
char    *w2u8(const wchar_t *w, char *buf, int cap);
char    *w2acp(const wchar_t *w, char *buf, int cap);

/* ---- host 层扩展接口 ---- */
void plat_set_cur_font(HFONT f);             /* 渲染前设当前字体（替代 SelectObject） */
void plat_prepare_draw(HDC hdc, HFONT f);    /* 绑定 Display 并设当前字体 */
int  plat_text_width(Display *dpy, HFONT f, const wchar_t *s);  /* 单行宽度测量 */
int  plat_font_height(HFONT f);              /* 行高 */

#endif /* HOW_PLAT_LINUX_H */
