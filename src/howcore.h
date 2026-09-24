/* howcore.h - HOW Runtime 核心层（无 Windows 依赖，可在任意平台单测）
 *  - JSON5 宽松解析器（用于 app.json5 / module.json5 / pages/index.json）
 *  - Panda .abc 文件头解析（真实 ArkTS 字节码容器格式 PACA）
 *  - HOWVM 微型栈式字节码虚拟机（执行 HAP 内嵌代码块）
 */
#ifndef HOWCORE_H
#define HOWCORE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- JSON5 ---------------- */
typedef enum { JNull, JBool, JNum, JStr, JArr, JObj } JType;

typedef struct JV JV;
struct JV {
    JType t;
    double num;      /* JNum / JBool(0|1) */
    char *str;       /* JStr */
    JV **items;      /* JArr / JObj values */
    char **keys;     /* JObj keys */
    int n;
};

JV   *json5_parse(const char *s);
void  jv_free(JV *v);
JV   *jv_get(JV *o, const char *key);          /* 对象取值 */
JV   *jv_at(JV *a, int i);                     /* 数组取值 */
const char *jv_str(JV *v, const char *dflt);   /* 字符串安全取值 */
double jv_num(JV *v, double d);                /* 数值安全取值 */

/* ---------------- 变量表 ---------------- */
typedef struct {
    char   name[64];
    double num;
    char   str[192];
    int    isNum;
} Var;

int  var_find(Var *vars, int n, const char *name);
Var *var_ensure(Var *vars, int *n, int max, const char *name);

/* ---------------- HOWVM ---------------- */
typedef struct {
    char   name[64];
    int    ng;                       /* 全局变量个数 */
    char   gname[16][32];
    double ginit[16];
    unsigned char *code;             /* 字节码（堆上） */
    int    clen;
} VmChunk;

/* 操作码（单字节） */
enum {
    OP_PUSHI = 0, OP_LOADG, OP_STOREG, OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_DUP, OP_POP, OP_JMP, OP_JMPZ, OP_HALT
};

typedef struct {
    int ok;                          /* PACA 魔数校验结果 */
    char magic[5];
    unsigned char ver[4];
    long size;
    int  nstr;
    char strings[80][48];            /* 从文件中提取的字符串池样本 */
    int  nchunks;
    VmChunk chunks[8];               /* HOWRT 代码块 */
} AbcInfo;

int  abc_load(const char *pathU8, AbcInfo *out);   /* 载入并解析 .abc */
int  vm_run(VmChunk *c, Var *vars, int *nvars);    /* 执行代码块，读写变量表 */

#endif
