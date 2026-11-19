// lsp.c — LSP 服务端（stdio 传输的最小实现）
//
// 设计取舍见 lsp.h。这里只记实现上的三处关键决定。
//
// ── 一、为什么自己拼 JSON 输出，不做通用序列化 ──────────────
//
// 要发出去的消息形状是固定的（initialize 结果、补全列表、悬停内容）。
// 通用序列化器要处理嵌套、转义、类型分派 —— 几百行，而这里每个
// 响应都是「拼几段字符串」。写一个 jstr_* 系列把转义做对，其余
// 直接 printf 拼，代码短得多，出错点也少。
//
// 代价是「JSON 永远合法」这件事没有类型系统保证。用一个
// tools/check-lsp.js 在测试时把每条输出都解析一遍来兜底 ——
// 这比在 C 里造一个 DOM 更直接。
//
// ── 二、位置编码：UTF-16 码元 ───────────────────────────────
//
// LSP 规定 position.character 是**UTF-16 码元偏移**，而本项目的
// 词法器给出的列号是**UTF-8 字符数**。两者对 ASCII 相同，对中文
// 不同（一个汉字：UTF-8 占 3 字节、1 个字符、UTF-16 占 1 个码元）。
//
// 所以不能直接把 Tok.pos.col 发出去。这里做一次转换：从行首
// 逐个解码 UTF-8 字符，累加它的 UTF-16 码元数（BMP 字符 1 个，
// 补充平面字符 2 个）。
//
// ── 三、补全不求精，求「不误导」 ────────────────────────────
//
// 真正的语义补全要类型推断（这个表达式是 Int，所以该给 Int 的方法）。
// P0 不做：它给出的候选按来源分类（关键字 / 内建 / 本文档符号），
// 并**只在合适的位置**给 —— 比如 `import` 之后只给包名候选，
// 而不是把 75 个内建全倒出来。
//
// 「少给」比「乱给」好：用户从补全里选一个，期望它是能用的。
// 倒一堆不该出现的选项，等于把筛选工作推回给人。

#include "lsp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_read.h"
#include "lsp_data.h"
#include "platform.h"
#include "x.h"

// ============================================================
// 输出：JSON 字符串转义
// ============================================================
//
// 只需要一份转义实现。第一版写了两份（一个写 FILE*、一个写 Buf），
// 两份的转义表迟早会分叉 —— 而 JSON 转义错一个分支就是「客户端
// 解析失败，只显示语言服务器已停止」，没有任何细节可查。
// 所以统一到 buf_jstr，写 FILE* 的地方先拼进 Buf。

// 一条完整消息：头部 + 体。
//
// 必须一次性写完整（头和体之间不能插别的），所以调用方先拼到内存
// 再调用这里。拼内存用 open_memstream 也行，但它不是 C11 标准
// （POSIX 才有），MinGW 上不可用。这里用固定缓冲 + 手动增长。
typedef struct {
    char*  buf;
    size_t len;
    size_t cap;
} Buf;

static void buf_reserve(Buf* b, size_t n) {
    if (b->len + n + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 4096;
    while (ncap < b->len + n + 1) ncap *= 2;
    char* nb = (char*)realloc(b->buf, ncap);
    if (!nb) { fprintf(stderr, "[x lsp] 内存不足\n"); exit(70); }
    b->buf = nb;
    b->cap = ncap;
}

static void buf_puts(Buf* b, const char* s) {
    size_t n = strlen(s);
    buf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void buf_printf(Buf* b, const char* fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        buf_puts(b, tmp);
        return;
    }
    // 超长：再开一块
    char* big = (char*)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_puts(b, big);
    free(big);
}

// 转义一个 JSON 字符串（含引号）并追加到 Buf。
//
// 非 ASCII **原样输出**（JSON 允许 UTF-8 直接出现），不转 \u ——
// 转了之后输出膨胀三倍，日志里也完全不可读。
static void buf_jstr(Buf* b, const char* s) {
    // 最坏情况：每个字节都要转成 \u00XX（6 字节）
    size_t cap = 3;
    if (s) cap += strlen(s) * 6;
    char* t = (char*)malloc(cap);
    if (!t) return;

    size_t w = 0;
    t[w++] = '"';
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            switch (*p) {
            case '"':  t[w++] = '\\'; t[w++] = '"';  break;
            case '\\': t[w++] = '\\'; t[w++] = '\\'; break;
            case '\n': t[w++] = '\\'; t[w++] = 'n';  break;
            case '\r': t[w++] = '\\'; t[w++] = 'r';  break;
            case '\t': t[w++] = '\\'; t[w++] = 't';  break;
            case '\b': t[w++] = '\\'; t[w++] = 'b';  break;
            case '\f': t[w++] = '\\'; t[w++] = 'f';  break;
            default:
                if (*p < 0x20) {
                    w += (size_t)snprintf(t + w, cap - w, "\\u%04x", *p);
                } else {
                    t[w++] = (char)*p;
                }
            }
        }
    }
    t[w++] = '"';
    t[w] = '\0';
    buf_puts(b, t);
    free(t);
}

// 发出一条消息。body 已是完整 JSON。
static void send_msg(const char* body) {
    size_t n = strlen(body);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", n);
    fwrite(body, 1, n, stdout);
    fflush(stdout);
}

// 只有 id 的响应（用于 notification 的错误？不需要）。
static void send_ok_id(const Jv* id, const char* result_json) {
    Buf b = {0};
    buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    // id 可能是数字或字符串，原样回显更安全 —— 但 Jv 里没有保存
    // 原始文本。这里按类型还原：数字输出数值，字符串输出带引号。
    if (id && id->kind == JV_NUM) {
        buf_printf(&b, "%lld", id->i);
    } else if (id && id->kind == JV_STR) {
        buf_jstr(&b, id->s);
    } else {
        buf_puts(&b, "null");
    }
    buf_puts(&b, ",\"result\":");
    buf_puts(&b, result_json ? result_json : "null");
    buf_puts(&b, "}");
    send_msg(b.buf ? b.buf : "{}");
    free(b.buf);
}

// ============================================================
// UTF-16 位置换算
// ============================================================

// UTF-8 字符长度（返回 1..4；非法字节按 1 处理，宁可错位也不要死循环）
static int u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 一个 UTF-8 字符占几个 UTF-16 码元
//
// 只看字节长度就够了：4 字节的必然是 U+10000 以上，UTF-16 里要两个
// 码元（代理对）；1~3 字节的都在 BMP 内，一个码元。不需要解码出
// 实际码点 —— 而这也正是它不需要 s 参数的原因。
static int u16_units(int len) {
    return (len == 4) ? 2 : 1;
}

// 把「第 line 行（0 基）、第 col 个 UTF-16 码元」换成字节偏移。
//
// 越界时钳到行尾 / 文本末尾 —— LSP 客户端偶尔会发超出行的位置
// （比如光标在行尾之后），报错不如钳位：提示错了但还能用，
// 报错就整个请求失败。
static size_t pos_to_offset(const char* text, size_t len, int line, int col) {
    size_t i = 0;
    int cur = 0;
    while (i < len && cur < line) {
        if (text[i] == '\n') cur++;
        i++;
    }
    if (cur < line) return len;

    int u16 = 0;
    while (i < len && text[i] != '\n' && text[i] != '\r') {
        if (u16 >= col) break;
        int cl = u8_len((unsigned char)text[i]);
        if (i + (size_t)cl > len) cl = 1;
        u16 += u16_units(cl);
        i += (size_t)cl;
    }
    return i;
}

// 字节偏移 → (行, UTF-16 列)
//
// ── 这里原来只有一个 bug，但很致命 ──────────────────────────
//
// 早先的实现是「从位置 0 走到 off，遇到换行就 break」，于是它返回的
// 永远是**第一行**上的列号。文档符号因此全部堆在 (0, col) 附近，
// 编辑器的面包屑、大纲、跳转全都定位到文件开头 —— 看起来像「大纲
// 里点了没反应」，其实是每个符号的 range 都是同一个死点。
//
// 正确做法是边走边记行号，换行时把列清零。反过来不行：先按 off
// 反查行号再截前缀，等价于把整段走两遍，而这里一次就够。
static void offset_to_pos(const char* text, size_t len, size_t off,
                          int* out_line, int* out_col) {
    int line = 0;
    int u16 = 0;
    size_t i = 0;
    while (i < len && i < off) {
        if (text[i] == '\n') {
            line++;
            u16 = 0;
            i++;
            continue;
        }
        if (text[i] == '\r') {
            // \r\n 里的 \r 不算列：Windows 文件上是它先出现。
            // 不算进来，否则每个 Windows 换行都会让列多 1。
            i++;
            continue;
        }
        int cl = u8_len((unsigned char)text[i]);
        if (i + (size_t)cl > len) cl = 1;
        u16 += u16_units(cl);
        i += (size_t)cl;
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = u16;
}

// 标识符的 UTF-16 宽度。
//
// strlen 给的是**字节数**，中文一个字 3 字节。直接拿它加到 UTF-16 列上，
// 中文标识符的 selectionRange 会越过词尾 —— 大纲里点击「打招呼」会
// 跳到名字后面一个字符。所有「名字宽度」都必须走这里。
static int utf16_width(const char* s) {
    int u = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ) {
        int l = u8_len(*p);
        u += u16_units(l);
        p += l;
    }
    return u;
}

// ============================================================
// 文档管理
// ============================================================

#define LSP_MAX_DOCS 32

typedef struct {
    char   uri[900];
    char*  text;        // 堆上的副本
    size_t len;
    // 从文本里扫出来的符号（函数名 / 变量名 / 参数名）
    // 每次 didChange 重扫 —— 全量同步的前提下这是最直接的做法。
} Doc;

static Doc g_docs[LSP_MAX_DOCS];
static int g_doc_count = 0;

static Doc* doc_find(const char* uri) {
    for (int i = 0; i < g_doc_count; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) return &g_docs[i];
    }
    return NULL;
}

static Doc* doc_open(const char* uri, const char* text, size_t len) {
    Doc* d = doc_find(uri);
    if (!d) {
        if (g_doc_count >= LSP_MAX_DOCS) return NULL;
        d = &g_docs[g_doc_count++];
        snprintf(d->uri, sizeof(d->uri), "%s", uri);
        d->text = NULL;
        d->len = 0;
    }
    char* nt = (char*)malloc(len + 1);
    if (!nt) return NULL;
    memcpy(nt, text, len);
    nt[len] = '\0';
    free(d->text);
    d->text = nt;
    d->len = len;
    return d;
}

static void doc_close(const char* uri) {
    for (int i = 0; i < g_doc_count; i++) {
        if (strcmp(g_docs[i].uri, uri) == 0) {
            free(g_docs[i].text);
            for (int k = i; k < g_doc_count - 1; k++) g_docs[k] = g_docs[k + 1];
            g_doc_count--;
            return;
        }
    }
}

// ============================================================
// 符号扫描：从文档里找出函数名 / 变量名 / 参数名
// ============================================================
//
// ── 为什么不用 parser ───────────────────────────────────────
//
// 用 parser 更准，但它要建 arena、要能容忍**不完整的代码**。
// 用户正在敲的时候文件必然是残缺的（少一个右括号、函数写了一半），
// 而 parser 对残缺输入的处理是「报错并放弃」—— 那样每次输入到
// 一半补全就全空，体验极差。
//
// 词法级扫描没有这个问题：它不关心括号是否配对。代价是不区分
// 作用域（函数 A 里的局部变量和 B 里的会混在一起）。对补全来说
// 这个代价可以接受 —— 多给几个候选，总比给不出好。
//
// 判据用词法单元而不是字符串匹配，这样注释和字符串里的
// 「fn xxx」不会被当成定义。

typedef struct {
    char name[128];
    int  kind;       // 0 = 函数, 1 = 变量/参数, 2 = 类/结构体/接口
    int  line;       // 0 基
    int  col;        // UTF-16 列
} Sym;

#define LSP_MAX_SYMS 512
static Sym g_syms[LSP_MAX_SYMS];
static int g_sym_count = 0;

// 判断 token 是否是一个可用于补全的标识符
static bool is_ident_tok(const Token* t) {
    return t->kind == TK_IDENT && t->s && t->len > 0 && t->len < 128;
}

// 扫描一个文档。返回符号数（写进 g_syms）。
static int scan_symbols(const Doc* d) {
    g_sym_count = 0;
    if (!d || !d->text) return 0;

    // 中文模式下 `函`/`令` 也是定义关键字。这里两种模式都认 ——
    // LSP 不强制区分 .xle 与 .xlc：一个 .xle 里写中文关键字是
    // 编译不过，但提示多给两个候选没有害处。
    //
    // 反过来（只认英文）会让 .xlc 文件的补全完全失效，
    // 那才是真问题。
    lexer_set_zh_mode(true);

    Arena a;
    arena_init(&a);
    Lexer lx;
    lexer_init(&lx, d->uri, d->text, d->len, &a);

    Token prev;
    memset(&prev, 0, sizeof(prev));
    bool have_prev = false;
    // 定义位置：上一个 token 是 fn/令/变/常 时，这个标识符就是定义
    int want = 0;   // 0 = 无, 1 = 函数名, 2 = 变量名

    for (;;) {
        // ── token 的真实起点 ────────────────────────────────
        //
        // t.s 指向的不是源文本里那个位置 —— 词法器把标识符**复制**
        // 到 arena 里（源码是 const，不能就地写 NUL）。所以
        // `t.s - d->text` 得到的是「arena 缓冲区地址减去源码地址」，
        // 是一个毫无意义的负数或巨大值，落到 offset_to_pos 里就被
        // 钳成文件末尾。
        //
        // ── 为什么不能用 lx.pos - t.len ─────────────────────
        //
        // 那个算式只在「token 文本与源码逐字节相同」时成立，而这
        // 门语言有两处会破坏它：
        //
        //   1. 中文模式下 lex_ident 会把 `整数` 这类类型别名归一化
        //      成 `Int`，于是 t.len 是 3（"Int"）而不是 6（"整数"）。
        //      lx.pos - 3 落到中文字的中间，位置直接错位。
        //   2. lexer_next 之前的 lx.pos 是**上一个 token 的末尾**，
        //      和真正的起点之间隔着空白/注释 —— `fn greet` 里
        //      greet 会被算到第 2 列。
        //
        // 正确且唯一可靠的来源是 Token 自带的 pos：lexer 在
        // skip_trivia 之后才取它，所以它精确指向 token 第一个字符，
        // 且列号按**字符**计（中文一个字一列，见 lexer.c 的 advance）。
        // 行号 0 基、列号 1 基，转换到 LSP 的 0 基只需减 1。
        Token t = lexer_next(&lx);
        if (t.kind == TK_EOF) break;

        if (want && is_ident_tok(&t)) {
            if (g_sym_count < LSP_MAX_SYMS) {
                Sym* s = &g_syms[g_sym_count++];
                snprintf(s->name, sizeof(s->name), "%.*s", (int)t.len, t.s);
                s->kind = (want == 1) ? 0 : (want == 3) ? 2 : 1;
                s->line = t.pos.line - 1;
                s->col  = t.pos.col - 1;
            }
            want = 0;
        } else if (t.kind == TK_FN) {
            want = 1;
        } else if (t.kind == TK_LET || t.kind == TK_VAR || t.kind == TK_CONST) {
            want = 2;
        } else if (t.kind == TK_CLASS || t.kind == TK_STRUCT || t.kind == TK_INTERFACE) {
            // 类 / 结构体 / 接口声明：下一个标识符是类型名。
            // ctrl+点击 `new User(...)` 里的 User 要落到 `class User` 上，
            // 没有这一类符号就永远跳不过去。
            want = 3;
        } else if (want == 2 && t.kind == TK_COLON) {
            // `let x: Int` —— 类型名不是变量名，别收
            want = 0;
        } else if (t.kind != TK_IDENT) {
            // 中间隔了别的 token，放弃这个定义位置
            if (want == 1 && t.kind == TK_LPAREN) want = 0;
            if (want == 3) want = 0;
        }

        prev = t;
        have_prev = true;
    }
    (void)prev;
    (void)have_prev;

    // 参数：`fn f(a: Int, b: String)` 里的 a / b。
    // 上面的循环只抓了函数名，参数要单独扫 —— 直接在源码文本里
    // 找 fn 之后的括号不靠谱（有嵌套、有字符串）。这里用词法器
    // 再走一遍，跟踪括号深度。
    //
    // ── 只在 `fn` 之后的括号里收 ────────────────────────────
    //
    // 早先这里把「任何 depth==0 的左括号」都当成参数列表开始，于是
    // `greet("x")` 这种**调用**也被当成定义，把实参名收进符号表；
    // 更糟的是参数里的类型名（`name: String` 的 String）也被收进去，
    // 大纲里就多出一堆 `String` `Int` `x` 这样的伪符号。
    //
    // 判据必须收紧到「前一个 token 是 fn 之后的函数名，紧跟这个 (」。
    // 用一个 want_name 状态跟着走，和上面第一遍扫函数名同样的思路。
    {
        Lexer lx2;
        lexer_init(&lx2, d->uri, d->text, d->len, &a);
        int depth = 0;
        bool in_params = false;
        int want_fn_paren = 0;   // 1 = 见过 fn，2 = 见过函数名，等 (
        bool after_colon = false;

        for (;;) {
            Token t = lexer_next(&lx2);
            if (t.kind == TK_EOF) break;

            if (!in_params) {
                if (t.kind == TK_FN) {
                    want_fn_paren = 1;
                } else if (want_fn_paren == 1 && is_ident_tok(&t)) {
                    want_fn_paren = 2;
                } else if (want_fn_paren == 2 && t.kind == TK_LPAREN) {
                    in_params = true;
                    depth = 1;
                    want_fn_paren = 0;
                } else if (t.kind != TK_IDENT && t.kind != TK_LPAREN) {
                    // 中间隔了别的（比如 `=`），这个 fn 后面不是参数表
                    if (want_fn_paren) want_fn_paren = 0;
                }
            } else {
                if (t.kind == TK_LPAREN) depth++;
                else if (t.kind == TK_RPAREN) {
                    depth--;
                    if (depth == 0) { in_params = false; after_colon = false; }
                } else if (depth == 1 && t.kind == TK_COLON) {
                    // 进入类型注解区：跳过之后的标识符，别把类型名当参数
                    after_colon = true;
                } else if (depth == 1 && t.kind == TK_COMMA) {
                    after_colon = false;
                } else if (depth == 1 && t.kind == TK_IDENT && is_ident_tok(&t)
                           && !after_colon) {
                    bool dup = false;
                    for (int k = 0; k < g_sym_count; k++) {
                        if (strcmp(g_syms[k].name, "") != 0 &&
                            (size_t)t.len == strlen(g_syms[k].name) &&
                            memcmp(g_syms[k].name, t.s, t.len) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup && g_sym_count < LSP_MAX_SYMS) {
                        Sym* s = &g_syms[g_sym_count++];
                        snprintf(s->name, sizeof(s->name), "%.*s", (int)t.len, t.s);
                        s->kind = 1;
                        // 同 scan_symbols 的理由：用 Token 自带的 pos，
                        // 不用 lx2.pos - t.len（中文归一化会让 t.len 失真）。
                        s->line = t.pos.line - 1;
                        s->col  = t.pos.col - 1;
                    }
                }
            }
        }
    }

    arena_free(&a);
    lexer_set_zh_mode(false);
    return g_sym_count;
}

// ============================================================
// 补全
// ============================================================

// 在光标位置取正在输入的词前缀。
//
// 返回值是前缀的**字节偏移**（= 词首），并填 out 前缀文本。
// 没有前缀（光标紧跟空格/标点）时返回当前偏移、前缀为空。
static size_t current_word(const char* text, size_t len, size_t off,
                           char* out, size_t out_len) {
    // off 来自客户端算出的位置，可能超出文本末尾（光标在最后一行的
    // 行尾之后）。不钳住的话 text[start-1] 会读到分配区之外。
    if (off > len) off = len;
    size_t start = off;
    while (start > 0) {
        unsigned char c = (unsigned char)text[start - 1];
        // 标识符字符：字母数字下划线，以及 >= 0x80 的 UTF-8 字节
        // （中文标识符里一个汉字占 3 字节，逐字节回退能正确覆盖）
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) {
            start--;
        } else {
            break;
        }
    }
    size_t n = off - start;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, text + start, n);
    out[n] = '\0';
    return start;
}

// 前缀匹配（区分大小写 —— 语言本身区分）
static bool starts_with(const char* s, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

// 补全项的种类（LSP CompletionItemKind）
enum {
    CIK_FUNCTION = 3,
    CIK_FIELD = 5,
    CIK_VARIABLE = 6,
    CIK_KEYWORD = 14,
    CIK_MODULE = 9,
    CIK_TYPE = 22,
};

static void add_item(Buf* b, bool* first, const char* label,
                     const char* detail, int kind, const char* insert) {
    if (!*first) buf_puts(b, ",");
    *first = false;
    buf_puts(b, "{\"label\":");
    buf_jstr(b, label);
    buf_puts(b, ",\"kind\":");
    buf_printf(b, "%d", kind);
    if (detail && detail[0]) {
        buf_puts(b, ",\"detail\":");
        buf_jstr(b, detail);
    }
    if (insert && insert[0]) {
        buf_puts(b, ",\"insertText\":");
        buf_jstr(b, insert);
    }
    buf_puts(b, "}");
}

// 生成补全结果
static char* build_completion(const Doc* d, size_t off) {
    char prefix[128];
    current_word(d->text, d->len, off, prefix, sizeof(prefix));

    Buf b = {0};
    buf_puts(&b, "[");
    bool first = true;

    // ── 本文档里的符号优先 ──────────────────────────────
    //
    // 顺序重要：补全列表在编辑器里是「越靠前越显眼」。用户当前
    // 文件的函数名比语言内建更可能是他要的东西。
    scan_symbols(d);
    for (int i = 0; i < g_sym_count; i++) {
        if (prefix[0] && !starts_with(g_syms[i].name, prefix)) continue;
        char det[64];
        snprintf(det, sizeof(det), "%s（本文档）",
                 g_syms[i].kind == 0 ? "函数" : "变量");
        add_item(&b, &first, g_syms[i].name, det,
                 g_syms[i].kind == 0 ? CIK_FUNCTION : CIK_VARIABLE, NULL);
    }

    // ── 内建函数 ────────────────────────────────────────
    for (int i = 0; i < kLspBuiltinName_COUNT; i++) {
        const char* nm = kLspBuiltinName[i];
        if (prefix[0] && !starts_with(nm, prefix)) continue;
        int argc = kLspBuiltinArgc[i];
        const char* ret = kLspBuiltinRet[i];
        char det[128];
        if (argc >= 0) {
            snprintf(det, sizeof(det), "内建函数（%d 个参数）→ %s", argc, ret);
        } else {
            // argc == -1：可变参数（只有 math 是这样）
            snprintf(det, sizeof(det), "内建函数（可变参数）→ %s", ret);
        }
        // insertText：内建带括号，光标落在括号里。
        //
        // 用带参数占位的形式让编辑器把光标放进去。LSP 的 snippet
        // 语法 $0 表示最终光标位置；有些客户端不支持 snippet，
        // 所以 InsertTextFormat 也要声明（见下面 build_completion_result）。
        char ins[96];
        if (argc == 0) snprintf(ins, sizeof(ins), "%s()", nm);
        else if (argc > 0) snprintf(ins, sizeof(ins), "%s($0)", nm);
        else snprintf(ins, sizeof(ins), "%s($0)", nm);
        add_item(&b, &first, nm, det, CIK_FUNCTION, ins);
    }

    // ── 类型 ────────────────────────────────────────────
    for (int i = 0; i < kLspTypes_COUNT; i++) {
        const char* nm = kLspTypes[i];
        if (prefix[0] && !starts_with(nm, prefix)) continue;
        add_item(&b, &first, nm, "类型", CIK_TYPE, NULL);
    }

    // ── 中文类型别名 ────────────────────────────────────
    for (int i = 0; i < kLspZhTypeZh_COUNT; i++) {
        const char* nm = kLspZhTypeZh[i];
        if (prefix[0] && !starts_with(nm, prefix)) continue;
        char det[96];
        snprintf(det, sizeof(det), "中文类型别名 → %s", kLspZhTypeEn[i]);
        add_item(&b, &first, nm, det, CIK_TYPE, NULL);
    }

    // ── 关键字 ──────────────────────────────────────────
    for (int i = 0; i < kLspKeywords_COUNT; i++) {
        const char* nm = kLspKeywords[i];
        if (prefix[0] && !starts_with(nm, prefix)) continue;
        add_item(&b, &first, nm, "关键字", CIK_KEYWORD, NULL);
    }

    // ── 中文关键字别名 ──────────────────────────────────
    for (int i = 0; i < kLspZhKeywords_COUNT; i++) {
        const char* nm = kLspZhKeywords[i];
        if (prefix[0] && !starts_with(nm, prefix)) continue;
        add_item(&b, &first, nm, "中文关键字", CIK_KEYWORD, NULL);
    }

    buf_puts(&b, "]");
    return b.buf;
}

// 把补全数组包成 CompletionList（带 isIncomplete + insertTextFormat）
static char* wrap_completion_list(char* items_json) {
    Buf b = {0};
    buf_puts(&b, "{\"isIncomplete\":false,\"items\":");
    buf_puts(&b, items_json ? items_json : "[]");
    buf_puts(&b, "}");
    return b.buf;
}

// ============================================================
// 悬停
// ============================================================

// 在光标位置取出完整的词（不只是前缀 —— 悬停要整词）
static void word_at(const char* text, size_t len, size_t off,
                    char* out, size_t out_len, size_t* out_start) {
    size_t s = off, e = off;
    while (s > 0) {
        unsigned char c = (unsigned char)text[s - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) s--;
        else break;
    }
    while (e < len) {
        unsigned char c = (unsigned char)text[e];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) e++;
        else break;
    }
    size_t n = e - s;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, text + s, n);
    out[n] = '\0';
    if (out_start) *out_start = s;
}

// 查一个词是什么
static char* build_hover(const Doc* d, size_t off) {
    char w[128];
    word_at(d->text, d->len, off, w, sizeof(w), NULL);
    if (!w[0]) return NULL;

    // 内建？
    for (int i = 0; i < kLspBuiltinName_COUNT; i++) {
        if (strcmp(kLspBuiltinName[i], w) != 0) continue;
        int argc = kLspBuiltinArgc[i];
        Buf b = {0};
        buf_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        char md[512];
        if (argc >= 0) {
            snprintf(md, sizeof(md),
                     "**内建函数** `%s`\n\n参数：%d 个\n\n返回：`%s`",
                     w, argc, kLspBuiltinRet[i]);
        } else {
            snprintf(md, sizeof(md),
                     "**内建函数** `%s`\n\n可变参数\n\n返回：`%s`",
                     w, kLspBuiltinRet[i]);
        }
        buf_jstr(&b, md);
        buf_puts(&b, "}}");
        return b.buf;
    }

    // 类型？
    for (int i = 0; i < kLspTypes_COUNT; i++) {
        if (strcmp(kLspTypes[i], w) != 0) continue;
        Buf b = {0};
        buf_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        char md[256];
        snprintf(md, sizeof(md), "**内建类型** `%s`", w);
        buf_jstr(&b, md);
        buf_puts(&b, "}}");
        return b.buf;
    }

    // 中文类型别名？
    for (int i = 0; i < kLspZhTypeZh_COUNT; i++) {
        if (strcmp(kLspZhTypeZh[i], w) != 0) continue;
        Buf b = {0};
        buf_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        char md[256];
        snprintf(md, sizeof(md), "**中文类型别名** `%s` → `%s`",
                 w, kLspZhTypeEn[i]);
        buf_jstr(&b, md);
        buf_puts(&b, "}}");
        return b.buf;
    }

    // 文档内符号？
    scan_symbols(d);
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, w) != 0) continue;
        Buf b = {0};
        buf_puts(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        char md[384];
        snprintf(md, sizeof(md),
                 "**%s** `%s`\n\n定义于本文件第 %d 行",
                 g_syms[i].kind == 0 ? "函数" : "变量",
                 w, g_syms[i].line + 1);
        buf_jstr(&b, md);
        buf_puts(&b, "}}");
        return b.buf;
    }

    return NULL;
}

// ============================================================
// 参数提示（signatureHelp）
// ============================================================
//
// 光标在 `arrPush(` 的括号里时，告诉用户这个函数要什么参数。
//
// 内建的参数名我们**没有**（g_builtins 只记了 argc 与返回类型）。
// 所以这里只给「第 1 个参数 / 共 2 个」，不编造参数名。
//
// 编造参数名看起来更友好，但那是在文档里写不存在的东西 ——
// 用户照着写，然后发现名字根本不重要（位置参数），或者更糟：
// 以为有具名参数可用。

static char* build_signature_help(const Doc* d, size_t off) {
    // ── 往回找最近的开括号 ──────────────────────────────────
    //
    // 三个地方容易写错：
    //
    // 1. 光标之前在**同一行**里有 `)` 时，那是已经闭合的另一个调用，
    //    不该拦下搜索。所以用深度计数：遇到 `)` 加一层，遇到 `(`
    //    减一层，只有 depth == 0 时的 `(` 才是我们要找的。
    // 2. 必须允许跨行。多参数调用被格式化成
    //        arrPush(a,
    //                b)
    //    是很常见的写法，光标在第二行。早先这里遇到换行就 break，
    //    于是那些调用永远没有参数提示 —— 而单行的又正常，很容易
    //    被误判成「有时灵有时不灵」。
    // 3. 深度变成负数说明光标左边有未配对的 `)`，那是残缺代码。
    //    此时继续往左找会跑到别的语句上去，直接放弃更安全。
    size_t i = off;
    int depth = 0;
    size_t pos = (size_t)-1;
    while (i > 0) {
        i--;
        char c = d->text[i];
        if (c == ')') depth++;
        else if (c == '(') {
            if (depth == 0) { pos = i; break; }
            depth--;
        } else if (c == '}') {
            // 语句块结束：光标不在任何调用的括号里
            break;
        }
        if (depth < 0) break;
    }
    if (pos == (size_t)-1) return NULL;

    // `(` 之前的词
    size_t s = pos;
    while (s > 0) {
        unsigned char c = (unsigned char)d->text[s - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) s--;
        else break;
    }
    char name[128];
    size_t n = pos - s;
    if (n == 0 || n >= sizeof(name)) return NULL;
    memcpy(name, d->text + s, n);
    name[n] = '\0';

    // 是内建吗？
    for (int k = 0; k < kLspBuiltinName_COUNT; k++) {
        if (strcmp(kLspBuiltinName[k], name) != 0) continue;

        int argc = kLspBuiltinArgc[k];
        // 算当前是第几个参数：从 `(` 到光标数顶层逗号
        int argi = 0;
        int dep = 0;
        for (size_t j = pos + 1; j < off && j < d->len; j++) {
            char c = d->text[j];
            if (c == '(' || c == '[' || c == '{') dep++;
            else if (c == ')' || c == ']' || c == '}') dep--;
            else if (c == ',' && dep == 0) argi++;
        }

        Buf b = {0};
        buf_puts(&b, "{\"signatures\":[{\"label\":");
        // 512 是为了让编译器能证明不会截断：name ≤ 127 + params ≤ 191
                // + "() → " 与返回类型。给多了不浪费（栈上 200 字节），
                // 给少了编译器会警告，而警告在这里就等于「签名可能被切」。
                char label[512];
        if (argc >= 0) {
            // 拼一个 `name(<arg1>, <arg2>)` 的标签。没有真实参数名，
            // 用占位符而不是编造。
            char params[192];
            params[0] = '\0';
            for (int a = 0; a < argc; a++) {
                char piece[24];
                snprintf(piece, sizeof(piece), "%s#%d", a ? ", " : "", a + 1);
                strncat(params, piece, sizeof(params) - strlen(params) - 1);
            }
            snprintf(label, sizeof(label), "%s(%s) → %s",
                     name, params, kLspBuiltinRet[k]);
        } else {
            snprintf(label, sizeof(label), "%s(...) → %s",
                     name, kLspBuiltinRet[k]);
        }
        buf_jstr(&b, label);
        buf_puts(&b, ",\"documentation\":");
        char doc[256];
        if (argc >= 0) {
            snprintf(doc, sizeof(doc),
                     "内建函数，接受 %d 个参数。项目没有为内建记录参数名"
                     "（都是位置参数），所以这里用 #1 #2 占位。", argc);
        } else {
            snprintf(doc, sizeof(doc), "内建函数，接受可变数量的参数。");
        }
        buf_jstr(&b, doc);
        buf_puts(&b, "}],\"activeSignature\":0,\"activeParameter\":");
        buf_printf(&b, "%d", argi);
        buf_puts(&b, "}");
        return b.buf;
    }

    // ── 本文档定义的函数 ────────────────────────────────────
    //
    // 内建之外就是用户自己写的函数。它比内建更值得提示 —— 内建的
    // 签名用户查文档就行，自己写的函数才容易记错。
    //
    // 参数名从 `fn name(a: Int, b: String)` 的源码里取。取不到就退化成
    // 「有 N 个参数」，不编造。
    //
    // 这里直接按文本找 `fn <name>(`，不用符号表：符号表只存了名字和
    // 位置，没存参数列表；而按文本找的搜索空间就是一份源码，代价可
    // 忽略。前提是前面已经确认 `name` 是标识符字符，不会含空格。
    {
        char params[256];
        params[0] = '\0';
        int pcount = 0;
        size_t hit = (size_t)-1;
        for (size_t j = 0; j + 2 < d->len; j++) {
            if (memcmp(d->text + j, "fn", 2) != 0) continue;
            // `fn` 必须是独立的词：`infn` 这种不该算
            if (j > 0) {
                unsigned char pc = (unsigned char)d->text[j - 1];
                if ((pc >= 'a' && pc <= 'z') || (pc >= 'A' && pc <= 'Z') ||
                    (pc >= '0' && pc <= '9') || pc == '_' || pc >= 0x80) continue;
            }
            size_t q = j + 2;
            while (q < d->len && (d->text[q] == ' ' || d->text[q] == '\t')) q++;
            if (q + n > d->len) continue;
            if (memcmp(d->text + q, name, n) != 0) continue;
            size_t r = q + n;
            while (r < d->len && (d->text[r] == ' ' || d->text[r] == '\t')) r++;
            if (r < d->len && d->text[r] != '(') continue;
            hit = r;
            break;
        }
        if (hit == (size_t)-1) return NULL;

        // 抓括号里的参数名：形如 `a: Int, b: String`
        //
        // 判据是「标识符 + 可选空白 + 冒号」。只看冒号会漏掉没写类型
        // 的参数（`fn f(a, b)`），但那种写法本项目里不合法，所以不额外
        // 兼容 —— 兼容了反而会把类型名误当参数名。
        char pnames[16][64];
        size_t j = hit + 1;
        int dep = 1;
        while (j < d->len && dep > 0) {
            unsigned char c = (unsigned char)d->text[j];
            if (c == '(') dep++;
            else if (c == ')') { dep--; if (dep == 0) break; }
            else if (dep == 1) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '_' || (c >= 0x80)) {
                    size_t q0 = j;
                    while (j < d->len) {
                        unsigned char cc = (unsigned char)d->text[j];
                        if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') ||
                            (cc >= '0' && cc <= '9') || cc == '_' || cc >= 0x80) j++;
                        else break;
                    }
                    size_t k2 = j;
                    while (k2 < d->len && (d->text[k2] == ' ' || d->text[k2] == '\t')) k2++;
                    if (k2 < d->len && d->text[k2] == ':' && pcount < 16) {
                        size_t pn = j - q0;
                        if (pn >= sizeof(pnames[0])) pn = sizeof(pnames[0]) - 1;
                        memcpy(pnames[pcount], d->text + q0, pn);
                        pnames[pcount][pn] = '\0';
                        char piece[80];
                        snprintf(piece, sizeof(piece), "%s%s",
                                 pcount ? ", " : "", pnames[pcount]);
                        strncat(params, piece, sizeof(params) - strlen(params) - 1);
                        pcount++;
                    }
                    continue;
                }
            }
            j++;
        }

        // 512：name ≤ 127 + params ≤ 255 + 括号，余量足够让编译器
        // 确认不会截断。早先写 320 时它警告 —— 而内建函数参数多
        // （winBorderRect 有 6 个）时确实会被切掉右括号。
        char label[512];
        if (pcount > 0) {
            snprintf(label, sizeof(label), "%s(%s)", name, params);
        } else {
            snprintf(label, sizeof(label), "%s()", name);
        }

        // activeParameter：从 `(` 到光标数顶层逗号
        int argi = 0;
        int dep2 = 0;
        for (size_t q = hit + 1; q < off && q < d->len; q++) {
            char c = d->text[q];
            if (c == '(' || c == '[' || c == '{') dep2++;
            else if (c == ')' || c == ']' || c == '}') dep2--;
            else if (c == ',' && dep2 == 0) argi++;
        }

        Buf b = {0};
        buf_puts(&b, "{\"signatures\":[{\"label\":");
        buf_jstr(&b, label);
        buf_puts(&b, ",\"documentation\":");
        char doc[256];
        snprintf(doc, sizeof(doc), "本文件定义的函数，接受 %d 个参数。", pcount);
        buf_jstr(&b, doc);
        buf_puts(&b, ",\"parameters\":[");
        for (int a = 0; a < pcount; a++) {
            if (a) buf_puts(&b, ",");
            buf_puts(&b, "{\"label\":");
            buf_jstr(&b, pnames[a]);
            buf_puts(&b, "}");
        }
        buf_puts(&b, "]}],\"activeSignature\":0,\"activeParameter\":");
        buf_printf(&b, "%d", argi);
        buf_puts(&b, "}");
        return b.buf;
    }
}

// ============================================================
// 文档大纲（documentSymbol）
// ============================================================
//
// ── range 不能是一个点 ──────────────────────────────────────
//
// 每一项的 range 决定「在编辑器里高亮/跳转这一项时覆盖多少」。早先
// 这里 start 和 end 用同一起点终点，于是范围是零长度：大纲列表能看
// 见名字，但点击不跳转、面包屑里不显示，用户感知就是「大纲有了但
// 没用」。
//
// 函数的 range 应当覆盖整个函数体。这里没有解析器给出精确范围，
// 但能可靠地用括号配平算出来：从函数定义的 `{` 开始数到配平为止。
// 配不平（用户正在写）就退回「到文件末尾」—— 范围偏大不影响可用性，
// 偏成零点才影响。

// 从 byte 偏移找配平右花括号的位置；找不到返回 (size_t)-1
static size_t match_brace(const char* text, size_t len, size_t open) {
    int dep = 0;
    for (size_t i = open; i < len; i++) {
        char c = text[i];
        if (c == '{') dep++;
        else if (c == '}') {
            dep--;
            if (dep == 0) return i;
        }
    }
    return (size_t)-1;
}

// 行首偏移（用于 range 起点对齐到行首，让高亮覆盖整行）
static size_t line_start_of(const char* text, size_t len, size_t off) {
    if (off > len) off = len;
    while (off > 0 && text[off - 1] != '\n') off--;
    return off;
}

static char* build_document_symbols(const Doc* d) {
    scan_symbols(d);
    Buf b = {0};
    buf_puts(&b, "[");
    bool first = true;
    for (int i = 0; i < g_sym_count; i++) {
        if (!first) buf_puts(&b, ",");
        first = false;

        // 符号定义点 → 字节偏移：用 pos_to_offset 反算最直接
        size_t defoff = pos_to_offset(d->text, d->len,
                                      g_syms[i].line, g_syms[i].col);
        size_t rstart = line_start_of(d->text, d->len, defoff);
        size_t rend = defoff;

        if (g_syms[i].kind == 0 || g_syms[i].kind == 2) {
            // 函数 / 类：找到定义行之后的第一个 `{`，再配平到对应的 `}`
            size_t k = defoff;
            while (k < d->len && d->text[k] != '{' && d->text[k] != '\n') k++;
            // 跳过参数列表可能跨行的情况：一路找到 `{`
            while (k < d->len && d->text[k] != '{') k++;
            if (k < d->len) {
                size_t close = match_brace(d->text, d->len, k);
                if (close != (size_t)-1) {
                    // 覆盖到 `}` 所在行末尾
                    rend = close;
                    while (rend < d->len && d->text[rend] != '\n') rend++;
                } else {
                    rend = d->len;
                }
            }
        } else {
            // 变量：只覆盖它所在的那一行
            rend = defoff;
            while (rend < d->len && d->text[rend] != '\n') rend++;
        }

        int sl = 0, sc = 0, el = 0, ec = 0;
        offset_to_pos(d->text, d->len, rstart, &sl, &sc);
        offset_to_pos(d->text, d->len, rend, &el, &ec);

        buf_puts(&b, "{\"name\":");
        buf_jstr(&b, g_syms[i].name);
        buf_puts(&b, ",\"kind\":");
        // LSP SymbolKind：12 = Function，13 = Variable，22 = Struct。
        // 类/结构体/接口用 22 —— 大纲里它们该有类型图标，混成变量
        // 会让人以为大纲只认函数。
        buf_printf(&b, "%d",
                   g_syms[i].kind == 0 ? 12 :
                   g_syms[i].kind == 2 ? 22 : 13);

        // range：整个定义（函数体 / 一行）
        buf_puts(&b, ",\"range\":{\"start\":{\"line\":");
        buf_printf(&b, "%d", sl);
        buf_puts(&b, ",\"character\":");
        buf_printf(&b, "%d", sc);
        buf_puts(&b, "},\"end\":{\"line\":");
        buf_printf(&b, "%d", el);
        buf_puts(&b, ",\"character\":");
        buf_printf(&b, "%d", ec);
        buf_puts(&b, "}}");

        // selectionRange：必须是 range 的子集，且不能为零长度 ——
        // 编辑器用它来决定「跳转后把光标放在哪儿」，零长度虽然合法
        // 但会让跳转看起来没反应。
        int nsl = g_syms[i].line, nsc = g_syms[i].col;
        int nel = g_syms[i].line, nec = g_syms[i].col + utf16_width(g_syms[i].name);
        buf_puts(&b, ",\"selectionRange\":{\"start\":{\"line\":");
        buf_printf(&b, "%d", nsl);
        buf_puts(&b, ",\"character\":");
        buf_printf(&b, "%d", nsc);
        buf_puts(&b, "},\"end\":{\"line\":");
        buf_printf(&b, "%d", nel);
        buf_puts(&b, ",\"character\":");
        buf_printf(&b, "%d", nec);
        buf_puts(&b, "}}");
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]");
    return b.buf;
}

// ============================================================
// 定义跳转（textDocument/definition）
// ============================================================
//
// ctrl+点击一个名字，编辑器要的是「这个名字定义在哪」。响应格式是
// Location 数组（uri + range）。
//
// ── 搜索范围的三个层级 ──────────────────────────────────────
//
// 1. 当前文档（最优先 —— 同名时本文件的定义最可能是用户要的）
// 2. 其它已打开的文档（跨文件跳转的主路径：import 进来的东西多半开着）
// 3. 同目录下的 .xle / .xlc 文件（按需读盘扫描，只在前面都没找到时才做，
//    否则每次点击都读一堆文件）
//
// ── URI 与文件系统路径的互转 ────────────────────────────────
//
// VSCode 发来的 URI 把 ':'、中文、空格全部 percent-encode 了
// （file:///f%3A/%E9%A1%B9%E2%80%A6）。要拿它去列目录、读文件，
// 必须先解码；反过来，磁盘上找到的定义要回传 uri，也必须按同样的
// 规则编码 —— 两端形态不一致，编辑器要么打不开要么跳错文件。

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// file:// URI → 文件系统路径（'/' 分隔，不含 query）。只支持 file scheme。
static bool uri_to_path(const char* uri, char* out, size_t cap) {
    const char* p = NULL;
    if (strncmp(uri, "file:///", 8) == 0) p = uri + 7;          // 留下开头的 '/'
    else if (strncmp(uri, "file://localhost/", 17) == 0) p = uri + 16;
    else return false;

    size_t w = 0;
    for (; *p && w + 1 < cap; p++) {
        if (*p == '%' && hex_val(p[1]) >= 0 && hex_val(p[2]) >= 0) {
            out[w++] = (char)((hex_val(p[1]) << 4) | hex_val(p[2]));
            p += 2;
        } else if (*p == '?') {
            break;   // query 部分不属于路径
        } else {
            out[w++] = *p;
        }
    }
    out[w] = '\0';
    return w > 0;
}

// 文件系统路径 → file:// URI。除 RFC 3986 unreserved 与 '/' 外全部
// percent-encode —— 与 VSCode 自己的编码习惯一致（':' 也编成 %3A）。
static void path_to_uri(const char* path, char* out, size_t cap) {
    static const char* safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._~-";
    size_t w = 0;
    out[w++] = 'f'; out[w++] = 'i'; out[w++] = 'l'; out[w++] = 'e';
    out[w++] = ':'; out[w++] = '/'; out[w++] = '/'; out[w++] = '/';
    for (const unsigned char* p = (const unsigned char*)path;
         *p && w + 4 < cap; p++) {
        if (strchr(safe, *p)) {
            out[w++] = (char)*p;
        } else {
            w += (size_t)snprintf(out + w, cap - w, "%%%02X", *p);
        }
    }
    out[w] = '\0';
}

// 在一个文档里找名字为 w 的定义，把找到的 Location 追加进 JSON 数组。
// 返回找到的个数。
static int collect_defs(const Doc* d, const char* w, Buf* b, bool* first) {
    scan_symbols(d);
    int found = 0;
    for (int i = 0; i < g_sym_count; i++) {
        if (strcmp(g_syms[i].name, w) != 0) continue;
        if (!*first) buf_puts(b, ",");
        *first = false;
        found++;
        int el = g_syms[i].line;
        int ec = g_syms[i].col + utf16_width(g_syms[i].name);
        buf_puts(b, "{\"uri\":");
        buf_jstr(b, d->uri);
        buf_puts(b, ",\"range\":{\"start\":{\"line\":");
        buf_printf(b, "%d", g_syms[i].line);
        buf_puts(b, ",\"character\":");
        buf_printf(b, "%d", g_syms[i].col);
        buf_puts(b, "},\"end\":{\"line\":");
        buf_printf(b, "%d", el);
        buf_puts(b, ",\"character\":");
        buf_printf(b, "%d", ec);
        buf_puts(b, "}}}");
    }
    return found;
}

static char* build_definition(const Doc* d, size_t off) {
    char w[128];
    word_at(d->text, d->len, off, w, sizeof(w), NULL);
    if (!w[0]) return NULL;

    Buf b = {0};
    buf_puts(&b, "[");
    bool first = true;
    int found = collect_defs(d, w, &b, &first);

    // 其它打开的文档。cap=8 是防同名爆炸：一个名字被定义十几次时，
    // 编辑器会弹一个列表让人选，8 个足够选了。
    for (int k = 0; k < g_doc_count && found < 8; k++) {
        if (&g_docs[k] == d) continue;
        found += collect_defs(&g_docs[k], w, &b, &first);
    }

    // 还没有就去同目录的磁盘文件里找。这一步覆盖「跳进 import 的模块、
    // 跳进标准库文件」——它们没打开，但通常和当前文件在同一目录。
    if (found == 0) {
        char cur_path[900];
        if (uri_to_path(d->uri, cur_path, sizeof(cur_path))) {
            // VSCode 把 Windows 盘符路径编码成 "/f:/dir"（盘符前多一个
            // '/'）。类 Unix 的绝对路径第二个字符不可能是 ':'，所以
            // 这个判断可以放心用。
            if (cur_path[0] == '/' && cur_path[2] == ':') {
                memmove(cur_path, cur_path + 1, strlen(cur_path));
            }
            char dir[900];
            snprintf(dir, sizeof(dir), "%s", cur_path);
            char* slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                static const char* pats[2] = { "*.xle", "*.xlc" };
                for (int pi = 0; pi < 2 && found == 0; pi++) {
                    PlatDir pd;
                    if (!plat_dir_open(&pd, dir, pats[pi])) continue;
                    while (plat_dir_next(&pd) && found == 0) {
                        char fpath[1024];
                        snprintf(fpath, sizeof(fpath), "%s/%s", dir, pd.name);
                        if (strcmp(fpath, cur_path) == 0) continue;
                        size_t flen = 0;
                        char* ftext = plat_file_read(fpath, &flen);
                        if (!ftext) continue;
                        char furi[1200];
                        path_to_uri(fpath, furi, sizeof(furi));
                        Doc tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        snprintf(tmp.uri, sizeof(tmp.uri), "%s", furi);
                        tmp.text = ftext;
                        tmp.len = flen;
                        found += collect_defs(&tmp, w, &b, &first);
                        free(ftext);
                    }
                    plat_dir_close(&pd);
                }
            }
        }
    }

    if (found == 0) {
        free(b.buf);
        return NULL;
    }
    buf_puts(&b, "]");
    return b.buf;
}

// ============================================================
// 消息分发
// ============================================================

static const char* CAPS =
    "{"
    "\"positionEncoding\":\"utf-16\","
    "\"textDocumentSync\":1,"          // 1 = 全量同步
    "\"completionProvider\":{\"triggerCharacters\":[\".\",\"@\",\"(\"]},"
    "\"hoverProvider\":true,"
    "\"definitionProvider\":true,"
    "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
    "\"documentSymbolProvider\":true"
    "}";

static const char* SERVER_INFO =
    "{\"name\":\"x-lang-lsp\",\"version\":\"0.1.0\"}";

// 处理一条消息。返回值：0 = 继续，1 = 收到 exit
static int handle(const Jv* root) {
    const char* method = jv_str(jv_get(root, "method"));
    const Jv* id = jv_get(root, "id");
    const Jv* params = jv_get(root, "params");

    if (!method) {
        // 没有 method 的是响应（客户端对我们请求的回复）。
        // 本服务端不发请求，所以忽略即可。
        return 0;
    }

    // ---- 生命周期 ----
    if (strcmp(method, "initialize") == 0) {
        Buf b = {0};
        buf_puts(&b, "{\"capabilities\":");
        buf_puts(&b, CAPS);
        buf_puts(&b, ",\"serverInfo\":");
        buf_puts(&b, SERVER_INFO);
        buf_puts(&b, "}");
        send_ok_id(id, b.buf);
        free(b.buf);
        return 0;
    }
    if (strcmp(method, "initialized") == 0) return 0;

    if (strcmp(method, "shutdown") == 0) {
        send_ok_id(id, "null");
        return 0;
    }
    if (strcmp(method, "exit") == 0) return 1;

    // ---- 文档同步 ----
    if (strcmp(method, "textDocument/didOpen") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const char* txt = jv_str(jv_get(td, "text"));
        if (uri && txt) doc_open(uri, txt, strlen(txt));
        return 0;
    }
    if (strcmp(method, "textDocument/didChange") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const Jv* chs = jv_get(params, "contentChanges");
        // 全量同步：取最后一条的 text
        int n = jv_len(chs);
        if (uri && n > 0) {
            const char* txt = jv_str(jv_get(jv_at(chs, n - 1), "text"));
            if (txt) doc_open(uri, txt, strlen(txt));
        }
        return 0;
    }
    if (strcmp(method, "textDocument/didClose") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        if (uri) doc_close(uri);
        return 0;
    }

    // ---- 语言特性 ----
    if (strcmp(method, "textDocument/completion") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const Jv* pos = jv_get(params, "position");
        int line = (int)jv_int(jv_get(pos, "line"), 0);
        int ch   = (int)jv_int(jv_get(pos, "character"), 0);

        Doc* d = doc_find(uri);
        if (!d) { send_ok_id(id, "{\"isIncomplete\":false,\"items\":[]}"); return 0; }

        size_t off = pos_to_offset(d->text, d->len, line, ch);
        char* items = build_completion(d, off);
        char* list = wrap_completion_list(items);
        send_ok_id(id, list);
        free(items);
        free(list);
        return 0;
    }

    if (strcmp(method, "textDocument/hover") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const Jv* pos = jv_get(params, "position");
        int line = (int)jv_int(jv_get(pos, "line"), 0);
        int ch   = (int)jv_int(jv_get(pos, "character"), 0);

        Doc* d = doc_find(uri);
        if (!d) { send_ok_id(id, "null"); return 0; }
        size_t off = pos_to_offset(d->text, d->len, line, ch);
        char* h = build_hover(d, off);
        send_ok_id(id, h ? h : "null");
        free(h);
        return 0;
    }

    if (strcmp(method, "textDocument/definition") == 0) {
        // ctrl+点击跳定义。找不到定义回 "null"（不是 "[]"）——
        // 编辑器对 null 直接不跳，对空数组会闪一下「无结果」。
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const Jv* pos = jv_get(params, "position");
        int line = (int)jv_int(jv_get(pos, "line"), 0);
        int ch   = (int)jv_int(jv_get(pos, "character"), 0);

        Doc* d = doc_find(uri);
        if (!d) { send_ok_id(id, "null"); return 0; }
        size_t off = pos_to_offset(d->text, d->len, line, ch);
        char* r = build_definition(d, off);
        send_ok_id(id, r ? r : "null");
        free(r);
        return 0;
    }

    if (strcmp(method, "textDocument/signatureHelp") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        const Jv* pos = jv_get(params, "position");
        int line = (int)jv_int(jv_get(pos, "line"), 0);
        int ch   = (int)jv_int(jv_get(pos, "character"), 0);

        Doc* d = doc_find(uri);
        if (!d) { send_ok_id(id, "null"); return 0; }
        size_t off = pos_to_offset(d->text, d->len, line, ch);
        char* s = build_signature_help(d, off);
        send_ok_id(id, s ? s : "null");
        free(s);
        return 0;
    }

    if (strcmp(method, "textDocument/documentSymbol") == 0) {
        const Jv* td = jv_get(params, "textDocument");
        const char* uri = jv_str(jv_get(td, "uri"));
        Doc* d = doc_find(uri);
        if (!d) { send_ok_id(id, "[]"); return 0; }
        char* syms = build_document_symbols(d);
        send_ok_id(id, syms);
        free(syms);
        return 0;
    }

    // 不认识的方法：如果带 id（是请求）必须回错误，否则客户端会一直等。
    // 通知（无 id）直接忽略 —— 规范就是这么定的：通知不期待响应。
    if (id) {
        Buf b = {0};
        buf_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
        if (id->kind == JV_NUM) buf_printf(&b, "%lld", id->i);
        else if (id->kind == JV_STR) buf_jstr(&b, id->s);
        else buf_puts(&b, "null");
        buf_puts(&b, ",\"error\":{\"code\":-32601,\"message\":");
        buf_jstr(&b, "未实现的方法");
        buf_puts(&b, ",\"data\":");
        buf_jstr(&b, method);
        buf_puts(&b, "}}");
        send_msg(b.buf);
        free(b.buf);
    }
    return 0;
}

// ============================================================
// 传输层
// ============================================================

// 从 stdin 读一行（到 \n 或 \r\n）。返回 false 表示 EOF。
static bool read_line(char* out, size_t cap) {
    size_t w = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) return false;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (w + 1 < cap) out[w++] = (char)c;
    }
    out[w] = '\0';
    return true;
}

int lsp_run(void) {
    // ── 必须把 stdout/stdin 切到二进制模式（Windows 上） ──────
    //
    // LSP 的传输格式是写死的字节：`Content-Length: N\r\n\r\n<body>`。
    // 而 Windows 的 C 运行库默认把 stdout 当文本流，会把每个 '\n'
    // 翻译成 "\r\n"。于是我们写出的 "\r\n\r\n" 变成了 "\r\r\n\r\r\n"，
    // 客户端的 `\r\n\r\n` 搜索永远匹配不上 —— 表现就是「语言服务器
    // 启动了，但一直不响应」。
    //
    // 这个 bug 只在 Windows 出现，Linux/macOS 上一切正常，而且服务
    // 端自己看不出任何异常（它确实把字节写出来了，只是多了东西）。
    //
    // 修复收在 platform.c 的 plat_set_binary_stdio()（完整的因果分析
    // 在那个函数的注释里）—— 本文件不许出现平台宏，这条约定由
    // tools/check_portable.sh 机械验证。
    plat_set_binary_stdio();

    // 诊断信息走 stderr —— stdout 是协议通道，混进一个字都会让客户端
    // 解析失败。
    fprintf(stderr, "[x lsp] 已启动（stdio）\n");

    for (;;) {
        // 读头部，直到空行
        size_t content_len = 0;
        bool got_len = false;
        char line[1024];

        for (;;) {
            if (!read_line(line, sizeof(line))) {
                fprintf(stderr, "[x lsp] stdin 关闭，退出\n");
                return 0;
            }
            if (line[0] == '\0') break;      // 空行 = 头部结束
            if (strncmp(line, "Content-Length:", 15) == 0) {
                content_len = (size_t)strtoul(line + 15, NULL, 10);
                got_len = true;
            }
            // 其它头（Content-Type）忽略
        }

        if (!got_len || content_len == 0) continue;

        // 上限：LSP 消息不该超过 8 MB。超了说明头部被污染或客户端
        // 有问题，继续读会把内存吃光。
        if (content_len > 8u * 1024u * 1024u) {
            fprintf(stderr, "[x lsp] 消息过大（%zu 字节），丢弃\n", content_len);
            // 必须把这条消息吃掉，否则后面全错位
            for (size_t i = 0; i < content_len; i++) {
                if (fgetc(stdin) == EOF) return 0;
            }
            continue;
        }

        char* body = (char*)malloc(content_len + 1);
        if (!body) {
            fprintf(stderr, "[x lsp] 内存不足\n");
            return 70;
        }
        size_t got = fread(body, 1, content_len, stdin);
        body[got] = '\0';

        char err[256];
        Jv* root = jv_parse(body, got, err, sizeof(err));
        if (!root) {
            fprintf(stderr, "[x lsp] JSON 解析失败：%s\n", err);
            fprintf(stderr, "[x lsp] 收到的前 200 字节：%.200s\n", body);
            free(body);
            continue;
        }

        int r = handle(root);
        free(body);
        if (r == 1) {
            fprintf(stderr, "[x lsp] exit，退出版\n");
            return 0;
        }
    }
}