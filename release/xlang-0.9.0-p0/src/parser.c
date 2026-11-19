// parser.c — 递归下降语法分析（P0）
//
// 覆盖 01-语言设计.md 的 P0 语法子集：
//   fn / let / var / if-else / while / return / 算术 / 比较 / 逻辑 /
//   函数调用 / 字符串 / 布尔 / 整数 / 浮点 / 赋值
//
// 运算符优先级（从低到高），与 01 §3.2 一致：
//   1. 赋值        =  +=  -=  *=  /=  %=      右结合
//   2. 逻辑或      or   ||                     左结合
//   3. 逻辑与      &&                          左结合
//   4. 相等        ==  !=                       左结合
//   5. 关系        <  <=  >  >=                左结合
//   6. 位或        |
//   7. 位异或      ^
//   8. 位与        &
//   9. 移位        <<  >>
//  10. 加减        +  -
//  11. 乘除模      *  /  %
//  12. 一元        -  not  !  ~
//  13. 后缀        ()  []  .
//
// 注意 01 §12：不引入三元 ?:（if 是表达式），不引入运算符自定义。

#include "x.h"

typedef struct {
    Token*  toks;
    size_t  count;
    size_t  pos;
    Arena*  arena;
    const char* file;
    // 出错恢复：一旦有过错误，继续解析但不再报错轰炸
    int     errors_this_stmt;
    // import 语句收集到的路径（P0 的模块解析在 main.c 里做）
    char**  imports;
    int     import_count;
    int     import_cap;
    // 与 imports 平行的行号数组。诊断要指到那句 import 上，
    // 见 x.h 里 Module.import_lines 的说明。
    int*    import_lines;
} Parser;

// ============================================================
// 基础
// ============================================================

static Token* peek(Parser* p) {
    if (p->pos >= p->count) return &p->toks[p->count - 1];
    return &p->toks[p->pos];
}

static Token* peek_ahead(Parser* p, int n) {
    size_t i = p->pos + n;
    if (i >= p->count) return &p->toks[p->count - 1];
    return &p->toks[i];
}

static bool check(Parser* p, TokKind k) { return peek(p)->kind == k; }

static Token* advance(Parser* p) {
    Token* t = peek(p);
    if (p->pos < p->count - 1) p->pos++;
    return t;
}

static bool match(Parser* p, TokKind k) {
    if (check(p, k)) { advance(p); return true; }
    return false;
}

static Token* expect(Parser* p, TokKind k, const char* ctx) {
    if (check(p, k)) return advance(p);
    Token* t = peek(p);
    if (ctx) {
        diag_error(t->pos, "期望 `%s`（%s），实际读到 `%s`",
                   tok_name(k), ctx,
                   t->kind == TK_EOF ? "文件结束" : tok_name(t->kind));
    diag_help("先补上缺失的 `%s` 再继续", tok_name(k));
    } else {
        diag_error(t->pos, "期望 `%s`，实际读到 `%s`",
                   tok_name(k),
                   t->kind == TK_EOF ? "文件结束" : tok_name(t->kind));
    diag_help("先补上缺失的 `%s` 再继续", tok_name(k));
    }
    // 不消费 token，让上层决定是否恢复
    return t;
}

static char* arena_dup(Parser* p, const char* s) {
    return arena_strdup(p->arena, s);
}

// ============================================================
// 表达式
// ============================================================

static Expr* parse_expr(Parser* p);
static Expr* parse_match(Parser* p);
static PatKind parse_pattern(Parser* p, MatchArm* arm);
static Expr* lower_interp(Parser* p, Token* t, SrcPos pos);
static Expr* parse_coalesce(Parser* p);
static Expr* parse_ternary(Parser* p);
static Expr* parse_block_expr(Parser* p);
static Expr* parse_unary(Parser* p);
static Stmt* parse_stmt(Parser* p);
static char* parse_type_name(Parser* p);

static Expr* new_expr(Parser* p, ExprKind k, SrcPos pos) {
    Expr* e = arena_alloc(p->arena, sizeof(Expr));
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->pos  = pos;
    return e;
}

static Stmt* new_stmt(Parser* p, StmtKind k, SrcPos pos) {
    Stmt* s = arena_alloc(p->arena, sizeof(Stmt));
    memset(s, 0, sizeof(*s));
    s->kind = k;
    s->pos  = pos;
    return s;
}

// 结构体字面量。调用点已经用 check_struct_lit 确认过：
// 当前 token 是 IDENT，下一个是 `{`。
//
// 语法：`Rect { x: 1, y: 2 }`
//
// ── 为什么不写成 `Rect(1, 2)`（位置参数）──────────────────
//
// 位置参数在矩形这种场景下是灾难：`Rect(0, 0, 800, 600)` 和
// `Rect(0, 0, 600, 800)` 编译期完全等价，但一个是横屏一个是竖屏。
// 而字段名让调用点自带文档，也让「少传一个」这种错误能在编译期
// 定位到具体缺哪个字段。
//
// 空字面量 `Rect {}` 是合法的 —— 字段全都有类型，可以用默认值
// （Int/Float → 0，Bool → false，String → ""，其余 → null）。
static Expr* parse_struct_lit(Parser* p) {
    SrcPos pos = peek(p)->pos;
    Token* tn  = peek(p);
    advance(p);   // 结构体名

    Expr* e = new_expr(p, EX_STRUCT_LIT, pos);
    e->slit.type_name = tn->s;

    expect(p, TK_LBRACE, "结构体字面量");

    char** names  = NULL;
    Expr** vals   = NULL;
    int count = 0, cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Token* fn = peek(p);
        if (fn->kind != TK_IDENT) {
            diag_error(fn->pos, "结构体字面量里期望字段名，实际读到 `%s`",
                       tok_name(fn->kind));
            diag_help("写成 `类型名 { 字段: 值, ... }`，例如 `Rect { x: 0, y: 0, w: 10, h: 10 }`");
            break;
        }
        advance(p);
        expect(p, TK_COLON, "字段名与字段值之间的 `:`");

        Expr* v = parse_expr(p);

        if (count == cap) {
            cap = cap ? cap * 2 : 4;
            char** nn = arena_alloc(p->arena, cap * sizeof(char*));
            Expr** nv = arena_alloc(p->arena, cap * sizeof(Expr*));
            if (names) memcpy(nn, names, count * sizeof(char*));
            if (vals)  memcpy(nv, vals,  count * sizeof(Expr*));
            names = nn; vals = nv;
        }
        names[count] = fn->s;
        vals[count]  = v;
        count++;

        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RBRACE, "结构体字面量");

    e->slit.field_names  = names;
    e->slit.field_values = vals;
    e->slit.field_count  = count;
    return e;
}

// 结构体声明：`struct Rect { x: Int, y: Int }`
//
// 字段之间用逗号分隔，也允许换行分隔（与函数参数列表一致）——
// 逗号可选，因为字段比较多时末尾那个逗号写不写都自然。
static StructDecl* parse_struct(Parser* p) {
    SrcPos pos = peek(p)->pos;
    expect(p, TK_STRUCT, "结构体声明");

    Token* name = peek(p);
    if (name->kind != TK_IDENT) {
        diag_error(name->pos, "`struct` 之后期望结构体名，实际读到 `%s`",
                   tok_name(name->kind));
        diag_help("写成 `struct 名字 { 字段: 类型, ... }`");
        return NULL;
    }
    advance(p);

    StructDecl* d = arena_alloc(p->arena, sizeof(StructDecl));
    memset(d, 0, sizeof(*d));
    d->name = name->s;
    d->pos  = pos;

    expect(p, TK_LBRACE, "结构体声明体");

    StructField* fields = NULL;
    int count = 0, cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Token* fn = peek(p);
        if (fn->kind != TK_IDENT) {
            diag_error(fn->pos, "结构体里期望字段名，实际读到 `%s`",
                       tok_name(fn->kind));
            diag_help("每个字段写成 `名字: 类型`，例如 `struct Rect { x: Int, y: Int }`");
            break;
        }
        advance(p);

        char* ftype = NULL;
        if (match(p, TK_COLON)) {
            ftype = parse_type_name(p);
        } else {
            diag_error(fn->pos, "字段 `%s` 缺少类型注解", fn->s);
            diag_help("结构体字段**必须**写类型：`%s: Int`。"
                      "函数参数能靠推断，字段不行——"
                      "它要参与内存布局，没有类型就定不出字段宽度", fn->s);
        }

        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            StructField* nf = arena_alloc(p->arena, cap * sizeof(StructField));
            if (fields) memcpy(nf, fields, count * sizeof(StructField));
            fields = nf;
        }
        fields[count].name      = fn->s;
        fields[count].type_name = ftype;
        count++;

        if (!match(p, TK_COMMA)) {
            // 没有逗号：可能直接换行跟下一个字段，也可能已经 `}`。
            // 两种都接受，靠循环条件判断。
            if (!check(p, TK_RBRACE) && !check(p, TK_IDENT)) break;
        }
    }
    expect(p, TK_RBRACE, "结构体声明体");

    d->fields      = fields;
    d->field_count = count;

    if (count == 0) {
        diag_error(pos, "结构体 `%s` 没有任何字段", d->name);
        diag_help("空结构体在 P0.5 里没有用处（不占空间也没有行为）。"
                  "至少要有一个字段，或者删掉这个声明");
    }
    return d;
}

// ============================================================
// 类（v0.15）
// ============================================================
//
// 语法：
//
//   [abstract|final] class Name [extends Parent] [implements A, B] {
//       [private|protected|public] [final] field: Type
//       [private|protected|public] [static] fn name(params) -> Ret [override] { ... }
//       [private|protected|public] abstract fn name(params) -> Ret
//       constructor(params) { ... }
//   }
//
//   interface Name [extends A, B] {
//       fn name(params) -> Ret      // 无函数体 = 抽象
//   }
//
// ── 为什么构造函数单独一个关键字而不是 fn constructor ────────
//
// 因为它在语义上确实不是普通方法：不进虚表、不被继承覆写、
// 返回值是 self（不能写返回类型）、只能通过 `new` 调用。
// 用 `fn constructor` 会让用户以为它可以像别的方法一样被调用
// 或者被覆写，而这两种都是错的。
//
// ── 为什么 override 是**可选**的（不强制） ──────────────────
//
// Java 强制 @Override，C++ 不强制。这里不强制，但一旦写了就必须
// 真的覆写到东西 —— 也就是「写错了会报错，不写不报错」。
//
// 理由：强制会让人为了省事到处复制粘贴，反而降低信息量；而
// 「写了却没覆写到」几乎必然是打错了方法名（或者基类签名改了
// 而子类没跟上），这类错误必须在编译期抓到。

// 解析可见性修饰符。没有修饰符时返回 VIS_PUBLIC。
static Visibility parse_visibility(Parser* p) {
    if (match(p, TK_PRIVATE))   return VIS_PRIVATE;
    if (match(p, TK_PROTECTED)) return VIS_PROTECTED;
    if (match(p, TK_PUB))       return VIS_PUBLIC;
    return VIS_PUBLIC;
}

// ============================================================
// 注解（v0.16）
// ============================================================
//
// 语法：
//
//   @Name
//   @Name(1, "a", key = true)
//   @A @B(x)          —— 连续多个，等价于两次标注
//
// 可以标在**类**、**字段**、**方法**上。语义完全交给使用者，语言层
// 只做「解析 + 存元数据 + 提供查询」，理由见 x.h 里 Annotation 的注释。
//
// ── 为什么实参只收字面量 ────────────────────────────────────
//
// 注解在**编译期**就要变成元数据，还要能单独写进 .xbc 发给没有源码
// 的机器。支持 `@Size(n * 2)` 就必须在编译期求值 —— 那是 comptime
// 能力，当前版本没有。所以这里明确拒绝表达式，并给一条说清原因的
// 错误，而不是悄悄存一个运行时求值不了的 AST。
static char* anno_dup(Parser* p, const char* s) { return arena_dup(p, s); }

static char* anno_int_text(Parser* p, int64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return arena_dup(p, buf);
}

static char* anno_float_text(Parser* p, double v) {
    char buf[64];
    // %g 会给出最短可回读的表示；注解实参只需要「人和工具都看得懂」，
    // 不需要精确到最后一个 ulp —— 要精确值时用户写字符串。
    snprintf(buf, sizeof(buf), "%.17g", v);
    return arena_dup(p, buf);
}

// 解析一个注解实参的**字面量**值，返回它的字符串形式。
static char* parse_anno_literal(Parser* p, const char* anno) {
    Token* t = peek(p);
    switch (t->kind) {
        case TK_INT:
            advance(p);
            return anno_int_text(p, t->i);
        case TK_FLOAT:
            advance(p);
            return anno_float_text(p, t->f);
        case TK_STRING:
            advance(p);
            return anno_dup(p, t->s);
        case TK_TRUE:  advance(p); return anno_dup(p, "true");
        case TK_FALSE: advance(p); return anno_dup(p, "false");
        case TK_NULL:  advance(p); return anno_dup(p, "null");
        case TK_MINUS: {
            // 负数字面量。允许，因为 `@Min(-1)` 是很自然的写法，而
            // 「负数要写成表达式」会让人以为注解不支持负数。
            advance(p);
            Token* n = peek(p);
            if (n->kind == TK_INT) {
                advance(p);
                return anno_int_text(p, -n->i);
            }
            if (n->kind == TK_FLOAT) {
                advance(p);
                return anno_float_text(p, -n->f);
            }
            diag_error(n->pos, "注解 `%s` 的实参里 `-` 之后期望数字", anno);
            return anno_dup(p, "");
        }
        default:
            diag_error(t->pos,
                       "注解 `%s` 的实参只能是字面量"
                       "（Int / Float / String / Bool / null），实际读到 `%s`",
                       anno, tok_name(t->kind));
            diag_help("注解在编译期就变成元数据，还要能写进 .xbc 发给"
                      "没有源码的机器；表达式求值属于 comptime 能力，"
                      "当前版本还没有。需要动态值请改用字符串");
            return anno_dup(p, "");
    }
}

// 解析连续的注解，返回一组。没有 `@` 时返回 count == 0 的空组。
static AnnotationSet parse_annotations(Parser* p) {
    AnnotationSet set;
    memset(&set, 0, sizeof(set));
    int cap = 0;

    while (check(p, TK_AT)) {
        advance(p);   // 消费 @
        Token* nt = peek(p);
        if (nt->kind != TK_IDENT) {
            diag_error(nt->pos, "`@` 之后期望注解名，实际读到 `%s`",
                       tok_name(nt->kind));
            diag_help("写法是 `@Deprecated` 或 `@Route(\"/api\")`");
            break;
        }
        advance(p);

        Annotation a;
        memset(&a, 0, sizeof(a));
        a.name = nt->s;
        a.pos  = nt->pos;

        if (match(p, TK_LPAREN)) {
            int acap = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    // 具名实参：`key = 值`。判据是「IDENT 后面紧跟 =」，
                    // 因为 `@Foo(a)` 里 a 是一个普通字面量吗？不是 ——
                    // IDENT 不是字面量，会走下面的字面量解析并报错。
                    char* key = NULL;
                    Token* t = peek(p);
                    if (t->kind == TK_IDENT) {
                        Token* eq = peek_ahead(p, 1);
                        if (eq && eq->kind == TK_ASSIGN) {
                            key = t->s;
                            advance(p);   // 名字
                            advance(p);   // =
                        }
                    }
                    char* val = parse_anno_literal(p, a.name);

                    if (a.arg_count == acap) {
                        acap = acap ? acap * 2 : 4;
                        char** nk = arena_alloc(p->arena, (size_t)acap * sizeof(char*));
                        char** nv = arena_alloc(p->arena, (size_t)acap * sizeof(char*));
                        if (a.arg_keys) memcpy(nk, a.arg_keys,
                                               (size_t)a.arg_count * sizeof(char*));
                        if (a.arg_vals) memcpy(nv, a.arg_vals,
                                               (size_t)a.arg_count * sizeof(char*));
                        a.arg_keys = nk;
                        a.arg_vals = nv;
                    }
                    a.arg_keys[a.arg_count] = key;
                    a.arg_vals[a.arg_count] = val;
                    a.arg_count++;

                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "注解实参列表");
        }

        if (set.count == cap) {
            cap = cap ? cap * 2 : 4;
            Annotation* ni = arena_alloc(p->arena, (size_t)cap * sizeof(Annotation));
            if (set.items) memcpy(ni, set.items,
                                  (size_t)set.count * sizeof(Annotation));
            set.items = ni;
        }
        set.items[set.count++] = a;
    }
    return set;
}

// 该类成员是否是 static（`static` 不是保留字，靠文本识别）。
static bool parse_is_static(Parser* p) {
    Token* t = peek(p);
    if (t->kind == TK_IDENT && strcmp(t->s, "static") == 0) {
        advance(p);
        return true;
    }
    return false;
}

// 解析类体
static void parse_class_body(Parser* p, ClassDecl* d, bool is_interface) {
    expect(p, TK_LBRACE, "类体");

    ClassField*  fields  = NULL;  int fcount = 0, fcap = 0;
    ClassMethod* meths   = NULL;  int mcount = 0, mcap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        SrcPos mpos = peek(p)->pos;

        // 注解在最前：它修饰的是**紧跟其后的那个成员**。
        //
        // 放在修饰符之前而不是之后，因为 `@Deprecated private x: Int`
        // 读起来是「这是一个已废弃的私有字段」，注解描述整体；而
        // `private @Deprecated x` 会让人以为注解只作用于类型。
        // Java/C# 都采用前置，跟它们一致。
        AnnotationSet annos = parse_annotations(p);

        Visibility vis = parse_visibility(p);
        bool is_final = match(p, TK_FINAL);
        bool is_abstract = match(p, TK_ABSTRACT);
        bool is_static = parse_is_static(p);
        bool is_override = match(p, TK_OVERRIDE);

        // ---- 构造函数 ----
        Token* t = peek(p);
        if (t->kind == TK_IDENT && strcmp(t->s, "constructor") == 0) {
            advance(p);
            if (is_static) {
                diag_error(t->pos, "构造函数不能是 static");
                diag_help("构造函数的作用是初始化新实例，必须有 self");
            }
            if (is_abstract) {
                diag_error(t->pos, "构造函数不能是 abstract");
                diag_help("抽象成员只能是没有函数体的方法；"
                          "构造函数必须能给出一份真实的初始化");
            }

            // 参数列表
            expect(p, TK_LPAREN, "构造函数参数列表");
            char** pnames = NULL; char** ptypes = NULL;
            int pcount = 0, pcap = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    Token* pn = peek(p);
                    if (pn->kind != TK_IDENT && pn->kind != TK_SELF) {
                        diag_error(pn->pos, "期望参数名，实际读到 `%s`",
                                   tok_name(pn->kind));
                        break;
                    }
                    char* pname = (pn->kind == TK_SELF) ? arena_dup(p, "self")
                                                        : pn->s;
                    advance(p);
                    char* ptype = NULL;
                    if (match(p, TK_COLON)) ptype = parse_type_name(p);
                    if (pcount == pcap) {
                        pcap = pcap ? pcap * 2 : 4;
                        char** nn = arena_alloc(p->arena, pcap * sizeof(char*));
                        char** nt = arena_alloc(p->arena, pcap * sizeof(char*));
                        if (pnames) memcpy(nn, pnames, pcount * sizeof(char*));
                        if (ptypes) memcpy(nt, ptypes, pcount * sizeof(char*));
                        pnames = nn; ptypes = nt;
                    }
                    pnames[pcount] = pname;
                    ptypes[pcount] = ptype;
                    pcount++;
                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "构造函数参数列表");

            if (!check(p, TK_LBRACE)) {
                diag_error(peek(p)->pos, "构造函数缺少函数体：期望 `{`，实际读到 `%s`",
                           tok_name(peek(p)->kind));
                break;
            }
            Expr* body_expr = parse_block_expr(p);

            // 合成一个 FuncDecl，名字是 `类名.constructor`，
            // 第一个形参自动插入 self。
            FuncDecl* f = arena_alloc(p->arena, sizeof(FuncDecl));
            memset(f, 0, sizeof(*f));
            size_t need = strlen(d->name) + sizeof(".constructor");
            char* fname = arena_alloc(p->arena, need);
            memcpy(fname, d->name, strlen(d->name));
            memcpy(fname + strlen(d->name), ".constructor", sizeof(".constructor"));
            f->name = fname;

            char** allnames = arena_alloc(p->arena, (size_t)(pcount + 1) * sizeof(char*));
            char** alltypes = arena_alloc(p->arena, (size_t)(pcount + 1) * sizeof(char*));
            allnames[0] = arena_dup(p, "self");
            alltypes[0] = d->name;
            for (int i = 0; i < pcount; i++) {
                allnames[i + 1] = pnames[i];
                alltypes[i + 1] = ptypes[i];
            }
            f->param_names = allnames;
            f->param_types = alltypes;
            f->param_count = pcount + 1;
            f->ret_type    = d->name;   // 构造函数返回自身类型
            f->pos         = mpos;

            Stmt* body = new_stmt(p, ST_BLOCK, body_expr->pos);
            body->block.stmts = body_expr->block.stmts;
            body->block.count = body_expr->block.count;
            f->body = body;

            if (mcount == mcap) {
                mcap = mcap ? mcap * 2 : 8;
                ClassMethod* nm = arena_alloc(p->arena,
                                              (size_t)mcap * sizeof(ClassMethod));
                if (meths) memcpy(nm, meths, (size_t)mcount * sizeof(ClassMethod));
                meths = nm;
            }
            ClassMethod* cm = &meths[mcount++];
            memset(cm, 0, sizeof(*cm));
            cm->name        = arena_dup(p, "constructor");
            cm->is_static   = false;
            cm->is_abstract = false;
            cm->is_override = false;
            cm->is_final    = true;
            cm->vis         = vis;
            cm->fn          = f;
            cm->pos         = mpos;
            cm->annos       = annos;
            continue;
        }

        // ---- 字段 ----
        if (t->kind == TK_IDENT && !check(p, TK_FN)) {
            // 往前看一个 token：`name :` 才是字段，`name (` 是别的东西
            Token* t1 = peek_ahead(p, 1);
            if (t1 && t1->kind == TK_COLON) {
                Token* fname_tok = t;
                advance(p);   // 名字
                advance(p);   // 冒号
                char* ftype = parse_type_name(p);
                if (!ftype) {
                    diag_error(fname_tok->pos, "字段 `%s` 缺少类型注解",
                               fname_tok->s);
                    diag_help("类字段必须写类型：`%s: Int`。"
                              "字段要参与对象布局，没有类型就定不出偏移",
                              fname_tok->s);
                }
                if (is_interface) {
                    diag_error(fname_tok->pos, "接口不能声明字段");
                    diag_help("接口只描述「能做什么」，不描述「有什么数据」。"
                              "把字段放到实现它的类里");
                }
                if (fcount == fcap) {
                    fcap = fcap ? fcap * 2 : 8;
                    ClassField* nf = arena_alloc(p->arena,
                                                 (size_t)fcap * sizeof(ClassField));
                    if (fields) memcpy(nf, fields, (size_t)fcount * sizeof(ClassField));
                    fields = nf;
                }
                ClassField* cf = &fields[fcount++];
                cf->name      = fname_tok->s;
                cf->type_name = ftype;
                cf->vis       = vis;
                cf->is_final  = is_final;
                cf->pos       = fname_tok->pos;
                cf->annos     = annos;
                match(p, TK_COMMA);
                continue;
            }
        }

        // ---- 方法 ----
        if (check(p, TK_FN)) {
            advance(p);   // fn
            Token* mname = peek(p);
            if (mname->kind != TK_IDENT) {
                diag_error(mname->pos, "`fn` 之后期望方法名，实际读到 `%s`",
                           tok_name(mname->kind));
                break;
            }
            advance(p);

            // 参数
            expect(p, TK_LPAREN, "方法参数列表");
            char** pnames = NULL; char** ptypes = NULL;
            int pcount = 0, pcap = 0;
            bool has_self = false;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    Token* pn = peek(p);
                    if (pn->kind != TK_IDENT && pn->kind != TK_SELF) {
                        diag_error(pn->pos, "期望参数名，实际读到 `%s`",
                                   tok_name(pn->kind));
                        break;
                    }
                    char* pname = (pn->kind == TK_SELF) ? arena_dup(p, "self")
                                                        : pn->s;
                    if (pn->kind == TK_SELF) has_self = true;
                    advance(p);
                    char* ptype = NULL;
                    if (match(p, TK_COLON)) ptype = parse_type_name(p);
                    if (pcount == pcap) {
                        pcap = pcap ? pcap * 2 : 4;
                        char** nn = arena_alloc(p->arena, (size_t)pcap * sizeof(char*));
                        char** nt = arena_alloc(p->arena, (size_t)pcap * sizeof(char*));
                        if (pnames) memcpy(nn, pnames, (size_t)pcount * sizeof(char*));
                        if (ptypes) memcpy(nt, ptypes, (size_t)pcount * sizeof(char*));
                        pnames = nn; ptypes = nt;
                    }
                    pnames[pcount] = pname;
                    ptypes[pcount] = ptype;
                    pcount++;
                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "方法参数列表");

            char* ret_type = NULL;
            if (match(p, TK_ARROW)) ret_type = parse_type_name(p);

            bool bodyless = is_abstract || is_interface;
            Stmt* body = NULL;

            if (!bodyless && !check(p, TK_LBRACE)) {
                diag_error(peek(p)->pos,
                           "方法 `%s` 缺少函数体：期望 `{`，实际读到 `%s`",
                           mname->s, tok_name(peek(p)->kind));
                diag_help("要么给出 `{ ... }` 函数体，"
                          "要么在类/方法上标 `abstract` 声明成抽象方法");
                break;
            }
            if (!bodyless) {
                Expr* body_expr = parse_block_expr(p);
                body = new_stmt(p, ST_BLOCK, body_expr->pos);
                body->block.stmts = body_expr->block.stmts;
                body->block.count = body_expr->block.count;
            } else {
                match(p, TK_SEMI);
            }

            // 合成 FuncDecl：实例方法自动插入 self，静态方法不插。
            FuncDecl* f = arena_alloc(p->arena, sizeof(FuncDecl));
            memset(f, 0, sizeof(*f));
            size_t need = strlen(d->name) + 1 + strlen(mname->s) + 1;
            char* fname = arena_alloc(p->arena, need);
            memcpy(fname, d->name, strlen(d->name));
            fname[strlen(d->name)] = '.';
            memcpy(fname + strlen(d->name) + 1, mname->s, strlen(mname->s));
            fname[need - 1] = '\0';
            f->name = fname;

            if (is_static || has_self) {
                f->param_names = pnames;
                f->param_types = ptypes;
                f->param_count = pcount;
            } else {
                // 自动插入 self
                char** allnames = arena_alloc(p->arena, (size_t)(pcount + 1) * sizeof(char*));
                char** alltypes = arena_alloc(p->arena, (size_t)(pcount + 1) * sizeof(char*));
                allnames[0] = arena_dup(p, "self");
                alltypes[0] = d->name;
                for (int i = 0; i < pcount; i++) {
                    allnames[i + 1] = pnames[i];
                    alltypes[i + 1] = ptypes[i];
                }
                f->param_names = allnames;
                f->param_types = alltypes;
                f->param_count = pcount + 1;
            }
            f->ret_type = ret_type;
            f->pos      = mpos;
            f->body     = body;

            if (mcount == mcap) {
                mcap = mcap ? mcap * 2 : 8;
                ClassMethod* nm = arena_alloc(p->arena,
                                              (size_t)mcap * sizeof(ClassMethod));
                if (meths) memcpy(nm, meths, (size_t)mcount * sizeof(ClassMethod));
                meths = nm;
            }
            ClassMethod* cm = &meths[mcount++];
            memset(cm, 0, sizeof(*cm));
            cm->name        = mname->s;
            cm->is_static   = is_static;
            cm->is_abstract = bodyless;
            cm->is_override = is_override;
            cm->is_final    = is_final;
            cm->vis         = vis;
            cm->fn          = f;
            cm->pos         = mpos;
            cm->annos       = annos;
            continue;
        }

        diag_error(t->pos, "类体里期望字段或方法，实际读到 `%s`",
                   tok_name(t->kind));
        diag_help("类成员只能是 `名字: 类型`（字段）、"
                  "`fn 名字(...) { }`（方法）、`constructor(...) { }`");
        break;
    }

    expect(p, TK_RBRACE, "类体");

    d->fields       = fields;
    d->field_count  = fcount;
    d->methods      = meths;
    d->method_count = mcount;
}

// 解析 class / interface 声明。
//
// 两个关键字的差别只在：interface 的所有方法都隐式抽象、不能有字段、
// 不能被实例化。共用同一个解析函数，靠 is_interface 分支。
static ClassDecl* parse_class(Parser* p, bool is_interface) {
    SrcPos pos = peek(p)->pos;
    bool is_abstract = false;
    bool is_final = false;

    if (is_interface) {
        expect(p, TK_INTERFACE, "接口声明");
    } else {
        // abstract / final 已在外层吃掉；这里再检查一次是为了
        // 支持 `class` 前面的修饰符（顶层循环会调到这里）。
        expect(p, TK_CLASS, "类声明");
    }

    Token* name = peek(p);
    if (name->kind != TK_IDENT) {
        diag_error(name->pos, "`%s` 之后期望名字，实际读到 `%s`",
                   is_interface ? "interface" : "class", tok_name(name->kind));
        diag_help("写成 `%s 名字 { ... }`",
                  is_interface ? "interface" : "class");
        return NULL;
    }
    advance(p);

    ClassDecl* d = arena_alloc(p->arena, sizeof(ClassDecl));
    memset(d, 0, sizeof(*d));
    d->name         = name->s;
    d->pos          = pos;
    d->is_interface = is_interface;
    d->is_abstract  = is_abstract;
    d->is_final     = is_final;

    // extends：类是单继承，接口是多继承（接口不做字段布局，所以安全）
    char** parents = NULL; int pcount = 0, pcap = 0;
    if (match(p, TK_EXTENDS)) {
        for (;;) {
            char* pn = parse_type_name(p);
            if (!pn) break;
            if (pcount == pcap) {
                pcap = pcap ? pcap * 2 : 4;
                char** np = arena_alloc(p->arena, (size_t)pcap * sizeof(char*));
                if (parents) memcpy(np, parents, (size_t)pcount * sizeof(char*));
                parents = np;
            }
            parents[pcount++] = pn;
            if (!match(p, TK_COMMA)) break;
        }
    }

    if (is_interface) {
        // 接口的 extends 列表全是接口，允许多个
        d->iface_names = parents;
        d->iface_count = pcount;
    } else {
        if (pcount > 1) {
            diag_error(pos, "类 `%s` 只能继承一个父类（实际写了 %d 个）",
                       d->name, pcount);
            diag_help("需要多重能力时用 `implements` 接多个接口 —— "
                      "接口只描述行为，不占对象布局，所以可以多个；"
                      "类有字段布局，多继承会带来菱形二义性");
        }
        d->parent_name = pcount > 0 ? parents[0] : NULL;
    }

    // implements
    if (!is_interface && match(p, TK_IMPLEMENTS)) {
        for (;;) {
            char* pn = parse_type_name(p);
            if (!pn) break;
            if (pcount == pcap) {
                pcap = pcap ? pcap * 2 : 4;
                char** np = arena_alloc(p->arena, (size_t)pcap * sizeof(char*));
                if (parents) memcpy(np, parents, (size_t)pcount * sizeof(char*));
                parents = np;
            }
            parents[pcount++] = pn;
            if (!match(p, TK_COMMA)) break;
        }
    }

    // 重新整理：extends 至多 1 个，其余都是 implements。
    // 上面为了共用数组把两者混在一起了，这里按「有没有 extends 过」
    // 来切分，避免依赖计数细节。
    //
    // 说明：这是实现上的取舍 —— 用两个数组会更清晰，但 arena 分配
    // 在这条路径上不是瓶颈，而两段几乎相同的收集循环是复制粘贴的
    // 温床。合并收集 + 事后切分让「读一遍就够」。
    if (!is_interface) {
        int impl_start = d->parent_name ? 1 : 0;
        if (pcount > impl_start) {
            int n = pcount - impl_start;
            char** impl = arena_alloc(p->arena, (size_t)n * sizeof(char*));
            for (int i = 0; i < n; i++) impl[i] = parents[impl_start + i];
            d->iface_names = impl;
            d->iface_count = n;
        }
    }

    if (is_interface) {
        // 接口体里只有方法
        expect(p, TK_LBRACE, "接口体");
        ClassMethod* meths = NULL; int mcount = 0, mcap = 0;
        while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
            SrcPos mpos = peek(p)->pos;
            Visibility vis = parse_visibility(p);
            match(p, TK_FINAL);
            match(p, TK_ABSTRACT);
            bool is_static = parse_is_static(p);
            match(p, TK_OVERRIDE);

            if (!check(p, TK_FN)) {
                diag_error(peek(p)->pos, "接口体里只允许方法声明，实际读到 `%s`",
                           tok_name(peek(p)->kind));
                diag_help("接口描述「能做什么」，不描述数据。"
                          "字段放到实现它的类里");
                break;
            }
            advance(p);
            Token* mname = peek(p);
            if (mname->kind != TK_IDENT) {
                diag_error(mname->pos, "`fn` 之后期望方法名，实际读到 `%s`",
                           tok_name(mname->kind));
                break;
            }
            advance(p);

            expect(p, TK_LPAREN, "接口方法参数列表");
            char** pnames = NULL; char** ptypes = NULL;
            int pcount2 = 0, pcap2 = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    Token* pn = peek(p);
                    if (pn->kind != TK_IDENT && pn->kind != TK_SELF) {
                        diag_error(pn->pos, "期望参数名，实际读到 `%s`",
                                   tok_name(pn->kind));
                        break;
                    }
                    char* pname = (pn->kind == TK_SELF) ? arena_dup(p, "self")
                                                        : pn->s;
                    advance(p);
                    char* ptype = NULL;
                    if (match(p, TK_COLON)) ptype = parse_type_name(p);
                    if (pcount2 == pcap2) {
                        pcap2 = pcap2 ? pcap2 * 2 : 4;
                        char** nn = arena_alloc(p->arena, (size_t)pcap2 * sizeof(char*));
                        char** nt = arena_alloc(p->arena, (size_t)pcap2 * sizeof(char*));
                        if (pnames) memcpy(nn, pnames, (size_t)pcount2 * sizeof(char*));
                        if (ptypes) memcpy(nt, ptypes, (size_t)pcount2 * sizeof(char*));
                        pnames = nn; ptypes = nt;
                    }
                    pnames[pcount2] = pname;
                    ptypes[pcount2] = ptype;
                    pcount2++;
                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "接口方法参数列表");

            char* ret_type = NULL;
            if (match(p, TK_ARROW)) ret_type = parse_type_name(p);
            match(p, TK_SEMI);

            FuncDecl* f = arena_alloc(p->arena, sizeof(FuncDecl));
            memset(f, 0, sizeof(*f));
            size_t need = strlen(d->name) + 1 + strlen(mname->s) + 1;
            char* fname = arena_alloc(p->arena, need);
            memcpy(fname, d->name, strlen(d->name));
            fname[strlen(d->name)] = '.';
            memcpy(fname + strlen(d->name) + 1, mname->s, strlen(mname->s));
            fname[need - 1] = '\0';
            f->name = fname;

            char** allnames = arena_alloc(p->arena, (size_t)(pcount2 + 1) * sizeof(char*));
            char** alltypes = arena_alloc(p->arena, (size_t)(pcount2 + 1) * sizeof(char*));
            allnames[0] = arena_dup(p, "self");
            alltypes[0] = d->name;
            for (int i = 0; i < pcount2; i++) {
                allnames[i + 1] = pnames[i];
                alltypes[i + 1] = ptypes[i];
            }
            f->param_names = allnames;
            f->param_types = alltypes;
            f->param_count = pcount2 + 1;
            f->ret_type    = ret_type;
            f->pos         = mpos;
            f->body        = NULL;

            if (mcount == mcap) {
                mcap = mcap ? mcap * 2 : 8;
                ClassMethod* nm = arena_alloc(p->arena,
                                              (size_t)mcap * sizeof(ClassMethod));
                if (meths) memcpy(nm, meths, (size_t)mcount * sizeof(ClassMethod));
                meths = nm;
            }
            ClassMethod* cm = &meths[mcount++];
            memset(cm, 0, sizeof(*cm));
            cm->name        = mname->s;
            cm->is_static   = is_static;
            cm->is_abstract = true;    // 接口方法必然抽象
            cm->is_override = false;
            cm->is_final    = false;
            cm->vis         = vis;
            cm->fn          = f;
            cm->pos         = mpos;
        }
        expect(p, TK_RBRACE, "接口体");
        d->methods      = meths;
        d->method_count = mcount;
    } else {
        parse_class_body(p, d, false);
    }

    return d;
}

// 基础表达式（字面量、标识符、括号、if 表达式、块）
// 把一段原始文本 unescape 成字面量，并折叠 `}}` → `}`。
//
// `}}` 不能交给 x_unescape_string：那里只认反斜杠转义，`}}` 是两个
// 普通字符。而规范要求 `"}}"` 与 `"{{"` 对称地输出单个花括号。
// 所以在这一层做折叠 —— 它只作用于**字面量段**，插值表达式内部
// 的 `}}` 属于内层语法，不受影响。
static Expr* make_literal_seg(Parser* p, const char* raw, size_t n,
                              SrcPos pos) {
    // 先折叠 } } → }
    char* folded = arena_alloc(p->arena, n + 1);
    size_t fn = 0;
    for (size_t k = 0; k < n; k++) {
        if (raw[k] == '}' && k + 1 < n && raw[k + 1] == '}') {
            folded[fn++] = '}';
            k++;
        } else {
            folded[fn++] = raw[k];
        }
    }
    folded[fn] = '\0';

    size_t out = 0;
    char* s = x_unescape_string(p->arena, folded, fn, &out, pos);
    Expr* lit = new_expr(p, EX_STRING, pos);
    lit->str.s   = s;
    lit->str.len = out;
    return lit;
}

// 把 `rhs` 用 `+` 拼到 `lhs` 后面（左倾链）。lhs 为 NULL 时直接返回 rhs。
// 插值降级专用。
static Expr* append_concat(Parser* p, Expr* lhs, Expr* rhs, SrcPos pos) {
    if (!lhs) return rhs;
    Expr* e = new_expr(p, EX_BINARY, pos);
    e->binary.op  = OP_ADD;
    e->binary.lhs = lhs;
    e->binary.rhs = rhs;
    return e;
}

// ── 字符串插值 `"a{x}b"` → `"a" + (x) + "b"` ──────────────
//
// 规范 01 §3.6 已定义该语法，这里在 **parser 侧降级**，不加新字节码
// 也不加新节点：结果就是普通的 `+` 拼接链，`+` 对 String 与任意
// 右操作数已有「转字符串再拼」的语义（见 compile 的字符串加法分支）。
//
// ── 为什么要看 raw 而不是已 unescape 的 s ────────────────
//
// `"{{"` 是转义的 `{`，`"{x}"` 是插值，而 unescape 之后两种都是裸 `{`，
// 已经无法区分。raw 保留着 `\u{48}` 里的反斜杠，所以扫 raw 才能
// 正确跳过转义序列里的花括号。
//
// ── 表达式怎么解析 ──────────────────────────────────────
//
// `{...}` 里的内容是**另一段源码**，不在当前 token 流里。做法是：把
// 这段子串交给一个独立的 lexer 切词，再用一个临时 Parser 解析成表达式。
// 这样 `{a + b}` / `{f(1)}` / `{obj.x}` 全都能用，而不必手写一个
// 「表达式迷你解析器」——那才是真正会走样的做法。
static Expr* lower_interp(Parser* p, Token* t, SrcPos pos) {
    const char* raw = t->raw;
    size_t raw_len  = t->raw_len;
    size_t i = 0, seg_start = 0;

    Expr* chain = NULL;          // 已拼好的部分
    bool  saw_any = false;

    // 追加一段字面量。空段跳过 —— 否则每段间会多出一个 `+ ""`。
    #define FLUSH_LITERAL(upto)                                              \
        do {                                                                 \
            size_t _n = (upto) - seg_start;                                   \
            if (_n > 0) {                                                     \
                Expr* lit = make_literal_seg(p, raw + seg_start, _n, pos);    \
                chain = append_concat(p, chain, lit, pos);                    \
                saw_any = true;                                               \
            }                                                                 \
        } while (0)

    while (i < raw_len) {
        char c = raw[i];

        // 转义序列整体跳过，免得 `\{` / `\u{48}` 里的花括号被当成插值
        if (c == '\\') {
            i += 2;
            if (i > raw_len) i = raw_len;
            // `\u{XXXX}`：跳过整段
            if (i - 1 < raw_len && raw[i - 1] == 'u' && i < raw_len &&
                raw[i] == '{') {
                i++;
                while (i < raw_len && raw[i] != '}') i++;
                if (i < raw_len) i++;   // 吃掉 `}`
            }
            continue;
        }

        if (c != '{') { i++; continue; }

        // `{{` → 字面量 `{`。注意这**不能**只是「跳过」，否则
        // `"{{x}}"` 会被原样输出成 `{{x}}`，而规范要求 `{x}`。
        // 做法：把这一段（含 `{{`）落地成字面量，但落地前把 `{{`
        // 折成 `{` —— 靠下面这段「抽出一份改写的拷贝」实现。
        if (i + 1 < raw_len && raw[i + 1] == '{') {
            // 落地 [seg_start, i)，再单独压一个字面量 `{`
            FLUSH_LITERAL(i);
            Expr* lit = new_expr(p, EX_STRING, pos);
            lit->str.s   = "{";
            lit->str.len = 1;
            chain = append_concat(p, chain, lit, pos);
            saw_any = true;
            i += 2;
            seg_start = i;
            continue;
        }

        // 找到配对的 `}`。允许嵌套花括号（结构体字面量 `{a: 1}`）。
        size_t j = i + 1;
        int depth = 1;
        while (j < raw_len && depth > 0) {
            char d = raw[j];
            if (d == '\\') { j += 2; continue; }
            if (d == '{') depth++;
            else if (d == '}') depth--;
            if (depth == 0) break;
            j++;
        }
        if (depth != 0) {
            // 未配对的 `{`：按方案一（v0.17 裁决）**不报错**，当字面量。
            //
            // 理由：`"a{b"` 这种在正则、模板片段、用户数据里都可能出现。
            // 若报错，任何含单个未闭合花括号的字符串就编译不过 ——
            // 而插值只是字符串的一个可选用法，不该对不用的写法收税。
            //
            // 代价：真正的插值写漏了 `}` 时，不会报「插值未闭合」，
            // 而是原样输出 `{b`。这一条与「写错会静默」是同一笔交易，
            // 在 3.6 的实现状态表和测试里都写明了。
            break;   // 跳出扫描，剩下的部分整体当字面量
        }

        // ── 判定「这到底是不是插值」──────────────────────────────
        //
        // 这里是本实现唯一一处「猜错了就静默」的地方，所以要说清判据。
        //
        // 问题：`"{}"` 和 `"{\"a\":1}"` 是 JSON 字符串里极常见的写法，
        // 而它们长得和插值一模一样。若一律当插值，所有 JSON 代码立刻
        // 编译不过 —— 这是不可接受的破坏性变更（实测 23-stdlib-json
        // 就因此失败）。
        //
        // 判据（按顺序，任一成立就不当插值）：
        //   1. 内容为空：`{}` 原样输出
        //   2. 内容解析后没有吃完全部 token（有剩余）：`{"a":1}` 里的
        //      引号+冒号在表达式语法里不成立，会剩 `:`
        //   3. 内容解析过程中报过错：子解析器已经 diag_error 了
        //
        // 代价：真正的插值写错时（比如 `{a b}`）会静默退回字面量，而不是
        // 报错。这个代价是刻意接受的 —— 见下面 side 分支的注释。
        //
        // ── 为什么用「试探」而不是「先看首字符是不是标识符」────
        //
        // 首字符判据挡不住 `{a: 1}`（结构体字面量，首字符是标识符但
        // 有冒号）、也挡不住 `{"a":1}`（首字符是引号）。而试探是
        // 「能不能真解析成一个完整表达式」，那正是插值的定义。
        size_t elen = j - (i + 1);
        bool is_interp = false;
        Expr* inner = NULL;

        if (elen > 0) {
            Arena* a2 = p->arena;

            // ── 先把 `\"` 还原成裸 `"` ──────────────────────
            //
            // 外层 lex_string 把整段字符串当「一串字符」扫，`\"` 只是
            // 一个被跳过的转义；但插值内容要重新走一遍 **lexer**，
            // 而 lexer 不认「前面没有开引号的 `\"`」，会报
            // 「无法识别的字符 `\`」。
            //
            // 所以这里是两个理解层之间的接缝：插值表达式里的字符串
            // 定界符在外层必须写成 `\"`，在内层必须是裸 `"`。
            // 只转这两种（`\"` 与 `\'`），其余转义留给内层 lexer
            // 自己处理 —— 它们在内层含义相同。
            char* src = arena_alloc(a2, elen + 1);
            size_t sn = 0;
            for (size_t k = 0; k < elen; k++) {
                if (raw[i + 1 + k] == '\\' && k + 1 < elen &&
                    (raw[i + 2 + k] == '"' || raw[i + 2 + k] == '\'')) {
                    src[sn++] = raw[i + 2 + k];
                    k++;                    // 跳过那两个字符
                } else {
                    src[sn++] = raw[i + 1 + k];
                }
            }
            src[sn] = '\0';

            Lexer lx2;
            lexer_init(&lx2, p->file, src, sn, a2);

            // 切完全部 token
            size_t cap = 16, n = 0;
            Token* toks = arena_alloc(a2, sizeof(Token) * cap);
            for (;;) {
                Token tk = lexer_next(&lx2);
                if (n + 1 >= cap) {
                    size_t nc = cap * 2;
                    Token* nt = arena_alloc(a2, sizeof(Token) * nc);
                    memcpy(nt, toks, sizeof(Token) * n);
                    toks = nt;
                    cap = nc;
                }
                toks[n++] = tk;
                if (tk.kind == TK_EOF) break;
            }

            // 子解析：复用当前 Parser 结构，只换 token 流。
            Parser sub;
            memset(&sub, 0, sizeof(sub));
            sub.toks  = toks;
            sub.count = n;
            sub.pos   = 0;
            sub.arena = p->arena;
            sub.file  = p->file;

            // ── 试探解析：这段到底能不能当表达式 ──────────────────
            //
            // 全程静音（g_diag_silent）。理由见 x.h 里那段说明：
            // `"{\"a\":1}"` 是合法 JSON，试探失败是**预期内**的，
            // 不能让它附赠一条「期望一个表达式」的报错。
            //
            // sub.errors_this_stmt 也只能靠静音后重算：子解析器在
            // 静音期间报的错只进 g_silent_errors，不进 g_error_count。
            int saved_silent = g_diag_silent;
            int saved_silent_err = g_silent_errors;
            g_diag_silent = 1;
            g_silent_errors = 0;

            Expr* try_inner = parse_expr(&sub);

            int trial_errors = g_silent_errors;
            g_silent_errors = saved_silent_err;
            g_diag_silent = saved_silent;

            bool consumed_all = (sub.pos >= sub.count ||
                                 sub.toks[sub.pos].kind == TK_EOF);

            if (try_inner && consumed_all && trial_errors == 0) {
                is_interp = true;
                inner = try_inner;
            }
            // 失败时不报错、不回滚任何东西 —— 因为什么都没发射过。
            // 这一段会被当作普通字面量的一部分（seg_start 不动）。
        }

        if (is_interp) {
            // 先落地 `{` 之前的字面量，再接上表达式
            FLUSH_LITERAL(i);
            chain = append_concat(p, chain, inner, pos);
            saw_any = true;
            i = j + 1;
            seg_start = i;
        } else {
            // 不是插值：把这个 `{` 当普通字符，继续往后扫。
            // 不推进 seg_start，让这段连同后面的 `}` 一起落地成字面量。
            i++;
        }
    }

    // ── 收尾：最后一段字面量 ──────────────────────────────────
    //
    // 这一步**必须先判 saw_any**，不能无条件 FLUSH_LITERAL。
    //
    // 原来这里是无条件收尾的，结果 `saw_any` 被强行置成 true，下面
    // 「整串都不是插值」的回退分支永远进不去，于是 make_literal_seg
    // 的 `}}` 折叠被无差别施加到了**所有**字符串上。
    //
    // 真实症状（这个 bug 卡了整整一轮 ES 驱动）：
    //
    //     let f = "{\"a\":{\"b\":1}}"
    //     len(f)  // 13 是对的，实际拿到 12
    //
    // 末尾那个 `}` 被 `}}` → `}` 吃掉了。而且不报错 —— 只能靠
    // len 或 jsonParse 的「期望 , 或 }」间接发现，排查成本极高。
    // 嵌套 JSON、正则、任何以 `}}` 结尾的文本全中招。
    //
    // ── 折中裁决 ─────────────────────────────────────────────
    //
    // `}}` 折叠只在**这个字符串确实走了插值路径**时启用，即至少识别出
    // 一个 `{expr}` 或一对 `{{`。判据就是 saw_any。
    //
    //   · `"{\"a\":{\"b\":1}}"` —— 没有插值也没有 `{{`，saw_any 为 false
    //     → 原样返回 t->s，13 个字符，`}}` 不动。
    //   · `"{{x}}"` —— 命中 `{{`，saw_any 为 true → 折叠生效，
    //     输出 `{x}`，符合规范 01 §3.6。
    //   · `"x{a}}"` —— 命中插值 → 折叠生效，符合规范。
    //
    // 代价（刻意接受并已写进规范 3.6.3）：纯字面量的 `"a}}b"` 现在输出
    // 两个花括号，不再是 `a}b`。这与「转义必须成对」的直觉一致 ——
    // 单独一个 `}}` 本来就没有对应的 `{{` 可配对。而收益是整类
    // 「含 `}}` 的 JSON / 正则 / 模板必须手工绕开」的坑彻底消失。
    if (saw_any) {
        FLUSH_LITERAL(raw_len);
    }
    #undef FLUSH_LITERAL

    if (!saw_any || !chain) {
        // 整串都不是插值 —— 直接返回 lexer 已经 unescape 好的原文。
        //
        // 注意这里用 t->s / t->len 而不是重新 unescape：lexer 那一遍
        // 已经把 `\n` / `\"` / `\u{XXXX}` 全部处理完了，再处理一次会把
        // `\\n`（本意是反斜杠加 n）变成换行。
        Expr* e = new_expr(p, EX_STRING, pos);
        e->str.s   = t->s;
        e->str.len = t->len;
        return e;
    }
    return chain;
}

static Expr* parse_primary(Parser* p) {
    Token* t = peek(p);
    SrcPos pos = t->pos;

    switch (t->kind) {
        case TK_INT: {
            advance(p);
            Expr* e = new_expr(p, EX_INT, pos);
            e->i.v = t->i;
            return e;
        }
        case TK_FLOAT: {
            advance(p);
            Expr* e = new_expr(p, EX_FLOAT, pos);
            e->f.v = t->f;
            return e;
        }
        case TK_STRING: {
            advance(p);
            // 含 `{expr}` 时拆成拼接链，否则就是普通字符串字面量。
            // 判定看 raw（转义原文）—— 见 Token.raw 的注释。
            if (t->raw && strchr(t->raw, '{')) {
                return lower_interp(p, t, pos);
            }
            Expr* e = new_expr(p, EX_STRING, pos);
            e->str.s   = t->s;
            e->str.len = t->len;
            return e;
        }
        case TK_TRUE: {
            advance(p);
            Expr* e = new_expr(p, EX_BOOL, pos);
            e->b.v = true;
            return e;
        }
        case TK_FALSE: {
            advance(p);
            Expr* e = new_expr(p, EX_BOOL, pos);
            e->b.v = false;
            return e;
        }
        case TK_NULL: {
            advance(p);
            return new_expr(p, EX_NULL, pos);
        }
        // ---- new Foo(a, b) ----
        //
        // 只认 `new 类型名(` 这一种形式。不写括号的 `new Foo` 也不行：
        // 类的所有字段都由构造函数初始化，没有「零参构造」和
        // 「不构造」两种路径 —— 强制写括号让「这里造了个对象」
        // 在读代码时一眼可见。
        case TK_NEW: {
            advance(p);   // new
            Token* tn = peek(p);
            if (tn->kind != TK_IDENT) {
                diag_error(tn->pos, "`new` 之后期望类名，实际读到 `%s`",
                           tok_name(tn->kind));
                diag_help("写成 `new 类名(构造参数)`，如 `new Button(rect, \"确定\")`");
                return new_expr(p, EX_NULL, pos);
            }
            advance(p);

            Expr* e = new_expr(p, EX_NEW, pos);
            e->newx.type_name = tn->s;

            if (!check(p, TK_LPAREN)) {
                diag_error(tn->pos, "类名 `%s` 之后期望 `(`", tn->s);
                diag_help("即使构造函数没有参数也要写 `new %s()` —— "
                          "省略括号会让「造对象」和「取值」在语法上无法区分",
                          tn->s);
                e->newx.args = NULL;
                e->newx.argc = 0;
                return e;
            }
            advance(p);   // (

            Expr** args = NULL;
            int argc = 0, acap = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    Expr* a = parse_expr(p);
                    if (argc == acap) {
                        acap = acap ? acap * 2 : 4;
                        Expr** na = arena_alloc(p->arena,
                                                (size_t)acap * sizeof(Expr*));
                        if (args) memcpy(na, args, (size_t)argc * sizeof(Expr*));
                        args = na;
                    }
                    args[argc++] = a;
                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "构造参数列表");
            e->newx.args = args;
            e->newx.argc = argc;
            return e;
        }
        case TK_IDENT: {
            advance(p);
            // 结构体字面量 `Rect { x: 1 }`。
            //
            // ── 这个判据为什么必须是三 token 前瞻 ──────────────────
            //
            // 最初只查「标识符紧跟 `{`」，看起来足够 —— 因为条件表达式
            // 都带括号（`if (cond) {`、`while (cond) {`）。但实测立刻
            // 撞了两处真实回归：
            //
            //     for x in arr { ... }     ← 语法糖里 `arr` 直接跟 `{`
            //     while cond { ... }        ← 本文件允许省略括号的 while
            //
            // 两处的 `{` 都是**循环体**，而 `arr` / `cond` 被当成了
            // 结构体名，于是整段循环体被解析成字面量的字段列表，
            // 报出「期望 `:` 实际读到 `+=`」这种完全指错方向的错。
            //
            // 判据：`标识符 {` 之后必须紧跟 `标识符 :`。
            // 结构体字面量的第一个字段一定是 `名字:`，而循环体第一条
            // 语句几乎不可能是「裸标识符 后接冒号」—— 那在表达式位置
            // 就不合法。空字面量 `Rect {}` 单独放行（后面是 `}`）。
            //
            // ── 与 Rust 的取舍对比 ────────────────────────────────
            //
            // Rust 干脆禁止 `if x { }` 这种写法（强制括号），换来
            // 结构体字面量可以无歧义。本语言早期为了少打括号允许了
            // `while cond { }` 且已经在测试里用开，改语法等于破坏
            // 兼容；而三 token 前瞻的代价是一次数组下标，零风险。
            bool is_lit = false;
            if (check(p, TK_LBRACE)) {
                Token* t1 = peek_ahead(p, 1);
                Token* t2 = peek_ahead(p, 2);
                if (t1->kind == TK_RBRACE) {
                    is_lit = true;                       // `Rect {}`
                } else if (t1->kind == TK_IDENT && t2->kind == TK_COLON) {
                    is_lit = true;                       // `Rect { x: ...`
                }
            }
            if (is_lit) {
                p->pos--;                    // 退回标识符，交给字面量解析
                return parse_struct_lit(p);
            }
            Expr* e = new_expr(p, EX_IDENT, pos);
            e->ident.name = t->s;
            return e;
        }
        case TK_SELF: {
            advance(p);
            Expr* e = new_expr(p, EX_IDENT, pos);
            e->ident.name = arena_dup(p, "self");
            return e;
        }
        case TK_LPAREN: {
            advance(p);
            Expr* e = parse_expr(p);
            expect(p, TK_RPAREN, "括号表达式");
            return e;
        }
        case TK_IF: {
            // if 是表达式（01 §12 不引入三元运算符）
            advance(p);
            Expr* cond = parse_expr(p);
            // 注意：parse_block_expr 自己会消费 `{`，这里不能再 expect 一次
            Expr* then_block = parse_block_expr(p);
            Expr* else_expr = NULL;
            if (match(p, TK_ELSE)) {
                if (check(p, TK_IF)) {
                    else_expr = parse_primary(p);   // else if
                } else {
                    else_expr = parse_block_expr(p);
                }
            }
            Expr* e = new_expr(p, EX_IF, pos);
            e->ifx.cond       = cond;
            e->ifx.then_block = then_block;
            e->ifx.else_expr  = else_expr;
            return e;
        }
        case TK_LBRACKET: {
            // 数组字面量 `[a, b, c]`
            //
            // ── 为什么在 primary 里就认定是字面量 ────────────────
            //
            // `[` 只出现在两个语法位置：
            //   · 表达式**开头**  → 数组字面量（这里）
            //   · 表达式**之后**  → 索引 `a[i]`（parse_postfix 里）
            //
            // 位置本身就区分了它们，不需要回溯也不需要看后面。
            // 空字面量 `[]` 合法：允许 `var xs = []` 再循环 push，
            // 这是最常见的形态；不允许的话用户得先想一个占位填充值。
            advance(p);   // `[`

            Expr** elems = NULL;
            int count = 0, cap = 0;

            if (!check(p, TK_RBRACKET)) {
                for (;;) {
                    Expr* el = parse_expr(p);
                    if (count + 1 > cap) {
                        int nc = cap ? cap * 2 : 4;
                        Expr** ne = arena_alloc(p->arena, sizeof(Expr*) * (size_t)nc);
                        if (elems) memcpy(ne, elems, sizeof(Expr*) * (size_t)count);
                        elems = ne;
                        cap = nc;
                    }
                    elems[count++] = el;

                    if (!match(p, TK_COMMA)) break;
                    // 允许尾逗号 `[1, 2, 3,]` —— 多行书写时少一个
                    // 「删掉上一行末尾逗号」的动作，diff 也干净。
                    if (check(p, TK_RBRACKET)) break;
                }
            }
            expect(p, TK_RBRACKET, "数组字面量的 `]`");

            Expr* e = new_expr(p, EX_ARRAY_LIT, pos);
            e->arrlit.elems = elems;
            e->arrlit.count = count;
            return e;
        }
        case TK_LBRACE: {
            return parse_block_expr(p);
        }
        case TK_MATCH: {
            return parse_match(p);
        }
        case TK_MINUS: {
            advance(p);
            Expr* operand = parse_unary(p);
            Expr* e = new_expr(p, EX_UNARY, pos);
            e->unary.op = UOP_NEG;
            e->unary.operand = operand;
            return e;
        }
        default:
            break;
    }

    diag_error(pos, "期望一个表达式，实际读到 `%s`", tok_name(t->kind));
    diag_help("此处需要字面量、变量名、函数调用或带括号的表达式");
    // 生产一个占位整数字面量，避免上层到处判 NULL
    advance(p);
    Expr* e = new_expr(p, EX_INT, pos);
    e->i.v = 0;
    return e;
}

// 后缀：调用、索引、成员访问
static Expr* parse_postfix(Parser* p) {
    Expr* e = parse_primary(p);
    for (;;) {
        if (check(p, TK_LPAREN)) {
            SrcPos pos = peek(p)->pos;
            advance(p);
            Expr** args = NULL;
            int argc = 0, cap = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    Expr* a = parse_expr(p);
                    if (argc == cap) {
                        cap = cap ? cap * 2 : 4;
                        Expr** na = arena_alloc(p->arena, cap * sizeof(Expr*));
                        if (args) memcpy(na, args, argc * sizeof(Expr*));
                        args = na;
                    }
                    args[argc++] = a;
                    if (!match(p, TK_COMMA)) break;
                }
            }
            expect(p, TK_RPAREN, "参数列表");
            Expr* c = new_expr(p, EX_CALL, pos);
            c->call.callee = e;
            c->call.args   = args;
            c->call.argc   = argc;
            e = c;
            continue;
        }
        if (check(p, TK_LBRACKET)) {
            SrcPos pos = peek(p)->pos;
            advance(p);
            Expr* idx = parse_expr(p);
            expect(p, TK_RBRACKET, "索引表达式");
            Expr* ix = new_expr(p, EX_INDEX, pos);
            ix->index.obj   = e;
            ix->index.index = idx;
            e = ix;
            continue;
        }
        if (check(p, TK_DOT)) {
            SrcPos pos = peek(p)->pos;
            advance(p);
            Token* name = peek(p);
            if (name->kind != TK_IDENT) {
                diag_error(name->pos, "`.` 之后期望一个成员名，实际读到 `%s`", tok_name(name->kind));
    diag_help("`.` 后面必须跟一个字段名，如 `obj.field`");
                return e;
            }
            advance(p);
            Expr* m = new_expr(p, EX_MEMBER, pos);
            m->member.obj  = e;
            m->member.name = name->s;

            // 注意 `?.` 不在这里处理，而在下面的独立分支里。
            // 理由写在那个分支的注释里（要把 `?` 与 `.` 分开看，
            // 避免破坏三元里 `.5` 这种字面量）。
            e = m;
            continue;
        }
        // `a?.b` / `a?[i]`
        if (check(p, TK_QUESTION)) {
            // 只看一眼 `?` 后面是什么。是 `.` 或 `[` 才认作可选链；
            // 否则 `?` 属于三元运算符，必须留给 parse_ternary。
            Token* q = peek(p);
            Token* after = peek_ahead(p, 1);
            bool opt_member = after && after->kind == TK_DOT;
            bool opt_index  = after && after->kind == TK_LBRACKET;

            if (opt_member || opt_index) {
                advance(p);   // `?`
                if (opt_member) {
                    advance(p);   // `.`
                    Token* name = peek(p);
                    if (name->kind != TK_IDENT) {
                        diag_error(name->pos,
                                   "`?.` 之后期望一个成员名，实际读到 `%s`",
                                   tok_name(name->kind));
                        diag_help("写法是 `a?.b`，例如 `user?.name`");
                        return e;
                    }
                    advance(p);
                    Expr* m = new_expr(p, EX_MEMBER, q->pos);
                    m->member.obj  = e;
                    m->member.name = name->s;
                    Expr* oc = new_expr(p, EX_OPT_CHAIN, q->pos);
                    oc->optc.access = m;
                    e = oc;
                } else {
                    advance(p);   // `[`
                    Expr* idx = parse_expr(p);
                    expect(p, TK_RBRACKET, "可选索引的 `]`");
                    Expr* ix = new_expr(p, EX_INDEX, q->pos);
                    ix->index.obj   = e;
                    ix->index.index = idx;
                    Expr* oc = new_expr(p, EX_OPT_CHAIN, q->pos);
                    oc->optc.access = ix;
                    e = oc;
                }
                continue;
            }
        }
        break;
    }
    return e;
}

// 一元前缀
static Expr* parse_unary(Parser* p) {
    Token* t = peek(p);
    SrcPos pos = t->pos;
    switch (t->kind) {
        case TK_MINUS: {
            advance(p);
            Expr* o = parse_unary(p);
            Expr* e = new_expr(p, EX_UNARY, pos);
            e->unary.op = UOP_NEG;
            e->unary.operand = o;
            return e;
        }
        case TK_PLUS: {
            advance(p);
            return parse_unary(p);   // 一元 + 是恒等
        }
        case TK_NOT:
        case TK_BANG: {
            advance(p);
            Expr* o = parse_unary(p);
            Expr* e = new_expr(p, EX_UNARY, pos);
            e->unary.op = UOP_NOT;
            e->unary.operand = o;
            return e;
        }
        case TK_TILDE: {
            advance(p);
            Expr* o = parse_unary(p);
            Expr* e = new_expr(p, EX_UNARY, pos);
            e->unary.op = UOP_BNOT;
            e->unary.operand = o;
            return e;
        }
        default:
            return parse_postfix(p);
    }
}

// 二元运算的通用构造
static Expr* mk_binary(Parser* p, BinOp op, Expr* l, Expr* r, SrcPos pos) {
    Expr* e = new_expr(p, EX_BINARY, pos);
    e->binary.op  = op;
    e->binary.lhs = l;
    e->binary.rhs = r;
    return e;
}

// 乘除模
static Expr* parse_mul(Parser* p) {
    Expr* left = parse_unary(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        BinOp op;
        if      (k == TK_STAR)    op = OP_MUL;
        else if (k == TK_SLASH)   op = OP_DIV;
        else if (k == TK_PERCENT) op = OP_MOD;
        else break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_unary(p);
        left = mk_binary(p, op, left, right, pos);
    }
    return left;
}

// 加减
static Expr* parse_add(Parser* p) {
    Expr* left = parse_mul(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        BinOp op;
        if      (k == TK_PLUS)  op = OP_ADD;
        else if (k == TK_MINUS) op = OP_SUB;
        else break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_mul(p);
        left = mk_binary(p, op, left, right, pos);
    }
    return left;
}

// 移位
static Expr* parse_shift(Parser* p) {
    Expr* left = parse_add(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        BinOp op;
        if      (k == TK_SHL) op = OP_SHL;
        else if (k == TK_SHR) op = OP_SHR;
        else break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_add(p);
        left = mk_binary(p, op, left, right, pos);
    }
    return left;
}

// 位与
static Expr* parse_band(Parser* p) {
    Expr* left = parse_shift(p);
    while (check(p, TK_AMP)) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_shift(p);
        left = mk_binary(p, OP_BAND, left, right, pos);
    }
    return left;
}

// 位异或
static Expr* parse_bxor(Parser* p) {
    Expr* left = parse_band(p);
    while (check(p, TK_CARET)) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_band(p);
        left = mk_binary(p, OP_BXOR, left, right, pos);
    }
    return left;
}

// 位或
static Expr* parse_bor(Parser* p) {
    Expr* left = parse_bxor(p);
    while (check(p, TK_PIPE)) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_bxor(p);
        left = mk_binary(p, OP_BOR, left, right, pos);
    }
    return left;
}

// 关系
static Expr* parse_relational(Parser* p) {
    Expr* left = parse_bor(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        BinOp op;
        if      (k == TK_LT) op = OP_LT;
        else if (k == TK_LE) op = OP_LE;
        else if (k == TK_GT) op = OP_GT;
        else if (k == TK_GE) op = OP_GE;
        else break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_bor(p);
        left = mk_binary(p, op, left, right, pos);
    }
    return left;
}

// 相等
static Expr* parse_equality(Parser* p) {
    Expr* left = parse_relational(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        BinOp op;
        if      (k == TK_EQ) op = OP_EQ;
        else if (k == TK_NE) op = OP_NE;
        else break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_relational(p);
        left = mk_binary(p, op, left, right, pos);
    }
    return left;
}

// 逻辑与
static Expr* parse_and(Parser* p) {
    Expr* left = parse_equality(p);
    while (check(p, TK_ANDAND)) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_equality(p);
        left = mk_binary(p, OP_AND, left, right, pos);
    }
    return left;
}

// 逻辑或（or 与 || 等价，01 §2.4 的 or 是关键字）
static Expr* parse_or(Parser* p) {
    Expr* left = parse_and(p);
    for (;;) {
        TokKind k = peek(p)->kind;
        if (k != TK_OROR && k != TK_OR) break;
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_and(p);
        left = mk_binary(p, OP_OR, left, right, pos);
    }
    return left;
}

// 空值合并 `a ?? b`（左结合）
//
// ── 为什么放在 or 与三元之间 ─────────────────────────────────
//
// `??` 的直觉是「取第一个非 null 的」，它与逻辑或的关系最近，
// 所以优先级紧挨着 `||`、比它低一档。这样 `a || b ?? c` 是
// `(a || b) ?? c` —— 虽然这种混写几乎没人会写，但万一写了，
// 结果符合「先算布尔、再兜底」的直觉。
//
// 左结合：`a ?? b ?? c` 是 `(a ?? b) ?? c`，与 `||` 一致。
// 右结合也说得通（数学上等价），但左结合和同一层的其他运算符
// 保持同一种结合性，少一条需要记忆的规则。
//
// 注意这个函数名与 or 的关系：它调 parse_or，被 parse_ternary 调。
static Expr* parse_coalesce(Parser* p) {
    Expr* left = parse_or(p);
    while (check(p, TK_QQ)) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_or(p);
        Expr* e = new_expr(p, EX_NULL_COALESCE, pos);
        e->coal.lhs = left;
        e->coal.rhs = right;
        left = e;
    }
    return left;
}

// ============================================================
// match 表达式
// ============================================================
//
//     match 值 {
//         1        => "一",
//         2..=9    => "几",
//         n @ 10..99 => "两位数：{n}",
//         "a", "b" => "字母",       // 多模式共享一个分支
//         _        => "其他",
//     }
//
// ── 为什么是表达式而不是语句 ──────────────────────────────────
//
// 每个分支产出一个值，整个 match 的值就是命中的那个分支的值。
// 好处是 `let s = match n { ... }` 直接可用 —— 而语句形式会逼出
// 「先声明 var s，再在每个分支里赋值」，那在每个分支都要写一遍
// 变量名，且漏写一个分支就是「变量未初始化」的运行时错。
//
// ── 穷尽性 ────────────────────────────────────────────────────
//
// 要求必须有 `_` 分支（或等价的全捕获）。理由：没有穷尽性检查的
// match 在漏掉某个值时必须走到「没有分支命中」的路径，那条路径只能
// 返回 null 或抛错 —— 两者都是「运行到才知道」，而这类错误在测试
// 里往往命不中。强制 `_` 把「漏了怎么办」变成写代码时必须回答的
// 问题，与 §12 的「无隐式可选值」是同一条原则。
//
// 代价：写起来必须多一个 `_`。对 2~3 个分支的场景确实啰嗦，但换来
// 的是「match 永远有值」这个可以无脑依赖的性质。
// 解析模式本身，填进 `arm`，返回种类。绑定名与 guard 由调用方处理，
// 因为它们在语法上位于模式之外（`n @ pat` / `pat if cond`）。
//
// 这里刻意只认「字面量」和「范围」，不认任意表达式：模式是结构化的，
// 允许 `a + b` 会让「匹配」退化成运行时求值比较，看不出这是模式匹配。
static PatKind parse_pattern(Parser* p, MatchArm* arm) {
    bool lit_start = check(p, TK_INT) || check(p, TK_FLOAT) ||
                     check(p, TK_STRING) || check(p, TK_TRUE) ||
                     check(p, TK_FALSE) || check(p, TK_NULL);

    if (!lit_start) {
        diag_error(peek(p)->pos, "match 分支期望一个模式，实际读到 `%s`",
                   tok_name(peek(p)->kind));
        diag_help("模式可以是字面量（`1` / `\"a\"` / `true`）、"
                  "范围（`0..=9`）、通配 `_` 或绑定 `n @ 0..=9`");
        // 吃一个 token 免得死循环；返回字面量壳子让降级有东西可用
        arm->kind = PAT_LITERAL;
        arm->lit  = parse_unary(p);
        return PAT_LITERAL;
    }

    Expr* first = parse_primary(p);

    if (check(p, TK_DOTDOT) || check(p, TK_DOTDOT_EQ)) {
        bool inc = (peek(p)->kind == TK_DOTDOT_EQ);
        advance(p);
        arm->kind      = PAT_RANGE;
        arm->lo        = first;
        arm->hi        = parse_primary(p);
        arm->inclusive = inc;
        return PAT_RANGE;
    }

    arm->kind = PAT_LITERAL;
    arm->lit  = first;
    return PAT_LITERAL;
}

static Expr* parse_match(Parser* p) {
    SrcPos pos = advance(p)->pos;   // `match`

    Expr* subject = parse_expr(p);

    expect(p, TK_LBRACE, "match 的分支块 `{`");

    MatchArm* arms = NULL;
    int count = 0, cap = 0;
    bool has_wild = false;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        MatchArm arm;
        memset(&arm, 0, sizeof(arm));
        arm.pos = peek(p)->pos;

        // `_` 通配
        bool is_wild = check(p, TK_IDENT) && peek(p)->len == 1 &&
                       peek(p)->s[0] == '_';
        // 绑定名：标识符后跟 `@`（`n @ 1..=9`），或裸标识符（`x if ...`）。
        // 后者语义等于 `_` 加绑定 —— 因为匹配任何值都成功。
        bool is_bind = !is_wild && check(p, TK_IDENT);

        if (is_wild) {
            advance(p);
            arm.kind = PAT_WILD;
            has_wild = true;
        } else if (is_bind) {
            Token* bn = peek(p);
            advance(p);
            arm.binding = bn->s;

            if (match(p, TK_AT)) {
                // `n @ <pat>` —— 模式部分必填
                arm.kind = parse_pattern(p, &arm);
            } else {
                // 裸名字：匹配一切，条件全靠 guard。等价于「带绑定的 `_`」，
                // 所以它同样算穷尽性担保 —— 否则 `match n { x if ... }`
                // 会被要求再补一个 `_`，而那个 `_` 永远走不到。
                arm.kind = PAT_WILD;
                has_wild = true;
            }
        } else {
            arm.kind = parse_pattern(p, &arm);
        }

        // 可选的 guard：`pat if 条件 => ...`
        if (match(p, TK_IF)) {
            arm.guard = parse_expr(p);
        }

        // `=>`
        expect(p, TK_FATARROW, "分支的 `=>`");
        arm.body = parse_expr(p);

        // 追加
        if (count + 1 > cap) {
            int nc = cap ? cap * 2 : 8;
            MatchArm* na = arena_alloc(p->arena, sizeof(MatchArm) * (size_t)nc);
            if (arms) memcpy(na, arms, sizeof(MatchArm) * (size_t)count);
            arms = na;
            cap = nc;
        }
        arms[count++] = arm;

        if (!match(p, TK_COMMA)) {
            // 分支之间可以不写逗号（换行分隔）
        }
    }

    expect(p, TK_RBRACE, "match 分支块的 `}`");

    if (!has_wild) {
        diag_error(pos,
                   "match 必须有 `_` 分支（穷尽性）");
        diag_help("补一个 `_ => 默认值`。强制穷尽是为了让「漏了某个值」"
                  "在写代码时就必须回答，而不是运行时才发现");
    }

    Expr* e = new_expr(p, EX_MATCH, pos);
    e->matchx.subject   = subject;
    e->matchx.arms      = arms;
    e->matchx.arm_count = count;
    return e;
}

// 条件表达式 `cond ? a : b`（右结合）
//
// ── 为什么插在 or 与赋值之间 ──────────────────────────────────
//
// C 系语言的优先级是：`||` < `?:` < `=`。这里照抄，理由是「照抄
// 常见语言的优先级」本身就是正确的选择 —— 用户的手已经记住了
// `a || b ? x : y` 是 `(a || b) ? x : y`，改成别的只会让人写错。
//
// 右结合：`a ? b : c ? d : e` 解析成 `a ? b : (c ? d : e)`，与
// C 一致。三元很少嵌套超过两层，但链式写法（默认值套默认值）确实
// 会出现，右结合让它的读法与书写顺序一致。
//
// ── 关于「条件求值一次」──────────────────────────────────────
//
// 条件只算一次：降级成 `JUMP_IF_FALSE` 时栈上已经是被弹出的那个
// 布尔值，没有重复求值的路径。这条不需要额外写代码，但需要在
// compile.c 里保持「先编译 cond、再 emit 跳转」的顺序。
static Expr* parse_ternary(Parser* p) {
    Expr* cond = parse_coalesce(p);

    if (!check(p, TK_QUESTION)) return cond;

    SrcPos pos = advance(p)->pos;   // `?`
    Expr* then_e = parse_ternary(p);   // 中间段允许再出现三元（左结合感）

    if (!check(p, TK_COLON)) {
        diag_error(peek(p)->pos, "三元表达式缺少 `:`");
        diag_help("写法是 `条件 ? 真值 : 假值`，如 `n > 0 ? \"正\" : \"负\"`");
        // 吞到能继续解析的地方，避免后面连锁报错
        Expr* e = new_expr(p, EX_TERNARY, pos);
        e->tern.cond = cond;
        e->tern.then_e = then_e;
        e->tern.else_e = new_expr(p, EX_NULL, peek(p)->pos);
        return e;
    }
    advance(p);   // `:`

    // 假值段用 parse_ternary 递归 → 右结合
    Expr* else_e = parse_ternary(p);

    Expr* e = new_expr(p, EX_TERNARY, pos);
    e->tern.cond   = cond;
    e->tern.then_e = then_e;
    e->tern.else_e = else_e;
    return e;
}

// 赋值（右结合）
static Expr* parse_expr(Parser* p) {
    Expr* left = parse_ternary(p);

    TokKind k = peek(p)->kind;
    if (k == TK_ASSIGN) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_expr(p);   // 右结合
        Expr* e = new_expr(p, EX_ASSIGN, pos);
        e->assign.target = left;
        e->assign.value  = right;
        return e;
    }
    // 复合赋值：+= 等，语法上等价于 left = left op right
    // 但要求 left 是左值。这里直接展开，P1 再做「只求值一次」的优化。
    BinOp cop;
    bool is_compound = true;
    switch (k) {
        case TK_PLUS_EQ:    cop = OP_ADD; break;
        case TK_MINUS_EQ:   cop = OP_SUB; break;
        case TK_STAR_EQ:    cop = OP_MUL; break;
        case TK_SLASH_EQ:   cop = OP_DIV; break;
        case TK_PERCENT_EQ: cop = OP_MOD; break;
        default: is_compound = false; cop = OP_ADD; break;
    }
    if (is_compound) {
        SrcPos pos = advance(p)->pos;
        Expr* right = parse_expr(p);
        Expr* bin = mk_binary(p, cop, left, right, pos);
        Expr* e = new_expr(p, EX_ASSIGN, pos);
        e->assign.target = left;
        e->assign.value  = bin;
        return e;
    }

    return left;
}

// ============================================================
// 语句
// ============================================================

// 解析 `{ ... }`，返回 EX_BLOCK。块的最后表达式作为值（01 §3.3）。
static Expr* parse_block_expr(Parser* p) {
    SrcPos pos = peek(p)->pos;
    expect(p, TK_LBRACE, "块开始");

    Stmt** stmts = NULL;
    int count = 0, cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Stmt* s = parse_stmt(p);
        if (!s) continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            Stmt** ns = arena_alloc(p->arena, cap * sizeof(Stmt*));
            if (stmts) memcpy(ns, stmts, count * sizeof(Stmt*));
            stmts = ns;
        }
        stmts[count++] = s;
    }

    expect(p, TK_RBRACE, "块结束");

    Expr* e = new_expr(p, EX_BLOCK, pos);
    e->block.stmts = stmts;
    e->block.count = count;
    e->block.tail  = NULL;   // P1: 支持块尾表达式作为值
    return e;
}

// 解析一个类型名（P0 只收集名字，不做语义检查）
static char* parse_type_name(Parser* p) {
    // 简化：`Int` / `Float` / `Bool` / `String` / `void` / 标识符
    // 支持 `[T]`（数组）与 `T?`（可选）
    Token* t = peek(p);
    if (t->kind == TK_IDENT || t->kind == TK_VOID) {
        advance(p);
        return t->s;
    }
    if (t->kind == TK_LBRACKET) {
        advance(p);
        char* inner = parse_type_name(p);
        expect(p, TK_RBRACKET, "数组类型");
        // P0 不构造复合类型字符串，只返回内层名
        return inner;
    }
    return NULL;
}

// let / var 声明
static Stmt* parse_var_decl(Parser* p, bool mutable_, bool is_const) {
    SrcPos pos = peek(p)->pos;
    advance(p);   // let / var / const

    // 支持 `let x: Int = 1` 与 `let x = 1` 与 `let x: Int`（无初值）
    // 也支持解构 `let (a, b) = f()`——P0 先不支持，留 P1

    Token* name = peek(p);
    if (name->kind != TK_IDENT) {
        diag_error(name->pos, "`%s` 之后期望一个变量名，实际读到 `%s`",
                   mutable_ ? "var" : (is_const ? "const" : "let"),
                   tok_name(name->kind));
    diag_help("变量名必须是合法标识符，如 `let x = 1`");
        advance(p);
        return NULL;
    }
    advance(p);

    char* type_name = NULL;
    if (match(p, TK_COLON)) {
        type_name = parse_type_name(p);
    }

    Expr* init = NULL;
    if (match(p, TK_ASSIGN)) {
        init = parse_expr(p);
    }

    match(p, TK_SEMI);   // 分号可选（01 §12「大括号 + 分号（可选）」）

    Stmt* s = new_stmt(p, mutable_ ? ST_VAR : ST_LET, pos);
    s->var.name      = name->s;
    s->var.type_name = type_name;
    s->var.init      = init;
    s->var.mutable_  = mutable_ || is_const;
    s->var.is_const  = is_const;
    return s;
}

// ============================================================
// for 展开：把递增语句同时插到 continue 之前
// ============================================================
//
// ── 这个函数解决什么问题 ────────────────────────────────────
//
// `for i in 0..n` 展开成 `let i = lo; while i < n { body; i += 1 }`。
// 看起来完美，但 `continue` 会暴露一个致命缺陷：
//
//     for i in 0..3 {
//         if i == 1 { continue }
//         print(i)
//     }
//
// 展开之后 continue 跳回 while 的条件判断 —— 而 `i += 1` 在 body
// 末尾，被整个跳过了。于是 i 永远停在 1，**死循环**。
//
// 这个 bug 的实际表现非常难受：程序不报错、不崩溃，只是在某个循环
// 上永远不返回。第一次遇到时的第一反应是「解释器死锁了」，而不是
// 「continue 语义错了」。而在扫雷里它出现在
// 「遍历 3×3 邻域并跳过越界格」这种每个格子都要跑的代码上。
//
// ── 为什么在 parser 层修 ────────────────────────────────────
//
// 另一条路是给 LoopCtx 加一个「continue 前要执行的语句」，让
// compile.c 在回填 continue 跳转时先编译它。那需要给 Stmt 加字段、
// 改 compile、顺便让 codegen_c 和栈深分析也认它 —— 就是「for 是
// 纯语法糖」这条设计原则想避免的连锁改动。
//
// 在 parser 层改成「物理上把 inc 复制到每个 continue 前面」之后，
// 下游看到的仍然只是普通的 var/while/continue，零改动。
// 代价是代码体积（每个 continue 多一条递增指令），可忽略。
//
// ── 为什么递归时要「穿透」而不只看本层 ──────────────────────
//
// continue 可能写在 if 里面、块里面、或者嵌套的 while 里面。
// 前两种属于本层循环，必须插；后一种属于内层循环，**不能插**
// （内层 continue 该跳内层的递增）。
//
// 所以遍历规则是：
//   · if / block / while(内层) 的体 → 递归，但 while/for 的体不再深入
//     因为 for 已经被展开成 while，内层的 continue 已经由内层处理过
//   · 遇到 ST_CONTINUE 且当前深度属于本层 → 前面插入一份 inc
//
// inc 是同一个 Stmt 指针被多处引用。这是安全的：编译一个表达式
// 语句不修改 AST，所以同一节点被编译两次只会生成两份指令。

// 判断一个 Stmt 是不是「内层循环」——遇到就停止深入
static bool is_inner_loop(Stmt* s) {
    return s && s->kind == ST_WHILE;
}

// 把一个「语句」当作语句列表来递归处理。
//
// ── 为什么需要这个包装，而不是直接取 ->block ────────────────
//
// ST_IF 的 then_body 通常是 ST_BLOCK（`if c { ... }`），但
// **else 分支不是**：`else if c2 { ... }` 走的是
// `else_body = parse_stmt(p)`，拿到的是一个 ST_IF 节点。
//
// 之前这里无条件下读 `body->block.stmts` / `body->block.count`，
// 于是对 `else if` 的节点，把 union 里的 `ifs.cond`（一个 Expr*）
// 当成 Stmt** 用、把 `ifs.then_body`（一个 Stmt*）当成 int 当数组
// 长度用 —— 相当于「指针当长度、指针当数组」，立刻越界。
//
// 症状极具误导性：扫雷的 ui.xle 里有 `else if !hover { tc = C_DIM }`，
// 解析到这里时崩溃。因为读到的是垃圾长度，崩的位置随堆布局变化，
// 表现为**非确定性段错误**（同一份源码反复编译，有时崩有时不崩），
// 而 rip 落在模块外的野地址上 —— 花了很久才定位到这几行。
//
// 正确处理：只有 ST_BLOCK 才往下钻；ST_IF 交给外层分支单独处理
// （它有 then/else 两条路，不能当成一条列表）。
static void inject_into_body(Stmt* body, Stmt* pre);

static void inject_pre_before_continues(Stmt*** pstmts, int* pcount, Stmt* pre) {
    Stmt** stmts = *pstmts;
    int count = *pcount;

    // ── 第一遍：递归处理子结构 ──
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (!s || is_inner_loop(s)) continue;
        if (s->kind == ST_IF) {
            // then_body 一定是 ST_BLOCK（parse_stmt 的 TK_IF 分支构造的），
            // 但 else_body 可能是 ST_IF（`else if`）—— 所以两条路都要
            // 走 inject_into_body，由它按实际种类分派。
            if (s->ifs.then_body) inject_into_body(s->ifs.then_body, pre);
            if (s->ifs.else_body) inject_into_body(s->ifs.else_body, pre);
        } else if (s->kind == ST_BLOCK) {
            inject_pre_before_continues(&s->block.stmts, &s->block.count, pre);
        }
    }

    // ── 第二遍：数本层剩余的 continue，重建数组 ──
    int inserts = 0;
    for (int i = 0; i < count; i++) {
        if (stmts[i] && stmts[i]->kind == ST_CONTINUE) inserts++;
    }
    if (inserts == 0) return;

    Stmt** arr = (Stmt**)x_alloc(sizeof(Stmt*) * (size_t)(count + inserts));
    int k = 0;
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s && s->kind == ST_CONTINUE) {
            arr[k++] = pre;
            arr[k++] = s;
            continue;
        }
        arr[k++] = s;
    }
    *pstmts = arr;
    *pcount = k;
}

// 对「一个语句」做注入，按它实际的种类分派。
//
// 只有 ST_BLOCK 才是一条语句列表；ST_IF 有自己的 then/else 两条路；
// 其余种类（ST_WHILE / ST_RETURN / ...）都不含「属于本层循环的
// continue」，直接忽略。这个分派正是上面那个崩溃 bug 的修法。
static void inject_into_body(Stmt* body, Stmt* pre) {
    if (!body) return;
    switch (body->kind) {
        case ST_BLOCK:
            inject_pre_before_continues(&body->block.stmts, &body->block.count, pre);
            break;
        case ST_IF:
            if (body->ifs.then_body) inject_into_body(body->ifs.then_body, pre);
            if (body->ifs.else_body) inject_into_body(body->ifs.else_body, pre);
            break;
        default:
            // ST_WHILE：内层循环，它的 continue 属于它自己，不插。
            // 其余种类没有语句列表，无事可做。
            break;
    }
}

static Stmt* parse_stmt(Parser* p) {
    Token* t = peek(p);
    SrcPos pos = t->pos;

    switch (t->kind) {
        case TK_LET:
            return parse_var_decl(p, false, false);

        case TK_VAR:
            return parse_var_decl(p, true, false);

        case TK_CONST:
            return parse_var_decl(p, false, true);

        case TK_RETURN: {
            advance(p);
            Expr* v = NULL;
            if (!check(p, TK_SEMI) && !check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                v = parse_expr(p);
            }
            match(p, TK_SEMI);
            Stmt* s = new_stmt(p, ST_RETURN, pos);
            s->ret.value = v;
            return s;
        }

        case TK_WHILE: {
            advance(p);
            Expr* cond = parse_expr(p);
            if (!check(p, TK_LBRACE)) {
                diag_error(peek(p)->pos, "`while` 条件之后期望 `{`，实际读到 `%s`",
                           tok_name(peek(p)->kind));
    diag_help("`while` 的循环体必须用大括号包起来：`while cond { ... }`");
            }
            Expr* body_expr = parse_block_expr(p);
            Stmt* body = new_stmt(p, ST_BLOCK, body_expr->pos);
            body->block.stmts = body_expr->block.stmts;
            body->block.count = body_expr->block.count;

            Stmt* s = new_stmt(p, ST_WHILE, pos);
            s->whilex.cond = cond;
            s->whilex.body = body;
            return s;
        }

        case TK_FOR: {
            // for 循环：**纯语法糖**，展开成等价的 while。
            //
            // ── 为什么在 parser 层展开而不是新增一个 Stmt 种类 ──────
            //
            // 如果新增 ST_FOR，那么 compile / codegen_c / 超指令合并 /
            // 栈深分析全都要认这个新节点，每一处都是一次遗漏机会。
            // 而 `for` 的语义完全能用现有的 var + while 表达，没有任何
            // 额外能力。展开成既有节点，下游一行都不用改 —— 这是
            // 「用最少的机制解决问题」的直接体现。
            //
            // 代价是：错误信息里的位置会落到合成节点上。所以下面
            // 每个合成节点都用用户写 `for` 的那一行作为 pos。
            advance(p);

            Token* var = peek(p);
            if (var->kind != TK_IDENT) {
                diag_error(var->pos, "`for` 之后期望循环变量名，实际读到 `%s`",
                           tok_name(var->kind));
                diag_help("写法：`for i in 0..n { ... }` 或 `for x in arr { ... }`");
                return NULL;
            }
            advance(p);

            if (!check(p, TK_IN)) {
                diag_error(peek(p)->pos, "`for` 循环变量之后期望 `in`，实际读到 `%s`",
                           tok_name(peek(p)->kind));
                diag_help("写法：`for i in 0..n { ... }`");
                return NULL;
            }
            advance(p);

            // 两种形式：
            //   for i in a..b   → 整数区间，i 从 a 到 b-1（右开）
            //   for x in arr    → 遍历数组元素
            //
            // 判断依据是表达式里有没有 `..`。这里先尝试解析区间：
            // 若能解析出 EX_RANGE 就走区间形式，否则当成数组遍历。
            //
            // 用一个临时的「是否区间」标记：先正常解析表达式，
            // 然后看 AST 节点是不是 EX_RANGE。
            // 先解析左侧表达式，再看后面是不是 `..`。
            //
            // 不把 range 做成通用表达式是因为它只在 for 里有意义：
            // 允许 `let r = 0..5` 会立刻引出「r 是什么类型」「能迭代几次」
            // 「能不能切片」一整串问题。限制在 for 内部，语义面最小。
            Expr* iter = parse_or(p);
            bool is_range = false;
            if (check(p, TK_DOTDOT) || check(p, TK_DOTDOT_EQ)) {
                bool incl = check(p, TK_DOTDOT_EQ);
                advance(p);
                Expr* hi = parse_or(p);
                Expr* r = new_expr(p, EX_RANGE, pos);
                r->range.lo = iter;
                r->range.hi = hi;
                r->range.inclusive = incl;
                iter = r;
                is_range = true;
                if (incl) {
                    // `a..=b` 右闭：等价于 `a..(b+1)`。在语法层就展开，
                    // 下游不需要知道闭区间的存在。
                    Expr* one = new_expr(p, EX_INT, pos);
                    one->i.v = 1;
                    Expr* sum = new_expr(p, EX_BINARY, pos);
                    sum->binary.op = OP_ADD;
                    sum->binary.lhs = r->range.hi;
                    sum->binary.rhs = one;
                    r->range.hi = sum;
                }
            }

            Expr* body_expr = parse_block_expr(p);
            Stmt* body = new_stmt(p, ST_BLOCK, body_expr->pos);
            body->block.stmts = body_expr->block.stmts;
            body->block.count = body_expr->block.count;

            if (is_range) {
                // for i in lo..hi { body }
                //   ≡
                // let i = lo
                // while i < hi { body; i = i + 1 }
                //
                // 用 `<` 而不是 `<=`：`0..n` 是右开的，遍历 n 个元素
                // 用 `0..n` 恰好，不用记 `0..n-1` 的 off-by-one。
                Stmt* init = new_stmt(p, ST_VAR, pos);
                init->var.name      = var->s;
                init->var.type_name = "Int";
                init->var.init      = iter->range.lo;
                init->var.mutable_  = true;

                // 条件：i < hi
                Expr* cond = new_expr(p, EX_BINARY, pos);
                cond->binary.op  = OP_LT;
                Expr* lhs = new_expr(p, EX_IDENT, pos);
                lhs->ident.name = var->s;
                cond->binary.lhs = lhs;
                cond->binary.rhs = iter->range.hi;

                // 递增：i = i + 1，放在循环体末尾
                Expr* inc_target = new_expr(p, EX_IDENT, pos);
                inc_target->ident.name = var->s;
                Expr* inc_one = new_expr(p, EX_INT, pos);
                inc_one->i.v = 1;
                Expr* inc_sum = new_expr(p, EX_BINARY, pos);
                inc_sum->binary.op  = OP_ADD;
                Expr* inc_lhs = new_expr(p, EX_IDENT, pos);
                inc_lhs->ident.name = var->s;
                inc_sum->binary.lhs = inc_lhs;
                inc_sum->binary.rhs = inc_one;
                Expr* inc_assign = new_expr(p, EX_ASSIGN, pos);
                inc_assign->assign.target = inc_target;
                inc_assign->assign.value  = inc_sum;

                Stmt* inc_stmt = new_stmt(p, ST_EXPR, pos);
                inc_stmt->expr.expr = inc_assign;

                // ── 关键一步：把 inc 也插到每个 `continue` 之前 ──
                //
                // 不做这一步的话 `continue` 会跳过递增，导致下标永不
                // 前进 —— 死循环，而且不报错。详细分析见
                // inject_pre_before_continues 的注释。
                inject_pre_before_continues(&body->block.stmts,
                                            &body->block.count, inc_stmt);

                // 把 inc 追加到循环体末尾（正常路径的递增）
                int n = body->block.count;
                Stmt** arr = (Stmt**)x_alloc(sizeof(Stmt*) * (size_t)(n + 1));
                for (int i = 0; i < n; i++) arr[i] = body->block.stmts[i];
                arr[n] = inc_stmt;
                body->block.stmts = arr;
                body->block.count = n + 1;

                Stmt* w = new_stmt(p, ST_WHILE, pos);
                w->whilex.cond = cond;
                w->whilex.body = body;

                Stmt* blk = new_stmt(p, ST_BLOCK, pos);
                blk->block.stmts = (Stmt**)x_alloc(sizeof(Stmt*) * 2);
                blk->block.stmts[0] = init;
                blk->block.stmts[1] = w;
                blk->block.count = 2;
                return blk;
            }

            // 数组遍历：for x in arr { body }
            //   ≡
            // let __i = 0
            // while __i < len(arr) { let x = arr[__i]; body; __i = __i + 1 }
            //
            // 这里需要一个「隐藏变量」存下标。名字用 `$for_i`：
            // `$` 不是合法标识符字符，所以用户不可能撞名。
            {
                static int for_seq = 0;
                char* idx_name = (char*)x_alloc(32);
                snprintf(idx_name, 32, "$for_%d", for_seq++);

                // let $for_i = 0
                Stmt* init = new_stmt(p, ST_VAR, pos);
                init->var.name      = idx_name;
                init->var.type_name = "Int";
                Expr* zero = new_expr(p, EX_INT, pos);
                zero->i.v = 0;
                init->var.init   = zero;
                init->var.mutable_ = true;

                // 条件：$for_i < len(iter)
                Expr* len_call = new_expr(p, EX_CALL, pos);
                Expr* len_fn = new_expr(p, EX_IDENT, pos);
                len_fn->ident.name = "len";
                len_call->call.callee = len_fn;
                len_call->call.args = (Expr**)x_alloc(sizeof(Expr*));
                len_call->call.args[0] = iter;
                len_call->call.argc = 1;

                Expr* cond = new_expr(p, EX_BINARY, pos);
                cond->binary.op = OP_LT;
                Expr* ilhs = new_expr(p, EX_IDENT, pos);
                ilhs->ident.name = idx_name;
                cond->binary.lhs = ilhs;
                cond->binary.rhs = len_call;

                // 体首插入：let x = iter[$for_i]
                Expr* idx_get = new_expr(p, EX_INDEX, pos);
                idx_get->index.obj = iter;
                Expr* idxv = new_expr(p, EX_IDENT, pos);
                idxv->ident.name = idx_name;
                idx_get->index.index = idxv;

                Stmt* bind = new_stmt(p, ST_LET, pos);
                bind->var.name = var->s;
                bind->var.type_name = NULL;
                bind->var.init = idx_get;
                bind->var.mutable_ = true;

                // 体尾：$for_i = $for_i + 1
                Expr* inc_target = new_expr(p, EX_IDENT, pos);
                inc_target->ident.name = idx_name;
                Expr* one = new_expr(p, EX_INT, pos);
                one->i.v = 1;
                Expr* sum = new_expr(p, EX_BINARY, pos);
                sum->binary.op = OP_ADD;
                Expr* sl = new_expr(p, EX_IDENT, pos);
                sl->ident.name = idx_name;
                sum->binary.lhs = sl;
                sum->binary.rhs = one;
                Expr* asg = new_expr(p, EX_ASSIGN, pos);
                asg->assign.target = inc_target;
                asg->assign.value  = sum;
                Stmt* inc = new_stmt(p, ST_EXPR, pos);
                inc->expr.expr = asg;

                // 把 inc 插到每个 continue 之前（理由同 range 分支：
                // 否则 continue 跳过递增，下标不动，死循环）。
                //
                // 注意这里必须在**加 bind 之前**做：bind 是
                // `let x = arr[$for_i]`，它在 continue 前面重复执行
                // 是无害的（只是重新绑一次同名变量），但如果 bind
                // 已经在数组里，inject 会在 `continue` 前插 inc，
                // 语义仍然是「先递增再 continue」——所以顺序其实都能
                // 对上。放在前面只是为了让 inject 处理的数组更小。
                inject_pre_before_continues(&body->block.stmts,
                                            &body->block.count, inc);

                int n = body->block.count;
                Stmt** arr = (Stmt**)x_alloc(sizeof(Stmt*) * (size_t)(n + 2));
                arr[0] = bind;
                for (int i = 0; i < n; i++) arr[i + 1] = body->block.stmts[i];
                arr[n + 1] = inc;
                body->block.stmts = arr;
                body->block.count = n + 2;

                Stmt* w = new_stmt(p, ST_WHILE, pos);
                w->whilex.cond = cond;
                w->whilex.body = body;

                Stmt* blk = new_stmt(p, ST_BLOCK, pos);
                blk->block.stmts = (Stmt**)x_alloc(sizeof(Stmt*) * 2);
                blk->block.stmts[0] = init;
                blk->block.stmts[1] = w;
                blk->block.count = 2;
                return blk;
            }
        }

        case TK_LOOP: {
            advance(p);
            Expr* body_expr = parse_block_expr(p);
            Stmt* body = new_stmt(p, ST_BLOCK, body_expr->pos);
            body->block.stmts = body_expr->block.stmts;
            body->block.count = body_expr->block.count;
            Stmt* s = new_stmt(p, ST_WHILE, pos);
            // loop 等价于 while true
            Expr* true_e = new_expr(p, EX_BOOL, pos);
            true_e->b.v = true;
            s->whilex.cond = true_e;
            s->whilex.body = body;
            return s;
        }

        case TK_BREAK: {
            advance(p);
            match(p, TK_SEMI);
            return new_stmt(p, ST_BREAK, pos);
        }

        case TK_CONTINUE: {
            advance(p);
            match(p, TK_SEMI);
            return new_stmt(p, ST_CONTINUE, pos);
        }

        case TK_IF: {
            advance(p);
            Expr* cond = parse_expr(p);
            Expr* then_expr = parse_block_expr(p);
            Stmt* then_body = new_stmt(p, ST_BLOCK, then_expr->pos);
            then_body->block.stmts = then_expr->block.stmts;
            then_body->block.count = then_expr->block.count;

            Stmt* else_body = NULL;
            if (match(p, TK_ELSE)) {
                if (check(p, TK_IF)) {
                    else_body = parse_stmt(p);   // else if
                } else {
                    Expr* else_expr = parse_block_expr(p);
                    else_body = new_stmt(p, ST_BLOCK, else_expr->pos);
                    else_body->block.stmts = else_expr->block.stmts;
                    else_body->block.count = else_expr->block.count;
                }
            }

            Stmt* s = new_stmt(p, ST_IF, pos);
            s->ifs.cond      = cond;
            s->ifs.then_body = then_body;
            s->ifs.else_body = else_body;
            return s;
        }

        case TK_LBRACE: {
            Expr* be = parse_block_expr(p);
            Stmt* s = new_stmt(p, ST_BLOCK, be->pos);
            s->block.stmts = be->block.stmts;
            s->block.count = be->block.count;
            return s;
        }

        case TK_SEMI:
            advance(p);
            return NULL;   // 空语句，跳过

        case TK_DEFER: {
            advance(p);
            Stmt* body = parse_stmt(p);
            Stmt* s = new_stmt(p, ST_DEFER, pos);
            s->defer.body = body;
            return s;
        }

        default:
            break;
    }

    // 表达式语句
    Expr* e = parse_expr(p);
    match(p, TK_SEMI);

    // 若表达式是赋值，转成 ST_EXPR（赋值在 01 里是表达式，语义一致）
    Stmt* s = new_stmt(p, ST_EXPR, pos);
    s->expr.expr = e;
    return s;
}

// ============================================================
// 顶层声明
// ============================================================

static FuncDecl* parse_fn(Parser* p) {
    SrcPos pos = peek(p)->pos;
    expect(p, TK_FN, "函数声明");

    Token* name = peek(p);
    if (name->kind != TK_IDENT) {
        diag_error(name->pos, "`fn` 之后期望函数名，实际读到 `%s`", tok_name(name->kind));
    diag_help("写成 `fn 名字(参数) -> 返回类型 { ... }`");
        return NULL;
    }
    advance(p);

    // ── 可选的点前缀：`fn Rect.area(self: Rect) -> Int` ──────────
    //
    // 这是「方法」的全部语法糖。解析结果就是一个名字里带点的
    // 普通 FuncDecl —— 之后编译器、字节码、解释器、AOT 全都
    // 不需要知道「方法」这个概念存在。
    //
    // 为什么要在这里拼名字而不是加一个 MethodDecl：
    // 拼名字的代价是一次 arena 分配，收益是**零新增类型**。加
    // MethodDecl 则意味着 Module 多一张表、main.c 多一轮合并、
    // 编译器多一套查找、`self` 多一条隐式绑定规则 —— 四处改动
    // 换来的只是「声明时少写第一参数 type」。不划算。
    //
    // 反过来，不做这个糖也不行：让用户手写
    //     fn `Rect.area`(...)
    // 就没有任何语法能把点放进标识符，只能靠字符串或转义，那比
    // 不支持方法还糟。所以这 6 行是**必需**的，不是可选的糖。
    char* fname = name->s;
    if (match(p, TK_DOT)) {
        Token* meth = peek(p);
        if (meth->kind != TK_IDENT) {
            diag_error(meth->pos, "`.` 之后期望方法名，实际读到 `%s`",
                       tok_name(meth->kind));
            diag_help("写成 `fn 结构体名.方法名(self: 结构体名, ...) { ... }`");
            return NULL;
        }
        advance(p);
        size_t need = name->len + 1 + meth->len + 1;
        char* full = arena_alloc(p->arena, need);
        memcpy(full, name->s, name->len);
        full[name->len] = '.';
        memcpy(full + name->len + 1, meth->s, meth->len);
        full[need - 1] = '\0';
        fname = full;
    }

    // 打点：函数名在 token 里不是 NUL 结尾的，先临时截一份出来。
    // 只在 trace 打开时才分配，所以热路径（trace 关）零开销 ——
    // 这也是为什么不在 token 里存 NUL 结尾的副本。
    char fnbuf[128];
    {
        size_t n = strlen(fname) < sizeof(fnbuf) - 1 ? strlen(fname) : sizeof(fnbuf) - 1;
        memcpy(fnbuf, fname, n);
        fnbuf[n] = '\0';
        strace_enter(fnbuf);
    }

    expect(p, TK_LPAREN, "函数参数列表");

    char** pnames = NULL;
    char** ptypes = NULL;
    int pcount = 0, cap = 0;

    if (!check(p, TK_RPAREN)) {
        for (;;) {
            Token* pn = peek(p);
            // 允许 `self` 作参数名。
            //
            // ── 为什么必须特批 ──────────────────────────────────
            //
            // `self` 在 lexer 里是关键字（TK_SELF），而关键字不是
            // TK_IDENT，所以方法声明的第一个参数 `self: Rect` 会被
            // 这里直接拒掉 —— 症状是一连串「期望参数名」的雪崩式
            // 报错，因为解析器接下来的恢复策略一路错位。
            //
            // 而 `self` 恰恰**只能**以这种方式出现：词法上它是保留字，
            // 语法上既不能声明 `let self` 也不该有别的用处（表达式位置
            // 的 TK_SELF 由 parse_primary 处理成标识符）。
            //
            // 名字统一归一化成英文 "self"。中文模式下 tk->s 是 "自身"，
            // 而 parse_primary 里表达式位置的 self 硬编码成 "self"，
            // 两边不统一就会出现「形参叫 自身、函数体里 self 找不到」。
            if (pn->kind != TK_IDENT && pn->kind != TK_SELF) {
                diag_error(pn->pos, "期望参数名，实际读到 `%s`", tok_name(pn->kind));
                diag_help("参数写成 `名字: 类型`，如 `fn f(a: Int, b: Float)`");
                break;
            }
            char* pname = (pn->kind == TK_SELF) ? arena_dup(p, "self") : pn->s;
            advance(p);
            char* ptype = NULL;
            if (match(p, TK_COLON)) {
                ptype = parse_type_name(p);
            }
            if (pcount == cap) {
                cap = cap ? cap * 2 : 4;
                char** nn = arena_alloc(p->arena, cap * sizeof(char*));
                char** nt = arena_alloc(p->arena, cap * sizeof(char*));
                if (pnames) memcpy(nn, pnames, pcount * sizeof(char*));
                if (ptypes) memcpy(nt, ptypes, pcount * sizeof(char*));
                pnames = nn; ptypes = nt;
            }
            pnames[pcount] = pname;
            ptypes[pcount] = ptype;
            pcount++;

            if (!match(p, TK_COMMA)) break;
        }
    }
    expect(p, TK_RPAREN, "函数参数列表");

    char* ret_type = NULL;
    if (match(p, TK_ARROW)) {
        ret_type = parse_type_name(p);
    }

    // throws 子句（01 §9）：P0 解析但忽略
    if (match(p, TK_IDENT)) {
        // 这里可能是 `throws`——它不是关键字，靠文本判断
        Token* tk = peek_ahead(p, -1);
        if (tk && tk->kind == TK_IDENT && strcmp(tk->s, "throws") == 0) {
            // 吃掉异常类型列表
            while (!check(p, TK_LBRACE) && !check(p, TK_EOF)) advance(p);
        } else {
            p->pos--;   // 回退
        }
    }

    if (!check(p, TK_LBRACE)) {
        diag_error(peek(p)->pos, "函数 `%s` 缺少函数体：期望 `{`，实际读到 `%s`",
                   name->s, tok_name(peek(p)->kind));
    diag_help("函数声明必须以 `{ ... }` 结束，P0 不支持只有签名的声明");
        return NULL;
    }

    Expr* body_expr = parse_block_expr(p);

    FuncDecl* f = arena_alloc(p->arena, sizeof(FuncDecl));
    memset(f, 0, sizeof(*f));
    f->name        = fname;   // 可能带 `类型.` 前缀（方法）
    f->param_names = pnames;
    f->param_types = ptypes;
    f->param_count = pcount;
    f->ret_type    = ret_type;
    f->pos         = pos;

    Stmt* body = new_stmt(p, ST_BLOCK, body_expr->pos);
    body->block.stmts = body_expr->block.stmts;
    body->block.count = body_expr->block.count;
    f->body = body;

    return f;
}

// 解析 import 语句，把路径记下来。
//
// ── P0 的模块系统是什么 ────────────────────────────────────
//
// 支持的写法有四种：
//     import "board.xle"                // 取字符串字面量里的路径（旧式）
//     import board                      // 取标识符，补 .xle 后缀
//     import @util                      // 包依赖，解析见 pkg.c
//     import @util as u                 // 带别名（P0 只记录，不强制使用）
//
// ── 两种 import 的语义差别 ─────────────────────────────────
//
// `import board` 与 `import @util` 在**解析阶段**就分成两条路：
//
//   裸标识符 → 相对于当前文件的路径，找不到就报错（文件级）
//   @ 前缀   → 相对于 x.toml 的依赖键，先查清单再无路径解析（包级）
//
// 为什么要分成两套：想在一份代码里同时说清「这是同项目的模块」
// 和「这是外部依赖」。混成一种写法，读者/工具/审计都分不清
// `import utils` 到底是隔壁文件还是需要从网上拉的包。
// spec/01 §11.1 给了完整论证。
//
// 语义上两者都是**文本级合并**：被 import 的文件只允许包含 `fn` 声明
// 与不可变声明，它们被原样并入当前编译单元。没有命名空间、
// 没有可见性、没有循环依赖处理（见 parse_file 的说明）。
//
// ── 为什么只允许 fn 与不可变声明 ───────────────────────────
//
// 顶层语句会被合成为 main。如果两个文件都有顶层语句，就会得到
// 两个 main —— 那是个必报错的组合，但报错位置很别扭（指向另一个
// 文件的行）。所以干脆禁止：被 import 的文件只能是「函数库」。
static void parse_import(Parser* p) {
    SrcPos pos = peek(p)->pos;
    advance(p);   // 消费 import 或 from

    // `from X import a, b` 这种形式 P0 不支持：语法上能跳过，
    // 但语义上「只导入其中几个名字」需要符号表，留到 P1。
    char* path = NULL;

    if (check(p, TK_AT)) {
        // ---- 包依赖：import @name [as alias] ----
        advance(p);   // 消费 @
        if (!check(p, TK_IDENT)) {
            diag_error(pos, "`@` 之后期望包名");
            diag_help("写法是 `import @util` 或 `import @util as u`");
            match(p, TK_SEMI);
            return;
        }
        Token* t = advance(p);
        size_t n = t->len + 2;          // "@" + 名字 + NUL
        path = arena_alloc(p->arena, n);
        path[0] = '@';
        memcpy(path + 1, t->s, t->len);
        path[t->len + 1] = '\0';

        // 可选别名。P0 只**接受**这个语法，不使用它 ——
        // 别名会改变名字查找规则，那是 P1 的符号表能力。
        //
        // 为什么现在就要接受而不是报错：让用户能写出未来版本的代码，
        // 而不用等 P1 才能开始用别名组织代码。代价是「写了 alias
        // 但没生效」，所以这里给一条警告说清楚。
        if (check(p, TK_AS)) {
            advance(p);
            if (check(p, TK_IDENT)) {
                Token* a = advance(p);
                diag_warn(a->pos, "`as` 别名在 P0 尚未生效");
                diag_note("P0 的模块是文本级合并，没有独立的命名空间 —— "
                          "也就是说函数名本来就是全局的，别名无处可挂。");
                diag_help("暂时直接写 `import @%s`，函数名不带前缀直接调用。",
                          path + 1);
                diag_note("别名会在 P1 的符号表能力到位后生效。");
            }
        }
    } else if (check(p, TK_STRING)) {
        Token* t = advance(p);
        path = arena_strndup(p->arena, t->s, t->len);
    } else if (check(p, TK_IDENT)) {
        Token* t = advance(p);

        // `import std.json` —— 点路径模块。
        //
        // ── 为什么要支持点路径，而不是只用 `import "std/json.xle"` ──
        //
        // 标准库是**语言的一部分**，用户心智里它叫 `std.json`，不叫
        // 「某个目录下的某个文件」。如果强制写相对路径，那标准库就退化
        // 成了「碰巧放在旁边的文件」—— 换个工作目录就跑不了，而且用户
        // 得知道 stdlib 装在哪。点路径把「装在哪」这件事从用户视野里
        // 拿掉：`import std.json` 在哪个目录都是同一句话。
        //
        // 解析结果保留成 "std.json" 字面量（**不**在这里补 .xle），
        // 因为 main.c 要先用 `std.` 前缀判断走标准库查找还是同项目查找。
        // 在这里补后缀会让两种路径的判定纠缠在一起。
        if (check(p, TK_DOT)) {
            // 逐段拼接：std.json / std.db.sqlite 都支持
            size_t cap = t->len + 16;
            char* buf = arena_alloc(p->arena, cap);
            size_t used = 0;
            memcpy(buf, t->s, t->len);
            used = t->len;
            buf[used] = '\0';

            while (check(p, TK_DOT)) {
                advance(p);          // 消费 .
                Token* seg = peek(p);
                if (seg->kind != TK_IDENT) {
                    diag_error(seg->pos, "`.` 之后期望模块名，实际读到 `%s`",
                               tok_name(seg->kind));
                    diag_help("点路径写法：`import std.json`、`import std.db.sqlite`");
                    break;
                }
                advance(p);
                // 需要 1（点） + 段长 + 1（NUL）
                if (used + seg->len + 2 > cap) {
                    size_t ncap = cap * 2 + seg->len;
                    char* nb = arena_alloc(p->arena, ncap);
                    memcpy(nb, buf, used + 1);
                    buf = nb;
                    cap = ncap;
                }
                buf[used++] = '.';
                memcpy(buf + used, seg->s, seg->len);
                used += seg->len;
                buf[used] = '\0';
            }
            path = buf;
            match(p, TK_SEMI);
            if (!path) return;
            goto record_import;
        }

        // `import board` → "board.xle"
        // 用 arena 而不是栈上 buffer：路径要活到编译结束
        //（main.c 用它去开文件），不能是局部数组。
        size_t n = t->len + 5;   // ".xle" + NUL
        path = arena_alloc(p->arena, n);
        memcpy(path, t->s, t->len);
        memcpy(path + t->len, ".xle", 5);
    } else {
        diag_error(pos, "`import` 之后期望文件路径、模块名或 `@包名`，实际读到 `%s`",
                   tok_name(peek(p)->kind));
        diag_help("三种写法：");
        diag_help("  import \"board.xle\"    相对路径");
        diag_help("  import board           同项目模块（补 .xle 后缀）");
        diag_help("  import @util           外部依赖（需在 x.toml 声明）");
        // 只消费一个 token 就返回，**不做「跳到语句末尾」的恢复**。
        //
        // 这里原本有一个 while 循环，想跳到分号或下一个语句关键字。
        // 那是错的：X 语言允许省略分号，`import "a.xle"` 后面紧跟的
        // 就是下一条语句。于是那个循环会一路吞掉 `println(1)`
        // 甚至 `var x = 1` 之后的内容，表现为「顶层语句凭空消失，
        // 程序报没有入口」。吞掉的范围取决于下一条语句是不是以
        // fn/let/var 开头，所以症状很随机。
        //
        // 相反，只消费一个 token 让错误恢复尽量小：语法错在这一行，
        // 影响范围就只有这一行。
        advance(p);
        match(p, TK_SEMI);
        return;
    }

    // 可选的结尾分号。**只认分号，不吞其余 token** —— 理由同上。
    match(p, TK_SEMI);

record_import:
    if (path) {
        // 两个平行数组必须**同时**扩容。
        //
        // 这里踩过一个坑：原本只在 imports 满了才扩容，然后行号数组
        // 用 `import_line_count == import_cap` 单独判断。第一次 import 时
        // imports 扩容到 4（import_cap=4），import_count 变成 1；
        // 而 import_line_count 还是 0，`0 == 4` 不成立，于是跳过分配，
        // 紧接着写 `p->import_lines[0]` —— 对 NULL 解引用，当场段错误。
        //
        // 症状极具迷惑性：段错误发生在**第一个 import 语句**上，
        // 任何带 import 的程序都崩，但崩的地方看起来完全无害。
        // 教训：平行数组的生命周期必须绑在一起，各自判各自的条件
        // 是错的 —— 它们的容量是同一个概念。
        if (p->import_count == p->import_cap) {
            int nc = p->import_cap ? p->import_cap * 2 : 4;

            char** ni = arena_alloc(p->arena, (size_t)nc * sizeof(char*));
            if (p->imports) {
                memcpy(ni, p->imports, (size_t)p->import_count * sizeof(char*));
            }
            p->imports = ni;

            int* nl = arena_alloc(p->arena, (size_t)nc * sizeof(int));
            if (p->import_lines) {
                memcpy(nl, p->import_lines, (size_t)p->import_count * sizeof(int));
            }
            p->import_lines = nl;

            p->import_cap = nc;
        }
        p->imports[p->import_count]      = path;
        p->import_lines[p->import_count] = pos.line;
        p->import_count++;
    }
}

Module* parse(Token* toks, size_t n, Arena* a, const char* file) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks  = toks;
    p.count = n;
    p.pos   = 0;
    p.arena = a;
    p.file  = file;

    Module* m = arena_alloc(a, sizeof(Module));
    memset(m, 0, sizeof(*m));
    m->file = file;

    int fcap = 0, scap = 0, dcap = 0, ccap = 0;

    while (!check(&p, TK_EOF)) {
        TokKind k = peek(&p)->kind;

        if (k == TK_IMPORT || k == TK_FROM) {
            parse_import(&p);
            continue;
        }

        if (k == TK_PUB) {
            advance(&p);
            continue;   // pub 影响可见性，P0 忽略
        }

        if (k == TK_FN) {
            FuncDecl* f = parse_fn(&p);
            if (f) {
                if (m->func_count == fcap) {
                    fcap = fcap ? fcap * 2 : 8;
                    FuncDecl** nf = arena_alloc(a, fcap * sizeof(FuncDecl*));
                    if (m->funcs) memcpy(nf, m->funcs, m->func_count * sizeof(FuncDecl*));
                    m->funcs = nf;
                }
                m->funcs[m->func_count++] = f;
            }
            continue;
        }

        if (k == TK_STRUCT) {
            StructDecl* d = parse_struct(&p);
            if (d) {
                if (m->struct_count == dcap) {
                    dcap = dcap ? dcap * 2 : 8;
                    StructDecl** nd = arena_alloc(a, dcap * sizeof(StructDecl*));
                    if (m->structs) memcpy(nd, m->structs,
                                           (size_t)m->struct_count * sizeof(StructDecl*));
                    m->structs = nd;
                }
                m->structs[m->struct_count++] = d;
            }
            continue;
        }

        // ---- 类级注解 ----
        //
        // 注解写在 `abstract` / `final` 前面是最自然的读法
        // （`@Entity abstract class`），但也要接受写在 `class` 前面
        // 的形态（`abstract @Entity class`）。两种都收：先吃掉注解，
        // 再把它们挂到随后解析出来的 ClassDecl 上。
        //
        // 这里只在「后面真的跟着类」时才认，否则 `@foo` 会被当成
        // 顶层表达式 —— 而 `@` 在表达式位置没有任何合法用法，那
        // 种情况留给下面的解析器给出准确的错误。
        if (k == TK_AT) {
            size_t save = p.pos;
            AnnotationSet annos = parse_annotations(&p);
            TokKind nk = peek(&p)->kind;
            bool is_cls = (nk == TK_CLASS || nk == TK_INTERFACE);
            bool is_mod = false;
            if (nk == TK_ABSTRACT || nk == TK_FINAL) {
                Token* nn = peek_ahead(&p, 1);
                if (nn && (nn->kind == TK_CLASS || nn->kind == TK_INTERFACE)) {
                    is_mod = true;
                }
            }
            if (is_cls || is_mod) {
                bool was_abstract = false, was_final = false;
                if (is_mod) {
                    was_abstract = peek(&p)->kind == TK_ABSTRACT;
                    was_final    = peek(&p)->kind == TK_FINAL;
                    advance(&p);
                }
                ClassDecl* d = parse_class(&p, peek(&p)->kind == TK_INTERFACE);
                if (d) {
                    d->annos = annos;
                    if (was_abstract) d->is_abstract = true;
                    if (was_final)    d->is_final = true;
                    if (m->class_count == ccap) {
                        ccap = ccap ? ccap * 2 : 8;
                        ClassDecl** nc = arena_alloc(a, (size_t)ccap * sizeof(ClassDecl*));
                        if (m->classes) memcpy(nc, m->classes,
                                               (size_t)m->class_count * sizeof(ClassDecl*));
                        m->classes = nc;
                    }
                    m->classes[m->class_count++] = d;
                }
                continue;
            }
            // 不是类声明：回退，让下面的普通语句解析去报准确的错。
            p.pos = save;
        }

        // ---- 类系统（v0.15） ----
        //
        // 四种起始形态：
        //   class Foo { ... }
        //   abstract class Foo { ... }
        //   final class Foo { ... }
        //   interface Foo { ... }
        //
        // `abstract` / `final` 是 class 的前缀修饰符，所以要先看
        // 它们后面跟的是不是 class —— 否则会把别的东西误吞。
        if (k == TK_CLASS || k == TK_INTERFACE) {
            ClassDecl* d = parse_class(&p, k == TK_INTERFACE);
            if (d) {
                if (m->class_count == ccap) {
                    ccap = ccap ? ccap * 2 : 8;
                    ClassDecl** nc = arena_alloc(a, (size_t)ccap * sizeof(ClassDecl*));
                    if (m->classes) memcpy(nc, m->classes,
                                           (size_t)m->class_count * sizeof(ClassDecl*));
                    m->classes = nc;
                }
                m->classes[m->class_count++] = d;
            }
            continue;
        }

        if (k == TK_ABSTRACT || k == TK_FINAL) {
            // 往前看一个：`abstract class` / `final class` 才算修饰，
            // 否则 `abstract` 是普通标识符（可能是个变量名）。
            Token* nt = peek_ahead(&p, 1);
            if (nt && (nt->kind == TK_CLASS || nt->kind == TK_INTERFACE)) {
                bool was_abstract = (k == TK_ABSTRACT);
                bool was_final    = (k == TK_FINAL);
                advance(&p);
                ClassDecl* d = parse_class(&p, peek(&p)->kind == TK_INTERFACE);
                if (d) {
                    if (was_abstract) d->is_abstract = true;
                    if (was_final)    d->is_final = true;
                    if (m->class_count == ccap) {
                        ccap = ccap ? ccap * 2 : 8;
                        ClassDecl** nc = arena_alloc(a, (size_t)ccap * sizeof(ClassDecl*));
                        if (m->classes) memcpy(nc, m->classes,
                                               (size_t)m->class_count * sizeof(ClassDecl*));
                        m->classes = nc;
                    }
                    m->classes[m->class_count++] = d;
                }
                continue;
            }
            // 落到顶层语句
        }

        // 顶层语句
        if (k == TK_SEMI) { advance(&p); continue; }

        Stmt* s = parse_stmt(&p);
        if (s) {
            if (m->top_count == scap) {
                scap = scap ? scap * 2 : 8;
                Stmt** ns = arena_alloc(a, scap * sizeof(Stmt*));
                if (m->top_stmts) memcpy(ns, m->top_stmts, m->top_count * sizeof(Stmt*));
                m->top_stmts = ns;
            }
            m->top_stmts[m->top_count++] = s;
        }
    }

    // 把收集到的 import 列表交给 Module，供 main.c 递归加载。
    //
    // 为什么不用返回值传两个东西：Module 是这个阶段唯一的产物，
    // 多一个 out 参数会让所有调用点都得处理它。放进 Module 之后
    // 「这个文件依赖谁」成了文件自身的属性，语义也更自然。
    m->imports      = p.imports;
    m->import_count = p.import_count;
    m->import_lines = p.import_lines;

    return m;
}