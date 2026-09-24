/* store.c - HAP 安装管理：探测 app.json5、解压安装（miniz）、应用清单扫描 */
#include "how.h"
#include "miniz.h"

/* ---------------- 编码转换 ---------------- */
wchar_t *u8w(const char *u8, wchar_t *buf, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, buf, cap);
    if (n <= 0) { buf[0] = 0; }
    return buf;
}

char *w2u8(const wchar_t *w, char *buf, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap, NULL, NULL);
    if (n <= 0) buf[0] = 0;
    return buf;
}

char *w2acp(const wchar_t *w, char *buf, int cap) {
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, buf, cap, NULL, NULL);
    if (n <= 0) buf[0] = 0;
    return buf;
}

/* ---------------- 目录工具 ---------------- */
static void mkdirs_w(const wchar_t *path) {
    wchar_t tmp[1024];
    wcsncpy(tmp, path, 1023);
    tmp[1023] = 0;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

void store_apps_dir(wchar_t *out, int cap) {
    wchar_t base[768];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, 768);
    if (n == 0 || n >= 768) wcscpy(base, L"C:");
    _snwprintf(out, cap, L"%s\\HOW\\apps", base);
    out[cap - 1] = 0;
    mkdirs_w(out);
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

/* ---------------- install.json ---------------- */
static void write_install_json(const char *dirU8, const char *nameU8,
                               const char *bundleU8, const char *verU8) {
    char path[1024];
    wchar_t wdir[512];
    u8w(dirU8, wdir, 512);
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(path, sizeof(path), "%s\\install.json", dirU8);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f,
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"bundle\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"installed\": \"%04u-%02u-%02u %02u:%02u:%02u\"\n"
        "}\n",
        nameU8, bundleU8, verU8 ? verU8 : "",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fclose(f);
}

/* ---------------- HAP 探测 ---------------- */
static char *zip_read_entry(mz_zip_archive *z, const char *entry, long *outLen) {
    int idx = mz_zip_reader_locate_file(z, entry, NULL, 0);
    if (idx < 0) return NULL;
    size_t len = 0;
    void *buf = mz_zip_reader_extract_to_heap(z, (mz_uint)idx, &len, 0);
    if (!buf) return NULL;
    /* miniz 返回的缓冲区恰为 len 字节；拷贝到 len+1 并补 NUL，避免堆越界 */
    char *out = (char *)malloc(len + 1);
    if (!out) {
        mz_free(buf);
        return NULL;
    }
    memcpy(out, buf, len);
    out[len] = 0;
    mz_free(buf);
    if (outLen) *outLen = (long)len;
    return out;
}

int hap_probe(const wchar_t *hapPath, char *nameU8, int nameCap,
              char *bundleU8, int bCap, char *verU8, int vCap) {
    char pathA[1024];
    w2acp(hapPath, pathA, 1024);
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
        /* label 引用资源或缺失 → 试 pack.info，再退回包名 */
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

/* ---------------- HAP 解压安装 ---------------- */
static int entry_unsafe(const char *name) {
    if (!name || !name[0]) return 1;
    if (name[0] == '/' || name[0] == '\\') return 1;
    if (strstr(name, "..")) return 1;
    if (strchr(name, ':')) return 1;
    return 0;
}

int store_install(const wchar_t *hapPath, const char *bundleU8,
                  const char *nameU8, const char *verU8) {
    char pathA[1024];
    w2acp(hapPath, pathA, 1024);
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
        snprintf(dirU8, sizeof(dirU8), "%s\\%s", appsU8, sub);
    }
    wchar_t wdir[512];
    u8w(dirU8, wdir, 512);
    mkdirs_w(wdir);

    unsigned n = mz_zip_reader_get_num_files(&z);
    unsigned installed = 0;
    for (unsigned i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        if (entry_unsafe(st.m_filename)) continue;
        size_t L = strlen(st.m_filename);
        if (L > 0 && st.m_filename[L - 1] == '/') continue; /* 纯目录项 */
        char destA[1200];
        snprintf(destA, sizeof(destA), "%s\\%s", dirU8, st.m_filename);
        /* 归一化斜杠并建父目录 */
        for (char *p = destA; *p; p++) if (*p == '/') *p = '\\';
        {
            wchar_t full[1200];
            u8w(destA, full, 1200);
            wchar_t parent[1200];
            wcsncpy(parent, full, 1199);
            parent[1199] = 0;
            wchar_t *cut = wcsrchr(parent, L'\\');
            if (cut) { *cut = 0; mkdirs_w(parent); }
        }
        if (mz_zip_reader_extract_to_file(&z, i, destA, 0)) installed++;
    }
    mz_zip_reader_end(&z);
    if (installed == 0) return -2;
    write_install_json(dirU8, nameU8, bundleU8, verU8);
    return 0;
}

/* ---------------- 已安装列表 ---------------- */
int store_list(AppInfo *arr, int max) {
    wchar_t apps[512];
    store_apps_dir(apps, 512);
    int cnt = 0;
    wchar_t pattern[560];
    _snwprintf(pattern, 560, L"%s\\*", apps);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] == L'.') continue;
            if (cnt >= max) break;
            wchar_t ij[1024];
            _snwprintf(ij, 1024, L"%s\\%s\\install.json", apps, fd.cFileName);
            FILE *f = _wfopen(ij, L"rb");
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
            if (!ai->name[0]) wcscpy(ai->name, fd.cFileName);
            char dirU8[160];
            w2u8(fd.cFileName, dirU8, 160);
            snprintf(ai->dir, sizeof(ai->dir), "%s", dirU8);
            cnt++;
            jv_free(j);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return cnt;
}
