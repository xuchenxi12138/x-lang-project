// util.c — 内存、arena、诊断
//
// 这里的实现刻意朴素：P0 的目标是「语义跑对」，不是性能。
// 07-高性能解释器.md 的七层优化在 P1 才加。

#include "x.h"
#include <stdarg.h>
#include <errno.h>


long g_alloc_count = 0;
long g_free_count  = 0;
int  g_error_count = 0;
int  g_help_count  = 0;

// ============================================================
// 内存
// ============================================================

void* x_alloc(size_t n) {
    if (n == 0) n = 1;
    void* p = malloc(n);
    if (!p) {
        fprintf(stderr, "致命：内存不足，无法分配 %zu 字节\n", n);
        exit(70);   // 70 = 内部软件错误（sysexits.h EX_SOFTWARE）
    }
    g_alloc_count++;
    return p;
}

void* x_realloc(void* p, size_t n) {
    if (n == 0) n = 1;
    void* q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "致命：内存不足，无法扩展到 %zu 字节\n", n);
        exit(70);
    }
    if (!p) g_alloc_count++;
    return q;
}

char* x_strdup(const char* s) {
    size_t n = strlen(s);
    char* p = x_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char* x_strndup(const char* s, size_t n) {
    char* p = x_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

// ============================================================
// Arena
// ============================================================
// 编译期的 AST、标识符、字符串常量全放 arena，编译结束一次性释放。
// 这让「编译期不用管释放」成为可能，代价是编译期内存只增不减——
// 对单文件编译完全可以接受（P1 做模块级 arena 再细分）。

#define ARENA_BLOCK_MIN (64 * 1024)

void arena_init(Arena* a) {
    a->head = NULL;
}

void* arena_alloc(Arena* a, size_t n) {
    // 8 字节对齐
    n = (n + 7) & ~(size_t)7;

    ArenaBlock* b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_BLOCK_MIN ? n : ARENA_BLOCK_MIN;
        ArenaBlock* nb = x_alloc(sizeof(ArenaBlock) + cap);
        nb->next = a->head;
        nb->used = 0;
        nb->cap  = cap;
        a->head  = nb;
        b = nb;
    }
    void* p = b->data + b->used;
    b->used += n;
    return p;
}

char* arena_strdup(Arena* a, const char* s) {
    size_t n = strlen(s);
    char* p = arena_alloc(a, n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char* arena_strndup(Arena* a, const char* s, size_t n) {
    char* p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void arena_free(Arena* a) {
    ArenaBlock* b = a->head;
    while (b) {
        ArenaBlock* next = b->next;
        free(b);
        g_free_count++;
        b = next;
    }
    a->head = NULL;
}

// ============================================================
// 诊断
// ============================================================
// 06-异常与诊断.md §3 规定六段式输出。编译期诊断做前三段：
//   位置 / 原因 / 建议
// 并且位置行同时给出纯文本与可点击形式（06 §4）：
//   src/main.xle:12:5
// 终端支持 OSC 8 时自动变成超链接，不支持时就是纯文本。

// 终端能力探测（是否连着终端、是否支持颜色/超链接）全部在 platform.c。
// 这里只做一层转发，保持调用点的可读性。
static bool terminal_supports_osc8(FILE* f) {
    return plat_supports_osc8(f);
}

// 颜色：只有在真终端里才输出，避免污染管道
static const char* C_RESET  = "";
static const char* C_BOLD   = "";
static const char* C_RED    = "";
static const char* C_YELLOW = "";
static const char* C_DIM    = "";

static void init_colors(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    // 是否上色完全由 platform.c 判定：它同时考虑「是否真终端」
    // 与「终端是否支持 ANSI」，并且能被 --no-color 关掉。
    if (!plat_supports_color(stderr)) return;
    C_RESET  = "\x1b[0m";
    C_BOLD   = "\x1b[1m";
    C_RED    = "\x1b[31m";
    C_YELLOW = "\x1b[33m";
    C_DIM    = "\x1b[2m";
}

// 诊断标签按**显示宽度**对齐到同一列。
//
// 为什么不能直接写死空格数：标签里混着中英文。
//   " 位置"    行首 1 空格 + 2 汉字(4列) = 5 显示列 → 需补 3 空格
//   "  error:" 行首 2 空格 + 6 ASCII      = 8 显示列 → 需补 0 空格
//   "  note:"  行首 2 空格 + 5 ASCII      = 7 显示列 → 需补 1 空格
// 这三者写出来的空格数完全不同，靠人肉数空格必然错位。
// 统一成「先算已打印的显示宽度，再补到固定列」，加新标签也不用重数。
#define DIAG_LABEL_COL 8

// 计算字符串的显示宽度（中文 2 列，ASCII 1 列，组合符号 0 列）
static int display_cols(const char* s) {
    int cols = 0;
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        int adv = 1;
        int cp = utf8_decode(s + i, len - i, &adv);
        cols += utf8_width(cp);
        i += (size_t)adv;
    }
    return cols;
}

// 已打印 used_cols 个显示列，补空格让内容从 DIAG_LABEL_COL 列开始。
// 标签本身超宽时只补一个空格，不硬塞负数的空格。
static void pad_to_content_col(FILE* out, int used_cols) {
    int pad = DIAG_LABEL_COL - used_cols;
    if (pad < 1) pad = 1;
    for (int i = 0; i < pad; i++) fputc(' ', out);
}

// 打印中文标签（如「位置」）。行首只缩 1 个空格：
// 中文标签本身宽 4 列，比英文标签窄，缩 1 个空格刚好也能对齐到第 8 列。
static void print_label_zh(FILE* out, const char* label) {
    fputc(' ', out);
    fprintf(out, "%s", label);
    pad_to_content_col(out, 1 + display_cols(label));
}

static void print_src_pos_to(FILE* out, SrcPos pos, bool with_link) {
    // ── 没有位置 ───────────────────────────────────────────
    //
    // 有一类错误根本没有源位置，传 NULL 进来：
    //   命令行参数不合法（`x pkg add Bad-Name`）—— 参数不是文件
    //   文件级 I/O 失败、配置缺失
    //
    // 这里**不打印「位置」这一行**，而不是打印「位置 <未知>」。
    // 理由是：诊断的标签行不是装饰，它声明「接下来是一条坐标」。
    // 写 <未知> 等于说「我本该给你个坐标但没找到」—— 暗示存在
    // 一个坐标只是丢了。而实际情况是这个错误按定义就没有坐标。
    //
    // 少一行比一行假话好。
    if (!pos.file) {
        return;
    }

    print_label_zh(out, "位置");

    // ── line == 0 表示「只有文件，没有具体行列」─────────────
    //
    // 有些错误是**关于整个文件**的，不属于任何一行：
    //
    //   x.toml 缺少 [package] name     —— 缺的那个键不在任何一行上
    //   x.toml 里没有依赖 `foo`         —— 不存在的东西没有行号
    //   无法读取 / 无法写入 x.toml      —— 文件级 I/O 故障
    //
    // 以前这些一律写成 `{path, 1, 1}`，于是报错显示
    //
    //     位置   x.toml:1:1
    //
    // 用户照着跳到第 1 行，看到的是 `[package]`，与错误毫无关系。
    // 更糟的是这**看起来像**一个精确的定位 —— 精确的错误位置是本
    // 项目到处强调的东西，伪造一个比不报更有害：它教用户不要相信
    // 位置信息。
    //
    // 只打文件、不打行列，是诚实的做法：这条错误属于这个文件整体。
    if (pos.line <= 0) {
        fprintf(out, "%s%s%s", C_DIM, pos.file, C_RESET);
        if (with_link && terminal_supports_osc8(out)) {
            fprintf(out, "  \x1b]8;;x://open?file=%s\x1b\\%s打开%s\x1b]8;;\x1b\\",
                    pos.file, C_DIM, C_RESET);
        }
        fprintf(out, "\n");
        return;
    }

    fprintf(out, "%s%s:%d:%d%s",
            C_DIM, pos.file, pos.line, pos.col, C_RESET);

    // 可点击形式：OSC 8 超链接指向 x:// 协议（06 §5）
    if (with_link && terminal_supports_osc8(out)) {
        // OSC 8 ; params ; URI ST
        fprintf(out, "  \x1b]8;;x://open?file=%s&line=%d&col=%d\x1b\\%s跳转%s\x1b]8;;\x1b\\",
                pos.file, pos.line, pos.col, C_DIM, C_RESET);
    }
    fprintf(out, "\n");
}

// 公开版本：运行时错误（interp.c）也用它，保证编译期与运行期的
// 位置格式完全一致——用户不该区分「这是编译器的错还是运行时的错」。
void print_src_pos(FILE* out, SrcPos pos) {
    init_colors();
    print_src_pos_to(out, pos, true);
}

// 公开版本：让 interp.c 的运行时诊断也能按显示宽度对齐标签。
// label 传不带缩进的标签名（"error" / "note" / "help"），冒号由这里补。
void diag_label(FILE* out, const char* label) {
    init_colors();
    if (strcmp(label, "位置") == 0) {
        print_label_zh(out, label);
        return;
    }
    fputs("  ", out);
    fprintf(out, "%s:", label);
    pad_to_content_col(out, 2 + display_cols(label) + 1);   // +1 = 冒号
}

// 静音通道的计数器。见 x.h 里 g_diag_silent 的说明。
int g_diag_silent  = 0;
int g_silent_errors = 0;

void diag_error(SrcPos pos, const char* fmt, ...) {
    if (g_diag_silent > 0) {
        // 试探解析期间：只记账，不输出，也不进 g_error_count。
        // 记 g_silent_errors 是为了让调用方能用「本次是否有错」判断
        // 试探是否成功 —— 那是它唯一需要的信息。
        g_silent_errors++;
        (void)pos;
        return;
    }
    init_colors();
    g_error_count++;

    fprintf(stderr, "  %s%serror%s:", C_BOLD, C_RED, C_RESET);
    pad_to_content_col(stderr, 2 + 6);   // 2 缩进 + "error" 5 + 冒号 1

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    print_src_pos_to(stderr, pos, true);
}

void diag_warn(SrcPos pos, const char* fmt, ...) {
    if (g_diag_silent > 0) { (void)pos; return; }
    init_colors();

    fprintf(stderr, "  %s%swarn%s:", C_BOLD, C_YELLOW, C_RESET);
    pad_to_content_col(stderr, 2 + 5);   // 2 + "warn" 4 + 冒号

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    print_src_pos_to(stderr, pos, true);
}

void diag_help(const char* fmt, ...) {
    if (g_diag_silent > 0) return;
    init_colors();
    g_help_count++;

    fprintf(stderr, "  %shelp%s:", C_DIM, C_RESET);
    pad_to_content_col(stderr, 2 + 5);   // 2 + "help" 4 + 冒号

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void diag_note(const char* fmt, ...) {
    if (g_diag_silent > 0) return;
    init_colors();

    fprintf(stderr, "  %snote%s:", C_DIM, C_RESET);
    pad_to_content_col(stderr, 2 + 5);   // 2 + "note" 4 + 冒号

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

// ============================================================
// 文件读取
// ============================================================
// 不做增量，直接读全文。P1 才需要考虑大文件。

// 以 UTF-8 路径打开文件。内部统一用 UTF-8 表示路径（这样可以原样
// 打印进诊断、写进 .xbc），只在真正调系统 API 的那一刻转成本地编码。
FILE* x_fopen(const char* path, const char* mode) {
    char* local = utf8_to_locale(path);
    FILE* f = fopen(local ? local : path, mode);
    free(local);
    return f;
}

bool x_remove(const char* path) {
    char* local = utf8_to_locale(path);
    int rc = remove(local ? local : path);
    free(local);
    return rc == 0;
}

bool x_rename(const char* from, const char* to) {
    char* lf = utf8_to_locale(from);
    char* lt = utf8_to_locale(to);
    int rc = rename(lf ? lf : from, lt ? lt : to);
    free(lf);
    free(lt);
    return rc == 0;
}

bool x_is_dir(const char* path) {
    if (!path || !*path) return false;
    // 走平台层而不是直接 stat。
    //
    // 两个理由：
    //   1. 「除 platform.c 外不许包含系统头文件」是项目的铁律，
    //      而 check_portable.sh 会查这一条。原先这里 include 了
    //      <sys/stat.h>，检查脚本确实报了出来 —— 那正是它的作用。
    //   2. 平台层已经做了 UTF-8 → UTF-16 的路径转换，含中文的
    //      目录名在 Windows 上要靠它才判得对。
    return plat_is_dir(path);
}

// 路径是否指向一个**普通的、可读的**文件。
//
// 与 x_is_dir 成对：标准库解析要区分「目录在但模块名写错」与
// 「整个 stdlib 都没装」两种情况，诊断完全不同 —— 前者让用户改
// import 那一行，后者让他检查安装。
bool x_file_exists(const char* path) {
    if (!path || !*path) return false;
    if (plat_is_dir(path)) return false;      // 目录不算文件
    return plat_file_exists(path);
}

char* read_file(const char* path, size_t* out_len) {
    FILE* f = x_fopen(path, "rb");
    if (!f) {
        diag_error((SrcPos){ NULL, 0, 0 }, "无法打开文件 `%s`：%s", path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        diag_error((SrcPos){ NULL, 0, 0 }, "无法定位文件末尾 `%s`", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        diag_error((SrcPos){ NULL, 0, 0 }, "无法获取文件大小 `%s`", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    char* buf = x_alloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);

    // 剥 BOM（01 §2.1 的承诺）。Windows 上的记事本、部分编辑器、以及
    // PowerShell 的 `Out-File -Encoding utf8` 都会加 BOM。BOM 若留在
    // 源码头部，词法器会把它当成一个 U+FEFF 标识符字符，于是第一行
    // `println` 变成 `println`（肉眼完全看不出来），报「未定义的函数」。
    // 用户会盯着一个看上去正确的单词发疯 —— 所以必须在读文件这层剥掉。
    //
    // 用 memmove 就地前移而不是返回 s+3：调用方拿到的必须是一个能被
    // free 的指针，否则释放时崩在几万行之外。
    if (got >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3);
        got -= 3;
        buf[got] = '\0';
    }

    if (out_len) *out_len = got;
    return buf;
}