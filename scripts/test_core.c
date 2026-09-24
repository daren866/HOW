/* test_core.c - HOW Runtime 核心层 Linux 单测（无 Windows 依赖）
 * 验证：miniz 解压 HAP → .abc 解析 → HOWVM 执行 → JSON5 UI 解析
 */
#include <assert.h>
#include "../src/howcore.h"
#include "miniz.h"

static int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  [PASS] %s\n", name); \
    else { printf("  [FAIL] %s\n", name); g_fail++; } } while (0)

int main(void) {
    const char *hap = "samples/MyFirstDemo.hap";
    printf("== HOW Runtime core tests ==\n");

    /* 1. 打开 HAP（zip） */
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    CHECK(mz_zip_reader_init_file(&z, hap, 0) == 1, "miniz 打开 HAP 包");
    CHECK(mz_zip_reader_locate_file(&z, "app.json5", NULL, 0) >= 0, "HAP 含 app.json5");
    CHECK(mz_zip_reader_locate_file(&z, "ets/modules.abc", NULL, 0) >= 0, "HAP 含 ets/modules.abc");

    /* 2. 提取 .abc 并解析 Panda 容器 */
    size_t alen = 0;
    void *abc = mz_zip_reader_extract_to_heap(&z,
        mz_zip_reader_locate_file(&z, "ets/modules.abc", NULL, 0), &alen, 0);
    CHECK(abc != NULL && alen > 32, "解压 modules.abc");
    FILE *f = fopen("/tmp/how_test.abc", "wb");
    fwrite(abc, 1, alen, f);
    fclose(f);

    AbcInfo info;
    CHECK(abc_load("/tmp/how_test.abc", &info) == 1, "abc_load: PACA 魔数校验");
    printf("    magic=%s ver=%u.%u.%u.%u size=%ld strings=%d chunks=%d\n",
           info.magic, info.ver[0], info.ver[1], info.ver[2], info.ver[3],
           info.size, info.nstr, info.nchunks);
    CHECK(info.nstr >= 5, "字符串池采样 >= 5");
    CHECK(info.nchunks == 1, "HOWRT 代码块 == 1");

    /* 3. HOWVM 执行 onPlus 计数器字节码 */
    Var vars[24];
    int nvars = 0;
    VmChunk *ck = &info.chunks[0];
    CHECK(strcmp(ck->name, "onPlus") == 0, "代码块名为 onPlus");
    CHECK(ck->ng == 1 && strcmp(ck->gname[0], "count") == 0, "全局变量 count");
    CHECK(vm_run(ck, vars, &nvars) == 0, "vm_run 第一次执行");
    CHECK(nvars == 1 && vars[0].num == 1.0, "执行后 count == 1");
    CHECK(vm_run(ck, vars, &nvars) == 0 && vars[0].num == 2.0, "再次执行 count == 2");
    CHECK(vm_run(ck, vars, &nvars) == 0 && vars[0].num == 3.0, "第三次执行 count == 3");

    /* 4. JSON5 解析 UI 模式 */
    size_t jlen = 0;
    void *jd = mz_zip_reader_extract_to_heap(&z,
        mz_zip_reader_locate_file(&z, "pages/index.json", NULL, 0), &jlen, 0);
    char *js = (char *)malloc(jlen + 1); /* 安全拷贝并补 NUL */
    memcpy(js, jd, jlen);
    js[jlen] = 0;
    JV *ui = json5_parse(js);
    free(js);
    CHECK(ui != NULL, "json5_parse(pages/index.json)");
    JV *root = jv_get(ui, "root");
    JV *rootch = jv_get(root, "children");
    CHECK(rootch != NULL && rootch->n == 7, "根节点含 7 个子组件");
    JV *btn = jv_at(rootch, 3);
    JV *b0 = jv_at(jv_get(btn, "children"), 0);
    CHECK(strcmp(jv_str(jv_get(b0, "type"), ""), "button") == 0, "子项 0 为 button");
    CHECK(strcmp(jv_str(jv_get(jv_get(b0, "action"), "chunk"), ""), "onPlus") == 0,
          "按钮绑定 vmcall:onPlus");
    JV *st = jv_get(ui, "state");
    CHECK(jv_num(jv_get(st, "count"), -1) == 0, "初始 state.count == 0");

    mz_zip_reader_end(&z);
    jv_free(ui);
    free(abc); free(jd);

    printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
