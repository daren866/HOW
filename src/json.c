/* json.c - JSON5 宽松解析器
 * 支持：单引号 / 双引号、转义（含 \uXXXX 与代理对）、行/块注释、
 *       无引号键名、尾随逗号。够用且足够小。
 */
#include "howcore.h"

typedef struct { const char *s; int i, n, depth, err; } P;

#define MAXDEPTH 64

static void skipws(P *p) {
    for (;;) {
        while (p->i < p->n && (unsigned char)p->s[p->i] <= ' ') p->i++;
        if (p->i + 1 < p->n && p->s[p->i] == '/' && p->s[p->i + 1] == '/') {
            while (p->i < p->n && p->s[p->i] != '\n') p->i++;
            continue;
        }
        if (p->i + 1 < p->n && p->s[p->i] == '/' && p->s[p->i + 1] == '*') {
            p->i += 2;
            while (p->i + 1 < p->n && !(p->s[p->i] == '*' && p->s[p->i + 1] == '/')) p->i++;
            p->i = (p->i + 2 <= p->n) ? p->i + 2 : p->n;
            continue;
        }
        break;
    }
}

static void utf8_put(char *out, int *len, unsigned c) {
    if (c < 0x80) out[(*len)++] = (char)c;
    else if (c < 0x800) {
        out[(*len)++] = (char)(0xC0 | (c >> 6));
        out[(*len)++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out[(*len)++] = (char)(0xE0 | (c >> 12));
        out[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (c & 0x3F));
    } else {
        out[(*len)++] = (char)(0xF0 | (c >> 18));
        out[(*len)++] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[(*len)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (c & 0x3F));
    }
}

/* 解析带引号字符串，返回 malloc 的 UTF-8 串 */
static char *pstring(P *p) {
    char q = p->s[p->i];
    int cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    p->i++;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == (unsigned char)q) { p->i++; break; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            switch (e) {
            case 'n': out[len++] = '\n'; break;
            case 't': out[len++] = '\t'; break;
            case 'r': out[len++] = '\r'; break;
            case 'b': out[len++] = '\b'; break;
            case 'f': out[len++] = '\f'; break;
            case '/': out[len++] = '/'; break;
            case '\\': out[len++] = '\\'; break;
            case '"': out[len++] = '"'; break;
            case '\'': out[len++] = '\''; break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 0; k < 4 && p->i < p->n; k++) {
                    char h = p->s[p->i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 5 < p->n &&
                    p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    p->i += 2;
                    unsigned lo = 0;
                    for (int k = 0; k < 4 && p->i < p->n; k++) {
                        char h = p->s[p->i++];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cap - len < 5) { cap *= 2; out = (char *)realloc(out, cap); }
                utf8_put(out, &len, cp);
                continue;
            }
            default: out[len++] = e; break;
            }
        } else {
            out[len++] = (char)c;
            p->i++;
        }
        if (cap - len < 8) { cap *= 2; out = (char *)realloc(out, cap); }
    }
    out[len] = 0;
    return out;
}

static int pident_end(P *p, int start) {
    int i = start;
    while (i < p->n) {
        char c = p->s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '-') i++;
        else break;
    }
    return i;
}

static JV *pvalue(P *p);

static JV *pobj(P *p) {
    JV *v = (JV *)calloc(1, sizeof(JV));
    v->t = JObj;
    p->i++; /* { */
    for (;;) {
        skipws(p);
        if (p->i >= p->n) break;
        if (p->s[p->i] == '}') { p->i++; break; }
        /* 键 */
        char *key = NULL;
        if (p->s[p->i] == '"' || p->s[p->i] == '\'') key = pstring(p);
        else {
            int e = pident_end(p, p->i);
            if (e == p->i) { p->err = 1; break; }
            key = (char *)malloc(e - p->i + 1);
            memcpy(key, p->s + p->i, (size_t)(e - p->i));
            key[e - p->i] = 0;
            p->i = e;
        }
        skipws(p);
        if (p->i < p->n && p->s[p->i] == ':') p->i++;
        JV *val = pvalue(p);
        v->keys = (char **)realloc(v->keys, sizeof(char *) * (size_t)(v->n + 1));
        v->items = (JV **)realloc(v->items, sizeof(JV *) * (size_t)(v->n + 1));
        v->keys[v->n] = key;
        v->items[v->n] = val;
        v->n++;
        skipws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; skipws(p);
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; } }
    }
    return v;
}

static JV *parr(P *p) {
    JV *v = (JV *)calloc(1, sizeof(JV));
    v->t = JArr;
    p->i++; /* [ */
    for (;;) {
        skipws(p);
        if (p->i >= p->n) break;
        if (p->s[p->i] == ']') { p->i++; break; }
        JV *val = pvalue(p);
        v->items = (JV **)realloc(v->items, sizeof(JV *) * (size_t)(v->n + 1));
        v->items[v->n++] = val;
        skipws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; skipws(p);
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; } }
    }
    return v;
}

static JV *pvalue(P *p) {
    if (p->depth >= MAXDEPTH) { p->err = 1; JV *v = (JV *)calloc(1, sizeof(JV)); v->t = JNull; return v; }
    skipws(p);
    if (p->i >= p->n) { JV *v = (JV *)calloc(1, sizeof(JV)); v->t = JNull; return v; }
    char c = p->s[p->i];
    JV *v;
    if (c == '{') { p->depth++; v = pobj(p); p->depth--; }
    else if (c == '[') { p->depth++; v = parr(p); p->depth--; }
    else if (c == '"' || c == '\'') {
        v = (JV *)calloc(1, sizeof(JV));
        v->t = JStr;
        v->str = pstring(p);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(p->s + p->i, &end);
        p->i = (int)(end - p->s);
        v = (JV *)calloc(1, sizeof(JV));
        v->t = JNum; v->num = d;
    } else {
        int e = pident_end(p, p->i);
        int L = e - p->i;
        v = (JV *)calloc(1, sizeof(JV));
        if (L == 4 && memcmp(p->s + p->i, "true", 4) == 0) { v->t = JNum; v->num = 1; }
        else if (L == 5 && memcmp(p->s + p->i, "false", 5) == 0) { v->t = JNum; v->num = 0; }
        else if (L == 4 && memcmp(p->s + p->i, "null", 4) == 0) { v->t = JNull; }
        else { v->t = JStr; v->str = (char *)malloc((size_t)L + 1); memcpy(v->str, p->s + p->i, (size_t)L); v->str[L] = 0; }
        p->i = e;
    }
    return v;
}

JV *json5_parse(const char *s) {
    if (!s) return NULL;
    P p; p.s = s; p.i = 0; p.n = (int)strlen(s); p.depth = 0; p.err = 0;
    JV *v = pvalue(&p);
    return v;
}

void jv_free(JV *v) {
    if (!v) return;
    if (v->str) free(v->str);
    for (int i = 0; i < v->n; i++) {
        if (v->items) jv_free(v->items[i]);
        if (v->keys && v->keys[i]) free(v->keys[i]);
    }
    free(v->items); free(v->keys); free(v);
}

JV *jv_get(JV *o, const char *key) {
    if (!o || o->t != JObj) return NULL;
    for (int i = 0; i < o->n; i++)
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

JV *jv_at(JV *a, int i) {
    if (!a || a->t != JArr || i < 0 || i >= a->n) return NULL;
    return a->items[i];
}

const char *jv_str(JV *v, const char *dflt) {
    if (v && v->t == JStr && v->str) return v->str;
    return dflt;
}

double jv_num(JV *v, double d) {
    if (v && v->t == JNum) return v->num;
    return d;
}

/* ---------------- 变量表 ---------------- */
int var_find(Var *vars, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(vars[i].name, name) == 0) return i;
    return -1;
}

Var *var_ensure(Var *vars, int *n, int max, const char *name) {
    int i = var_find(vars, *n, name);
    if (i >= 0) return &vars[i];
    if (*n >= max) return NULL;
    Var *v = &vars[(*n)++];
    memset(v, 0, sizeof(Var));
    snprintf(v->name, sizeof(v->name), "%s", name);
    return v;
}
