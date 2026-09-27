/* plat_linux.c - Linux/X11 GDI 子集 shim 实现（Xft 软渲染）
 *
 * 设计要点：
 *  - ui_render.c / ui_parse.c 的布局与绘制逻辑零改动复用（同 Windows 侧代码）
 *  - 文本：UCS-4（Linux wchar_t）经 UTF-8 转换后由 Xft 绘制，
 *          字体经 fontconfig 解析，优先 CJK 字体族（Noto Sans CJK SC 等）
 *  - DrawTextW 支持 DT_CALCRECT / DT_WORDBREAK / DT_SINGLELINE / DT_VCENTER /
 *          DT_CENTER / DT_RIGHT / DT_END_ELLIPSIS / DT_NOPREFIX（折行按词元：
 *          ASCII 连续段为词，CJK 逐字，与 ArkUI Text 竖排折行行为一致）
 *  - 矩形：XFillRectangle；圆角 = 主体矩形 + 四角四分之一 XFillArc
 */
#include "plat_linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 当前字体（SelectObject 无状态语义的替代：渲染前由 host 设置） */
static HFONT g_curFont;
HFONT plat_cur_font(void) { return g_curFont; }
void plat_set_cur_font(HFONT f) { g_curFont = f; }

/* ================= 编码转换（UTF-8 <-> UCS-4） ================= */

static int u8_decode(const char *s, unsigned *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
    if (!n) { *cp = 0xFFFD; return 1; }
    unsigned v = c & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = (v << 6) | (cc & 0x3F);
    }
    *cp = v;
    return n;
}

static int u8_encode(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

wchar_t *u8w(const char *u8, wchar_t *buf, int cap) {
    if (!u8 || cap <= 0) { if (buf && cap > 0) buf[0] = 0; return buf; }
    int o = 0;
    const char *p = u8;
    while (*p && o < cap - 1) {
        unsigned cp;
        int n = u8_decode(p, &cp);
        if (cp >= 0x110000) cp = 0xFFFD;
        buf[o++] = (wchar_t)cp;
        p += n ? n : 1;
    }
    buf[o] = 0;
    return buf;
}

char *w2u8(const wchar_t *w, char *buf, int cap) {
    if (!w || cap <= 0) { if (buf && cap > 0) buf[0] = 0; return buf; }
    int o = 0;
    for (int i = 0; w[i] && o < cap - 4; i++) {
        unsigned cp = (unsigned)w[i];
        o += u8_encode(cp, buf + o);
    }
    buf[o] = 0;
    return buf;
}

char *w2acp(const wchar_t *w, char *buf, int cap) {
    /* Linux 无 ANSI 代码页概念，文件系统编码即 UTF-8 */
    return w2u8(w, buf, cap);
}

/* ================= 工具 ================= */

DWORD GetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* ================= 字体 ================= */

/* CJK 优先字体族链：font_open 会逐一验证 charset 是否真含 CJK 字形 */
static const char *FONT_FAMILIES[] = {
    "Noto Sans CJK SC", "Noto Sans SC", "WenQuanYi Zen Hei",
    "WenQuanYi Micro Hei", "LXGW WenKai", "Noto Serif SC",
    "Source Han Sans SC", "Microsoft YaHei", "sans-serif",
};

HFONT CreateFontW(int height, int width, int esc, int orient, int weight,
                  int italic, int underline, int strike, int charset,
                  int outp, int clipp, int qual, int pitchfam, const wchar_t *face) {
    /* Win32 负 height = 字符 em 高；转正数像素尺寸 */
    int size = height < 0 ? -height : height;
    if (size < 6) size = 6;
    int bold = (weight >= FW_SEMIBOLD);

    HFONT f = (HFONT)calloc(1, sizeof(HOWFONT_));
    if (!f) return NULL;
    f->tag = PLAT_OBJ_FONT;
    f->size = size;
    f->bold = bold;
    f->dpy = NULL;      /* 由 plat_prepare_draw 在有 Display 时填充 */
    f->xf = NULL;
    return f;
}

/* 验证字体真含 CJK 字形（Xft 无 per-glyph fallback，选错族即豆腐块） */
static int font_has_cjk(Display *dpy, XftFont *f) {
    FcCharSet *cs = NULL;
    if (!f) return 0;
    if (FcPatternGetCharSet(f->pattern, FC_CHARSET, 0, &cs) != FcResultMatch)
        return 0;
    return FcCharSetHasChar(cs, 0x4E2D);   /* '中' */
}

/* 打开/绑定 Xft 字体：逐一尝试族链，选首个真含 CJK 的（幂等） */
static XftFont *font_open(Display *dpy, int screen, int size, int bold) {
    char spec[200];
    XftFont *latinOnly = NULL;
    for (unsigned i = 0; i < sizeof(FONT_FAMILIES) / sizeof(FONT_FAMILIES[0]); i++) {
        snprintf(spec, sizeof(spec), "%s-%d", FONT_FAMILIES[i], size);
        XftFont *f = NULL;
        if (bold) {
            char t[260];
            snprintf(t, sizeof(t), "%s:bold", spec);
            f = XftFontOpenName(dpy, screen, t);
        }
        if (!f) f = XftFontOpenName(dpy, screen, spec);
        if (!f) continue;
        if (font_has_cjk(dpy, f)) return f;   /* CJK 就绪，直接采用 */
        if (!latinOnly) latinOnly = f;        /* 记住首个拉丁字体作兜底 */
        else XftFontClose(dpy, f);
    }
    if (latinOnly) return latinOnly;
    return XftFontOpenName(dpy, screen, "sans-serif-12");
}

static int font_attach(HFONT f, Display *dpy, int screen) {
    if (!f) return 0;
    if (!f->xf) {
        f->dpy = dpy;
        f->xf = font_open(dpy, screen, f->size, f->bold);
        if (!f->xf) return 0;
    }
    return 1;
}

/* 内部释放（含真实 XftFont 关闭，供 host 层窗口销毁时调用） */
void plat_font_free(HFONT f) {
    if (!f) return;
    if (f->xf) XftFontClose(f->dpy, f->xf);
    free(f);
}

/* ================= 笔刷 / 对象 ================= */

HBRUSH CreateSolidBrush(COLORREF c) {
    HBRUSH b = (HBRUSH)malloc(sizeof(HOWBRUSH_));
    if (!b) return NULL;
    b->tag = PLAT_OBJ_BRUSH;
    b->dpy = NULL;
    /* COLORREF 0x00BBGGRR -> TrueColor X 像素 0xRRGGBB */
    b->pixel = ((unsigned long)GetRValue(c) << 16) |
               ((unsigned long)GetGValue(c) << 8) |
                (unsigned long)GetBValue(c);
    return b;
}

int DeleteObject(void *obj) {
    if (!obj) return 0;
    HFONT f = (HFONT)obj;
    if (f->tag == PLAT_OBJ_FONT) {
        if (f->xf && f->dpy) { XftFontClose(f->dpy, f->xf); f->xf = NULL; }
    }
    /* HBRUSH 是纯数据结构，直接释放；tag 校验兜底误传 */
    free(obj);
    return 1;
}

void *SelectObject(HDC hdc, void *obj) {
    /* shim 语义：
     *  - HFONT：设为当前字体（DrawTextW 取 g_curFont）；ui_render 的
     *    measure/draw 按节点字号反复切换，此路径必须真实生效。
     *  - HBRUSH：绑定到 DC（GDI 语义：之后的 Rectangle/RoundRect 用笔刷填充）。 */
    HFONT f = (HFONT)obj;
    if (hdc && f) {
        if (f->tag == PLAT_OBJ_FONT) {
            font_attach(f, hdc->dpy, DefaultScreen(hdc->dpy));
            plat_set_cur_font(f);
        } else if (f->tag == PLAT_OBJ_BRUSH) {
            hdc->curBrush = obj;
        }
    }
    return NULL;   /* 旧对象由调用方自行保存/恢复（与现有代码模式一致） */
}

HPEN GetStockObject(int which) {
    (void)which;
    return NULL;    /* NULL_PEN：X11 填充原语本就无边框 */
}

/* ================= 矩形绘制 ================= */

int FillRect(HDC hdc, const RECT *rc, HBRUSH br) {
    if (!hdc || !hdc->gc || !br) return 0;
    XSetForeground(hdc->dpy, hdc->gc, br->pixel);
    int w = (int)(rc->right - rc->left), h = (int)(rc->bottom - rc->top);
    if (w <= 0 || h <= 0) return 0;
    XFillRectangle(hdc->dpy, hdc->px ? hdc->px : hdc->win, hdc->gc,
                   (int)rc->left, (int)rc->top, (unsigned)w, (unsigned)h);
    return 1;
}

int Rectangle(HDC hdc, int l, int t, int r, int b) {
    if (!hdc || !hdc->gc) return 0;
    int w = r - l, h = b - t;
    if (w <= 0 || h <= 0) return 0;
    HBRUSH br = (HBRUSH)hdc->curBrush;
    unsigned long fill = br ? br->pixel : hdc->textColor;
    XSetForeground(hdc->dpy, hdc->gc, fill);
    XFillRectangle(hdc->dpy, hdc->px ? hdc->px : hdc->win, hdc->gc, l, t,
                   (unsigned)w, (unsigned)h);
    return 1;
}

int RoundRect(HDC hdc, int l, int t, int r, int b, int rw, int rh) {
    if (!hdc || !hdc->gc) return 0;
    int w = r - l, h = b - t;
    if (w <= 0 || h <= 0) return 0;
    Drawable d = hdc->px ? hdc->px : hdc->win;
    int rad = (rw < rh ? rw : rh) / 2;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    HBRUSH br = (HBRUSH)hdc->curBrush;
    unsigned long fill = br ? br->pixel : hdc->textColor;
    XSetForeground(hdc->dpy, hdc->gc, fill);
    if (rad <= 0) {
        XFillRectangle(hdc->dpy, d, hdc->gc, l, t, (unsigned)w, (unsigned)h);
        return 1;
    }
    /* 中心十字矩形 + 四角四分之一圆弧 */
    XFillRectangle(hdc->dpy, d, hdc->gc, l + rad, t, (unsigned)(w - 2 * rad), (unsigned)h);
    XFillRectangle(hdc->dpy, d, hdc->gc, l, t + rad, (unsigned)rad, (unsigned)(h - 2 * rad));
    XFillRectangle(hdc->dpy, d, hdc->gc, r - rad, t + rad, (unsigned)rad, (unsigned)(h - 2 * rad));
    int aw = rad * 360 * 64, start;
    start = 90 * 64;  XFillArc(hdc->dpy, d, hdc->gc, l, t, 2 * rad, 2 * rad, start, aw);
    start = 0 * 64;   XFillArc(hdc->dpy, d, hdc->gc, r - 2 * rad, t, 2 * rad, 2 * rad, start, aw);
    start = 270 * 64; XFillArc(hdc->dpy, d, hdc->gc, l, b - 2 * rad, 2 * rad, 2 * rad, start, aw);
    start = 180 * 64; XFillArc(hdc->dpy, d, hdc->gc, r - 2 * rad, b - 2 * rad, 2 * rad, 2 * rad, start, aw);
    return 1;
}

COLORREF SetTextColor(HDC hdc, COLORREF c) {
    if (!hdc) return 0;
    COLORREF old = RGB((hdc->textColor >> 16) & 0xFF, (hdc->textColor >> 8) & 0xFF,
                       hdc->textColor & 0xFF);
    hdc->textColor = ((unsigned long)GetBValue(c) << 16) |
                     ((unsigned long)GetGValue(c) << 8) |
                      (unsigned long)GetRValue(c);
    return old;
}

int SetBkMode(HDC hdc, int mode) {
    if (!hdc) return 0;
    int old = hdc->bkMode;
    hdc->bkMode = mode;
    return old;
}

/* ================= 文本测量与绘制（DrawTextW） ================= */

/* 词元类型：ASCII 连续段为一个词元，CJK 等宽字符逐字成元 */
typedef struct { const wchar_t *s; int len; } Token;

static int next_token(const wchar_t *s, int i, int len, Token *tk) {
    if (i >= len) return 0;
    wchar_t c = s[i];
    int ascii = (c > 0 && c < 0x80 && c != ' ' && c != '\n');
    tk->s = s + i;
    tk->len = 0;
    if (ascii) {
        int j = i;
        while (j < len && s[j] > 0 && s[j] < 0x80 && s[j] != ' ' && s[j] != '\n') j++;
        tk->len = j - i;
    } else {
        tk->len = 1;   /* 空格/CJK/其他逐字 */
    }
    return 1;
}

static void tok_utf8(const wchar_t *s, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && o < cap - 5; i++) o += u8_encode((unsigned)s[i], out + o);
    out[o] = 0;
}

/* Xft 测量词元宽度（缓存常用字符可再加，当前规模足够） */
static int tok_width(HDC hdc, XftFont *f, const wchar_t *s, int len) {
    char u8[640];
    tok_utf8(s, len, u8, sizeof(u8));
    XGlyphInfo ext;
    XftTextExtentsUtf8(hdc->dpy, f, (FcChar8 *)u8, (int)strlen(u8), &ext);
    return ext.xOff ? ext.xOff : ext.width;
}

/* 行内逐词元折行：返回该文本在 maxW 内的行数组与总高。
 * 行结构由调用方缓冲传入（行数上限 linesCap）。 */
typedef struct { int start, len, w; } TLine;

static int layout_lines(HDC hdc, XftFont *f, const wchar_t *text, int len,
                        int maxW, TLine *lines, int linesCap) {
    int nl = 0, i = 0;
    int lineStart = 0, lineW = 0, lastSpace = -1, lastSpaceW = 0;
    while (i < len) {
        wchar_t c = text[i];
        if (c == L'\n') {
            if (nl < linesCap) { lines[nl].start = lineStart; lines[nl].len = i - lineStart; lines[nl].w = lineW; nl++; }
            i++; lineStart = i; lineW = 0; lastSpace = -1;
            continue;
        }
        Token tk;
        next_token(text, i, len, &tk);
        int tw = tok_width(hdc, f, tk.s, tk.len);
        if (c == L' ') { lastSpace = i; lastSpaceW = lineW; }
        if (lineW + tw > maxW && i > lineStart) {
            /* 需要换行 */
            int brk = i, w = lineW;
            if (lastSpace > lineStart) { brk = lastSpace + 1; w = lastSpaceW; }
            if (nl < linesCap) { lines[nl].start = lineStart; lines[nl].len = brk - lineStart; lines[nl].w = w; nl++; }
            lineStart = brk;
            lineW = tok_width(hdc, f, text + lineStart, i - lineStart) + tw; /* 近似：当前词放入新行 */
            lastSpace = -1;
        } else {
            lineW += tw;
        }
        i += tk.len;
    }
    if (nl < linesCap) { lines[nl].start = lineStart; lines[nl].len = len - lineStart; lines[nl].w = lineW; nl++; }
    return nl;
}

static void draw_line(HDC hdc, XftFont *f, const wchar_t *text, TLine *ln,
                      int x, int y, int maxW, UINT flags) {
    XftColor col;
    XRenderColor rc;
    rc.red   = (unsigned short)(((hdc->textColor >> 16) & 0xFF) * 0x101);
    rc.green = (unsigned short)(((hdc->textColor >> 8) & 0xFF) * 0x101);
    rc.blue  = (unsigned short)(((hdc->textColor) & 0xFF) * 0x101);
    rc.alpha = 0xFFFF;
    XftColorAllocValue(hdc->dpy, DefaultVisual(hdc->dpy, 0),
                       DefaultColormap(hdc->dpy, 0), &rc, &col);
    int tx = x;
    wchar_t buf[640];
    int drawLen = ln->len;
    if (flags & DT_END_ELLIPSIS && ln->w > maxW && drawLen > 1) {
        /* 截断加省略号 */
        int ow = 0, keep = 0;
        int ews = tok_width(hdc, f, L"...", 3);
        while (keep < drawLen) {
            Token tk;
            next_token(text + ln->start, keep, drawLen, &tk);
            int tw = tok_width(hdc, f, text + ln->start + keep, tk.len);
            if (ow + tw + ews > maxW) break;
            ow += tw; keep += tk.len;
        }
        drawLen = keep;
        for (int i = 0; i < drawLen && i < 600; i++) buf[i] = text[ln->start + i];
        buf[drawLen++] = '.'; buf[drawLen++] = '.'; buf[drawLen++] = '.';
        buf[drawLen] = 0;
    } else {
        for (int i = 0; i < drawLen && i < 639; i++) buf[i] = text[ln->start + i];
        buf[drawLen] = 0;
    }
    if (flags & DT_CENTER) tx = x + (maxW - ln->w) / 2;
    else if (flags & DT_RIGHT) tx = x + maxW - ln->w;
    if (tx < x) tx = x;
    char u8[1400];
    tok_utf8(buf, drawLen, u8, sizeof(u8));
    if (hdc->draw)
        XftDrawStringUtf8(hdc->draw, &col, f, tx, y, (FcChar8 *)u8, (int)strlen(u8));
    XftColorFree(hdc->dpy, DefaultVisual(hdc->dpy, 0), DefaultColormap(hdc->dpy, 0), &col);
}

int DrawTextW(HDC hdc, const wchar_t *text, int len, RECT *r, UINT flags) {
    if (!hdc || !r || !text) return 0;
    if (len < 0) len = (int)wcslen(text);
    if (!hdc->dpy) return 0;
    /* 字体由 SelectObject 语义约定为最近创建——shim 下用全局当前字体 */
    extern HFONT plat_cur_font(void);
    HFONT cur = plat_cur_font();
    if (!cur || cur->tag != PLAT_OBJ_FONT || !cur->xf) return 0;
    XftFont *f = cur->xf;
    int maxW = (int)(r->right - r->left);
    if (maxW < 8) maxW = 8;
    int lh = f->ascent + f->descent;

    if (flags & DT_SINGLELINE) {
        Token tk0;
        int w = tok_width(hdc, f, text, len);
        (void)tk0;
        int x = (int)r->left, ty = (int)r->top;
        if (flags & (DT_VCENTER)) ty = (int)r->top + ((int)(r->bottom - r->top) - lh) / 2 + f->ascent;
        else ty = (int)r->top + f->ascent;
        TLine ln = { 0, len, w };
        if (flags & DT_END_ELLIPSIS && w > maxW) {
            /* 截断 */
            RECT rr = { r->left, ty - f->ascent, r->right, ty - f->ascent + lh };
            TLine full = { 0, len, w };
            draw_line(hdc, f, text, &full, (int)r->left, ty, maxW, flags | DT_NOPREFIX);
            (void)rr;
            return lh;
        }
        if (flags & DT_CENTER) x = (int)r->left + (maxW - w) / 2;
        else if (flags & DT_RIGHT) x = (int)r->left + maxW - w;
        draw_line(hdc, f, text, &ln, x, ty, maxW, flags | DT_NOPREFIX);
        return lh;
    }

    /* 多行折行 */
    TLine lines[64];
    int nl = layout_lines(hdc, f, text, len, maxW, lines, 64);
    int totalH = nl * lh;
    if (flags & DT_CALCRECT) {
        r->right = r->left;
        for (int i = 0; i < nl; i++)
            if (lines[i].w > r->right - r->left) r->right = r->left + lines[i].w;
        r->bottom = r->top + totalH;
        return totalH;
    }
    int y = (int)r->top;
    for (int i = 0; i < nl; i++) {
        draw_line(hdc, f, text, &lines[i], (int)r->left, y + f->ascent, maxW, flags);
        y += lh;
    }
    return totalH;
}

/* 当前字体定义移至文件头部（DrawTextW 需要前向引用） */

/* host 层工具：为 DC 上的字体绑定 Display 并设为当前字体 */
void plat_prepare_draw(HDC hdc, HFONT f) {
    if (!hdc || !f) return;
    font_attach(f, hdc->dpy, DefaultScreen(hdc->dpy));
    plat_set_cur_font(f);
}

/* 测量专用：文本宽度（不含对齐逻辑） */
int plat_text_width(Display *dpy, HFONT f, const wchar_t *s) {
    if (!dpy || !f) return 0;
    font_attach(f, dpy, DefaultScreen(dpy));
    if (!f->xf) return 0;
    char u8[1024];
    int n = (int)wcslen(s);
    int o = 0;
    for (int i = 0; i < n && o < 1000; i++) o += u8_encode((unsigned)s[i], u8 + o);
    u8[o] = 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(dpy, f->xf, (FcChar8 *)u8, o, &ext);
    return ext.xOff ? ext.xOff : ext.width;
}

int plat_font_height(HFONT f) {
    return f && f->xf ? f->xf->ascent + f->xf->descent : 16;
}
