// platform.c — 平台边界的唯一实现
//
// 声明与理由见 platform.h。这个文件之外，整个代码库不许出现
// #ifdef _WIN32 或 include <windows.h>（tools/check_portable.sh 会查）。
//
// 结构上分两支：Windows 与类 Unix。macOS 与 Linux 在这一层完全同构，
// 所以没有第三支——这不是省略，是真的没有差异。

#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
// ── 这两个头文件的顺序不能换 ────────────────────────────────
//
// winsock2.h 必须**先于** windows.h 包含。反过来的话 windows.h 会
// 拉进老的 winsock.h，两者对同一批符号给出不同声明，结果是编译期
// 一堆 redefinition，或者（更糟）编过但 socket 句柄解释错。
//
// 这个顺序问题只影响本文件 —— 整个代码库只有这里包含 winsock2.h。
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <direct.h>
#  define plat_isatty _isatty
#  define plat_fileno _fileno
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  define plat_isatty isatty
#  define plat_fileno fileno
#endif

// ============================================================
// UTF-8 → 宽字符（仅 Windows 用）
// ============================================================
//
// Windows 的文件 API 全是宽字符版（_wfopen/_wstat/_wremove），而
// 语言里的字符串是 UTF-8。这中间的转换不能省：用窄字符版 API
// 的话，路径会按 ANSI 代码页解释，于是「用户目录名叫中文」
// 这个再常见不过的情况会直接打不开文件。
//
// 用静态轮转缓冲而不是让调用方 free：调用点全是「一行内用完」，
// 让它管内存只会引入泄漏。4 个缓冲足够覆盖「源与目标同在一次
// 调用里出现」的情况（plat_file_replace 就是）。
#if X_HOST_WINDOWS
static wchar_t* plat_utf8_to_wide(const char* s) {
    static wchar_t bufs[4][1024];
    static int slot = 0;
    wchar_t* out = bufs[slot];
    slot = (slot + 1) & 3;
    out[0] = L'\0';
    if (!s) return out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, 1023);
    if (n <= 0) out[0] = L'\0';
    return out;
}
#endif

// 颜色开关的全局状态。用文件级 static 而不是环境变量：
// 环境变量会被子进程继承，`x build` 调 gcc 时 gcc 也会看到，
// 而 gcc 的 --no-color 语义与我们不同，污染它没有好处。
static bool s_color_disabled = false;

// ============================================================
// 平台识别
// ============================================================

const char* plat_exe_suffix(void) {
#if X_HOST_WINDOWS
    return ".exe";
#else
    return "";
#endif
}

const char* plat_host_name(void) {
#if X_HOST_WINDOWS
    return "windows";
#elif X_HOST_MACOS
    return "macos";
#elif X_HOST_LINUX
    return "linux";
#else
    return "unknown";
#endif
}

const char* plat_host_triple(void) {
    // 架构 + 系统 + ABI 三段。这里只覆盖当前真实会跑的三类组合，
    // 其余情况返回 "unknown-<system>"，让 --target 的校验能明确报错
    // 而不是给出一个猜的字符串。
#if defined(__x86_64__) || defined(_M_X64)
#  define X_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define X_ARCH "aarch64"
#else
#  define X_ARCH "unknown"
#endif

#if X_HOST_WINDOWS
    return X_ARCH "-windows-msvc";
#elif X_HOST_MACOS
    return X_ARCH "-macos";
#elif X_HOST_LINUX
    // 不区分 gnu/musl：宿主用的是哪个 libc 在编译期无法可靠判断，
    // 猜错会让 --target=host 落到错误的工具链上。留给用户显式指定。
    return X_ARCH "-linux-gnu";
#else
    return X_ARCH "-unknown";
#endif
#undef X_ARCH
}

// ============================================================
// 终端初始化
// ============================================================

#if X_HOST_WINDOWS

// 记住进入时的代码页，退出时还原。
//
// 为什么必须还原：SetConsoleOutputCP 改的是**控制台这个共享对象**的
// 属性，不是本进程的私有状态。如果 x.exe 在一个 cmd/PowerShell 窗口
// 里被调用，改完不退，父 shell 后续所有命令的输入输出都会按 UTF-8
// 处理 —— 那些用 GBK 的工具（很多老国产软件、部分构建脚本）立刻
// 变成乱码。「我的程序能正确显示」不能以「弄坏别人的终端」为代价。
static UINT s_prev_out_cp = 0;
static UINT s_prev_in_cp  = 0;
static int  s_cp_saved    = 0;

static void restore_console_cp(void) {
    if (!s_cp_saved) return;
    if (s_prev_out_cp) SetConsoleOutputCP(s_prev_out_cp);
    if (s_prev_in_cp)  SetConsoleCP(s_prev_in_cp);
    s_cp_saved = 0;
}

#endif  // X_HOST_WINDOWS

void plat_init_console(void) {
    static int done = 0;
    if (done) return;
    done = 1;

#if X_HOST_WINDOWS
    // 判断「是否真的挂着一个控制台」。
    //
    // 用 GetConsoleOutputCP() != 0 而不是 GetConsoleWindow() != NULL：
    // ConPTY（VSCode / Windows Terminal 用的伪控制台）下 GetConsoleWindow
    // 的返回值在部分 Windows 版本上不可靠，而 GetConsoleOutputCP 在
    // 「没有控制台」时确定返回 0。这个判据更硬。
    //
    // 没有控制台时（输出被重定向到文件/管道、或从 GUI 程序启动）什么都
    // 不做：此时改代码页没有意义，而字节流本来就该是 UTF-8，无需转换。
    UINT cur = GetConsoleOutputCP();
    if (cur == 0) return;

    if (cur == CP_UTF8 && GetConsoleCP() == CP_UTF8) {
        // 已经是 UTF-8 就不用改，但仍然要开 VT 处理，所以往下走
        s_cp_saved = 0;
    } else {
        s_prev_out_cp = cur;
        s_prev_in_cp  = GetConsoleCP();
        s_cp_saved    = 1;
        atexit(restore_console_cp);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    // 开 ANSI 转义序列（颜色 + OSC 8 超链接）。不加这个，日志里的
    // ESC[31m 会原样打印出来。ENABLE_VIRTUAL_TERMINAL_PROCESSING
    // 需要 Windows 10 1511 以上；老系统上失败就静默降级为无颜色。
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    if (hErr != INVALID_HANDLE_VALUE && GetConsoleMode(hErr, &mode)) {
        SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
    // 类 Unix：默认假设终端已是 UTF-8。
    //
    // 刻意不调 setlocale(LC_ALL, "")：那会把数字格式化、字符分类等
    // 行为绑到系统 locale 上，同一个程序在不同机器上算出不同的
    // toString(0.1) —— 那会破坏「两档输出逐字节一致」这条承诺。
    // UTF-8 处理完全由我们自己的 utf8.c 负责，不依赖 libc 的 locale。
}

// ============================================================
// stdio 二进制模式（LSP 的通信底座）
// ============================================================
//
// LSP over stdio 的分帧靠精确的字节序列：头和体之间是一个
// "\r\n\r\n"，Content-Length 按 UTF-8 字节数算。Windows 的 C 运行库
// 默认把 stdin/stdout 当**文本流**，写入时把每个 '\n' 翻译成
// "\r\n"。于是我们精心写出的 "\r\n\r\n" 落到管道里成了
// "\r\r\n\r\r\n"，客户端按 "\r\n\r\n" 找帧头永远找不到 —— 表现是
// 「语言服务器启动了，但一直不响应」，而服务端自己看不出任何异常
// （它确实把字节写出来了，只是多了一层翻译）。
//
// 所以在进入消息循环前把两个标准流都切到二进制模式，一次性关掉
// 翻译，比在每个 printf 里提防要可靠。只有 Windows 有这个问题；
// 类 Unix 上文本/二进制模式没有区别，直接空实现。
//
// 可重复调用（_setmode 幂等），但必须在任何读写 stdin/stdout 之前
// 调 —— 已经写出去的字节不会被"修正"。
void plat_set_binary_stdio(void) {
#if X_HOST_WINDOWS
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// ============================================================
// 编码转换
// ============================================================

#if X_HOST_WINDOWS

// Windows 的两步转换：ANSI ↔ UTF-8 必须经过宽字符中转。
// 没有直接从 CP_ACP 到 CP_UTF8 的 API，所以中间必然有 wchar_t 缓冲。
static char* win_conv(UINT from, UINT to, const char* s) {
    int wlen = MultiByteToWideChar(from, 0, s, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!w) return NULL;
    if (MultiByteToWideChar(from, 0, s, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    int olen = WideCharToMultiByte(to, 0, w, -1, NULL, 0, NULL, NULL);
    if (olen <= 0) {
        free(w);
        return NULL;
    }
    char* out = (char*)malloc((size_t)olen);
    if (!out) {
        free(w);
        return NULL;
    }
    if (WideCharToMultiByte(to, 0, w, -1, out, olen, NULL, NULL) <= 0) {
        free(w);
        free(out);
        return NULL;
    }
    free(w);
    return out;
}

#endif  // X_HOST_WINDOWS

char* plat_from_locale(const char* s) {
    if (!s) return NULL;
#if X_HOST_WINDOWS
    // Windows 上 main 收到的 argv 是 ANSI 代码页（简体中文系统里是 GBK）。
    // 中文路径直接拿来 fopen 恰好能工作，但一旦拿去打印、拼进诊断、
    // 或写进 .xbc 的文件名字段，就会和 UTF-8 的其它字符串混在一起，
    // 出现「一半 GBK 一半 UTF-8」的字节串。所以入口处统一转成 UTF-8。
    return win_conv(CP_ACP, CP_UTF8, s);
#else
    // 类 Unix 的 argv 已经是字节串，约定为 UTF-8，直接拷贝。
    char* copy = (char*)malloc(strlen(s) + 1);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
#endif
}

char* plat_to_locale(const char* s) {
    if (!s) return NULL;
#if X_HOST_WINDOWS
    // 内部一律 UTF-8，但 fopen/remove/rename 在 Windows 上看的是 ANSI
    // 代码页。中文路径若直接把 UTF-8 字节交给 fopen，得到的是 ENOENT，
    // 而诊断里打印的路径又是对的（因为诊断用的是 UTF-8）——这种
    // 「路径看着完全正确但打不开」的 bug 最难查。所以显式转一趟。
    return win_conv(CP_UTF8, CP_ACP, s);
#else
    char* copy = (char*)malloc(strlen(s) + 1);
    if (!copy) return NULL;
    strcpy(copy, s);
    return copy;
#endif
}

// ============================================================
// 终端能力探测
// ============================================================

bool plat_is_tty(FILE* f) {
    if (!f) return false;
    return plat_isatty(plat_fileno(f)) != 0;
}

bool plat_supports_color(FILE* f) {
    if (s_color_disabled) return false;
    if (!plat_is_tty(f)) return false;

#if X_HOST_WINDOWS
    // Windows 10+ 的终端支持 ANSI，但需要能区分「真终端」与「老 cmd」。
    // 判据用环境变量：Windows Terminal 设 WT_SESSION，VS Code 设
    // TERM_PROGRAM。两者都没有时，只有 TERM 存在才认为支持
    // （ConEmu / Cmder 会设 TERM）。
    //
    // 这个判据是保守的：误判为「不支持」只是少点颜色，
    // 误判为「支持」会把 ESC[31m 原样打进日志。宁可少颜色。
    const char* wt   = getenv("WT_SESSION");
    if (wt && *wt) return true;
    const char* prog = getenv("TERM_PROGRAM");
    if (prog && *prog) return true;
    const char* term = getenv("TERM");
    if (term && *term) return true;
    return false;
#else
    // 类 Unix 的判据简单得多：有 TERM 且不是 "dumb" 就支持。
    const char* term = getenv("TERM");
    if (!term || !*term) return false;
    if (strcmp(term, "dumb") == 0) return false;
    return true;
#endif
}

bool plat_supports_osc8(FILE* f) {
    if (s_color_disabled) return false;
    if (!plat_is_tty(f)) return false;

    // OSC 8 比颜色更挑终端，判据也更严：只认明确支持的几家。
    // 不支持时降级为纯文本 "file:line:col"，功能不丢，只是不能点。
#if X_HOST_WINDOWS
    const char* wt = getenv("WT_SESSION");
    if (wt && *wt) return true;
    const char* prog = getenv("TERM_PROGRAM");
    if (prog && (strstr(prog, "vscode") || strstr(prog, "iTerm"))) return true;
    return false;
#else
    const char* term = getenv("TERM");
    if (!term) return false;
    if (strstr(term, "xterm")     || strstr(term, "screen") ||
        strstr(term, "tmux")      || strstr(term, "kitty")  ||
        strstr(term, "alacritty") || strstr(term, "wezterm") ||
        strstr(term, "foot")) return true;
    // TERM_PROGRAM 在某些终端里比 TERM 更可信
    const char* prog = getenv("TERM_PROGRAM");
    if (prog && (strstr(prog, "vscode") || strstr(prog, "iTerm") ||
                 strstr(prog, "Apple_Terminal"))) return true;
    return false;
#endif
}

void plat_disable_color(void) {
    s_color_disabled = true;
}
// ============================================================
// 子进程
// ============================================================

#if X_HOST_WINDOWS

// 把 argv 拼成 CreateProcess 要的命令行。
//
// Windows 的命令行拼接口有它自己的转义规则，与 shell 完全不同：
// 一个参数里 n 个反斜杠后面跟引号，要写成 2n+1 个反斜杠加引号。
// 写错会让路径里的空格或引号被误解。
//
// 这里按 CommandLineToArgvW 的逆规则实现，只处理我们实际会遇到的
// 字符（路径里的空格、反斜杠、引号）。
static void append_quoted(char* buf, size_t cap, const char* arg) {
    size_t len = strlen(buf);
    bool need_quote = false;
    for (const char* p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') { need_quote = true; break; }
    }
    if (!need_quote && *arg) {
        snprintf(buf + len, cap - len, "%s ", arg);
        return;
    }
    snprintf(buf + len, cap - len, "\"");
    len = strlen(buf);
    for (const char* p = arg; *p; p++) {
        if (*p == '"') {
            snprintf(buf + len, cap - len, "\\\"");
            len = strlen(buf);
        } else {
            buf[len++] = *p;
            buf[len] = '\0';
        }
    }
    snprintf(buf + len, cap - len, "\" ");
}

// 内部：真正调用 CreateProcess 的那一层。envblock 是已经摊平的连续块，
// 或者 NULL 表示继承。
static int spawn_ex(char* const argv[], const char* workdir, char* envblock) {
    static char cmdline[8192];
    cmdline[0] = '\0';
    for (int i = 0; argv[i]; i++) append_quoted(cmdline, sizeof(cmdline), argv[i]);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    // lpApplicationName 用 NULL，让系统从命令行第一段解析 —— 这样
    // 与 shell 的行为一致，也支持 "gcc" 这种只有名字的调用（走 PATH）。
    BOOL ok = CreateProcessA(
        NULL, cmdline,
        NULL, NULL,
        TRUE,                        // 继承句柄（stdout 要能被继承）
        0,
        envblock,                    // ★ 必须是连续块，不能是指针数组
        workdir,                     // ★ 关键：能指定 ASCII 工作目录
        &si, &pi);

    if (!ok) {
        // 诊断输出走环境变量开关，默认安静。返回 -1 与「子进程以非零码退出」
        // 是两类完全不同的失败：前者是进程根本没起来（路径/环境块/权限），
        // 后者是编译器自己报错。不区分这两者会让排查方向完全跑偏。
        if (getenv("X_SPAWN_DEBUG")) {
            fprintf(stderr, "[spawn] CreateProcessA failed, GetLastError=%lu\n",
                    (unsigned long)GetLastError());
            fprintf(stderr, "[spawn] cmdline = %s\n", cmdline);
            fprintf(stderr, "[spawn] workdir = %s\n", workdir ? workdir : "(null)");
            fprintf(stderr, "[spawn] env     = %s\n",
                    envblock ? "custom (flattened block)" : "(inherit)");
        }
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int plat_spawn(char* const argv[], const char* workdir) {
    return spawn_ex(argv, workdir, NULL);
}

int plat_spawn_with_tmp(char* const argv[], const char* workdir, const char* tmpdir) {
    if (!tmpdir || !tmpdir[0]) return spawn_ex(argv, workdir, NULL);

    // CreateProcess 的 lpEnvironment 要求**一整块连续内存**，里面是
    // 一串 "K=V\0K=V\0...\0\0"。只把指针数组强转成 LPVOID 传进去会得到
    // ERROR_INVALID_PARAMETER (87) —— 因为 _environ 的各个字符串在堆上
    // 是分散的，CreateProcess 顺着第一段往后读，读到第二个指针就崩。
    // 这是本次踩坑的直接原因，必须摊平。
    char** src = _environ;
    int cnt = 0;
    while (src && src[cnt]) cnt++;

    size_t cap = 0;
    for (int i = 0; i < cnt; i++) cap += strlen(src[i]) + 1;
    cap += strlen(tmpdir) * 2 + 32;

    char* blk = (char*)malloc(cap);
    if (!blk) return -1;

    size_t o = 0;
    for (int i = 0; i < cnt; i++) {
        // 丢掉原有的 TMP / TEMP（不区分大小写），下面统一替换
        if (_strnicmp(src[i], "TMP=", 4) == 0)  continue;
        if (_strnicmp(src[i], "TEMP=", 5) == 0) continue;
        size_t n = strlen(src[i]) + 1;
        memcpy(blk + o, src[i], n);
        o += n;
    }
    int w = snprintf(blk + o, cap - o, "TMP=%s", tmpdir);
    o += (size_t)w + 1;
    w = snprintf(blk + o, cap - o, "TEMP=%s", tmpdir);
    o += (size_t)w + 1;
    blk[o] = '\0';   // 双 \0 结尾：上一个字符串自带一个，这里再补一个

    int rc = spawn_ex(argv, workdir, blk);
    free(blk);
    return rc;
}

void plat_abs_path(const char* in, char* out, size_t out_len) {
    // 不能用 _fullpath：它按 ANSI 代码页处理输入输出，中文路径会变乱码。
    // 正确路径是 UTF-8 → 宽字符 → _wfullpath → UTF-8。
    // 这个 bug 的实际表现是「产物路径打印成乱码，随后复制失败」，
    // 而编译本身是成功的，很容易误判成权限问题。
    // ── 这里曾经有一个 bug，值得记下来 ────────────────────────
    //
    // 原实现第一步调了 plat_from_locale(in)，把输入「从本地代码页
    // 转成 UTF-8」。但 plat_abs_path 的**契约就是输入已经是 UTF-8**
    // ——它所有的调用点传的都是 X 语言侧拼出来的 UTF-8 路径。
    //
    // 于是「UTF-8 → 本地代码页解读 → 再当 UTF-8」形成二次转换：
    // 输入 F:\项目 的 UTF-8 字节 E9 A1 B9 E7 9B AE 被按 GBK 解读成
    // 「椤圭洰」，再转回 UTF-8 得到另一个字节串。最终 out 里是
    // 一个不存在的路径，报错是
    //     error: 无法打开文件 `F:\椤圭洰\x-project\...`：No such file
    // 编译器的其他部分全都正确，只有这一个函数把路径搞坏了。
    //
    // 教训：带编码转换的函数必须把「输入是什么编码、输出是什么编码」
    // 写进契约，而不是靠调用者记住。
    if (!in || !in[0]) {
        snprintf(out, out_len, "%s", in ? in : "");
        return;
    }

    const char* src = in;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (wlen <= 0) {
        snprintf(out, out_len, "%s", in);
        return;
    }
    wchar_t* w_in_buf = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!w_in_buf) {
        snprintf(out, out_len, "%s", in);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, w_in_buf, wlen);

    wchar_t w_abs[1024];
    bool ok = _wfullpath(w_abs, w_in_buf, 1024) != NULL;
    free(w_in_buf);
    if (!ok) {
        snprintf(out, out_len, "%s", in);
        return;
    }

    int olen = WideCharToMultiByte(CP_UTF8, 0, w_abs, -1, NULL, 0, NULL, NULL);
    if (olen <= 0 || (size_t)olen > out_len) {
        snprintf(out, out_len, "%s", in);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, w_abs, -1, out, (int)out_len, NULL, NULL);
}

#else  // 类 Unix

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

int plat_spawn(char* const argv[], const char* workdir) {
    // posix_spawn 支持指定 cwd，要过 posix_spawnattr + file_actions。
    // 但类 Unix 上不存在「.o 落在中文路径下会失败」这个问题
    // （文件系统本来就是字节串），所以 workdir 通常为 NULL。
    if (!workdir) {
        pid_t pid;
        int st = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
        if (st != 0) return -1;
        int status;
        if (waitpid(pid, &status, 0) < 0) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // 需要切目录：用 posix_spawn_file_actions_addchdir_np（glibc 2.29+）。
    // 没有这个扩展时退化为「不管 workdir」——类 Unix 上这不影响正确性。
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    posix_spawn_file_actions_addchdir_np(&fa, workdir);
#else
    (void)workdir;
#endif
    pid_t pid;
    int st = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (st != 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int plat_spawn_with_tmp(char* const argv[], const char* workdir, const char* tmpdir) {
    // 类 Unix 上文件系统是字节串，不存在「临时目录路径含非 ASCII 就编不过」
    // 这个问题（tools/BUILD.md 坑 3 是 Windows 独有）。tmpdir 直接忽略。
    // 保留这个参数是为了让调用方的代码在三平台上完全一致。
    (void)tmpdir;
    return plat_spawn(argv, workdir);
}

void plat_abs_path(const char* in, char* out, size_t out_len) {
    // 类 Unix 上 realpath 要求文件存在；这里用「以 / 开头就是绝对路径」
    // 的简单判断，其余情况原样返回（调用方负责传绝对路径）。
    if (in && in[0] == '/') {
        snprintf(out, out_len, "%s", in);
    } else {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(out, out_len, "%s/%s", cwd, in);
        } else {
            snprintf(out, out_len, "%s", in);
        }
    }
}

#endif  // X_HOST_WINDOWS

bool plat_getcwd(char* out, size_t out_len) {
    if (!out || out_len == 0) return false;
#if X_HOST_WINDOWS
    // 为什么不用 getcwd：
    //
    // MSVCRT 的 getcwd 返回的是 **ANSI 代码页**的字节流（简体中文
    // 系统上就是 GBK）。项目路径是 F:/项目/... 时，拿到的是一串 GBK
    // 字节。这串东西：
    //   · 拼进 UTF-8 的诊断信息 → 打印成乱码
    //   · 和 arena 里的 UTF-8 路径比较 → 永远不相等（去重失效）
    //   · 传给 x_fopen（内部转宽字符）→ 转错
    //
    // 而 GetCurrentDirectoryW 拿的是宽字符，由 win_conv 转成 UTF-8，
    // 与程序内部其它字符串一致。
    DWORD need = GetCurrentDirectoryW(0, NULL);
    if (need == 0) return false;
    wchar_t* w = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
    if (!w) return false;
    DWORD got = GetCurrentDirectoryW(need, w);
    if (got == 0 || got >= need) {
        free(w);
        return false;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    bool ok = false;
    if (bytes > 0 && (size_t)bytes <= out_len) {
        ok = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, bytes, NULL, NULL) > 0;
    }
    free(w);
    return ok;
#else
    // 类 Unix：文件系统是字节串，getcwd 直接可用，约定为 UTF-8。
    return getcwd(out, out_len) != NULL;
#endif
}

bool plat_is_ascii(const char* s) {
    if (!s) return true;
    for (const char* p = s; *p; p++) {
        if ((unsigned char)*p >= 0x80) return false;
    }
    return true;
}

// 取可执行文件所在目录。理由见 platform.h。
bool plat_exe_dir(char* out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';

#if X_HOST_WINDOWS
    // ── 这里曾经有个「看起来更优雅」的写法 ────────────────────
    //
    // 原来的第一行是：
    //     DWORD need = GetModuleFileNameW(NULL, NULL, 0);
    // 意图是「先用 0 长度问一次需要多少字符，再按需分配」。
    //
    // 这个用法是错的。GetModuleFileNameW **不支持** 传 NULL 查询长度：
    // nSize 传 0 时它直接失败并返回 0（GetLastError 给
    // ERROR_INSUFFICIENT_BUFFER），于是 need == 0 → 整个函数返回 false。
    //
    // 后果比「AOT 少个功能」严重得多：plat_exe_dir 是标准库定位的
    // 第一顺位（<exe 目录>/stdlib），它一旦恒失败，就只剩下
    // <cwd>/stdlib 兜底。于是「x.exe 和 stdlib 放一个文件夹，
    // 拷到哪都能 import」这个核心诉求**只在 cwd 恰好是那个文件夹
    // 时才成立** —— 在子目录里跑、或者从别处用绝对路径调用，
    // 都会报「找不到标准库模块 std.fs」。
    //
    // 正确做法：先给一个足够大的栈缓冲，如果返回值等于缓冲容量
    // （表示可能被截断），再把缓冲翻倍重试。MAX_PATH 是 260，
    // 但长路径开启后可以到 32767，所以循环直到成功或到达上限。
    {
        DWORD cap = 512;
        wchar_t* w = NULL;
        DWORD got = 0;
        for (;;) {
            wchar_t* nw = (wchar_t*)realloc(w, ((size_t)cap) * sizeof(wchar_t));
            if (!nw) { free(w); return false; }
            w = nw;
            got = GetModuleFileNameW(NULL, w, cap);
            if (got == 0) { free(w); return false; }
            if (got < cap - 1) break;      // 未截断
            if (cap >= 32768) { free(w); return false; }   // 到上限仍不齐
            cap *= 2;
        }

    // 砍掉文件名部分，保留目录（与 plat_getcwd 的约定一致：不带结尾分隔符）
    {
        wchar_t* slash = w;
        for (wchar_t* p = w; *p; p++) {
            if (*p == L'\\' || *p == L'/') slash = p;
        }
        *slash = L'\0';
    }

    int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    bool ok = false;
    if (bytes > 0 && (size_t)bytes <= out_len) {
        ok = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, bytes, NULL, NULL) > 0;
    }
    free(w);
    return ok && out[0] != '\0';
    }

#else
    // 类 Unix。Linux 用 /proc/self/exe，macOS 用 _NSGetExecutablePath。
    //
    // 两个都要给：/proc 在 macOS 上不存在，而 _NSGetExecutablePath 是
    // macOS 独有的。只写一支的话，另一个平台编译期就报未定义符号。
#  if defined(__APPLE__)
    extern int _NSGetExecutablePath(char* buf, unsigned int* bufsize);
    char tmp[4096];
    unsigned int sz = sizeof(tmp);
    if (_NSGetExecutablePath(tmp, &sz) != 0) return false;
#  else
    char tmp[4096];
    ssize_t n = readlink("/proc/self/exe", tmp, sizeof(tmp) - 1);
    if (n <= 0) return false;
    tmp[n] = '\0';
#  endif

    // 砍掉文件名
    char* slash = strrchr(tmp, '/');
    if (!slash) return false;
    *slash = '\0';
    // 根目录下的程序（/x）砍完是空串，此时目录就是 "/"
    if (tmp[0] == '\0') { snprintf(out, out_len, "/"); return true; }
    if (strlen(tmp) >= out_len) return false;
    memcpy(out, tmp, strlen(tmp) + 1);
    return true;
#endif
}

long plat_pid(void) {
#if X_HOST_WINDOWS
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

// ============================================================
// 目录遍历
// ============================================================
//
// 声明与理由见 platform.h。要点：Windows 用 FindFirstFileW（中文路径
// 必须走宽字符），类 Unix 用 opendir/readdir 并自己匹配 pattern。

#if X_HOST_WINDOWS

// 把 "*/*?" 通配模式与名字做匹配。Windows 的 FindFirstFileW 已经帮我们
// 匹配了，但为了与类 Unix 分支的行为完全一致（尤其是 "*.xle" 这种
// 多点文件名），这里统一用同一套匹配逻辑，不依赖系统差异。
static bool glob_match(const char* pat, const char* name) {
    const char* p = pat;
    const char* n = name;
    const char* star_p = NULL;
    const char* star_n = NULL;

    while (*n) {
        if (*p == '*') {
            star_p = ++p;
            star_n = n;
        } else if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (star_p) {
            p = star_p;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

struct WinFind {
    HANDLE h;
    bool   pending;                 // 第一次 Open 时已经取到的项还没返回
    WIN32_FIND_DATAW first_entry;
};

bool plat_dir_open(PlatDir* d, const char* dir, const char* pattern) {
    if (!d) return false;
    memset(d, 0, sizeof(*d));
    snprintf(d->pattern, sizeof(d->pattern), "%s", pattern ? pattern : "*");

    // 拼 目录\*，让系统做第一层筛选（减少无用遍历）
    char full[1200];
    snprintf(full, sizeof(full), "%s\\*", dir);

    wchar_t* w = NULL;
    {
        // 直接把 UTF-8 转成宽字符。platform.c 不能调 utf8_to_locale ——
        // 那个在 util.c，属于更上层；平台层必须自足，否则会形成
        // platform ← util ← platform 的循环依赖。
        // 程序内部路径统一是 UTF-8，所以这里直接按 CP_UTF8 解。
        int need = MultiByteToWideChar(CP_UTF8, 0, full, -1, NULL, 0);
        if (need <= 0) return false;
        w = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
        if (!w) return false;
        MultiByteToWideChar(CP_UTF8, 0, full, -1, w, need);
    }

    // FindFirstFileW 是「打开 + 取第一项」二合一：它在返回 HANDLE 的同时
    // 把第一项写进 out 参数。所以第一项必须缓存下来，由第一次 next 返回。
    //
    // 踩过的坑：如果 open 时取了第一项却不缓存，next 直接调 FindNextFileW，
    // 结果就是**永远少一个文件**，且少的是哪个由文件系统顺序决定 ——
    // 表现为「stdlib 里明明有 json.xle，x stdlib 却没列出来」，
    // 换台机器又好了。缓存是第一项唯一的正确处理方式。
    struct WinFind* st = (struct WinFind*)calloc(1, sizeof(*st));
    if (!st) { free(w); return false; }

    st->h = FindFirstFileW(w, &st->first_entry);
    free(w);
    if (st->h == INVALID_HANDLE_VALUE) { free(st); return false; }
    st->pending = true;
    d->handle = st;
    return true;
}

bool plat_dir_next(PlatDir* d) {
    if (!d || !d->handle) return false;
    struct WinFind* st = (struct WinFind*)d->handle;

    WIN32_FIND_DATAW fd;
    BOOL ok;
    if (st->pending) {
        fd = st->first_entry;
        st->pending = false;
        ok = TRUE;
    } else {
        ok = FindNextFileW(st->h, &fd);
    }
    if (!ok) return false;

    // 转 UTF-8 到 d->buf
    int bytes = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0 || (size_t)bytes > sizeof(d->buf)) return plat_dir_next(d);
    WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, d->buf, bytes, NULL, NULL);

    // "." 与 ".." 一律滤掉 —— 交给调用方滤的话，忘一次就多出两项，
    // 而 ".." 被当成模块名时的错误信息会非常费解。
    if (strcmp(d->buf, ".") == 0 || strcmp(d->buf, "..") == 0) return plat_dir_next(d);
    if (!glob_match(d->pattern, d->buf)) return plat_dir_next(d);

    d->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->name = d->buf;
    return true;
}

void plat_dir_close(PlatDir* d) {
    if (!d || !d->handle) return;
    struct WinFind* st = (struct WinFind*)d->handle;
    FindClose(st->h);
    free(st);
    d->handle = NULL;
}

#else

#include <dirent.h>

typedef struct {
    DIR* dir;
    char dirpath[1100];
} UnixFind;

static bool glob_match(const char* pat, const char* name) {
    const char* p = pat;
    const char* n = name;
    const char* star_p = NULL;
    const char* star_n = NULL;
    while (*n) {
        if (*p == '*') { star_p = ++p; star_n = n; }
        else if (*p == '?' || *p == *n) { p++; n++; }
        else if (star_p) { p = star_p; n = ++star_n; }
        else return false;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

bool plat_dir_open(PlatDir* d, const char* dir, const char* pattern) {
    if (!d) return false;
    memset(d, 0, sizeof(*d));
    snprintf(d->pattern, sizeof(d->pattern), "%s", pattern ? pattern : "*");

    UnixFind* st = (UnixFind*)malloc(sizeof(*st));
    if (!st) return false;
    st->dir = opendir(dir);
    if (!st->dir) { free(st); return false; }
    snprintf(st->dirpath, sizeof(st->dirpath), "%s", dir);
    d->handle = st;
    return true;
}

bool plat_dir_next(PlatDir* d) {
    if (!d || !d->handle) return false;
    UnixFind* st = (UnixFind*)d->handle;

    struct dirent* e;
    while ((e = readdir(st->dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!glob_match(d->pattern, e->d_name)) continue;
        snprintf(d->buf, sizeof(d->buf), "%s", e->d_name);

        // d_type 在部分文件系统上是 DT_UNKNOWN，此时退回 stat
        if (e->d_type == DT_DIR) {
            d->is_dir = true;
        } else if (e->d_type == DT_UNKNOWN) {
            char full[1400];
            snprintf(full, sizeof(full), "%s/%s", st->dirpath, e->d_name);
            struct stat sb;
            d->is_dir = (stat(full, &sb) == 0) && S_ISDIR(sb.st_mode);
        } else {
            d->is_dir = false;
        }
        d->name = d->buf;
        return true;
    }
    return false;
}

void plat_dir_close(PlatDir* d) {
    if (!d || !d->handle) return;
    UnixFind* st = (UnixFind*)d->handle;
    closedir(st->dir);
    free(st);
    d->handle = NULL;
}

#endif

// ============================================================
// 文件读写
// ============================================================
//
// 声明与理由见 platform.h。这里只记实现上的两个决定：
//
//   · 读写都用「打开 → 循环 → 关闭」而不是映射内存。数据库文件
//     通常只有几十 KB，映射的收益抵不上它的复杂度（还要处理
//     映射失败、跨平台 mremap）。
//
//   · 路径全部按 UTF-8 进，Windows 侧转 UTF-16。这条转换必须
//     走 plat_from_locale 的同一套工具，否则含中文的路径在
//     非 936 代码页的机器上会打不开 —— 而那正是"用户目录里有
//     中文用户名"这个最常见的场景。

#if X_HOST_WINDOWS
#  define plat_fopen_x(path, mode) \
      _wfopen(plat_utf8_to_wide(path), L##mode)
#else
#  define plat_fopen_x(path, mode) fopen(path, mode)
#endif

char* plat_file_read(const char* path, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    FILE* f = plat_fopen_x(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    // +1 让「空文件」也能返回一个可 free 的非 NULL 指针。
    // 这是有意的：调用方靠 NULL 判「打不开」，靠长度 0 判「空」。
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = 0;
    while (got < (size_t)sz) {
        size_t n = fread(buf + got, 1, (size_t)sz - got, f);
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

bool plat_file_write(const char* path, const char* data, size_t len) {
    if (!path) return false;
    FILE* f = plat_fopen_x(path, "wb");
    if (!f) return false;
    bool ok = true;
    if (len > 0 && fwrite(data, 1, len, f) != len) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool plat_file_append(const char* path, const char* data, size_t len) {
    if (!path) return false;
    FILE* f = plat_fopen_x(path, "ab");
    if (!f) return false;
    bool ok = true;
    if (len > 0 && fwrite(data, 1, len, f) != len) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool plat_file_exists(const char* path) {
    if (!path) return false;
#if X_HOST_WINDOWS
    return _waccess(plat_utf8_to_wide(path), 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

bool plat_file_remove(const char* path) {
    if (!path) return false;
#if X_HOST_WINDOWS
    return _wremove(plat_utf8_to_wide(path)) == 0;
#else
    return remove(path) == 0;
#endif
}

long plat_file_size(const char* path) {
    if (!path) return -1;
#if X_HOST_WINDOWS
    struct _stat64 st;
    if (_wstat64(plat_utf8_to_wide(path), &st) != 0) return -1;
    return (long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
#endif
}

bool plat_mkdir(const char* path) {
    if (!path) return false;
#if X_HOST_WINDOWS
    if (_wmkdir(plat_utf8_to_wide(path)) == 0) return true;
    return plat_is_dir(path);      // 已存在也算成功
#else
    if (mkdir(path, 0755) == 0) return true;
    return plat_is_dir(path);
#endif
}

bool plat_is_dir(const char* path) {
    if (!path) return false;
#if X_HOST_WINDOWS
    DWORD a = GetFileAttributesW(plat_utf8_to_wide(path));
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

// 原子替换。临时文件名带进程号，避免两个进程同时写同一目标时
// 互相覆盖对方的临时文件 —— 那会让"原子"这件事失效。
bool plat_file_replace(const char* path, const char* data, size_t len) {
    if (!path) return false;
    size_t n = strlen(path);
    char* tmp = (char*)malloc(n + 32);
    if (!tmp) return false;
    snprintf(tmp, n + 32, "%s.tmp%ld", path, plat_pid());

    bool ok = plat_file_write(tmp, data, len);
    if (ok) {
#if X_HOST_WINDOWS
        // Windows 上 rename 到已存在的文件会失败，所以要先把
        // 旧文件挪开。这个窗口不是原子的，但它是当前 API 能给
        // 的最接近的做法（ReplaceFile 需要目标已存在，语义不同）。
        plat_file_remove(path);
        ok = _wrename(plat_utf8_to_wide(tmp), plat_utf8_to_wide(path)) == 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) plat_file_remove(tmp);
    free(tmp);
    return ok;
}

// ============================================================
// TCP
// ============================================================
//
// ── 非阻塞是刻意的 ──────────────────────────────────────────
//
// 服务器写成「每帧 poll 一次」的即时模式（与 std.ui 同一个思路），
// 这样单线程就能同时处理「接受连接 / 读请求 / 该退出了吗」。阻塞
// 的 accept 会让空闲的服务器卡死在一个调用里，连退出信号都收不到。
//
// ── 关于错误信息的取舍 ──────────────────────────────────────
//
// 每个失败路径都把系统错误码转成一句人话存进 s_net_err。
// 「连不上」对用户没有任何指向性；「连不上：Connection refused」
// 直接告诉他端口没人监听。这个差别在排障时是决定性的。

#if X_HOST_WINDOWS
static char s_net_err[512] = "";

// ── 为什么必须用 FormatMessageW 而不是 FormatMessageA ────────
//
// FormatMessageA 返回的是**本地代码页**编码的字符串。中文 Windows
// 上那是 GBK，而本项目的所有输出（println、诊断、错误消息）一律
// 是 UTF-8。把 GBK 字节直接塞进 UTF-8 字符串，用户看到的是：
//
//   连接 127.0.0.1:18081 失败：连接失败（10061: Ŀ...）
//
// 这条消息本来是「目标计算机积极拒绝，无法连接」—— 它是这个
// 错误里**唯一有指向性**的部分。变成乱码之后，用户只知道「连接
// 失败」，而不知道是端口没人听、防火墙拦了、还是主机名写错。
//
// 修法：用 W 版拿宽字符，再按 CP_UTF8 转回来。这与 plat_abs_path
// 里「不能用 _fullpath，要用 _wfullpath」是同一个根因 —— Windows
// 上凡是路径与系统消息，都必须走宽字符再显式转码。
static void net_set_err(const char* what) {
    int code = WSAGetLastError();
    wchar_t* wmsg = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD)code, 0, (LPWSTR)&wmsg, 0, NULL);
    if (n && wmsg) {
        // 去掉系统消息末尾的换行（Windows 的格式普遍带 \r\n）
        size_t L = wcslen(wmsg);
        while (L > 0 && (wmsg[L-1] == L'\n' || wmsg[L-1] == L'\r')) wmsg[--L] = L'\0';

        // 先转 UTF-8。转不出来（极少见）就只报错误码，不硬塞乱码。
        char utf8[400];
        int got = WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, utf8, sizeof(utf8), NULL, NULL);
        if (got > 0) {
            snprintf(s_net_err, sizeof(s_net_err), "%s（%d: %s）", what, code, utf8);
        } else {
            snprintf(s_net_err, sizeof(s_net_err), "%s（错误码 %d）", what, code);
        }
        LocalFree(wmsg);
    } else {
        snprintf(s_net_err, sizeof(s_net_err), "%s（错误码 %d）", what, code);
    }
}
#else
static char s_net_err[256] = "";

static void net_set_err(const char* what) {
    int code = errno;
    snprintf(s_net_err, sizeof(s_net_err), "%s（%s）", what, strerror(code));
}
#endif

const char* plat_net_error(void) { return s_net_err; }

bool plat_sock_valid(PlatSock s) {
#if X_HOST_WINDOWS
    return s.v != -1 && s.v != (long long)INVALID_SOCKET;
#else
    return s.v >= 0;
#endif
}

#if X_HOST_WINDOWS
static int s_wsa_refs = 0;

bool plat_net_init(void) {
    if (s_wsa_refs > 0) { s_wsa_refs++; return true; }
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        net_set_err("无法初始化网络子系统（WSAStartup 失败）");
        return false;
    }
    s_wsa_refs = 1;
    return true;
}

void plat_net_shutdown(void) {
    if (s_wsa_refs > 0) {
        s_wsa_refs--;
        if (s_wsa_refs == 0) WSACleanup();
    }
}

// 把 socket 设成非阻塞。所有 accept 出来的连接都要走这一步，
// 否则 tcpRecv 会永久阻塞在「对端还没发数据」上。
static bool set_nonblock(SOCKET s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

PlatSock plat_tcp_listen(const char* bind_addr, int port) {
    if (!plat_net_init()) return PLAT_SOCK_INVALID;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { net_set_err("创建监听 socket 失败"); return PLAT_SOCK_INVALID; }

    // SO_REUSEADDR：进程刚退出时端口处于 TIME_WAIT，不设这一项
    // 会让「改一行代码重跑服务器」连续失败几十秒。
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)port);
    if (bind_addr && bind_addr[0] && strcmp(bind_addr, "0.0.0.0") != 0) {
        a.sin_addr.s_addr = inet_addr(bind_addr);
        if (a.sin_addr.s_addr == INADDR_NONE) a.sin_addr.s_addr = INADDR_ANY;
    } else {
        a.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        net_set_err("绑定端口失败"); closesocket(s); return PLAT_SOCK_INVALID;
    }
    if (listen(s, SOMAXCONN) != 0) {
        net_set_err("监听失败"); closesocket(s); return PLAT_SOCK_INVALID;
    }
    if (!set_nonblock(s)) {
        net_set_err("无法把监听 socket 设为非阻塞");
        closesocket(s);
        return PLAT_SOCK_INVALID;
    }
    return (PlatSock){ .v = (long long)s };
}
#else
// ---- 类 Unix ----
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>

bool plat_net_init(void) { return true; }
void plat_net_shutdown(void) {}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

PlatSock plat_tcp_listen(const char* bind_addr, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { net_set_err("创建监听 socket 失败"); return PLAT_SOCK_INVALID; }

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (bind_addr && bind_addr[0] && strcmp(bind_addr, "0.0.0.0") != 0) {
        a.sin_addr.s_addr = inet_addr(bind_addr);
        if (a.sin_addr.s_addr == INADDR_NONE) a.sin_addr.s_addr = INADDR_ANY;
    } else {
        a.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        net_set_err("绑定端口失败"); close(s); return PLAT_SOCK_INVALID;
    }
    if (listen(s, SOMAXCONN) != 0) {
        net_set_err("监听失败"); close(s); return PLAT_SOCK_INVALID;
    }
    set_nonblock(s);
    return (PlatSock){ .v = s };
}
#endif

PlatSock plat_tcp_accept(PlatSock listener) {
    if (!plat_sock_valid(listener)) return PLAT_SOCK_INVALID;
#if X_HOST_WINDOWS
    SOCKET c = accept((SOCKET)listener.v, NULL, NULL);
    if (c == INVALID_SOCKET) return PLAT_SOCK_INVALID;   // 没有待处理连接
    if (!set_nonblock(c)) { closesocket(c); return PLAT_SOCK_INVALID; }
    // Nagle 关掉：HTTP 响应通常一次写完，攒包只会增加延迟
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return (PlatSock){ .v = (long long)c };
#else
    int c = accept((int)listener.v, NULL, NULL);
    if (c < 0) return PLAT_SOCK_INVALID;
    set_nonblock(c);
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return (PlatSock){ .v = c };
#endif
}

long plat_tcp_recv(PlatSock s, char* buf, size_t cap) {
    if (!plat_sock_valid(s) || !buf || cap == 0) return -1;
#if X_HOST_WINDOWS
    int n = recv((SOCKET)s.v, buf, (int)cap, 0);
    if (n == 0) return 0;                       // 对端正常关闭
    if (n == SOCKET_ERROR) return -1;
    return (long)n;
#else
    ssize_t n = recv((int)s.v, buf, cap, 0);
    if (n == 0) return 0;
    if (n < 0) return -1;                        // EAGAIN 也走这里
    return (long)n;
#endif
}

long plat_tcp_send(PlatSock s, const char* data, size_t len) {
    if (!plat_sock_valid(s) || !data || len == 0) return 0;
#if X_HOST_WINDOWS
    int n = send((SOCKET)s.v, data, (int)len, 0);
    if (n == SOCKET_ERROR) return -1;
    return (long)n;
#else
    ssize_t n = send((int)s.v, data, len, MSG_NOSIGNAL);
    if (n < 0) return -1;
    return (long)n;
#endif
}

void plat_tcp_close(PlatSock s) {
    if (!plat_sock_valid(s)) return;
#if X_HOST_WINDOWS
    closesocket((SOCKET)s.v);
#else
    close((int)s.v);
#endif
}

int plat_tcp_local_port(PlatSock listener) {
    if (!plat_sock_valid(listener)) return -1;
    struct sockaddr_in a;
#if X_HOST_WINDOWS
    int alen = sizeof(a);
    if (getsockname((SOCKET)listener.v, (struct sockaddr*)&a, &alen) != 0) return -1;
#else
    socklen_t alen = sizeof(a);
    if (getsockname((int)listener.v, (struct sockaddr*)&a, &alen) != 0) return -1;
#endif
    return (int)ntohs(a.sin_port);
}

const char* plat_tcp_peer_addr(PlatSock s, char* out, size_t out_len) {
    if (!out || out_len == 0) return "";
    out[0] = '\0';
    if (!plat_sock_valid(s)) return out;
    struct sockaddr_in a;
#if X_HOST_WINDOWS
    int alen = sizeof(a);
    if (getpeername((SOCKET)s.v, (struct sockaddr*)&a, &alen) != 0) return out;
    unsigned char* b = (unsigned char*)&a.sin_addr.s_addr;
    snprintf(out, out_len, "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3],
             (int)ntohs(a.sin_port));
#else
    socklen_t alen = sizeof(a);
    if (getpeername((int)s.v, (struct sockaddr*)&a, &alen) != 0) return out;
    unsigned char* b = (unsigned char*)&a.sin_addr.s_addr;
    snprintf(out, out_len, "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3],
             (int)ntohs(a.sin_port));
#endif
    return out;
}

PlatSock plat_tcp_connect(const char* host, int port) {
    if (!host || port <= 0 || port > 65535) {
        snprintf(s_net_err, sizeof(s_net_err), "地址或端口非法（%s:%d）", host ? host : "", port);
        return PLAT_SOCK_INVALID;
    }
    if (!plat_net_init()) return PLAT_SOCK_INVALID;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);

    // 先按点分十进制试，失败再走 DNS。
    // 顺序有讲究：localhost 走 DNS 可能返回 ::1（IPv6），而这里
    // 只建了 IPv4 socket，于是 getaddrinfo 成功但 connect 失败 ——
    // 错误信息会变成 "Connection refused"，而实际原因是协议族不匹配。
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct hostent* he = gethostbyname(host);
        if (!he) {
            snprintf(s_net_err, sizeof(s_net_err), "无法解析主机 `%s`", host);
            return PLAT_SOCK_INVALID;
        }
        memcpy(&a.sin_addr, he->h_addr_list[0], sizeof(a.sin_addr));
    }

#if X_HOST_WINDOWS
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { net_set_err("创建 socket 失败"); return PLAT_SOCK_INVALID; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        net_set_err("连接失败"); closesocket(s); return PLAT_SOCK_INVALID;
    }
    set_nonblock(s);
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return (PlatSock){ .v = (long long)s };
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { net_set_err("创建 socket 失败"); return PLAT_SOCK_INVALID; }
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        net_set_err("连接失败"); close(s); return PLAT_SOCK_INVALID;
    }
    set_nonblock(s);
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return (PlatSock){ .v = s };
#endif
}

// 对端是否还活着。声明与理由见 platform.h。
//
// MSG_PEEK 的语义正是这里需要的：看一眼但不取走。三种结果对应
// 三种状态，没有歧义。
bool plat_tcp_alive(PlatSock s) {
    if (!plat_sock_valid(s)) return false;
    char probe[1];
#if X_HOST_WINDOWS
    int n = recv((SOCKET)s.v, probe, 1, MSG_PEEK);
    if (n == 0) return false;          // 对端有序关闭
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        // WSAEWOULDBLOCK = 没数据但连接还在
        return e != WSAECONNRESET && e != WSAENOTCONN && e != WSAECONNABORTED;
    }
    return true;
#else
    ssize_t n = recv((int)s.v, probe, 1, MSG_PEEK);
    if (n == 0) return false;
    if (n < 0) {
        return errno != ECONNRESET && errno != ENOTCONN;
        // EAGAIN / EWOULDBLOCK 落在这里，返回 true —— 连接活着，只是没数据
    }
    return true;
#endif
}

// 睡眠 ms 毫秒。理由见 platform.h —— 关键点是它**不属于 GUI**：
// 只用到 sleep 的程序不该被判成「用到了窗口能力」。
//
// 两个平台的实现差异不影响契约：
//   · Windows 的 Sleep(0) 会让出时间片然后立即返回，符合 ms<=0 的语义
//   · POSIX 的 nanosleep 参数是「请求的剩余时间」，被信号打断时要
//     循环重试；直接调一次会在收到信号时提前醒来，表现为「睡 50ms
//     实际只睡了 2ms」，事件循环的限速因此失效
void plat_sleep_ms(int ms) {
    if (ms <= 0) return;
#if X_HOST_WINDOWS
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    // 被信号打断（EINTR）就按剩余时间再睡：这不算错误，
    // 而「少睡一截」在限速场景里就等于没限速。
    while (nanosleep(&req, &req) == -1 && errno == EINTR) { }
#endif
}

// 墙上时钟（Unix 纪元毫秒）。
//
// ── 为什么用「墙上时钟」而不是单调时钟 ─────────────
//
// 两者的用途不同，不能混：
//
//   · 量耗时要用**单调**时钟（QueryPerformanceCounter /
//     CLOCK_MONOTONIC）。墙上时钟会被 NTP 校正，一次向后校正
//     就能让「耗时」变成负数。
//   · 但记录「这条记录是什么时候写进去的」必须用墙上时钟，
//     否则时间戳没有任何跨进程意义。
//
// 语言层同时需要这两种，所以这里两个都提供：
//   plat_now_ms()     墙上时钟，用于时间戳
//   plat_mono_ms()    单调时钟，用于耗时
//
// ── 为什么不用 C 的 time()/clock() ─────────────────
//
// clock() 返回的是**进程 CPU 时间**，不是墙上时间 —— 它在 IO 阻塞
// 时不增长，拿它当时间戳会得到完全错误的日期。time() 只有秒精度，
// 而本层要毫秒。
int64_t plat_now_ms(void) {
#if X_HOST_WINDOWS
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    // FILETIME 是「1601-01-01 起的 100ns 数」。先拼成 64 位，
    // 再减去 1601→1970 之间的 100ns 数（11644473600 秒）。
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    int64_t since_epoch = (int64_t)(t / 10000ULL) - 11644473600000LL;
    return since_epoch;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// 单调时钟（毫秒）。只用于测两个时刻之间的差值。
int64_t plat_mono_ms(void) {
#if X_HOST_WINDOWS
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // 先算秒再乘 1000 会丢精度（整数除法），所以按 1000/freq 缩放
    return (int64_t)(c.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
