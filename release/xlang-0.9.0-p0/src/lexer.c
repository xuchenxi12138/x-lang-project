// lexer.c — X 语言词法分析（P0）
//
// 设计约束（来自 01-语言设计.md）：
//   - 标识符永远 Unicode（铁律 6），因此中文标识符合法
//   - 关键字 45 个，控制在 50 以内是硬约束
//   - 中文关键字是「词法别名」（04 文档），默认关闭，需 #lang zh

#include "x.h"
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

// ============================================================
// 关键字表
// ============================================================
// 45 个，与 01 §2.4 完全一致。按字母序排列以便二分/线性查找。

typedef struct {
    const char* word;
    TokKind     kind;
} Keyword;

static const Keyword kKeywords[] = {
    {"as",       TK_AS},
    {"break",    TK_BREAK},
    {"case",     TK_CASE},
    {"catch",    TK_CATCH},
    {"comptime", TK_COMPTIME},
    {"const",    TK_CONST},
    {"continue", TK_CONTINUE},
    {"defer",    TK_DEFER},
    {"do",       TK_DO},
    {"dyn",      TK_DYN},
    {"else",     TK_ELSE},
    {"enum",     TK_ENUM},
    {"extern",   TK_EXTERN},
    {"false",    TK_FALSE},
    {"fn",       TK_FN},
    {"for",      TK_FOR},
    {"from",     TK_FROM},
    {"if",       TK_IF},
    {"impl",     TK_IMPL},
    {"import",   TK_IMPORT},
    {"in",       TK_IN},
    {"let",      TK_LET},
    {"loop",     TK_LOOP},
    {"match",    TK_MATCH},
    {"mod",      TK_MOD},
    {"mut",      TK_MUT},
    {"not",      TK_NOT},
    {"null",     TK_NULL},
    {"or",       TK_OR},
    {"par",      TK_PAR},
    {"pub",      TK_PUB},
    // `public` 是 `pub` 的同义词。
    //
    // ── 为什么需要这个别名 ────────────────────────────────────
    //
    // 类系统的另外两个修饰符是 `private` / `protected`（都是全写），
    // 只有公开那个是缩写 `pub`。于是 `private x` 旁边写 `pub y`
    // 看起来像打错了 —— 实际写代码时几乎每个人第一次都会写成
    // `public`，然后拿到一条「类体里期望字段或方法」的报错，而那个
    // 报错完全没提示「也许你想写 pub」。
    //
    // 两个词都接受，代价是关键字表多一行，收益是「三个修饰符能被
    // 写成同一套风格」。已有的 `pub` 保持不变（顶层 `pub` 仍然有效）。
    {"public",   TK_PUB},
    {"ref",      TK_REF},
    {"return",   TK_RETURN},
    {"self",     TK_SELF},
    {"spawn",    TK_SPAWN},
    {"struct",   TK_STRUCT},
    {"trait",    TK_TRAIT},
    {"true",     TK_TRUE},
    {"type",     TK_TYPE},
    {"unsafe",   TK_UNSAFE},
    {"var",      TK_VAR},
    {"void",     TK_VOID},
    {"where",    TK_WHERE},
    {"while",    TK_WHILE},
    {"yield",    TK_YIELD},

    // 类系统（v0.15）
    {"class",      TK_CLASS},
    {"extends",    TK_EXTENDS},
    {"implements", TK_IMPLEMENTS},
    {"abstract",   TK_ABSTRACT},
    {"interface",  TK_INTERFACE},
    {"override",   TK_OVERRIDE},
    {"private",    TK_PRIVATE},
    {"protected",  TK_PROTECTED},
    {"new",        TK_NEW},
    {"super",      TK_SUPER},
    {"final",      TK_FINAL},
};

#define KEYWORD_COUNT ((int)(sizeof(kKeywords) / sizeof(kKeywords[0])))

static TokKind lookup_keyword(const char* s, size_t len) {
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        if (strlen(kKeywords[i].word) == len && memcmp(kKeywords[i].word, s, len) == 0) {
            return kKeywords[i].kind;
        }
    }
    return TK_IDENT;
}

// ============================================================
// 中文关键字别名表（04 文档 §2.3 的 45 项）
// ============================================================
// 只在 #lang zh 或 --lang=zh 时启用。默认关闭，所以英文项目里
// `如果` 是普通标识符（04 §2.5「可关闭」）。

typedef struct {
    const char* zh;
    TokKind     kind;
} ZhAlias;

static const ZhAlias kZhAliases[] = {
    {"函",     TK_FN},      {"令",     TK_LET},
    {"变",     TK_VAR},     {"常",     TK_CONST},
    {"结构",   TK_STRUCT},  {"枚举",   TK_ENUM},
    {"特征",   TK_TRAIT},   {"实现",   TK_IMPL},
    {"模组",   TK_MOD},     {"引入",   TK_IMPORT},
    {"从",     TK_FROM},    {"公开",   TK_PUB},
    {"自身",   TK_SELF},    {"真",     TK_TRUE},
    {"假",     TK_FALSE},   {"空",     TK_NULL},
    {"可变",   TK_MUT},     {"引用",   TK_REF},
    {"类型",   TK_TYPE},    {"作为",   TK_AS},
    {"非",     TK_NOT},     {"编译期", TK_COMPTIME},
    {"外部",   TK_EXTERN},  {"做",     TK_DO},
    {"捕获",   TK_CATCH},   {"如果",   TK_IF},
    {"否则",   TK_ELSE},    {"当",     TK_WHILE},
    {"遍历",   TK_FOR},     {"于",     TK_IN},
    {"循环",   TK_LOOP},    {"跳出",   TK_BREAK},
    {"继续",   TK_CONTINUE},{"匹配",   TK_MATCH},
    {"情形",   TK_CASE},    {"返回",   TK_RETURN},
    {"产出",   TK_YIELD},   {"延后",   TK_DEFER},
    {"派生",   TK_SPAWN},   {"并行",   TK_PAR},
    {"动态",   TK_DYN},     {"约束",   TK_WHERE},
    {"或",     TK_OR},      {"不安全", TK_UNSAFE},
    {"无",     TK_VOID},

    // 类系统（v0.15）
    {"类",     TK_CLASS},       {"继承",   TK_EXTENDS},
    {"实现接口", TK_IMPLEMENTS}, {"抽象",  TK_ABSTRACT},
    {"接口",   TK_INTERFACE},   {"覆写",   TK_OVERRIDE},
    {"私有",   TK_PRIVATE},     {"保护",   TK_PROTECTED},
    {"新建",   TK_NEW},         {"父类",   TK_SUPER},
    {"终态",   TK_FINAL},
};

// ------------------------------------------------------------
// 内建类型名的中文映射（04 文档 §2.4）
// ------------------------------------------------------------
// 与关键字的处理不同：类型名是**标识符**，不是关键字。
// 所以不能映射成新的 TokKind，而是把标识符的**文本**归一化为英文。
// 这样 `let x: 整数 = 1` 与 `let x: Int = 1` 在语法树里完全一样，
// 后续所有阶段（类型推断、诊断、字节码）都不需要知道中文的存在。
//
// 这是 04 §2.5「零成本」的实现细节：归一化只发生在词法器内一次，
// 运行时零开销，编译器后续阶段也零分支。

typedef struct {
    const char* zh;
    const char* en;
} ZhTypeAlias;

static const ZhTypeAlias kZhTypeAliases[] = {
    {"整数",   "Int"},
    {"浮点",   "Float"},
    {"布尔",   "Bool"},
    {"字符串", "String"},
    {"字符",   "Char"},
    {"复数",   "Complex"},
    {"数组",   "Array"},
    {"任意",   "Any"},      // 多态参数 / 透传返回（见 compile.c 重载解析）
    {"无",     "void"},      // 与 void 关键字冲突时以关键字优先，见 lex_ident
};

#define ZH_TYPE_COUNT ((int)(sizeof(kZhTypeAliases) / sizeof(kZhTypeAliases[0])))

static const char* lookup_zh_type(const char* s, size_t len) {
    for (int i = 0; i < ZH_TYPE_COUNT; i++) {
        if (strlen(kZhTypeAliases[i].zh) == len &&
            memcmp(kZhTypeAliases[i].zh, s, len) == 0) {
            return kZhTypeAliases[i].en;
        }
    }
    return NULL;
}

// ------------------------------------------------------------
// 普通标识符的中文归一化（04 §2.6）
// ------------------------------------------------------------
//
// 入口函数名 `main` 不是关键字，所以进不了上面的关键字别名表；
// 但规格 04 §2.6 的示例明确写着 `公开 函 主()` —— 中文文件里
// 连入口函数都要写英文，体验就断在最后一行。
//
// 处理方式与类型名相同：zh 模式下把标识符**文本**归一化为英文。
// 整 token 比较，所以 `主函数`、`主键` 这样的名字不受影响。
//
// 为什么只放 主 一个：这是目前唯一的「按名字查找」的编译器约定
// （入口检测）；包名、模块名、包内符号都不在编译器手里翻译
// （04 §2.5.1），不该开这个口子。
static const ZhTypeAlias kZhNameAliases[] = {
    {"主", "main"},
};

#define ZH_NAME_COUNT ((int)(sizeof(kZhNameAliases) / sizeof(kZhNameAliases[0])))

static const char* lookup_zh_name(const char* s, size_t len) {
    for (int i = 0; i < ZH_NAME_COUNT; i++) {
        if (strlen(kZhNameAliases[i].zh) == len &&
            memcmp(kZhNameAliases[i].zh, s, len) == 0) {
            return kZhNameAliases[i].en;
        }
    }
    return NULL;
}

#define ZH_ALIAS_COUNT ((int)(sizeof(kZhAliases) / sizeof(kZhAliases[0])))

static bool g_zh_mode = false;

void lexer_set_zh_mode(bool on) { g_zh_mode = on; }
bool lexer_zh_mode(void) { return g_zh_mode; }

static TokKind lookup_zh_alias(const char* s, size_t len) {
    for (int i = 0; i < ZH_ALIAS_COUNT; i++) {
        if (strlen(kZhAliases[i].zh) == len && memcmp(kZhAliases[i].zh, s, len) == 0) {
            return kZhAliases[i].kind;
        }
    }
    return TK_IDENT;
}

// ============================================================
// 名称表（给诊断用）
// ============================================================

static const char* kTokNames[TK_COUNT] = {
    [TK_EOF] = "文件结束",
    [TK_INT] = "整数字面量",
    [TK_FLOAT] = "浮点字面量",
    [TK_STRING] = "字符串字面量",
    [TK_IDENT] = "标识符",
    [TK_CHAR] = "字符字面量",
    [TK_AS] = "as", [TK_BREAK] = "break", [TK_CASE] = "case",
    [TK_CATCH] = "catch", [TK_COMPTIME] = "comptime", [TK_CONST] = "const",
    [TK_CONTINUE] = "continue", [TK_DEFER] = "defer", [TK_DO] = "do",
    [TK_DYN] = "dyn", [TK_ELSE] = "else", [TK_ENUM] = "enum",
    [TK_EXTERN] = "extern", [TK_FALSE] = "false", [TK_FN] = "fn",
    [TK_FOR] = "for", [TK_FROM] = "from", [TK_IF] = "if",
    [TK_IMPL] = "impl", [TK_IMPORT] = "import", [TK_IN] = "in",
    [TK_LET] = "let", [TK_LOOP] = "loop", [TK_MATCH] = "match",
    [TK_MOD] = "mod", [TK_MUT] = "mut", [TK_NOT] = "not",
    [TK_NULL] = "null", [TK_OR] = "or", [TK_PAR] = "par",
    [TK_PUB] = "pub", [TK_REF] = "ref", [TK_RETURN] = "return",
    [TK_SELF] = "self", [TK_SPAWN] = "spawn", [TK_STRUCT] = "struct",
    [TK_TRAIT] = "trait", [TK_TRUE] = "true", [TK_TYPE] = "type",
    [TK_UNSAFE] = "unsafe", [TK_VAR] = "var", [TK_VOID] = "void",
    // 类系统（v0.15）
    [TK_CLASS] = "class", [TK_EXTENDS] = "extends",
    [TK_IMPLEMENTS] = "implements", [TK_ABSTRACT] = "abstract",
    [TK_INTERFACE] = "interface", [TK_OVERRIDE] = "override",
    [TK_PRIVATE] = "private", [TK_PROTECTED] = "protected",
    [TK_NEW] = "new", [TK_SUPER] = "super", [TK_FINAL] = "final",
    [TK_WHERE] = "where", [TK_WHILE] = "while", [TK_YIELD] = "yield",
    [TK_PLUS] = "+", [TK_MINUS] = "-", [TK_STAR] = "*", [TK_SLASH] = "/",
    [TK_PERCENT] = "%", [TK_PLUSPLUS] = "++", [TK_MINUSMINUS] = "--",
    [TK_PLUS_EQ] = "+=", [TK_MINUS_EQ] = "-=", [TK_STAR_EQ] = "*=",
    [TK_SLASH_EQ] = "/=", [TK_PERCENT_EQ] = "%=", [TK_ASSIGN] = "=",
    [TK_EQ] = "==", [TK_NE] = "!=", [TK_LT] = "<", [TK_LE] = "<=",
    [TK_GT] = ">", [TK_GE] = ">=", [TK_ANDAND] = "&&", [TK_OROR] = "||",
    [TK_NOT_NOT] = "!!", [TK_AMP] = "&", [TK_PIPE] = "|", [TK_CARET] = "^",
    [TK_TILDE] = "~", [TK_SHL] = "<<", [TK_SHR] = ">>",
    [TK_AMP_EQ] = "&=", [TK_PIPE_EQ] = "|=", [TK_CARET_EQ] = "^=",
    [TK_SHL_EQ] = "<<=", [TK_SHR_EQ] = ">>=", [TK_ARROW] = "->",
    [TK_FATARROW] = "=>", [TK_QUESTION] = "?", [TK_QQ] = "??",
    [TK_DOT] = ".", [TK_DOTDOT] = "..", [TK_DOTDOT_EQ] = "..=",
    [TK_COMMA] = ",", [TK_COLON] = ":", [TK_SEMI] = ";",
    [TK_LPAREN] = "(", [TK_RPAREN] = ")", [TK_LBRACE] = "{",
    [TK_RBRACE] = "}", [TK_LBRACKET] = "[", [TK_RBRACKET] = "]",
    [TK_AT] = "@", [TK_HASH] = "#", [TK_BANG] = "!",
};

const char* tok_name(TokKind k) {
    if (k < 0 || k >= TK_COUNT) return "?";
    const char* n = kTokNames[k];
    return n ? n : "?";
}

// ============================================================
// 词法器本体
// ============================================================

void lexer_init(Lexer* lx, const char* file, const char* src, size_t len, Arena* a) {
    lx->src   = src;
    lx->len   = len;
    lx->pos   = 0;
    lx->line  = 1;
    lx->col   = 1;
    lx->file  = file;
    lx->arena = a;
}

static int peek(Lexer* lx) {
    if (lx->pos >= lx->len) return -1;
    return (unsigned char)lx->src[lx->pos];
}

static int peek2(Lexer* lx) {
    if (lx->pos + 1 >= lx->len) return -1;
    return (unsigned char)lx->src[lx->pos + 1];
}

static int peek3(Lexer* lx) {
    if (lx->pos + 2 >= lx->len) return -1;
    return (unsigned char)lx->src[lx->pos + 2];
}

static int advance(Lexer* lx) {
    if (lx->pos >= lx->len) return -1;
    int c = (unsigned char)lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else if (c == '\r') {
        // CR 不计列：CRLF 文件里若把 CR 也算上，每行最后一个字符的
        // 列号会整体偏 1（Windows 用户看到的列号全是错的）。
    } else if ((c & 0xC0) == 0x80) {
        // UTF-8 续字节（10xxxxxx）：**不增加列号**。
        //
        // 这是 04 §6.1 与 06 §3 的共同要求：位置必须按字符计，不是按字节。
        // 否则「令 年龄 = 姓名 + 不存在的」里的 `不存在的` 会被报在第 23 列
        // （字节偏移），而用户编辑器里看到的是第 13 列——中文一个字 3 字节，
        // 差出来的正是这类累积误差。诊断位置错了就等于没有诊断。
    } else {
        lx->col++;
    }
    return c;
}

static SrcPos cur_pos(Lexer* lx) {
    SrcPos p = { lx->file, lx->line, lx->col };
    return p;
}

static bool is_ident_start(int c) {
    if (c < 0) return false;
    // ASCII 字母与下划线
    if (isalpha(c) || c == '_') return true;
    // UTF-8 多字节序列的首字节（>= 0x80）：中文标识符合法（铁律 6）
    // 不在这里做完整的 Unicode 类别判断——P0 只需「字节 >= 0x80 就当标识符字符」，
    // 真正的类别校验放到 P1（那时需要 NFC 规范化，见 04 §6.1 列宽问题）。
    return c >= 0x80;
}

static bool is_ident_continue(int c) {
    if (c < 0) return false;
    if (isalnum(c) || c == '_') return true;
    return c >= 0x80;
}

// 跳过空白与注释。返回 false 表示已到文件尾。
static bool skip_trivia(Lexer* lx) {
    for (;;) {
        int c = peek(lx);
        if (c < 0) return false;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
            continue;
        }
        // 行注释 //
        if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) >= 0 && peek(lx) != '\n') advance(lx);
            continue;
        }
        // 块注释 /* */ （可嵌套，与 01 的语法简单原则一致：不做预处理）
        if (c == '/' && peek2(lx) == '*') {
            SrcPos start = cur_pos(lx);
            advance(lx); advance(lx);
            int depth = 1;
            while (depth > 0) {
                if (peek(lx) < 0) {
                    diag_error(start, "块注释没有闭合：期望 `*/`，但已到文件末尾");
                    return false;
                }
                if (peek(lx) == '/' && peek2(lx) == '*') {
                    advance(lx); advance(lx); depth++;
                } else if (peek(lx) == '*' && peek2(lx) == '/') {
                    advance(lx); advance(lx); depth--;
                } else {
                    advance(lx);
                }
            }
            continue;
        }
        return true;
    }
}

// 读一个标识符/关键字。返回的字符串在 arena 里。
static Token lex_ident(Lexer* lx, SrcPos pos) {
    size_t start = lx->pos;
    while (is_ident_continue(peek(lx))) advance(lx);
    size_t len = lx->pos - start;
    const char* s = lx->src + start;

    Token t;
    memset(&t, 0, sizeof(t));
    t.pos  = pos;
    t.len  = len;
    t.s    = arena_strndup(lx->arena, s, len);

    // 中文模式的处理顺序很重要（04 §2.5）：
    //   1. 先查关键字别名 —— 若命中，整个 token 变成那个关键字
    //   2. 再查内建类型名 —— 若命中，把文本归一化为英文标识符
    //   3. 都没命中，按普通标识符处理（此时 `如果x` 这样的名字是合法的）
    // 顺序不能颠倒：`无` 既是关键字别名（void）又在类型表里，
    // 关键字优先，这样 `fn f() -> 无` 也能正确解析。
    if (g_zh_mode) {
        TokKind k = lookup_zh_alias(s, len);
        if (k != TK_IDENT) {
            t.kind = k;
            return t;
        }
        const char* en = lookup_zh_type(s, len);
        if (en) {
            t.kind = TK_IDENT;
            t.s    = arena_strdup(lx->arena, en);
            t.len  = strlen(en);
            return t;
        }
        en = lookup_zh_name(s, len);
        if (en) {
            t.kind = TK_IDENT;
            t.s    = arena_strdup(lx->arena, en);
            t.len  = strlen(en);
            return t;
        }
    }
    t.kind = lookup_keyword(s, len);
    return t;
}

static Token lex_number(Lexer* lx, SrcPos pos) {
    size_t start = lx->pos;
    bool is_float = false;

    // 0x / 0b / 0o
    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        advance(lx); advance(lx);
        while (isxdigit(peek(lx)) || peek(lx) == '_') advance(lx);
        Token t; memset(&t, 0, sizeof(t));
        t.kind = TK_INT; t.pos = pos;
        // 手工解析十六进制（忽略下划线）
        int64_t v = 0;
        for (size_t i = start + 2; i < lx->pos; i++) {
            char c = lx->src[i];
            if (c == '_') continue;
            v = v * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
        }
        t.i = v;
        return t;
    }
    if (peek(lx) == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
        advance(lx); advance(lx);
        while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_') advance(lx);
        Token t; memset(&t, 0, sizeof(t));
        t.kind = TK_INT; t.pos = pos;
        int64_t v = 0;
        for (size_t i = start + 2; i < lx->pos; i++) {
            char c = lx->src[i];
            if (c == '_') continue;
            v = v * 2 + (c - '0');
        }
        t.i = v;
        return t;
    }

    while (isdigit(peek(lx)) || peek(lx) == '_') advance(lx);

    // 小数点：只有后面跟数字才算浮点（避免把 `1..5` 的 `..` 吃掉）
    if (peek(lx) == '.' && isdigit(peek2(lx))) {
        is_float = true;
        advance(lx);
        while (isdigit(peek(lx)) || peek(lx) == '_') advance(lx);
    }
    // 指数
    if (peek(lx) == 'e' || peek(lx) == 'E') {
        int n1 = peek2(lx);
        int n2 = peek3(lx);
        if (isdigit(n1) || ((n1 == '+' || n1 == '-') && isdigit(n2))) {
            is_float = true;
            advance(lx);
            if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
            while (isdigit(peek(lx))) advance(lx);
        }
    }

    Token t; memset(&t, 0, sizeof(t));
    t.pos = pos;

    if (is_float) {
        // 去掉下划线的临时缓冲
        char buf[128];
        size_t n = 0;
        for (size_t i = start; i < lx->pos && n < sizeof(buf) - 1; i++) {
            if (lx->src[i] != '_') buf[n++] = lx->src[i];
        }
        buf[n] = '\0';
        t.kind = TK_FLOAT;
        t.f    = strtod(buf, NULL);
    } else {
        int64_t v = 0;
        bool overflow = false;
        for (size_t i = start; i < lx->pos; i++) {
            char c = lx->src[i];
            if (c == '_') continue;
            if (v > (INT64_MAX - (c - '0')) / 10) { overflow = true; }
            v = v * 10 + (c - '0');
        }
        if (overflow) {
            diag_error(pos, "整数字面量超出 Int64 范围：`%.*s`",
                       (int)(lx->pos - start), lx->src + start);
        }
        t.kind = TK_INT;
        t.i    = v;
    }
    return t;
}

// 处理字符串转义。返回 arena 里的字符串，长度写入 out_len。
//
// 导出（不再是 static）：字符串插值在 parser 里把 `"a{x}b"` 拆成
// 多个片段，每段仍需要各自 unescape。若 parser 另写一份，两处转义
// 规则迟早走样 —— 症状是「`"\n"` 正常，`"x{\n}y"` 里的 `\n` 变成字面
// 的 N`。所以只保留这一份实现。
char* x_unescape_string(Arena* a, const char* raw, size_t raw_len,
                        size_t* out_len, SrcPos pos) {
    char* buf = arena_alloc(a, raw_len + 1);
    size_t n = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (c != '\\') { buf[n++] = c; continue; }
        if (i + 1 >= raw_len) {
            diag_error(pos, "字符串以单个反斜杠结尾：反斜杠后需要一个转义字符");
            break;
        }
        char e = raw[++i];
        switch (e) {
            case 'n':  buf[n++] = '\n'; break;
            case 't':  buf[n++] = '\t'; break;
            case 'r':  buf[n++] = '\r'; break;
            case '0':  buf[n++] = '\0'; break;
            case '\\': buf[n++] = '\\'; break;
            case '"':  buf[n++] = '"';  break;
            case '\'': buf[n++] = '\''; break;
            case '{':  buf[n++] = '{';  break;   // {{ }} 插值转义（01 §3.6）
            case '}':  buf[n++] = '}';  break;
            case 'u': {
                // \u{XXXX}
                if (i + 1 < raw_len && raw[i + 1] == '{') {
                    i += 2;
                    uint32_t cp = 0;
                    while (i < raw_len && raw[i] != '}') {
                        char h = raw[i];
                        int d = isdigit((unsigned char)h) ? h - '0'
                              : (isxdigit((unsigned char)h) ? tolower((unsigned char)h) - 'a' + 10 : -1);
                        if (d < 0) break;
                        cp = cp * 16 + d;
                        i++;
                    }
                    // UTF-8 编码
                    if (cp < 0x80) {
                        buf[n++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[n++] = (char)(0xC0 | (cp >> 6));
                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[n++] = (char)(0xE0 | (cp >> 12));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[n++] = (char)(0xF0 | (cp >> 18));
                        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    diag_error(pos, "无效的 Unicode 转义：应为 `\\u{XXXX}` 形式");
                }
                break;
            }
            default:
                diag_error(pos, "未知的转义字符 `\\%c`", e);
                buf[n++] = e;
                break;
        }
    }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static Token lex_string(Lexer* lx, SrcPos pos) {
    advance(lx);   // 开引号
    size_t start = lx->pos;
    while (peek(lx) >= 0 && peek(lx) != '"') {
        if (peek(lx) == '\\') advance(lx);
        if (peek(lx) >= 0) advance(lx);
    }
    if (peek(lx) < 0) {
        diag_error(pos, "字符串没有闭合：缺少结尾的 `\"`");
        Token t; memset(&t, 0, sizeof(t));
        t.kind = TK_STRING; t.pos = pos; t.s = ""; t.len = 0;
        return t;
    }
    size_t raw_len = lx->pos - start;
    advance(lx);   // 闭引号

    size_t out_len = 0;
    char* s = x_unescape_string(lx->arena, lx->src + start, raw_len, &out_len, pos);

    Token t; memset(&t, 0, sizeof(t));
    t.kind = TK_STRING;
    t.pos  = pos;
    t.s    = s;
    t.len  = out_len;
    // 转义原文另存一份：插值拆分要区分 `{` 与 `\u{...}` 里的 `{`，
    // 而 unescape 之后这个信息已经没了。
    char* raw = arena_alloc(lx->arena, raw_len + 1);
    memcpy(raw, lx->src + start, raw_len);
    raw[raw_len] = '\0';
    t.raw     = raw;
    t.raw_len = raw_len;
    return t;
}

// 原始字符串 `r"..."`：不处理任何转义，也不做插值（v0.17）。
//
// ── 为什么必须有它 ─────────────────────────────────────
//
// 插值实现之后，`"{name}"` 会被当表达式求值。而运行时模板
// （`strTemplate("{name}", keys, vals)`）、正则、JSON 片段、代码生成器
// 里的模板，全都需要「花括号原样保留」。没有原始字符串的话，
// 这些地方的 `{` 必须逐个写成 `\{` 或 `{{` —— 而且 `strTemplate`
// 的模板常常是拼接出来的，根本无法在字面量里转义。
//
// 规范 01 §2.5 早就列了 `r"原始\字符串"`，这里补上实现。
//
// ── 关键取舍：raw 字段置空 ────────────────────────────
//
// parser 靠 Token.raw 判断要不要做插值。原始字符串把 raw 置成 NULL，
// 于是 `"a{x}b"` 的插值路径完全不会被触发 —— 不需要在 parser 里
// 再加一个 `is_raw` 标志。一个字段两种含义，但方向一致：
// 「没有 raw」= 「不参与插值」。
static Token lex_raw_string(Lexer* lx, SrcPos pos) {
    advance(lx);   // `r`
    advance(lx);   // 开引号
    size_t start = lx->pos;
    while (peek(lx) >= 0 && peek(lx) != '"') {
        // 原始字符串里 `\` 没有特殊含义，直接往后走
        advance(lx);
    }
    if (peek(lx) < 0) {
        diag_error(pos, "原始字符串没有闭合：缺少结尾的 `\"`");
        diag_help("原始字符串写成 `r\"...\"`，内部不做任何转义，"
                  "所以不能包含 `\"`");
        Token t; memset(&t, 0, sizeof(t));
        t.kind = TK_STRING; t.pos = pos; t.s = ""; t.len = 0;
        return t;
    }
    size_t raw_len = lx->pos - start;
    advance(lx);   // 闭引号

    char* s = arena_alloc(lx->arena, raw_len + 1);
    memcpy(s, lx->src + start, raw_len);
    s[raw_len] = '\0';

    Token t; memset(&t, 0, sizeof(t));
    t.kind = TK_STRING;
    t.pos  = pos;
    t.s    = s;
    t.len  = raw_len;
    // raw 故意留 NULL —— 见上面注释
    t.raw     = NULL;
    t.raw_len = 0;
    return t;
}

// 单字符 token 的便捷构造
static Token mk(Lexer* lx, TokKind k, SrcPos pos) {
    Token t; memset(&t, 0, sizeof(t));
    t.kind = k; t.pos = pos;
    (void)lx;
    return t;
}

Token lexer_next(Lexer* lx) {
    if (!skip_trivia(lx)) {
        return mk(lx, TK_EOF, cur_pos(lx));
    }
    SrcPos pos = cur_pos(lx);
    int c = peek(lx);

    // 原始字符串 `r"..."`。必须排在标识符分支**之前** ——
// 否则 `r` 先被 lex_ident 吃掉，退化成「未定义的变量 r」。
    if (c == 'r' && lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '"') {
        return lex_raw_string(lx, pos);
    }

    // 标识符 / 关键字
    if (is_ident_start(c)) {
        return lex_ident(lx, pos);
    }

    // 数字
    if (isdigit(c)) {
        return lex_number(lx, pos);
    }

    // 字符串
    if (c == '"') {
        return lex_string(lx, pos);
    }

    // 运算符与分隔符：先匹配长的
    advance(lx);
    switch (c) {
        case '+':
            if (peek(lx) == '+') { advance(lx); return mk(lx, TK_PLUSPLUS, pos); }
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_PLUS_EQ, pos); }
            return mk(lx, TK_PLUS, pos);
        case '-':
            if (peek(lx) == '-') { advance(lx); return mk(lx, TK_MINUSMINUS, pos); }
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_MINUS_EQ, pos); }
            if (peek(lx) == '>') { advance(lx); return mk(lx, TK_ARROW, pos); }
            return mk(lx, TK_MINUS, pos);
        case '*':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_STAR_EQ, pos); }
            return mk(lx, TK_STAR, pos);
        case '/':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_SLASH_EQ, pos); }
            return mk(lx, TK_SLASH, pos);
        case '%':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_PERCENT_EQ, pos); }
            return mk(lx, TK_PERCENT, pos);
        case '=':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_EQ, pos); }
            if (peek(lx) == '>') { advance(lx); return mk(lx, TK_FATARROW, pos); }
            return mk(lx, TK_ASSIGN, pos);
        case '!':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_NE, pos); }
            if (peek(lx) == '!') { advance(lx); return mk(lx, TK_NOT_NOT, pos); }
            return mk(lx, TK_BANG, pos);
        case '<':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_LE, pos); }
            if (peek(lx) == '<') {
                advance(lx);
                if (peek(lx) == '=') { advance(lx); return mk(lx, TK_SHL_EQ, pos); }
                return mk(lx, TK_SHL, pos);
            }
            return mk(lx, TK_LT, pos);
        case '>':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_GE, pos); }
            if (peek(lx) == '>') {
                advance(lx);
                if (peek(lx) == '=') { advance(lx); return mk(lx, TK_SHR_EQ, pos); }
                return mk(lx, TK_SHR, pos);
            }
            return mk(lx, TK_GT, pos);
        case '&':
            if (peek(lx) == '&') { advance(lx); return mk(lx, TK_ANDAND, pos); }
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_AMP_EQ, pos); }
            return mk(lx, TK_AMP, pos);
        case '|':
            if (peek(lx) == '|') { advance(lx); return mk(lx, TK_OROR, pos); }
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_PIPE_EQ, pos); }
            return mk(lx, TK_PIPE, pos);
        case '^':
            if (peek(lx) == '=') { advance(lx); return mk(lx, TK_CARET_EQ, pos); }
            return mk(lx, TK_CARET, pos);
        case '~': return mk(lx, TK_TILDE, pos);
        case '?':
            if (peek(lx) == '?') { advance(lx); return mk(lx, TK_QQ, pos); }
            return mk(lx, TK_QUESTION, pos);
        case '.':
            if (peek(lx) == '.') {
                advance(lx);
                if (peek(lx) == '=') { advance(lx); return mk(lx, TK_DOTDOT_EQ, pos); }
                return mk(lx, TK_DOTDOT, pos);
            }
            return mk(lx, TK_DOT, pos);
        case ',': return mk(lx, TK_COMMA, pos);
        case ':': return mk(lx, TK_COLON, pos);
        case ';': return mk(lx, TK_SEMI, pos);
        case '(': return mk(lx, TK_LPAREN, pos);
        case ')': return mk(lx, TK_RPAREN, pos);
        case '{': return mk(lx, TK_LBRACE, pos);
        case '}': return mk(lx, TK_RBRACE, pos);
        case '[': return mk(lx, TK_LBRACKET, pos);
        case ']': return mk(lx, TK_RBRACKET, pos);
        case '@': return mk(lx, TK_AT, pos);
        case '#': return mk(lx, TK_HASH, pos);
        default:
            break;
    }

    // 无法识别的字符。给出可点击的位置（06 文档要求）。
    diag_error(pos, "无法识别的字符 `%c`（0x%02X）", (c >= 32 && c < 127) ? c : '?', c);
    return mk(lx, TK_EOF, pos);
}

Token* lex_all(Lexer* lx, size_t* out_count) {
    size_t cap = 256, n = 0;
    Token* toks = x_alloc(cap * sizeof(Token));
    for (;;) {
        Token t = lexer_next(lx);
        if (n == cap) {
            cap *= 2;
            toks = x_realloc(toks, cap * sizeof(Token));
        }
        toks[n++] = t;
        if (t.kind == TK_EOF) break;
    }
    *out_count = n;
    return toks;
}