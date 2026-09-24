/* abc.c - Panda 字节码容器（.abc）解析
 * 真实 .abc 文件头：magic "PACA" + 4 字节版本号（参考 arkcompiler_runtime_core
 * libpandafile/file.h 的 FileHeader 布局前 8 字节）。本模块做头部校验、
 * 字符串池采样，并加载内嵌于文件尾部的 HOWRT 微型代码段（HOWVM 程序）。
 */
#include "howcore.h"

static int printable_run(const unsigned char *b, int i, int n) {
    int L = 0;
    while (i + L < n) {
        unsigned char c = b[i + L];
        int ok = (c >= 0x20 && c < 0x7F) || c >= 0x80;
        if (!ok) break;
        L++;
        if (L >= 48) break;
    }
    return L;
}

static int str_dup_exists(AbcInfo *o, const char *s) {
    for (int i = 0; i < o->nstr; i++)
        if (strcmp(o->strings[i], s) == 0) return 1;
    return 0;
}

static void scan_strings(const unsigned char *b, long n, AbcInfo *o) {
    long i = 8;
    while (i < n && o->nstr < 80) {
        unsigned char c = b[i];
        int good = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= 0x80); /* 标识符开头或 UTF-8 中文 */
        if (!good) { i++; continue; }
        int L = printable_run(b, (int)i, (int)n);
        if (L >= 3) {
            char tmp[52];
            int cp = L > 47 ? 47 : L;
            /* 仅当字节序列为合法 UTF-8 中文或 ascii 标识符片段时收录 */
            memcpy(tmp, b + i, (size_t)cp);
            tmp[cp] = 0;
            int keep = 1;
            for (int k = 0; k < cp; k++) {
                unsigned char d = (unsigned char)tmp[k];
                if (d < 0x20 || d == 0x7F) { keep = 0; break; }
            }
            if (keep && !str_dup_exists(o, tmp)) {
                int cp2 = cp > 47 ? 47 : cp;
                memcpy(o->strings[o->nstr], tmp, (size_t)cp2);
                o->strings[o->nstr][cp2] = 0;
                o->nstr++;
            }
            i += L;
        } else i++;
    }
}

static unsigned rd16(const unsigned char *b, int off) {
    return (unsigned)b[off] | ((unsigned)b[off + 1] << 8);
}
static unsigned rd32(const unsigned char *b, int off) {
    return (unsigned)b[off] | ((unsigned)b[off + 1] << 8) |
           ((unsigned)b[off + 2] << 16) | ((unsigned)b[off + 3] << 24);
}

static int find_howrt(const unsigned char *b, long n) {
    for (long i = 0; i + 5 <= n; i++)
        if (b[i] == 'H' && b[i + 1] == 'O' && b[i + 2] == 'W' &&
            b[i + 3] == 'R' && b[i + 4] == 'T') return (int)i;
    return -1;
}

static void load_howrt(const unsigned char *b, long n, int off, AbcInfo *o) {
    int p = off + 5;
    if (p + 4 > n) return;
    /* u16 ver, u16 nchunks */
    unsigned nch = rd16(b, p + 2);
    p += 4;
    if (nch > 8) nch = 8;
    for (unsigned c = 0; c < nch && p + 2 <= n; c++) {
        VmChunk *ck = &o->chunks[o->nchunks];
        memset(ck, 0, sizeof(VmChunk));
        unsigned nl = rd16(b, p); p += 2;
        if (p + (int)nl > (int)n || nl >= sizeof(ck->name)) return;
        memcpy(ck->name, b + p, nl); p += nl;
        if (p + 2 > n) return;
        ck->ng = (int)rd16(b, p); p += 2;
        if (ck->ng > 16) ck->ng = 16;
        for (int g = 0; g < ck->ng && p + 2 <= n; g++) {
            unsigned gl = rd16(b, p); p += 2;
            if (p + (int)gl > (int)n || gl >= sizeof(ck->gname[0])) return;
            memcpy(ck->gname[g], b + p, gl); p += gl;
            /* u8 isNum + f64 init */
            if (p + 9 > n) return;
            double d;
            memcpy(&d, b + p + 1, 8);
            ck->ginit[g] = d;
            p += 9;
        }
        if (p + 4 > n) return;
        unsigned cl = rd32(b, p); p += 4;
        if (cl > 4096 || p + (int)cl > (int)n) return;
        ck->code = (unsigned char *)malloc(cl);
        memcpy(ck->code, b + p, cl);
        ck->clen = (int)cl;
        p += (int)cl;
        o->nchunks++;
    }
}

int abc_load(const char *pathU8, AbcInfo *out) {
    memset(out, 0, sizeof(AbcInfo));
    FILE *f = fopen(pathU8, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n > 4 * 1024 * 1024) n = 4 * 1024 * 1024; /* 采样上限 4MB */
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    out->size = (long)got;
    if (got < 8 || b[0] != 'P' || b[1] != 'A' || b[2] != 'C' || b[3] != 'A') {
        free(b);
        return 0;
    }
    out->ok = 1;
    memcpy(out->magic, "PACA", 4);
    out->magic[4] = 0;
    memcpy(out->ver, b + 4, 4);
    scan_strings(b, (long)got, out);
    int rt = find_howrt(b, (long)got);
    if (rt >= 0) load_howrt(b, (long)got, rt, out);
    free(b);
    return 1;
}

/* ---------------- HOWVM ---------------- */
int vm_run(VmChunk *c, Var *vars, int *nvars) {
    if (!c || !c->code || c->clen <= 0) return -1;
    double g[16];
    for (int i = 0; i < c->ng; i++) {
        int vi = var_find(vars, *nvars, c->gname[i]);
        g[i] = (vi >= 0 && vars[vi].isNum) ? vars[vi].num : c->ginit[i];
    }
    double st[256];
    int sp = 0;
    int pc = 0;
    long steps = 0;
    while (pc < c->clen && steps++ < 200000) {
        unsigned char op = c->code[pc++];
        switch (op) {
        case OP_PUSHI: {
            if (pc + 4 > c->clen) return -2;
            int v = (int)rd32(c->code, pc);
            pc += 4;
            if (sp >= 256) return -3;
            st[sp++] = (double)v;
            break;
        }
        case OP_LOADG: {
            if (pc >= c->clen) return -2;
            unsigned idx = c->code[pc++];
            if (idx >= 16 || sp >= 256) return -3;
            st[sp++] = g[idx];
            break;
        }
        case OP_STOREG: {
            if (pc >= c->clen) return -2;
            unsigned idx = c->code[pc++];
            if (idx >= 16 || sp < 1) return -3;
            g[idx] = st[--sp];
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
            if (sp < 2) return -3;
            double y = st[--sp], x = st[--sp], r = 0;
            if (op == OP_ADD) r = x + y;
            else if (op == OP_SUB) r = x - y;
            else if (op == OP_MUL) r = x * y;
            else r = (y != 0) ? x / y : 0;
            st[sp++] = r;
            break;
        }
        case OP_DUP: if (sp < 1 || sp >= 256) return -3; st[sp] = st[sp - 1]; sp++; break;
        case OP_POP: if (sp < 1) return -3; sp--; break;
        case OP_JMP: {
            if (pc + 4 > c->clen) return -2;
            int t = (int)rd32(c->code, pc);
            pc = t;
            break;
        }
        case OP_JMPZ: {
            if (pc + 4 > c->clen) return -2;
            int t = (int)rd32(c->code, pc);
            pc += 4;
            if (sp < 1) return -3;
            if (st[--sp] == 0) pc = t;
            break;
        }
        case OP_HALT: goto done;
        default: return -4;
        }
    }
done:
    for (int i = 0; i < c->ng; i++) {
        Var *v = var_ensure(vars, nvars, 24, c->gname[i]);
        if (v) { v->isNum = 1; v->num = g[i]; }
    }
    return 0;
}
