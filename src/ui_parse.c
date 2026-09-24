/* ui_parse.c - RtState 生命周期、运行日志、ArkUI 声明式 UI 模式解析 */
#include "how.h"
#include <stdarg.h>

static void node_free(UINode *nd);


/* ---------------- 生命周期 ---------------- */
RtState *rt_create(void) {
    RtState *rt = (RtState *)calloc(1, sizeof(RtState));
    rt->logCap = 8192;
    rt->log = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)rt->logCap);
    rt->log[0] = 0;
    return rt;
}

void rt_destroy(RtState *rt) {
    if (!rt) return;
    if (rt->root) free_node(rt->root);
    for (int i = 0; i < rt->nfonts; i++) DeleteObject(rt->fonts[i]);
    free(rt->rects);
    free(rt->log);
    free(rt);
}

HFONT rt_getfont(RtState *rt, int size, int bold) {
    if (size < 9) size = 9;
    if (size > 72) size = 72;
    int want = (size - 9) * 2 + (bold ? 1 : 0);
    if (want < 0 || want >= 24) {
        static HFONT fallback[2];
        int slot = bold ? 1 : 0;
        if (!fallback[slot])
            fallback[slot] = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                                         0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        return fallback[slot];
    }
    if (!rt->fonts[want])
        rt->fonts[want] = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                                      0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    return rt->fonts[want];
}

/* ---------------- 运行日志 ---------------- */
void rt_log(RtState *rt, const char *fmtU8, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmtU8);
    vsnprintf(buf, sizeof(buf) - 1, fmtU8, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    wchar_t wbuf[1100];
    u8w(buf, wbuf, 1100);
    int add = (int)wcslen(wbuf);
    if (rt->logLen + add + 2 > rt->logCap) {
        while (rt->logLen + add + 2 > rt->logCap) rt->logCap *= 2;
        rt->log = (wchar_t *)realloc(rt->log, sizeof(wchar_t) * (size_t)rt->logCap);
    }
    memcpy(rt->log + rt->logLen, wbuf, sizeof(wchar_t) * (size_t)add);
    rt->logLen += add;
    rt->log[rt->logLen++] = L'\n';
    rt->log[rt->logLen] = 0;
}

const wchar_t *rt_logbuf(RtState *rt) { return rt->log; }

void rt_set_toast(RtState *rt, const char *msgU8) {
    u8w(msgU8, rt->toast, 256);
    rt->toastOn = 1;
    rt->toastTick = GetTickCount();
}

/* ---------------- 节点解析 ---------------- */
static void node_free(UINode *nd) {
    if (!nd) return;
    for (int i = 0; i < nd->nch; i++) node_free(nd->ch[i]);
    free(nd->ch);
    free(nd);
}

void free_node(UINode *nd) { node_free(nd); }

static COLORREF parse_color(const char *s, COLORREF dflt) {
    if (!s || s[0] != '#') return dflt;
    size_t L = strlen(s);
    unsigned r, g, b;
    if (L == 7 && sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) == 3) return RGB(r, g, b);
    if (L == 4 && sscanf(s + 1, "%1x%1x%1x", &r, &g, &b) == 3) return RGB(r * 17, g * 17, b * 17);
    return dflt;
}

static void parse_action(JV *ja, Action *act) {
    memset(act, 0, sizeof(Action));
    const char *t = jv_str(jv_get(ja, "type"), "");
    if (strcmp(t, "inc") == 0) {
        act->type = ACT_INC;
        snprintf(act->var, 64, "%s", jv_str(jv_get(ja, "var"), "count"));
        act->delta = jv_num(jv_get(ja, "delta"), 1.0);
    } else if (strcmp(t, "set") == 0) {
        act->type = ACT_SET;
        snprintf(act->var, 64, "%s", jv_str(jv_get(ja, "var"), "count"));
        act->value = jv_num(jv_get(ja, "value"), 0.0);
    } else if (strcmp(t, "vmcall") == 0) {
        act->type = ACT_VMCALL;
        snprintf(act->chunk, 64, "%s", jv_str(jv_get(ja, "chunk"), ""));
    } else if (strcmp(t, "toast") == 0) {
        act->type = ACT_TOAST;
        snprintf(act->msg, 256, "%s", jv_str(jv_get(ja, "msg"), "HOW"));
    }
}

static void set_len_prop(JV *jv, const char *key, int *has, int *px, int *pct) {
    JV *v = jv_get(jv, key);
    if (!v) return;
    if (v->t == JNum) { *has = 1; *px = (int)v->num; }
    else if (v->t == JStr && v->str) {
        size_t L = strlen(v->str);
        if (L > 0 && v->str[L - 1] == '%') {
            *pct = atoi(v->str);
            *has = 2; /* 百分比 */
        } else { *has = 1; *px = atoi(v->str); }
    }
}

static int g_idc;

static UINode *node_from_jv(JV *j) {
    if (!j || j->t != JObj) return NULL;
    UINode *nd = (UINode *)calloc(1, sizeof(UINode));
    nd->id = g_idc++;
    snprintf(nd->type, sizeof(nd->type), "%s", jv_str(jv_get(j, "type"), "column"));
    nd->fontSize = 16;
    nd->fg = RGB(0x18, 0x24, 0x31);
    if (strcmp(nd->type, "button") == 0) {
        nd->bg = RGB(0x00, 0x7D, 0xFF);
        nd->hasBg = 1;
        nd->fg = RGB(0xFF, 0xFF, 0xFF);
        nd->radius = 8;
        nd->textAlign = 1;
    }
    nd->pad = (int)jv_num(jv_get(j, "padding"), 0);
    nd->margin = (int)jv_num(jv_get(j, "margin"), 0);
    nd->space = (int)jv_num(jv_get(j, "space"), 0);
    nd->radius = (int)jv_num(jv_get(j, "radius"), nd->radius);
    nd->fontSize = (int)jv_num(jv_get(j, "fontSize"), nd->fontSize);
    nd->bold = (int)jv_num(jv_get(j, "bold"), 0);
    nd->hasBg = jv_get(j, "backgroundColor") ? 1 : nd->hasBg;
    nd->bg = parse_color(jv_str(jv_get(j, "backgroundColor"), "#000000"), nd->bg);
    nd->fg = parse_color(jv_str(jv_get(j, "color"), "#182431"), nd->fg);
    snprintf(nd->justify, sizeof(nd->justify), "%s", jv_str(jv_get(j, "justify"), "start"));
    snprintf(nd->align, sizeof(nd->align), "%s", jv_str(jv_get(j, "align"), "center"));
    nd->textAlign = (int)jv_num(jv_get(j, "textAlign"), strcmp(nd->type, "text") == 0 ? 0 : 1);
    snprintf(nd->text, sizeof(nd->text), "%s", jv_str(jv_get(j, "text"), ""));
    snprintf(nd->var, sizeof(nd->var), "%s", jv_str(jv_get(j, "var"), ""));
    set_len_prop(j, "width", &nd->hasW, &nd->w, &nd->wPct);
    set_len_prop(j, "height", &nd->hasH, &nd->h, &nd->hPct);
    JV *act = jv_get(j, "action");
    if (act && act->t == JObj) parse_action(act, &nd->act);
    JV *ch = jv_get(j, "children");
    if (ch && ch->t == JArr) {
        int max = ch->n > 32 ? 32 : ch->n;
        nd->ch = (UINode **)calloc((size_t)max + 1, sizeof(UINode *));
        for (int i = 0; i < max; i++) {
            UINode *c = node_from_jv(jv_at(ch, i));
            if (c) nd->ch[nd->nch++] = c;
        }
    }
    return nd;
}

void rt_seed_state(RtState *rt, JV *pageObj) {
    JV *st = jv_get(pageObj, "state");
    if (!st || st->t != JObj) return;
    for (int i = 0; i < st->n; i++) {
        Var *v = var_ensure(rt->vars, &rt->nvars, 24, st->keys[i] ? st->keys[i] : "");
        if (!v) break;
        if (st->items[i]->t == JNum) {
            v->isNum = 1;
            v->num = st->items[i]->num;
        } else if (st->items[i]->t == JStr) {
            v->isNum = 0;
            snprintf(v->str, sizeof(v->str), "%s", st->items[i]->str ? st->items[i]->str : "");
        }
    }
}

int rt_load_ui(RtState *rt, const char *jsonU8) {
    JV *j = json5_parse(jsonU8);
    if (!j) return -1;
    rt_seed_state(rt, j);
    g_idc = 0;
    UINode *root = node_from_jv(jv_get(j, "root"));
    jv_free(j);
    if (!root) return -2;
    if (rt->root) free_node(rt->root);
    rt->root = root;
    if (rt->rects) free(rt->rects);
    rt->nrects = g_idc;
    rt->rects = (RECT *)calloc((size_t)g_idc + 1, sizeof(RECT));
    return g_idc;
}
