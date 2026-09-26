/* ui_render.c - HOW Runtime 渲染层
 * 两遍式 Flexbox 简化布局（measure/arrange）+ GDI 双缓冲软绘制 +
 * 命中测试与动作执行（inc/set/toast/vmcall）。
 */
#include "how.h"

static RtState *g_rt; /* 渲染会话指针（本程序单线程消息循环，无竞争） */

/* ---------------- .abc 载入 ---------------- */
int rt_load_abc(RtState *rt, const char *pathU8) {
    if (!abc_load(pathU8, &rt->abc)) {
        rt_log(rt, "[abc] %s 载入失败（非 PACA 容器或不存在）", pathU8);
        return 0;
    }
    rt_log(rt, "[abc] magic=%s version=%u.%u.%u.%u size=%ldB 字符串=%d",
           rt->abc.magic, rt->abc.ver[0], rt->abc.ver[1], rt->abc.ver[2],
           rt->abc.ver[3], rt->abc.size, rt->abc.nstr);
    /* 打印字符串池样本（最多 6 条） */
    {
        char pool[512];
        int off = 0;
        for (int i = 0; i < rt->abc.nstr && off < 480; i++) {
            int w = snprintf(pool + off, sizeof(pool) - (size_t)off, "%s%s",
                             i ? ", " : "", rt->abc.strings[i]);
            if (w < 0) break;
            off += w;
        }
        rt_log(rt, "[abc] 池样本: %s", pool);
    }
    rt_log(rt, "[vm] HOWRT 代码块 %d 个", rt->abc.nchunks);
    for (int i = 0; i < rt->abc.nchunks; i++) {
        char gs[256];
        int off = 0;
        for (int g = 0; g < rt->abc.chunks[i].ng && off < 230; g++)
            off += snprintf(gs + off, sizeof(gs) - (size_t)off, "%s%s",
                            g ? "," : "", rt->abc.chunks[i].gname[g]);
        rt_log(rt, "[vm] chunk '%s' bytes=%d globals=[%s]",
               rt->abc.chunks[i].name, rt->abc.chunks[i].clen, gs);
    }
    return 1;
}

/* ---------------- 状态变量与动作 ---------------- */
static void fmt_var(char *out, size_t cap, Var *v) {
    if (v->isNum) snprintf(out, cap, "%lld", (long long)v->num);
    else snprintf(out, cap, "%s", v->str);
}

/* 将 {{var}} 替换为状态值 */
static void expand_text(RtState *rt, const char *src, wchar_t *out, int cap) {
    char tmp[512];
    int o = 0;
    for (int i = 0; src[i] && o < 500;) {
        if (src[i] == '{' && src[i + 1] == '{') {
            const char *e = strstr(src + i + 2, "}}");
            if (e) {
                char name[64];
                size_t L = (size_t)(e - (src + i + 2));
                if (L >= sizeof(name)) L = sizeof(name) - 1;
                memcpy(name, src + i + 2, L);
                name[L] = 0;
                int vi = var_find(rt->vars, rt->nvars, name);
                char val[192] = "";
                if (vi >= 0) fmt_var(val, sizeof(val), &rt->vars[vi]);
                int w = snprintf(tmp + o, (size_t)(504 - o), "%s", val);
                if (w < 0) break;
                o += w;
                i = (int)(e - src) + 2;
                continue;
            }
        }
        tmp[o++] = src[i++];
    }
    tmp[o] = 0;
    u8w(tmp, out, cap);
}

static void exec_action(RtState *rt, Action *act) {
    switch (act->type) {
    case ACT_INC: {
        Var *v = var_ensure(rt->vars, &rt->nvars, 24, act->var);
        if (v) { v->isNum = 1; v->num += act->delta; }
        rt_log(rt, "[action] inc %s -> %lld", act->var,
               v ? (long long)v->num : 0);
        break;
    }
    case ACT_SET: {
        Var *v = var_ensure(rt->vars, &rt->nvars, 24, act->var);
        if (v) { v->isNum = 1; v->num = act->value; }
        rt_log(rt, "[action] set %s -> %lld", act->var,
               v ? (long long)v->num : 0);
        break;
    }
    case ACT_VMCALL: {
        VmChunk *ck = NULL;
        for (int i = 0; i < rt->abc.nchunks; i++)
            if (strcmp(rt->abc.chunks[i].name, act->chunk) == 0) { ck = &rt->abc.chunks[i]; break; }
        if (!ck) { rt_log(rt, "[vm] chunk '%s' 不存在", act->chunk); break; }
        int rc = vm_run(ck, rt->vars, &rt->nvars);
        Var *v = var_find(rt->vars, rt->nvars, ck->ng ? ck->gname[0] : "") >= 0
                     ? &rt->vars[var_find(rt->vars, rt->nvars, ck->gname[0])] : NULL;
        rt_log(rt, "[vm] chunk '%s' 执行 rc=%d %s=%lld", ck->name, rc,
               ck->ng ? ck->gname[0] : "", v ? (long long)v->num : 0);
        break;
    }
    case ACT_TOAST:
        rt_set_toast(rt, act->msg);
        rt_log(rt, "[action] toast: %s", act->msg);
        break;
    default: break;
    }
}

void ui_click(RtState *rt, int x, int y) {
    if (!rt->root) return;
    g_rt = rt;
    POINT pt;
    pt.x = x; pt.y = y;
    /* 反向后序近似取最上层可点击节点 */
    for (int id = rt->nrects - 1; id >= 0; id--) {
        /* id 与节点序号一致；查找拥有该 id 的节点 */
        UINode *found = NULL;
        /* 深度遍历查找（节点数 <= 256，成本可忽略） */
        static UINode *stk[512];
        int sp = 0;
        stk[sp++] = rt->root;
        while (sp > 0) {
            UINode *n = stk[--sp];
            if (n->id == id) { found = n; break; }
            for (int i = n->nch - 1; i >= 0 && sp < 511; i++) stk[sp++] = n->ch[i];
        }
        if (found && found->act.type != ACT_NONE &&
            PtInRect(&rt->rects[id], pt)) {
            exec_action(rt, &found->act);
            return;
        }
    }
}

/* ---------------- 布局 ---------------- */
typedef struct { int w, h; } Sz;

static void measure(UINode *nd, int availW, HDC hdc, Sz *out);

static HFONT font_for(UINode *nd) {
    return rt_getfont(g_rt, nd->fontSize, nd->bold);
}

static void text_sz(UINode *nd, int availW, HDC hdc, Sz *out) {
    wchar_t wtext[512];
    expand_text(g_rt, nd->text, wtext, 512);
    RECT r = {0, 0, availW, 0};
    HFONT of = (HFONT)SelectObject(hdc, font_for(nd));
    DrawTextW(hdc, wtext, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, of);
    out->w = r.right;
    out->h = r.bottom + 4;
}

static void measure(UINode *nd, int availW, HDC hdc, Sz *out) {
    int m2 = nd->margin * 2, p2 = nd->pad * 2;
    int innerW = availW - m2 - p2;
    if (innerW < 8) innerW = 8;
    Sz s = {0, 0};
    if (strcmp(nd->type, "text") == 0) {
        text_sz(nd, innerW, hdc, &s);
    } else if (strcmp(nd->type, "button") == 0) {
        Sz ts;
        text_sz(nd, innerW - 40, hdc, &ts);
        s.w = ts.w + 36;
        s.h = ts.h + 16;
    } else if (strcmp(nd->type, "spacer") == 0) {
        s.w = 0;
        s.h = nd->hasH == 1 ? nd->h : 8;
    } else if (strcmp(nd->type, "image") == 0) {
        s.w = nd->hasW == 1 ? nd->w : 64;
        s.h = nd->hasH == 1 ? nd->h : 64;
    } else { /* column / row / 容器 */
        int isCol = strcmp(nd->type, "column") == 0;
        int mainSum = 0, crossMax = 0, gap = nd->space;
        for (int i = 0; i < nd->nch; i++) {
            Sz cs;
            measure(nd->ch[i], innerW, hdc, &cs);
            if (isCol) { mainSum += cs.h; if (cs.w > crossMax) crossMax = cs.w; }
            else { mainSum += cs.w; if (cs.h > crossMax) crossMax = cs.h; }
        }
        if (nd->nch > 1) mainSum += gap * (nd->nch - 1);
        s.w = crossMax + p2;
        s.h = mainSum + p2;
    }
    /* 显式尺寸覆盖 */
    if (nd->hasW == 1 && nd->w > 0) s.w = nd->w;
    else if (nd->hasW == 2) s.w = innerW * nd->wPct / 100;
    if (nd->hasH == 1 && nd->h > 0) s.h = nd->h;
    else if (nd->hasH == 2 && availW > 0) s.h = availW * nd->hPct / 100; /* 近似 */
    out->w = s.w + m2;
    out->h = s.h + m2;
}

static int is_col(UINode *nd) { return strcmp(nd->type, "column") != 0 ? 0 : 1; }

static void arrange(UINode *nd, int x, int y, int w, int h, HDC hdc) {
    RECT *r = &g_rt->rects[nd->id];
    r->left = x + nd->margin;
    r->top = y + nd->margin;
    r->right = r->left + w;
    r->bottom = r->top + h;
    int p = nd->pad;
    int ix = r->left + p, iy = r->top + p;
    int iw = (r->right - r->left) - p * 2;
    int ih = (r->bottom - r->top) - p * 2;
    if (iw < 0) iw = 0;
    if (ih < 0) ih = 0;

    /* 先测每个孩子的实际占用 */
    Sz cs[32];
    int n = nd->nch > 32 ? 32 : nd->nch;
    int total = 0;
    for (int i = 0; i < n; i++) {
        measure(nd->ch[i], iw, hdc, &cs[i]);
        total += is_col(nd) ? cs[i].h : cs[i].w;
    }
    if (n > 1) total += nd->space * (n - 1);

    int col = is_col(nd);
    int axis = col ? ih : iw;
    int pos = 0, between = 0;
    if (strcmp(nd->justify, "center") == 0) { pos = (axis - total) / 2; if (pos < 0) pos = 0; }
    else if (strcmp(nd->justify, "end") == 0) { pos = axis - total; if (pos < 0) pos = 0; }
    else if (strcmp(nd->justify, "space-between") == 0 && n > 1) {
        between = (axis - total) / (n - 1);
        if (between < 0) between = 0;
    }

    for (int i = 0; i < n; i++) {
        UINode *c = nd->ch[i];
        int cw = cs[i].w, chh = cs[i].h;
        /* 主轴排布 */
        int mpos = pos;
        pos += (col ? chh : cw) + nd->space + between;
        /* 交叉轴对齐（默认 center，同 ArkUI Column/Row 语义） */
        int cross = col ? iw : ih;
        int cpos = 0;
        int stretch = 0;
        if (strcmp(c->align, "start") == 0) cpos = 0;
        else if (strcmp(c->align, "end") == 0) cpos = cross - (col ? cw : chh);
        else if (strcmp(c->align, "stretch") == 0) stretch = 1;
        else cpos = (cross - (col ? cw : chh)) / 2;
        if (stretch) {
            if (col) cw = iw; else chh = ih;
            cpos = 0;
        }
        /* 文本节点默认占满整行宽度，便于换行与 textAlign 生效 */
        if (col && strcmp(c->type, "text") == 0 && !c->hasW) {
            cw = iw;
            cpos = 0;
        }
        if (c->hasW == 2) cw = iw * c->wPct / 100;
        if (col) arrange(c, ix + cpos, iy + mpos, cw, chh, hdc);
        else arrange(c, ix + mpos, iy + cpos, cw, chh, hdc);
    }
}

/* ---------------- 绘制 ---------------- */
static void fill_round(HDC hdc, RECT *r, int radius, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    HPEN pen = (HPEN)GetStockObject(NULL_PEN);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    if (radius > 0) RoundRect(hdc, r->left, r->top, r->right + 1, r->bottom + 1, radius * 2, radius * 2);
    else Rectangle(hdc, r->left, r->top, r->right + 1, r->bottom + 1);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
}

static void draw_text_vcenter(HDC hdc, UINode *nd, RECT *r) {
    wchar_t wtext[512];
    expand_text(g_rt, nd->text, wtext, 512);
    HFONT of = (HFONT)SelectObject(hdc, font_for(nd));
    SetTextColor(hdc, nd->fg);
    SetBkMode(hdc, TRANSPARENT);
    UINT flags = DT_NOPREFIX | DT_EXTERNALLEADING |
                 (nd->textAlign ? DT_CENTER : DT_LEFT);
    RECT calc = *r;
    DrawTextW(hdc, wtext, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int th = calc.bottom - calc.top;
    RECT draw = *r;
    int off = ((r->bottom - r->top) - th) / 2;
    if (off > 0) { draw.top += off; draw.bottom = draw.top + th; }
    DrawTextW(hdc, wtext, -1, &draw, flags | DT_WORDBREAK);
    SelectObject(hdc, of);
}

static void draw_node(UINode *nd, HDC hdc) {
    RECT *r = &g_rt->rects[nd->id];
    if (r->right <= r->left || r->bottom <= r->top) return;
    if (nd->hasBg) fill_round(hdc, r, nd->radius, nd->bg);
    if (nd->text[0] || strcmp(nd->type, "button") == 0)
        draw_text_vcenter(hdc, nd, r);
    for (int i = 0; i < nd->nch; i++) draw_node(nd->ch[i], hdc);
}

void ui_render(RtState *rt, HDC hdc, RECT rcPage) {
    g_rt = rt;
    if (rt->root) {
        /* 页面底色 */
        COLORREF pageBg = rt->root->hasBg ? rt->root->bg : RGB(0xF1, 0xF3, 0xF5);
        HBRUSH br = CreateSolidBrush(pageBg);
        FillRect(hdc, &rcPage, br);
        DeleteObject(br);
        arrange(rt->root, rcPage.left, rcPage.top,
                rcPage.right - rcPage.left, rcPage.bottom - rcPage.top, hdc);
        draw_node(rt->root, hdc);
    } else {
        /* 页面未加载兜底：干净底色 + 居中提示（绝不留空/噪点） */
        HBRUSH br = CreateSolidBrush(RGB(0xF1, 0xF3, 0xF5));
        FillRect(hdc, &rcPage, br);
        DeleteObject(br);
        HFONT of = (HFONT)SelectObject(hdc, rt_getfont(rt, 14, 0));
        SetTextColor(hdc, RGB(0x99, 0xA0, 0xA8));
        SetBkMode(hdc, TRANSPARENT);
        wchar_t hint[128];
        u8w("页面未加载 · 未找到 pages/index.json 或解析失败", hint, 128);
        DrawTextW(hdc, hint, -1, &rcPage,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(hdc, of);
    }
    /* Toast 悬浮层 */
    if (rt->toastOn && GetTickCount() - rt->toastTick < 2000) {
        int tw = (int)wcslen(rt->toast) * 9 + 48;
        int pw = rcPage.right - rcPage.left;
        if (tw > pw - 40) tw = pw - 40;
        RECT tr;
        tr.left = rcPage.left + (pw - tw) / 2;
        tr.right = tr.left + tw;
        tr.top = rcPage.bottom - 52;
        tr.bottom = tr.top + 36;
        fill_round(hdc, &tr, 8, RGB(0x3A, 0x3F, 0x45));
        HFONT of = (HFONT)SelectObject(hdc, rt_getfont(rt, 14, 0));
        SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, rt->toast, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, of);
    } else rt->toastOn = 0;
}
