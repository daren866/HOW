/* arkrt.c - 真 Ark 运行时（上游 arkcompiler 真编译产物）探测与执行层
 *
 * Release zip 布局（由 CI release job 注入）：
 *   HOW.exe
 *   ark\windows\ark_js_vm.exe ...   <- CI ark-runtime-windows job 产物（Windows 原生）
 *   ark\linux\ark_js_vm + *.so ...  <- CI ark-runtime job 产物（Linux，经 WSL 执行）
 *
 * 引擎选择顺序（arkrt_probe）：
 *   1) ENG_ARK_NATIVE  ark\windows\ark_js_vm.exe   直接 CreateProcess 执行
 *   2) ENG_ARK_WSL     ark\linux\ark_js_vm + wsl.exe 可用   经 WSL 执行
 *   3) ENG_HOWVM       内置 HOWVM 引擎兜底（无外部组件时）
 *
 * arkrt_exec 会真实启动上游运行时进程加载 hap 的 modules.abc，
 * 捕获其 stdout/stderr 供兼容层日志面板与 selftest 使用。
 * 找不到组件或执行环境不满足时一律安全回退 HOWVM，绝不阻塞 UI。
 */
#include "how.h"
#include <wchar.h>

#define ARKRT_TIMEOUT_MS 20000

static int file_exists_w(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* 快速探测 WSL 是否可用（wsl.exe --status，3 秒超时，失败不阻塞） */
static int wsl_available(void) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    wchar_t cmd[] = L"\"C:\\Windows\\System32\\wsl.exe\" --status";
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return 0;
    }
    CloseHandle(wr);
    DWORD w = WaitForSingleObject(pi.hProcess, 3000);
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    /* 读掉管道防止阻塞残留（进程已退出，读至 EOF） */
    char sink[512];
    DWORD got = 0;
    while (ReadFile(rd, sink, sizeof(sink), &got, NULL) && got > 0) { }
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

/* C:\a\b.ext -> /mnt/c/a/b.ext（UTF-8，反斜杠转正斜杠） */
static void winpath_to_mnt(const wchar_t *w, char *out, int cap) {
    char u8[1024];
    w2u8(w, u8, 1024);
    if (((u8[0] >= 'A' && u8[0] <= 'Z') || (u8[0] >= 'a' && u8[0] <= 'z')) &&
        u8[1] == ':') {
        char drive = (u8[0] >= 'a') ? u8[0] : (char)(u8[0] - 'A' + 'a');
        _snprintf(out, (size_t)cap, "/mnt/%c%s", drive, u8 + 2);
        out[cap - 1] = 0;
        for (char *q = out; *q; q++)
            if (*q == '\\') *q = '/';
    } else {
        _snprintf(out, (size_t)cap, "%s", u8);
        out[cap - 1] = 0;
    }
}

int arkrt_probe(ArkRtProbe *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    wchar_t exe[1024];
    GetModuleFileNameW(NULL, exe, 1024);
    wchar_t *cut = wcsrchr(exe, L'\\');
    if (cut) *cut = 0; /* exe 所在目录 */
    wchar_t p[1100];

    /* 1) Windows 原生真编译运行时 */
    _snwprintf(p, 1100, L"%s\\ark\\windows\\ark_js_vm.exe", exe);
    p[1099] = 0;
    if (file_exists_w(p)) {
        out->type = ENG_ARK_NATIVE;
        _snwprintf(out->exePath, 1024, L"%s", p);
        _snwprintf(out->detail, 192,
                   L"\u4e0a\u6e38\u771f\u7f16\u8bd1\u4ea7\u7269 \u00b7 Windows \u539f\u751f");
        return 1;
    }

    /* 2) Linux 真编译运行时 + WSL 通路 */
    _snwprintf(p, 1100, L"%s\\ark\\linux\\ark_js_vm", exe);
    p[1099] = 0;
    if (file_exists_w(p) && wsl_available()) {
        out->type = ENG_ARK_WSL;
        _snwprintf(out->exePath, 1024, L"%s", p);
        _snwprintf(out->detail, 192,
                   L"\u4e0a\u6e38\u771f\u7f16\u8bd1\u4ea7\u7269 \u00b7 WSL \u901a\u8def");
        return 1;
    }

    out->type = ENG_HOWVM;
    return 0;
}

const wchar_t *arkrt_engine_name(HowEngine t) {
    switch (t) {
    case ENG_ARK_NATIVE:
        return L"\u771f Ark \u8fd0\u884c\u65f6 \u00b7 ark_js_vm (Windows)";
    case ENG_ARK_WSL:
        return L"\u771f Ark \u8fd0\u884c\u65f6 \u00b7 ark_js_vm (WSL)";
    default:
        return L"HOW Runtime \u00b7 HOWVM \u5f15\u64ce";
    }
}

/* 真实执行：启动上游 ark_js_vm 加载 modules.abc，捕获输出。
 * 返回值：>=0 进程退出码；-1 参数/组件缺失；-2 启动失败；-3 超时终止 */
int arkrt_exec(ArkRtProbe *p, const wchar_t *abcPathW, char *outBuf, int outCap) {
    if (!p || !p->exePath[0] || !abcPathW || !outBuf || outCap <= 0) return -1;
    outBuf[0] = 0;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1024 * 1024)) return -2; /* 1MB 缓冲防写阻塞 */
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    wchar_t cmd[2400];
    if (p->type == ENG_ARK_NATIVE) {
        _snwprintf(cmd, 2400, L"\"%s\" \"%s\"", p->exePath, abcPathW);
    } else {
        char exeM[1024], abcM[1024], dirM[1024];
        winpath_to_mnt(p->exePath, exeM, 1024);
        winpath_to_mnt(abcPathW, abcM, 1024);
        /* 运行时同目录（libark_jsruntime.so 等依赖） */
        _snprintf(dirM, sizeof(dirM), "%s", exeM);
        dirM[sizeof(dirM) - 1] = 0;
        char *slash = strrchr(dirM, '/');
        if (slash) *slash = 0;
        _snwprintf(cmd, 2400,
                   L"\"C:\\Windows\\System32\\wsl.exe\" -e sh -c "
                   L"\"cd '%s' && LD_LIBRARY_PATH=. '%s' '%s' 2>&1\"",
                   dirM, exeM, abcM);
    }
    cmd[2399] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -2;
    }
    CloseHandle(wr); /* 让管道 EOF 可达 */

    DWORD w = WaitForSingleObject(pi.hProcess, ARKRT_TIMEOUT_MS);
    int rc;
    if (w != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99);
        WaitForSingleObject(pi.hProcess, 2000);
        rc = -3;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        rc = (int)code;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* 读取输出（非阻塞场景：进程已退出/已终止，EOF 自然到达） */
    int used = 0;
    DWORD got = 0;
    while (used + 1 < outCap &&
           ReadFile(rd, outBuf + used, (DWORD)(outCap - 1 - used), &got, NULL) &&
           got > 0) {
        used += (int)got;
    }
    CloseHandle(rd);
    outBuf[used] = 0;
    return rc;
}
