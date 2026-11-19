// x.h — X 语言编译器公共定义（P0）
// 一个单头文件覆盖词法、语法、AST、IR、字节码、解释器的全部数据结构。
// 原则：先让语义跑对，不做任何优化（优化在 P1 按 07-高性能解释器.md 分层加）。

#ifndef X_H
#define X_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 平台边界。这里 include 而不是让各 .c 自己 include，是为了保证
// **没有任何 .c 会忘记它**——忘记的后果是那个文件里悄悄写了
// #ifdef _WIN32，然后在 Linux 上编不过，而你在 Windows 上毫无察觉。
// 规则与自动检查见 platform.h 头部说明。
#include "platform.h"

// 编译目标（产物格式、后缀、工具链）。平台边界定「我跑在哪」，
// 目标定「我要产出给谁」——交叉编译时这两个是不同的东西。
#include "target.h"

// 使用场景（Profile）。目标定「给哪个平台编」，档位定「给哪种场景编」。
// 这两件事正交：x86_64-linux 上既可以有控制台工具，也可以有服务；
// 而同一份「桌面」档在 Windows 上要 WINDOWS 子系统、在 Linux 上不要。
#include "profile.h"

// 包清单（x.toml）与包查找。`import @name` 的解析靠它。
//
// 注意这里 include pkg.h 会形成 x.h → pkg.h 的依赖，而 pkg.c 又
// include x.h。这在 C 里是合法的（头文件有 include guard），
// 顺序是：pkg.c 先拿到 x.h，x.h 里 include pkg.h 时 pkg.h 的
// 内容还没被 include 过，于是正常展开；之后 pkg.c 自己的
// `#include "pkg.h"` 被 guard 挡掉。所以不需要额外处理。
#include "pkg.h"

// ============================================================
// 1. 内存工具
// ============================================================

void* x_alloc(size_t n);
void* x_realloc(void* p, size_t n);
char* x_strdup(const char* s);
char* x_strndup(const char* s, size_t n);

// 全局分配计数器，供 `x run --stats` 检查是否有明显泄漏
extern long g_alloc_count;
extern long g_free_count;

// 一个极简的 bump arena：编译期的 AST 与字符串全放进去，
// 编译结束一次性释放。不追求精细，只求「编译期不用管释放」。
typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t used;
    size_t cap;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock* head;
} Arena;

void  arena_init(Arena* a);
void* arena_alloc(Arena* a, size_t n);
char* arena_strdup(Arena* a, const char* s);
char* arena_strndup(Arena* a, const char* s, size_t n);
void  arena_free(Arena* a);

// ============================================================
// 2. 诊断
// ============================================================
// 06-异常与诊断.md 的核心目标：任何失败都要给出「位置 / 原因 / 建议」。
// 编译期诊断走这套；运行期未捕获异常走 interp 里的 rt_throw。

typedef struct {
    const char* file;
    int line;
    int col;
} SrcPos;

// 打印一条诊断。
// 格式遵循 06 文档 §3 的六段式，P0 实现前四段：
//   位置 / 原因 / 建议（help）/ 备注（note）
// 剩余两段（调用栈、文档链接）在 P1/P2 补。
//
// 用法：diag_error / diag_warn 之后紧跟 diag_help / diag_note 给补充说明。
// 为什么不用可变参数一次写完：实际诊断里「建议」常常是有条件出现的
// （比如只有参数个数不匹配时才给「检查函数签名」的建议），
// 拆成独立函数让调用点可以自由组合，也便于 grep help 覆盖率。
void diag_error(SrcPos pos, const char* fmt, ...);
void diag_warn(SrcPos pos, const char* fmt, ...);

// 追加一段「怎么办」。缩进后输出，与 06 §3 的格式一致。
void diag_help(const char* fmt, ...);
// 追加一段「补充说明」，用于解释为什么会这样。
void diag_note(const char* fmt, ...);

// 只打印位置行（带 OSC 8 可点击链接）。运行时错误也走这个，
// 保证编译期与运行期的位置格式一致。
void print_src_pos(FILE* out, SrcPos pos);

// 打印一个诊断标签（"error:" / "note:" / "help:"）并补齐到内容列。
// 为什么需要：标签里混着中英文，`位置` 是 4 显示列、`error:` 是 6 显示列，
// 靠写死空格数对齐必然错位。运行时诊断（interp.c）也用它。
void diag_label(FILE* out, const char* label);

extern int g_error_count;
extern int g_help_count;   // 带 help 段的诊断数，供诊断覆盖率检查用

// ── 诊断静音通道（v0.17）─────────────────────────────────
//
// 用途只有一个：字符串插值的「试探解析」。`"{}"` / `"{\"a\":1}"` 到底
// 是插值还是字面量，唯一可靠的判据是「能不能真解析成一个完整表达式」——
// 而那要求先跑一遍 parser，失败了再回退。回退时那段诊断必须**不输出**，
// 否则 `"{\"a\":1}"` 这种合法 JSON 会附赠一条「期望一个表达式」的报错，
// 用户看到的是「我的 JSON 字符串怎么错了」。
//
// 语义：
//   g_diag_silent > 0    → diag_error / diag_warn / diag_help / diag_note
//                          不输出，且 diag_error 只累加 g_silent_errors，
//                          不增加 g_error_count（试探失败不算真错误）。
//   调用方负责保存/恢复它，不要嵌套滥用。
//
// 为什么不用「把 stderr 重定向到 /dev/null」：那会连带吞掉真正的
// 错误，而且跨平台各自有坑（Windows 下 freopen 语义不同）。
extern int g_diag_silent;
extern int g_silent_errors;

// ============================================================
// 2.5 UTF-8
// ============================================================
// X 的源文件、标识符、诊断文案全链路是 UTF-8（01 §2.1）。源码本身
// 不依赖 locale —— 中文标识符能编译，靠的是词法器按字节 >= 0x80 认
// 标识符字符，而不是 setlocale。
//
// 唯一需要特殊处理的是**与外部环境的交界处**：
//   · 输出到 Windows 控制台：默认代码页是 GBK(936)，UTF-8 字节会被
//     按 GBK 解释成乱码（「位置」→「浣嶇疆」）。必须在启动时把控制台
//     代码页切到 UTF-8(65001) 并开 VT 处理。
//   · 从 argv 读路径：Windows 的 argv 是 ANSI 代码页编码的，中文路径
//     会拿到 GBK 字节。需要用 GetCommandLineW + WideCharToMultiByte 重建。
//   · 读文件：可能带 BOM，01 §2.1 承诺剥离。
//   · 列号：必须按**字符**（码点）计，不是按字节——否则「令 年龄 = 姓名
//     + 不存在」里的错误会被报成第 23 列而不是第 13 列。

// 初始化终端与编码环境。在 main 最开头调用，幂等。
//
// 实现在 platform.c（平台边界集中处），这里保留旧名做兼容转发——
// 调用点很多，改名不会带来任何收益，只会增加 diff 噪音。
// 新代码应该直接调 plat_init_console()。
void utf8_init_console(void);

// 本地编码 ↔ UTF-8。实现同样在 platform.c。
//
// 为什么这一对必须有：程序内部统一用 UTF-8（一致性、可直接进诊断和
// .xbc 文件名字段），但 Windows 的 argv 与 fopen 接受的是 ANSI 代码页。
// 把 UTF-8 字节直接喂给 fopen，在中文路径上必然 ENOENT —— 而错误信息
// 里打印出来的路径看上去完全正确，这个 bug 极难自证。
// 返回 NULL 时调用方应回退到原指针（ASCII 下两者本来就相同）。
char* utf8_from_locale(const char* s);
char* utf8_to_locale(const char* s);

// 剥离 UTF-8 BOM（EF BB BF）。返回新的起始指针，并相应减少 *len。
// 无 BOM 则原样返回。调用方不需要释放返回值。
const char* utf8_strip_bom(const char* s, size_t* len);

// 解码一个 UTF-8 码点。*adv 返回消耗的字节数（1~4）。
// 非法字节序列返回 -1 且 *adv = 1，让调用方逐字节跳过而不是死循环。
// 用它来判宽、算列、按字符截断。
int utf8_decode(const char* s, size_t len, int* adv);
// 码点个数（不是字节数）。len(String) 用它。
size_t utf8_cplen(const char* s, size_t len);

// 校验整个缓冲区是否为合法 UTF-8。
// 合法返回 true；非法则把第一个非法字节的**字节偏移**写入 *bad_off，
// 并按行优先顺序推算出行列（行/列都从 1 起，列按字符计）写入 *bad_line / *bad_col。
//
// 为什么需要它：01 §2.1 规定源文件必须是 UTF-8。但一个 GBK 编码的文件里
// 「令」的字节是 c1 ee，「姓名」是 d0 d5 c3 fb —— 这些字节**恰好**大多是
// 合法的 UTF-8 起始/续字节组合，于是词法器不会报错，而是把它们当成
// 一串乱码标识符，最终报出「未定义的变量 ``」+「给未定义的变量 `` 赋值」。
// 用户对着屏幕上是乱码的名字查半天，根本想不到问题是「文件存成了 GBK」。
// 所以在读文件后立刻校验一次，把「编码不对」这个真正的原因直接说出来。
bool utf8_validate(const char* s, size_t len,
                   size_t* bad_off, int* bad_line, int* bad_col);

// 码点的显示宽度（04 §6.1：中日韩宽字符算 2 列，其余 1 列）。
// 控制字符与组合符号算 0 列。
int utf8_width(int cp);

// 从字符串头部取「前 n 个显示列」的字节数，且不切断多字节序列。
// 用于把过长的源码行按显示宽度截断而不是按字节截断。
size_t utf8_clip_cols(const char* s, size_t len, int max_cols);

// ============================================================
// 3. 词法
// ============================================================

typedef enum {
    // 字面量与标识符
    TK_EOF = 0,
    TK_INT,          // 42
    TK_FLOAT,        // 3.14
    TK_STRING,       // "abc"
    TK_IDENT,        // foo
    TK_CHAR,

    // 关键字（顺序与 01 文档 §2.4 一致，共 45 个）
    TK_AS, TK_BREAK, TK_CASE, TK_CATCH, TK_COMPTIME, TK_CONST,
    TK_CONTINUE, TK_DEFER, TK_DO, TK_DYN, TK_ELSE, TK_ENUM,
    TK_EXTERN, TK_FALSE, TK_FN, TK_FOR, TK_FROM, TK_IF,
    TK_IMPL, TK_IMPORT, TK_IN, TK_LET, TK_LOOP, TK_MATCH,
    TK_MOD, TK_MUT, TK_NOT, TK_NULL, TK_OR, TK_PAR,
    TK_PUB, TK_REF, TK_RETURN, TK_SELF, TK_SPAWN, TK_STRUCT,
    TK_TRAIT, TK_TRUE, TK_TYPE, TK_UNSAFE, TK_VAR, TK_VOID,
    TK_WHERE, TK_WHILE, TK_YIELD,

    // ---- 类系统（v0.15）----
    //
    // ── 为什么 class 与 struct 是两个关键字，而不是一个 ----
    //
    // 它们的**语义根本不同**，不是同一件东西的两种写法：
    //
    //   struct  值语义 —— 赋值即拷贝，字段不可变，无继承
    //   class   引用语义 —— 赋值共享同一对象，字段可变，可继承可多态
    //
    // 用一个关键字加修饰符（比如 `struct ref`）会让读者每次都要
    // 确认「这个到底是值还是引用」，而这是最容易看错的一件事。
    // 两个关键字，读一眼就知道。
    //
    // 这也符合语言设计的通行做法（C# 值类型/引用类型就是 struct/class）。
    TK_CLASS,       // class
    TK_EXTENDS,     // extends
    TK_IMPLEMENTS,  // implements
    TK_ABSTRACT,    // abstract
    TK_INTERFACE,   // interface
    TK_OVERRIDE,    // override
    TK_PRIVATE,     // private
    TK_PROTECTED,   // protected
    TK_NEW,         // new
    TK_SUPER,       // super
    TK_FINAL,       // final（禁止被继承 / 禁止被覆写）

    // 运算符与分隔符
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_PLUSPLUS, TK_MINUSMINUS,
    TK_PLUS_EQ, TK_MINUS_EQ, TK_STAR_EQ, TK_SLASH_EQ, TK_PERCENT_EQ,
    TK_ASSIGN,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ANDAND, TK_OROR, TK_NOT_NOT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_AMP_EQ, TK_PIPE_EQ, TK_CARET_EQ, TK_SHL_EQ, TK_SHR_EQ,
    TK_ARROW,        // ->
    TK_FATARROW,     // =>
    TK_QUESTION,     // ?
    TK_QQ,           // ??  空值合并
    TK_DOT, TK_DOTDOT, TK_DOTDOT_EQ, TK_COMMA, TK_COLON, TK_SEMI,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_AT,           // @
    TK_HASH,         // #
    TK_BANG,         // !  （注意：与 != 区分；01 里 `x!` 表示强制解包）

    TK_COUNT
} TokKind;

typedef struct {
    TokKind kind;
    SrcPos  pos;
    // TK_INT / TK_FLOAT 的数值；TK_STRING / TK_IDENT 的文本
    union {
        int64_t  i;
        double   f;
        char*    s;
    };
    size_t len;      // 字符串/标识符长度
    // 原始源文本（不含两侧引号，转义原样保留）。只有 TK_STRING 有。
    //
    // 为什么不能只用已经 unescape 的 `s`：字符串插值要区分「这是插值的
    // `{`」和「这是 `\u{48}` 里的 `{`」，而 unescape 之后两者都是裸 `{`，
    // 信息已经丢了。所以扫描时另存一份原文，插值拆分看原文。
    char*  raw;
    size_t raw_len;
} Token;

const char* tok_name(TokKind k);

typedef struct {
    const char* src;     // 源码全文（不拥有）
    size_t      len;
    size_t      pos;
    int         line;
    int         col;
    const char* file;
    Arena*      arena;   // 字符串 token 的内存来源
} Lexer;

void  lexer_init(Lexer* lx, const char* file, const char* src, size_t len, Arena* a);
Token lexer_next(Lexer* lx);

// 字符串转义的唯一实现，供 lexer 与字符串插值的 parser 侧共用。
char* x_unescape_string(Arena* a, const char* raw, size_t raw_len,
                        size_t* out_len, SrcPos pos);
// 一次性切完，返回以 TK_EOF 结尾的数组（供 parser 用）
Token* lex_all(Lexer* lx, size_t* out_count);

// 中文关键字别名开关（04 文档层 1：纯词法别名，零运行时成本）。
// 默认关闭，由命令行 `--zh` 或源文件首行 `#lang zh` 打开。
void lexer_set_zh_mode(bool on);
bool lexer_zh_mode(void);

// ============================================================
// 4. AST
// ============================================================
// 表达式与语句用两套 tagged union。P0 只覆盖 01 文档 P0 语法子集：
// fn / let / var / if / else / while / 算术 / 调用 / return / 字符串。

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
// match 的分支。定义在 Expr 之后（它自身持有 Expr*），
// 所以 Expr 里只能先存指针。
typedef struct MatchArm MatchArm;

typedef enum {
    EX_INT, EX_FLOAT, EX_STRING, EX_BOOL, EX_NULL,
    EX_IDENT,
    EX_BINARY, EX_UNARY,
    EX_CALL,
    EX_INDEX,
    EX_MEMBER,
    EX_ASSIGN,      // 赋值是个表达式（返回赋的值）
    EX_IF,          // if 是表达式
    EX_TERNARY,     // `cond ? a : b` —— 条件表达式的简写形式
    EX_NULL_COALESCE, // `a ?? b` —— 左侧为 null 时取右侧
    EX_OPT_CHAIN,   // `a?.b` / `a?[i]` / `a?(...)` —— 左侧为 null 时整串短路为 null
    EX_ARRAY_LIT,   // `[a, b, c]` —— 数组字面量
    EX_MATCH,       // `match x { pat => expr, ... }`
    // 内部节点：值已经在栈顶，不要为它发射任何指令。
    //
    // 只由编译器的语法糖降级过程产生，用户写不出来（parser 永不构造）。
    // 存在的理由：可选链 `a?.b` 要先算 a、检查非 null，然后把 a 交给
    // 正常的成员访问逻辑 —— 而那段逻辑的第一步就是「编译对象表达式」。
    // 有了这个节点，那个「编译对象」就退化成空操作，不必为可选链
    // 单独复制一遍成员访问/索引/调用的全部分支。
    //
    // 类型信息怎么带过去：用 ident.name 存宿主的**结构体类型名**
    // （NULL 表示「运行时才知道」）。不能靠栈上值反推 —— 编译期
    // 栈里没有静态类型，而 expr_struct_name() 是在编译期把
    // `a?.b` 解析成字段偏移的先决条件。漏了这条的症状是
    // 「无法确定 .b 的宿主类型」，把可选链判成不可用。
    EX_STACK_REF,
    EX_BLOCK,
    EX_CAST,        // `expr as Type`
    EX_RANGE,       // a..b / a..=b
    EX_STRUCT_LIT,  // `Rect { x: 1, y: 2 }`
    EX_NEW,         // `new Foo(a, b)` —— 类的实例化
} ExprKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
} BinOp;

typedef enum {
    UOP_NEG, UOP_NOT, UOP_BNOT, UOP_POS,
} UnOp;

struct Expr {
    ExprKind kind;
    SrcPos   pos;
    union {
        struct { int64_t v; } i;
        struct { double v; }  f;
        struct { char* s; size_t len; } str;
        struct { bool v; }    b;
        struct { char* name; } ident;

        struct {
            BinOp op;
            Expr* lhs;
            Expr* rhs;
        } binary;

        struct {
            UnOp op;
            Expr* operand;
        } unary;

        struct {
            Expr*  callee;
            Expr** args;
            int    argc;
        } call;

        struct {
            Expr* obj;
            Expr* index;
        } index;

        struct {
            Expr* obj;
            char* name;
        } member;

        struct {
            Expr* target;
            Expr* value;
        } assign;

        struct {
            Expr*  cond;
            Expr*  then_block;
            Expr*  else_expr;   // 可为 NULL
        } ifx;

        struct {
            Stmt** stmts;
            int    count;
            Expr*  tail;        // 块的最后表达式（作为值）
        } block;

        struct {
            Expr* expr;
            char* type_name;
        } cast;

        struct {
            Expr* lo;
            Expr* hi;
            bool  inclusive;
        } range;

        // 结构体字面量 `Rect { x: 1, y: 2 }`。
        //
        // ── 为什么字段用「名字 + 值」平行数组──────────────────
        //
        // 字段在语法里就是「名字: 值」的有序对。用平行的两个数组
        // 能保持书写顺序 —— 而顺序在编译期是有用的：按声明顺序
        // 发射字段槽时，需要拿「用户写了哪些字段」去和声明比对，
        // 报「缺字段」时按声明顺序报比按哈希顺序报可读得多。
        struct {
            char*    type_name;   // 结构体名
            char**   field_names;
            Expr**   field_values;
            int      field_count;
        } slit;

        // `new Foo(a, b)` —— 类实例化。
        //
        // ── 为什么它是表达式而不是调用 ──────────────────────
        //
        // `Foo(a, b)` 形式会与「静态方法/全局函数同名」冲突，而且
        // 光看调用点分不清「拿一个对象」还是「算一个值」。`new`
        // 让「这里在堆上造了个东西」在语法层就可见 —— 读者不需要
        // 去查 Foo 是类还是函数。
        //
        // 参数语义：全部传给该类的构造函数 `constructor`。类的字段
        // 初始化不写在声明里，而是由构造函数显式赋值（`self.x = 0`），
        // 这样「字段初值」只有一处，不会出现「声明里给了初值、构造
        // 函数里又覆盖一遍」的裂脑问题。
        struct {
            char* type_name;
            Expr** args;
            int    argc;
        } newx;

        // `cond ? a : b`
        //
        // ── 为什么它需要独立节点，而不是直接降成 if ──────────────
        //
        // 语义上 `c ? a : b` 与 `if (c) { a } else { b }` 完全等价 ——
        // 降级时也确实复用 EX_IF 那套字节码，不新增任何指令。但
        // **语法位置**不同：三元能直接出现在参数、返回值、数组元素
        // 这些「表达式槽」里，而 `if` 写在这些位置要多四对花括号，
        // 读起来像语句混进了表达式。
        //
        // 保留独立节点而非 parser 直接构造 EX_IF，是为了让**报错位置**
        // 指向 `?`。降级后位置丢失的话，`1 ? 2` 这种少写冒号的错误
        // 会报到整行开头，用户看不出是哪个运算符缺了右半边。
        struct {
            Expr* cond;
            Expr* then_e;    // `?` 之后
            Expr* else_e;    // `:` 之后（右结合，可为嵌套三元）
        } tern;

        // `a ?? b`：左侧是 null 时取 b，否则取 a。
        //
        // 不写成「`??` 等价于 `a != null ? a : b`」的纯语法糖，是为了
        // 保住「左侧只求值一次」。糖化写法会把 a 算两遍 —— 当 a 是
        // `f()` 时那是两次函数调用，副作用重复，而用户看不出来。
        struct {
            Expr* lhs;
            Expr* rhs;
        } coal;

        // `a?.b` / `a?[i]` / `a?(args)`：可选链。
        //
        // 语义与 `a == null ? null : <原本的访问>` 等价，同样要求
        // 左侧只求值一次。这个节点包住「原本的访问表达式」，
        // 访问表达式内部的第一层对象就是被短路检查的那个值。
        struct {
            Expr* access;   // 被短路的访问表达式（EX_MEMBER / EX_INDEX / EX_CALL）
        } optc;

        // `[a, b, c]`
        //
        // 空字面量 `[]` 合法（elems == NULL, count == 0），类型靠
        // 上下文推断；推断不出来时按 Array 处理。
        struct {
            Expr** elems;
            int    count;
        } arrlit;

        // `match x { pat => expr, ... }`
        //
        // 匹配是**表达式**（每个分支产出一个值），所以它出现在
        // EX_ 这一侧而不是 ST_。分支有序，第一个命中的生效 ——
        // 不做「重叠检测」，因为范围模式的交集判定在浮点上没有
        // 可靠答案，而漏判会让编译器给出错误的拒绝。
        struct {
            Expr*     subject;
            MatchArm* arms;
            int       arm_count;
        } matchx;
    };
};

// match 的一个分支。
//
// 模式的三种形态合并进一个结构体而不是再套一层 tagged union：
// 它们共享 `guard` / `binding` / `body`，区别只在 `kind` 和
// `lo`/`hi`/`lit` 这几个字段的用法。再拆一层只会让 parser 里
// 每次都多写一次 switch。
typedef enum {
    PAT_WILD,     // `_`
    PAT_LITERAL,  // `1` / `"a"` / `true` / `null`
    PAT_RANGE,    // `0..9` / `0..=9`
} PatKind;

typedef struct MatchArm {
    PatKind  kind;
    SrcPos   pos;

    // PAT_LITERAL：被匹配的字面量（用 Expr 存，避免再定义一套字面量表示）
    Expr*    lit;
    // PAT_RANGE：上下界（含 packed 的 inclusive 标记用闭区间语义表示）
    Expr*    lo;
    Expr*    hi;
    bool     inclusive;

    // `n @ 1..9`：命中时把被匹配值绑到这个名字上。可为 NULL。
    char*    binding;

    // `if guard`：模式命中后还要满足的附加条件。可为 NULL。
    Expr*    guard;

    Expr*    body;
} MatchArm;

typedef enum {
    ST_LET,
    ST_VAR,
    ST_EXPR,
    ST_RETURN,
    ST_WHILE,
    ST_BREAK,
    ST_CONTINUE,
    ST_IF,        // 语句位置的 if（无值）
    ST_BLOCK,
    ST_DEFER,
    ST_ASSIGN,
} StmtKind;

struct Stmt {
    StmtKind kind;
    SrcPos   pos;
    // 这条语句在源文件 token 数组中的跨度（闭区间）。
    // 顶层 const/let 用它做依赖闭包扫描，见 main.c 的 build_minimal_merge。
    // tok_end < tok_begin 表示没有记录跨度。
    int      tok_begin;
    int      tok_end;
    union {
        struct {
            char* name;
            char* type_name;   // 可 NULL（P0 通常 NULL，靠推断）
            Expr* init;        // 可 NULL
            bool  mutable_;
            // 是否用 `const` 声明。
            //
            // 为什么需要单独一位：const 与 let 在语义上都是「不可重新赋值」，
            // 编译期行为完全一样，所以最初没记。但模块加载要区分
            // 「顶层常量」与「顶层可执行语句」——前者允许出现在被 import
            // 的文件里，后者不允许（会撞 main）。只看 mutable_ 区分不出来。
            bool  is_const;
        } var;

        struct { Expr* expr; } expr;

        struct { Expr* value; } ret;      // 可 NULL

        struct {
            Expr* cond;
            Stmt* body;
        } whilex;

        struct { Stmt* body; } loop_body;

        struct {
            Expr*  cond;
            Stmt*  then_body;
            Stmt*  else_body;   // 可 NULL
        } ifs;

        struct {
            Stmt** stmts;
            int    count;
        } block;

        struct { Stmt* body; } defer;

        struct {
            char* name;
            Expr* value;
        } assign;   // P0 只支持给标识符赋值
    };
};

// 函数声明
typedef struct {
    char*  name;
    char** param_names;
    char** param_types;   // 可 NULL
    int    param_count;
    char*  ret_type;      // 可 NULL
    Stmt*  body;          // 块语句
    SrcPos pos;
    // 声明在源文件 token 数组中的跨度（含签名与函数体），
    // 供 main.c 计算 import 依赖闭包。
    int    tok_begin;
    int    tok_end;
} FuncDecl;

// 结构体字段声明
typedef struct {
    char*  name;
    char*  type_name;   // 可 NULL（不写注解时按初值推断，但字面量必须给全）
} StructField;

// 结构体声明（v0.14 · P0.5）
//
// ── 为什么现在做，而不是留到 P1 ────────────────────────────
//
// 标准库已经有 296 个自由函数，UI 模块 52 个常量 + 30 多个
// `uiButton(x, y, w, h, label)` 这样的函数。这套形态的问题是
// 「矩形」这个概念不存在 —— 它只是四个恰好一起出现的 Int。
// 于是每个函数都要重新摆一遍这四个参数，每次调用都可能把
// w 和 h 传反，而且 w/h 传反时**没有任何一层能发现**：四个参数
// 都是 Int，编译期类型检查全部通过。
//
// 加结构体不是为了「面向对象」，是为了让「矩形」成为一个
// 可以被检查的东西。一旦 `Rect` 存在，`uiButton(r: Rect, label)` 
// 就把「w/h 传反」从运行时静默错误变成编译期类型错误。
//
// ── P0.5 的实现边界 ────────────────────────────────────────
//
// 只有结构体，没有方法、没有继承、没有泛型、没有接口。
// 方法留到下一步（见 StructDecl 之后的说明）；继承这类东西
// 需要虚表与对象布局设计，在 P0 阶段加进来只会锁死未来的选择。
//
// 字段不可变（P0.5）：`r.x = 1` 报错。理由见 compile.c 里
// EX_ASSIGN 分支的注释 —— 可变字段需要解决「值语义拷贝之后
// 改的是哪一份」这个语义问题，而那需要先有引用类型（`&T`）。
typedef struct StructDecl {
    char*         name;
    StructField*  fields;
    int           field_count;
    SrcPos        pos;
    int           tok_begin;
    int           tok_end;
} StructDecl;

// ============================================================
// 类（v0.15）
// ============================================================
//
// ── struct 与 class 到底差在哪 ──────────────────────────────
//
// 这不是「同一个东西的两种写法」，是两套语义：
//
//              struct                    class
//   赋值       拷贝一份值                共享同一个对象
//   字段       不可变                    可变
//   继承       没有                      有（单继承）
//   多态       没有                      有（虚方法表）
//   相等       结构相等                  默认引用相等（== 比的是同一个对象）
//
// 所以它们是两个关键字、两张类型表、两套运行时表示。共用一套只会
// 让两边都变形。
//
// ── 为什么字段可变这件事在 class 上成立，而 struct 上不行 ────
//
// struct 是值语义：`let b = a` 之后改 b.x 会（或不会）影响 a，
// 这是个必须二选一牺牲某种语义的死结 —— 共享则值语义失效，
// 深拷贝则代价随嵌套深度爆炸。
//
// class 是引用语义：`let b = a` 就是「b 和 a 指向同一个对象」，
// 改 b.x 就是改 a.x。这不需要解释，任何有对象的语言都这样。
// 死结之所以是死结，是因为值语义；换成引用语义之后它根本不存在。
//
// ── 一句话 ──────────────────────────────────────────────────
//
// struct 回答「怎么把几个值捆在一起」；class 回答「一组对象如何
// 共享行为、又各自持有状态」。前者是数据结构，后者是类型抽象。

// 成员可见性
//
// 与 C++ / Java 同一套语义：
//   public    谁都能访问
//   protected 本类 + 子类
//   private   只有本类
typedef enum {
    VIS_PUBLIC = 0,
    VIS_PROTECTED,
    VIS_PRIVATE,
} Visibility;

// 注解
//
// ── 设计取向：只存，不解释 ──────────────────────────────────
//
// 注解分两半：**语法与元数据**属于语言，**语义**属于使用者。
// 语言这一层只做三件事：
//
//   1. 解析 `@Name` / `@Name(a, b, "c")`，挂在它前面的声明上
//   2. 把注解名与实参原样存进元数据表（并写进 .xbc）
//   3. 运行时提供 `classHasAnnotation` / `classAnnotation` 查询
//
// 至于 `@Deprecated` 该不该产生警告、`@Inject` 该注入什么 ——
// 那是框架的事，语言不该内置一套「官方注解」然后让所有人被迫
// 接受它的语义。
//
// ── 为什么实参只支持字面量 ────────────────────────────────
//
// 注解出现在声明上，而声明在**编译期**就要变成元数据（还要能
// 单独写进 .xbc 发给没有源码的机器）。如果要支持 `@Size(n * 2)`
// 这种表达式，就得在编译期求值 —— 那是 comptime 能力，P1 才有。
//
// 所以实参限定为字面量：Int / Float / String / Bool / null。
// 够覆盖绝大多数真实用法（路由路径、注入名、配置键、限值），
// 而表达力不足时会有一条明确的编译错误告诉你「这里只收字面量」，
// 而不是悄悄存一个求值不了的 AST。
typedef struct {
    char*   name;        // 不含 `@`，例如 "Deprecated"
    // 实参，按源码顺序。每个元素是已求值的字面量。
    char**  arg_keys;    // 可 NULL；支持 `@Foo(key = "v")` 具名实参
    char**  arg_vals;    // 一律存字符串形式（"3" / "true" / "abc"）
    int     arg_count;
    SrcPos  pos;
} Annotation;

// 一组注解。空组用 count == 0 表示（指针可 NULL）。
typedef struct {
    Annotation* items;
    int         count;
} AnnotationSet;

// 注解的挂载目标。
typedef enum {
    ANNO_TARGET_CLASS  = 0,
    ANNO_TARGET_FIELD  = 1,
    ANNO_TARGET_METHOD = 2
} AnnoTarget;

// 一条注解记录 —— 扁平表的一项。
//
// ── 为什么扁平存而不是嵌进 ClassType / 字段 / 方法 ──────────
//
// 三种嵌套存法都要在 .xbc 读写里各写一套（类一套、字段一套、
// 方法一套），三份几乎相同的序列化代码，而它们最容易出的错是
// 「加字段时漏改其中一份」—— 症状是某一类目标的注解在 .xbc
// 往返后消失，而另外两类正常，测试如果只覆盖类注解就发现不了。
//
// 扁平成一张表只有一套读写代码，查询是「按目标过滤」的一次线性
// 扫描。注解的数量级是「几十条」，扫描成本可以忽略。
typedef struct {
    AnnoTarget target;
    int        class_idx;    // 目标所属的类
    char*      member;       // 字段名 / 方法名；类注解为 NULL
    char*      name;         // 注解名，不含 `@`
    char**     arg_keys;     // 可 NULL（全是位置实参时）
    char**     arg_vals;
    int        arg_count;
} AnnoEntry;

// 类字段声明
typedef struct {
    char*      name;
    char*      type_name;
    Visibility vis;
    // `final` 字段：只允许在构造函数里赋值。用它可以表达「不可变
    // 但初始化时机晚于声明」的那类字段（比如依赖构造参数的派生值）。
    bool       is_final;
    SrcPos     pos;
    AnnotationSet annos;   // 字段上的注解
} ClassField;

// 类方法声明
//
// 方法体就是一个普通函数体。名字在编译期会被改写成
// `Class.method`（与结构体方法同一套名字改写规则），所以
// 方法表本质上就是函数表的一个视图。
//
// ── 哪些方法进虚表 ──────────────────────────────────────────
//
// **只有实例方法进虚表**。静态方法（没有 self）不进 —— 静态方法
// 在编译期就能确定调哪个，没有任何运行时派发的必要，塞进虚表
// 只会让每次调用多一次间接寻址。
//
// 构造函数也不进虚表：`new Foo()` 里 Foo 是写死的名字，不存在
// 「运行时才知道要构造哪个类」的情况。
typedef struct {
    char*  name;
    bool   is_static;      // 没有 self
    bool   is_abstract;    // 只有签名没有函数体（abstract 或接口方法）
    bool   is_override;    // 显式标注 override
    bool   is_final;       // 禁止子类覆写
    Visibility vis;
    FuncDecl* fn;          // 函数体（抽象方法为 NULL）
    SrcPos pos;
    AnnotationSet annos;   // 方法上的注解
} ClassMethod;

// 类声明
typedef struct {
    char*          name;
    char*          parent_name;    // 可 NULL
    char**         iface_names;    // implements 列表，可 NULL
    int            iface_count;
    bool           is_abstract;    // 不可实例化
    bool           is_final;       // 不可被继承
    bool           is_interface;   // interface：所有方法都抽象，不可有字段

    ClassField*    fields;
    int            field_count;
    ClassMethod*   methods;
    int            method_count;

    SrcPos         pos;
    AnnotationSet  annos;          // 类上的注解
    int            tok_begin;      // 整个类声明（含方法体）的 token 跨度
    int            tok_end;
} ClassDecl;

typedef struct {
    FuncDecl** funcs;
    int        func_count;
    Stmt**     top_stmts;   // 顶层语句（合成 main 用）
    int        top_count;
    StructDecl** structs;   // 结构体声明表（v0.14）
    int        struct_count;
    ClassDecl** classes;    // 类声明表（v0.15）
    int        class_count;
    const char* file;       // 源文件路径，供「整个文件的错误」定位用

    // 这个文件 import 了哪些文件（P0 是文本级合并，见 parser.c 的 parse_import）。
    // 路径已经补全前缀：main.c 会据此去读文件。
    char**     imports;
    // 与 imports 一一对应的行号数组（1 起）。
    //
    // 存在的理由只有一个：诊断要指到**那句 import**上。包解析失败时
    // （`import @x` 没在清单里声明）如果只报清单的行，用户会以为要改
    // 清单；而实际常见情况是「这个包不该依赖，删掉这句 import」。
    // 一个行号数组换掉一整类误导性诊断。
    int*       import_lines;
    int        import_count;

    // 与 imports / import_lines 平行的具名导入列表。
    // import_names[i][j] 是第 i 条 import 声明的第 j 个名字，
    // import_name_counts[i] 是它的名字个数。裸 `import` 报错后
    // 名字个数为 0，不参与合并。
    char***    import_names;
    int*       import_name_counts;

    // 与 imports 平行的解析后绝对路径。mod_load 解析完依赖后回填，
    // 供 build_minimal_merge 计算闭包时直接使用 —— @包 / 标准库 /
    // 相对路径三种解析规则不同，二次解析容易漂移。
    char**     import_targets;
} Module;

// ============================================================
// 5. 字节码
// ============================================================
// P0 用栈式字节码 + switch 分派。
// 05 文档 §2 明确：先做最笨的版本把语义跑对，P1 再换寄存器式+线程化。

// 指令集设计（v0.7 · P0）
//
// 核心原则来自 07-高性能解释器.md §3.2：**字节码带类型**。
// 静态类型已知时发射 `*_I64` / `*_F64` 这样的特化指令，解释器里
// 直接走 native 整数/浮点运算，零类型检查、零装箱。
// 只有在编译期无法确定类型时，才退化到 `*_NUM`（数值双分派）
// 或 `*_DYN`（全动态），并承担一次运行时判断的代价。
//
// 三档特化的命名约定：
//   _I64  只接受 Int，不做任何检查（编译期已保证）
//   _F64  只接受 Float
//   _NUM  接受 Int 或 Float，运行时提升到 Float 计算
//   _DYN  接受任何值，按标签分派（仅 `+` 需要，因为字符串拼接）
//   （无后缀）如 BC_EQ / BC_NE 对所有同类型值通用

// 整数二元运算码 —— BC_ARITH_STORE / BC_ARITH_CONST_STORE 的操作数。
//
// 独立成一个枚举而不是复用 OpCode：这样「运算」和「指令」是两个
// 维度，一条指令能承载十种运算，而 switch 只多两个 case。
//
// **顺序有意义**：编译器与解释器都按这里的编号查表或分派。新增
// 运算只能追加在末尾，不能插在中间 —— 已编译的 .xbc 里存的是
// 数字编号，插队会把它指向另一种运算（静默算错，不报错）。
typedef enum {
    XOP_ADD = 0,   // `+`  带溢出检查
    XOP_SUB,       // `-`  带溢出检查
    XOP_MUL,       // `*`  带溢出检查
    XOP_DIV,       // `/`  除零检查 + INT64_MIN/-1 溢出检查
    XOP_MOD,       // `%`  除零检查（INT64_MIN%-1 定为 0）
    XOP_BAND,      // `&`
    XOP_BOR,       // `|`
    XOP_BXOR,      // `^`
    XOP_SHL,       // `<<` 越界置 0（01 §4.3），不做回绕
    XOP_SHR,       // `>>` 算术右移，越界按符号填
    XOP_COUNT
} XArithOp;

typedef enum {
    // 栈操作
    BC_NOP = 0,
    BC_PUSH_I64,        // a = 常量索引（CONST_I64）
    BC_PUSH_F64,        // a = 常量索引（CONST_F64）
    BC_PUSH_STR,        // a = 常量索引（CONST_STR）
    BC_PUSH_TRUE,
    BC_PUSH_FALSE,
    BC_PUSH_NULL,
    BC_POP,
    BC_DUP,

    // 局部变量（a = 槽位号）
    BC_LOAD_LOCAL,
    BC_STORE_LOCAL,

    // 算术 · 特化
    BC_ADD_I64, BC_SUB_I64, BC_MUL_I64, BC_DIV_I64, BC_MOD_I64, BC_NEG_I64,
    BC_ADD_F64, BC_SUB_F64, BC_MUL_F64, BC_DIV_F64, BC_NEG_F64,

    // 算术 · 数值双分派（Int 与 Float 混算）
    BC_ADD_NUM, BC_SUB_NUM, BC_MUL_NUM, BC_DIV_NUM,

    // 算术 · 全动态（`+` 在类型未知时可能是拼接）
    BC_ADD_DYN,

    // 比较 · 通用（同类型即可比，含字符串字典序）
    BC_EQ, BC_NE,

    // 比较 · 特化
    BC_LT_I64, BC_LE_I64, BC_GT_I64, BC_GE_I64,
    BC_LT_F64, BC_LE_F64, BC_GT_F64, BC_GE_F64,

    // 比较 · 数值双分派
    BC_LT_NUM, BC_LE_NUM, BC_GT_NUM, BC_GE_NUM,

    // 位运算（仅 Int）
    BC_BAND_I64, BC_BOR_I64, BC_BXOR_I64, BC_SHL_I64, BC_SHR_I64, BC_BNOT_I64,

    // 逻辑
    BC_NOT,

    // 类型转换
    BC_TOBOOL,          // 任意值 → Bool（按真值规则）
    BC_I64_TO_F64,      // Int → Float，不丢精度
    BC_TOINT,           // 任意数值/字符串 → Int
    BC_TOFLOAT,         // 任意数值/字符串 → Float

    // 转字符串
    BC_TOSTRING_I64, BC_TOSTRING_F64, BC_TOSTRING_BOOL,
    BC_TOSTRING_NULL, BC_TOSTRING_ANY,

    // 字符串
    BC_CONCAT_STR,      // 两边都是 String，直接接
    BC_CONCAT_ANY,      // 至少一边非 String，先各自转字符串再接

    // 内建
    BC_ABS_I64, BC_ABS_F64, BC_LEN,

    // ---------------- 数组（v0.12）----------------
    //
    // 三条指令，对应三件必须能做的事：建、读一个元素、写一个元素。
    //
    // 为什么没有「数组长度」指令：长度查询复用 BC_LEN。BC_LEN 原本
    // 只处理字符串，扩成「String 或 Array 都返回长度」比新增一条
    // 指令更好 —— 两条指令做同一件事（取长度）会让反汇编和 AOT 都
    // 多一个分支，而语言层 `len(a)` 本来就该对两者一视同仁。
    BC_NEW_ARRAY,       // a = 元素个数（从栈上弹出），b = 填充值（从栈上弹出）
    BC_INDEX_GET,       // 弹容器、弹下标，压元素
    BC_INDEX_SET,       // 弹值、弹下标、弹容器；容器留在栈上（赋值表达式的值）

    // ---------------- 平台内建（v0.13）----------------
    //
    // 一条指令打天下：a = 内建编号（XBI_*），b = 参数个数。
    //
    // ── 为什么不是每个窗口函数一条 opcode ──────────────────
    //
    // 窗口能力有十几个函数（开窗、画矩形、画文字、轮询点击……）。
    // 每条一个 opcode 会让 OpCode 枚举迅速膨胀，而且每加一个函数就要
    // 改 5 个地方：枚举、名称表、栈效应表、解释器、AOT 生成器。
    //
    // 用一条统一的 BC_BUILTIN，新增函数只需要在 XBI_ 枚举里加一项
    // 并在两处 dispatch 里各加一个 case。更关键的是：**OpCode 是字节码
    // 格式的一部分**，动它就是动文件格式；而 XBI_ 只是运行时的编号，
    // 增删完全不影响已编译的 .xbc 兼容性。
    BC_BUILTIN,         // a = XBI_*，b = argc

    // 控制流（a = 绝对指令索引）
    BC_JUMP,
    BC_JUMP_IF_FALSE,   // 弹出栈顶并按真值判断
    BC_JUMP_IF_TRUE,

    // 调用（a = 函数索引）
    BC_CALL,
    BC_RETURN,          // 弹出栈顶作为返回值
    BC_RETURN_VOID,     // 返回 null

    // 输出与停机
    BC_PRINT,           // 弹出任意值并打印（不带换行）
    BC_HALT,

    // ---------------- 超指令（v0.10，07 §3.3）----------------
    //
    // 为什么需要：实测一段 1000 万次的循环要执行 1.2 亿条字节码，
    // 每条平均 2.07 ns（约 6 个周期）。单条指令已经很难更快了——
    // 真正的浪费是**指令数太多**：一次「sum = sum + i」在 C 里是
    // 一条乘加，在栈式字节码里要拆成 LOAD/LOAD/ADD/DUP/STORE/POP。
    //
    // 超指令把高频相邻对合并成一条，省掉一次分派 + 一次栈往返。
    //
    // 选哪几对**不是猜的**：用 --profile=pairs 跑真实程序统计出来
    // （见 tools/profile_pairs.sh）。当前前三名是：
    //   LOAD_LOCAL + LOAD_LOCAL   10000 次/万次循环
    //   DUP        + STORE_LOCAL  20000 次
    //   STORE_LOCAL+ POP          20000 次
    // 这三对覆盖了循环体 12 条指令里的 6 条。
    //
    // 合并由 compile.c 在字节码生成完成后跑一遍 peephole pass 完成，
    // 不侵入语法分析与类型推断——超指令是纯字节码层的局部改写。
    BC_LOAD_LOAD,       // LOAD_LOCAL a; LOAD_LOCAL b  → 压两个局部变量
    BC_DUP_STORE,       // DUP; STORE_LOCAL a          → 复制栈顶并存一份到 a
    BC_STORE_POP,       // STORE_LOCAL a; POP          → 存到 a 并弹掉

    // 第二轮：循环控制流。实测里 while 条件每次都走这四步，
    // 合并后循环头从 4 条指令压到 2 条。
    BC_LOAD_CONST,      // LOAD_LOCAL a; PUSH_I64 c    → 局部变量与常量的比较准备
    BC_LT_JUMP_FALSE,   // LT_I64; JUMP_IF_FALSE t     → 比较并条件跳（循环头最高频形态）

    // 第三轮：由 X_PROFILE 实测驱动（见 tools/bench.sh --profile）。
    //
    // 这一轮的核心发现：**前一轮的 DUP_STORE 反而暴露了更好的机会**。
    // 原本 `sum = sum + i` 编译成 DUP; STORE; POP 三条，合并成
    // DUP_STORE; POP 两条；而这两条的语义合起来恰好等价于一条
    // STORE_LOCAL（DUP 复制栈顶，STORE 存一份，POP 弹掉 —— 净效果
    // 就是「存一份到槽位并弹掉」）。于是再做一次二合一，3 → 1。
    //
    // 实测：循环体里 `DUP_STORE; POP` 出现 6000 万次，是绝对第一名。
    //
    // 最后一对 ADD_STORE / MOD_STORE 是「三合一的第二段」：
    //   ADD; DUP_STORE; POP → ADD; DUP_STORE_POP → ADD_STORE
    // 单看第一条规则配不上（ADD 的下一条是 DUP_STORE 而不是
    // DUP_STORE_POP），所以合并 pass 必须**反复跑直到不再变化**，
    // 让新生成的超指令成为下一轮配对的输入。这样规则之间不必
    // 手工排优先级，收敛结果自然是最优的。
    BC_DUP_STORE_POP,   // DUP_STORE a; POP            → 等价于 STORE_LOCAL a
    BC_ADD_STORE,       // ADD_I64; DUP_STORE_POP a    → 加完存回 a（`x += i` 的终态）
    BC_MOD_STORE,       // MOD_I64; DUP_STORE_POP a    → 取模后存回 a

    // ---------------- 结构体（v0.14）----------------
    //
    // ── 为什么只有两条，且没有「字段写」──────────────────
    //
    // P0.5 的结构体字段**不可变**（`r.x = 1` 是编译错误，
    // 见 compile.c 的 EX_ASSIGN 分支）。省掉 FIELD_SET 不是偷懒：
    //
    // 字段可变 + 值语义是一对会互相打架的东西。`let b = a` 之后
    // 两份结构体要么共享同一个对象（那 `b.x = 1` 会改到 a，
    // 值语义当场失效），要么必须深拷贝（那每次赋值都要递归复制
    // 整个对象图，代价随类型嵌套深度指数增长，而且和引用计数的
    // 「共享才便宜」正好相反）。
    //
    // 真正的解法是引用类型（`&T`）—— 有了它，「我要共享并一起改」
    // 和「我要各自一份」才能分开表达。在那之前，加可变字段只能
    // 二选一牺牲某种语义。而 UI 组件的实际用法（Rect 构造完就传
    // 出去只读，状态留在调用方）恰好一个可变字段都不需要。
    //
    // 所以这里只发射「建」和「读」。
    BC_NEW_STRUCT,      // a = 结构体类型索引，b = 字段个数。
                        //   弹 b 个字段值（按声明顺序，最后压的在最顶上），压对象
    BC_FIELD_GET,       // a = 结构体类型索引，b = 字段下标。
                        //   弹对象，压字段值

    // ---------------- 类（v0.15）----------------
    //
    // ── 三件事必须由字节码承担 ──────────────────────────────
    //
    // 1. 实例化：要在堆上分配、要把字段初始化、要调构造函数。
    // 2. 字段写：class 的字段可变，而 struct 的不行 —— 差别就在这里。
    // 3. 虚方法派发：调用点写的是 `obj.area()`，但 `obj` 的实际类型
    //    只有运行时才知道。这是多态的全部含义，绕不过去。
    //
    // ── 为什么字段写不沿用 BC_FIELD_GET 加个标志位 ──────────
    //
    // 因为语义完全不同：GET 是「弹对象、压值」（净栈效应 0），
    // SET 是「弹值、弹对象、压回值」（净效应 -1，为了让赋值表达式
    // `a.x = b.y = 1` 有值）。栈效应不同、操作数含义不同、合法性
    // 检查不同（SET 还要判 final）。合在一起只会写出「一个 case
    // 里半段 GET 半段 SET」的代码。
    BC_NEW_CLASS,       // a = 类索引，b = 实参个数
                        //   弹 b 个构造实参，压新实例
    BC_FIELD_SET,       // a = 字段下标（**含父类字段的全局偏移**），
                        //   弹值、弹对象，压回该值
    BC_VIRT_CALL,       // a = 虚表槽号，b = 实参个数（含 self）
                        //   栈上从下到上：self, arg1..argN
                        //   按 self 的实际类查虚表取函数索引再调

    // ---------------- 动态调用（v0.20，框架支持）----------------
    //
    // ── 为什么有了 BC_NEW_CLASS / BC_VIRT_CALL 还要这两条 ─────
    //
    // 编译期的 new 与方法调用都要求「类名 / 方法名写在源码里」。
    // 注解驱动的框架做不到这一点：IoC 容器扫描 @Component 时拿到
    // 的是**运行时字符串**，路由分派拿到的是注解里声明的**方法名
    // 字符串**。这两条指令把「名字」从编译期常量变成栈上的运行时
    // 值，是反射式调用的字节码出口。
    //
    // 为什么做成指令而不是 run_builtin 里的普通内建：两者都要
    // push frame（构造函数、方法体都要执行），而内建的分发约定是
    // 「弹参 → 返回值压栈」，无法交还控制流。放主循环里与
    // BC_NEW_CLASS / BC_VIRT_CALL 共用同一套推帧逻辑，语义一致。
    BC_NEW_CLASS_NAME,  // b = 0|1（栈顶有没有实参数组）
                        //   栈上从下到上：类名String [, 实参数组Array]
                        //   按名字查类表 → 数组按元素展开成构造实参
                        //   → 分配 → 调构造函数
    BC_CALL_NAME,       // b = 0|1（栈顶有没有实参数组）
                        //   栈上从下到上：接收者, 方法名String
                        //   [, 实参数组Array]
                        //   按名字查虚表槽 → 数组按元素展开成方法实参
                        //   → 按实际类派发（与 BC_VIRT_CALL 同一条派发
                        //   路径，多态语义不变）
                        //
                        //   ── 为什么实参收「一个数组」而不是散列参数 ──
                        //   反射调用点的实参列表几乎总是运行时数据
                        //   （路由分派把请求参数绑定成数组、容器按
                        //   bean 定义传依赖）。散列写法在编译期就把
                        //   个数定死了；收数组则个数天然是运行时的。

    // ── 第二批超指令（v0.18，07 §3.3 的第二轮挖掘）──────────
    //
    // 上一批 6 条把 `for i in a..b` 的循环体压到 7 条。实测每条
    // 字节码的成本约 4~5 ns，且与指令种类基本无关 —— 也就是说
    // 耗时几乎正比于分派次数。所以这里继续针对同一段循环体里
    // 剩下的三对形态合并：
    //
    //   LOAD_CONST + LT_JUMP_FALSE   → 局部量与常量比较、假则跳
    //   LOAD_CONST + ADD_STORE       → 局部量 += 常量（x = x + k）
    //   ADD_CONST_STORE + JUMP       → 上面那条再加回边
    //
    // 三条都落地后，那个循环体从 7 条降到 3 条：
    //   比较跳 → count+=1 → i+=1 并回跳
    //
    // ── 编码约定：跳转偏移一律放在 a ────────────────────────
    //
    // 跳转重映射（compile.c 的 superinstr_merge / codegen_c.c 的
    // 栈深传播）都按「a 是相对本条下一条的偏移」处理。新指令即使
    // 还要别的操作数，也把偏移留在 a、其余顺延到 b/c —— 这样那两处
    // 只需要在 opcode 列表里加一项，不必引入「偏移在第几个字段」
    // 的元数据表。多一张表就多一处能写错的地方。
    BC_LT_CONST_JUMP_FALSE,  // a = 跳转偏移, b = 局部槽位, c = 常量索引
                             //   LOAD_CONST b,c; LT_JUMP_FALSE a
                             //   → 若 stack[base+b] < consts[c] 为假则跳
    BC_ADD_CONST_STORE,      // a = 局部槽位, b = 常量索引
                             //   LOAD_CONST a,b; ADD_STORE a
                             //   → stack[base+a] += consts[b]（带溢出检查）
    BC_ADD_CONST_STORE_JUMP, // a = 跳转偏移, b = 局部槽位, c = 常量索引
                             //   ADD_CONST_STORE b,c; JUMP a

    // ── 第三批：减法家族的对称补齐（v0.21）────────────────────
    //
    // ── 为什么这一批必须补，而且是效率问题不是洁癖 ────────────
    //
    // 前两批超指令只做了 ADD 和 MOD 的形态，SUB 一条都没有。
    // 结果同样是「循环里做一次加减」，两种写法的指令数差一倍：
    //
    //     s = s + 1   →  ADD_CONST_STORE          (1 条)
    //     s = s - 1   →  LOAD_CONST               (4 条)
    //                    SUB_I64
    //                    DUP_STORE_POP
    //
    // 实测 3000 万次循环：ADD 侧 468ms，SUB 侧 820ms —— 慢 75%，
    // 而这个差距**完全来自少了两条合并规则**，与运算本身无关。
    //
    // 这不是「优化没做完」，而是一个很容易被误判成「解释器就是慢」
    // 的性能陷阱：用户的计数循环习惯写 `n = n - 1`，恰好全落在
    // 没有合并的那一侧。补齐后两侧指令数与耗时才对得上。
    //
    // 编码约定与 ADD 组逐字相同（偏移放 a），所以下面那几个
    // 「按 opcode 列表加一项」的地方不用引入新逻辑。
    BC_SUB_STORE,            // a = 局部槽位
                             //   SUB_I64; DUP_STORE_POP a
                             //   → 弹两个整数相减，存回 a
    BC_SUB_CONST_STORE,      // a = 局部槽位, b = 常量索引
                             //   LOAD_CONST a,b; SUB_STORE a
                             //   → stack[base+a] -= consts[b]（带溢出检查）
    BC_SUB_CONST_STORE_JUMP, // a = 跳转偏移, b = 局部槽位, c = 常量索引
                             //   SUB_CONST_STORE b,c; JUMP a

    // ── 第四批：运算族泛化（v0.21）──────────────────────────
    //
    // ── 为什么不再一条条枚举 ────────────────────────────────
    //
    // 前三批是「按实测热点手工加指令」：ADD 组、MOD 组、SUB 组。
    // 加上减法后我数了一下，还剩七个同构运算没有覆盖：
    //
    //     s = s * 2   →  LOAD_CONST; MUL_I64; DUP_STORE_POP   (3 条)
    //     s = s / 2   →  LOAD_CONST; DIV_I64; DUP_STORE_POP   (3 条)
    //     b = b & 7   →  LOAD_CONST; BAND_I64; DUP_STORE_POP  (3 条)
    //     b = b | 1   …  ^ 3  …  << 1  …  >> 1                同上
    //
    // 而 `s = s % n` 因为恰好有 MOD_STORE 只要 2 条 —— 同样是
    // 「一次赋值运算」，指令数却分两档。这个不一致没有任何道理，
    // 纯粹是「上次只挖到了 ADD 和 MOD」。
    //
    // 继续一条条补就是 7 × 3 = 21 条新指令，switch 膨胀一倍。
    // 所以这一批换成**按族泛化**：一条指令带一个运算码（XArithOp），
    // 覆盖全部十种整数二元运算。
    //
    // ── 为什么不用「opcode 偏移」那套（store = arith + K）────
    //
    // 那要求算术指令在枚举里连续排列。但 BC_ADD_I64..BC_MOD_I64
    // 和 BC_BAND_I64..BC_SHR_I64 之间夹着 F64 / NUM / DYN / EQ /
    // 比较指令，打乱重排会改变已有 .xbc 的指令编号 —— 那些文件
    // 立刻无法执行。宁可多一个运算码字段，也不动格式。
    //
    // ── 为什么不需要 CONST_STORE_JUMP 的泛化版 ──────────────
    //
    // 循环末尾那条回边几乎总是 `i = i + 1`，已有专门的
    // BC_ADD_CONST_STORE_JUMP。而 `s = s * 2` 这类出现在循环体
    // 中间，后面接的是循环变量自增，不是它自己的回边 —— 实测
    // 指令对直方图里 `MUL_I64 → DUP_STORE_POP → LOAD_CONST`，
    // 从不出现 `MUL → JUMP`。所以泛化版只需 STORE 和 CONST_STORE
    // 两种形态，第三条是白加的（还要多占一个偏移字段，挤不下）。
    BC_ARITH_STORE,          // a = 局部槽位, b = XArithOp
                             //   <OP>_I64; DUP_STORE_POP a → 运算后存回 a
    BC_ARITH_CONST_STORE,    // a = 局部槽位, b = 常量索引, c = XArithOp
                             //   LOAD_CONST a,b; <OP>_I64; DUP_STORE_POP a
                             //   → stack[base+a] = stack[base+a] OP consts[b]

    // ── 第五批：变量拷贝与比较跳转（v0.22）────────────────────
    //
    // 这一批的选题**不是猜的**，是用 X_PROFILE 跑真实混合负载
    // （标准库 + 应用，60 万轮循环）统计出来的。高频前几名里有
    // 两类形态没有任何超指令覆盖：
    //
    //     LOAD_LOCAL + STORE_LOCAL             315 万次  ← 第一名
    //     LOAD_CONST + NE + JUMP_IF_FALSE      255 万次
    //
    // ── 为什么比较只加了 NE 这一个，不做成运算码泛化 ────────
    //
    // 算术那边能做泛化（BC_ARITH_STORE 带 XArithOp），是因为它
    // 只需要「槽位 + 运算码」两个操作数。而「比较 + 条件跳」需要
    // 四样：跳转偏移、槽位、常量索引、比较种类 —— 分别要占 a/b/c
    // **再加上一个**，但 Insn 固定 8 字节只装得下 3 个 int16
    // （改结构等于把 cache line 从 8 条指令降到 5 条，得不偿失）。
    //
    // 所以比较类只能一码一种，那就严格按实测频率挑：NE 占 255 万次
    // （来自标准库里大量 `while a != b` 的循环），而 GT/LE/GE 在
    // 同一个负载里各只有几万次，不值得为它们各开一个 opcode。
    //
    // 这也是一个取舍的边界：泛化能压住「运算种类多」的膨胀，压不住
    // 「操作数本来就多」的形态。后者只能靠数据决定加哪几个。
    BC_MOV_LOCAL,            // a = 源槽位, b = 目标槽位
                             //   LOAD_LOCAL a; STORE_LOCAL b
                             //   → stack[base+b] = stack[base+a]
                             //   ★ 必须与拆开执行逐位等价：拷贝出去的
                             //     是**独立持有**的引用（字符串要 retain）。
                             //     漏掉就变成「两个槽共享一个未加计数的
                             //     指针」，释放旧值时把还在用的对象一起
                             //     释放 —— 崩溃点离原因极远。
    BC_NE_CONST_JUMP_FALSE,  // a = 跳转偏移, b = 局部槽位, c = 常量索引
                             //   LOAD_CONST b,c; NE; JUMP_IF_FALSE a
                             //   → 若 `stack[base+b] != consts[c]` 为假则跳
                             //
                             // ★ 这里**不能**简化成整数比较。NE 是通用
                             //   相等，实测语义包括：字符串按内容比、
                             //   Int 与 Float 数值互比、数组/结构体深度比、
                             //   类型不同一律不相等（不报错）。
                             //   实现必须复用 value_equal 那套规则，否则
                             //   `while name != "done"` 这种字符串循环会
                             //   直接算错 —— 而它恰恰是最常见的用法。
                             //
                             // 命名说的是指令构成（NE + 条件跳），不是
                             // 跳转条件；与 LT_CONST_JUMP_FALSE 同一套
                             // 约定：条件为假就跳。
    BC_NE_JUMP_FALSE,        // a = 跳转偏移, b = 左槽位, c = 右槽位
                             //   LOAD_LOAD b,c; NE; JUMP_IF_FALSE a
                             //   → 若 `stack[base+b] != stack[base+c]` 为假则跳
                             //
                             // ── 为什么不复用一个 NE_CONST_JUMP_FALSE ──
                             //
                             // 试过：把「变量 vs 变量」也编码进同一指令，
                             // 用 c 的高位区分常量索引 / 槽位号。但常量
                             // 索引和槽位号都是 int16，塞进一个 int16 就
                             // 得砍掉一半取值范围 —— 而槽位上限和常量
                             // 上限都不是 32767 能轻易缩的东西。
                             // 一条指令省下的分派，不值这个编码复杂度。
                             //
                             // 频率依据：实测 `while i != n`（两变量）与
                             // `while i != 0`（变量 vs 常量）在真实负载里
                             // 是同一量级 —— 手写计数循环偏爱 `!= 0`，
                             // 而遍历数据结构时两边都是变量。只做常量那
                             // 一半，等于只吃了一半的收益。
                             //
                             // 判等规则与 BC_NE_CONST_JUMP_FALSE 完全
                             // 相同（都走 value_equal），区别只是右边
                             // 那个操作数从常量池取还是从槽位取。

    BC_COUNT
} OpCode;

// 指令的可读名，供反汇编与运行时报错使用
const char* bc_op_name(OpCode op);

// 整数运算码的符号表示（`+` / `<<` …），供反汇编与 AOT 生成共用
const char* x_arith_op_symbol(XArithOp op);

// 该指令弹出几个值、压入几个值（供静态检查栈平衡用）
//
// ── 为什么还需要带上下文的版本 ──────────────────────────────
//
// 大多数 opcode 的栈效应只看自己就够，但有少数依赖操作数或函数表：
//   CALL  —— 弹出 argc 个实参、压入 1 个返回值（argc=0 时是 +1，不是 -1）
// 只有 opcode 时这类指令无法判断，会落进 default 的「二元运算 -1」，
// 于是静态栈平衡检查必然误报「栈不平衡」。
//
// 保留单参数版本是因为大部分调用点只需要粗粒度信息（比如 disasm 的
// 注解）；真正做栈平衡分析的地方必须用带 Bytecode* 的版本。
int bc_op_stack_effect(OpCode op);

// 一条指令。
//
// ── 为什么把 pos 从这里移出去（v0.10）──
// 原本 SrcPos pos 直接内嵌：{ OpCode; int32_t a, b; SrcPos pos; }
// 因为 SrcPos 里的 file 是 8 字节指针，整个结构体被对齐到 32 字节。
// 32 字节意味着**一个 64 字节的 cache line 只装 2 条指令**，
// 而解释器的主循环恰恰是顺序扫这条指令流——访存带宽直接浪费一半以上。
//
// 更糟的是：每条指令都要把 pos 读进寄存器（24 字节）才能开始干活，
// 而 99.999% 的指令根本不用它（只有出错时才用）。这是为了「出错时
// 能定位」而在**每一次成功执行**上付费。
//
// 现在拆成「指令数组 + 位置数组」两个平行数组：
//   · 指令 8 字节，一个 cache line 装 8 条
//   · pos 只在出错分支里按 pc 索引旁路数组，热路径完全碰不到
// 位置信息一个字都没丢（06 的「精确到列」照旧成立），只是从
// 「每条指令都付」变成「出错才付」。
//
// 操作数为什么可以是 int16_t：a/b 只存常量池索引、局部槽位号、
// 函数内的跳转目标。三者都被「单文件 4 MiB 上限」（01 §2.1）间接约束：
// 4 MiB 源码最多产出约 10 万条指令，局部槽位远小于 32767，
// 常量池也在同一量级。超限不是静默截断——bc_emit 里有断言，
// 触到上限会直接报「字节码超出 P0 限制」而不是产生错误跳转。
typedef struct {
    uint8_t op;     // OpCode，用 uint8_t 存以压到 8 字节
    int16_t a;      // 常量索引 / 局部槽位 / 跳转目标 / 参数个数
    int16_t b;
    int16_t c;      // 第三个操作数。只有三合一的超指令用得上
                    // （见 BC_LOAD_ADD_STORE）。原本这个位置是纯
                    // padding，拿它当操作数是零成本：结构体仍是
                    // 8 字节，cache line 仍装 8 条指令。
                    //
                    // 为什么敢这么用：C 标准只要求 padding 存在，
                    // 不要求它保持某个值，但我们**显式写它**，
                    // 所以不存在「读到不确定的值」的问题 ——
                    // bc_emit 会把没用到的 c 一律置 0。
} Insn;

// 操作数的合法上限。bc_emit 用它做边界检查。
#define BC_OPERAND_MAX 32767
#define BC_OPERAND_MIN (-32768)

typedef enum {
    CONST_I64,
    CONST_F64,
    CONST_STR,
} ConstKind;

typedef struct {
    ConstKind kind;
    union {
        int64_t i;
        double  f;
        struct { char* s; size_t len; } str;
    };
} Constant;

typedef struct {
    char*     name;
    int       param_count;
    // 参数名与参数类型名（与 param_count 平行的数组；类型项可为 NULL）。
    //
    // ── 为什么函数原型要存参数名（v0.20）───────────────────────
    //
    // 之前参数名只在 AST 里活着，编译完就丢了 —— 反射看得见方法
    // 名，看不见参数名。注解驱动的框架做不到「按名绑定」：路由
    // 收到 GET /users/42，处理器声明 fn byId(id: Int)，框架拿着
    // 路径变量 {id} 却不知道该填进第几个参数。
    //
    // 存下来之后，methodParams(cls, "byId") 返回 [[name, type], ...]，
    // 绑定就成了普通的数据匹配：路径变量、查询串、请求体字段按
    // 参数名查找，类型名负责把字符串转成 Int/Float/Bool。
    // 没有这一步，处理器只能收一个 ctx 对象自己拆包 —— 那是
    // Servlet 时代的姿势，不是 Spring 的。
    char**    param_names;   // 可为 NULL（参数个数为 0 时）
    char**    param_types;   // 可为 NULL；数组元素也可为 NULL（未标注）
    int       local_count;
    int       code_start;
    int       code_len;
    SrcPos    pos;
} FuncProto;

// .xbc 磁盘格式版本。任何影响字节码语义的改动都必须提升它。
//   0x0001 = P0 栈式字节码初版
//   0x0002 = 指令新增 c 操作数落盘。BC_BUILTIN 的 math 子操作编码
//            存在 c 里，不落盘的话从 .xbc 加载后编码全变 0，
//            运行时统一报「未知的数学子操作 0」。
//   0x0003 = 结构体。新增类型表段，且 BC_NEW_STRUCT / BC_FIELD_GET
//            出现在指令流里。旧的 .xbc 没有类型表，遇到这两条指令
//            会拿到不存在的类型索引 —— 所以必须拒绝，不能兼容。
//   0x0004 = 类。新增类表段（含虚表与全局方法槽表），且
//            BC_NEW_CLASS / BC_FIELD_SET / BC_VIRT_CALL 出现在
//            指令流里。旧 .xbc 没有类表，BC_NEW_CLASS 的 a 会索引
//            到不存在的类 —— 必须拒绝，不能兼容。
//   0x0005 = 方法可见性 + 反射。类表段每个类多一张与虚表平行的
//            method_vis 表。旧 .xbc 读进来 method_vis 是空指针 ——
//            而反射内建会解引用它，所以必须拒绝旧文件，不能当成
//            「全 public」。
//   0x0006 = 注解。新增注解元数据段（扁平表，类/字段/方法三种目标
//            混存）。旧 .xbc 没有这一段，读的时候会直接把后面的
//            内容当成注解 —— 长度字段对不上，必须靠版本号分辨。
//   0x0007 = 跳转操作数语义改变。0x0006 及以前 BC_JUMP / JUMP_IF_FALSE /
//            JUMP_IF_TRUE / LT_JUMP_FALSE 的 a 存的是**全程序绝对
//            指令下标**；0x0007 起改成**相对本条下一条的函数内偏移**。
//            两种语义在同一份字节码上会跳到完全不同的位置，而解释器
//            无法分辨自己读的是哪一种 —— 所以旧文件必须拒绝。
//
//            改这一条的真实原因：绝对下标把「整个程序 32767 条指令」
//            变成了硬上限，而本项目自己的 UI 组件库单独编译就有 2.8
//            万条。改成相对偏移后限制降到「单函数 32767 条」，那才是
//            真的写不出来。
//   0x0008 = 函数原型带参数名/参数类型名（methodParams 反射）。旧
//            .xbc 的函数表没有这一段，读的时候会把下一段内容当成
//            参数名 —— 长度对不上，必须靠版本号拒绝。
//
// 放在这里而不是 bytecode.c：写入方（bytecode.c 的 bc_init）与
// 校验方（xbc.c 的 xbc_read）必须看同一个常量。曾经它只定义在
// bytecode.c 里，而 xbc_read 手写了 0x0001 —— 提升版本时只改了
// 一半，症状是「刚 build 出来的 .xbc 立刻被自己拒绝」。
#define XBC_VERSION 0x0008u

// 运行时结构体布局。
//
// ── 为什么放运行时而不是只存在 AST 里 ──────────────────────
//
// 解释器要靠它知道「第 b 个字段在哪」和「这个字段是什么类型」；
// AOT 要靠它生成对应的 C struct 定义。两者读的必须是同一份描述，
// 否则两档的字段偏移一旦不一致，同一个程序在两档下会读写到
// 不同的槽 —— 而 AOT 一致性测试能抓到症状，却指不出原因。
//
// 这也是为什么要落进 .xbc：单发一个字节码文件给别人的机器，
// 那边没有源码，但仍然要能把字段读写跑对。
typedef struct {
    char* name;        // 结构体名（诊断信息用）
    char** field_names;
    // 每个字段的类型名（原样字符串）。运行时只用于报错信息里说
    // 「字段 x 期望 Int 实际是 String」；真正的类型检查在编译期。
    char** field_types;
    int    field_count;
} StructType;

// ============================================================
// 类类型表（v0.15）—— 虚表与对象布局
// ============================================================
//
// ── 虚表为什么必须存在，而不是按名字查 ──────────────────────
//
// 假设不做虚表，`obj.area()` 在运行时只能靠「对象实际类型名 + 方法名」
// 去查一张表。那是**每次调用一次字符串哈希/比较**，而多态调用的
// 频率与普通调用一样高。更糟的是：它把「一个类实现了哪些方法」
// 这件本该在编译期全知道的事，推迟到了运行时。
//
// 虚表把它变成一次数组索引：`vtable[slot]`。slot 在编译期就确定了
// （同一个方法名在所有类里用同一个槽），所以调用点只需带一个 int。
//
// ── 全局方法槽 ──────────────────────────────────────────────
//
// 每个**方法名**（不含类名前缀）分配一个全局槽号。所有类的虚表
// 长度相同 = 全局槽数，未实现的方法填 -1。
//
// 为什么按名字而不是按「每类独立编号」：子类覆写父类方法时，
// 两者的名字相同、槽号自然也相同，于是「把父类虚表抄一份、
// 再把覆写的槽替换掉」就是正确的继承语义。按每类独立编号做不到
// 这一点 —— 那需要一张映射，而映射正是我们想省掉的东西。
//
// 代价：不同类的不相关方法共用一个槽号空间，虚表里有大量 -1。
// 几百个方法 × 几十个类 = 几 KB，换来的是零查找。
typedef struct {
    char* name;       // 方法名（不含类名）
} MethodSlot;

typedef struct {
    char* name;

    // -1 = 无父类。继承链在编译期就解析完了，运行时不需要再找。
    int   parent_idx;

    // ---- 对象布局 ----
    //
    // **父类字段在前，本类新增字段在后**。这条规则是整个继承体系
    // 能工作的前提：`Base b = new Derived()` 之后，通过 b 访问
    // Base 的字段，偏移在 Base 与 Derived 里必须相同。
    //
    // 如果反过来（子类字段在前），同一个字段在两种静态类型下
    // 就有两个偏移，那字段访问就必须依赖静态类型 —— 而虚方法里
    // 的 self 是被当作父类类型编译的，于是子类方法访问自己的
    // 字段会拿到父类的偏移，读到错误的内存。
    //
    // 这个 bug 的表现是「子类打印出来的字段值全是父类的」，
    // 而且不崩溃 —— 只有靠 AOT 差分或断言才能发现。
    char** field_names;      // 全量（含继承），父类在前
    char** field_types;
    Visibility* field_vis;   // 与上平行，编译期检查用
    bool*  field_final;
    int    field_count;      // 全量
    int    own_field_start;  // 本类第一个自有字段的下标

    // ---- 虚表 ----
    //
    // 长度 = bc->method_slot_count，未实现的槽是 -1。
    // 值是对 bc->funcs 的下标。
    int*   vtable;
    int    vtable_len;

    // ---- 方法可见性（与 vtable 平行，按槽索引）----
    //
    // ── 为什么按槽而不是按方法名 ----
    //
    // 槽是全局唯一的：同名方法在所有类里共用一个槽。所以
    // `method_vis[slot]` 天然就是「这个名字的方法在这个类里的可见性」，
    // 不需要额外的名字查找。这一点很重要 —— 可见性检查发生在
    // 编译方法调用的热路径上，能直接索引就不该走字符串比较。
    //
    // ── 为什么单独一张表而不是塞进 vtable 的高位 ----
    //
    // vtable 里已经用了 -1（未实现）和 -2（待回填）两个哨兵值，
    // 再往前者的位宽里塞可见性会让「读虚表」这件事立刻变得需要
    // 解码。两张平行的表读起来慢半拍，但正确性一眼可验 ——
    // 而这个文件已经因为「哨兵值语义混用」出过一次 bug。
    //
    // 未实现的槽填 VIS_PUBLIC。那不是「猜的」：继承阶段会把父类的
    // 槽补下来，可见性同样要跟着补（见 build_vtable_funcs 末尾），
    // 填 PUBLIC 只是为了给「既没实现也没继承」的槽一个确定值。
    Visibility* method_vis;

    // ---- 类型性质 ----
    bool   is_abstract;
    bool   is_interface;
    bool   is_final;

    // 直接实现的接口类索引（不含通过父类继承来的）
    int*   iface_idx;
    int    iface_count;

    // 构造函数对应的函数索引。-1 表示没有显式构造函数
    // （此时所有字段清零：Int=0, Float=0.0, Bool=false, 其余 null）。
    int    ctor_fn;

    // 类名在 .xbc 里的稳定标识。目前直接用 name，保留字段是为了
    // 将来做类名混淆/重命名时不破坏已有字节码。
    char*  mangle;
} ClassType;

typedef struct {
    Insn*     code;
    int       code_count;
    int       code_cap;
    // 与 code 等长的旁路数组：code[i] 的源码位置。
    // 单独存放的理由见 Insn 的注释——热路径只读 code，出错才读 pos。
    // 反汇编、.xbc 读写、运行时诊断都从这里取位置。
    SrcPos*   pos;
    int       pos_cap;
    Constant* consts;
    int       const_count;
    int       const_cap;
    FuncProto* funcs;
    int       func_count;
    int       func_cap;
    // 结构体类型表。字段顺序就是内存布局顺序 —— 解释器和 AOT
    // 都按这个顺序算偏移，所以它一旦写进 .xbc 就不能重排。
    StructType* structs;
    int         struct_count;
    int         struct_cap;
    // 类表（v0.15）。与结构体表平行但**独立**：两者的运行时表示、
    // 字段可变性、继承能力全不同，塞进一张表会让每条指令都多一个
    // 「这是 struct 还是 class」的判断。
    ClassType*  classes;
    int         class_count;
    int         class_cap;
    // 全局方法槽表。所有类的虚表长度都等于这个数。
    MethodSlot* method_slots;
    int         method_slot_count;
    int         method_slot_cap;
    // 注解元数据（v0.16）。扁平表，三种目标混存 —— 见 AnnoEntry
    // 的注释：嵌套存会让 .xbc 读写变成三份几乎相同的代码。
    AnnoEntry*  annos;
    int         anno_count;
    int         anno_cap;
    uint32_t  version;      // .xbc 格式版本，从第一天就有（05 §6 建议 3）

    // 源文件名。代码生成需要把它写进产物（运行时错误信息要报出
    // 「哪个文件的第几行」），而 FuncProto 里的 pos.file 只覆盖到
    // 函数声明那一行——顶层语句合成的 main 未必有。
    // .xbc 读写时也会带上它，这样单发一个 .xbc 给别人的机器，
    // 报错信息里仍然有可读的文件名。
    char*     source_file;
} Bytecode;

// ============================================================
// 平台内建编号（BC_BUILTIN 的操作数）
// ============================================================
//
// 这些是「语言本身不提供、由运行时实现」的函数，统称平台内建。
// 与 array/len 这类纯语言内建不同，它们依赖外部环境（窗口系统、
// 文件系统、时钟）。
//
// 编号一旦发布就不要改：它出现在 .xbc 文件里（BC_BUILTIN 的 a 操作数），
// 改了会让旧的字节码文件行为错乱。新增只能往后加。
typedef enum {
    XBI_NONE = 0,

    // ---- 窗口 ----
    XBI_WIN_OPEN,          // (title: String, w: Int, h: Int) -> Bool
    XBI_WIN_CLOSE,         // () -> Null
    XBI_WIN_SHOULD_CLOSE,  // () -> Bool
    XBI_WIN_BEGIN_FRAME,   // () -> Null
    XBI_WIN_END_FRAME,     // () -> Null
    XBI_WIN_FILL_RECT,     // (x, y, w, h, color) -> Null
    XBI_WIN_BORDER_RECT,   // (x, y, w, h, thickness, color) -> Null
    XBI_WIN_TEXT,          // (x, y, text, color) -> Null
    XBI_WIN_TEXT_WIDTH,    // (text) -> Int
    XBI_WIN_TEXT_HEIGHT,   // () -> Int
    XBI_WIN_POLL_CLICK,    // () -> Array 或 Null（[x, y, button]）
    XBI_WIN_MOUSE_X,       // () -> Int
    XBI_WIN_MOUSE_Y,       // () -> Int
    XBI_WIN_MOUSE_DOWN,    // (button) -> Bool
    XBI_WIN_TIME,          // () -> Float
    XBI_WIN_SLEEP,         // (ms) -> Null
    XBI_WIN_SCREENSHOT,    // (path) -> Bool

    // ---- 形状 / 样式原语（v0.17）----
    //
    // ── 为什么原来画不出圆角、旋转、渐变 ────────────────────
    //
    // v0.16 之前全部绘制能力只有三个原语：fill_rect / border_rect /
    // text，而 Win32 侧三者都只调 FillRect（轴对齐实心矩形）。于是
    // 上层无论怎么写，都画不出圆角按钮、旋转图标、淡入淡出 ——
    // 这不是「UI 库缺包装」，是**底层没有能力**。
    //
    // 这批原语补的就是那层能力。设计上守两条：
    //
    //   1. 每个原语都能在一次 GDI 调用内完成，不做软件光栅化。
    //      理由：X 的定位是「小而全的工具链」，不带 2D 引擎。
    //      GDI 画不出来的东西（真实抗锯齿、3D 变换）不在这批里。
    //   2. 变换与 alpha 用**栈**（push/pop）而不是「传一个大结构体」。
    //      因为样式是嵌套生效的：面板设了 alpha=0.8，里面的按钮
    //      不该每层都把 0.8 再算一遍；栈让「继承 + 局部覆盖」自然成立。
    //
    // ── 圆角为什么是一个原语而不是四个 fill_rect ─────────────
    //
    // 四个角用扇形填充拼出来，接缝处会有 1px 的锯齿缝（GDI 的
    // Chord / Pie 边界不含终点）。实测过：在半径 4~8 区间肉眼可见。
    // 所以用 RoundRect 一次成型，靠 GDI 自己处理。
    XBI_WIN_FILL_ROUND,    // (x, y, w, h, radius, color) -> Null
    XBI_WIN_BORDER_ROUND,  // (x, y, w, h, radius, thickness, color) -> Null
    XBI_WIN_ELLIPSE,       // (x, y, w, h, color) -> Null
    XBI_WIN_PATH,          // (points: Array, color) -> Null   点集 [x0,y0,x1,y1,...] 自动闭合
    XBI_WIN_GRADIENT,      // (x, y, w, h, c1, c2, vertical: Bool) -> Null
    XBI_WIN_PUSH_TRANSFORM,// (pivotX, pivotY, rotDeg: Float, scale: Float) -> Null
    XBI_WIN_POP_TRANSFORM, // () -> Null
    XBI_WIN_ALPHA,         // (a: Float) -> Null   0.0~1.0，压栈，配 winPopAlpha
    XBI_WIN_POP_ALPHA,     // () -> Null
    XBI_WIN_CLIP,          // (x, y, w, h) -> Null
    XBI_WIN_UNCLIP,        // () -> Null
    XBI_WIN_TEXT_ROT,      // (x, y, text, color, rotDeg: Float) -> Null

    // ---- 窗口段的结束标记 ----
    //
    // ── 为什么必须有这一项 ──────────────────────────────────
    //
    // bc_uses_gui 判「程序要不要链接窗口运行时」，判据是
    // `id > XBI_NONE && id < 上界`。这一项就是那个上界。
    //
    // 原本的写法是上界 = XBI_COUNT（也就是「只要用了任何内建就算
    // 用了窗口」）。在枚举里只有窗口函数时这个近似恰好成立，于是
    // 它一直是对的 —— 直到数组/字符串那批内建加进来为止。
    //
    // 那之后每个用到 arrPush 或 strAt 的程序都会命中原判断，症状是：
    //
    //   · 编译期多一条「程序用到了窗口能力，加 --profile=desktop
    //     重编」的警告 —— 而这条建议是**有害**的，照做会把程序
    //     变成 GUI 子系统，双击之后 println 全部静默丢掉；
    //   · 产物里白白内嵌整段窗口运行时（几百 KB）。
    //
    // 所以这里显式钉一个边界，并且它必须是**窗口段的最后一项**。
    // 新增非窗口内建一律加在它之后。谁要是把窗口函数加在这一项
    // 前面，绕过边界只会让警告误报回归 —— 不会崩，但会骗人，
    // 而骗人的诊断比没有诊断更糟。
    XBI__WIN_END,

    // ---- 数组（标准库基础） ----
    //
    // 为什么这些必须是内建而不是写在 stdlib 里：
    // X 没有结构体、没有指针、没有引用参数。纯 X 函数拿不到「原数组」
    // 并原地扩容 —— 只能返回新数组，于是 `arrPush(a, x)` 会退化成
    // `a = arrPush(a, x)`，既反直觉又容易漏赋值。放进内建才能做
    // 原地操作，语义与 Python 的 list.append / JS 的 push 一致。
    XBI_ARR_PUSH,          // (arr, value) -> Int（新长度）
    XBI_ARR_POP,           // (arr) -> 被弹出的元素；空数组返回 null
    XBI_ARR_INSERT,        // (arr, index, value) -> Int
    XBI_ARR_REMOVE,        // (arr, index) -> 被移除的元素
    XBI_ARR_SLICE,         // (arr, start, end) -> Array（新数组）
    XBI_ARR_CONCAT,        // (a, b) -> Array（新数组）
    XBI_ARR_REVERSE,       // (arr) -> Null（原地）
    XBI_ARR_INDEX_OF,      // (arr, value) -> Int（找不到返回 -1）
    XBI_ARR_CONTAINS,      // (arr, value) -> Bool
    XBI_ARR_FILL,          // (arr, value) -> Null（原地全部填成 value）
    XBI_ARR_SORT,          // (arr) -> Null（原地，数值/字符串升序）
    XBI_ARR_COPY,          // (arr) -> Array（浅拷贝）

    // ---- 字符串（标准库基础） ----
    //
    // P0 的 String 只支持 len 与 +，连下标都取不到。任何字符串库
    // （json / 格式化 / 分词）都写不出来，所以这批原语是必需的。
    // 全部按 UTF-8 **码点**索引，不按字节 —— 中文用户会立刻发现
    // 按字节切的后果（一个字被切成半个）。
    XBI_STR_AT,            // (s, i) -> String（第 i 个字符，越界返回 ""）
    XBI_STR_SUB,           // (s, start, end) -> String
    XBI_STR_FIND,          // (s, sub) -> Int（找不到返回 -1）
    XBI_STR_FIND_FROM,     // (s, sub, from) -> Int
    XBI_STR_SPLIT,         // (s, sep) -> Array
    XBI_STR_JOIN,          // (arr, sep) -> String
    XBI_STR_UPPER,         // (s) -> String（仅 ASCII 字母）
    XBI_STR_LOWER,         // (s) -> String（仅 ASCII 字母）
    XBI_STR_TRIM,          // (s) -> String
    XBI_STR_REPLACE,       // (s, from, to) -> String
    XBI_STR_STARTS,        // (s, prefix) -> Bool
    XBI_STR_ENDS,          // (s, suffix) -> Bool
    XBI_STR_REPEAT,        // (s, n) -> String
    XBI_STR_CODE,          // (s) -> Int（首字符码点）
    XBI_STR_FROM_CODE,     // (code) -> String
    XBI_STR_PARSE_INT,     // (s) -> Int（失败返回 0，配合 isInt 用）
    XBI_STR_PARSE_FLOAT,   // (s) -> Float

    // ---- 数学 ----
    XBI_MATH,              // 统一入口：a = 子操作编号（见 XMathOp）
    XBI_RANDOM,            // () -> Float [0,1)
    XBI_RANDOM_SEED,       // (seed) -> Null
    // ---- 错误报告 ----
    // `fail(msg)` 是标准库唯一的报错手段。X 没有异常、没有 Result，
    // 标准库函数遇到非法输入时要么静默返回垃圾值、要么直接终止进程。
    // 前者会让错误一路传播到结果里（数值代码最怕这个），所以选后者。
    // 它的语义是「带源码位置的 fatal error」，与运行时自身报错同一条路径。
    XBI_FAIL,              // (msg: String) -> Null（不返回）
    // `typeOf(x)` 返回值的运行时类型名（"Int"/"Float"/"Bool"/"String"/"Array"/"Null"）。
    // 标准库需要它做序列化与字段取值：JSON 里一个值可能是数字也可能是
    // 字符串，要正确转义就必须先知道类型。没有它，标准库只能靠
    // 「toString 后能不能转回来」猜，而那个猜测在字符串内容是 "123"
    // 时会直接给出错误答案。
    XBI_TYPEOF,            // (x) -> String

    // ---- 文件 ----
    //
    // ── 为什么这批必须是内建 ────────────────────────────────
    //
    // 没有它们，语言写不出任何「有持久化」的程序：打印到终端之后
    // 一切都蒸发。std.fs / std.db / std.http 的静态文件服务全都
    // 建立在这 10 个函数之上。
    //
    // 为什么不是「一个 fileOpen 返回句柄 + 读写」：语言没有结构体
    // 也没有引用，句柄只能是一个整数，而整数的生命周期全靠调用方
    // 自觉。忘了关文件在长期运行的服务里会耗尽句柄数。整文件读写
    // 让「关了没有」这个问题不存在。
    //
    // 代价是没法增量写大文件 —— 但那是「流」该解决的问题，等语言
    // 有了句柄类型再说。现在这些函数覆盖的是配置、日志、数据库表
    // 这类「一次读写几 KB 到几 MB」的用法，正是实际需求所在。
    XBI_FS_READ,           // (path) -> String（打不开时 fail）
    XBI_FS_READ_OR,        // (path, def) -> String（打不开给默认值）
    XBI_FS_WRITE,          // (path, text) -> Bool
    XBI_FS_APPEND,         // (path, text) -> Bool
    XBI_FS_EXISTS,         // (path) -> Bool
    XBI_FS_IS_DIR,         // (path) -> Bool
    XBI_FS_REMOVE,         // (path) -> Bool
    XBI_FS_SIZE,           // (path) -> Int（不存在返回 -1）
    XBI_FS_LIST,           // (dir) -> Array（文件名，不含 . 与 ..）
    XBI_FS_MKDIR,          // (path) -> Bool（已存在视为成功）
    XBI_FS_REPLACE,        // (path, text) -> Bool（原子替换）

    // ---- TCP ----
    //
    // 服务端：listen / accept / recv / send / close
    // 客户端：connect
    //
    // ── 句柄用 Int 传递，-1 表示无效 ────────────────────────
    //
    // 上层把句柄当不透明整数用。平台层负责把 Windows 的 UINT_PTR
    // 塞进 int64，所以 64 位句柄不会被截断（那是「第一个连接正常、
    // 第二个收到杂质数」的经典来源）。
    //
    // ── 全部非阻塞 ──────────────────────────────────────────
    //
    // 这一条改变了 API 的形状：recv 没有数据时返回空串而不是等待，
    // accept 没有连接时返回 -1。调用方要自己轮询。
    //
    // 为什么这么设计：单线程的即时模式服务器（与 std.ui 同一个思路）
    // 必须能在同一个循环里同时做「接受连接、读数据、检查退出信号」。
    // 阻塞的 accept 会让空闲服务器卡死在一个调用里，连 Ctrl+C 都
    // 响应不了 —— 那种服务看起来「在跑」，实际已经不可控。
    XBI_NET_LISTEN,        // (bindAddr, port) -> Int（port 传 0 由系统分配）
    XBI_NET_ACCEPT,        // (listener) -> Int（没有连接给 -1）
    XBI_NET_RECV,          // (sock) -> String（没有数据给 ""，对端关闭给 ""）
    XBI_NET_SEND,          // (sock, text) -> Int（实际发出的字节数，出错 -1）
    XBI_NET_CLOSE,         // (sock) -> Null
    XBI_NET_CONNECT,       // (host, port) -> Int（失败 -1）
    XBI_NET_PORT,          // (listener) -> Int（实际监听端口）
    XBI_NET_PEER,          // (sock) -> String（"1.2.3.4:5678"）
    XBI_NET_ERR,           // () -> String（最近一次网络错误的可读描述）
    XBI_NET_PENDING,       // (sock) -> Bool（对端已关闭 / 无更多数据）

    // ---- 通用 ----
    //
    // ── sleep 为什么必须在这里，而不是复用 winSleep ──────────
    //
    // winSleep 在窗口段里，于是 bc_uses_gui 会把它算作「用到了窗口
    // 能力」。而「等一会儿」跟窗口毫无关系：事件循环限速、客户端
    // 轮询退避、测试等外部进程，全都要它。
    //
    // 后果不是少个功能，是给出**有害的建议**：一个纯控制台服务器
    // 收到「加 --profile=desktop 重编」，照做之后产物变成 WINDOWS
    // 子系统，println 全部静默丢失 —— 用户会去查逻辑 bug，而真相
    // 是编译选项。
    //
    // 所以 sleep 放在窗口段边界（XBI__WIN_END）之后，属于通用内建。
    XBI_SLEEP,             // (ms) -> Null

    // ---- 时间 ----
    //
    // ── 为什么需要独立的 nowMillis ────────────────────
    //
    // 唯一的计时原语本来是 winTime()，但它挂在窗口能力下。任何想
    // 测量「一次查询花了多久」的服务端代码只要用了它，就会被编译
    // 器判为图形程序 —— 控制台产物收到「加 --profile=desktop 重编」
    // 的建议，照做后 println 全部静默丢失。这和 sleep 当初的问题
    // 是同一个：计时与窗口毫无关系。
    //
    // 所以补一个通用墙上时钟：从 1970-01-01 UTC 起的毫秒数。
    // 用 Int 而不是 Float 是因为它主要用途是「两个时刻相减」和
    // 「作为时间戳存档」，两者都不需要亚毫秒精度，而 Int 差值
    // 不会有浮点累积误差。
    XBI_NOW_MILLIS,        // () -> Int
    XBI_NOW_SECONDS,       // () -> Int

    // 单调时钟（毫秒）。与 nowMillis 的分工见 platform.h：
    // 时间戳用 nowMillis，量差值用 monoMillis。它唯一的用途就是
    // 「两个时刻相减」，所以不给「转日期」之类的能力。
    XBI_MONO_MILLIS,       // () -> Int

    // ---- 字节 ----
    //
    // ── 为什么语言需要「从字节数组造字符串」这个内建 ─────────
    //
    // X 的 String 底层是 UTF-8 字节序列，但语言层只暴露了「字符」
    // 抽象：strFromCode(0x4E2D) 给出「中」的正确 UTF-8 编码，而
    // 拿不到「把这三个字节 E4 B8 AD 原样拼起来」这个能力。
    //
    // 缺了它，任何**按字节处理**的协议都无法正确实现。最典型的是
    // URL 解码：%E4%B8%AD 是三个字节，逐字节转字符再拼接，得到的
    // 是 U+00E4/U+00B8/U+00AD 三个 Latin-1 字符的 UTF-8 编码
    // （各 2 字节，共 6 字节），而不是「中」的 3 字节。症状是
    // 「英文参数正常，中文参数变成乱码」—— 而这不是可以留到 P1
    // 的小问题：中文查询参数、GBK/UTF-8 表单、二进制协议头全都
    // 走这条路。
    //
    // 所以加两个最小的原语：
    //   strFromBytes(arr) —— 把 0..255 的字节数组原样拼成字符串
    //   strBytes(s)       —— 把字符串的字节拆成 0..255 的数组
    //
    // 它们不做字符语义，只做「字节 ↔ 字符串」的无损转换。有了这
    // 一对，std.http 的 URL 编解码、std.db 的二进制字段、将来的
    // base64 与压缩都能在标准库里实现，而不需要每个协议再加内建。
    XBI_STR_FROM_BYTES,    // (Array) -> String
    XBI_STR_BYTES,         // (String) -> Array

    // ---- 反射 ----
    //
    // ── 为什么反射要作为内建，而不是纯标准库 ----
    //
    // 反射的本质是「在运行时读到编译期才存在的元数据」。标准库是
    // 普通 X 代码，它拿不到 ClassType 表，也看不见字段名/可见性 ——
    // 那都是编译器内部结构。所以入口必须在运行时里，由它把元数据
    // 打包成普通值（String / Array）交回语言层。
    //
    // ── 与「静态类型」是否冲突 ----
    //
    // 反射不破坏静态检查：`a.someField` 仍然在编译期校验，
    // `getField(a, "someField")` 是**显式**的运行时查找，名字打错
    // 只会在运行时失败。两者并存是正常语言的形态 —— 反射是逃生舱，
    // 不是默认路径。
    //
    // ── 为什么反射也要守可见性 ----
    //
    // 否则 private 就形同虚设：`getField(obj, "secret")` 一句就把
    // 封装绕过去了。所以反射读写字段时，判据与直接访问完全一致
    // （本类 / 子类 / 全开）。这条比「灵活」重要得多。
    XBI_CLASS_NAME,        // (x) -> String（实例的**实际**类名；非实例返回类型名）
    XBI_CLASS_PARENT,      // (x) -> String（父类名，没有则 ""）
    XBI_CLASS_IS,          // (x, className: String) -> Bool（沿继承链与接口判）
    XBI_CLASS_FIELDS,      // (x) -> Array of [name, type, vis, final]
    XBI_CLASS_METHODS,     // (x) -> Array of [name, vis]
    XBI_FIELD_GET,         // (x, name: String) -> 值（不存在或越权则 fail）
    XBI_FIELD_SET,         // (x, name: String, v) -> Null
    XBI_FIELD_HAS,         // (x, name: String) -> Bool
    XBI_METHOD_HAS,        // (x, name: String) -> Bool

    // ---- 注解查询 ----
    //
    // 注解在语言层「只存不解释」，所以查询必须暴露两件事：
    //   · 有没有某个注解
    //   · 它的实参是什么
    //
    // ── 为什么签名里一定有 member 这一位 ────────────────────
    //
    // 第一版写成 (x, target, name)，漏了「哪个成员」。于是
    // `annoGet(u, "field", "Column")` 在 User 有三个带注解的字段时
    // 无法回答「你要哪一个 Column」—— 实测它返回空数组，而那看起来
    // 像「这个注解没有实参」，是最容易被误读成正常结果的错误。
    //
    // 类目标不需要成员名，但那一位仍然存在（传 ""）。让四个函数
    // 形状一致，比省掉一个参数更重要：参数个数不同的重载在
    // 「弹栈顺序」上是两套代码，而那正是最容易错位的地方。
    XBI_ANNO_HAS,          // (x, target, member: String, name: String) -> Bool
    XBI_ANNO_GET,          // (x, target, member, name) -> Array（位置实参）
    XBI_ANNO_GET_KV,       // (x, target, member, name) -> Array of [key|null, val]
    XBI_ANNO_ALL,          // (x, target, member) -> Array of 注解名

    // ---- 动态调用辅助（v0.20，框架支持）----
    //
    // classNew / methodCall 不在这张表里：它们要推调用帧，走的是
    // BC_NEW_CLASS_NAME / BC_CALL_NAME 指令（见 OpCode 的说明），
    // 编译器按名字特判，不经过 BC_BUILTIN 通道。
    //
    // classAll 是纯查询（0 参 → 类名数组），不需要推帧，留在内建
    // 通道里。它是组件扫描的起点：框架先拿到全部类名，再用注解
    // 查询过滤，最后才对命中的类做 classNew。
    XBI_CLASS_ALL,         // () -> Array[String]（字节码里全部类名，含接口）

    // methodParams(cls, "methodName") -> [[name, type], ...]
    //
    // 参数元数据是「按名绑定」的依据：路由分派拿路径变量/查询串/
    // 请求体字段按参数名对号入座，类型名负责把字符串转成目标类型。
    // 返回值与 classFields 同风格 —— 全是普通数组，没有第二套
    // 「元对象」类型。接收者收实例或类名字符串；方法按虚表槽解析，
    // 继承来的方法返回实现方的参数表。
    XBI_METHOD_PARAMS,     // (x, name: String) -> Array

    // ---- 编辑器支持（v0.21）----
    //
    // 追加在末尾而不是插进窗口段：插入会改变后面所有编号，
    // 让已经生成的 .xbc 全部错位。bc_uses_gui 用显式范围把这一批
    // 也算进窗口能力（见 bytecode.c）。
    XBI_WIN_POLL_KEY,      // () -> Array?  [key: Int, ch: String, mods: Int]
    XBI_WIN_POLL_WHEEL,    // () -> Int
    XBI_WIN_KEY_DOWN,      // (key: Int) -> Bool
    XBI_WIN_WIDTH,         // () -> Int
    XBI_WIN_HEIGHT,        // () -> Int
    XBI_WIN_SET_TITLE,     // (title: String) -> Null
    XBI_WIN_SET_FONT,      // (family: String, size: Int, bold: Bool) -> Bool
    XBI_WIN_SET_CURSOR,    // (kind: Int) -> Null
    XBI_WIN_CLIP_GET,      // () -> String
    XBI_WIN_CLIP_SET,      // (text: String) -> Bool
    XBI_WIN_OPEN_FILE,     // (title, filter, initial: String) -> String
    XBI_WIN_SAVE_FILE,     // (title, filter, initial: String) -> String
    XBI_WIN_MESSAGE,       // (title, text: String, kind: Int) -> Int
    XBI_WIN_SET_RESIZABLE, // (on: Bool) -> Null

    // ---- 裸机（kernel profile 专用）----
    //
    // ── 为什么这批内建是「内核能不能写」的唯一门槛 ──────────
    //
    // 内核与用户态程序的差别不在于「功能多少」，而在于**它直接
    // 与硬件对话**。具体只有三类动作：
    //
    //   1. 按物理地址读写内存（显存 0xB8000、MMIO 寄存器、页表）
    //   2. 读写 I/O 端口（串口 0x3F8、PIC 0x20、PCI 配置空间）
    //   3. 执行特权指令（cli/sti/hlt/invlpg）
    //
    // 语言原本这三样都没有：无指针 → 拿不到「地址」这个概念；
    // 无 asm/extern → 调不出 out/in/sti。所以内核一行都写不出来。
    //
    // ── 为什么用「定宽读写」而不是引入指针类型 ─────────────
    //
    // 完整的指针类型系统（类型、解引用、取地址、指针算术、与数组
    // 的关系、生命周期）是一个跨词法/AST/类型检查/两档后端的工程。
    // 而内核真正需要的只是「对某个整数地址做一次 N 字节的易失读写」——
    // 这也正是 Linux 的 readb/writeb/readl/writel 抽象做的事。
    //
    // 定宽函数把语义钉死在函数名里（memRead32 就是 4 字节小端读），
    // 不会写出「指针类型错但值对」的代码；代价是写不出 memcpy 之外的
    // 指针算术，而 memcpy 这类东西本来就该用循环 + 这三个原语实现。
    //
    // ── volatile 是必须的，不是优化选项 ────────────────────
    //
    // 编译器不知道 0xB8000 是显存 —— 在它看来那只是个普通地址，
    // 没有任何人写它，于是 `x = memRead32(0xB8000)` 会被当成常量
    // 折叠，轮询循环 `while memRead32(reg) & 1 == 0 {}` 会被优化成
    // 死循环。所以生成代码里每次访问都必须是 volatile 限定。
    //
    // ── 宿主平台的行为 ────────────────────────────────────
    //
    // 在 console/desktop/service 档下这些函数**直接 fail 并说明原因**，
    // 不是返回 0。原因：返回 0 会让「忘了加 --profile=kernel」表现为
    // 「串口没输出但程序正常退出」，而真正的原因（档位不对）完全
    // 看不出来。内核内建只在 freestanding 目标里被允许使用。
    XBI_MEM_READ8,         // (addr: Int) -> Int   读 1 字节（无符号）
    XBI_MEM_READ16,        // (addr: Int) -> Int   读 2 字节小端（无符号）
    XBI_MEM_READ32,        // (addr: Int) -> Int   读 4 字节小端
    XBI_MEM_WRITE8,        // (addr: Int, v: Int) -> Null
    XBI_MEM_WRITE16,       // (addr: Int, v: Int) -> Null
    XBI_MEM_WRITE32,       // (addr: Int, v: Int) -> Null
    XBI_PORT_IN8,          // (port: Int) -> Int
    XBI_PORT_IN16,         // (port: Int) -> Int
    XBI_PORT_OUT8,         // (port: Int, v: Int) -> Null
    XBI_PORT_OUT16,        // (port: Int, v: Int) -> Null
    XBI_CPU_HLT,           // () -> Null    停到下一次中断
    XBI_CPU_CLI,           // () -> Null    关中断
    XBI_CPU_STI,           // () -> Null    开中断
    XBI_CPU_INVLPG,        // (vaddr: Int) -> Null  失效单页 TLB

    XBI_COUNT
} XBuiltin;

// 数学内建的子操作。
//
// ── 为什么合成一个 XBI_MATH 而不是每个函数一个编号 ──────────
//
// 数学函数有一批（pow/sqrt/sin/cos/tan/log/exp/floor/ceil/round/
// trunc/sign/min/max...），每个都加一个 XBI_ 项会让枚举迅速膨胀，
// 而它们**参数个数与返回值预处理方式完全同构**（1 或 2 个数字进，
// 1 个数字出）。分成 20 个编号意味着 interp.c 和 codegen_c.c 各写
// 20 个几乎一样的 case —— 复制粘贴 20 次，出错概率乘 20。
//
// 合成一个编号 + 子操作，两处都只写一个 case 加一张跳转表。
// 代价是 `a` 操作数要同时编码子操作（低 8 位）与参数个数（高位），
// 但那个编码只在一个地方解，比 20 份重复代码可控得多。
typedef enum {
    XMO_NONE = 0,
    XMO_POW,        // (base: Float, exp: Float) -> Float
    XMO_SQRT,       // (x) -> Float
    XMO_CBRT,       // (x) -> Float
    XMO_FLOOR,      // (x) -> Int
    XMO_CEIL,       // (x) -> Int
    XMO_ROUND,      // (x) -> Int
    XMO_TRUNC,      // (x) -> Int
    XMO_ABS,        // (x) -> 数值（保持输入类型：Int->Int, Float->Float）
    XMO_SIGN,       // (x) -> Int
    XMO_MIN,        // (a, b) -> 数值
    XMO_MAX,        // (a, b) -> 数值
    XMO_SIN, XMO_COS, XMO_TAN,
    XMO_ASIN, XMO_ACOS, XMO_ATAN,
    XMO_ATAN2,      // (y, x) -> Float
    XMO_LOG,        // (x) -> Float（自然对数）
    XMO_LOG2,
    XMO_LOG10,
    XMO_EXP,
    XMO_HYPOT,      // (x, y) -> Float
    XMO_FMOD,       // (x, y) -> Float
    XMO_CLAMP,      // (x, lo, hi) -> 数值
    XMO_COUNT
} XMathOp;

// 取出 XBI_MATH 指令里编码的子操作与参数个数。
//
// 编码方式：a = (argc << 8) | op。argc 最多 3（clamp），8 位够用。
// 用两个小函数而不是让每个调用点自己写移位，是为了避免「某处忘了
// 移位」这种错误 —— 那会让 pow 被当成 sqrt 执行，而且完全不报错。
static inline int xmath_op(int a)  { return a & 0xFF; }
static inline int xmath_argc(int a) { return (a >> 8) & 0xFF; }
static inline int xmath_enc(int op, int argc) { return (argc << 8) | (op & 0xFF); }

// 带上下文的栈效应。
//
// ── 为什么需要这个版本 ──────────────────────────────────────
//
// 大多数 opcode 的栈效应只看自己就够，但有少数依赖操作数或函数表：
//   CALL  —— 弹出 argc 个实参、压入 1 个返回值
//            argc=0 时是 +1，而 default 分支的「二元运算 -1」是错的。
// 只看 opcode 时这类指令必然算错，静态栈平衡检查就会误报
// 「栈不平衡」，还把原因指向超指令——方向完全跑了。
//
// 保留单参数版本是因为有些调用点只需要粗粒度信息（如 disasm 注解）；
// 真正做栈平衡分析的地方必须用这个版本。
int bc_op_stack_effect_at(Bytecode* bc, const Insn* in);

// 平台内建表查询。编译器按名字查下标，解释器/AOT 按 id 分派。
int         bc_builtin_count(void);
const char* bc_builtin_name(int i);
// 按 id 反查名字（与按表下标查的 bc_builtin_name 不同：表顺序与枚举
// 顺序不必相同，反汇编/报错都应该走这个）。
const char* bc_builtin_name_of(XBuiltin id);
XBuiltin    bc_builtin_id(int i);
int         bc_builtin_argc(int i);
const char* bc_builtin_ret(int i);
int         bc_builtin_find(const char* name);

void bc_init(Bytecode* bc);
int  bc_emit(Bytecode* bc, OpCode op, int32_t a, int32_t b, SrcPos pos);
int  bc_add_const_i64(Bytecode* bc, int64_t v);
int  bc_add_const_f64(Bytecode* bc, double v);
int  bc_add_const_str(Bytecode* bc, const char* s, size_t len);
int  bc_add_func(Bytecode* bc, const char* name, int param_count, SrcPos pos);

// 结构体类型表。返回类型索引（BC_NEW_STRUCT / BC_FIELD_GET 的 a 操作数）。
int  bc_add_struct(Bytecode* bc, const char* name,
                   char** field_names, char** field_types, int count);
int  bc_find_struct(Bytecode* bc, const char* name);          // -1 = 没有
int  bc_find_field(Bytecode* bc, int type_idx, const char* f); // -1 = 没有

// 类表（v0.15）。返回类索引（BC_NEW_CLASS 的 a 操作数）。
int  bc_add_class(Bytecode* bc, const char* name);
int  bc_find_class(Bytecode* bc, const char* name);           // -1 = 没有

// 全局方法槽：同名方法在所有类里共用同一个槽号，这是虚表能工作、
// 也是「子类覆写自动生效」的全部前提。见 ClassType 的说明。
int  bc_method_slot(Bytecode* bc, const char* name);          // 不存在则创建
int  bc_find_method_slot(Bytecode* bc, const char* name);     // -1 = 没有

// 在全量字段表里找字段（含继承字段）。返回字段下标，-1 = 没有。
int  bc_find_class_field(Bytecode* bc, int cls_idx, const char* f);

// 注解表（v0.16）。把一条 AST 注解拍平进全局表。
void bc_add_anno(Bytecode* bc, AnnoTarget target, int class_idx,
                 const char* member, const Annotation* a);

// 查注解。member 只在 FIELD/METHOD 目标下有意义，类注解传 NULL。
// 返回第一条命中的记录，没有则 NULL。
AnnoEntry* bc_find_anno(Bytecode* bc, AnnoTarget target, int class_idx,
                        const char* member, const char* name);

// 沿父链判断 sub 是否是 base 的子孙（含自身）。多态赋值与参数传递
// 的合法性检查全靠它。
bool bc_class_is_a(Bytecode* bc, int sub_idx, int base_idx);

// ============================================================
// 6. 各阶段入口
// ============================================================

// 语法分析：token 数组 → Module
Module* parse(Token* toks, size_t n, Arena* a, const char* file);

// 降级：Module → Bytecode
bool compile_to_bytecode(Module* m, Bytecode* bc, Arena* a);

// 解释执行：返回进程退出码
//
// prof 是使用场景（沿用 AOT 侧同一个档位定义）。它**只影响环境设定**，
// 不影响语言语义 —— 同一个程序在任何档位下必须算出同样的结果。
// 当前它做的唯一一件事是 service 档把 stderr 关掉缓冲（见 interp.c）。
// 传 NULL 等价于 console 档。
int run_bytecode(Bytecode* bc, const XProfileDef* prof);

// 反汇编（调试用，也用于差分测试的比对基准）
void bc_disasm(Bytecode* bc, FILE* out);

// 程序是否用到任一窗口内建。
//
// 为什么这是公共能力而不是 codegen 的私有细节：有两处要用同一份答案 ——
// 代码生成（决定内不内嵌窗口运行时）与参数检查（--profile 的语义告警）。
// 各写一份必然漂移，而漂移的症状极难查：「警告说没用到窗口，产物里
// 却内嵌了 500 KB GDI 代码」。
bool bc_uses_gui(Bytecode* bc);

// 程序是否用到 TCP。
//
// 与 bc_uses_gui 分开：AOT 产物按需链接系统库，用到 TCP 要
// -lws2_32，用到窗口要 -lgdi32/-luser32，两者互不包含。
// 详细理由见 bytecode.c 的实现注释。
bool bc_uses_net(Bytecode* bc);

// 程序是否会往 stdout 写东西（即是否含 BC_PRINT）。
//
// 用途见 main.c 的 check_profile_fit：desktop 档的产物双击启动时
// stdout 句柄为 NULL，println 会静默丢输出而退出码仍是 0。
// 「不开窗 + 有打印 + 选了 desktop 档」几乎必然是误用。
bool bc_has_print(Bytecode* bc);

// .xbc 字节码文件的读写（02 文档 §2「两档共享同一份字节码」的落地）
bool xbc_write(const char* path, Bytecode* bc, const char* src_file);
bool xbc_read(const char* path, Bytecode* out);

// ============================================================
// C 后端（AOT）
// ============================================================
//
// 两条路径：
//   codegen_c_emit  — 只产 C 代码（--emit=c），调试代码生成器用，
//                     也是「拿不到交叉工具链时」的逃生口
//   codegen_c_build — 产 C 代码再调编译器出原生可执行文件（--emit=exe）
//
// 为什么 AOT 走 C 而不是直接发机器码：见 codegen_c.c 头部。
// 一句话是「不自研汇编器与链接器」（02 §5.3 的裁决）。
//
// 两档的语义一致性是硬要求：同一个 .xbc，解释执行与 AOT 产物的
// stdout 必须逐字节相同，错误信息也必须相同（包括位置与文案）。
// 这条由 tools/run_tests.sh 对每个测试强制验证。
//
// 最后一个参数是使用场景（见 profile.h）。传 NULL 等价于 console 档，
// 这样不需要场景的调用点（--emit=c、测试里的直接调用）不必改动。

int codegen_c_emit(Bytecode* bc, const char* out_path, const XProfileDef* prof);
int codegen_c_build(Bytecode* bc, const char* src_path, const char* out_path,
                    const XTarget* tgt, const char* cc_override,
                    const XProfileDef* prof);

// 读文件
char* read_file(const char* path, size_t* out_len);

// x_fopen / x_remove / x_rename 的声明已下沉到 platform.h：
// 它们是平台边界的能力，不是编译器内部的数据结构操作。

#endif // X_H