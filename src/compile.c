// compile.c — AST → 字节码降级（P0）
//
// 关键裁决（来自 01/02/07 文档）：
//
//   1. **字节码带类型**。这是 X 与 CPython 拉开差距的根本。
//      静态类型已知时发射 `ADD_I64` / `ADD_F64`，解释器里直接走 native
//      整数/浮点运算，零类型检查、零装箱。只有编译期真的定不下来时，
//      才退化到 `ADD_NUM`（数值双分派）或 `ADD_DYN`（全动态）。
//      07 §3.2 的实测结论是：这一步就拿到了 6× 优化空间里的 2× 以上。
//
//   2. **栈式 + 局部槽位**。局部变量在编译期分配固定槽位，
//      LOAD_LOCAL/STORE_LOCAL 用立即数寻址，不做哈希查找。
//
//   3. **跳转用函数内相对偏移**（基准是本条指令的下一条），生成时留
//      -1 占位，事后 patch。不用全程序绝对下标是因为它是 int16：
//      绝对下标会把「整个程序 32767 条指令」变成硬上限，而单函数
//      32767 条才是真写不出来的量。
//
//   4. **P0 的类型推断只做四件事**：分清 Int / Float / String / Bool，
//      不做结构化类型、不做泛型、不做函数重载。够跑通测试即可。
//
// P0 已知限制（P1 偿还）：
//   - 块作用域只恢复槽位计数，不做真正的生命周期管理（defer 未实现）
//   - 函数调用不做参数类型检查
//   - 没有闭包，没有数组/结构体

#include "x.h"

// ============================================================
// 编译期类型
// ============================================================
// 比运行时的 ValueType 多一个 UNKNOWN，表示「编译器放弃推断」。
// 遇到 UNKNOWN 时一律选动态指令，把判断推迟到运行时。

typedef enum {
    TY_UNKNOWN = 0,
    TY_INT,
    TY_FLOAT,
    TY_BOOL,
    TY_STRING,
    TY_NULL,
    TY_ARRAY,     // 元素类型不做静态区分（P0 阶段统一当动态）
    TY_STRUCT,    // 结构体。具体是哪个用 Ctx->cur_struct_name 之外的
                  // 方式表达 —— 见下方 TyInfo 的说明。
    TY_CLASS,     // 类实例（引用语义）。同样需要一个平行的类型名字段
                  // 才能说清「是哪个类」。
} TyKind;

static const char* ty_name(TyKind t) {
    switch (t) {
        case TY_INT:    return "Int";
        case TY_FLOAT:  return "Float";
        case TY_BOOL:   return "Bool";
        case TY_STRING: return "String";
        case TY_NULL:   return "Null";
        case TY_ARRAY:  return "Array";
        case TY_STRUCT: return "结构体";
        case TY_CLASS:  return "类实例";
        default:        return "?";
    }
}

// 从源码里写的类型名推回 TyKind（`let x: Int = 1`）
//
// ── 为什么结构体要走查表 ────────────────────────────────────
//
// 其它类型名是**固定集合**，直接 strcmp 就行。结构体名是用户
// 定义的，数量与名字都不可预知，只能问字节码里的类型表。
//
// 这里用一个文件级指针而不是把 Bytecode 一路传进来：ty_from_name
// 有十几个调用点（含 infer_type 这种深层递归），全都改签名会把
// 这个「查一个名字」的小函数变成一次大面积重构，收益却是零 ——
// 编译器本来就是单文件、单线程、一次只编译一个模块。
//
// 指针在 compile_to_bytecode 一开始就设好，模块编译结束后保留
// （不清空）：编译完之后诊断信息里偶尔还会用到它，而清空只会
// 引入「有时能查有时不能」的不确定行为。
static Bytecode* g_cur_bc = NULL;

static bool is_struct_name(const char* n) {
    if (!n || !g_cur_bc) return false;
    return bc_find_struct(g_cur_bc, n) >= 0;
}

static bool is_class_name(const char* n) {
    if (!n || !g_cur_bc) return false;
    return bc_find_class(g_cur_bc, n) >= 0;
}

// 两个类索引是否相同（-1 表示「不在类里」，永远不相等）。
// 单独写一个是为了让可见性检查那段读起来是「是不是同一个类」，
// 而不是「两个 int 相等」。
static bool fu_ci_eq(int a, int b) {
    return a >= 0 && b >= 0 && a == b;
}

// 可见性的可读名字。诊断信息里必须能一眼看出是哪一档 ——
// 只报「不可访问」而不说是哪种，用户就无法判断该改成什么。
static const char* vis_name(Visibility v) {
    switch (v) {
        case VIS_PUBLIC:    return "public（公开）";
        case VIS_PROTECTED: return "protected（保护）";
        case VIS_PRIVATE:   return "private（私有）";
    }
    return "public（公开）";
}

static TyKind ty_from_name(const char* n) {
    if (!n) return TY_UNKNOWN;
    if (strcmp(n, "Int") == 0)     return TY_INT;
    if (strcmp(n, "Float") == 0)   return TY_FLOAT;
    if (strcmp(n, "Bool") == 0)    return TY_BOOL;
    if (strcmp(n, "String") == 0)  return TY_STRING;
    if (strcmp(n, "Array") == 0)   return TY_ARRAY;
    if (strcmp(n, "void") == 0)    return TY_NULL;
    if (strcmp(n, "Void") == 0)    return TY_NULL;
    if (strcmp(n, "Unknown") == 0) return TY_UNKNOWN;
    // 用户定义的类型。类先查 —— 类与结构体同名会是错误，但先命中
    // 哪个决定了报错信息的方向，类优先更符合「class 更常用」的预期。
    if (is_class_name(n))          return TY_CLASS;
    if (is_struct_name(n))         return TY_STRUCT;
    return TY_UNKNOWN;
}

// ============================================================
// 用户函数签名表
// ============================================================
// 调用一个用户函数时，编译器必须知道它的返回类型，
// 否则 `1 + f()` 无法决定发射 ADD_I64 还是 ADD_DYN。
// 这张表在编译开始前从 Module 建立，支持前向引用与互递归。

typedef struct {
    const char* name;
    int         param_count;
    // 每个形参的声明类型。NULL 表示该形参没写类型注解（此时不做检查）。
    // 存的是原样的类型名字符串而不是 TyKind，因为这些名字来自
    // FuncDecl->param_types，而 TyKind 的描述能力比它窄
    // （比如 Int8 之类以后要支持的类型在这里就会丢信息）。
    char**      param_types;
    TyKind      ret_type;
    // 返回类型的原样名字。TY_STRUCT 只说明「是个结构体」，说不清
    // 是哪个 —— 而 `fn mk() -> Rect` 之后再 `.x` 必须知道是 Rect。
    // 其余类型的名字可以从 TyKind 反推，只有结构体不行，所以这里
    // 只在 ret_type == TY_STRUCT 时才真正需要它。
    char*       ret_type_name;
    int         bc_index;    // 对应 bc->funcs 的下标
    bool        defined;
} FuncSig;

typedef struct {
    FuncSig* sigs;
    int      count;
    int      cap;
} FuncTable;

static void ft_init(FuncTable* ft) {
    memset(ft, 0, sizeof(*ft));
}

static FuncSig* ft_add(FuncTable* ft, const char* name, int param_count, TyKind ret) {
    if (ft->count == ft->cap) {
        ft->cap = ft->cap ? ft->cap * 2 : 8;
        ft->sigs = x_realloc(ft->sigs, (size_t)ft->cap * sizeof(FuncSig));
    }
    FuncSig* s = &ft->sigs[ft->count++];
    s->name        = name;
    s->param_count = param_count;
    s->param_types = NULL;
    s->ret_type    = ret;
    s->ret_type_name = NULL;
    s->bc_index    = -1;
    s->defined     = false;
    return s;
}

static FuncSig* ft_find(FuncTable* ft, const char* name) {
    for (int i = 0; i < ft->count; i++) {
        if (strcmp(ft->sigs[i].name, name) == 0) return &ft->sigs[i];
    }
    return NULL;
}

// ============================================================
// 重载解析（同名不同签名的函数，调用点按实参类型挑选）
// ============================================================
//
// 语言层面：`fn 查找(s: String, 子: String)` 与
// `fn 查找(xs: Array, v: Any)` 可以并存。调用点逐参数比对实参的
// 静态类型，选出唯一的可行候选。注册端的判定见 compile_program：
// 参数签名完全一致才算重复定义，参数不同即合法重载。
//
// 规则（依次）：
//   1. 参数个数必须一致；
//   2. 逐参数匹配：形参类型 == 实参类型；形参通配（未注解 / Any /
//      Unknown）时什么实参都接；Int↔Float 互相放行（与单签名版本
//      的数值放宽一致）；
//   3. 实参类型推不出来（UNKNOWN）时不淘汰候选，只影响「具体度」
//      打分：精确匹配 > 数值放行 > 实参未知时的具体形参 > 通配形参；
//   4. 最高分并列多于一个 → 歧义，报错而不是猜。
//
// 为什么在编译期做：BC_CALL 按函数**下标**调用（不做名字查找），
// 决策推迟到运行时就得给每个调用点加动态分派表；而这里需要的全部
// 信息（实参静态类型）编译期已经齐了。
//
// 兼容性：名字只对应一个签名、但实参个数不对时，沿用旧文案
// 「函数 `x` 需要 N 个参数，实际给了 M 个」—— 既有诊断场景断言
// 这句话，而且单签名下它确实比「没有匹配的重载」更直白。
static FuncSig* resolve_overload(FuncTable* ft, const char* name, int argc,
                                 const TyKind* argt, SrcPos pos,
                                 bool* exists, bool quiet) {
    *exists = false;

    // 收集：同名且参数个数一致的候选
    FuncSig* cand[32];
    int ncand = 0;
    int ntotal = 0;
    for (int i = 0; i < ft->count; i++) {
        FuncSig* t = &ft->sigs[i];
        if (strcmp(t->name, name) != 0) continue;
        *exists = true;
        ntotal++;
        if (t->param_count == argc && ncand < 32) cand[ncand++] = t;
    }

    if (!*exists) return NULL;   // 名字完全不存在 → 调用方按「未定义」报

    if (ncand == 0) {
        if (quiet) return NULL;
        if (ntotal == 1) {
            FuncSig* t = ft_find(ft, name);
            diag_error(pos, "函数 `%s` 需要 %d 个参数，实际给了 %d 个",
                       name, t->param_count, argc);
            diag_help("检查 `fn %s` 的声明：它接受 %d 个参数",
                      name, t->param_count);
        } else {
            diag_error(pos, "没有实参个数为 %d 的 `%s` 重载。可用签名：",
                       argc, name);
            for (int i = 0; i < ft->count; i++) {
                FuncSig* t = &ft->sigs[i];
                if (strcmp(t->name, name) != 0) continue;
                diag_note("  %s（%d 个参数）", name, t->param_count);
            }
        }
        return NULL;
    }

    // 只有一个候选时不用打分 —— 但仍要保证它与实参「兼容吗」？
    // 不需要：类型不匹配由调用方原有的参数类型检查报（文案更精确，
    // 指到具体第几个参数），这里不做提前淘汰。
    if (ncand == 1) return cand[0];

    FuncSig* best = NULL;
    int best_score = -1;
    FuncSig* best_tie = NULL;
    for (int i = 0; i < ncand; i++) {
        FuncSig* t = cand[i];
        int score = 0;
        bool ok = true;
        for (int j = 0; j < argc && ok; j++) {
            const char* pn = (t->param_types && j < t->param_count)
                                 ? t->param_types[j] : NULL;
            TyKind pt = ty_from_name(pn);   // 未注解 / Any / Unknown → 通配
            TyKind at = argt[j];
            if (pt == TY_UNKNOWN) {
                score += 0;                 // 通配形参：最不具体
            } else if (at == TY_UNKNOWN) {
                score += 1;                 // 实参未知：具体形参仍占优
            } else if (pt == at) {
                score += 2;                 // 精确匹配
            } else if ((pt == TY_INT && at == TY_FLOAT) ||
                       (pt == TY_FLOAT && at == TY_INT)) {
                score += 1;                 // 数值放行，比精确匹配弱
            } else {
                ok = false;                 // 类型不匹配，此候选出局
            }
        }
        if (!ok) continue;
        if (score > best_score) {
            best_score = score;
            best = t;
            best_tie = NULL;
        } else if (score == best_score && t != best) {
            best_tie = t;
        }
    }

    if (!best) {
        if (quiet) return NULL;
        diag_error(pos, "没有与实参类型匹配的 `%s` 重载", name);
        diag_note("实参类型依次为：");
        for (int j = 0; j < argc; j++) {
            diag_note("  第 %d 个：%s", j + 1, ty_name(argt[j]));
        }
        diag_note("可用签名（%d 个）：", ncand);
        for (int i = 0; i < ncand; i++) {
            FuncSig* t = cand[i];
            char sigbuf[256];
            size_t w = 0;
            w += (size_t)snprintf(sigbuf + w, sizeof(sigbuf) - w, "  %s(", name);
            for (int j = 0; j < t->param_count && w < sizeof(sigbuf) - 8; j++) {
                const char* pn = (t->param_types && t->param_types[j])
                                     ? t->param_types[j] : "Any";
                w += (size_t)snprintf(sigbuf + w, sizeof(sigbuf) - w,
                                      "%s%s", j ? ", " : "", pn);
            }
            snprintf(sigbuf + w, sizeof(sigbuf) - w, ")");
            diag_note("%s", sigbuf);
        }
        diag_help("用 toInt / toFloat / toString 显式转换实参，"
                  "或给其中一个重载改名");
        return NULL;
    }

    if (best_tie) {
        if (quiet) return NULL;
        diag_error(pos, "对 `%s` 的调用有歧义：实参类型同时匹配多个重载", name);
        diag_note("两个候选的参数具体度打分相同，编译器拒绝猜。");
        diag_help("给实参加显式转换（toInt / toFloat / toString），"
                  "或给其中一个重载改名");
        return NULL;
    }
    return best;
}

// ============================================================
// 编译上下文
// ============================================================
// 全局常量表（顶层 `const`）
// ============================================================
//
// ── 为什么需要这个 ──────────────────────────────────────────
//
// 语言故意没有全局变量（01 §10）。但「全局变量」和「全局常量」是
// 两件事：前者是可变的共享状态，会引来所有并发与初始化顺序问题；
// 后者只是一个命名好的字面量，语义上等价于在每个使用点重写一遍。
//
// 写游戏时这个区别立刻显形：棋盘的列数、单元格尺寸、九种颜色值，
// 这些必须能在任何函数里引用。没有它们，代码会退化成到处传参
// 或者到处写魔法数字 —— 两者都是真实的可维护性灾难。
//
// ── P0 的实现边界 ──────────────────────────────────────────
//
// 只接受**编译期可求值的字面量**：Int / Float / String / Bool / Null。
// 不做常量表达式折叠（`const A = B + 1` 报错），理由：
//   · 折叠需要一套常量求值器，算错时的症状是「常量值不对」，
//     比直接报错难查得多
//   · 折叠会让「哪个是常量、哪个是运行时计算」变得不清晰
//   · 真正需要折叠时，用户写 `const A = 11` 就行了
//
// 从 P1 开始，`const X = comptime { ... }` 会接管这块（01 §7.1）。
//
// ── 为什么用「直接发射字面量」而不是给常量分配存储 ──────────
//
// 后者需要一套全局数据段、初始化代码、以及 LOAD_GLOBAL 指令，
// 还要处理 AOT 侧的对应物。前者的实现是「在引用点发射和字面量
// 一样的指令」，运行时**完全不知道常量的存在** —— 零新指令、
// 零新运行时状态、AOT 侧零改动。
//
// 代价：每次引用多一条指令。这恰好和「直接写字面量」的开销一样，
// 所以没有任何性能损失。

// 前向声明：常量表的求值与发射都要用到 Ctx（局部变量表），
// 而 Ctx 又需要 ConstTable 字段。两者互相引用，只能靠前向声明打破。
//
// （这个 typedef 在文件更靠前的地方已经出现过一次，那是给可见性
// 检查函数用的。C 允许重复的 typedef，只要指向同一个类型 —— 两处
// 都保留是有意的：删掉任何一处都会让「读者得往回翻几百行才知道
// Ctx 是什么」。）

typedef enum {
    CONST_KIND_NONE = 0,
    CONST_KIND_INT,
    CONST_KIND_FLOAT,
    CONST_KIND_STRING,
    CONST_KIND_BOOL,
    CONST_KIND_NULL,
} GlobalConstKind;

typedef struct {
    char*            name;
    GlobalConstKind  kind;
    int64_t          i;
    double           f;
    char*            s;      // 字符串值（不含长度，UTF-8，NUL 结尾）
} GlobalConst;

typedef struct {
    GlobalConst* items;
    int          count;
    int          cap;
} ConstTable;

static void ct_init(ConstTable* t) {
    memset(t, 0, sizeof(*t));
}

static GlobalConst* ct_find(ConstTable* t, const char* name) {
    // 从后往前：后定义的遮蔽先定义的（与局部变量的作用域规则一致）
    for (int i = t->count - 1; i >= 0; i--) {
        if (strcmp(t->items[i].name, name) == 0) return &t->items[i];
    }
    return NULL;
}

static GlobalConst* ct_add(ConstTable* t, const char* name) {
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 32;
        t->items = x_realloc(t->items, (size_t)cap * sizeof(GlobalConst));
        memset(&t->items[t->cap], 0, (size_t)(cap - t->cap) * sizeof(GlobalConst));
        t->cap = cap;
    }
    GlobalConst* g = &t->items[t->count++];
    memset(g, 0, sizeof(*g));
    g->name = x_strdup(name);
    return g;
}

static void ct_free(ConstTable* t) {
    for (int i = 0; i < t->count; i++) {
        free(t->items[i].name);
        free(t->items[i].s);
    }
    free(t->items);
    memset(t, 0, sizeof(*t));
}

// 从初始化表达式里取出编译期值。失败时给出针对性诊断并返回 false。
//
// 为什么支持一元负号：`const MIN = -40` 是极其常见的写法，
// 而 `-40` 在 AST 里是 UNARY(NEG, INT(40))。不支持的话用户会
// 得到一个「常量必须是字面量」的错误，然后困惑「这明明是字面量」。
static bool ct_extract(GlobalConst* g, Expr* e) {
    if (!e) {
        diag_error(g->name ? (SrcPos){ NULL, 0, 0 } : (SrcPos){ NULL, 0, 0 },
                   "内部错误：常量为空");
        return false;
    }

    switch (e->kind) {
        case EX_INT:
            g->kind = CONST_KIND_INT;
            g->i = e->i.v;
            return true;
        case EX_FLOAT:
            g->kind = CONST_KIND_FLOAT;
            g->f = e->f.v;
            return true;
        case EX_STRING:
            g->kind = CONST_KIND_STRING;
            // str.s 的内存归 AST 的 arena 所有，活到编译结束即可 ——
            // 但常量表要在编译期一直被查，所以复制一份由自己管理，
            // 避免将来 AST 生命周期变化时出现悬空指针。
            g->s = x_strndup(e->str.s, e->str.len);
            return true;
        case EX_BOOL:
            g->kind = CONST_KIND_BOOL;
            g->i = e->b.v ? 1 : 0;
            return true;
        case EX_NULL:
            g->kind = CONST_KIND_NULL;
            return true;
        case EX_UNARY:
            if (e->unary.op == UOP_NEG) {
                Expr* inner = e->unary.operand;
                if (inner->kind == EX_INT) {
                    g->kind = CONST_KIND_INT;
                    g->i = -inner->i.v;
                    return true;
                }
                if (inner->kind == EX_FLOAT) {
                    g->kind = CONST_KIND_FLOAT;
                    g->f = -inner->f.v;
                    return true;
                }
            }
            break;
        default:
            break;
    }

    diag_error(e->pos,
               "顶层 `const` 的初始值必须是字面量（Int / Float / String / Bool / null）");
    diag_note("P0 不做常量表达式折叠，所以 `const A = B + 1` 这种写法还不支持"
              "（01 §7.1 把 comptime 排在 P1）。");
    diag_help("直接写成最终值：`const A = 11`，而不是 `const A = 10 + 1`");
    return false;
}

// ============================================================


#define MAX_LOOP_DEPTH 64

typedef struct Ctx_s Ctx;

typedef struct LoopCtx {
    int* breaks;         int break_count;    int break_cap;
    int* continues;      int continue_count; int continue_cap;
} LoopCtx;

struct Ctx_s {
    Bytecode*  bc;
    Arena*     arena;
    FuncTable* fts;
    ConstTable* ct;      // 顶层 const 常量表（见上方 GlobalConst 的说明）

    // 局部变量表：槽位 i 的名字与类型
    char**  local_names;
    TyKind* local_types;
    // 与 local_types 平行：结构体变量的**具体**类型名。
    //
    // ── 为什么 TyKind 不够 ──────────────────────────────────
    //
    // TY_STRUCT 只回答「是个结构体」，回答不了「是哪个」。而
    // `r.x` 必须知道 r 是 Rect 才能查字段表 —— 不知道的话，
    // 每个成员访问都得推迟到运行时，那等于放弃了编译期类型检查，
    // 而「w/h 传反能被编译期抓到」正是加结构体的全部理由。
    //
    // 非结构体槽位这里是 NULL。用平行数组而不是把 TyKind 换成
    // 一个带名字的结构体：后者要改所有 local_type / local_set_type
    // 的调用点（几十处），而收益只是省一个数组。
    char**  local_type_names;
    int     local_count;
    int     local_cap;

    // 声明的槽位峰值。
    // 为什么需要单独记：块作用域退出时 local_count 会回退（名字失效），
    // 但那些槽位在字节码里仍会被 STORE_LOCAL 访问到。
    // 帧必须按峰值分配，否则局部变量区会和操作数栈重叠 —— 这个 bug
    // 的表现是「嵌套块里声明的变量把栈顶覆盖了」，极难定位。
    int     max_local_count;

    // 当前正在编译的函数
    FuncSig* cur_func;

    // 当前正在编译的方法属于哪个类（普通函数里为 NULL）。
    //
    // 用途只有一处：private / protected 的可见性检查。放在 Ctx 上而不是
    // 作为参数一路传，是因为可见性检查发生在 EX_MEMBER 的编译里 ——
    // 那是个深层递归点，加参数要改十几个函数的签名。
    const char* cur_class;

    // 当前正在编译的方法名（不含类名前缀）。构造函数里的 final 字段
    // 赋值检查要用它 —— final 字段只允许在构造函数中赋值。
    const char* cur_method;

    // 当前模块。只有静态方法的可见性检查需要它 ——
    //
    // 静态方法不进虚表（没有运行时派发的必要），所以它没有「槽」可以
    // 索引，ClassType 里也就没有它的可见性。判据只能回到 ClassDecl：
    // 拿类名 + 方法名在 Module 的类声明里查那一条 ClassMethod。
    //
    // 为一个检查挂一个指针，代价是 8 字节；替代方案是把静态方法的
    // 可见性也塞进 ClassType（比如再开一张 method_static_vis 表），
    // 那要多一次序列化、多一处可能漏写的地方，而静态方法本来就不
    // 参与运行时任何操作。
    Module* cur_mod;

    LoopCtx loop[MAX_LOOP_DEPTH];
    int     loop_depth;
};

// ============================================================
// 可见性检查
// ============================================================
//
// 放在 Ctx 定义之后：两个函数都要读 c->bc / c->cur_class / c->cur_mod，
// 前向声明只能骗过编译器，读不懂字段。见文件开头 vis_name 附近
// 的说明。
//
// 方法可见性检查。返回 true 表示允许调用；false 表示已经报过错。
//
// ── 判据与字段完全一致 ──────────────────────────────────────
//
// private   只有声明它的类（含该类所有方法）能调
// protected 本类 + 子类
// public    谁都能
//
// ── 为什么按「槽 + 声明类」判，而不是只看静态类型 ----
//
// `s.area()` 里 s 的静态类型可能是父类，而实现在子类。可见性是
// **声明处**的属性，不是调用点的属性。所以要沿继承链找到最早声明
// 这个方法的那个类，按它的 method_vis 判。
//
// 好消息是槽是全局唯一的：同名方法在所有类里共用一个槽。所以
// 「沿链找声明类」只需找第一个 vtable[slot] != -1 的祖先，它的
// method_vis[slot] 就是这次调用的门槛。
//
// 找不到声明类（整条链上都没实现）说明调用的是抽象方法 —— 那一
// 定是接口/抽象类声明的，按 public 处理（能过编译期，运行时会
// 因为派发不到而报错，那是另一个错误）。
static bool check_method_visible(Ctx* c, int ci, int slot,
                                 const char* cls_tn, const char* mname,
                                 SrcPos pos) {
    if (!c->bc || ci < 0 || slot < 0) return true;
    if (slot >= c->bc->classes[ci].vtable_len) return true;

    int owner_ci = -1;
    Visibility need = VIS_PUBLIC;
    for (int cur = ci; cur >= 0; cur = c->bc->classes[cur].parent_idx) {
        ClassType* o = &c->bc->classes[cur];
        if (slot < o->vtable_len && o->vtable[slot] != -1) {
            owner_ci = cur;
            need = o->method_vis ? o->method_vis[slot] : VIS_PUBLIC;
            break;
        }
    }
    if (owner_ci < 0 || need == VIS_PUBLIC) return true;

    bool allowed = false;
    if (c->cur_class) {
        int cur_ci = bc_find_class(c->bc, c->cur_class);
        if (fu_ci_eq(cur_ci, owner_ci)) {
            allowed = true;             // 本类
        } else if (need == VIS_PROTECTED) {
            // 子类可以调父类的 protected
            allowed = bc_class_is_a(c->bc, cur_ci, owner_ci);
        }
    }
    if (allowed) return true;

    const char* owner_name = c->bc->classes[owner_ci].name;
    // 「不能在 X 里调用」在 cur_class 为空时会拼出「不能在类外里」——
    // 多一个「里」，读起来像是打字错误。所以分两路：
    // 有当前类时报类名，没有时报「从类外」。
    if (c->cur_class) {
        diag_error(pos, "方法 `%s.%s` 是 %s，不能在类 `%s` 里调用",
                   owner_name, mname, vis_name(need), c->cur_class);
    } else {
        diag_error(pos, "方法 `%s.%s` 是 %s，不能从类外调用",
                   owner_name, mname, vis_name(need));
    }
    diag_help(need == VIS_PRIVATE
              ? "private 方法只有声明它的类内部能调。"
                "需要外部调用就改成 public，或者在该类里加一个公开入口方法"
              : "protected 方法只对本类与子类开放。"
                "外部调用请改成 public，或者加一个公开入口方法");
    (void)cls_tn;
    return false;
}

// 静态方法的可见性检查。
//
// ── 为什么不能用 check_method_visible ----
//
// 静态方法不进虚表（它在编译期就能确定调哪个），所以 ClassType 里
// 没有它的槽，也就没有 method_vis。判据只能回到 AST 的 ClassDecl：
// 按「类名 + 方法名」找到那条 ClassMethod，读它的 vis 字段。
//
// 这是整个可见性体系里唯一一处需要 AST 的地方。它是编译期的、
// 只在方法调用点触发一次，不构成性能问题。
static bool check_static_visible(Ctx* c, const char* cls, const char* mname,
                                 SrcPos pos) {
    if (!c->cur_mod) return true;

    // 沿继承链找：静态方法不被覆写（也不进虚表），但子类可以通过
    // 父类名调到父类的静态方法，所以要允许「本类没有就往上找」。
    const char* look = cls;
    for (int guard = 0; guard < 64 && look; guard++) {
        ClassDecl* d = NULL;
        for (int k = 0; k < c->cur_mod->class_count; k++) {
            if (strcmp(c->cur_mod->classes[k]->name, look) == 0) {
                d = c->cur_mod->classes[k];
                break;
            }
        }
        if (!d) return true;

        for (int k = 0; k < d->method_count; k++) {
            ClassMethod* cm = &d->methods[k];
            if (strcmp(cm->name, mname) != 0) continue;
            if (cm->vis == VIS_PUBLIC) return true;

            bool allowed = false;
            if (c->cur_class) {
                int cur_ci = bc_find_class(c->bc, c->cur_class);
                int own_ci = bc_find_class(c->bc, d->name);
                if (fu_ci_eq(cur_ci, own_ci)) {
                    allowed = true;
                } else if (cm->vis == VIS_PROTECTED) {
                    allowed = bc_class_is_a(c->bc, cur_ci, own_ci);
                }
            }
            if (allowed) return true;

            if (c->cur_class) {
                diag_error(pos, "静态方法 `%s.%s` 是 %s，不能在类 `%s` 里调用",
                           d->name, mname, vis_name(cm->vis), c->cur_class);
            } else {
                diag_error(pos, "静态方法 `%s.%s` 是 %s，不能从类外调用",
                           d->name, mname, vis_name(cm->vis));
            }
            diag_help(cm->vis == VIS_PRIVATE
                      ? "private 静态方法只有声明它的类内部能调。"
                        "需要外部调用就改成 public"
                      : "protected 静态方法只对本类与子类开放。"
                        "外部调用请改成 public");
            return false;
        }
        look = d->parent_name;
    }
    return true;
}


// ============================================================
// 局部变量
// ============================================================

static int local_find(Ctx* c, const char* name) {
    // 从后往前找：后声明的遮蔽先声明的（与 01 §4 的作用域规则一致）
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (strcmp(c->local_names[i], name) == 0) return i;
    }
    return -1;
}

static int local_add(Ctx* c, const char* name, TyKind t) {
    if (c->local_count == c->local_cap) {
        int cap = c->local_cap ? c->local_cap * 2 : 16;
        c->local_names      = x_realloc(c->local_names, (size_t)cap * sizeof(char*));
        c->local_types      = x_realloc(c->local_types, (size_t)cap * sizeof(TyKind));
        c->local_type_names = x_realloc(c->local_type_names, (size_t)cap * sizeof(char*));
        // 新分配的区域必须清零：local_add 会 free 掉槽位里的旧名字，
        // 若这里留的是 malloc 的垃圾指针，第一次声明变量就会 free 野指针。
        for (int i = c->local_cap; i < cap; i++) {
            c->local_names[i]      = NULL;
            c->local_types[i]      = TY_UNKNOWN;
            c->local_type_names[i] = NULL;
        }
        c->local_cap = cap;
    }
    int slot = c->local_count++;
    free(c->local_names[slot]);           // 复用槽位时释放旧名字（NULL 时安全）
    free(c->local_type_names[slot]);
    c->local_names[slot]      = x_strdup(name);
    c->local_types[slot]      = t;
    c->local_type_names[slot] = NULL;
    if (c->local_count > c->max_local_count) {
        c->max_local_count = c->local_count;
    }
    return slot;
}

// 登记某个槽位的具体结构体类型名。
// 单独一个函数而不是给 local_add 加参数：绝大多数声明点不需要它，
// 加参数会让所有调用点都得传一个 NULL。
static void local_set_type_name(Ctx* c, int slot, const char* tn) {
    if (slot < 0 || slot >= c->local_count) return;
    free(c->local_type_names[slot]);
    c->local_type_names[slot] = tn ? x_strdup(tn) : NULL;
}

static const char* local_type_name(Ctx* c, int slot) {
    if (slot < 0 || slot >= c->local_count) return NULL;
    return c->local_type_names[slot];
}

static TyKind local_type(Ctx* c, int slot) {
    if (slot < 0 || slot >= c->local_count) return TY_UNKNOWN;
    return c->local_types[slot];
}

static void local_set_type(Ctx* c, int slot, TyKind t) {
    if (slot < 0 || slot >= c->local_count) return;
    c->local_types[slot] = t;
}

static void locals_clear(Ctx* c) {
    for (int i = 0; i < c->local_count; i++) {
        free(c->local_names[i]);
        c->local_names[i] = NULL;
        free(c->local_type_names[i]);
        c->local_type_names[i] = NULL;
    }
    c->local_count = 0;
    c->max_local_count = 0;   // 每个函数独立计峰值
}

// ============================================================
// 发射辅助
// ============================================================

static int emit(Ctx* c, OpCode op, int32_t a, SrcPos pos) {
    return bc_emit(c->bc, op, a, 0, pos);
}

// 把常量按字面量指令发射到栈上。
static TyKind ct_emit(Ctx* c, GlobalConst* g, SrcPos pos) {
    switch (g->kind) {
        case CONST_KIND_INT:
            emit(c, BC_PUSH_I64, bc_add_const_i64(c->bc, g->i), pos);
            return TY_INT;
        case CONST_KIND_FLOAT:
            emit(c, BC_PUSH_F64, bc_add_const_f64(c->bc, g->f), pos);
            return TY_FLOAT;
        case CONST_KIND_STRING: {
            int k = bc_add_const_str(c->bc, g->s, strlen(g->s));
            emit(c, BC_PUSH_STR, k, pos);
            return TY_STRING;
        }
        case CONST_KIND_BOOL:
            emit(c, g->i ? BC_PUSH_TRUE : BC_PUSH_FALSE, 0, pos);
            return TY_BOOL;
        case CONST_KIND_NULL:
        default:
            emit(c, BC_PUSH_NULL, 0, pos);
            return TY_NULL;
    }
}

// ============================================================



// 发射一个占位跳转，返回其索引，稍后用 patch 填目标
static int emit_jump(Ctx* c, OpCode op, SrcPos pos) {
    return bc_emit(c->bc, op, -1, 0, pos);
}

// ── 跳转操作数：函数内相对偏移 ───────────────────────────
//
// ── 为什么不能存绝对指令下标 ──────────────────────────
//
// 操作数是 int16（见 x.h 的 Insn：为把指令压到 8 字节）。如果
// 存的是「全程序第几条指令」，那么**整个程序**一旦超过 32767 条
// 就会编译失败 —— 而这不是大项目的特权：本项目的 UI 组件库
// 单独编出来就有 2.8 万条，加一个用到它的程序立刻触顶。
//
// 第一次撞上来的表现很误导人：报错指向某个 `switch` 里的一行
// 普通赋值，说「字节码操作数超出 P0 限制（a=32955）」。那行代码
// 本身没问题，真正的原因是它后面的循环跳转要跳到第 32955 条。
//
// ── 相对偏移为什么够 ─────────────────────────────
//
// 跳转只可能落在**同一个函数内**（P0 没有跨函数跳转）。而单文件
// 4 MiB 的源码上限决定的单函数长度远小于 32767 条。所以存「相对
// 本条指令的下一条偏移多少」就够了，限制从「程序规模」降级到
// 「函数规模」—— 前者是硬的、不可绕的，后者是真写不出来。
//
// ── 相对谁 ──────────────────────────────────
//
// 相对**本条指令的下一条**（即执行完本条之后的 pc）。这样解释器
// 和 AOT 两边都是 `pc = pc + a`，不需要知道本条指令的绝对地址，
// 也不需要函数基址。
//
// 顺带还有一个好处：函数整体搬家（比如前面某函数被优化掉几条）
// 时，函数内部的跳转**完全不用调整**。
static void patch_to(Ctx* c, int insn_idx, int target) {
    // target 是调用点给的绝对指令下标（bc->code 的下标）。
    // 换算成「从本条指令的下一条开始偏移多少」。
    int rel = target - (insn_idx + 1);
    if (rel > BC_OPERAND_MAX || rel < BC_OPERAND_MIN) {
        diag_error(c->bc->pos[insn_idx],
                   "单个函数太长，超过了 P0 的跳转距离上限（%d 条指令）",
                   BC_OPERAND_MAX);
        diag_help("把这个函数拆成几个小函数。跳转距离只受**单个函数**"
                  "长度限制，与整个程序大小无关");
        rel = 0;
    }
    c->bc->code[insn_idx].a = (int16_t)rel;
}

// 回填到「当前位置」（函数末尾兜底 RETURN 之前）
static void patch_here(Ctx* c, int insn_idx) {
    patch_to(c, insn_idx, c->bc->code_count);
}

// ============================================================
// 类型规则
// ============================================================

// 两数运算符操作数类型 → 结果类型
// 只要有一边是 Float 就提升到 Float（01 §4.3 的数值提升规则）
static TyKind arith_result(TyKind lt, TyKind rt) {
    if (lt == TY_FLOAT || rt == TY_FLOAT) return TY_FLOAT;
    return TY_INT;
}

// 两操作数都是已知同类且确定 → 可以用特化指令
static bool both_definitely_int(TyKind lt, TyKind rt) {
    return lt == TY_INT && rt == TY_INT;
}

static bool both_definitely_float(TyKind lt, TyKind rt) {
    return lt == TY_FLOAT && rt == TY_FLOAT;
}

// ============================================================
// 前向声明
// ============================================================

static TyKind compile_expr(Ctx* c, Expr* e);

// 合并 if / match / 三元这类「多分支表达式」的结果类型。
//
// 抽成公共函数而不在各分支里各抄一遍，是因为这几处的规则必须
// **逐字一致**。一旦分歧，症状是「同样的两个值，写成 if 能过、
// 写成三元报类型错」—— 而那种不一致读者根本无法从报错里看出来。
//
// 规则本身（按顺序）：
//   1. 两支同型 → 该型
//   2. 一支 Unknown → 取另一支（Unknown 表示「推不出来」，不是「类型不对」）
//   3. 一支 Float → Float（Int 与 Float 混用时按 Float 算）
//   4. 其余 → 取真支的类型（保守，让后续使用点去报具体的错）
static TyKind ty_merge_branches(TyKind a, TyKind b) {
    if (a == b) return a;
    if (a == TY_UNKNOWN) return b;
    if (b == TY_UNKNOWN) return a;
    // TY_NULL 与 TY_UNKNOWN 一样，表示「这支没有可用的类型信息」。
    //
    // ── 这条曾经漏了，症状极具误导性 ────────────────────────
    //
    // `null ?? 8` 的值是对的（栈上确实是 8），但 println 打印出
    // "null"。原因是 println 的打印指令在**编译期**按推断出的类型
    // 选：TY_INT → TOSTRING_I64，TY_NULL → TOSTRING_NULL，而
    // TOSTRING_NULL 根本不读栈、直接打印 "null"。
    //
    // 所以「值算对了」和「显示对了」是两件事，而错的那件完全不
    // 指向类型推断。好在修在源头就够了。
    if (a == TY_NULL) return b;
    if (b == TY_NULL) return a;
    // 两支都是同一数值类别才合并。Int 与 Float 混**不能**直接取 Float：
    // 两支各自按自己的类型压栈，真支压的是 int64，而声明成 Float 之后
    // 上层会发 TOSTRING_F64 / 按 double 读那 8 个字节 —— 1 的位模式
    // 读出来是 5e-324。值错了，但错得完全不指向类型合并。
    //
    // 所以混合时给 TY_UNKNOWN，让上层走动态路径按 tag 取。代价是丢了
    // 一点静态信息（少一次特化打印），换的是不撒谎。
    if (a == b) return a;
    return TY_UNKNOWN;
}

// 求一个表达式的**具体**结构体类型名。不是结构体或追不到时返回 NULL。
//
// ── 为什么需要这个函数，而不能只靠 TyKind ─────────────────
//
// TyKind 里只有一个 TY_STRUCT，它回答「是个结构体」。但 `r.x` 必须
// 知道 r 是 Rect 才能查 Rect 的字段表。少这一层信息，成员访问就只能
// 推迟到运行时查 —— 那等于把「w/h 传反」这类错误放回运行时，
// 而抓住它们正是加结构体的全部理由。
//
// ── 支持哪些形态 ──────────────────────────────────────────
//
//   r              局部变量（靠 local_type_names）
//   Rect { ... }   字面量（类型名就写在语法里）
//   r.center       链式访问（递归：先求 r 的类型，再查字段类型）
//   mkRect()       函数调用（靠 FuncSig->ret_type_name）
//   (expr)         括号 —— parser 不产生节点，无需处理
//   a[i]           不支持：数组元素类型不做静态区分（同 TY_ARRAY）
//
// 其余一律 NULL，调用点据此给「无法确定宿主类型」并提示怎么改写。
// 这个函数**不发射任何指令**，纯查询，可以放心在 infer_type 里调。
static const char* struct_name_of_ty(const char* tn) {
    if (!tn) return NULL;
    return is_struct_name(tn) ? tn : NULL;
}

static const char* expr_class_name(Ctx* c, Expr* e);

static const char* expr_struct_name(Ctx* c, Expr* e) {
    if (!e) return NULL;

    switch (e->kind) {
        case EX_IDENT: {
            int slot = local_find(c, e->ident.name);
            if (slot < 0) return NULL;
            return local_type_name(c, slot);
        }

        case EX_STRUCT_LIT:
            return is_struct_name(e->slit.type_name) ? e->slit.type_name : NULL;

        // 可选链降级时造出的伪节点：类型名由降级方算好塞在
        // ident.name 里（见 EX_STACK_REF 的注释）。
        case EX_STACK_REF:
            return e->ident.name && is_struct_name(e->ident.name)
                       ? e->ident.name : NULL;

        // `a?.b` 的静态类型 = `a.b` 的静态类型，只是多了「可能为 null」。
        //
        // 这里必须**透传**类型名，不能一律返回 NULL。反例：
        // `i?.a?.v` —— 内层 `i?.a` 的值可能是 null，但它的**类型**
        // 确实是 Inner（当 i 非 null 时）。不透传的话，外层 `.v`
        // 会因为拿不到宿主名而报「无法确定 .v 的宿主类型」，
        // 而用户的写法完全合法。
        //
        // null 的可能性由可选链自己的 null 检查兜住（外层 `?.` 会
        // 再判一次），这里只负责回答「宿主是什么类型」这个问题。
        case EX_OPT_CHAIN: {
            Expr* acc = e->optc.access;
            if (acc->kind == EX_MEMBER) return expr_struct_name(c, acc);
            return NULL;
        }

        case EX_CALL: {
            // 方法调用：`a.inner()` 返回结构体时，链式访问要继续能追。
            if (e->call.callee->kind == EX_MEMBER) {
                Expr* m = e->call.callee;
                const char* tn = expr_struct_name(c, m->member.obj);
                if (!tn && m->member.obj->kind == EX_IDENT &&
                    is_struct_name(m->member.obj->ident.name)) {
                    tn = m->member.obj->ident.name;   // 静态方法
                }
                if (!tn) return NULL;
                // 手工拼接而不是 snprintf("%s.%s")。
                //
                // gcc 的 -Wformat-truncation 只在能证明两个 %s 之和
                // 不超过目标大小时才放行，而 tn / name 的 strlen 是
                // 运行时值 —— 它证明不了，于是无论缓冲开到多大都报警。
                // 改成显式长度计算 + memcpy，警告消失，而且这里本来
                // 就只需要「知道确切长度」，printf 的格式化是多余的。
                size_t ltn = strlen(tn), lmn = strlen(m->member.name);
                char full[512];
                if (ltn + 1 + lmn + 1 > sizeof(full)) return NULL;
                memcpy(full, tn, ltn);
                full[ltn] = '.';
                memcpy(full + ltn + 1, m->member.name, lmn);
                full[ltn + 1 + lmn] = '\0';
                FuncSig* fs = ft_find(c->fts, full);
                return fs ? struct_name_of_ty(fs->ret_type_name) : NULL;
            }
            if (e->call.callee->kind != EX_IDENT) return NULL;
            FuncSig* fs = ft_find(c->fts, e->call.callee->ident.name);
            return fs ? fs->ret_type_name : NULL;
        }

        case EX_MEMBER: {
            // 链式：`r.center.x`。先求宿主的类型名，再查该字段的类型名。
            const char* host = expr_struct_name(c, e->member.obj);
            if (host && g_cur_bc) {
                int ti = bc_find_struct(g_cur_bc, host);
                if (ti >= 0) {
                    int fi = bc_find_field(g_cur_bc, ti, e->member.name);
                    if (fi >= 0) {
                        return struct_name_of_ty(g_cur_bc->structs[ti].field_types[fi]);
                    }
                }
            }

            // ---- 宿主是**类实例**，读它的结构体字段 ----
            //
            // `self.r.contains(...)` 这类写法在 UI 库里是主流形态：
            // 类的字段就是一个 Rect。上面那一段只查结构体表，追不到
            // —— 于是 `self.r` 的类型未知，`.contains()` 报「无法确定
            // 宿主类型」。
            //
            // 这里共用 expr_class_name 而不是再写一遍「怎么追类名」，
            // 让两条链（结构体链、类链）的分工只有一处。
            const char* chost = expr_class_name(c, e->member.obj);
            if (chost && g_cur_bc) {
                int ci = bc_find_class(g_cur_bc, chost);
                if (ci >= 0) {
                    int fi = bc_find_class_field(g_cur_bc, ci, e->member.name);
                    if (fi >= 0) {
                        const char* ft = g_cur_bc->classes[ci].field_types[fi];
                        return struct_name_of_ty(ft);
                    }
                }
            }
            return NULL;
        }

        default:
            return NULL;
    }
}

// 求一个表达式的**具体类名**。不是类实例或追不到时返回 NULL。
//
// ── 为什么不与 expr_struct_name 合并成一个 ──────────────────
//
// 因为两者查的是**两张不同的表**（结构体表 / 类表），而且语义上
// 结构体是值、类是引用 —— `a == b` 对前者是结构相等、对后者是
// 同一个对象。合并成一个「求用户类型名」的函数会让调用点还要
// 再判断一次「这名字属于哪张表」，反而更啰嗦。
//
// 两者形态高度相似（变量、new、链式、函数返回），所以代码结构
// 刻意保持一致 —— 读一个就能猜到另一个。
//
// 特别地，**返回类型声明的父类**是合法静态类型：
//     let s: Shape = new Circle(...)
//     s.area()        // 静态类型 Shape，实际调 Circle.area
// 这正是多态的定义。expr_class_name 在这里给出 "Shape"，
// 而虚调用的派发靠运行时对象的真实类完成 —— 两者不冲突。
static const char* expr_class_name(Ctx* c, Expr* e) {
    if (!e) return NULL;

    switch (e->kind) {
        case EX_IDENT: {
            int slot = local_find(c, e->ident.name);
            if (slot < 0) return NULL;
            const char* tn = local_type_name(c, slot);
            return (tn && is_class_name(tn)) ? tn : NULL;
        }

        case EX_NEW:
            return is_class_name(e->newx.type_name) ? e->newx.type_name : NULL;

        case EX_CALL: {
            if (e->call.callee->kind == EX_MEMBER) {
                Expr* m = e->call.callee;
                const char* tn = expr_class_name(c, m->member.obj);
                if (!tn && m->member.obj->kind == EX_IDENT &&
                    is_class_name(m->member.obj->ident.name)) {
                    tn = m->member.obj->ident.name;   // 静态方法
                }
                if (!tn) return NULL;
                size_t ltn = strlen(tn), lmn = strlen(m->member.name);
                char full[512];
                if (ltn + 1 + lmn + 1 > sizeof(full)) return NULL;
                memcpy(full, tn, ltn);
                full[ltn] = '.';
                memcpy(full + ltn + 1, m->member.name, lmn);
                full[ltn + 1 + lmn] = '\0';
                FuncSig* fs = ft_find(c->fts, full);
                if (!fs || !fs->ret_type_name) return NULL;
                return is_class_name(fs->ret_type_name) ? fs->ret_type_name : NULL;
            }
            if (e->call.callee->kind != EX_IDENT) return NULL;
            FuncSig* fs = ft_find(c->fts, e->call.callee->ident.name);
            if (!fs || !fs->ret_type_name) return NULL;
            return is_class_name(fs->ret_type_name) ? fs->ret_type_name : NULL;
        }

        case EX_MEMBER: {
            const char* host = expr_class_name(c, e->member.obj);
            if (!host || !g_cur_bc) return NULL;
            int ci = bc_find_class(g_cur_bc, host);
            if (ci < 0) return NULL;
            int fi = bc_find_class_field(g_cur_bc, ci, e->member.name);
            if (fi < 0) return NULL;
            const char* ftn = g_cur_bc->classes[ci].field_types[fi];
            return (ftn && is_class_name(ftn)) ? ftn : NULL;
        }

        default:
            return NULL;
    }
}

static void   compile_stmt(Ctx* c, Stmt* s);
static TyKind infer_type(Ctx* c, Expr* e);

// ============================================================
// 类型推断（不生成代码）
// ============================================================
// 与 compile_expr 分离，是为了在「发射指令前」就知道该发哪条。
// 比如 `a + b`：必须先知道 a、b 的类型，才能决定 ADD_I64 还是 ADD_DYN。
// 如果两边都编译完再问类型，栈上已经压了值，只能靠运行时判断。

static TyKind infer_binary(Ctx* c, Expr* e) {
    BinOp op = e->binary.op;

    // 逻辑与/或的结果永远是 Bool
    if (op == OP_AND || op == OP_OR) return TY_BOOL;

    // 比较的结果永远是 Bool
    if (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE ||
        op == OP_GT || op == OP_GE) {
        return TY_BOOL;
    }

    TyKind lt = infer_type(c, e->binary.lhs);
    TyKind rt = infer_type(c, e->binary.rhs);

    // 位运算永远是 Int
    if (op == OP_BAND || op == OP_BOR || op == OP_BXOR ||
        op == OP_SHL  || op == OP_SHR || op == OP_MOD) {
        return TY_INT;
    }

    // `+` 在任一侧是 String 时是拼接
    if (op == OP_ADD && (lt == TY_STRING || rt == TY_STRING)) return TY_STRING;

    return arith_result(lt, rt);
}

static TyKind infer_type(Ctx* c, Expr* e) {
    if (!e) return TY_UNKNOWN;

    switch (e->kind) {
        case EX_INT:    return TY_INT;
        case EX_FLOAT:  return TY_FLOAT;
        case EX_STRING: return TY_STRING;
        case EX_BOOL:   return TY_BOOL;
        case EX_NULL:   return TY_NULL;

        case EX_IDENT: {
            int slot = local_find(c, e->ident.name);
            return slot >= 0 ? local_type(c, slot) : TY_UNKNOWN;
        }

        case EX_BINARY: return infer_binary(c, e);

        case EX_UNARY:
            if (e->unary.op == UOP_NOT) return TY_BOOL;
            return infer_type(c, e->unary.operand);

        case EX_ASSIGN:
            return infer_type(c, e->assign.value);

        case EX_STRUCT_LIT:
            return is_struct_name(e->slit.type_name) ? TY_STRUCT : TY_UNKNOWN;

        // 成员访问：查宿主类型的字段表。
        case EX_MEMBER: {
            const char* host = expr_struct_name(c, e->member.obj);
            if (!host || !g_cur_bc) return TY_UNKNOWN;
            int ti = bc_find_struct(g_cur_bc, host);
            if (ti < 0) return TY_UNKNOWN;
            int fi = bc_find_field(g_cur_bc, ti, e->member.name);
            if (fi < 0) return TY_UNKNOWN;
            return ty_from_name(g_cur_bc->structs[ti].field_types[fi]);
        }

        case EX_IF: {
            TyKind a = infer_type(c, e->ifx.then_block);
            TyKind b = e->ifx.else_expr ? infer_type(c, e->ifx.else_expr) : TY_NULL;
            return ty_merge_branches(a, b);
        }

        case EX_TERNARY:
            return ty_merge_branches(infer_type(c, e->tern.then_e),
                                     infer_type(c, e->tern.else_e));

        case EX_NULL_COALESCE:
            return ty_merge_branches(infer_type(c, e->coal.lhs),
                                     infer_type(c, e->coal.rhs));

        case EX_ARRAY_LIT:
            return TY_ARRAY;

        // `a?.b`：结果类型是「`a.b` 的类型」与 null 的合并 —— 链上任何
// 一环为 null，整串就是 null。所以静态类型必须包含 TY_NULL。
        //
        // 宿主静态已知是 null 时与编译侧一样折叠成 TY_NULL：否则
        // `p?.a?.v` 的外层会以为内层不是 null，去要求 `.v` 的宿主
        // 结构体名，而那个名字根本不存在（内层恒为 null）。
        case EX_OPT_CHAIN: {
            Expr* acc = e->optc.access;
            Expr*  host = (acc->kind == EX_MEMBER) ? acc->member.obj
                                                   : acc->index.obj;
            if (host && host->kind == EX_NULL) return TY_NULL;
            if (host && !expr_struct_name(c, host) &&
                infer_type(c, host) == TY_NULL) {
                return TY_NULL;
            }
            return TY_UNKNOWN;
        }

        case EX_STACK_REF:
            return TY_UNKNOWN;

        case EX_MATCH: {
            // 所有分支的公共类型。空 match 不合法（parser 已拦）。
            TyKind acc = TY_UNKNOWN;
            for (int i = 0; i < e->matchx.arm_count; i++) {
                acc = ty_merge_branches(acc, infer_type(c, e->matchx.arms[i].body));
            }
            return acc;
        }

        case EX_BLOCK: {
            if (!e->block.stmts || e->block.count == 0) return TY_UNKNOWN;
            Stmt* last = e->block.stmts[e->block.count - 1];
            if (last->kind == ST_EXPR) return infer_type(c, last->expr.expr);
            return TY_UNKNOWN;
        }

        case EX_CALL: {
            // 方法调用 `obj.m()`：与 compile_call 同一条规则，
            // 拼成 `Type.m` 去查。两处必须一致，否则会出现
            // 「编译走的是方法分支，推断走的是函数分支」的错位。
            if (e->call.callee->kind == EX_MEMBER) {
                Expr* m = e->call.callee;
                const char* tn = expr_struct_name(c, m->member.obj);
                // 静态方法 `Rect.of(...)`：宿主是类型名本身。
                if (!tn && m->member.obj->kind == EX_IDENT &&
                    is_struct_name(m->member.obj->ident.name)) {
                    tn = m->member.obj->ident.name;
                }
                if (!tn) return TY_UNKNOWN;
                size_t ltn = strlen(tn), lmn = strlen(m->member.name);
                char full[512];
                if (ltn + 1 + lmn + 1 > sizeof(full)) return TY_UNKNOWN;
                memcpy(full, tn, ltn);
                full[ltn] = '.';
                memcpy(full + ltn + 1, m->member.name, lmn);
                full[ltn + 1 + lmn] = '\0';
                FuncSig* fs = ft_find(c->fts, full);
                return fs ? fs->ret_type : TY_UNKNOWN;
            }
            if (e->call.callee->kind != EX_IDENT) return TY_UNKNOWN;
            const char* n = e->call.callee->ident.name;

            // 内置函数
            if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) return TY_NULL;
            if (strcmp(n, "toInt") == 0)      return TY_INT;
            if (strcmp(n, "toFloat") == 0)    return TY_FLOAT;
            if (strcmp(n, "toString") == 0)   return TY_STRING;
            if (strcmp(n, "typeOf") == 0)     return TY_STRING;
            if (strcmp(n, "fail") == 0)       return TY_NULL;
            if (strcmp(n, "len") == 0)        return TY_INT;
            if (strcmp(n, "array") == 0)      return TY_ARRAY;
            if (strcmp(n, "abs") == 0) {
                // abs 保持入参类型
                return e->call.argc > 0 ? infer_type(c, e->call.args[0]) : TY_INT;
            }

            // 平台内建（win* 系列）
            {
                int bi = bc_builtin_find(n);
                if (bi >= 0) return ty_from_name(bc_builtin_ret(bi));
            }

            // 用户函数。
            //
            // 重载：这里与 compile_call 走**同一套**按实参类型的挑选
            // 规则，否则会出现「推断走第一个签名、编译挑了第二个」
            // 的错位。quiet = true：infer_type 会被编译期试探性调用
            //（match 臂合并、空合并接收者判断等），真正的错误由
            // compile_call 报，这里只安静地给推断结果。
            {
                TyKind* iargt = NULL;
                if (e->call.argc > 0) {
                    iargt = (TyKind*)x_alloc(
                        sizeof(TyKind) * (size_t)e->call.argc);
                    for (int i = 0; i < e->call.argc; i++) {
                        iargt[i] = infer_type(c, e->call.args[i]);
                    }
                }
                bool iexists = false;
                FuncSig* s = resolve_overload(c->fts, n, e->call.argc,
                                              iargt, e->pos, &iexists, true);
                if (iargt) free(iargt);
                return s ? s->ret_type : TY_UNKNOWN;
            }
        }

        default:
            return TY_UNKNOWN;
    }
}

// ============================================================
// 转字符串（拼接与 print 共用）
// ============================================================

static void emit_to_string(Ctx* c, TyKind t, SrcPos pos) {
    switch (t) {
        case TY_INT:    emit(c, BC_TOSTRING_I64,  0, pos); break;
        case TY_FLOAT:  emit(c, BC_TOSTRING_F64,  0, pos); break;
        case TY_BOOL:   emit(c, BC_TOSTRING_BOOL, 0, pos); break;
        case TY_NULL:   emit(c, BC_TOSTRING_NULL, 0, pos); break;
        case TY_STRING: break;   // 已经是字符串，无需转换
        // 结构体走 BC_TOSTRING_ANY —— 运行时的 value_to_str / v_str_of
        // 会把对象打印成 `Rect { x: 1, y: 2 }`。这里不为结构体加一条
        // 专门指令：它和「未知类型」的处理完全一样，多一条指令只是
        // 多一处要维护的东西（枚举、名称表、栈效应表、两档后端）。
        default:        emit(c, BC_TOSTRING_ANY,  0, pos); break;
    }
}

// ============================================================
// 表达式编译
// ============================================================

static TyKind compile_binary(Ctx* c, Expr* e) {
    BinOp op = e->binary.op;

    // ---- 短路逻辑：&& 与 or ----
    // 栈式实现不需要「KEEP」类指令，直接分两条路各自压一个 Bool 即可。
    // 这样两个分支的栈效果完全对称，不会出现栈深不一致。
    if (op == OP_AND) {
        compile_expr(c, e->binary.lhs);
        int jf = emit_jump(c, BC_JUMP_IF_FALSE, e->pos);   // 弹栈顶，假则跳
        compile_expr(c, e->binary.rhs);
        emit(c, BC_TOBOOL, 0, e->pos);
        int jend = emit_jump(c, BC_JUMP, e->pos);
        patch_here(c, jf);
        emit(c, BC_PUSH_FALSE, 0, e->pos);
        patch_here(c, jend);
        return TY_BOOL;
    }
    if (op == OP_OR) {
        compile_expr(c, e->binary.lhs);
        int jt = emit_jump(c, BC_JUMP_IF_TRUE, e->pos);
        compile_expr(c, e->binary.rhs);
        emit(c, BC_TOBOOL, 0, e->pos);
        int jend = emit_jump(c, BC_JUMP, e->pos);
        patch_here(c, jt);
        emit(c, BC_PUSH_TRUE, 0, e->pos);
        patch_here(c, jend);
        return TY_BOOL;
    }

    // ---- 其余二元运算：先推断类型，再决定指令 ----
    TyKind lt = infer_type(c, e->binary.lhs);
    TyKind rt = infer_type(c, e->binary.rhs);

    compile_expr(c, e->binary.lhs);
    compile_expr(c, e->binary.rhs);

    // `+` 且可能涉及字符串 → 动态拼加
    if (op == OP_ADD && (lt == TY_STRING || rt == TY_STRING ||
                         lt == TY_UNKNOWN || rt == TY_UNKNOWN)) {
        // 两侧都是确定的 String 时可以直接 CONCAT_STR
        if (lt == TY_STRING && rt == TY_STRING) {
            emit(c, BC_CONCAT_STR, 0, e->pos);
            return TY_STRING;
        }
        emit(c, BC_ADD_DYN, 0, e->pos);
        // 动态 `+` 的结果类型运行时才知；保守地按推断宣布
        if (lt == TY_STRING || rt == TY_STRING) return TY_STRING;
        return arith_result(lt, rt);
    }

    // ---- 相等 / 不等：通用指令，支持字符串字典序比较 ----
    if (op == OP_EQ) { emit(c, BC_EQ, 0, e->pos); return TY_BOOL; }
    if (op == OP_NE) { emit(c, BC_NE, 0, e->pos); return TY_BOOL; }

    // ---- 位运算（强制 Int）----
    switch (op) {
        case OP_BAND: emit(c, BC_BAND_I64, 0, e->pos); return TY_INT;
        case OP_BOR:  emit(c, BC_BOR_I64,  0, e->pos); return TY_INT;
        case OP_BXOR: emit(c, BC_BXOR_I64, 0, e->pos); return TY_INT;
        case OP_SHL:  emit(c, BC_SHL_I64,  0, e->pos); return TY_INT;
        case OP_SHR:  emit(c, BC_SHR_I64,  0, e->pos); return TY_INT;
        case OP_MOD:  emit(c, BC_MOD_I64,  0, e->pos); return TY_INT;
        default: break;
    }

    // ---- 关系比较 ----
    // 全 Int → 特化；全 Float → 特化；其余（含 unknown、混算）→ 数值双分派
    {
        bool use_i64 = both_definitely_int(lt, rt);
        bool use_f64 = both_definitely_float(lt, rt);

        switch (op) {
            case OP_LT:
                emit(c, use_i64 ? BC_LT_I64 : (use_f64 ? BC_LT_F64 : BC_LT_NUM), 0, e->pos);
                return TY_BOOL;
            case OP_LE:
                emit(c, use_i64 ? BC_LE_I64 : (use_f64 ? BC_LE_F64 : BC_LE_NUM), 0, e->pos);
                return TY_BOOL;
            case OP_GT:
                emit(c, use_i64 ? BC_GT_I64 : (use_f64 ? BC_GT_F64 : BC_GT_NUM), 0, e->pos);
                return TY_BOOL;
            case OP_GE:
                emit(c, use_i64 ? BC_GE_I64 : (use_f64 ? BC_GE_F64 : BC_GE_NUM), 0, e->pos);
                return TY_BOOL;
            default: break;
        }
    }

    // ---- 四则 ----
    {
        bool use_i64 = both_definitely_int(lt, rt);
        bool use_f64 = both_definitely_float(lt, rt);

        switch (op) {
            case OP_ADD:
                emit(c, use_i64 ? BC_ADD_I64 : (use_f64 ? BC_ADD_F64 : BC_ADD_NUM), 0, e->pos);
                return arith_result(lt, rt);
            case OP_SUB:
                emit(c, use_i64 ? BC_SUB_I64 : (use_f64 ? BC_SUB_F64 : BC_SUB_NUM), 0, e->pos);
                return arith_result(lt, rt);
            case OP_MUL:
                emit(c, use_i64 ? BC_MUL_I64 : (use_f64 ? BC_MUL_F64 : BC_MUL_NUM), 0, e->pos);
                return arith_result(lt, rt);
            case OP_DIV:
                // Int/Int 走整数除法（与 01 §4.3「Int 除法截断向零」一致）；
                // 只要有一边是 Float 就是浮点除法。
                emit(c, use_i64 ? BC_DIV_I64 : (use_f64 ? BC_DIV_F64 : BC_DIV_NUM), 0, e->pos);
                return arith_result(lt, rt);
            default: break;
        }
    }

    diag_error(e->pos, "P0 尚不支持此二元运算符（op=%d）", (int)op);
    return TY_UNKNOWN;
}

// math 子操作名 → 编号 + 参数个数 + 返回类型。
//
// ── 为什么用「名字表」而不是让用户写 `math(1, 2, 3)` ────────
//
// 数字编号对人类不可读，而且一旦重排枚举，老代码会静默算错
// （不报错，只是 pow 变成 sqrt）。名字表编译期查，写错名字立刻
// 报「未知的数学函数」，改的时候也一眼能看出改的是哪个。
//
// 表里第三项是参数个数，第四项是返回类型。返回类型不能统一成 Float：
// floor/ceil/round/trunc/sign 返回 Int（用户拿去做下标或数组长度时必须
// 是 Int，否则要再套一层 toInt），min/max/clamp 保持输入类型。
typedef struct {
    const char* name;
    XMathOp     op;
    int         argc;
    TyKind      ret;
} MathInfo;

static const MathInfo g_math[] = {
    { "pow",   XMO_POW,   2, TY_FLOAT },
    { "sqrt",  XMO_SQRT,  1, TY_FLOAT },
    { "cbrt",  XMO_CBRT,  1, TY_FLOAT },
    { "floor", XMO_FLOOR, 1, TY_INT   },
    { "ceil",  XMO_CEIL,  1, TY_INT   },
    { "round", XMO_ROUND, 1, TY_INT   },
    { "trunc", XMO_TRUNC, 1, TY_INT   },
    { "abs",   XMO_ABS,   1, TY_UNKNOWN },   // 保持输入类型
    { "sign",  XMO_SIGN,  1, TY_INT   },
    { "min",   XMO_MIN,   2, TY_UNKNOWN },   // 保持输入类型，运行时决定
    { "max",   XMO_MAX,   2, TY_UNKNOWN },
    { "sin",   XMO_SIN,   1, TY_FLOAT },
    { "cos",   XMO_COS,   1, TY_FLOAT },
    { "tan",   XMO_TAN,   1, TY_FLOAT },
    { "asin",  XMO_ASIN,  1, TY_FLOAT },
    { "acos",  XMO_ACOS,  1, TY_FLOAT },
    { "atan",  XMO_ATAN,  1, TY_FLOAT },
    { "atan2", XMO_ATAN2, 2, TY_FLOAT },
    { "log",   XMO_LOG,   1, TY_FLOAT },
    { "log2",  XMO_LOG2,  1, TY_FLOAT },
    { "log10", XMO_LOG10, 1, TY_FLOAT },
    { "exp",   XMO_EXP,   1, TY_FLOAT },
    { "hypot", XMO_HYPOT, 2, TY_FLOAT },
    { "fmod",  XMO_FMOD,  2, TY_FLOAT },
    { "clamp", XMO_CLAMP, 3, TY_UNKNOWN },
};

// 编译 `math(op, args...)`。
//
// 第一个实参必须是**裸标识符**（pow/sqrt/...），不能是变量或表达式：
// 子操作要在编译期定下来才能生成正确的指令编码。允许变量会让
// `let f = pow; math(f, 2)` 这种东西需要运行时查表，那是解释器
// 循环里的一次字符串比较 —— 对 P0 的「先跑对再做快」原则不划算，
// 而且它带来的表达力几乎没人需要。
static TyKind compile_math_call(Ctx* c, Expr* e) {
    if (e->call.argc < 1) {
        diag_error(e->pos, "`math` 至少需要 1 个参数：`math(函数名, ...)`");
        diag_help("例：`math(sqrt, 2.0)`、`math(pow, 2.0, 10.0)`");
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return TY_FLOAT;
    }

    Expr* head = e->call.args[0];
    if (head->kind != EX_IDENT) {
        diag_error(head->pos, "`math` 的第一个参数必须是数学函数名（裸标识符）");
        diag_help("写 `math(sqrt, x)`，不要写字符串或变量");
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return TY_FLOAT;
    }

    const char* opname = head->ident.name;
    const MathInfo* mi = NULL;
    for (size_t i = 0; i < sizeof(g_math) / sizeof(g_math[0]); i++) {
        if (strcmp(g_math[i].name, opname) == 0) { mi = &g_math[i]; break; }
    }
    if (!mi) {
        diag_error(head->pos, "未知的数学函数 `%s`", opname);
        // 给出候选而不是让用户猜。25 个名字全列出来太长，
        // 按首字母提示最常见的那几个。
        diag_help("常用：pow sqrt abs min max floor ceil round log exp sin cos");
        diag_help("完整列表见 stdlib/math.xle 顶部注释");
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return TY_FLOAT;
    }

    int want = mi->argc + 1;   // +1 是那个函数名参数
    if (e->call.argc != want) {
        diag_error(e->pos, "`math(%s, ...)` 需要 %d 个数值参数，实际给了 %d 个",
                   opname, mi->argc, e->call.argc - 1);
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return mi->ret;
    }

    // 只压数值实参，**不压函数名** —— 函数名已经编码进操作数了。
    // 压了会多一个垃圾参数，解释器按 argc 弹栈时会把别的值当参数。
    TyKind a0 = TY_UNKNOWN, a1 = TY_UNKNOWN;
    for (int i = 1; i < e->call.argc; i++) {
        TyKind t = compile_expr(c, e->call.args[i]);
        if (i == 1) a0 = t;
        if (i == 2) a1 = t;
        if (t != TY_INT && t != TY_FLOAT && t != TY_UNKNOWN) {
            diag_error(e->call.args[i]->pos,
                       "`math(%s, ...)` 的参数必须是数值（Int 或 Float），实际是 %s",
                       opname, ty_name(t));
            diag_help("字符串请先用 `strParseInt` / `strParseFloat` 转成数值");
        }
    }

    // ── 操作数分配 ──────────────────────────────────────────────
    // a 必须是 XBI_MATH（函数编号）：bc_uses_gui 与反汇编都要靠 a 认
    // 出这是哪个内建。子操作编码只能放别处。
    //   b = 参数个数（bc_op_stack_effect_at 用它算栈效应）
    //   c = (argc << 8) | op   子操作编码
    // 之前把编码写进 a 导致 a 变成 264，运行时走到 run_builtin 的
    // switch 里一个 case 都不匹配，报「未知的平台内建编号」。
    emit(c, BC_BUILTIN, (int)XBI_MATH, e->pos);
    c->bc->code[c->bc->code_count - 1].b = (int16_t)mi->argc;
    c->bc->code[c->bc->code_count - 1].c = (int16_t)xmath_enc((int)mi->op, mi->argc);

    // min/max/clamp 保持输入类型：两边都是 Int 就给 Int，
    // 否则给 Float。这与运行时的行为一致（运行时只做比较，不转类型）。
    if (mi->ret == TY_UNKNOWN) {
        if (a0 == TY_INT && (mi->argc == 1 || a1 == TY_INT)) return TY_INT;
        if (a0 == TY_FLOAT || a1 == TY_FLOAT) return TY_FLOAT;
        return TY_UNKNOWN;
    }
    return mi->ret;
}

static TyKind compile_call(Ctx* c, Expr* e) {
    // ---------- 方法调用 `obj.method(args)` ----------
    //
    // ── 实现方式：降级为普通函数调用 ───────────────────────────
    //
    // 没有虚表、没有动态分派、没有 `self` 关键字的新语义。规则是
    // 唯一的一条：
    //
    //     obj.method(a, b)   ≡   Type.method(obj, a, b)
    //
    // 其中 Type 由 obj 的静态类型决定。于是「方法」不是新东西，
    // 只是一个名字里带点的普通函数 —— 而它照样能：
    //   · 参与超指令合并（就是普通 BC_CALL）
    //   · 被 AOT 编译（同上）
    //   · 写进 .xbc（同上）
    //   · 递归、互递归、作为参数传递（同上）
    //
    // ── 为什么不引入独立的 MethodDecl 与 self ─────────────────
    //
    // 独立的语法（`fn Rect.area(self) -> Int`）需要：解析器认新的
    // 声明形态、FuncDecl 加宿主字段、编译器建方法表、`self` 变成
    // 隐式局部变量、类型检查区分「方法」与「函数」。这五件事每一
    // 件都要在解释器和 AOT 两边各做一遍。
    //
    // 而收益只是「少写一个 obj」。在 P0.5 这个阶段，用普通函数
    // 加命名约定就能拿到全部表达力，代价是声明处要手写第一个参数。
    // 等真的需要重载/继承（那才需要方法表）时再引入，那时已经有
    // 真实用例告诉我们方法表该长什么样。
    if (e->call.callee->kind == EX_MEMBER) {
        Expr* m = e->call.callee;

        // ---------- 类的方法调用（含多态派发） ----------
        //
        // ── 这里的判据比结构体那边复杂，为什么 ──────────────────
        //
        // 结构体的方法调用是**静态**的：`r.area()` 里 r 的静态类型
        // 就是 Rect，直接调 Rect.area 即可。
        //
        // 类不行。`let s: Shape = new Circle(); s.area()` 里 s 的
        // 静态类型是 Shape，但真正该执行的是 Circle.area。这必须在
        // 运行时按对象的**实际类**决定 —— 这就是虚调用。
        //
        // 三种情形要分开：
        //   1. 静态方法 `Klass.make()`  —— 宿主是类名，编译期确定
        //   2. 静态类型已确定但方法可能被覆写 —— 必须走虚调用
        //   3. 静态类型是抽象类/接口 —— 只能走虚调用
        //
        // 第 2 种不能「优化成直接调用」：`s.area()` 静态类型 Shape
        // 时直接调 Shape.area 会执行抽象方法的空壳（或父类实现），
        // 而用户期望的是子类实现。这个 bug 只在「有子类覆写」时
        // 出现，单类测试永远发现不了。
        {
            const char* cls_tn = expr_class_name(c, m->member.obj);

            // 情形 1：宿主是类名本身 → 静态方法
            if (!cls_tn && m->member.obj->kind == EX_IDENT &&
                is_class_name(m->member.obj->ident.name)) {
                const char* ktn = m->member.obj->ident.name;
                size_t ltn = strlen(ktn), lmn = strlen(m->member.name);
                char sfull[512];
                if (ltn + 1 + lmn + 1 <= sizeof(sfull)) {
                    memcpy(sfull, ktn, ltn);
                    sfull[ltn] = '.';
                    memcpy(sfull + ltn + 1, m->member.name, lmn);
                    sfull[ltn + 1 + lmn] = '\0';

                    FuncSig* sf = ft_find(c->fts, sfull);
                    if (sf) {
                        // 静态方法可见性：找到签名后立刻查，免得写了
                        // 半天参数检查才报权限问题。
                        check_static_visible(c, ktn, m->member.name, m->pos);
                        if (e->call.argc != sf->param_count) {
                            diag_error(e->pos, "`%s` 需要 %d 个参数，实际给了 %d 个",
                                       sfull, sf->param_count, e->call.argc);
                        }
                        for (int i = 0; i < e->call.argc; i++) {
                            TyKind at = compile_expr(c, e->call.args[i]);
                            if (sf->param_types && i < sf->param_count &&
                                sf->param_types[i]) {
                                TyKind pt = ty_from_name(sf->param_types[i]);
                                bool np2 = (pt == TY_INT && at == TY_FLOAT) ||
                                           (pt == TY_FLOAT && at == TY_INT);
                                if (pt != TY_UNKNOWN && at != TY_UNKNOWN &&
                                    at != pt && !np2) {
                                    diag_error(e->call.args[i]->pos,
                                               "`%s` 的第 %d 个参数声明为 %s，实际传入 %s",
                                               sfull, i + 1, ty_name(pt), ty_name(at));
                                }
                            }
                        }
                        emit(c, BC_CALL, sf->bc_index, e->pos);
                        return sf->ret_type;
                    }
                    // 不是静态方法，可能是把实例方法当静态调了
                    if (ltn + 1 + lmn + 1 <= sizeof(sfull)) {
                        diag_error(m->pos, "类 `%s` 没有静态方法 `%s`",
                                   ktn, m->member.name);
                        diag_help("静态方法声明时不带 `self`："
                                  "`static fn %s.%s(...)`。"
                                  "实例方法要用对象调用：`obj.%s(...)`",
                                  ktn, m->member.name, m->member.name);
                    }
                    for (int i = 0; i < e->call.argc; i++) {
                        compile_expr(c, e->call.args[i]);
                        emit(c, BC_POP, 0, e->pos);
                    }
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_UNKNOWN;
                }
            }

            // 情形 2 / 3：实例方法 → 虚调用
            if (cls_tn) {
                int ci = bc_find_class(c->bc, cls_tn);
                if (ci >= 0) {
                    int slot = bc_find_method_slot(c->bc, m->member.name);

                    // 在**静态类型的继承链**上确认这个方法存在。
                    //
                    // 为什么必须查而不是「虚表槽非 -1 就算有」：虚表槽
                    // 记录的是「这个类有没有给出实现」，而抽象方法本来就
                    // 没有实现（槽是 -1）。如果按槽判断，所有对抽象方法
                    // 的调用都会被误报成「没有这个方法」—— 而调用抽象
                    // 方法正是多态的主要用法。
                    //
                    // 所以判据是「继承链上有没有这个名字的声明」，
                    // 声明来源有两处：类体里的 fn，以及 implements 的接口。
                    bool exists = false;
                    if (slot >= 0) {
                        for (int cur = ci; cur >= 0;
                             cur = c->bc->classes[cur].parent_idx) {
                            size_t ln = strlen(c->bc->classes[cur].name);
                            size_t mn = strlen(m->member.name);
                            char full2[512];
                            if (ln + 1 + mn + 1 > sizeof(full2)) continue;
                            memcpy(full2, c->bc->classes[cur].name, ln);
                            full2[ln] = '.';
                            memcpy(full2 + ln + 1, m->member.name, mn);
                            full2[ln + 1 + mn] = '\0';
                            if (ft_find(c->fts, full2)) { exists = true; break; }
                        }
                        // 接口里声明的方法（接口本身不收进函数表 ——
                        // 它没有函数体，但 BC_METHOD 槽在接口的虚表里
                        // 被标记过，靠 iface 列表回查）。
                        if (!exists) {
                            for (int k = 0; k < c->bc->classes[ci].iface_count; k++) {
                                int ii = c->bc->classes[ci].iface_idx[k];
                                size_t ln = strlen(c->bc->classes[ii].name);
                                size_t mn = strlen(m->member.name);
                                char full2[512];
                                if (ln + 1 + mn + 1 > sizeof(full2)) continue;
                                memcpy(full2, c->bc->classes[ii].name, ln);
                                full2[ln] = '.';
                                memcpy(full2 + ln + 1, m->member.name, mn);
                                full2[ln + 1 + mn] = '\0';
                                if (ft_find(c->fts, full2)) { exists = true; break; }
                            }
                        }
                    }

                    if (!exists && slot >= 0) {
                        diag_error(m->pos, "类 `%s` 没有方法 `%s`",
                                   cls_tn, m->member.name);
                        char buf[1024]; buf[0] = '\0';
                        for (int k = 0; k < c->bc->method_slot_count; k++) {
                            const char* nm = c->bc->method_slots[k].name;
                            // 只列本类继承链上真实存在的
                            bool found = false;
                            for (int cur = ci; cur >= 0;
                                 cur = c->bc->classes[cur].parent_idx) {
                                size_t ln = strlen(c->bc->classes[cur].name);
                                size_t nn = strlen(nm);
                                char full3[512];
                                if (ln + 1 + nn + 1 <= sizeof(full3)) {
                                    memcpy(full3, c->bc->classes[cur].name, ln);
                                    full3[ln] = '.';
                                    memcpy(full3 + ln + 1, nm, nn);
                                    full3[ln + 1 + nn] = '\0';
                                    if (ft_find(c->fts, full3)) { found = true; break; }
                                }
                            }
                            if (found) {
                                if (buf[0]) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
                                strncat(buf, nm, sizeof(buf) - strlen(buf) - 1);
                            }
                        }
                        if (buf[0]) diag_note("`%s` 已有的方法：%s", cls_tn, buf);
                        diag_help("检查方法名拼写。方法必须在类体里声明："
                                  "`fn %s { ... }`", m->member.name);
                    }

                    if (slot < 0) {
                        diag_error(m->pos, "内部错误：方法 `%s` 没有虚表槽",
                                   m->member.name);
                        compile_expr(c, m->member.obj);
                        emit(c, BC_POP, 0, e->pos);
                        for (int i = 0; i < e->call.argc; i++) {
                            compile_expr(c, e->call.args[i]);
                            emit(c, BC_POP, 0, e->pos);
                        }
                        emit(c, BC_PUSH_NULL, 0, e->pos);
                        return TY_UNKNOWN;
                    }

                    // ---- 方法可见性 ----
                    //
                    // 放在「方法存在」之后、「参数个数」之前：名字压根
                    // 不存在时先报「没有这个方法」更有用，而可见性错误
                    // 的前提是方法确实存在。
                    check_method_visible(c, ci, slot, cls_tn,
                                         m->member.name, m->pos);

                    // 参数个数校验：从继承链上找具体签名
                    FuncSig* fsig = NULL;
                    for (int cur = ci; cur >= 0 && !fsig;
                         cur = c->bc->classes[cur].parent_idx) {
                        size_t ln = strlen(c->bc->classes[cur].name);
                        size_t mn = strlen(m->member.name);
                        char full2[512];
                        if (ln + 1 + mn + 1 <= sizeof(full2)) {
                            memcpy(full2, c->bc->classes[cur].name, ln);
                            full2[ln] = '.';
                            memcpy(full2 + ln + 1, m->member.name, mn);
                            full2[ln + 1 + mn] = '\0';
                            fsig = ft_find(c->fts, full2);
                        }
                    }
                    if (fsig && e->call.argc + 1 != fsig->param_count) {
                        diag_error(e->pos,
                                   "方法 `%s.%s` 需要 %d 个参数（不含接收者），实际给了 %d 个",
                                   cls_tn, m->member.name,
                                   fsig->param_count - 1, e->call.argc);
                    }

                    // 压栈顺序：self 在最下，实参依次向上
                    TyKind rt2 = compile_expr(c, m->member.obj);
                    for (int i = 0; i < e->call.argc; i++) {
                        TyKind at = compile_expr(c, e->call.args[i]);
                        if (fsig && fsig->param_types && i + 1 < fsig->param_count &&
                            fsig->param_types[i + 1]) {
                            TyKind pt = ty_from_name(fsig->param_types[i + 1]);
                            bool np2 = (pt == TY_INT && at == TY_FLOAT) ||
                                       (pt == TY_FLOAT && at == TY_INT);
                            bool poly = false;
                            if (pt == TY_CLASS && at == TY_CLASS) {
                                const char* an = expr_class_name(c, e->call.args[i]);
                                if (an) {
                                    int ai = bc_find_class(c->bc, an);
                                    int wi = bc_find_class(c->bc, fsig->param_types[i + 1]);
                                    poly = bc_class_is_a(c->bc, ai, wi);
                                }
                            }
                            if (pt != TY_UNKNOWN && at != TY_UNKNOWN &&
                                at != pt && !np2 && !poly) {
                                diag_error(e->call.args[i]->pos,
                                           "第 %d 个参数声明为 %s，实际传入 %s",
                                           i + 1, ty_name(pt), ty_name(at));
                            }
                        }
                    }
                    (void)rt2;

                    emit(c, BC_VIRT_CALL, slot, e->pos);
                    c->bc->code[c->bc->code_count - 1].b =
                        (int16_t)(e->call.argc + 1);

                    // 返回类型：以静态类型的声明为准。
                    // 这就是「返回父类、实际是子类」能继续多态的原因。
                    if (fsig && fsig->ret_type_name) {
                        return ty_from_name(fsig->ret_type_name);
                    }
                    return fsig ? fsig->ret_type : TY_UNKNOWN;
                }
            }
        }

        const char* tn = expr_struct_name(c, m->member.obj);
        if (!tn) {
            // 宿主追不到。有两种可能，必须先分清：
            //
            //   1. 真的是「不知道类型」（`getSomething().m()`）
            //   2. 宿主其实是个**结构体名**，即静态方法 `Rect.of(...)`
            //
            // 第 2 种是这个设计里唯一需要特殊照顾的情形：`Rect.of()`
            // 里的 `Rect` 不是一个值，它是类型名。而语法上 `Rect.of`
            // 与 `r.of` 完全同形 —— 解析器不可能区分，只能在这里看
            // 「这个名字是不是结构体」。
            //
            // ── 为什么静态方法值得支持 ──────────────────────────
            //
            // 构造器是结构体最基本的用法：`Rect.of(...)`、`Theme.dark()`。
            // 没有它，用户只能写 `Rect { ... }` 字面量，而字面量无法
            // 包含计算逻辑（比如 `Theme.dark()` 内部引用了 12 个常量）。
            // 有了它，「结构体 + 一组构造器」就成了完整的类型封装。
            if (m->member.obj->kind == EX_IDENT &&
                is_struct_name(m->member.obj->ident.name)) {
                tn = m->member.obj->ident.name;
                // 静态方法：没有接收者，参数个数不加 1。
                size_t ltn2 = strlen(tn), lmn2 = strlen(m->member.name);
                char sfull[512];
                if (ltn2 + 1 + lmn2 + 1 <= sizeof(sfull)) {
                    memcpy(sfull, tn, ltn2);
                    sfull[ltn2] = '.';
                    memcpy(sfull + ltn2 + 1, m->member.name, lmn2);
                    sfull[ltn2 + 1 + lmn2] = '\0';

                    FuncSig* sf = ft_find(c->fts, sfull);
                    if (sf) {
                        if (e->call.argc != sf->param_count) {
                            diag_error(e->pos,
                                       "`%s` 需要 %d 个参数，实际给了 %d 个",
                                       sfull, sf->param_count, e->call.argc);
                        }
                        for (int i = 0; i < e->call.argc; i++) {
                            TyKind at = compile_expr(c, e->call.args[i]);
                            if (sf->param_types && i < sf->param_count &&
                                sf->param_types[i]) {
                                TyKind pt = ty_from_name(sf->param_types[i]);
                                bool np2 = (pt == TY_INT && at == TY_FLOAT) ||
                                           (pt == TY_FLOAT && at == TY_INT);
                                if (pt != TY_UNKNOWN && at != TY_UNKNOWN &&
                                    at != pt && !np2) {
                                    diag_error(e->call.args[i]->pos,
                                               "`%s` 的第 %d 个参数声明为 %s，实际传入 %s",
                                               sfull, i + 1, ty_name(pt), ty_name(at));
                                }
                            }
                        }
                        emit(c, BC_CALL, sf->bc_index, e->pos);
                        return sf->ret_type;
                    }
                    diag_error(m->pos, "结构体 `%s` 没有静态方法 `%s`", tn, m->member.name);
                    char sbuf[1024];
                    sbuf[0] = '\0';
                    for (int i = 0; i < c->fts->count; i++) {
                        const char* nm = c->fts->sigs[i].name;
                        if (strncmp(nm, tn, ltn2) == 0 && nm[ltn2] == '.' &&
                            c->fts->sigs[i].param_count > 0 &&
                            c->fts->sigs[i].param_types &&
                            c->fts->sigs[i].param_types[0] &&
                            strcmp(c->fts->sigs[i].param_types[0], tn) == 0) {
                            continue;   // 这是实例方法，不算静态方法
                        }
                        if (strncmp(nm, tn, ltn2) == 0 && nm[ltn2] == '.') {
                            if (sbuf[0]) strncat(sbuf, ", ", sizeof(sbuf) - strlen(sbuf) - 1);
                            strncat(sbuf, nm + ltn2 + 1, sizeof(sbuf) - strlen(sbuf) - 1);
                        }
                    }
                    if (sbuf[0]) diag_note("`%s` 已有的静态方法：%s", tn, sbuf);
                    diag_help("静态方法就是第一个参数**不是** `self: %s` 的 "
                              "`fn %s.名字(...)` —— 它不接收者，返回一个新值",
                              tn, tn);
                    for (int i = 0; i < e->call.argc; i++) {
                        compile_expr(c, e->call.args[i]);
                        emit(c, BC_POP, 0, e->pos);
                    }
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_UNKNOWN;
                }
                return TY_UNKNOWN;
            }

            // 真的是「不知道类型」。
            // 先编译一遍宿主，让它的错误（未定义变量等）
            // 优先暴露出来 —— 否则这里报「无法确定宿主类型」会把
            // 真正的原因盖掉。
            compile_expr(c, m->member.obj);
            diag_error(m->pos, "无法确定 `%s(...)` 的宿主类型", m->member.name);
            diag_help("方法调用 `obj.m()` 等价于 `类型.m(obj)`，"
                      "需要 obj 的类型可静态追踪：给它加类型注解，或用字面量初始化");
            emit(c, BC_POP, 0, e->pos);
            for (int i = 0; i < e->call.argc; i++) {
                compile_expr(c, e->call.args[i]);
                emit(c, BC_POP, 0, e->pos);
            }
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_UNKNOWN;
        }

        // 拼 `Type.method`，然后走普通函数调用的完整路径。
        //
        // ── 为什么拼名字而不是维护方法表 ───────────────────────
        //
        // 拼名字只需要一次 strlen + memcpy，而且**复用**了现有的
        // ft_find 查找、参数个数校验、参数类型校验、未定义报错。
        // 方法表则要把这些逻辑再实现一遍，且两份实现必然逐渐不一致。
        //
        // 名字用 `.` 分隔而不是 `::`：`.` 在用户的函数名里不可能出现
        // （lexer 只允许标识符），所以不存在撞名。而 `::` 要多认一个
        // 语法元素，收益为零。
        size_t need = strlen(tn) + 1 + strlen(m->member.name) + 1;
        char* full = x_alloc(need);
        snprintf(full, need, "%s.%s", tn, m->member.name);

        FuncSig* fs = ft_find(c->fts, full);
        if (!fs) {
            diag_error(m->pos, "结构体 `%s` 没有方法 `%s`", tn, m->member.name);
            // 列出该结构体真实存在的方法 —— 与「字段名拼错」同一
            // 思路：直接给候选比让用户回去翻代码快。
            char buf[1024];
            buf[0] = '\0';
            size_t prelen = strlen(tn) + 1;
            for (int i = 0; i < c->fts->count; i++) {
                const char* nm = c->fts->sigs[i].name;
                if (strncmp(nm, tn, prelen - 1) == 0 && nm[prelen - 1] == '.') {
                    if (buf[0]) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
                    strncat(buf, nm + prelen, sizeof(buf) - strlen(buf) - 1);
                }
            }
            if (buf[0]) {
                diag_note("`%s` 已有的方法：%s", tn, buf);
            } else {
                diag_note("`%s` 目前没有任何方法", tn);
            }
            diag_help("方法就是普通的 `fn %s.名字(...)`。"
                      "声明时第一个参数写接收者：`fn %s.%s(self: %s, ...)`",
                      tn, tn, m->member.name, tn);
            free(full);
            compile_expr(c, m->member.obj);
            emit(c, BC_POP, 0, e->pos);
            for (int i = 0; i < e->call.argc; i++) {
                compile_expr(c, e->call.args[i]);
                emit(c, BC_POP, 0, e->pos);
            }
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_UNKNOWN;
        }

        // 参数个数：显式实参 +1（接收者）
        if (e->call.argc + 1 != fs->param_count) {
            diag_error(e->pos, "方法 `%s.%s` 需要 %d 个参数（不含接收者），实际给了 %d 个",
                       tn, m->member.name, fs->param_count - 1, e->call.argc);
            if (fs->param_count > 1 && fs->param_types) {
                diag_note("它的第一个参数是接收者 `self: %s`",
                          fs->param_types[0] ? fs->param_types[0] : "?");
            }
        }

        // 接收者当第 0 个实参压栈 —— 必须**先**压，它对应槽位 0。
        // 这里做一次接收者的类型检查（真正那套通用检查在下面，
        // 但它按 param_types 逐位比，而这里 e->call.args 的下标 0
        // 是用户的第一个实参，不是接收者，所以分开处理）。
        TyKind rt = compile_expr(c, m->member.obj);
        if (fs->param_types && fs->param_types[0]) {
            TyKind want = ty_from_name(fs->param_types[0]);
            if (want != TY_UNKNOWN && rt != TY_UNKNOWN && rt != want) {
                diag_error(m->member.obj->pos,
                           "方法 `%s.%s` 的接收者声明为 %s，实际是 %s",
                           tn, m->member.name, fs->param_types[0], ty_name(rt));
                diag_help("接收者就是 `fn %s.%s(self: %s, ...)` 里的第一个参数。"
                          "它必须与宿主类型一致", tn, m->member.name, tn);
            }
        }

        // 其余实参按序压栈
        for (int i = 0; i < e->call.argc; i++) {
            TyKind at = compile_expr(c, e->call.args[i]);
            // 下标要 +1 跳过接收者
            if (fs->param_types && i + 1 < fs->param_count && fs->param_types[i + 1]) {
                TyKind pt = ty_from_name(fs->param_types[i + 1]);
                bool numeric_pair =
                    (pt == TY_INT && at == TY_FLOAT) ||
                    (pt == TY_FLOAT && at == TY_INT);
                if (pt != TY_UNKNOWN && at != TY_UNKNOWN && at != pt && !numeric_pair) {
                    diag_error(e->call.args[i]->pos,
                               "方法 `%s.%s` 的第 %d 个参数声明为 %s，实际传入 %s",
                               tn, m->member.name, i + 1, ty_name(pt), ty_name(at));
                    diag_help("参数类型是编译期契约：运行时不会自动转换");
                }
            }
        }

        emit(c, BC_CALL, fs->bc_index, e->pos);
        free(full);
        return fs->ret_type;
    }

    // P0 只支持按名字调用
    if (e->call.callee->kind != EX_IDENT) {
        diag_error(e->pos, "只支持按函数名调用，不支持调用表达式");
        diag_help("可写 `f(x)`（普通函数）或 `obj.m(x)`（结构体方法）");
        return TY_UNKNOWN;
    }
    const char* n = e->call.callee->ident.name;

    // ---------- 内置函数 ----------
    if (strcmp(n, "print") == 0 || strcmp(n, "println") == 0) {
        for (int i = 0; i < e->call.argc; i++) {
            TyKind at = compile_expr(c, e->call.args[i]);
            emit_to_string(c, at, e->call.args[i]->pos);
            emit(c, BC_PRINT, 0, e->call.args[i]->pos);
        }
        if (strcmp(n, "println") == 0) {
            int k = bc_add_const_str(c->bc, "\n", 1);
            emit(c, BC_PUSH_STR, k, e->pos);
            emit(c, BC_PRINT, 0, e->pos);
        }
        // 调用表达式总有值（void 用 null 表示）
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return TY_NULL;
    }

    if (strcmp(n, "toInt") == 0) {
        if (e->call.argc != 1) {
            diag_error(e->pos, "`toInt` 需要恰好 1 个参数，实际给了 %d 个", e->call.argc);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_INT;
        }
        compile_expr(c, e->call.args[0]);
        emit(c, BC_TOINT, 0, e->pos);
        return TY_INT;
    }

    if (strcmp(n, "toFloat") == 0) {
        if (e->call.argc != 1) {
            diag_error(e->pos, "`toFloat` 需要恰好 1 个参数，实际给了 %d 个", e->call.argc);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_FLOAT;
        }
        compile_expr(c, e->call.args[0]);
        emit(c, BC_TOFLOAT, 0, e->pos);
        return TY_FLOAT;
    }

    if (strcmp(n, "toString") == 0) {
        if (e->call.argc != 1) {
            diag_error(e->pos, "`toString` 需要恰好 1 个参数，实际给了 %d 个", e->call.argc);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_STRING;
        }
        TyKind at = compile_expr(c, e->call.args[0]);
        emit_to_string(c, at, e->pos);
        return TY_STRING;
    }

    if (strcmp(n, "abs") == 0) {
        if (e->call.argc != 1) {
            diag_error(e->pos, "`abs` 需要恰好 1 个参数，实际给了 %d 个", e->call.argc);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_INT;
        }
        TyKind at = compile_expr(c, e->call.args[0]);
        if (at == TY_FLOAT) {
            emit(c, BC_ABS_F64, 0, e->pos);
            return TY_FLOAT;
        }
        if (at == TY_INT) {
            emit(c, BC_ABS_I64, 0, e->pos);
            return TY_INT;
        }
        // 类型未知：用 TOFLOAT/TOINT 往返不划算，直接报错更诚实
        diag_error(e->pos, "`abs` 的参数类型必须是 Int 或 Float，实际是 %s", ty_name(at));
            diag_help("`abs` 只处理数值。字符串请改用 `len`，或用 `toInt`/`toFloat` 先转换");
        emit(c, BC_POP, 0, e->pos);
        emit(c, BC_PUSH_NULL, 0, e->pos);
        return TY_INT;
    }

    if (strcmp(n, "len") == 0) {
        if (e->call.argc != 1) {
            diag_error(e->pos, "`len` 需要恰好 1 个参数，实际给了 %d 个", e->call.argc);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_INT;
        }
        TyKind at = compile_expr(c, e->call.args[0]);
        if (at != TY_STRING && at != TY_ARRAY && at != TY_UNKNOWN) {
            diag_error(e->pos, "`len` 只接受 String 或 Array，实际是 %s", ty_name(at));
            diag_help("String 返回 UTF-8 字节数，Array 返回元素个数");
        }
        emit(c, BC_LEN, 0, e->pos);
        return TY_INT;
    }

    // array(n, fill) —— 建一个长度为 n、每个元素都是 fill 的数组。
    //
    // ── 为什么是两个参数而不是一个 ────────────────────────────
    //
    // 只有 array(n) 的话，新数组里的元素必须有个默认值。选什么都有问题：
    // 0 会让人以为「数组里是 0」而不是「未初始化」；null 则让每个元素
    // 都要做类型判断，扫雷这种纯整数棋盘会白白多一层分支。
    // 显式传 fill 把选择交给用户，编译器不需要猜。
    //
    // 押入顺序与解释器一致：先压填充值，再压长度（长度在最顶上）。
    if (strcmp(n, "array") == 0) {
        if (e->call.argc != 2) {
            diag_error(e->pos, "`array` 需要 2 个参数（长度, 填充值），实际给了 %d 个",
                       e->call.argc);
            diag_help("例：`let row = array(10, 0)` 建一个 10 个 0 的数组");
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_ARRAY;
        }
        // 求值顺序从左到右（与其它调用一致）：先长度，后填充值。
        // 压栈顺序也是这个顺序，于是栈上从下到上是 [长度, 填充值]。
        // 解释器 BC_NEW_ARRAY 按这个约定弹：先弹填充值，再弹长度。
        TyKind lt = compile_expr(c, e->call.args[0]);
        if (lt != TY_INT && lt != TY_UNKNOWN) {
            diag_error(e->call.args[0]->pos,
                       "`array` 的长度必须是 Int，实际是 %s", ty_name(lt));
            diag_help("长度要从数值算出来，比如 `array(n * n, 0)`");
        }
        compile_expr(c, e->call.args[1]);
        emit(c, BC_NEW_ARRAY, 0, e->pos);
        return TY_ARRAY;
    }

    // ---------- 动态调用（classNew / methodCall，v0.20）----------
    //
    // 这两个名字不走下面的通用内建通道：它们要在**运行时推调用帧**
    // （构造函数体、方法体都要执行），而 BC_BUILTIN 的约定是
    // 「弹参 → 压返回值」，控制流交不回来。所以编译期就特判成专用
    // 指令，让解释器在主循环里走与 BC_NEW_CLASS / BC_VIRT_CALL
    // 同一套推帧逻辑。
    //
    // 签名（与 anno* 的「实参数组」风格一致）：
    //   classNew("类名")               或  classNew("类名", [实参...])
    //   methodCall(实例, "方法名")      或  methodCall(实例, "方法名", [实参...])
    //
    // ── 为什么实参收「一个数组」而不是散列参数 ────────────────
    //
    // 反射调用点的实参列表几乎总是**运行时数据**：路由分派把请求
    // 参数绑定成数组，容器按 bean 定义传依赖。散列写法在编译期就
    // 把实参个数定死了，没法把一个运行期拼出来的列表展开传进去。
    // 收数组的话，解释器在弹出时按元素展开 —— 个数天然是运行时的。
    //
    // 栈布局（弹出顺序 = 压入顺序的逆序）：
    //   classNew    [类名, 实参数组?]        → BC_NEW_CLASS_NAME, b = 0|1
    //   methodCall  [接收者, 方法名, 实参数组?] → BC_CALL_NAME, b = 0|1
    //   b 是「有没有实参数组」的标志位，不是实参个数。
    if (strcmp(n, "classNew") == 0) {
        if (e->call.argc < 1 || e->call.argc > 2) {
            diag_error(e->pos,
                       "`classNew` 要 1~2 个参数（类名, 实参数组?），实际给了 %d 个",
                       e->call.argc);
            diag_help("用法：classNew(\"User\") 或 classNew(\"User\", [\"ann\", 18])");
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_UNKNOWN;
        }
        TyKind nt = compile_expr(c, e->call.args[0]);
        if (nt != TY_STRING && nt != TY_UNKNOWN) {
            diag_error(e->call.args[0]->pos,
                       "`classNew` 的第 1 个参数必须是 String 类名，实际是 %s",
                       ty_name(nt));
        }
        if (e->call.argc == 2) {
            TyKind at = compile_expr(c, e->call.args[1]);
            if (at != TY_ARRAY && at != TY_UNKNOWN) {
                diag_error(e->call.args[1]->pos,
                           "`classNew` 的第 2 个参数必须是实参数组，实际是 %s",
                           ty_name(at));
            }
            emit(c, BC_NEW_CLASS_NAME, 0, e->pos);
            c->bc->code[c->bc->code_count - 1].b = 1;
        } else {
            emit(c, BC_NEW_CLASS_NAME, 0, e->pos);
            c->bc->code[c->bc->code_count - 1].b = 0;
        }
        return TY_UNKNOWN;
    }
    if (strcmp(n, "methodCall") == 0) {
        if (e->call.argc < 2 || e->call.argc > 3) {
            diag_error(e->pos,
                       "`methodCall` 要 2~3 个参数（接收者, 方法名, 实参数组?），"
                       "实际给了 %d 个",
                       e->call.argc);
            diag_help("用法：methodCall(obj, \"sum\") 或 methodCall(obj, \"sum\", [1, 2])");
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_UNKNOWN;
        }
        // 压栈顺序 = 栈布局从下到上：接收者、方法名、实参数组?。
        // 解释器按相反顺序弹出（数组? → 方法名 → 接收者）。
        compile_expr(c, e->call.args[0]);
        TyKind nt = compile_expr(c, e->call.args[1]);
        if (nt != TY_STRING && nt != TY_UNKNOWN) {
            diag_error(e->call.args[1]->pos,
                       "`methodCall` 的第 2 个参数必须是 String 方法名，"
                       "实际是 %s",
                       ty_name(nt));
        }
        if (e->call.argc == 3) {
            TyKind at = compile_expr(c, e->call.args[2]);
            if (at != TY_ARRAY && at != TY_UNKNOWN) {
                diag_error(e->call.args[2]->pos,
                           "`methodCall` 的第 3 个参数必须是实参数组，实际是 %s",
                           ty_name(at));
            }
            emit(c, BC_CALL_NAME, 0, e->pos);
            c->bc->code[c->bc->code_count - 1].b = 1;
        } else {
            emit(c, BC_CALL_NAME, 0, e->pos);
            c->bc->code[c->bc->code_count - 1].b = 0;
        }
        return TY_UNKNOWN;
    }

    // ---------- 平台内建（win* / arr* / str* / math 系列）----------
    //
    // 这些函数由运行时实现，编译器只负责把参数压栈并发一条 BC_BUILTIN。
    // 参数个数在编译期就对表校验 —— 早报错比运行到那一步才发现好。
    {
        int bi = bc_builtin_find(n);
        if (bi >= 0) {
            XBuiltin bid = bc_builtin_id(bi);

            // ── math 是统一的数学入口，参数个数由第一个参数决定 ──
            //
            // 它不能用通用路径：签名表里 argc 是 -1（可变），而具体
            // 是哪个子操作只有看到源码才知道（`math(pow, 2, 3)` 里
            // 第一个实参必须是能静态求值的子操作名）。
            //
            // 为什么把子操作作为**第一个参数名**而不是让用户写
            // `pow(2,3)`：那会往全局命名空间塞 25 个名字，与用户
            // 函数撞名的概率不低，而 X 没有命名空间来隔离。
            if (bid == XBI_MATH) {
                return compile_math_call(c, e);
            }

            int want = bc_builtin_argc(bi);
            if (want >= 0 && e->call.argc != want) {
                diag_error(e->pos, "`%s` 需要 %d 个参数，实际给了 %d 个",
                           n, want, e->call.argc);
                diag_help("这是内建函数，签名固定。用 `x stdlib` 看不到它，"
                          "它在语言内建表里（见 README 的内建清单）。");
                // 仍然压 null 占位，避免栈失衡引发一堆连带错误
                for (int i = 0; i < e->call.argc; i++) compile_expr(c, e->call.args[i]);
                for (int i = 0; i < e->call.argc; i++) emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return ty_from_name(bc_builtin_ret(bi));
            }

            for (int i = 0; i < e->call.argc; i++) {
                compile_expr(c, e->call.args[i]);
            }
            emit(c, BC_BUILTIN, (int)bid, e->pos);
            // b 操作数存参数个数，单独设：emit 只带 a。
            // 这里直接改最后一条指令的 b —— 清楚而且不用扩展 emit。
            c->bc->code[c->bc->code_count - 1].b = (int16_t)e->call.argc;

            // 返回类型。表里写 "Unknown" 的（arrPop/arrRemove）确实
            // 取决于运行时元素类型，给 TY_UNKNOWN 是对的 —— 后续
            // 类型推断遇到 UNKNOWN 会走动态路径，而不是错误地假定 Int。
            TyKind rt = ty_from_name(bc_builtin_ret(bi));
            return rt;
        }
    }

    // ---------- 用户函数 ----------
    //
    // 先编译实参、收集静态类型，再选签名。为什么顺序与旧版不同：
    // 旧版按名字找到唯一签名后边编译实参边查类型；支持重载之后
    // 「选哪个签名」取决于实参类型，实参必须先编译完。
    // 单签名场景发射的指令与旧版逐条相同（先压实参、再 BC_CALL）。
    TyKind* argt = NULL;
    if (e->call.argc > 0) {
        argt = (TyKind*)x_alloc(sizeof(TyKind) * (size_t)e->call.argc);
        for (int i = 0; i < e->call.argc; i++) {
            argt[i] = compile_expr(c, e->call.args[i]);
        }
    }

    bool fn_exists = false;
    FuncSig* s = resolve_overload(c->fts, n, e->call.argc, argt, e->pos,
                                  &fn_exists, false);
    if (!s && !fn_exists) {
        diag_error(e->pos, "未定义的函数 `%s`", n);
        // ── 这条提示曾经是错的 ──────────────────────────────
        //
        // 原文写死「P0 没有模块系统，函数必须在同一文件内定义」。
        // 那句话在加 import 之前是对的，之后就成了**误导**。
        //
        // 而且它命中的典型场景恰恰相反：用户想用标准库却忘了
        // import。此时真正该做的是加一行 import，而不是在本文件里
        // 重写一个同名函数 —— 提示却在教他走后者。
        //
        // 为什么这里不能按「有没有模块系统」分支：import 是**编译
        // 前**在 main.c 里展开的（文本级合并），到了 compile.c 这个
        // 阶段所有被 import 的函数都已经是本文件的 fn 了。也就是说
        // 编译器这里根本看不到「模块系统开没开」，写个条件分支只会
        // 得到永远为真的假实现。
        //
        // 所以按「函数名像不像标准库的」给提示 —— 那是这里唯一
        // 能拿到的真实信息。
        bool looks_std = false;
        const char* prefixes[] = {
            "str", "arr", "mat", "json", "toBase", "toHex", "toBin",
            "toOct", "fromHex", "fromBin", "padZero", "fixed",
            "thousands", "percent", "bytes", "uiButton", "uiCheck",
            "uiSlider", "uiList", "uiPanel", "uiProgress", "uiStatus",
            "uiText", "uiField", "uiRadio", "uiToolbar", "uiSeparator",
            "isSpace", "isPrintable", "escapeChar", "numbersToString",
            "stringToNumbers", "stringToInts", "nullValue", "boolToString"
        };
        for (int pi = 0; pi < (int)(sizeof(prefixes) / sizeof(prefixes[0])); pi++) {
            if (strncmp(n, prefixes[pi], strlen(prefixes[pi])) == 0) {
                looks_std = true;
                break;
            }
        }
        if (looks_std) {
            diag_help("`%s` 看起来是标准库函数 —— 文件开头按名字导入：", n);
            diag_note("例如 `from std.math import %s`，"
                      "std.str / std.array / std.json / std.conv / std.ui 同理", n);
        } else {
            diag_help("确认 `fn %s(...) { ... }` 已声明；"
                      "如果它在别的文件里，加 `from \"那个文件.xle\" import %s`", n, n);
        }
    }
    if (!s) {
        // 弹掉已编译的实参，压 null 占位，避免栈失衡引发连锁错误。
        // （实参在上面已经编译过 —— 重载决策需要它们的类型。）
        for (int i = 0; i < e->call.argc; i++) emit(c, BC_POP, 0, e->pos);
        emit(c, BC_PUSH_NULL, 0, e->pos);
        if (argt) free(argt);
        return TY_UNKNOWN;
    }

    // （参数个数的校验在 resolve_overload 里完成：单签名沿用旧文案
    // 「需要 N 个参数」，多签名报「没有匹配个数的重载」。）

    // ---- 参数类型检查 ----
    //
    // ── 为什么必须有 ────────────────────────────────────────
    //
    // 之前这里直接编译实参、调 BC_CALL，不做任何类型比对。看起来
    // 「反正运行时的值自带类型，函数体里按实际类型走就行」，但实际上
    // 返回值会被调用方按**声明**的返回类型解释 —— 于是
    //
    //     fn f(x: Int) -> String { return toString(x) }
    //     println(f("hello"))        // 打印出一串指针数字
    //
    // 函数体里一切正常（toString 拿到 String，返回 String），但
    // 调用点认为「返回类型是 String」却按 Int 的参数槽去读，得到
    // 的是被当成 int64 的指针值。这不是「少检查一个类型」，而是
    // 产生了被静默接受的内存误解释。
    //
    // 标准库大量使用「一个函数吃多种类型」的写法（jsonStringify
    // 要处理 Int/Float/String/Bool/Null），所以这条限制必须先堵上，
    // 而不是靠调用方自觉。
    //
    // ── 允许的放宽 ──────────────────────────────────────────
    //
    // Int 与 Float 之间互相放行：语言已有数值隐式转换的语义
    // （01 §4.2），`fn f(x: Float)` 传 1 是合理的。其余类型
    // 一律严格。UNKNOWN 一律放行 —— 那是编译器放弃推断的情况，
    // 不能因为推断不出来就报错。
    for (int i = 0; i < e->call.argc; i++) {
        TyKind at = argt[i];   // 实参已在重载决策前编译，这里直接取类型
        if (i < s->param_count && s->param_types && s->param_types[i]) {
            TyKind pt = ty_from_name(s->param_types[i]);
            bool numeric_pair =
                (pt == TY_INT && at == TY_FLOAT) ||
                (pt == TY_FLOAT && at == TY_INT);
            if (pt != TY_UNKNOWN && at != TY_UNKNOWN &&
                at != pt && !numeric_pair) {
                diag_error(e->call.args[i]->pos,
                           "函数 `%s` 的第 %d 个参数声明为 %s，实际传入 %s",
                           n, i + 1, ty_name(pt), ty_name(at));
                diag_help("参数类型是编译期契约：运行时不会自动转换。"
                          "用 `toInt` / `toFloat` / `toString` 显式转换");
            }
        }
    }

    if (argt) free(argt);
    emit(c, BC_CALL, s->bc_index, e->pos);
    return s->ret_type;
}

static TyKind compile_expr(Ctx* c, Expr* e) {
    if (!e) return TY_UNKNOWN;

    switch (e->kind) {
        case EX_INT: {
            int k = bc_add_const_i64(c->bc, e->i.v);
            emit(c, BC_PUSH_I64, k, e->pos);
            return TY_INT;
        }
        case EX_FLOAT: {
            int k = bc_add_const_f64(c->bc, e->f.v);
            emit(c, BC_PUSH_F64, k, e->pos);
            return TY_FLOAT;
        }
        case EX_STRING: {
            int k = bc_add_const_str(c->bc, e->str.s, e->str.len);
            emit(c, BC_PUSH_STR, k, e->pos);
            return TY_STRING;
        }
        case EX_BOOL:
            emit(c, e->b.v ? BC_PUSH_TRUE : BC_PUSH_FALSE, 0, e->pos);
            return TY_BOOL;

        case EX_NULL:
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_NULL;

        case EX_IDENT: {
            int slot = local_find(c, e->ident.name);
            if (slot >= 0) {
                emit(c, BC_LOAD_LOCAL, slot, e->pos);
                return local_type(c, slot);
            }
            // 局部变量里没有 → 查顶层常量。
            //
            // 顺序不能颠倒：局部优先。这保证「函数内的 let 可以遮蔽
            // 同名常量」这条常规作用域规则成立，也能让用户在函数里
            // 临时覆盖 `const STEP = 16` 做试验而不必改名。
            if (c->ct) {
                GlobalConst* g = ct_find(c->ct, e->ident.name);
                if (g) return ct_emit(c, g, e->pos);
            }
            diag_error(e->pos, "未定义的变量 `%s`", e->ident.name);
        diag_help("先用 `let %s = ...` 声明，或检查拼写", e->ident.name);
            emit(c, BC_PUSH_NULL, 0, e->pos);   // 占位，避免连锁栈错误
            return TY_UNKNOWN;
        }

        case EX_BINARY: return compile_binary(c, e);

        case EX_UNARY: {
            TyKind t = compile_expr(c, e->unary.operand);
            switch (e->unary.op) {
                case UOP_NEG:
                    if (t == TY_FLOAT) {
                        emit(c, BC_NEG_F64, 0, e->pos);
                        return TY_FLOAT;
                    }
                    if (t != TY_INT) {
                        diag_error(e->pos, "一元 `-` 只接受 Int 或 Float，实际是 %s", ty_name(t));
            diag_help("取负只对数值有意义。若要判断真假，用 `!` 或 `not`");
                    }
                    emit(c, BC_NEG_I64, 0, e->pos);
                    return TY_INT;
                case UOP_NOT:
                    emit(c, BC_NOT, 0, e->pos);
                    return TY_BOOL;
                case UOP_BNOT:
                    if (t != TY_INT) {
                        diag_error(e->pos, "一元 `~` 只接受 Int，实际是 %s", ty_name(t));
            diag_help("`~` 是整数按位取反。Float 没有位表示，请先 `toInt`");
                    }
                    emit(c, BC_BNOT_I64, 0, e->pos);
                    return TY_INT;
                case UOP_POS:
                    return t;
            }
            return t;
        }

        case EX_IF: {
            // if 是表达式，两个分支都往栈上留一个值
            compile_expr(c, e->ifx.cond);
            int jf = emit_jump(c, BC_JUMP_IF_FALSE, e->pos);

            TyKind then_t = compile_expr(c, e->ifx.then_block);
            int jend = emit_jump(c, BC_JUMP, e->pos);

            patch_here(c, jf);
            TyKind else_t;
            if (e->ifx.else_expr) {
                else_t = compile_expr(c, e->ifx.else_expr);
            } else {
                emit(c, BC_PUSH_NULL, 0, e->pos);
                else_t = TY_NULL;
            }
            patch_here(c, jend);

            return ty_merge_branches(then_t, else_t);
        }

        // `cond ? a : b`
        //
        // 与 EX_IF 走**完全相同的字节码**：条件压栈 → JUMP_IF_FALSE
        // → 真支 → JUMP → 假支 → 汇合。不新增指令的意思是 .xbc 的
        // 格式版本不用动，旧的 .xbc 也不会因为这条语法失效。
        //
        // 条件只求值一次：compile_expr(cond) 之后栈上就是那个布尔，
        // JUMP_IF_FALSE 会把它弹掉。没有「先存到临时变量再读两次」
        // 的必要，也没有那个开销。
        case EX_TERNARY: {
            compile_expr(c, e->tern.cond);
            int jf = emit_jump(c, BC_JUMP_IF_FALSE, e->pos);

            TyKind then_t = compile_expr(c, e->tern.then_e);
            int jend = emit_jump(c, BC_JUMP, e->pos);

            patch_here(c, jf);
            TyKind else_t = compile_expr(c, e->tern.else_e);
            patch_here(c, jend);

            // 类型合并规则与 EX_IF 逐条一致。两处必须同源 ——
            // 一边改了另一处没改的话，症状是「同样的两支，写成 if
            // 能过、写成三元报类型错」，那种不一致极难自证。
            return ty_merge_branches(then_t, else_t);
        }

        // `[a, b, c]`
        //
        // 降成「建空数组 + 逐个 arrPush」。不新增指令，理由是 arrPush
        // 已经在字节码里、已经过 AOT 与解释器两条路径的验证，而数组
        // 字面量本质上就是这两步的语法糖 —— 它省的是书写，不是能力。
        //
        // 空字面量 `[]` 也走这条路径：建一个长 0 的数组。允许它的
        // 理由是要能写 `var xs = []` 然后循环里 push，这是极常见的
        // 形态；不允许的话用户就得先想一个填充值。
        case EX_ARRAY_LIT: {
            // NEW_ARRAY 要两个栈值：[长度, 填充值]。建 0 长数组，
            // 填充值随意（0 长度时不会被用到），给 0 省一个常量池槽。
            emit(c, BC_PUSH_I64, bc_add_const_i64(c->bc, 0), e->pos);  // 长度 0
            emit(c, BC_PUSH_I64, bc_add_const_i64(c->bc, 0), e->pos);  // 填充值占位
            emit(c, BC_NEW_ARRAY, 0, e->pos);

            // ── 为什么每个元素前都要 DUP ──────────────────────
            //
            // arrPush(arr, v) 的参数是**按值传**的：它会弹掉 arr 和 v。
            // 如果直接 `PUSH v; BUILTIN arrPush`，弹掉 arr 之后数组本体
            // 就只剩 arrPush 内部那一份引用，而它返回的是**长度**（Int），
            // 不是数组 —— 于是栈上留下的是长度，数组丢了。
            //
            // 症状极具误导性：`let b = [9]` 之后 b 是 null，
            // 报错却是「len 只接受 String 或 Array，实际是 Null」，
            // 指向 len 而不指向数组字面量。
            //
            // 所以顺序是：DUP（留住数组）→ PUSH v → arrPush → POP（弹长度）。
            for (int i = 0; i < e->arrlit.count; i++) {
                emit(c, BC_DUP, 0, e->pos);          // 栈: [arr, arr]
                compile_expr(c, e->arrlit.elems[i]); // 栈: [arr, arr, v]
                // arrPush 的参数顺序是「数组在前、元素在后」，与此时
                // 栈上的 [arr, v] 一致（下面那个 arr 是 DUP 出来的）。
                emit(c, BC_BUILTIN, XBI_ARR_PUSH, e->pos);
                // b 操作数存参数个数，必须单独设。漏了这一步的话
                // 解释器按 argc=0 处理，什么参数都不弹 —— 于是它去
                // 弹栈顶当 arr，实际弹到的是别的东西，报「第 1 个
                // 参数必须是 Array，实际是 Null」。这个错误看起来像
                // 数组没建出来，实际是 argc 没写。
                c->bc->code[c->bc->code_count - 1].b = 2;
                // arrPush 返回新长度（Int），弹掉。留着的话它就成了
                // 整个字面量的值 —— 而那个值本该是数组。
                emit(c, BC_POP, 0, e->pos);
            }
            return TY_ARRAY;
        }

        // `a ?? b`
        //
        // ── 为什么不用「牺牲一次重复求值」换简单 ────────────────
        //
        // 最省事的写法是把 `a ?? b` 糖化成 `a != null ? a : b`，
        // 但那会把 a 算两遍：a 是 `f()` 时等于两次函数调用，副作用
        // 重复，而调用点完全看不出来。所以必须保住「只求值一次」。
        //
        // ── 怎么用现有指令做到 ─────────────────────────────────
        //
        //      compile(a)            栈: [a]
        //      DUP                   栈: [a, a]
        //      PUSH_NULL             栈: [a, a, null]
        //      BC_NE                 栈: [a, a!=null]
        //      JUMP_IF_FALSE takeR   → 弹掉比较结果
        //      （a 留在栈上，正好是结果）JUMP end
        //    takeR:
        //      POP                   弹掉那个 null
        //      compile(b)            栈: [b]
        //    end:
        //
        // 全程没有新增指令 —— `??` 是 DUP + 比较 + 两条跳转的组合，
        // 因此 .xbc 格式版本不用动，旧的 .xbc 也不会失效。
        //
        // 代价是比专门的 JUMP_IF_NOT_NULL 多两条指令（PUSH_NULL + NE）。
        // 这个代价换的是「字节码格式不变」，而格式变更会让所有已编译
        // 的 .xbc 失效 —— 那个成本高得多。
        case EX_NULL_COALESCE: {
            compile_expr(c, e->coal.lhs);
            emit(c, BC_DUP, 0, e->pos);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            emit(c, BC_NE, 0, e->pos);
            int jtake_rhs = emit_jump(c, BC_JUMP_IF_FALSE, e->pos);

            // 左侧非 null：它已经在栈上，直接跳到汇合点
            int jend = emit_jump(c, BC_JUMP, e->pos);

            patch_here(c, jtake_rhs);
            emit(c, BC_POP, 0, e->pos);        // 弹掉那个 null
            TyKind rt = compile_expr(c, e->coal.rhs);
            patch_here(c, jend);

            return ty_merge_branches(infer_type(c, e->coal.lhs), rt);
        }

        // `a?.b` / `a?[i]`
        //
        // 语义：对象是 null 时整个访问短路成 null，否则正常访问。
        //
        // ── 为什么节点里存的是「完整的访问表达式」────────────────
        //
        // 存 access（EX_MEMBER / EX_INDEX）而不是「对象 + 名字」，
        // 是为了复用正常的成员访问/索引逻辑 —— 包括字段偏移、类与
        // 结构体两条分支、可见性检查。若只存名字，这些都得在可选链
        // 分支里再抄一遍，抄漏一处的症状是「结构体的可选字段访问
        // 不了，类可以」，极难定位。
        //
        // ── 怎么把「已算好的对象」喂给那段逻辑 ──────────────────
        //
        // 那段逻辑的第一步是「编译对象表达式」。把对象指针临时换成
        // EX_STACK_REF（编译它不发射任何指令），就正好复用 —— 栈顶
        // 已经是我们 DUP 出来的那个值。
        case EX_OPT_CHAIN: {
            Expr* acc = e->optc.access;
            Expr** obj_slot = (acc->kind == EX_MEMBER) ? &acc->member.obj
                                                       : &acc->index.obj;

            // ── 宿主静态已知是 null：整串恒为 null ──────────────
            //
            // 这既是优化也是必需：`let p = null` 的 p 没有结构体类型，
            // 下面的成员访问会因为「无法确定 .n 的宿主类型」直接报错。
            // 但 `p?.n` 的意义恰恰是「p 可能是 null，不必给它类型」。
            // 所以常量折叠在这里不是省两条指令，而是让写法成立。
            //
            // 两种 null 来源都要认：直接写 `null`（EX_NULL），以及
            // `let p = null` 之后的 p（推断为 TY_NULL，没有结构体名）。
            {
                bool is_null_host = ((*obj_slot)->kind == EX_NULL);
                if (!is_null_host && !expr_struct_name(c, *obj_slot)) {
                    is_null_host = (infer_type(c, *obj_slot) == TY_NULL);
                }
                if (is_null_host) {
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_NULL;
                }
            }

            compile_expr(c, *obj_slot);            // 栈: [obj]
            emit(c, BC_DUP, 0, e->pos);            // 栈: [obj, obj]
            emit(c, BC_PUSH_NULL, 0, e->pos);
            emit(c, BC_NE, 0, e->pos);             // 栈: [obj, obj != null]
            int jnull = emit_jump(c, BC_JUMP_IF_FALSE, e->pos);

            // 非 null：走正常访问，但对象不重新求值。
            //
            // 宿主的结构体名必须在替换**之前**算好 —— 替换之后
            // expr_struct_name 看到的是 EX_STACK_REF，无从推断。
            const char* host = expr_struct_name(c, *obj_slot);
            Expr* saved = *obj_slot;
            Expr* ref = arena_alloc(c->arena, sizeof(Expr));
            memset(ref, 0, sizeof(*ref));
            ref->kind = EX_STACK_REF;
            ref->pos  = e->pos;
            ref->ident.name = (char*)host;   // 借用，不拥有
            *obj_slot = ref;
            TyKind at = compile_expr(c, acc);
            *obj_slot = saved;                     // 还原，避免影响共享 AST

            int jend = emit_jump(c, BC_JUMP, e->pos);

            // null：弹掉那个 null，压 null 作为整串的结果
            patch_here(c, jnull);
            emit(c, BC_POP, 0, e->pos);
            emit(c, BC_PUSH_NULL, 0, e->pos);
            patch_here(c, jend);

            return ty_merge_branches(at, TY_NULL);
        }

        // 值已在栈顶，什么都不做。
        //
        // 返回 TY_UNKNOWN 而不是猜一个类型：调用方（成员访问/索引）
        // 拿到 UNKNOWN 会走动态路径，那正是「类型可能来自运行时」的
        // 正确表达 —— 可选链的对象在编译期常常推不出静态类型。
        case EX_STACK_REF:
            return TY_UNKNOWN;

        // `match x { pat => body, ... }`
        //
        // ── 降级成一条比较链 ──────────────────────────────────
        //
        // 不新增任何字节码指令。整体形状是：
        //
        //     <算 x>             栈: [x]
        //     LOAD? 不需要 —— x 一直在栈上供每次比较复用
        //
        // 但「x 只求值一次」与「多处比较」要求它在栈上留一份底。
        // 所以实际做法是：先算 x 并 **STORE_LOCAL 到一个临时槽**，
        // 之后每个分支都从那个槽取值来比较。牺牲一个局部槽，换来
        // 三段逻辑（字面量/范围/通配）都不必关心 x 的来源。
        //
        // 每个分支的形状：
        //     LOAD tmp; <算模式值>; EQ; JUMP_IF_FALSE next
        //     [guard 检查]
        //     <body>; JUMP end
        //   next:
        //
        // 范围分支把 EQ 换成「GE && LE」，两条比较用 AND 串起来：
        //     LOAD tmp; <lo>; GE; JUMP_IF_FALSE next
        //     LOAD tmp; <hi>; LE; JUMP_IF_FALSE next
        //
        // 绑定（`n @ pat`）在分支体内先 STORE 到那个名字的槽。
        case EX_MATCH: {
            // 被匹配值存到临时槽：每个分支都要读它比较，而它只能
            // 求值一次（可能是函数调用）。
            int tmp = local_add(c, "$match", TY_UNKNOWN);
            compile_expr(c, e->matchx.subject);
            emit(c, BC_STORE_LOCAL, tmp, e->pos);

            // 两类待回填：分支失败要跳到「本分支 next」的跳转，
            // 以及分支体结束要跳到「整个 match 之后」的跳转。
            int cap = e->matchx.arm_count * 4 + 4;
            int* pend_next = x_alloc(sizeof(int) * (size_t)cap);
            int* pend_end  = x_alloc(sizeof(int) * (size_t)cap);
            int  n_next = 0, n_end = 0;

            TyKind acc = TY_UNKNOWN;

            for (int i = 0; i < e->matchx.arm_count; i++) {
                MatchArm* arm = &e->matchx.arms[i];

                if (arm->kind == PAT_LITERAL) {
                    emit(c, BC_LOAD_LOCAL, tmp, arm->pos);
                    compile_expr(c, arm->lit);
                    emit(c, BC_EQ, 0, arm->pos);
                    pend_next[n_next++] = emit_jump(c, BC_JUMP_IF_FALSE, arm->pos);
                } else if (arm->kind == PAT_RANGE) {
                    // 下界：tmp >= lo（开区间是 >）
                    emit(c, BC_LOAD_LOCAL, tmp, arm->pos);
                    compile_expr(c, arm->lo);
                    // 用 *_NUM 而不是 *_I64：范围端点可能是小数（`0.0..1.0`），
                    // 而 *_NUM 会按运行期实际类型选 Int/Float 比较。
                    emit(c, arm->inclusive ? BC_GE_NUM : BC_GT_NUM, 0, arm->pos);
                    pend_next[n_next++] = emit_jump(c, BC_JUMP_IF_FALSE, arm->pos);

                    // 上界：tmp <= hi（开区间是 <）
                    emit(c, BC_LOAD_LOCAL, tmp, arm->pos);
                    compile_expr(c, arm->hi);
                    emit(c, arm->inclusive ? BC_LE_NUM : BC_LT_NUM, 0, arm->pos);
                    pend_next[n_next++] = emit_jump(c, BC_JUMP_IF_FALSE, arm->pos);
                }
                // PAT_WILD：无条件命中，不发射比较

                // 绑定：把被匹配值存进绑定名的槽
                if (arm->binding) {
                    int bslot = local_find(c, arm->binding);
                    if (bslot < 0) bslot = local_add(c, arm->binding, TY_UNKNOWN);
                    emit(c, BC_LOAD_LOCAL, tmp, arm->pos);
                    emit(c, BC_STORE_LOCAL, bslot, arm->pos);
                }

                // guard：`pat if 条件 =>`，条件不成立也走 next
                if (arm->guard) {
                    compile_expr(c, arm->guard);
                    pend_next[n_next++] = emit_jump(c, BC_JUMP_IF_FALSE, arm->pos);
                }

                TyKind bt = compile_expr(c, arm->body);
                acc = ty_merge_branches(acc, bt);
                pend_end[n_end++] = emit_jump(c, BC_JUMP, arm->pos);

                // 本分支的 next 就在 body 之后、下个分支之前
                for (int k = 0; k < n_next; k++) patch_here(c, pend_next[k]);
                n_next = 0;
            }

            // 全都没命中：压 null。parser 强制要求 `_`，所以这条路径
            // 不可达；但字节码必须有「栈上留一个值」的保证，否则栈
            // 深度分析会算错 —— 那个错误会表现为「后面代码随机出错」。
            emit(c, BC_PUSH_NULL, 0, e->pos);

            for (int i = 0; i < n_end; i++) patch_here(c, pend_end[i]);
            free(pend_next);
            free(pend_end);
            return acc;
        }

        case EX_BLOCK: {
            if (!e->block.stmts || e->block.count == 0) {
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_NULL;
            }
            int saved = c->local_count;

            TyKind last_t = TY_NULL;
            for (int i = 0; i < e->block.count; i++) {
                Stmt* s = e->block.stmts[i];
                bool is_last = (i == e->block.count - 1);
                if (is_last && s->kind == ST_EXPR) {
                    last_t = compile_expr(c, s->expr.expr);   // 值留在栈上
                } else {
                    compile_stmt(c, s);
                }
            }
            // 最后一条不是表达式语句 → 块没有值，补 null 保证栈效果一致
            Stmt* last = e->block.stmts[e->block.count - 1];
            if (last->kind != ST_EXPR) {
                emit(c, BC_PUSH_NULL, 0, e->pos);
                last_t = TY_NULL;
            }

            c->local_count = saved;   // 退出块作用域
            return last_t;
        }

        case EX_ASSIGN: {
            // 下标赋值 `a[i] = v`。
            //
            // 生成顺序：先压容器、再压下标、最后压值，然后 INDEX_SET。
            // 这个顺序由栈语义决定 —— 最内层（最右）的先压，弹出时
            // 才能按「值、下标、容器」拿到。
            if (e->assign.target->kind == EX_INDEX) {
                Expr* ix = e->assign.target;
                compile_expr(c, ix->index.obj);
                compile_expr(c, ix->index.index);
                TyKind vt = compile_expr(c, e->assign.value);
                emit(c, BC_INDEX_SET, 0, e->pos);
                return vt;
            }

            if (e->assign.target->kind == EX_MEMBER) {
                // 字段赋值 `obj.x = 1`。
                //
                // ── 类是引用语义，所以这条合法 ────────────────────
                //
                // 结构体的字段不可变是**有意的限制**（见下面的拒绝分支）：
                // 值语义下「改副本还是改原件」没有正确答案。
                //
                // 类没有这个问题。`let b = a` 里 a 和 b 指向同一个对象，
                // 改 b.x 就是改 a.x —— 这不是实现细节，这就是引用语义的
                // 定义。所有有对象的语言都这样，用户不会误解。
                {
                    const char* host_cls = expr_class_name(c, e->assign.target->member.obj);
                    if (host_cls) {
                        int ci = bc_find_class(c->bc, host_cls);
                        if (ci >= 0) {
                            const char* fname = e->assign.target->member.name;
                            int fi = bc_find_class_field(c->bc, ci, fname);
                            if (fi < 0) {
                                ClassType* ct = &c->bc->classes[ci];
                                diag_error(e->pos, "类 `%s` 没有字段 `%s`",
                                           host_cls, fname);
                                char buf[512]; buf[0] = '\0';
                                for (int i = 0; i < ct->field_count; i++) {
                                    if (i) strncat(buf, ", ",
                                                   sizeof(buf) - strlen(buf) - 1);
                                    strncat(buf, ct->field_names[i],
                                            sizeof(buf) - strlen(buf) - 1);
                                }
                                diag_note("`%s` 的字段是：%s", host_cls, buf);
                                compile_expr(c, e->assign.value);
                                emit(c, BC_POP, 0, e->pos);
                                emit(c, BC_PUSH_NULL, 0, e->pos);
                                return TY_UNKNOWN;
                            }

                            ClassType* ct = &c->bc->classes[ci];

                            // ---- final 字段：只允许在构造函数里赋值 ----
                            if (ct->field_final[fi]) {
                                bool in_ctor = c->cur_class &&
                                               strcmp(c->cur_class, host_cls) == 0 &&
                                               c->cur_method &&
                                               strcmp(c->cur_method, "constructor") == 0;
                                if (!in_ctor) {
                                    diag_error(e->pos,
                                               "字段 `%s` 是 final 的，只能在构造函数里赋值",
                                               fname);
                                    diag_help("final 表示「构造完就不再变」。"
                                              "如果它需要中途改变，去掉 final");
                                }
                            }

                            // ---- 可见性 ----
                            {
                                Visibility fv = ct->field_vis[fi];
                                if (fv != VIS_PUBLIC) {
                                    int owner_ci = ci;
                                    while (owner_ci >= 0) {
                                        ClassType* o = &c->bc->classes[owner_ci];
                                        if (fi < o->own_field_start) {
                                            owner_ci = o->parent_idx;
                                            continue;
                                        }
                                        break;
                                    }
                                    bool allowed = false;
                                    if (c->cur_class) {
                                        int cur_ci = bc_find_class(c->bc, c->cur_class);
                                        if (fu_ci_eq(cur_ci, owner_ci)) allowed = true;
                                        else if (fv == VIS_PROTECTED) {
                                            allowed = bc_class_is_a(c->bc, cur_ci, owner_ci);
                                        }
                                    }
                                    if (!allowed) {
                                        // 分两路拼接，避免「不能在类外里赋值」
                                        // 这种多一个「里」的读起来像打字错误
                                        // 的句子。
                                        if (c->cur_class) {
                                            diag_error(e->pos,
                                                       "字段 `%s` 是 %s，不能在类 `%s` 里赋值",
                                                       fname,
                                                       fv == VIS_PRIVATE ? "private（私有）"
                                                                         : "protected（保护）",
                                                       c->cur_class);
                                        } else {
                                            diag_error(e->pos,
                                                       "字段 `%s` 是 %s，不能从类外赋值",
                                                       fname,
                                                       fv == VIS_PRIVATE ? "private（私有）"
                                                                         : "protected（保护）");
                                        }
                                        diag_help("封装的目的就是这个："
                                                  "外部只能通过方法改状态，"
                                                  "从而保证不变量始终成立");
                                    }
                                }
                            }

                            // ---- 类型检查 ----
                            TyKind wt = ty_from_name(ct->field_types[fi]);
                            TyKind at = infer_type(c, e->assign.value);
                            if (wt != TY_UNKNOWN && at != TY_UNKNOWN && wt != at) {
                                bool np2 = (wt == TY_INT || wt == TY_FLOAT) &&
                                           (at == TY_INT || at == TY_FLOAT);
                                bool poly = false;
                                if (wt == TY_CLASS && at == TY_CLASS) {
                                    const char* an = expr_class_name(c, e->assign.value);
                                    if (an) {
                                        int ai = bc_find_class(c->bc, an);
                                        int wi = bc_find_class(c->bc, ct->field_types[fi]);
                                        poly = bc_class_is_a(c->bc, ai, wi);
                                    }
                                }
                                if (!np2 && !poly) {
                                    diag_error(e->assign.value->pos,
                                               "字段 `%s` 声明为 %s，不能赋 %s",
                                               fname, ty_name(wt), ty_name(at));
                                }
                            }

                            // 栈序：先压对象，再压值。
                            // BC_FIELD_SET 弹值、弹对象、压回值 ——
                            // 最后压回的那个值让 `a.x = b.y = 1` 能成立。
                            compile_expr(c, e->assign.target->member.obj);
                            compile_expr(c, e->assign.value);
                            emit(c, BC_FIELD_SET, fi, e->pos);
                            return wt;
                        }
                    }
                }

                // 结构体字段：仍然拒绝（值语义下没有正确答案）
                diag_error(e->pos, "结构体字段不可赋值（P0.5 限制）");
                diag_note("字段不可变是为了让「值语义」真的成立："
                          "`let b = a` 之后 b 和 a 是两个独立的值，"
                          "改一个不可能影响另一个");
                diag_help("改用「构造一个新的」写法，例如 "
                          "`let r2 = Rect { x: 1, y: r.y, w: r.w, h: r.h }`。"
                          "可变的共享状态需要引用类型 `&T`，属后续版本");
                compile_expr(c, e->assign.value);
                emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            if (e->assign.target->kind != EX_IDENT) {
                diag_error(e->pos, "赋值目标必须是变量名或数组下标");
                diag_help("支持 `x = v` 与 `a[i] = v`");
                return TY_UNKNOWN;
            }
            const char* name = e->assign.target->ident.name;
            int slot = local_find(c, name);
            if (slot < 0) {
                diag_error(e->pos, "给未定义的变量 `%s` 赋值", name);
            diag_help("`%s` 在此处不可见。若它声明在内层块或别的函数里，需要重新声明", name);
                compile_expr(c, e->assign.value);
                return TY_UNKNOWN;
            }
            TyKind vt = compile_expr(c, e->assign.value);
            // 赋值是表达式（01 §3.2）：值留在栈上一个副本
            emit(c, BC_DUP, 0, e->pos);
            emit(c, BC_STORE_LOCAL, slot, e->pos);
            local_set_type(c, slot, vt);
            return vt;
        }

        case EX_CALL: return compile_call(c, e);

        case EX_INDEX: {
            // 数组读取。元素类型不做静态区分 —— 数组里可能混装
            // 不同类型（现在允许），所以返回 UNKNOWN，让上层选动态指令。
            // 静态区分元素类型需要泛型，属 P1。
            //
            // 但**被索引的对象必须是数组**这一条在编译期就能查。
            // 之前不查的后果实测过：`let a = 1; println(a[0])` 编译
            // 通过（`x check` 报 ok），直到运行时才报「只能用 [] 索引
            // 数组，实际是 Int」。这条错误本来可以早一步给出，而且
            // 编译期报错能一次列全所有同类问题，运行时只能撞到第一条。
            TyKind ot = compile_expr(c, e->index.obj);
            if (ot != TY_ARRAY && ot != TY_UNKNOWN) {
                diag_error(e->index.obj->pos,
                           "只能用 `[]` 索引数组，实际是 %s", ty_name(ot));
                diag_help("`%s` 不是数组。取字符串里的第 n 个字符请用 "
                          "`strAt(s, n)`；取子串用 `strSub(s, from, to)`。"
                          "两者都按码点索引，不是字节",
                          e->index.obj->kind == EX_IDENT
                              ? e->index.obj->ident.name : "这个表达式");
            }
            TyKind it = compile_expr(c, e->index.index);
            if (it != TY_INT && it != TY_UNKNOWN) {
                diag_error(e->index.index->pos,
                           "数组下标必须是 Int，实际是 %s", ty_name(it));
                diag_help("下标会做整数运算，浮点下标需要显式 `toInt(x)`");
            }
            emit(c, BC_INDEX_GET, 0, e->pos);
            return TY_UNKNOWN;
        }

        case EX_MEMBER: {
            // 成员访问 `obj.field`。
            //
            // ── 关键点：必须知道 obj 的**具体**结构体类型 ──────
            //
            // TY_STRUCT 只说「是个结构体」。不知道是哪个就没法查
            // 字段表，也就无法在编译期确认「Rect 有这个字段」和
            // 「这是 Int」。而这两条恰恰是加结构体的全部价值 ——
            // 放弃它们等于退回「四个 Int 靠位置约定」的老样子。
            //
            // 所以下面分三种情况，且**类型未知不报错、也不猜**：
            // 给 TY_UNKNOWN 让上层走动态路径（运行时会检查）。
            // 这不是放水：编译器推断不出来的情况客观存在（比如
            // 数组元素），硬报错会把这些合法程序拒掉。
            TyKind ot = compile_expr(c, e->member.obj);

            const char* tn = expr_struct_name(c, e->member.obj);

            // 类实例的具体类名与结构体名是两条链，各查各的表。
            //
            // ── 为什么不能复用 tn ──────────────────────────────
            //
            // tn 来自 expr_struct_name，它只认结构体表，对类实例返回
            // NULL。下面的类分支若沿用 tn，`o.inner.v` 这种「类的字段
            // 本身是另一个类」的链式访问就会一路报「无法确定宿主类型」
            // —— 而 inner 明明有静态类型。
            //
            // 症状很反直觉：`o.inner` 单独访问正常（类型来自字段表），
            // 但紧接着 `.v` 就失败，看起来像第二级访问坏了。
            const char* ctn = expr_class_name(c, e->member.obj);

            // ---- 类实例的字段访问（引用语义） ----
            //
            // 与结构体的关键差别：类不需要「字段所在类型的静态名字」
            // 也能访问，因为字段偏移是**编译期常量**（父类在前、
            // 本类在后，见 ClassType 的说明）。但这里仍然要求能追到
            // 静态类型 —— 那是为了做可见性检查与「字段是否存在」的
            // 编译期校验，不是为了算偏移。
            if (ot == TY_CLASS || (ot == TY_UNKNOWN && ctn)) {
                if (!ctn) {
                    diag_error(e->pos, "无法确定 `.%s` 的宿主类型",
                               e->member.name);
                    diag_help("类实例的静态类型要能追到：用类型注解、"
                              "`new 类名(...)` 直接赋值，或从已知返回类型的"
                              "方法/函数取回");
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_UNKNOWN;
                }
                tn = ctn;   // 后面这一段统一用类名
                int ci = bc_find_class(c->bc, tn);
                if (ci < 0) {
                    diag_error(e->pos, "内部错误：类表里找不到 `%s`", tn);
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_UNKNOWN;
                }
                int fi = bc_find_class_field(c->bc, ci, e->member.name);
                if (fi < 0) {
                    ClassType* ct = &c->bc->classes[ci];
                    diag_error(e->pos, "类 `%s` 没有字段 `%s`", tn, e->member.name);
                    char buf[512]; buf[0] = '\0';
                    for (int i = 0; i < ct->field_count; i++) {
                        if (i) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
                        strncat(buf, ct->field_names[i],
                                sizeof(buf) - strlen(buf) - 1);
                    }
                    diag_note("`%s` 的字段是：%s（含继承来的）", tn, buf);
                    diag_help("如果 `%s` 是方法，写成 `obj.%s(...)` "
                              "而不是 `obj.%s`", e->member.name,
                              e->member.name, e->member.name);
                    emit(c, BC_POP, 0, e->pos);
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_UNKNOWN;
                }

                // 可见性检查。
                //
                // ── 判据 ────────────────────────────────────────
                //
                // private：只有声明它的类（含其所有方法）能访问。
                // protected：本类 + 子类。
                // public：谁都能。
                //
                // 「当前在哪个类里」由 c->cur_class 给出（编译方法体时
                // 设置）。不在任何类里（普通函数、main）时按「外部」算。
                {
                    ClassType* ct = &c->bc->classes[ci];
                    Visibility fv = ct->field_vis[fi];
                    if (fv == VIS_PRIVATE || fv == VIS_PROTECTED) {
                        const char* owner = NULL;
                        // 找到声明该字段的类：父类在前，所以字段下标
                        // 小于 own_field_start 时属于祖先
                        int owner_ci = ci;
                        while (owner_ci >= 0) {
                            ClassType* o = &c->bc->classes[owner_ci];
                            if (fi >= o->own_field_start &&
                                fi < o->own_field_start +
                                     (o->field_count - o->own_field_start)) {
                                break;
                            }
                            if (fi < o->own_field_start) {
                                owner_ci = o->parent_idx;
                            } else {
                                break;
                            }
                        }
                        if (owner_ci >= 0) owner = c->bc->classes[owner_ci].name;

                        bool allowed = false;
                        if (c->cur_class && owner) {
                            int cur_ci = bc_find_class(c->bc, c->cur_class);
                            if (fu_ci_eq(cur_ci, owner_ci)) {
                                allowed = true;    // 本类（含子类访问继承的字段）
                            } else if (fv == VIS_PROTECTED) {
                                // 子类可以访问父类的 protected
                                allowed = bc_class_is_a(c->bc, cur_ci, owner_ci);
                            }
                        }
                        if (!allowed) {
                            if (c->cur_class) {
                                diag_error(e->pos,
                                           "字段 `%s` 是 %s，不能在类 `%s` 里访问",
                                           e->member.name,
                                           fv == VIS_PRIVATE ? "private（私有）"
                                                              : "protected（保护）",
                                           c->cur_class);
                            } else {
                                diag_error(e->pos,
                                           "字段 `%s` 是 %s，不能从类外访问",
                                           e->member.name,
                                           fv == VIS_PRIVATE ? "private（私有）"
                                                              : "protected（保护）");
                            }
                            diag_help(fv == VIS_PRIVATE
                                      ? "private 字段只有声明它的类能用。"
                                        "需要外部读写就改成 public，"
                                        "或者加一个公开的读取方法"
                                      : "protected 字段只对子类开放。"
                                        "外部访问请改成 public 或加访问方法");
                        }
                    }
                }

                emit(c, BC_FIELD_GET, ci, e->pos);
                c->bc->code[c->bc->code_count - 1].b = (int16_t)fi;

                const char* ftn = c->bc->classes[ci].field_types[fi];
                TyKind rt = ty_from_name(ftn);
                // 字段若声明为某个类类型，保留具体名字供链式访问
                if (rt == TY_CLASS && ftn) {
                    // 交给上层的 expr_struct_name 去追
                }
                return rt;
            }

            if (ot != TY_STRUCT && ot != TY_UNKNOWN) {
                diag_error(e->member.obj->pos,
                           "只能用 `.` 访问结构体字段，实际是 %s", ty_name(ot));
                diag_help("数组用 `arr[i]`，字符串取字符用 `strAt(s, i)`。"
                          "`.` 只对结构体有效");
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            if (!tn) {
                // ── 宿主恒为 null：结果也是 null，不需要结构体名 ──
                //
                // 这支撑 `p?.a?.v`：内层 `p?.a` 已经是常量折叠出的
                // null（p 静态为 null），它的静态类型就是 TY_NULL。
                // 外层再要求 `.v` 的宿主结构体名是不可能的 —— 那里
                // 根本没有结构体，值永远是 null。
                //
                // 顺序很重要：必须在下面的报错之前判。否则
                // `p?.a?.v` 会报「无法确定 .v 的宿主类型」，而用户
                // 的写法明明是对的，且报错指向可选链的第二段。
                TyKind hk = infer_type(c, e->member.obj);
                if (hk == TY_NULL || ot == TY_NULL) {
                    // compile_expr 已把 null 压栈，弹掉再压 null 保持净零
                    emit(c, BC_POP, 0, e->pos);
                    emit(c, BC_PUSH_NULL, 0, e->pos);
                    return TY_NULL;
                }

                // 类型未知（或没能追到具体名字）。
                //
                // 这里**不能**猜一个类型索引去发射 FIELD_GET —— 猜错
                // 会让运行时读到错误的字段槽，而且完全静默。压 null
                // 让程序在运行时给出能看懂的错误。
                diag_error(e->pos, "无法确定 `.%s` 的宿主类型", e->member.name);
                diag_help("成员访问要求宿主是一个**可静态追踪**的结构体："
                          "用类型注解声明它（`let r: Rect = ...`）或直接用字面量"
                          "（`let r = Rect { ... }`）。数组元素、函数返回的未知类型"
                          "都追不到");
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            int ti = bc_find_struct(c->bc, tn);
            if (ti < 0) {
                diag_error(e->pos, "内部错误：类型表里找不到结构体 `%s`", tn);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }
            int fi = bc_find_field(c->bc, ti, e->member.name);
            if (fi < 0) {
                diag_error(e->pos, "结构体 `%s` 没有字段 `%s`", tn, e->member.name);
                // 列出真实字段名 —— 拼写错误是最常见的成因，
                // 直接列出可选值比让用户回去翻声明快得多。
                StructType* st = &c->bc->structs[ti];
                char buf[512];
                buf[0] = '\0';
                for (int i = 0; i < st->field_count; i++) {
                    if (i) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
                    strncat(buf, st->field_names[i], sizeof(buf) - strlen(buf) - 1);
                }
                diag_note("`%s` 的字段是：%s", tn, buf);
                emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            emit(c, BC_FIELD_GET, ti, e->pos);
            c->bc->code[c->bc->code_count - 1].b = (int16_t)fi;

            const char* ftn = c->bc->structs[ti].field_types[fi];
            return ty_from_name(ftn);
        }

        case EX_CAST:
            diag_error(e->pos, "`as` 转换在 P0 中尚未实现（隐式转换已被 01 §12 禁止，显式转换留 P1）");
            diag_help("P0 请用 `toInt` / `toFloat` / `toString` 三个内置函数做转换");
            compile_expr(c, e->cast.expr);
            return TY_UNKNOWN;

        // ---- new 类名(实参...) ----
        //
        // 三件事按顺序发生在运行时：
        //   1. 按字段总数分配实例，全字段清零
        //   2. 调构造函数（它通过 self.field = ... 填字段）
        //   3. 返回实例
        //
        // ── 为什么构造函数要单独调，而不是把字段初值直接编进来 ──
        //
        // 因为字段初值常常依赖构造参数（`self.w = w * 2`），也可能
        // 调用别的方法算出来。把它们编进「分配」这一步，等于要求
        // 分配指令能执行任意表达式 —— 那就把分配变成了一次函数调用，
        // 不如干脆承认它就是函数调用。
        //
        // 没有显式构造函数的类：分配后字段全零，BC_NEW_CLASS 的
        // ctor 字段是 -1，运行时跳过调用。
        case EX_NEW: {
            const char* tn = e->newx.type_name;
            int ci = bc_find_class(c->bc, tn);
            if (ci < 0) {
                diag_error(e->pos, "未知的类 `%s`", tn);
                if (is_struct_name(tn)) {
                    diag_help("`%s` 是结构体，用字面量构造：`%s { ... }`。"
                              "`new` 只用于 class", tn, tn);
                } else {
                    diag_help("类必须先声明：`class %s { ... }`。"
                              "若它定义在别的文件里，加一行 import", tn);
                }
                for (int i = 0; i < e->newx.argc; i++)
                    compile_expr(c, e->newx.args[i]);
                for (int i = 0; i < e->newx.argc; i++)
                    emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            ClassType* ct = &c->bc->classes[ci];

            // 抽象类不能实例化。
            //
            // 这条是抽象类存在的**全部意义** —— 它把「这个类只是
            // 个骨架，你必须用某个具体子类」从口头约定变成编译错误。
            if (ct->is_abstract && !ct->is_interface) {
                diag_error(e->pos, "类 `%s` 是 abstract 的，不能实例化", tn);
                diag_help("用它的具体子类：`new 某个子类(...)`。"
                          "抽象类只能作为变量/参数的静态类型出现");
                for (int i = 0; i < e->newx.argc; i++)
                    compile_expr(c, e->newx.args[i]);
                for (int i = 0; i < e->newx.argc; i++)
                    emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }
            if (ct->is_interface) {
                diag_error(e->pos, "`%s` 是接口，不能实例化", tn);
                diag_help("接口只描述行为。用 `implements %s` 的具体类来 new", tn);
                for (int i = 0; i < e->newx.argc; i++)
                    compile_expr(c, e->newx.args[i]);
                for (int i = 0; i < e->newx.argc; i++)
                    emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }

            // ---- 找构造函数并检查实参 ----
            //
            // 构造函数的签名在函数表里是 `类名.constructor`。
            // 它的第一个形参是自动插入的 self，所以实参个数比用户
            // 看到的参数多 1。
            char ctor_name[512];
            size_t nl = strlen(tn);
            if (nl + sizeof(".constructor") <= sizeof(ctor_name)) {
                memcpy(ctor_name, tn, nl);
                memcpy(ctor_name + nl, ".constructor", sizeof(".constructor"));
                FuncSig* cs = ft_find(c->fts, ctor_name);
                if (cs) {
                    int expect = cs->param_count - 1;   // 减掉 self
                    if (e->newx.argc != expect) {
                        diag_error(e->pos,
                                   "`new %s(...)` 需要 %d 个参数，实际给了 %d 个",
                                   tn, expect, e->newx.argc);
                        diag_help("检查构造函数 `constructor(...)` 的形参个数");
                    }
                    // 实参类型检查（有注解的形参才查，同函数调用规则）
                    for (int i = 0; i < e->newx.argc && i + 1 < cs->param_count; i++) {
                        const char* want = cs->param_types ? cs->param_types[i + 1] : NULL;
                        if (!want) continue;
                        TyKind wt = ty_from_name(want);
                        TyKind at = infer_type(c, e->newx.args[i]);
                        if (wt == TY_UNKNOWN || at == TY_UNKNOWN) continue;
                        bool num_ok = (wt == TY_INT || wt == TY_FLOAT) &&
                                      (at == TY_INT || at == TY_FLOAT);
                        // 类参数允许传子类实例（多态）
                        bool poly_ok = false;
                        if (wt == TY_CLASS && at == TY_CLASS) {
                            const char* an = expr_class_name(c, e->newx.args[i]);
                            if (an) {
                                int ai = bc_find_class(c->bc, an);
                                int wi = bc_find_class(c->bc, want);
                                poly_ok = bc_class_is_a(c->bc, ai, wi);
                            }
                        }
                        if (wt != at && !num_ok && !poly_ok) {
                            diag_error(e->newx.args[i]->pos,
                                       "构造参数 %d 期望 %s，实际是 %s",
                                       i + 1, want, ty_name(at));
                        }
                    }
                    // 这里**不能**写 `ct->ctor_fn = -1`。
                    //
                    // 曾经在这里把它清成 -1，理由是「由运行时按名字查」。
                    // 解释器确实是这样做的，所以单跑解释档一切正常；
                    // 但 AOT 生成期必须知道要调哪个 xf_N —— 而
                    // ctor_fn 是 build_vtable_funcs 阶段填好的唯一来源。
                    // 被这里覆盖之后，所有类的构造函数都被当成「不存在」，
                    // 症状是 AOT 版程序里对象字段全是零值，而解释版完全
                    // 正确。两档对比才看得出来，单跑任何一档都像另一个
                    // 模块的问题。
                } else if (e->newx.argc > 0) {
                    diag_error(e->pos,
                               "类 `%s` 没有构造函数，但给了 %d 个参数",
                               tn, e->newx.argc);
                    diag_help("给 `%s` 加 `constructor(...) { ... }`，"
                              "或者不带参数 `new %s()`", tn, tn);
                }
            }

            // 依次压构造实参，然后 NEW_CLASS。
            //
            // 参数压在实例**之前**，所以运行时先弹实参、再分配 ——
            // 而构造函数需要拿到 self 才能填字段，那由运行时自己
            // 把新对象当作第 0 个实参传进去。
            for (int i = 0; i < e->newx.argc; i++) {
                compile_expr(c, e->newx.args[i]);
            }
            emit(c, BC_NEW_CLASS, ci, e->pos);
            c->bc->code[c->bc->code_count - 1].b = (int16_t)e->newx.argc;
            return TY_CLASS;
        }

        case EX_STRUCT_LIT: {
            // 结构体字面量 `Rect { x: 1, y: 2 }`。
            //
            // ── 发射顺序 ────────────────────────────────────────
            //
            // 按**声明**顺序把字段值依次压栈（不是按用户书写顺序）。
            // 这样运行时的 BC_NEW_STRUCT 可以只按顺序弹 b 个值，
            // 完全不需要知道字段名 —— 字段名在运行时不参与任何
            // 布局计算，只有诊断信息用。
            //
            // 用户写 `Rect { y: 2, x: 1 }` 也照样正确：编译期会
            // 把 y 的值排到第 1 个、x 排到第 0 个。
            const char* tn = e->slit.type_name;
            int ti = bc_find_struct(c->bc, tn);
            if (ti < 0) {
                diag_error(e->pos, "未知的结构体 `%s`", tn);
                diag_help("结构体必须先声明：`struct %s { ... }`。"
                          "若它定义在别的文件里，加一行 import", tn);
                for (int i = 0; i < e->slit.field_count; i++)
                    compile_expr(c, e->slit.field_values[i]);
                for (int i = 0; i < e->slit.field_count; i++)
                    emit(c, BC_POP, 0, e->pos);
                emit(c, BC_PUSH_NULL, 0, e->pos);
                return TY_UNKNOWN;
            }
            StructType* st = &c->bc->structs[ti];

            // ---- 检查用户写的字段名都是真的，且没有重复 ----
            for (int i = 0; i < e->slit.field_count; i++) {
                if (bc_find_field(c->bc, ti, e->slit.field_names[i]) < 0) {
                    diag_error(e->pos, "结构体 `%s` 没有字段 `%s`",
                               tn, e->slit.field_names[i]);
                    char buf[512];
                    buf[0] = '\0';
                    for (int k = 0; k < st->field_count; k++) {
                        if (k) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
                        strncat(buf, st->field_names[k], sizeof(buf) - strlen(buf) - 1);
                    }
                    diag_note("`%s` 的字段是：%s", tn, buf);
                }
                for (int k = i + 1; k < e->slit.field_count; k++) {
                    if (strcmp(e->slit.field_names[i], e->slit.field_names[k]) == 0) {
                        diag_error(e->pos, "字段 `%s` 在字面量里重复出现",
                                   e->slit.field_names[i]);
                        diag_help("每个字段最多出现一次");
                    }
                }
            }

            // ---- 按声明顺序求值并压栈 ----
            //
            // 对每个声明字段，找用户是否提供了值；没提供就用默认值。
            //
            // ── 为什么要允许省略字段 ──────────────────────────
            //
            // 强制写全所有字段在矩形这种场景下会让代码被无意义的
            // 0 填满：`Rect { x: 0, y: 0, w: 800, h: 600 }` 里前面
            // 两个 0 是噪音。而默认值（数值 0 / 空串 / false / null）
            // 是唯一合理的选择，不是我随便挑的 —— 它是「零值」，
            // 与 C 的静态初始化、Go 的零值、Java 的字段默认值同一个
            // 概念，用户不需要学新东西。
            for (int i = 0; i < st->field_count; i++) {
                Expr* v = NULL;
                for (int k = 0; k < e->slit.field_count; k++) {
                    if (strcmp(e->slit.field_names[k], st->field_names[i]) == 0) {
                        v = e->slit.field_values[k];
                        break;
                    }
                }
                if (!v) {
                    // 零值
                    TyKind ft = ty_from_name(st->field_types[i]);
                    switch (ft) {
                        case TY_INT:    emit(c, BC_PUSH_I64,
                                             bc_add_const_i64(c->bc, 0), e->pos); break;
                        case TY_FLOAT:  emit(c, BC_PUSH_F64,
                                             bc_add_const_f64(c->bc, 0.0), e->pos); break;
                        case TY_BOOL:   emit(c, BC_PUSH_FALSE, 0, e->pos); break;
                        case TY_STRING: emit(c, BC_PUSH_STR,
                                             bc_add_const_str(c->bc, "", 0), e->pos); break;
                        default:        emit(c, BC_PUSH_NULL, 0, e->pos); break;
                    }
                    continue;
                }
                TyKind at = compile_expr(c, v);
                TyKind ft = ty_from_name(st->field_types[i]);
                if (ft != TY_UNKNOWN && at != TY_UNKNOWN && at != ft) {
                    bool numeric_pair =
                        (ft == TY_INT && at == TY_FLOAT) ||
                        (ft == TY_FLOAT && at == TY_INT);
                    if (!numeric_pair) {
                        diag_error(v->pos,
                                   "结构体 `%s` 的字段 `%s` 声明为 %s，实际给了 %s",
                                   tn, st->field_names[i],
                                   st->field_types[i] ? st->field_types[i] : "?",
                                   ty_name(at));
                        diag_help("字段类型是编译期契约。用 `toInt` / `toFloat` / "
                                  "`toString` 显式转换，或改结构体声明里的类型");
                    }
                }
            }

            emit(c, BC_NEW_STRUCT, ti, e->pos);
            c->bc->code[c->bc->code_count - 1].b = (int16_t)st->field_count;
            return TY_STRUCT;
        }

        case EX_RANGE:
            diag_error(e->pos, "区间 `..` 在 P0 中尚未实现（需要 for 循环与迭代器）");
            diag_help("P0 请用 `while` 手写循环；`for x in a..b` 属于 P1");
            emit(c, BC_PUSH_NULL, 0, e->pos);
            return TY_UNKNOWN;
    }

    diag_error(e->pos, "未知的表达式节点（kind=%d）", (int)e->kind);
    emit(c, BC_PUSH_NULL, 0, e->pos);
    return TY_UNKNOWN;
}

// ============================================================
// 语句编译
// ============================================================

static void compile_var_decl(Ctx* c, Stmt* s) {
    const char* kw = s->var.mutable_ ? "var" : "let";
    TyKind annotated = ty_from_name(s->var.type_name);

    if (s->var.init) {
        TyKind vt = compile_expr(c, s->var.init);

        // 有类型注解时，尝试让它成立
        if (annotated != TY_UNKNOWN && annotated != vt) {
            if (annotated == TY_FLOAT && vt == TY_INT) {
                emit(c, BC_I64_TO_F64, 0, s->pos);
                vt = TY_FLOAT;
            } else if (annotated == TY_FLOAT && vt == TY_UNKNOWN) {
                emit(c, BC_TOFLOAT, 0, s->pos);
                vt = TY_FLOAT;
            } else if (annotated == TY_INT && vt == TY_UNKNOWN) {
                emit(c, BC_TOINT, 0, s->pos);
                vt = TY_INT;
            } else if (vt != TY_UNKNOWN) {
                // 01 §12 禁止隐式类型转换，但 P0 的类型推断还不够聪明，
                // 这里只给警告不报错，避免把合理的程序拦下来。
                diag_warn(s->pos, "`%s` 声明为 %s，初始化表达式是 %s",
                          s->var.name, ty_name(annotated), ty_name(vt));
            }
        }
        if (annotated != TY_UNKNOWN) vt = annotated;

        int slot = local_add(c, s->var.name, vt);
        emit(c, BC_STORE_LOCAL, slot, s->pos);

        // 记录结构体 / 类的**具体**类型名。
        //
        // 两个来源，优先级从高到低：
        //   1. 类型注解（`let r: Rect = ...`）—— 用户明确说了，最可信
        //   2. 初始化表达式能追出来的名字（`let r = Rect { ... }`、
        //      `let a = new A(1)`）
        //
        // 为什么必须有这一步：`r.x` / `a.x` 要查字段表，而字段表是按
        // 类型名索引的。这里不记，成员访问就只能报「无法确定宿主类型」。
        if (vt == TY_STRUCT) {
            const char* tn = is_struct_name(s->var.type_name)
                             ? s->var.type_name
                             : expr_struct_name(c, s->var.init);
            if (!tn) {
                // 追不到（比如 `let r = getSomething()`，而 getSomething
                // 没写返回类型注解）。给一条能照着改的提示，而不是在
                // 后面每次 `r.x` 都报一次同样的错。
                diag_warn(s->pos,
                          "`%s` 是结构体，但推断不出是哪个结构体；后续 `.字段` 会报错",
                          s->var.name);
                diag_help("给它加类型注解：`let %s: 结构体名 = ...`", s->var.name);
            } else {
                local_set_type_name(c, slot, tn);
            }
        } else if (vt == TY_CLASS) {
            // 类走同一套追踪，但**不警告**推断不出来。
            //
            // ── 为什么类可以静默放过 ────────────────────────────
            //
            // 结构体字段不可变、大小在编译期固定，所以「不知道是哪个
            // 结构体」意味着代码根本编不下去，值得立刻提醒。
            //
            // 类的成员访问确实也需要静态类型来查字段偏移（所以能追时
            // 一定要追），但它还有一条退路：只要用户写了类型注解，
            // 或者初始值本身带类型（`new A()`），就都能追到。追不到
            // 的典型情形是「从数组元素取回」—— 那种代码在 `.字段`
            // 那一步会报一条准确的错，届时提示「加类型注解」比在这里
            // 提前警告有用得多。
            const char* tn = is_class_name(s->var.type_name)
                             ? s->var.type_name
                             : expr_class_name(c, s->var.init);
            if (tn) local_set_type_name(c, slot, tn);
        }
        return;
    }

    // 无初值：必须能确定类型，否则不知道该压什么零值
    if (annotated == TY_UNKNOWN) {
        if (s->var.type_name) {
            diag_error(s->pos, "`%s` 的类型 `%s` 未知", s->var.name, s->var.type_name);
            diag_help("可用类型：Int / Float / Bool / String / Array / void，"
                      "或已声明的结构体名");
        } else {
            diag_error(s->pos, "`%s %s` 缺少类型注解也缺少初始值，无法推断类型",
                       kw, s->var.name);
            diag_help("写成 `%s %s: Int = 0` 或 `%s %s = 0`", kw, s->var.name, kw, s->var.name);
        }
        int slot = local_add(c, s->var.name, TY_UNKNOWN);
        emit(c, BC_PUSH_NULL, 0, s->pos);
        emit(c, BC_STORE_LOCAL, slot, s->pos);
        return;
    }

    switch (annotated) {
        case TY_FLOAT: {
            int k = bc_add_const_f64(c->bc, 0.0);
            emit(c, BC_PUSH_F64, k, s->pos);
            break;
        }
        case TY_STRING: {
            int k = bc_add_const_str(c->bc, "", 0);
            emit(c, BC_PUSH_STR, k, s->pos);
            break;
        }
        case TY_BOOL:
            emit(c, BC_PUSH_FALSE, 0, s->pos);
            break;
        case TY_NULL:
            emit(c, BC_PUSH_NULL, 0, s->pos);
            break;
        default: {
            int k = bc_add_const_i64(c->bc, 0);
            emit(c, BC_PUSH_I64, k, s->pos);
            break;
        }
    }
    int slot = local_add(c, s->var.name, annotated);
    emit(c, BC_STORE_LOCAL, slot, s->pos);
}

static void compile_stmt(Ctx* c, Stmt* s) {
    if (!s) return;

    switch (s->kind) {
        case ST_LET:
        case ST_VAR:
            compile_var_decl(c, s);
            break;

        case ST_EXPR:
            compile_expr(c, s->expr.expr);
            emit(c, BC_POP, 0, s->pos);   // 表达式语句丢弃值
            break;

        case ST_RETURN:
            if (s->ret.value) {
                TyKind t = compile_expr(c, s->ret.value);
                // 函数返回类型与实参类型不符时提醒（P0 不强制，因为类型推断还不完整）
                if (c->cur_func && c->cur_func->ret_type != TY_UNKNOWN &&
                    t != TY_UNKNOWN && t != c->cur_func->ret_type) {
                    diag_warn(s->pos, "函数 `%s` 声明返回 %s，实际返回 %s",
                              c->cur_func->name,
                              ty_name(c->cur_func->ret_type), ty_name(t));
                }
                emit(c, BC_RETURN, 0, s->pos);
            } else {
                emit(c, BC_RETURN_VOID, 0, s->pos);
            }
            break;

        case ST_IF: {
            compile_expr(c, s->ifs.cond);
            int jf = emit_jump(c, BC_JUMP_IF_FALSE, s->pos);

            compile_stmt(c, s->ifs.then_body);

            if (s->ifs.else_body) {
                int jend = emit_jump(c, BC_JUMP, s->pos);
                patch_here(c, jf);
                compile_stmt(c, s->ifs.else_body);
                patch_here(c, jend);
            } else {
                patch_here(c, jf);
            }
            break;
        }

        case ST_WHILE: {
            // 循环条件的位置 —— continue 要跳回这里
            int cond_pos = c->bc->code_count;

            compile_expr(c, s->whilex.cond);
            int jf = emit_jump(c, BC_JUMP_IF_FALSE, s->pos);

            if (c->loop_depth >= MAX_LOOP_DEPTH) {
                diag_error(s->pos, "循环嵌套超过 %d 层", MAX_LOOP_DEPTH);
                break;
            }
            LoopCtx* lc = &c->loop[c->loop_depth++];
            memset(lc, 0, sizeof(*lc));

            compile_stmt(c, s->whilex.body);

            // 回填 continue → 循环条件处
            for (int i = 0; i < lc->continue_count; i++) {
                patch_to(c, lc->continues[i], cond_pos);
            }
            // 跳回条件。
            //
            // ── 为什么不直接 emit(c, BC_JUMP, cond_pos, ...) ────────
            //
            // 因为跳转操作数存的是「相对本条下一条的偏移」，而这里
            // 手上是**绝对**下标 cond_pos。直接写进去会让跳转落到
            // 「cond_pos 条之后」，症状是 while 循环一次都不执行
            // 或者跑到循环体外面 —— 改成相对偏移后的第一个 bug
            // 就是这个：反汇编里 `JUMP -> 0022` 而目标应该是 0002。
            //
            // 统一走 emit_jump + patch_to，换算只在 patch_to 里做一次。
            {
                int jback = emit_jump(c, BC_JUMP, s->pos);
                patch_to(c, jback, cond_pos);
            }

            // 回填 break → 循环之后
            patch_here(c, jf);
            for (int i = 0; i < lc->break_count; i++) {
                patch_to(c, lc->breaks[i], c->bc->code_count);
            }

            free(lc->breaks);
            free(lc->continues);
            c->loop_depth--;
            break;
        }

        case ST_BREAK: {
            if (c->loop_depth == 0) {
                diag_error(s->pos, "`break` 只能在循环内使用");
            diag_help("`break` 用于跳出 `while` / `loop`。若想提前返回，用 `return`");
                break;
            }
            LoopCtx* lc = &c->loop[c->loop_depth - 1];
            int idx = emit_jump(c, BC_JUMP, s->pos);
            if (lc->break_count == lc->break_cap) {
                lc->break_cap = lc->break_cap ? lc->break_cap * 2 : 4;
                lc->breaks = x_realloc(lc->breaks, (size_t)lc->break_cap * sizeof(int));
            }
            lc->breaks[lc->break_count++] = idx;
            break;
        }

        case ST_CONTINUE: {
            if (c->loop_depth == 0) {
                diag_error(s->pos, "`continue` 只能在循环内使用");
            diag_help("`continue` 用于跳到下一轮循环。它必须直接位于 `while` / `loop` 体内");
                break;
            }
            LoopCtx* lc = &c->loop[c->loop_depth - 1];
            int idx = emit_jump(c, BC_JUMP, s->pos);
            if (lc->continue_count == lc->continue_cap) {
                lc->continue_cap = lc->continue_cap ? lc->continue_cap * 2 : 4;
                lc->continues = x_realloc(lc->continues, (size_t)lc->continue_cap * sizeof(int));
            }
            lc->continues[lc->continue_count++] = idx;
            break;
        }

        case ST_BLOCK: {
            int saved = c->local_count;
            for (int i = 0; i < s->block.count; i++) {
                compile_stmt(c, s->block.stmts[i]);
            }
            c->local_count = saved;
            break;
        }

        case ST_DEFER:
            // defer 需要栈展开支持，与 06 的三档异常展开耦合，P0 不做。
            // 明确警告，不给用户「它生效了」的错觉。
            diag_warn(s->pos, "`defer` 在 P0 中尚未实现，此语句被忽略");
            diag_note("`defer` 依赖栈展开机制，与 06 文档的三档异常展开耦合，属 P2");
            diag_help("P0 请手动在函数返回前写清理代码");
            break;

        case ST_ASSIGN: {
            int slot = local_find(c, s->assign.name);
            if (slot < 0) {
                diag_error(s->pos, "给未定义的变量 `%s` 赋值", s->assign.name);
            diag_help("`%s` 在此处不可见。确认它已在本作用域或外层作用域用 `let`/`var` 声明", s->assign.name);
                break;
            }
            TyKind t = compile_expr(c, s->assign.value);
            emit(c, BC_STORE_LOCAL, slot, s->pos);
            local_set_type(c, slot, t);
            break;
        }
    }
}

// ============================================================
// 入口
// ============================================================

// ============================================================
// 超指令合并（07 §3.3 L3）
// ============================================================
//
// 为什么放在字节码生成之后、而不是在语法树里做：
// 超指令是**纯字节码层的局部改写**，与语义无关。放在这里意味着
// 它不影响类型推断、不影响诊断位置、不影响与 AOT 的语义一致性
// （AOT 读的是同一份字节码，也能识别这些超指令，或先展开再用）。
//
// 选哪几对由实测决定，见 tools/profile_pairs.sh 的统计：
//   LOAD_LOCAL + LOAD_LOCAL   ← 双操作数准备，循环里最高频
//   DUP        + STORE_LOCAL  ← 赋值表达式的标准形态
//   STORE_LOCAL+ POP          ← 赋值语句的收尾
//
// 关键难点：删掉一条指令会让这条之后的指令整体前移，而跳转操作数
// 是「相对本条下一条」的偏移 —— 起点和终点各自挪了不同步数，
// 距离就变了。所以必须做一次完整的下标重映射。
//
// ── 重映射必须在压缩**之前**做 ──────────────────────
//
// 因为算新偏移需要「本条在旧布局的位置」和「目标在旧布局的位置」，
// 而压缩一做完旧位置就丢了。第一版先压缩后重映射，把新下标当旧
// 下标用，症状是 `while i < 3` 一次都不执行。

typedef struct {
    int* map;       // 旧下标 → 新下标（被合并掉的指令映射到合并后的那条）
    int  old_count;
    int  new_count;
} Remap;

// 把旧下标翻译成新下标。落在「被合并的第二个位置」上时，
// 指向合并后的指令（这样跳过来仍会正确执行那两条的合并语义）。
static int remap_pc(Remap* r, int old_pc) {
    if (old_pc < 0 || old_pc >= r->old_count) return old_pc;
    return r->map[old_pc];
}

// 合并规则的匹配与改写。返回 true 表示本位置合并成功
// （p 处的指令被改写，q 处被标记 drop）。
//
// 单独抽成函数是因为合并要**反复跑到收敛**：
//   ADD; DUP_STORE; POP
//   第一遍：DUP_STORE; POP → DUP_STORE_POP  ⇒  ADD; DUP_STORE_POP
//   第二遍：ADD; DUP_STORE_POP → ADD_STORE
// 单遍匹配配不上最后那一对（第一遍时 p 的下一条还是 DUP_STORE），
// 所以必须多轮。好处是规则之间不必手工排优先级 —— 收敛结果就是最优的。
// 这条指令的操作数 `a` 是否是一个「相对本条下一条」的跳转偏移。
//
// ── 为什么必须收敛成一个函数 ────────────────────────────────
//
// 这个判据原本在两处各写了一遍（jtarget 标注、跳转重映射），都是
// 硬编码的 opcode 列表。加 `n = n - 1` 的减法超指令时就栽了：
// 新指令 BC_SUB_CONST_STORE_JUMP 有偏移，但两处列表都没加 ——
//
//   1. jtarget 标注漏掉它 → 合并 pass 不知道「这条跳转指向别处」，
//      于是把它的目标指令吞进上一对合并里，指令凭空消失；
//   2. 重映射漏掉它 → 指令压缩后偏移不更新，跳错位置。
//
// 症状（实测）：`while i > 0 { ... i = i - 1 }` 第一次迭代就报
// 「值栈下溢」，而单独写 `n = n - 1` 完全正常 —— 因为只有回边
// 形态才会生成带偏移的那条。
//
// 这类 bug 的特征是「加一条指令要改 N 个地方，漏一个就静默出错」，
// 所以判据必须只有一个定义。新增跳转类 opcode 时只改这里。
static bool op_has_jump_offset(OpCode op) {
    switch (op) {
        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_LT_JUMP_FALSE:
        case BC_LT_CONST_JUMP_FALSE:
        case BC_ADD_CONST_STORE_JUMP:
        case BC_SUB_CONST_STORE_JUMP:
        case BC_NE_CONST_JUMP_FALSE:
        case BC_NE_JUMP_FALSE:
            return true;
        default:
            return false;
    }
}

// 整数运算指令 → 运算码。查不到返回 false。
//
// 这是「运算族泛化」的唯一映射表。把它单独拎出来而不是内联进
// try_merge_pair 的 switch，是因为它同时被两处需要（STORE 形态、
// CONST_STORE 形态），分离后新增运算只改这一个函数。
//
// 只收 _I64 特化指令：_F64 / _NUM / _DYN 的语义（浮点提升、字符串
// 拼接）与整数运算不同，混进同一张表会让超指令的静态类型保证失效。
static bool arith_op_of(OpCode op, XArithOp* out) {
    switch (op) {
        case BC_ADD_I64:  *out = XOP_ADD;  return true;
        case BC_SUB_I64:  *out = XOP_SUB;  return true;
        case BC_MUL_I64:  *out = XOP_MUL;  return true;
        case BC_DIV_I64:  *out = XOP_DIV;  return true;
        case BC_MOD_I64:  *out = XOP_MOD;  return true;
        case BC_BAND_I64: *out = XOP_BAND; return true;
        case BC_BOR_I64:  *out = XOP_BOR;  return true;
        case BC_BXOR_I64: *out = XOP_BXOR; return true;
        case BC_SHL_I64:  *out = XOP_SHL;  return true;
        case BC_SHR_I64:  *out = XOP_SHR;  return true;
        default: return false;
    }
}

// A/B 隔离测量的开关：X_NO_NEW_MERGE=1 时跳过第五批（v0.22）的超指令。
//
// ── 为什么需要它 ────────────────────────────────────────────
//
// 判断「某一轮优化值多少」不能用两个 git 版本对比 —— 那样测到的是
// **多轮累计**。实测过这个坑：第三轮改完后对比 git HEAD，10 亿次
// 循环从 13.2s 降到 7.4s，看起来第三轮值 5.8 秒；但那 13.2s 的版本
// 里连第一轮的 SUB 超指令都还没有，差值是三轮的合计。
//
// 正确做法是同一份二进制、只切换一个开关，这样编译器其余部分
// （寄存器分配、代码布局、甚至分支预测器的状态）完全一致。
static bool new_merge_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("X_NO_NEW_MERGE");
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached != 0;
}

static bool try_merge_pair(Bytecode* bc, int p, int q) {
    OpCode a = (OpCode)bc->code[p].op;
    OpCode b = (OpCode)bc->code[q].op;
    bool no_new = new_merge_disabled();

    if (a == BC_LOAD_LOCAL && b == BC_LOAD_LOCAL) {
        // LOAD a; LOAD b → LOAD_LOAD a,b
        bc->code[p].op = (uint8_t)BC_LOAD_LOAD;
        bc->code[p].b  = bc->code[q].a;
        return true;
    }
    if (!no_new && a == BC_LOAD_LOCAL && b == BC_STORE_LOCAL) {
        // LOAD_LOCAL a; STORE_LOCAL b → MOV_LOCAL a, b
        //
        // `let y = x` / `y = x` 这种纯拷贝。实测在真实混合负载里是
        // 指令对第一名（315 万次），却因为「两条指令看起来都很便宜」
        // 而一直没人合并它。实际上它值一次分派 + 一次值栈往返：
        // 拆开执行要先 retain 压栈，再弹栈把所有权转给目标槽位。
        //
        // ── 为什么不能简化成「直接 memcpy 槽位」──────────────
        //
        // 中间那次压栈不是白做的：它让两边**各自持有一份引用**。
        // 合并后仍必须 retain 一个新引用，否则 y 和 x 共享一个未
        // 计数的指针，x 被覆盖时就把 y 还在用的对象释放了。
        // 这正是 BC_MOV_LOCAL 的 interpreter 实现里那次 value_retain
        // 存在的原因 —— 不能因为「只是拷贝」就省掉。
        //
        // 槽位相同时（a == b）也照收：等价于自我赋值，语义无害，
        // 而且 retain/release 配对正好抵消。特意不为它加特判 ——
        // 多一条分支就多一处可能写错的地方。
        bc->code[p].op = (uint8_t)BC_MOV_LOCAL;
        bc->code[p].b  = bc->code[q].a;
        return true;
    }
    if (a == BC_DUP && b == BC_STORE_LOCAL) {
        // DUP; STORE a → DUP_STORE a
        bc->code[p].op = (uint8_t)BC_DUP_STORE;
        bc->code[p].a  = bc->code[q].a;
        return true;
    }
    if (a == BC_STORE_LOCAL && b == BC_POP) {
        // STORE a; POP → STORE_POP a
        bc->code[p].op = (uint8_t)BC_STORE_POP;
        return true;
    }
    if (a == BC_LOAD_LOCAL && b == BC_PUSH_I64) {
        // LOAD_LOCAL a; PUSH_I64 c → LOAD_CONST a, c
        // 循环条件的前半段：把局部变量和一个常量压栈准备比较。
        bc->code[p].op = (uint8_t)BC_LOAD_CONST;
        bc->code[p].b  = bc->code[q].a;   // b 存常量索引
        return true;
    }
    if (a == BC_LT_I64 && b == BC_JUMP_IF_FALSE) {
        // LT_I64; JUMP_IF_FALSE t → LT_JUMP_FALSE t
        // 这是 while i < n 的标准形态，循环头最高频的一对。
        // 注意跳转目标仍存在 a 字段，超指令执行完直接改 pc。
        //
        // ── 为什么这里要 +(q - p) ──────────────────────────
        //
        // 操作数是「相对本条**下一条**」的偏移。指令从 q 搬到 p
        // （p < q）之后基准从 q+1 变成 p+1，偏移必须加 (q-p) 才能
        // 指到同一个目标。
        //
        // 漏掉这一项的症状实测过：`while i < 3` 跑起来是死循环 ——
        // 因为 JUMP_IF_FALSE 假分支的目标少跳了一条，落在循环末尾
        // 那条 JUMP 上，于是「条件为假」变成了「再跳回循环头」。
        bc->code[p].op = (uint8_t)BC_LT_JUMP_FALSE;
        bc->code[p].a  = (int16_t)(bc->code[q].a + (q - p));
        return true;
    }
    if (a == BC_DUP_STORE && b == BC_POP) {
        // DUP_STORE a; POP → DUP_STORE_POP a
        //
        // 语义上这一条**等价于 STORE_LOCAL a**：DUP 复制栈顶、
        // STORE 存一份到槽位、POP 弹掉 —— 净效果就是「存一份并弹掉」。
        // 之所以不直接改写回 STORE_LOCAL，是因为 STORE_LOCAL 的
        // 解释器实现要先 value_release 旧值再 copy，而 DUP_STORE_POP
        // 能省掉那次 release（值是同类型同位宽时直接覆盖）。
        //
        // 实测这是循环体里的绝对第一名：3000 万次循环里出现 6000 万次。
        bc->code[p].op = (uint8_t)BC_DUP_STORE_POP;
        return true;
    }
    if (a == BC_ADD_I64 && b == BC_DUP_STORE_POP) {
        // ADD; DUP_STORE_POP a → ADD_STORE a
        bc->code[p].op = (uint8_t)BC_ADD_STORE;
        bc->code[p].a  = bc->code[q].a;
        return true;
    }
    if (a == BC_MOD_I64 && b == BC_DUP_STORE_POP) {
        // MOD; DUP_STORE_POP a → MOD_STORE a
        bc->code[p].op = (uint8_t)BC_MOD_STORE;
        bc->code[p].a  = bc->code[q].a;
        return true;
    }

    // ── 减法家族（v0.21）────────────────────────────────────
    //
    // 与下面 ADD 组的三条规则逐项对称。补这一组的理由见 x.h 里
    // BC_SUB_STORE 的注释：漏掉它会让 `n = n - 1` 这种最常见的
    // 计数循环多执行一倍指令，而表面现象只是「慢」，看不出原因。
    if (a == BC_SUB_I64 && b == BC_DUP_STORE_POP) {
        // SUB; DUP_STORE_POP a → SUB_STORE a
        bc->code[p].op = (uint8_t)BC_SUB_STORE;
        bc->code[p].a  = bc->code[q].a;
        return true;
    }

    // ── 第四批：运算族泛化（v0.21）──────────────────────────
    //
    // 上面的写法是「一种运算一条规则」。补到减法时还剩七个同构
    // 运算没覆盖（MUL / DIV / BAND / BOR / BXOR / SHL / SHR），
    // 一条条写就是 7 × 2 条规则 + 7 条新 opcode。
    //
    // 这里换成一张表查：整数运算指令 → 运算码。表驱动的直接好处
    // 是**新增运算时只改表**，不再需要在 switch 里补 case，也不
    // 会像上次那样「补了 SUB 忘了 DIV」。
    //
    // 注意：ADD / SUB / MOD 已经在上面的专用规则里处理掉了，走到
    // 这里时它们的 DUP_STORE_POP 形态已被吃掉。表里仍保留它们，
    // 是为了让表本身完整 —— 万一将来专用规则被删掉，这里能兜住，
    // 而不是静默退回 3 条指令的慢路径。
    {
        XArithOp aop;
        if (arith_op_of((OpCode)a, &aop)) {
            if (b == BC_DUP_STORE_POP) {
                // <OP>_I64; DUP_STORE_POP s → ARITH_STORE s, op
                bc->code[p].op = (uint8_t)BC_ARITH_STORE;
                bc->code[p].a  = bc->code[q].a;
                bc->code[p].b  = (int16_t)aop;
                return true;
            }
        }
    }

    // ── 第二批：把 `for i in a..b` 的循环体从 7 条压到 3 条 ──
    //
    // 三条规则之间**有先后**：先有 LOAD_CONST + ADD_STORE → ADD_CONST_STORE，
    // 才可能配对 ADD_CONST_STORE + JUMP。但不需要手工排优先级 ——
    // 外层本来就是「扫到不再变化为止」，第 2 轮自然配上。
    //
    // 常量必须是 CONST_I64：这三条都按整数语义实现（溢出检查、
    // 整数比较、错误文案）。若将来浮点也参与，要另开一组指令，
    // 不能靠运行时判类型 —— 那就把静态类型的红利丢掉了。
    if (a == BC_LOAD_CONST && b == BC_LT_JUMP_FALSE &&
        bc->consts[bc->code[p].b].kind == CONST_I64) {
        // LOAD_CONST s,k; LT_JUMP_FALSE t → LT_CONST_JUMP_FALSE t, s, k
        // 偏移放 a，与其它跳转指令保持同一约定（见 x.h 的说明）。
        int16_t slot = bc->code[p].a;
        int16_t kidx = bc->code[p].b;
        int16_t off  = (int16_t)(bc->code[q].a + (q - p));
        bc->code[p].op = (uint8_t)BC_LT_CONST_JUMP_FALSE;
        bc->code[p].a  = off;
        bc->code[p].b  = slot;
        bc->code[p].c  = kidx;
        return true;
    }
    if (a == BC_LOAD_CONST && b == BC_ADD_STORE &&
        bc->code[p].a == bc->code[q].a &&
        bc->consts[bc->code[p].b].kind == CONST_I64) {
        // LOAD_CONST s,k; ADD_STORE s → ADD_CONST_STORE s, k
        // 槽位与常量索引本来就在 a/b 上，改个 op 即可。
        bc->code[p].op = (uint8_t)BC_ADD_CONST_STORE;
        return true;
    }

    // 泛化版：LOAD_CONST s,k; ARITH_STORE s,op → ARITH_CONST_STORE s,k,op
    //
    // 这一条覆盖 `s = s * 2` / `b = b & 7` / `b = b << 1` 这类
    // 「槽位与常量做运算再存回」的形态。实测指令对直方图里
    // `LOAD_CONST → MUL_I64` 这类对每个 300 万次 —— 与 ADD 完全
    // 同级的高频，却因为没有规则而停在 3 条指令。
    //
    // 前提与 ADD 组一致：常量必须是 CONST_I64。浮点常量要走浮点
    // 指令，混进来就会把「静态类型的零检查」丢掉。
    if (a == BC_LOAD_CONST && b == BC_ARITH_STORE &&
        bc->code[p].a == bc->code[q].a &&
        bc->consts[bc->code[p].b].kind == CONST_I64) {
        bc->code[p].op = (uint8_t)BC_ARITH_CONST_STORE;
        bc->code[p].c  = bc->code[q].b;   // 运算码从 q 的 b 搬到 p 的 c
        return true;
    }
    if (a == BC_ADD_CONST_STORE && b == BC_JUMP) {
        // ADD_CONST_STORE s,k; JUMP t → ADD_CONST_STORE_JUMP t, s, k
        int16_t slot = bc->code[p].a;
        int16_t kidx = bc->code[p].b;
        bc->code[p].op = (uint8_t)BC_ADD_CONST_STORE_JUMP;
        bc->code[p].a  = (int16_t)(bc->code[q].a + (q - p));
        bc->code[p].b  = slot;
        bc->code[p].c  = kidx;
        return true;
    }

    // 减法组的两条，与上面 ADD 组逐项对称。
    //
    // 常量必须是 CONST_I64，理由与 ADD 组相同：这三条都按整数语义
    // 实现（overflow 检查、整数比较、`-` 的错误文案）。浮点要走
    // 浮点指令，不能靠运行时判类型 —— 那会把静态类型的红利丢掉。
    if (a == BC_LOAD_CONST && b == BC_SUB_STORE &&
        bc->code[p].a == bc->code[q].a &&
        bc->consts[bc->code[p].b].kind == CONST_I64) {
        // LOAD_CONST s,k; SUB_STORE s → SUB_CONST_STORE s, k
        bc->code[p].op = (uint8_t)BC_SUB_CONST_STORE;
        return true;
    }
    if (a == BC_SUB_CONST_STORE && b == BC_JUMP) {
        // SUB_CONST_STORE s,k; JUMP t → SUB_CONST_STORE_JUMP t, s, k
        int16_t slot = bc->code[p].a;
        int16_t kidx = bc->code[p].b;
        bc->code[p].op = (uint8_t)BC_SUB_CONST_STORE_JUMP;
        bc->code[p].a  = (int16_t)(bc->code[q].a + (q - p));
        bc->code[p].b  = slot;
        bc->code[p].c  = kidx;
        return true;
    }
    return false;
}

// 三合一的合并：`LOAD_CONST k; NE; JUMP_IF_FALSE t` → NE_CONST_JUMP_FALSE。
// ── 为什么单独开一个函数，而不走「两两合并 + 收敛」──────────
//
// 已有的三合一（ADD; DUP_STORE; POP → ADD_STORE）靠的是收敛：
// 前两条先合成 DUP_STORE_POP，再与第一条合成 ADD_STORE。那条路径
// 成立的前提是**中间形态本身有意义** —— DUP_STORE_POP 单独存在时
// 语义完整、可执行、可被别处作为合并输入。
//
// 这里不行。`NE; JUMP_IF_FALSE` 若是造一条 NE_JUMP_FALSE 出来，
// 它是纯过渡形态：既不能省掉中间那个 Bool 的压栈弹栈（NE 已经把
// 两个操作数弹了，结果必须落在栈上再被 JUMP_IF_FALSE 弹掉），
// 又多一条 opcode 要同步 6 个文件。收益为零，成本是真实的维护面。
//
// 所以这里一次性看三条，直接合成终态 —— 中间值从头到尾不上栈，
// 这才是收益来源：省 2 次分派 + 2 次 16 字节写入 + 2 次 sp 调整。
//
// 返回 true 时表示 p 被改写、q 与 r 都应标 drop。
static bool try_merge_triple(Bytecode* bc, int p, int q, int r) {
    OpCode a = (OpCode)bc->code[p].op;
    OpCode b = (OpCode)bc->code[q].op;
    OpCode c = (OpCode)bc->code[r].op;

    // LOAD_CONST s,k; NE; JUMP_IF_FALSE t
    //   → NE_CONST_JUMP_FALSE t, s, k
    //
    // 这是 `while a != 0` / `while name != "done"` 的标准形态。
    // 实测在真实混合负载里排第二（255 万次），仅次于变量拷贝。
    //
    // ── 常量类型这里**不限** CONST_I64 ──────────────────────
    //
    // 与 LT 那组不同：`<` 只对整数有定义（浮点比较是另一条指令），
    // 所以 LT_CONST_JUMP_FALSE 可以假定整数。而 `!=` 是通用相等，
    // 字符串常量、浮点常量都是合法的右操作数 —— 限死 CONST_I64
    // 会把 `while cmd != "quit"` 漏掉，而那正是最常见的写法。
    //
    // 代价是运行时要多判一次常量种类。但这一步本来就要判
    // （value_equal 需要构造出对应类型的值），不是额外开销。
    if (!new_merge_disabled() && a == BC_LOAD_CONST && b == BC_NE && c == BC_JUMP_IF_FALSE) {
        int16_t slot = bc->code[p].a;
        int16_t kidx = bc->code[p].b;
        // 偏移基准从 r+1 搬到 p+1，加 (r - p)。与两两合并的
        // (q - p) 同理 —— 漏掉这一项就是死循环或跳过循环。
        int16_t off  = (int16_t)(bc->code[r].a + (r - p));
        bc->code[p].op = (uint8_t)BC_NE_CONST_JUMP_FALSE;
        bc->code[p].a  = off;
        bc->code[p].b  = slot;
        bc->code[p].c  = kidx;
        return true;
    }

    // LOAD_LOAD s1,s2; NE; JUMP_IF_FALSE t
    //   → NE_JUMP_FALSE t, s1, s2
    //
    // `while i != n` 这类两边都是变量的比较。注意第一条是 LOAD_LOAD
    // 而不是两条 LOAD_LOCAL —— 前者已被更早的规则合并掉了，所以
    // 这里看到的已经是两槽位压栈的形态。
    //
    // ── 为什么这个形态必须补上 ────────────────────────────────
    //
    // 只做常量形态时，我以为收益已经拿到了。实测立刻否掉了这个想法：
    // `while i != n`（n 是变量）走的是本形态，而它是遍历类循环的
    // 标准写法；只有 `while i != 30000000`（常量）才命中常量版本。
    //
    // 换句话说，手写计数循环偏爱 `!= 0`（常量形态），而遍历数据
    // 结构时上界往往本身是个变量（本形态）。两种都常见，只做一半
    // 等于把一半的循环留在 4 条指令。
    if (!new_merge_disabled() && a == BC_LOAD_LOAD && b == BC_NE &&
        c == BC_JUMP_IF_FALSE) {
        int16_t s1 = bc->code[p].a;
        int16_t s2 = bc->code[p].b;
        int16_t off = (int16_t)(bc->code[r].a + (r - p));
        bc->code[p].op = (uint8_t)BC_NE_JUMP_FALSE;
        bc->code[p].a  = off;
        bc->code[p].b  = s1;
        bc->code[p].c  = s2;
        return true;
    }

    // LOAD_LOAD s1,s2; LT_I64; JUMP_IF_FALSE t
    //
    // ── 这里刻意**不做**合并，与上面的 NE 版本不同 ──────────────
    //
    // BC_LT_JUMP_FALSE 已经被两两规则用来承载「LT_I64; JUMP_IF_FALSE」
    // 这条纯栈形态：操作数从栈顶取，指令本身不需要知道值来自哪个槽位，
    // 因此只有 a（跳转偏移）一个操作数，b/c 是 bc_emit 留下的 0。
    //
    // 若把槽位形态也塞进同一个 opcode，解释器就无法区分两者 —— 「b
    // 是否为 -1」不能当标记，因为 bc_emit 把不识别的操作数一律清 0，
    // 而槽位 0 是完全合法的操作数。更糟的是磁盘上的老 .xbc 里
    // LT_JUMP_FALSE 的 b 就是 0，会被误判成「从槽位 0/0 取两个值比较」，
    // 静默算错。这类兼容性坑不值得为一条指令去趟。
    //
    // 真正的需求（让这种循环能被融合执行）由 interp.c 的 lf_translate
    // 直接识别「LOAD_LOAD + LT_JUMP_FALSE」这个循环头来满足 —— 它在
    // 解释器内部做，不改变字节码格式，也就没有兼容性问题。
    return false;
}

// 对 [start, start+len) 这段函数体做超指令合并。
// 返回合并后节省的指令数。
static int superinstr_merge(Bytecode* bc, int start, int len) {
    if (len < 2) return 0;

    bool* drop = x_alloc((size_t)len * sizeof(bool));
    memset(drop, 0, (size_t)len * sizeof(bool));

    // ── 谁被跳进来过 ─────────────────────────────────────────
    //
    // 合并 p、q 的语义是「执行 p 的全部 + q 的全部」。如果有别处的
    // 跳转落在 q 上，那段控制流原本只做 q 那一半，合并后就会多出 p
    // 那一半 —— 结果悄悄错掉。
    //
    // 这不是理论风险：`for`/`while` 里 `continue` 跳到循环底那条
    // JUMP 是常态，而「i = i + 1 后面紧跟回边 JUMP」又正是最想合并
    // 的形态。实测 25-stdlib-str 就栽在这：if 分支的 JUMP 目标正好
    // 是回边，合并后 if 分支多执行了一次 i += 1，`strReplaceN`
    // 少吐字符。
    //
    // 所以先把「被指向的位置」标出来，配对时只要 q 在集合里就放弃
    // 这一对。p 是目标没关系：目标落在合并体的开头，语义正好完整。
    bool* jtarget = x_alloc((size_t)len * sizeof(bool));
    memset(jtarget, 0, (size_t)len * sizeof(bool));
    for (int i = 0; i < len; i++) {
        if (!op_has_jump_offset((OpCode)bc->code[start + i].op)) continue;
        int t = i + 1 + bc->code[start + i].a;
        if (t >= 0 && t < len) jtarget[t] = true;
    }

    // 反复扫到不再变化。每一轮可能生成新的超指令，成为下一轮的输入。
    // 轮数上界取 len（最坏情况每轮只合并一对），实际 3~4 轮就收敛。
    for (int round = 0; round < len; round++) {
        bool changed = false;
        for (int i = 0; i + 1 < len; i++) {
            if (drop[i]) continue;
            // 找到下一个未被合并的位置作为配对对象
            int j = i + 1;
            while (j < len && drop[j]) j++;
            if (j >= len) break;

            // 别处会跳到 q 上：这一对不能合并（原因见上面的 jtarget）
            if (jtarget[j]) continue;

            if (try_merge_pair(bc, start + i, start + j)) {
                drop[j] = true;
                changed = true;
                i = j;      // 跳过已吃掉的 j，避免重叠合并
                continue;
            }

            // ── 两两配不上，再试三合一 ──────────────────────
            //
            // 顺序刻意是「先两两、后三合一」：三合一的形态
            // (LOAD_CONST k, NE, JUMP_IF_FALSE t) 里前两条配不上任何
            // 两两规则，所以先试两两不会有损失，反而保住了
            // 「两两能解决就别吞三条」的保守性 —— 三合一吃掉的
            // 指令更多，一旦判据写松（比如漏查 jtarget[r]），
            // 出错时丢的信息也更多。
            int k2 = j + 1;
            while (k2 < len && drop[k2]) k2++;
            if (k2 >= len) continue;
            // 中间态和末条都不能是别处的跳转目标。检查 r 尤其重要：
            // JUMP_IF_FALSE 正是循环回边最常指向的地方，漏检会让
            // `continue` 直接落进比较指令里。
            if (jtarget[j] || jtarget[k2]) continue;

            if (try_merge_triple(bc, start + i, start + j, start + k2)) {
                drop[j]  = true;
                drop[k2] = true;
                changed = true;
                i = k2;
                continue;
            }
        }
        if (!changed) break;
    }

    // 计算新下标映射
    Remap r;
    r.old_count = len;
    r.map = x_alloc((size_t)len * sizeof(int));
    int n = 0;
    int last_kept = 0;
#ifdef X_DEBUG
    if (getenv("X_DEBUG_JUMP")) {
        fprintf(stderr, "[merge] start=%d len=%d drop: ", start, len);
        for (int i = 0; i < len; i++) if (drop[i]) fprintf(stderr, "%d ", i);
        fprintf(stderr, "\n");
    }
#endif
    for (int i = 0; i < len; i++) {
        if (drop[i]) {
            // 被合并掉的位置 → 指向前一条（合并后的）指令
            r.map[i] = last_kept;
        } else {
            r.map[i] = n;
            last_kept = n;
            n++;
        }
    }
    r.new_count = n;

    // ── 顺序很关键：先把跳转操作数改对，再压缩 ──────────────
    //
    // 因为操作数是「相对本条下一条的偏移」，改它需要知道
    // 「本条在旧布局里的下标」和「目标在旧布局里的下标」。
    // 一旦压缩做完，旧下标就丢了（drop[] 还在，但本条已经搬到
    // 新位置，无法反推它原来在哪）。
    //
    // ── 第一版就栽在顺序上 ────────────────────────────
    //
    // 先压缩再重映射时，代码用 `i - start` 当「旧下标」—— 那其实是
    // **新**下标。于是偏移算成 new_target - (new_self + 1)，而两个
    // 量一个来自旧表、一个来自新表，混着用。症状：`while i < 3`
    // 一次都不执行（JUMP 跳到了循环体后面），而阶乘函数返回 1。
    for (int i = start; i < start + len; i++) {
        if (drop[i - start]) continue;
        if (!op_has_jump_offset((OpCode)bc->code[i].op)) continue;
        // 旧布局下标 → 新布局下标。两者都是「段内相对」。
        int old_self_rel   = i - start;
        int old_target_rel = old_self_rel + 1 + bc->code[i].a;
        int new_self_rel   = remap_pc(&r, old_self_rel);
        int new_target_rel = remap_pc(&r, old_target_rel);
        bc->code[i].a = (int16_t)(new_target_rel - (new_self_rel + 1));
#ifdef X_DEBUG
        if (getenv("X_DEBUG_JUMP")) {
            fprintf(stderr, "[jump] at=%d old_rel=%d new_rel=%d (n=%d)\n",
                    i, old_target_rel, new_target_rel, n);
        }
#endif
    }

    // 就地压缩（整段搬移，不重新分配）
    int w = start;
    for (int i = 0; i < len; i++) {
        if (drop[i]) continue;
        bc->code[w] = bc->code[start + i];
        bc->pos[w]  = bc->pos[start + i];
        w++;
    }

    // 就地压缩 w 之后剩余空间的起点（本段压缩结束的位置）
    int seg_end = w;

    // 关键：本段压缩后，**后面所有函数的数据必须整体前移 saved 条**。
    //
    // 漏掉这一步的后果实测过：f 函数压缩掉 1 条后，main 的内容还留在
    // 原来的地址上，而 main->code_start 已经减了 1，于是 main 的第一条
    // 指令指向了 f 的最后一条 RETURN——表现为「函数一进去就返回、
    // 值栈下溢」。这类 bug 只在程序有多个函数时出现，单函数测试全绿。
    int saved = len - n;
    if (saved > 0) {
        int tail_start = start + len;              // 原本段之后的位置
        int tail_count = bc->code_count - tail_start;
        if (tail_count > 0) {
            memmove(&bc->code[seg_end], &bc->code[tail_start],
                    (size_t)tail_count * sizeof(Insn));
            memmove(&bc->pos[seg_end], &bc->pos[tail_start],
                    (size_t)tail_count * sizeof(SrcPos));

            // ── 这里**不需要**再调整后面函数的跳转操作数 ────────────
            //
            // 这是改成「函数内相对偏移」之后省掉的一整块逻辑。
            //
            // 以前操作数存绝对下标时，本段压缩让后面所有函数整体前移
            // saved 条，于是后面那些函数里的跳转目标也必须一起减 ——
            // 而这个调整曾经漏写过一次，症状是 08-zh.xlc 的 factorial
            // 阶乘算错（跳转跳过一条指令，n 的第二个操作数没压栈）。
            //
            // 现在跳转操作数是「相对本条的下一条」的函数内偏移：函数
            // 整体搬家时，函数里每条指令和它的目标一起搬，相对距离
            // 完全不变。这个不变量就是改用相对偏移换来的最大好处。
        }
        bc->code_count -= saved;
    }

    free(drop);
    free(jtarget);
    free(r.map);
    return saved;
}

// ============================================================
// 类表构建（v0.15）
// ============================================================
//
// 这一步必须在编译任何函数体之前完成，理由与函数签名表相同：
// 类体里 `self.field` 需要字段偏移，`self.method()` 需要虚表槽号，
// 而这些都属于「类型信息」，编译函数体时只应该**查**，不应该还在
// 构建。
//
// 分四个阶段，顺序不可交换：
//   1. 登记所有类名（只建空壳）—— 这样 A 引用后文才定义的 B 也能解析
//   2. 接父链 + 检测环
//   3. 展开字段（父类字段在前）+ 检查重名
//   4. 分配虚表（父表抄一份，覆写的槽替换）
//
// 为什么字段展开必须在虚表之前：字段偏移是对象布局的一部分，
// 而虚表槽号是调用约定的一部分，两者无关 —— 但第四步要检查
// 「覆写的方法签名是否与父类一致」，那需要第 2 步的父链已就绪。
// 顺序按依赖排，不按「看起来相关」排。

// 类的字段布局：父类字段在前，本类新增在后。
static void build_class_fields(Bytecode* bc, int ci, ClassDecl* d) {
    ClassType* ct = &bc->classes[ci];

    int parent_fields = 0;
    if (ct->parent_idx >= 0) {
        parent_fields = bc->classes[ct->parent_idx].field_count;
    }

    int total = parent_fields + d->field_count;
    ct->field_names = total ? x_alloc((size_t)total * sizeof(char*)) : NULL;
    ct->field_types = total ? x_alloc((size_t)total * sizeof(char*)) : NULL;
    ct->field_vis   = total ? x_alloc((size_t)total * sizeof(Visibility)) : NULL;
    ct->field_final = total ? x_alloc((size_t)total * sizeof(bool)) : NULL;
    ct->own_field_start = parent_fields;
    ct->field_count     = total;

    // 先拷父类字段（顺序与偏移必须完全一致）
    if (ct->parent_idx >= 0) {
        ClassType* pt = &bc->classes[ct->parent_idx];
        for (int i = 0; i < parent_fields; i++) {
            ct->field_names[i] = x_strdup(pt->field_names[i]);
            ct->field_types[i] = pt->field_types[i] ? x_strdup(pt->field_types[i]) : NULL;
            ct->field_vis[i]   = pt->field_vis[i];
            ct->field_final[i] = pt->field_final[i];
        }
    }

    // 再追加本类字段
    for (int i = 0; i < d->field_count; i++) {
        ClassField* cf = &d->fields[i];
        // 重名检查（含父类字段）。
        //
        // 为什么不许「子类字段遮蔽父类字段」：那会让 `self.x` 在不同
        // 静态类型下指向不同的槽，而虚方法里的 self 是父类类型编译的
        // —— 同一个表达式两种含义，是数据结构里最难查的一类 bug。
        // C++ 允许遮蔽，Java 不允许；这里跟 Java。
        for (int k = 0; k < parent_fields + i; k++) {
            if (ct->field_names[k] && strcmp(ct->field_names[k], cf->name) == 0) {
                diag_error(cf->pos, "字段 `%s` 与%s的字段重名",
                           cf->name, k < parent_fields ? "父类" : "本类");
                diag_help("类字段名必须唯一（含继承来的）。"
                          "子类不能用一个同名字段遮蔽父类字段 —— "
                          "那会让 `self.%s` 在不同静态类型下指向不同的槽",
                          cf->name);
                break;
            }
        }
        int slot = parent_fields + i;
        ct->field_names[slot] = x_strdup(cf->name);
        ct->field_types[slot] = cf->type_name ? x_strdup(cf->type_name) : NULL;
        ct->field_vis[slot]   = cf->vis;
        ct->field_final[slot] = cf->is_final;
    }
}

// 虚表构建。
//
// 规则只有两条，但都要精确：
//   1. 先抄父类虚表（含祖父类已解析好的结果）—— 这一步让
//      「不覆写就继承父类实现」自动成立，不需要任何额外代码
//   2. 把本类每个实例方法写进它的名字槽 —— 覆写就是把父类
//      同一个槽的值替换掉
//
// 接口方法在这里**不做**任何事：接口只是编译期的契约检查，
// 运行时它没有独立实现。一个类「实现接口」体现为它的虚表里
// 那些槽不是 -1，而这在写入自己的方法时自然就成立了。
static void build_class_vtable(Bytecode* bc, int ci, ClassDecl* d) {
    ClassType* ct = &bc->classes[ci];

    int n = bc->method_slot_count;
    ct->vtable = n ? x_alloc((size_t)n * sizeof(int)) : NULL;
    ct->vtable_len = n;
    for (int i = 0; i < n; i++) ct->vtable[i] = -1;

    // 可见性与虚表平行建表，初值 PUBLIC —— 只是占位，真正有意义的
    // 值在下面按本类声明写入，继承的槽由 build_vtable_funcs 末尾跟着
    // 虚表一起补下来。
    ct->method_vis = n ? x_alloc((size_t)n * sizeof(Visibility)) : NULL;
    for (int i = 0; i < n; i++) ct->method_vis[i] = VIS_PUBLIC;

    // ── 这里**不**抄父类虚表 ──────────────────────────────────
    //
    // 最初的设计是「抄父表 + 替换覆写槽」，看起来最自然。但它有一个
    // 致命的时序问题：建虚表时函数表还不存在，所以本类方法只能写
    // -2（待回填），而父类此时**也是** -2 —— 子类抄过去之后，那个
    // -2 到底代表「本类声明了、待回填」还是「从父类抄来的、待回填」，
    // 已经分不清了。
    //
    // 回填阶段按 ClassDecl 逐类处理「本类声明的方法」，而抄来的槽
    // 不属于本类的声明，于是永远不会被回填 —— 槽里留着 -2，运行时
    // 就报「类 Dog 没有实现方法 describe」，而 Dog 明明沿用了父类
    // 的实现。
    //
    // 所以改成：本类只填自己的槽，继承放到回填之后做（那时父类
    // 的索引已经是真实值）。见 build_vtable_funcs。

    // 写入本类方法
    for (int i = 0; i < d->method_count; i++) {
        ClassMethod* cm = &d->methods[i];
        if (cm->is_static) continue;              // 静态方法不进虚表
        if (strcmp(cm->name, "constructor") == 0) continue;  // 构造函数不进

        int slot = bc_method_slot(bc, cm->name);
        // 这里不填函数索引 —— 因为此刻函数表还没建，bc->funcs 里
        // 还没有这个方法的条目。真正的回填在 build_vtable_funcs()
        // 里做（函数表建完之后）。这里只负责：
        //   · 建槽（保证所有类的虚表长度一致）
        //   · 校验覆写合法性
        int parent_fi = -1;
        if (ct->parent_idx >= 0) {
            ClassType* pt = &bc->classes[ct->parent_idx];
            if (slot < pt->vtable_len) parent_fi = pt->vtable[slot];
        }

        if (cm->is_abstract) {
            // 抽象方法：如果本类不是抽象类，这是错误
            if (!d->is_abstract && !d->is_interface) {
                diag_error(cm->pos,
                           "类 `%s` 有抽象方法 `%s`，但类本身没有标 `abstract`",
                           d->name, cm->name);
                diag_help("两种改法：把类声明成 `abstract class %s`，"
                          "或者给 `%s` 补一个 `{ ... }` 函数体", d->name, cm->name);
            }
            continue;
        }

        if (cm->is_override && parent_fi < 0) {
            // 父类真没有这个槽。注意 parent_fi < 0 有两种情况：
            // 父类没有该方法（真错），或父类的实现还没回填索引。
            // 后者是正常的 —— 所以这里只判「槽是否存在」，把
            // 「父类到底有没有这个方法」的判断推迟到回填阶段。
            // 为让判断可靠，这里改用「槽在父类里是否被父类的声明占用」，
            // 而那需要另一张表；简单起见，override 的严格校验放在
            // 回填之后（那里能拿到完整信息）。
        }

        // 记录「这个类在这个槽上声明了自己的方法」，回填阶段据此填索引
        ct->vtable[slot] = -2;   // -2 = 已声明待回填（区别于 -1 = 未实现）

        // 可见性跟着一起记。
        //
        // 「覆写不能收窄可见性」的校验**不在这里**做 —— 见
        // build_vtable_funcs 里回填阶段的那段注释：此刻父类在这个
        // 槽上的值还是占位（-1 或 -2），拿它判「父类有没有这个方法」
        // 会得到错误答案。
        ct->method_vis[slot] = cm->vis;
    }
}

// 检查一个类是否实现了它声明的所有接口方法。
//
// 判据：对每个接口方法名，本类（含父类）的虚表槽必须 >= 0。
// 这正是「接口是编译期契约」的落地 —— 运行时不需要接口存在。
static void check_interfaces(Bytecode* bc, int ci, Arena* a) {
    ClassType* ct = &bc->classes[ci];
    if (ct->is_interface) return;

    for (int i = 0; i < ct->iface_count; i++) {
        int ii = ct->iface_idx[i];
        if (ii < 0 || ii >= bc->class_count) continue;
        ClassType* it = &bc->classes[ii];

        // 接口的方法在虚表里的槽必须已被本类（或祖先）填上
        for (int s = 0; s < it->vtable_len && s < ct->vtable_len; s++) {
            if (it->vtable[s] < 0) continue;   // 接口自己没声明的槽
            if (ct->vtable[s] < 0) {
                // 找出这个槽对应的方法名，报给用户
                const char* mname = (s < bc->method_slot_count)
                                    ? bc->method_slots[s].name : "?";
                diag_error((SrcPos){ NULL, 1, 1 },
                           "类 `%s` 声明实现了接口 `%s`，但没有实现方法 `%s`",
                           ct->name, it->name, mname);
                diag_help("给 `%s` 补上 `fn %s(...) { ... }`，"
                          "或者删掉 `implements %s`",
                          ct->name, mname, it->name);
            }
        }
    }
    (void)a;
}

// 把 AST 上的注解拍平进 Bytecode 的全局注解表。
//
// ── 为什么在编译期就拍平，而不是运行时查 AST ────────────────
//
// 运行时没有 AST —— 解释器只加载 Bytecode，`x run foo.xbc` 和
// AOT 产物更是连源码都没有。注解的**唯一**存在形式必须是字节码里
// 的元数据，AST 只是它的编译期来源。
//
// ── 为什么跳过没有注解的目标 ────────────────────────────────
//
// 绝大多数字段/方法没有注解，给它们各建一条空记录会让表膨胀十倍
// 而信息量为零。查询侧本来就要处理「找不到」的情况（那是最常见的
// 结果），所以缺席即「无注解」不增加任何分支。
static void build_annotations(Bytecode* bc, Module* m) {
    for (int i = 0; i < m->class_count; i++) {
        ClassDecl* d = m->classes[i];
        int ci = bc_find_class(bc, d->name);
        if (ci < 0) continue;   // 重复定义/AST 里没有对应类，已报过错

        // 类注解
        for (int k = 0; k < d->annos.count; k++) {
            bc_add_anno(bc, ANNO_TARGET_CLASS, ci, NULL, &d->annos.items[k]);
        }
        // 字段注解
        for (int f = 0; f < d->field_count; f++) {
            AnnotationSet* s = &d->fields[f].annos;
            for (int k = 0; k < s->count; k++) {
                bc_add_anno(bc, ANNO_TARGET_FIELD, ci, d->fields[f].name,
                            &s->items[k]);
            }
        }
        // 方法注解
        for (int mm = 0; mm < d->method_count; mm++) {
            AnnotationSet* s = &d->methods[mm].annos;
            for (int k = 0; k < s->count; k++) {
                bc_add_anno(bc, ANNO_TARGET_METHOD, ci, d->methods[mm].name,
                            &s->items[k]);
            }
        }
    }
}

// 建立整张类表。返回 false 表示有错误（诊断已报）。
static bool build_class_table(Module* m, Bytecode* bc, Arena* a) {

    // ---- 1. 登记类名（空壳） ----
    //
    // 先全部登记再解析父类，因为父类可能写在后文。这与函数签名表
    // 「先建全表再编译」是同一个理由：前向引用不该要求用户排序。
    for (int i = 0; i < m->class_count; i++) {
        ClassDecl* d = m->classes[i];
        d->pos = d->pos;   // keep
        if (bc_find_class(bc, d->name) >= 0) {
            diag_error(d->pos, "类型 `%s` 重复定义", d->name);
            diag_help("类与类、类与结构体都不能同名 —— 它们占同一个"
                      "类型命名空间（`let x: %s` 只能指向一个东西）", d->name);
            continue;
        }
        if (bc_find_struct(bc, d->name) >= 0) {
            diag_error(d->pos, "`%s` 已经是结构体名，不能再用作类名", d->name);
            diag_help("结构体与类共用类型命名空间。给其中一个改名");
            continue;
        }
        int ci = bc_add_class(bc, d->name);
        bc->classes[ci].is_abstract  = d->is_abstract || d->is_interface;
        bc->classes[ci].is_interface = d->is_interface;
        bc->classes[ci].is_final     = d->is_final;
    }

    // ---- 2. 接父链 ----
    for (int i = 0; i < m->class_count; i++) {
        ClassDecl* d = m->classes[i];
        int ci = bc_find_class(bc, d->name);
        if (ci < 0) continue;

        if (d->parent_name) {
            int pi = bc_find_class(bc, d->parent_name);
            if (pi < 0) {
                diag_error(d->pos, "父类 `%s` 未定义", d->parent_name);
                diag_help("检查拼写，或确认 `%s` 已在同一模块/已 import 的模块里声明",
                          d->parent_name);
            } else {
                bc->classes[ci].parent_idx = pi;
                if (bc->classes[pi].is_final) {
                    diag_error(d->pos, "类 `%s` 是 final 的，不能被继承",
                               d->parent_name);
                    diag_help("删掉父类的 `final`，或让 `%s` 不继承它", d->name);
                }
                if (bc->classes[pi].is_interface) {
                    diag_error(d->pos,
                               "`%s` 是接口，不能作为父类（应该用 `implements`）",
                               d->parent_name);
                    diag_help("把 `extends %s` 改成 `implements %s` —— "
                              "接口只描述行为，不提供字段布局",
                              d->parent_name, d->parent_name);
                }
            }
        }

        // 接口列表
        if (d->iface_count > 0) {
            bc->classes[ci].iface_idx = x_alloc((size_t)d->iface_count * sizeof(int));
            bc->classes[ci].iface_count = 0;
            for (int k = 0; k < d->iface_count; k++) {
                int ii = bc_find_class(bc, d->iface_names[k]);
                if (ii < 0) {
                    diag_error(d->pos, "接口 `%s` 未定义", d->iface_names[k]);
                    diag_help("检查拼写，或确认该接口已声明/已 import");
                    continue;
                }
                if (!bc->classes[ii].is_interface) {
                    diag_error(d->pos, "`%s` 不是接口，不能 implements",
                               d->iface_names[k]);
                    diag_help("`implements` 后面只能跟 `interface`。"
                              "要继承类请用 `extends`");
                    continue;
                }
                bc->classes[ci].iface_idx[bc->classes[ci].iface_count++] = ii;
            }
        }
    }

    // ---- 3. 检测继承环 ----
    //
    // 环检测必须在展开字段之前：字段展开会沿父链递归，遇环会
    // 无限递归/爆栈。而环本身是一个用户错误，应该给出诊断而不是崩。
    for (int i = 0; i < bc->class_count; i++) {
        int cur = i;
        int guard = 0;
        while (cur >= 0 && guard <= bc->class_count) {
            cur = bc->classes[cur].parent_idx;
            guard++;
        }
        if (guard > bc->class_count) {
            diag_error((SrcPos){ m->file, 1, 1 },
                       "类 `%s` 的继承链上存在环", bc->classes[i].name);
            diag_help("检查 extends 关系，继承不能成环（A extends B, B extends A）");
            return false;
        }
    }

    // ---- 4. 展开字段（父类先处理）----
    //
    // 按「父类深度」排序处理后，每个类展开时其父类一定已完成。
    // 用朴素的插入排序：类数量是几十的量级，不值得上快排。
    int* order = x_alloc((size_t)bc->class_count * sizeof(int));
    for (int i = 0; i < bc->class_count; i++) order[i] = i;
    for (int i = 1; i < bc->class_count; i++) {
        int v = order[i];
        int depth_v = 0;
        for (int c = bc->classes[v].parent_idx; c >= 0; c = bc->classes[c].parent_idx) depth_v++;
        int j = i - 1;
        while (j >= 0) {
            int depth_j = 0;
            for (int c = bc->classes[order[j]].parent_idx; c >= 0; c = bc->classes[c].parent_idx) depth_j++;
            if (depth_j <= depth_v) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    for (int oi = 0; oi < bc->class_count; oi++) {
        int ci = order[oi];
        ClassDecl* d = NULL;
        for (int k = 0; k < m->class_count; k++) {
            if (strcmp(m->classes[k]->name, bc->classes[ci].name) == 0) {
                d = m->classes[k];
                break;
            }
        }
        if (!d) continue;
        if (bc->classes[ci].is_interface && d->field_count > 0) {
            continue;   // 解析阶段已报过
        }
        build_class_fields(bc, ci, d);
    }

    // ---- 5. 虚表 ----
    //
    // 先扫一遍所有实例方法名，把全局槽位建全 —— 否则后面构建虚表时
    // 槽表还在增长，先建的类虚表长度就短了。
    for (int i = 0; i < m->class_count; i++) {
        ClassDecl* d = m->classes[i];
        for (int k = 0; k < d->method_count; k++) {
            ClassMethod* cm = &d->methods[k];
            if (cm->is_static) continue;
            if (strcmp(cm->name, "constructor") == 0) continue;
            bc_method_slot(bc, cm->name);
        }
    }

    for (int oi = 0; oi < bc->class_count; oi++) {
        int ci = order[oi];
        ClassDecl* d = NULL;
        for (int k = 0; k < m->class_count; k++) {
            if (strcmp(m->classes[k]->name, bc->classes[ci].name) == 0) {
                d = m->classes[k];
                break;
            }
        }
        if (!d) continue;
        build_class_vtable(bc, ci, d);
    }

    // ---- 6. 接口实现检查 ----
    for (int i = 0; i < bc->class_count; i++) {
        check_interfaces(bc, i, a);
    }

    // ---- 7. 注解元数据 ----
    //
    // 放在所有类建完、`class_idx` 稳定之后。注解本身不参与任何语义
    // 判断（语言层只存不解释），所以它可以是整个流程的最后一步 ——
    // 放到前面反而要担心「这时候类表还没齐」。
    build_annotations(bc, m);

    free(order);
    return g_error_count == 0;
}


// 虚表回填：把 -2 的「已声明待回填」槽换成真正的函数索引。
//
// ── 为什么必须分成两步 ──────────────────────────────────────
//
// 虚表要在编译函数体**之前**就绪（函数体里 `self.area()` 需要槽号），
// 而槽里存的函数索引要等函数表建完（也就是类方法被登记成 FuncProto
// 之后）。两个需求互相矛盾，只能拆开：先占槽（-2），后填索引。
//
// 用 -2 而不是 0 做占位符：0 是合法函数索引（第一个函数），
// 拿它当「未填」的哨兵会让「类只声明了一个方法、方法恰好是 0 号函数」
// 这种情况无法区分。魔鬼都在这种地方。
//
// ── 这一段同时做覆写校验 ────────────────────────────────────
//
// 因为「父类有没有这个方法」只有回填之后才可靠（父类可能还没回填）。
// 校验放在信息最全的时刻，而不是代码看起来最顺手的地方。
static void build_vtable_funcs(Module* m, Bytecode* bc) {
    // ── 必须按「父类优先」的顺序处理 ────────────────────────────
    //
    // 回填之后要抄父类的实现（见末尾）。如果先处理子类，父类的槽
    // 还是 -2，抄过来又变成 -2 —— 同样的 bug 换个地方复现。
    //
    // 用深度排序（与字段展开同一套办法）：父类深度小，先处理。
    int* order = x_alloc((size_t)bc->class_count * sizeof(int));
    for (int i = 0; i < bc->class_count; i++) order[i] = i;
    for (int i = 1; i < bc->class_count; i++) {
        int v = order[i];
        int dv = 0;
        for (int c = bc->classes[v].parent_idx; c >= 0;
             c = bc->classes[c].parent_idx) dv++;
        int j = i - 1;
        while (j >= 0) {
            int dj = 0;
            for (int c = bc->classes[order[j]].parent_idx; c >= 0;
                 c = bc->classes[c].parent_idx) dj++;
            if (dj <= dv) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    for (int oi = 0; oi < bc->class_count; oi++) {
        int ci = order[oi];
        ClassType* ct = &bc->classes[ci];

        // 找对应的 ClassDecl
        ClassDecl* d = NULL;
        for (int k = 0; k < m->class_count; k++) {
            if (strcmp(m->classes[k]->name, ct->name) == 0) {
                d = m->classes[k];
                break;
            }
        }
        if (!d) continue;

        // ---- 回填构造函数的函数索引 ----
        //
        // 构造函数不进虚表（它不能被继承、不能被覆写、只能经 new 调），
        // 所以它的索引没地方顺带记下来。但 AOT 生成期必须知道调哪个
        // 函数 —— 解释器可以按 `Class.constructor` 名字现查，生成 C 代码
        // 不行（那里没有函数名表）。
        //
        // 别的类可能还没有这条记录，所以这里找不到就保持 -1，
        // 让「没有构造函数」与「还没回填」统一表现为 -1。
        if (ct->ctor_fn < 0) {
            char cbuf[512];
            size_t nl = strlen(ct->name);
            if (nl + sizeof(".constructor") <= sizeof(cbuf)) {
                memcpy(cbuf, ct->name, nl);
                memcpy(cbuf + nl, ".constructor", sizeof(".constructor"));
                for (int x = 0; x < bc->func_count; x++) {
                    if (bc->funcs[x].name && strcmp(bc->funcs[x].name, cbuf) == 0) {
                        ct->ctor_fn = x;
                        break;
                    }
                }
            }
        }

        for (int k = 0; k < d->method_count; k++) {
            ClassMethod* cm = &d->methods[k];
            if (cm->is_static) continue;
            if (strcmp(cm->name, "constructor") == 0) continue;

            int slot = bc_find_method_slot(bc, cm->name);
            if (slot < 0 || slot >= ct->vtable_len) continue;

            // 只有「本类自己声明了实现」的槽才回填。
            // 建表阶段本类只写自己的槽（不抄父类），所以这里的
            // 非 -2 只可能是先前处理过的重复声明，跳过即可。
            if (ct->vtable[slot] != -2) continue;

            if (!cm->fn || !cm->fn->body) {
                // 抽象方法：把槽还原成「继承来的实现」或 -1
                ct->vtable[slot] = -1;
                if (ct->parent_idx >= 0) {
                    ClassType* pt = &bc->classes[ct->parent_idx];
                    if (slot < pt->vtable_len) ct->vtable[slot] = pt->vtable[slot];
                }
                continue;
            }

            // 按改写后的名字（Class.method）查真实函数索引
            char buf[512];
            size_t nl = strlen(ct->name);
            size_t ml = strlen(cm->name);
            if (nl + 1 + ml + 1 <= sizeof(buf)) {
                memcpy(buf, ct->name, nl);
                buf[nl] = '.';
                memcpy(buf + nl + 1, cm->name, ml);
                buf[nl + 1 + ml] = '\0';
                int fi = -1;
                for (int x = 0; x < bc->func_count; x++) {
                    if (bc->funcs[x].name && strcmp(bc->funcs[x].name, buf) == 0) {
                        fi = x;
                        break;
                    }
                }
                ct->vtable[slot] = fi;
            }

            // ---- 覆写校验 ----
            int parent_slot_val = -1;
            if (ct->parent_idx >= 0) {
                ClassType* pt = &bc->classes[ct->parent_idx];
                if (slot < pt->vtable_len) parent_slot_val = pt->vtable[slot];
            }

            if (cm->is_override && parent_slot_val < 0) {
                diag_error(cm->pos,
                           "方法 `%s` 标了 `override`，但父类 `%s` 没有同名方法可覆写",
                           cm->name,
                           ct->parent_idx >= 0 ? bc->classes[ct->parent_idx].name
                                               : "(无)");
                diag_help("检查方法名是否拼错，或父类签名是否刚改过。"
                          "如果是新方法，删掉 `override`");
            }

            // final 方法禁止被覆写
            if (parent_slot_val >= 0 && cm->is_override) {
                // 查父类那个方法是不是 final
                for (int pk = 0; pk < m->class_count; pk++) {
                    if (strcmp(m->classes[pk]->name, ct->name) != 0) continue;
                    break;
                }
                if (ct->parent_idx >= 0) {
                    ClassDecl* pd = NULL;
                    for (int pk = 0; pk < m->class_count; pk++) {
                        if (strcmp(m->classes[pk]->name,
                                   bc->classes[ct->parent_idx].name) == 0) {
                            pd = m->classes[pk];
                            break;
                        }
                    }
                    if (pd) {
                        for (int pk = 0; pk < pd->method_count; pk++) {
                            if (strcmp(pd->methods[pk].name, cm->name) == 0 &&
                                pd->methods[pk].is_final) {
                                diag_error(cm->pos,
                                           "方法 `%s` 在父类 `%s` 里是 final 的，不能覆写",
                                           cm->name, pd->name);
                                diag_help("删掉父类该方法上的 `final`，"
                                          "或者给子类的方法换个名字");
                            }
                        }
                    }
                }
            }

            // ---- 覆写不能收窄可见性 ----
            //
            // 这是 LSP（里氏替换）的直接推论：父类方法 public、子类覆写
            // 成 private 之后，「拿着父类引用调这个方法」在编译期看的是
            // 父类（放行），运行时会撞上子类的限制 —— 而调用点完全
            // 无法预知。所以子类只能放宽或保持，不能收紧。
            //
            // ── 为什么校验放在这里而不是 build_class_vtable ----
            //
            // 建表阶段父类在同一个槽上的值还是占位（-1 或 -2），用它
            // 判「父类到底有没有这个方法」会得出错误答案 —— 于是整个
            // 校验静默失效，而成品看起来一切正常。这类「检查写了但
            // 永远不触发」的代码比不写更危险。
            //
            // 此刻父类的槽已经是真实函数索引，parent_slot_val >= 0
            // 精确地表示「父类链上有实现，这是一次覆写」。
            if (parent_slot_val >= 0 && ct->parent_idx >= 0) {
                ClassType* pt = &bc->classes[ct->parent_idx];
                if (slot < pt->vtable_len && pt->method_vis) {
                    Visibility pv = pt->method_vis[slot];
                    // 枚举顺序是 PUBLIC=0 < PROTECTED=1 < PRIVATE=2，
                    // 所以「数值变大」正是「收窄」。
                    if ((int)cm->vis > (int)pv) {
                        diag_error(cm->pos,
                                   "覆写 `%s` 时不能收窄可见性：父类 `%s` 里是 %s，"
                                   "这里写成了 %s",
                                   cm->name, pt->name,
                                   vis_name(pv), vis_name(cm->vis));
                        diag_help("子类方法的可见性只能放宽或保持，不能收紧。"
                                  "否则「用父类引用调用」在运行时会撞上访问限制，"
                                  "而调用点完全无法预知");
                    }
                }
            }
        }
    }

    // ---- 继承：把「本类没声明」的槽从父类补下来 ----
    //
    // ── 为什么继承只能放在回填之后 ──────────────────────────
    //
    // 建表时父类的槽还是 -2（函数表还不存在），此时抄过来就把占位
    // 符固化进了子类，而回填只认「本类声明的方法」，抄来的槽没人
    // 管 —— 于是子类明明继承了父类实现，运行时却说没有。
    //
    // 现在父类的槽已经是真实的函数索引，抄下来的就是可用值。
    // 只填 -1 的槽（= 本类和祖先都没声明），已经回填或继承过的不动。
    //
    // 顺序与上面一致：父类先处理，所以这里读到的父表一定是终值。
    for (int oi = 0; oi < bc->class_count; oi++) {
        int ci = order[oi];
        ClassType* ct = &bc->classes[ci];
        if (ct->parent_idx < 0) continue;

        ClassType* pt = &bc->classes[ct->parent_idx];
        for (int s = 0; s < ct->vtable_len && s < pt->vtable_len; s++) {
            if (ct->vtable[s] < 0 && pt->vtable[s] >= 0) {
                ct->vtable[s] = pt->vtable[s];
                // 可见性必须**跟着实现一起**继承。
                //
                // 漏了这一行的症状极隐蔽：子类继承了一个 protected
                // 方法，虚表里指向父类实现（能跑对），但 method_vis
                // 还是建表时的占位 PUBLIC —— 于是外部可以调用它。
                // 功能完全正常，封装已经漏了，任何测试只要不检查
                // 「能不能访问」就都发现不了。
                ct->method_vis[s] = pt->method_vis[s];
            }
        }
    }

    free(order);
}


// ============================================================
// 字节码可达性裁剪
// ============================================================
//
// 合并阶段已经挡住了「未 import 的名字」，但入口文件里没有被调用
// 的函数、以及声明了却走不到的库函数仍然会占内存。这里从 main
// 出发做一遍可达性分析，不可达函数整段删除，并重写所有函数索引。
//
// 保守点：classNew / methodCall 的类名可能来自运行期字符串，静态
// 无法证明哪些类用不到；一旦出现就把所有类的构造与虚表方法标为
// 可达（只影响体积，不影响正确性）。

static void bc_reach_func(Bytecode* bc, int fi, bool* reach,
                          int* queue, int* qn) {
    if (fi < 0 || fi >= bc->func_count || reach[fi]) return;
    reach[fi] = true;
    queue[(*qn)++] = fi;
}

static void bc_reach_class(Bytecode* bc, int ci, bool* reach,
                           int* queue, int* qn) {
    if (ci < 0 || ci >= bc->class_count) return;
    ClassType* c = &bc->classes[ci];
    bc_reach_class(bc, c->parent_idx, reach, queue, qn);
    bc_reach_func(bc, c->ctor_fn, reach, queue, qn);
    for (int i = 0; i < c->vtable_len; i++) {
        bc_reach_func(bc, c->vtable[i], reach, queue, qn);
    }
}

static void bc_prune_unreachable(Bytecode* bc) {
    if (bc->func_count == 0 || bc->code_count == 0) return;

    int main_idx = -1;
    for (int i = 0; i < bc->func_count; i++) {
        if (bc->funcs[i].name && strcmp(bc->funcs[i].name, "main") == 0) {
            main_idx = i;
            break;
        }
    }
    // 没有 main 说明前面已经报过错（没有入口），这里不动。
    if (main_idx < 0) return;

    bool* reach = calloc((size_t)bc->func_count, sizeof(bool));
    int*  queue = malloc((size_t)bc->func_count * sizeof(int));
    if (!reach || !queue) { free(reach); free(queue); return; }
    int  qn = 0;

    bc_reach_func(bc, main_idx, reach, queue, &qn);

    bool dynamic_classes = false;
    for (int qi = 0; qi < qn; qi++) {
        FuncProto* fp = &bc->funcs[queue[qi]];
        for (int k = 0; k < fp->code_len; k++) {
            Insn* in = &bc->code[fp->code_start + k];
            switch (in->op) {
                case BC_CALL:
                    bc_reach_func(bc, in->a, reach, queue, &qn);
                    break;
                case BC_NEW_CLASS:
                    bc_reach_class(bc, in->a, reach, queue, &qn);
                    break;
                case BC_NEW_CLASS_NAME:
                case BC_CALL_NAME:
                    dynamic_classes = true;
                    break;
                // 反射类内建按「字符串类名」工作，静态扫描看不到它们
                // 引用了哪个类。不把它们算进来，DCE 会把没有被 new 的
                // 类的 vtable 裁掉，表现为 methodParams("Foo", ...)/
                // classAll() 返回空 —— 而源码里明明声明了这个类。
                case BC_BUILTIN:
                    switch ((XBuiltin)in->a) {
                        case XBI_METHOD_PARAMS:
                        case XBI_CLASS_ALL:
                        case XBI_ANNO_HAS:
                        case XBI_ANNO_GET:
                        case XBI_ANNO_GET_KV:
                        case XBI_ANNO_ALL:
                        case XBI_CLASS_FIELDS:
                        case XBI_CLASS_METHODS:
                            dynamic_classes = true;
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (dynamic_classes) {
        for (int ci = 0; ci < bc->class_count; ci++) {
            bc_reach_class(bc, ci, reach, queue, &qn);
        }
    }

    // old → new 下标
    int* map = malloc((size_t)bc->func_count * sizeof(int));
    if (!map) { free(reach); free(queue); return; }
    int new_count = 0;
    for (int i = 0; i < bc->func_count; i++) {
        map[i] = reach[i] ? new_count++ : -1;
    }

    // 未保留的函数名单独释放；保留了的是指针复制，所有权跟着新表。
    for (int i = 0; i < bc->func_count; i++) {
        if (!reach[i]) free(bc->funcs[i].name);
    }

    FuncProto* nf = calloc((size_t)(new_count ? new_count : 1), sizeof(FuncProto));
    Insn* ncode = malloc((size_t)(bc->code_count ? bc->code_count : 1) * sizeof(Insn));
    SrcPos* npos = bc->pos
        ? malloc((size_t)(bc->code_count ? bc->code_count : 1) * sizeof(SrcPos))
        : NULL;
    if (!nf || !ncode || (bc->pos && !npos)) {
        free(nf); free(ncode); free(npos); free(map); free(reach); free(queue);
        return;
    }

    int ni = 0, nc = 0;
    for (int i = 0; i < bc->func_count; i++) {
        if (!reach[i]) continue;
        FuncProto* src = &bc->funcs[i];
        nf[ni] = *src;
        nf[ni].code_start = nc;

        memcpy(&ncode[nc], &bc->code[src->code_start],
               (size_t)src->code_len * sizeof(Insn));
        if (bc->pos) {
            memcpy(&npos[nc], &bc->pos[src->code_start],
                   (size_t)src->code_len * sizeof(SrcPos));
        }

        for (int k = 0; k < src->code_len; k++) {
            Insn* in = &ncode[nc + k];
            if (in->op == BC_CALL && in->a >= 0 && in->a < bc->func_count) {
                in->a = map[in->a];
            }
        }

        nc += src->code_len;
        ni++;
    }

    // 类表里的函数索引同样要改。
    for (int ci = 0; ci < bc->class_count; ci++) {
        ClassType* ct = &bc->classes[ci];
        if (ct->ctor_fn >= 0) {
            ct->ctor_fn = (ct->ctor_fn < bc->func_count) ? map[ct->ctor_fn] : -1;
        }
        for (int i = 0; i < ct->vtable_len; i++) {
            int v = ct->vtable[i];
            // -1（未实现）/ -2（待回填）原样保留，只重写真实函数下标。
            ct->vtable[i] = (v >= 0 && v < bc->func_count) ? map[v] : v;
        }
    }

    free(bc->code);
    bc->code = ncode;
    bc->code_count = nc;
    bc->code_cap = nc;

    free(bc->pos);
    bc->pos = npos;
    bc->pos_cap = nc;

    free(bc->funcs);
    bc->funcs = nf;
    bc->func_count = new_count;
    bc->func_cap = new_count;

    free(map);
    free(reach);
    free(queue);
}

bool compile_to_bytecode(Module* m, Bytecode* bc, Arena* a) {
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.bc      = bc;
    c.arena   = a;
    c.cur_mod = m;   // 静态方法可见性检查要用（见 check_static_visible）
    g_cur_bc = bc;   // 供 ty_from_name / is_struct_name 查结构体表

    // ---- 第负一遍：建立结构体类型表 ----
    //
    // 必须在**任何**类型推断之前完成：函数签名里可能写 `-> Rect`，
    // 局部变量可能写 `let r: Rect`，而 ty_from_name 需要类型表
    // 已经建好才能把 "Rect" 认成 TY_STRUCT。放后面就会出现
    // 「函数返回值类型推断不出来」的假性错误。
    //
    // 顺带做三件早期检查（都属「一次列全同类问题」那类）：
    //   1. 结构体重名
    //   2. 字段重名
    //   3. 字段类型名存在（拼错的类型要到用的时候才报，那时候
    //      错误位置指向使用点，而真正的错在声明处）
    //
    // 不检查「结构体套结构体」的循环引用：P0.5 用引用计数共享，
    // `A { b: B }` 和 `B { a: A }` 不会造成无限大小，只是构造时
    // 必然有一个要先用 null —— 而这不影响正确性。
    for (int i = 0; i < m->struct_count; i++) {
        StructDecl* d = m->structs[i];
        if (bc_find_struct(bc, d->name) >= 0) {
            diag_error(d->pos, "结构体 `%s` 重复定义", d->name);
            diag_help("同一文件内结构体名必须唯一。若两个 import 的文件都定义了同名结构体，"
                      "重命名其中一个");
            continue;
        }
        for (int x = 0; x < d->field_count; x++) {
            for (int y = x + 1; y < d->field_count; y++) {
                if (d->fields[x].name && d->fields[y].name &&
                    strcmp(d->fields[x].name, d->fields[y].name) == 0) {
                    diag_error(d->pos, "结构体 `%s` 里字段 `%s` 重复",
                               d->name, d->fields[x].name);
                    diag_help("字段名在结构体内必须唯一。字段顺序参与内存布局，"
                              "重名的两个字段无法区分");
                }
            }
        }
        char** fnames = d->field_count ? x_alloc((size_t)d->field_count * sizeof(char*)) : NULL;
        char** ftypes = d->field_count ? x_alloc((size_t)d->field_count * sizeof(char*)) : NULL;
        for (int x = 0; x < d->field_count; x++) {
            fnames[x] = d->fields[x].name;
            ftypes[x] = d->fields[x].type_name;
        }
        bc_add_struct(bc, d->name, fnames, ftypes, d->field_count);
        free(fnames);
        free(ftypes);
    }

    // 第二遍：字段类型是否都是已知类型。
    //
    // 放在单独一轮而不是上面那一轮：上面那一轮还在建表，
    // 此时前面定义的结构体已入表、后面的还没有，跨结构体引用
    // 会被误报「未知类型」。全部入表之后再查才准确。
    for (int i = 0; i < m->struct_count; i++) {
        StructDecl* d = m->structs[i];
        for (int x = 0; x < d->field_count; x++) {
            const char* tn = d->fields[x].type_name;
            if (!tn) continue;   // 缺失注解已经在 parser 报过
            if (ty_from_name(tn) == TY_UNKNOWN) {
                diag_error(d->pos, "结构体 `%s` 的字段 `%s` 类型 `%s` 未知",
                           d->name, d->fields[x].name, tn);
                diag_help("字段类型必须是 Int / Float / Bool / String / Array "
                          "或另一个已声明的结构体");
            }
        }
    }

    // ---- 第零遍：收集顶层常量 ----
    //
    // 必须在编译任何函数体**之前**做完：函数体里会引用常量，而引用点
    // 需要立刻拿到值来发射字面量指令。放到后面就会出现「先引用的
    // 函数拿不到常量」的假性未定义错误。
    //
    // 这里也顺手把顶层 const 从 top_stmts 里**抹掉**（换成空语句），
    // 因为它们的值已经在编译期用光了。不抹的话，合成的 main 里会
    // 再执行一次「把常量存进局部变量」，既浪费指令，又会让常量名
    // 在 main 里变成一个真实的局部变量 —— 那会让「函数不能改常量」
    // 这条语义在 main 里失效。
    ConstTable cts;
    ct_init(&cts);
    c.ct = &cts;

    for (int i = 0; i < m->top_count; i++) {
        Stmt* s = m->top_stmts[i];
        if (s->kind != ST_LET || !s->var.is_const) continue;

        GlobalConst* g = ct_add(&cts, s->var.name);
        if (!ct_extract(g, s->var.init)) {
            // ct_extract 已经报错，这里只需让名字有个可用的值，
            // 避免后续引用点再报一堆「未定义」。
            g->kind = CONST_KIND_NULL;
        }
    }

    // 把 const 声明从顶层语句里**摘掉**，而不是抹成空语句。
    //
    // ── 为什么不能留成空语句 ────────────────────────────────
    //
    // 常量已经被 ct_emit 在**引用点**直接发射成字面量了，留着声明
    // 只会多一条无用指令。原本的做法是原地替换成 `0`（一条无害的
    // 表达式语句），看起来很安全，但它把「这里有一条顶层语句」
    // 这个事实保留了下来 —— 而模块系统恰好拿这个事实做判据：
    //
    //     import "ui.xle"    // 里面有 52 条 const
    //     fn main() -> Int { ... }
    //
    // 合并后的顶层语句 = 52 条 const + demo.xle 自己的若干条，于是
    // 编译器报「存在 fn main() 时不允许再有顶层语句」。这条错误信息
    // 完全指错方向：用户确实没写任何顶层语句，是 import 带进来的
    // 常量声明被误算了。
    //
    // 根本原因在于「常量声明」和「顶层可执行语句」是两件事，不该
    // 用同一个数组表达。P0 最小修法：摘掉之后数组里就只剩真正的
    // 可执行语句，判据自然正确。
    //
    // 这里重建数组而不是原地删除，是因为 top_stmts 可能来自模块
    // 合并（main.c 的 ModGraph），共享同一份内存。新建一份数组、
    // 只把要保留的指针拷过去，谁都不会被改写。ct_add 已经 x_strdup
    // 了常量名，所以丢弃原 Stmt 不会留下悬垂指针。
    if (cts.count > 0) {
        int keep = 0;
        for (int i = 0; i < m->top_count; i++) {
            Stmt* s = m->top_stmts[i];
            if (s->kind == ST_LET && s->var.is_const) continue;
            keep++;
        }
        if (keep != m->top_count) {
            Stmt** out = arena_alloc(a, (size_t)(keep ? keep : 1) * sizeof(Stmt*));
            int k = 0;
            for (int i = 0; i < m->top_count; i++) {
                Stmt* s = m->top_stmts[i];
                if (s->kind == ST_LET && s->var.is_const) continue;
                out[k++] = s;
            }
            m->top_stmts = out;
            m->top_count = k;
        }
    }

// ---- 类表 ----
    //
    // 放在结构体表之后：类字段可以引用结构体类型（`pos: Point`），
    // 反过来结构体字段也能持有类引用。但类的字段布局要算偏移，
    // 所以它必须在结构体表**已经完整**之后建。
    if (!build_class_table(m, bc, a)) {
        // 类表建不起来时后续编译无意义 —— 所有字段偏移与虚表槽都是错的。
        // 直接返回，让用户先修类型错误。
        locals_clear(&c);
        free(c.local_names);
        free(c.local_types);
        free(c.local_type_names);
        return false;
    }

// ---- 第一遍：建立函数签名表 ----
    // 必须先建全表再编译，否则递归与互递归都无法解析。
    //
    // 类方法（含构造函数）会在这里被展开成普通 FuncDecl 加进函数表：
    // 它们在语法阶段就已经是 `Class.method` 形式的普通函数了，
    // 所以这里不需要任何「方法」的特殊处理 —— 只是**来源不同**。
    //
    // 这也解释了为什么虚表要存函数索引而不是函数指针：函数索引在
    // 这个阶段（还没编译函数体）就能确定，而函数指针要等 code_start
    // 全部算完。用索引让「先建表、后填实现」这个顺序成立。
    int total_funcs = m->func_count;
    for (int i = 0; i < m->class_count; i++) {
        total_funcs += m->classes[i]->method_count;
    }

    FuncTable fts;
    ft_init(&fts);
    c.fts = &fts;

    FuncDecl** allf = arena_alloc(a, (size_t)(total_funcs ? total_funcs : 1) * sizeof(FuncDecl*));
    int nf = 0;
    for (int i = 0; i < m->func_count; i++) allf[nf++] = m->funcs[i];

    // 展开类方法，同时记录「方法名 → FuncDecl」的映射，
    // 供后面回填虚表槽（虚表要存的是 bc->funcs 的下标）。
    for (int i = 0; i < m->class_count; i++) {
        ClassDecl* d = m->classes[i];
        for (int k = 0; k < d->method_count; k++) {
            ClassMethod* cm = &d->methods[k];
            if (cm->fn) allf[nf++] = cm->fn;
            // 抽象方法（接口里的、或 abstract 标的方法）没有函数体，
            // 不登记签名。它们只以虚表槽 -1 的形式存在，调用它们
            // 会被运行时报「调用了未实现的抽象方法」—— 而编译期
            // 的 check_interfaces 已经能抓住绝大多数情况。
        }
    }

    // 预分配签名表：容量 = 声明总数（重载让同名声明各自占一张签名，
    // 所以总数上界就是 nf）。必须在注册前一次到位 —— 注册期间任何
    // 一次 realloc 都会让 declsig 里已存下的指针全体失效，症状是
    // 「函数体的 cur_func 指向搬家前的旧数组」，随后在读 name 时
    // 解引用垃圾指针段错误。指针失效与否取决于声明总数跨过几次
    // 倍增阈值，所以小文件碰巧没事、大文件必崩 —— 这类「规模决定
    // 是否复现」的 bug 最难靠肉眼看出来。
    if (nf > 0) {
        fts.cap = nf;
        fts.sigs = (FuncSig*)x_alloc(sizeof(FuncSig) * (size_t)nf);
    }

    // 每个声明对应的签名。第二遍编译函数体必须用这张映射而不是
    // ft_find(名字)：同名重载时按名字找永远命中第一张，会把第二个
    // 函数体编进第一个的 bc 槽位，同时留下一个「被调用但从未定义」
    // 的假报错 —— 两个 bug 互为表里，都源于「名字 → 签名」在重载
    // 下不再是一对一。
    FuncSig** declsig =
        (FuncSig**)x_alloc(sizeof(FuncSig*) * (size_t)(nf > 0 ? nf : 1));

    for (int i = 0; i < nf; i++) {
        FuncDecl* f = allf[i];
        TyKind ret = ty_from_name(f->ret_type);
        if (!f->ret_type) ret = TY_NULL;   // 无返回类型声明 → 视作 void

        // ── 重载判定：同名 ≠ 重复 ─────────────────────────────
        //
        // 参数签名完全一致（个数 + 每个参数的类型名逐字相同）才算
        // 重复定义；参数不同就是合法重载，两张签名都入表，调用点由
        // resolve_overload 按实参类型挑选。
        //
        // 为什么按「类型名逐字比较」而不是 TyKind：param_types 存的
        // 就是名字字符串，而 TyKind 的描述能力比名字窄。两个
        // `fn f(a: Int)` 与 `fn f(a: Int)` 只有真的完全一样才无法
        // 在调用点区分 —— 那才是重复定义。
        //
        // 返回类型不参与判定：`fn f() -> Int` 与 `fn f() -> String`
        // 调用点长得一模一样，靠返回类型区分等于逼读者数返回值，
        // 那不是重载是陷阱。
        FuncSig* dup = NULL;
        for (int k = 0; k < fts.count; k++) {
            FuncSig* t = &fts.sigs[k];
            if (strcmp(t->name, f->name) != 0) continue;
            bool same_params = (t->param_count == f->param_count);
            for (int j = 0; same_params && j < f->param_count; j++) {
                const char* a = (t->param_types && t->param_types[j])
                                    ? t->param_types[j] : NULL;
                const char* b = (f->param_types && f->param_types[j])
                                    ? f->param_types[j] : NULL;
                if ((a == NULL) != (b == NULL)) same_params = false;
                else if (a && strcmp(a, b) != 0) same_params = false;
            }
            if (same_params) { dup = t; break; }
        }
        if (dup) {
            declsig[i] = NULL;
            diag_error(f->pos, "函数 `%s` 重复定义", f->name);
            diag_note("与已有声明的参数个数与类型完全一致，调用点无法区分。");
            diag_help("参数不同就直接写成重载（同名不同签名，调用按实参类型选择）；");
            diag_help("参数相同请改名或合并实现。");
            continue;
        }
        FuncSig* sig = ft_add(&fts, f->name, f->param_count, ret);
        declsig[i] = sig;
        sig->param_types = f->param_types;   // 供调用点做类型检查
        // 返回类型的具体名字。只有结构体需要（其余能从 TyKind 反推），
        // 但这里统一存：将来加泛型时，`[T]` 这类名字也会用上它。
        sig->ret_type_name = f->ret_type;
        sig->bc_index = bc_add_func(bc, f->name, f->param_count, f->pos);

        // 参数名 / 参数类型名进入函数原型（v0.20）。
        //
        // AST 里参数名是必有的（语法要求），类型名可以为 NULL
        // （未标注类型的参数）。指针直接交给 bytecode —— 所有者
        // 是 arena，编译结束后统一释放，没有单独的生命周期要管。
        if (f->param_count > 0) {
            bc->funcs[sig->bc_index].param_names = f->param_names;
            bc->funcs[sig->bc_index].param_types = f->param_types;
        }
    }

    // 函数表已全 → 回填虚表的函数索引。
    // 必须在这一刻做（不能更早：函数还没有索引；不能更晚：编译
    // 函数体时 `self.m()` 会读到 -2 而误判为「未实现」）。
    build_vtable_funcs(m, bc);

    // ---- 第二遍：编译每个函数体 ----
    //
    // 遍历 allf 而不是 m->funcs —— 类方法也要编译。
    // 顺序：先普通函数、后类方法，这与它们在 allf 里的排列一致。
    for (int i = 0; i < nf; i++) {
        FuncDecl* f = allf[i];
        FuncSig* sig = declsig[i];   // 重载安全：见 declsig 的注释
        if (!sig) continue;

        int fi = sig->bc_index;
        bc->funcs[fi].code_start = bc->code_count;

        locals_clear(&c);
        c.cur_func   = sig;
        c.loop_depth = 0;

        // 当前方法属于哪个类 / 叫什么名 —— 可见性检查与 final 判定要用。
        //
        // 名字形如 `Class.method`。普通函数里两者都是 NULL，
        // 于是「在类外访问 private」这条判据自然成立。
        c.cur_class  = NULL;
        c.cur_method = NULL;
        {
            const char* dot = strchr(f->name, '.');
            if (dot) {
                size_t cl = (size_t)(dot - f->name);
                char* cn = x_alloc(cl + 1);
                memcpy(cn, f->name, cl);
                cn[cl] = '\0';
                // 只有真的是类名才算（结构体方法的名字也带点）
                if (is_class_name(cn)) {
                    c.cur_class  = cn;
                    c.cur_method = dot + 1;
                } else {
                    free(cn);
                }
            }
        }

        // 参数占据槽位 0..n-1，顺序与调用方压栈顺序一致
        for (int k = 0; k < f->param_count; k++) {
            TyKind pt = ty_from_name(f->param_types ? f->param_types[k] : NULL);
            int pslot = local_add(&c, f->param_names[k], pt);
            // 结构体/类形参也要登记具体类型名，否则函数体里 `r.w` /
            // `self.n` 追不到宿主。这是最容易漏的一处：局部变量有登
            // 记、形参没有，症状是「同样的代码写在函数体里能过，写成
            // 参数就报无法确定宿主类型」。
            //
            // 类尤其致命：**每个**实例方法都以 `self: 类名` 作为第 0
            // 个形参。漏掉这一支，所有方法体里的 `self.x` 全部报错，
            // 等于类系统的成员访问整体不可用。
            if ((pt == TY_STRUCT || pt == TY_CLASS) &&
                f->param_types && f->param_types[k]) {
                local_set_type_name(&c, pslot, f->param_types[k]);
            }
        }

        compile_stmt(&c, f->body);

        // 函数末尾兜底返回 null（用户没写 return 时）
        emit(&c, BC_PUSH_NULL, 0, f->pos);
        emit(&c, BC_RETURN, 0, f->pos);

        bc->funcs[fi].code_len    = bc->code_count - bc->funcs[fi].code_start;
        bc->funcs[fi].local_count = c.max_local_count;
        sig->defined = true;
    }

    // ---- 第三遍：顶层语句 → 合成 main ----
    bool has_main = (ft_find(&fts, "main") != NULL);

    if (!has_main) {
        if (m->top_count > 0) {
            SrcPos pos = m->top_stmts[0]->pos;
            int fi = bc_add_func(bc, "main", 0, pos);
            bc->funcs[fi].code_start = bc->code_count;

            locals_clear(&c);
            c.cur_func   = NULL;
            c.loop_depth = 0;

            for (int i = 0; i < m->top_count; i++) {
                compile_stmt(&c, m->top_stmts[i]);
            }

            emit(&c, BC_PUSH_I64, bc_add_const_i64(bc, 0), pos);
            emit(&c, BC_RETURN, 0, pos);

            bc->funcs[fi].code_len    = bc->code_count - bc->funcs[fi].code_start;
            bc->funcs[fi].local_count = c.max_local_count;

            FuncSig* sig = ft_add(&fts, "main", 0, TY_INT);
            sig->bc_index = fi;
            sig->defined  = true;
        } else {
            // 这是整个文件的错误，没有具体位置。用文件的第 1 行的位置，
            // 让用户能点击跳转到文件开头，而不是得到「位置 <未知>」。
            diag_error((SrcPos){ m->file, 1, 1 },
                       "程序既没有 `fn main()`，也没有任何顶层语句——没有可执行的入口");
            diag_help("加一个 `fn main() -> Int { ... }`，或直接在文件顶层写语句");
        }
    } else if (m->top_count > 0) {
        diag_error(m->top_stmts[0]->pos,
                   "存在 `fn main()` 时不允许再有顶层语句（P0 限制）。"
                   "请把顶层代码移进 main 函数里。");
            diag_help("把这 %d 条顶层语句全部移进 `fn main()` 体内", m->top_count);
    }

    // ---- 第四遍：检查有没有调用了不存在的函数 ----
    for (int i = 0; i < fts.count; i++) {
        FuncSig* sig = &fts.sigs[i];
        if (!sig->defined) {
            diag_error(bc->funcs[sig->bc_index].pos,
                       "函数 `%s` 被调用但从未定义", sig->name);
            diag_help("补上 `fn %s(...) { ... }` 的定义，或删掉调用", sig->name);
        }
    }

    // ---- 第五遍：超指令合并 ----
    // 放在所有函数都编译完、且确认没有错误之后再做：
    // 有错误时字节码是不完整的，合并它只会让错误信息里的位置错乱。
    //
    // 顺序：**从后往前**。superinstr_merge 会把本段之后的数据整体前移
    // （否则后面函数的 code_start 就错位了），所以处理第 i 个函数时
    // 不能让 i+1..n 的 code_start 已经被改过——从后往前正好满足：
    // 处理 i 时只影响它之后的 k，而那些 k 的 code_start 正好需要减 saved。
    if (g_error_count == 0 && !getenv("X_NO_SUPER")) {
        for (int i = bc->func_count - 1; i >= 0; i--) {
            FuncProto* fn = &bc->funcs[i];
            if (fn->code_len < 2) continue;
            int saved = superinstr_merge(bc, fn->code_start, fn->code_len);
            if (saved <= 0) continue;
            fn->code_len -= saved;
            // 本函数之后的所有函数都前移 saved 条
            for (int k = i + 1; k < bc->func_count; k++) {
                bc->funcs[k].code_start -= saved;
            }
        }
    }

    // ---- 第六遍：不可达函数裁剪 ----
    // 放在超指令合并之后：合并会改 code_len / code_start，裁剪本身
    // 又会重排 code，两者顺序反过来会互相破坏对方的偏移假设。
    if (g_error_count == 0) {
        bc_prune_unreachable(bc);
    }

    // ---- 清理 ----
    locals_clear(&c);
    free(c.local_names);
    free(c.local_types);
    free(c.local_type_names);
    free(fts.sigs);
    ct_free(&cts);

    return g_error_count == 0;
}