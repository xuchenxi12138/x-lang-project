// bytecode.c — 字节码数据结构与发射器
//
// P0 用栈式字节码 + switch 分派（05 文档 §2 的裁决：先做最笨的版本）。
// P1 会在这里换成寄存器式 + 直接线程化，指令表参考 07 文档 §4。
//
// 关键设计：从第一天就有版本号（05 §6 建议 3）。
// .xbc 格式一旦被测试用例依赖，改动成本会陡增。

#include "x.h"

// .xbc 格式版本已挪到 x.h 的 XBC_VERSION —— 写入方（bytecode.c）
// 与校验方（xbc.c）必须看同一个常量。曾经把它留在本文件里，
// 于是提升版本时只改了一半：产物写 0x0002，读回却坚持 0x0001，
// 症状是「刚 build 出来的 .xbc 立刻被自己拒绝」。

void bc_init(Bytecode* bc) {
    memset(bc, 0, sizeof(*bc));
    bc->version = XBC_VERSION;
}

static void bc_ensure_code(Bytecode* bc, int need) {
    if (bc->code_count + need <= bc->code_cap) return;
    int cap = bc->code_cap ? bc->code_cap : 64;
    while (cap < bc->code_count + need) cap *= 2;
    bc->code = x_realloc(bc->code, (size_t)cap * sizeof(Insn));
    // 位置数组跟指令数组同步扩容，两者必须等长（下标一一对应）。
    bc->pos  = x_realloc(bc->pos,  (size_t)cap * sizeof(SrcPos));
    bc->code_cap = cap;
    bc->pos_cap  = cap;
}

int bc_emit(Bytecode* bc, OpCode op, int32_t a, int32_t b, SrcPos pos) {
    // 操作数收窄到 int16_t（见 x.h 里 Insn 的注释）。这里做硬边界检查：
    // 超限必须明确报错，绝不能静默截断——截断后的跳转目标会指到
    // 完全无关的指令上，那种 bug 表现为「偶尔执行了不该执行的代码」，
    // 比崩溃难查一百倍。
    if (a > BC_OPERAND_MAX || a < BC_OPERAND_MIN ||
        b > BC_OPERAND_MAX || b < BC_OPERAND_MIN) {
        diag_error(pos, "字节码操作数超出 P0 限制（a=%d, b=%d，上限 ±%d）",
                   a, b, BC_OPERAND_MAX);
        diag_note("单条指令的操作数用 16 位存储以把指令压到 8 字节，"
                  "让一个 cache line 能装 8 条指令。");
        diag_help("把过长的函数拆成几个小函数，或减少单个函数里的局部变量与常量数。");
        return -1;
    }

    bc_ensure_code(bc, 1);
    int idx = bc->code_count++;
    bc->code[idx].op   = (uint8_t)op;
    bc->code[idx].a    = (int16_t)a;
    bc->code[idx].b    = (int16_t)b;
    bc->code[idx].c    = 0;    // 没用到的第三操作数一律清零，
                               // 避免读到一个不确定的值（见 x.h 里的说明）
    bc->pos[idx]       = pos;
    return idx;
}

static int bc_add_const(Bytecode* bc, Constant c) {
    if (bc->const_count + 1 > bc->const_cap) {
        int cap = bc->const_cap ? bc->const_cap * 2 : 32;
        bc->consts = x_realloc(bc->consts, (size_t)cap * sizeof(Constant));
        bc->const_cap = cap;
    }
    int idx = bc->const_count++;
    bc->consts[idx] = c;
    return idx;
}

int bc_add_const_i64(Bytecode* bc, int64_t v) {
    // 常量去重：相同值只存一份。
    // 注意：浮点不能用 == 去重（NaN、-0.0），所以浮点不去重。
    for (int i = 0; i < bc->const_count; i++) {
        if (bc->consts[i].kind == CONST_I64 && bc->consts[i].i == v) return i;
    }
    Constant c;
    memset(&c, 0, sizeof(c));
    c.kind = CONST_I64;
    c.i    = v;
    return bc_add_const(bc, c);
}

int bc_add_const_f64(Bytecode* bc, double v) {
    Constant c;
    memset(&c, 0, sizeof(c));
    c.kind = CONST_F64;
    c.f    = v;
    return bc_add_const(bc, c);
}

int bc_add_const_str(Bytecode* bc, const char* s, size_t len) {
    // 字符串去重：内容相同只存一份
    for (int i = 0; i < bc->const_count; i++) {
        if (bc->consts[i].kind == CONST_STR &&
            bc->consts[i].str.len == len &&
            memcmp(bc->consts[i].str.s, s, len) == 0) {
            return i;
        }
    }
    Constant c;
    memset(&c, 0, sizeof(c));
    c.kind    = CONST_STR;
    c.str.s   = x_strndup(s, len);
    c.str.len = len;
    return bc_add_const(bc, c);
}

int bc_add_func(Bytecode* bc, const char* name, int param_count, SrcPos pos) {
    if (bc->func_count + 1 > bc->func_cap) {
        int cap = bc->func_cap ? bc->func_cap * 2 : 8;
        bc->funcs = x_realloc(bc->funcs, (size_t)cap * sizeof(FuncProto));
        bc->func_cap = cap;
    }
    int idx = bc->func_count++;
    FuncProto* f = &bc->funcs[idx];
    memset(f, 0, sizeof(*f));
    f->name        = x_strdup(name);
    f->param_count = param_count;
    f->code_start  = bc->code_count;
    f->code_len    = 0;
    f->pos         = pos;
    return idx;
}

// ============================================================
// 反汇编
// ============================================================
// 两个用途：
//   1. 调试（看编译器生成了什么）
//   2. 差分测试的比对基准（05 §3.1：两档共享同一份字节码）

// 名字表用 switch 而非数组，好处是编译器会检查有没有漏项
//（-Wswitch 会在新增指令但忘记登记时报错，这是 P0 阶段最值钱的检查之一）
const char* bc_op_name(OpCode op) {
    switch (op) {
        case BC_NOP: return "NOP";

        case BC_PUSH_I64: return "PUSH_I64";
        case BC_PUSH_F64: return "PUSH_F64";
        case BC_PUSH_STR: return "PUSH_STR";
        case BC_PUSH_TRUE: return "PUSH_TRUE";
        case BC_PUSH_FALSE: return "PUSH_FALSE";
        case BC_PUSH_NULL: return "PUSH_NULL";
        case BC_POP: return "POP";
        case BC_DUP: return "DUP";

        case BC_LOAD_LOCAL: return "LOAD_LOCAL";
        case BC_STORE_LOCAL: return "STORE_LOCAL";

        case BC_ADD_I64: return "ADD_I64";
        case BC_SUB_I64: return "SUB_I64";
        case BC_MUL_I64: return "MUL_I64";
        case BC_DIV_I64: return "DIV_I64";
        case BC_MOD_I64: return "MOD_I64";
        case BC_NEG_I64: return "NEG_I64";
        case BC_ADD_F64: return "ADD_F64";
        case BC_SUB_F64: return "SUB_F64";
        case BC_MUL_F64: return "MUL_F64";
        case BC_DIV_F64: return "DIV_F64";
        case BC_NEG_F64: return "NEG_F64";

        case BC_ADD_NUM: return "ADD_NUM";
        case BC_SUB_NUM: return "SUB_NUM";
        case BC_MUL_NUM: return "MUL_NUM";
        case BC_DIV_NUM: return "DIV_NUM";
        case BC_ADD_DYN: return "ADD_DYN";

        case BC_EQ: return "EQ";
        case BC_NE: return "NE";

        case BC_LT_I64: return "LT_I64";
        case BC_LE_I64: return "LE_I64";
        case BC_GT_I64: return "GT_I64";
        case BC_GE_I64: return "GE_I64";
        case BC_LT_F64: return "LT_F64";
        case BC_LE_F64: return "LE_F64";
        case BC_GT_F64: return "GT_F64";
        case BC_GE_F64: return "GE_F64";

        case BC_LT_NUM: return "LT_NUM";
        case BC_LE_NUM: return "LE_NUM";
        case BC_GT_NUM: return "GT_NUM";
        case BC_GE_NUM: return "GE_NUM";

        case BC_BAND_I64: return "BAND_I64";
        case BC_BOR_I64: return "BOR_I64";
        case BC_BXOR_I64: return "BXOR_I64";
        case BC_SHL_I64: return "SHL_I64";
        case BC_SHR_I64: return "SHR_I64";
        case BC_BNOT_I64: return "BNOT_I64";

        case BC_NOT: return "NOT";

        case BC_TOBOOL: return "TOBOOL";
        case BC_I64_TO_F64: return "I64_TO_F64";
        case BC_TOINT: return "TOINT";
        case BC_TOFLOAT: return "TOFLOAT";

        case BC_TOSTRING_I64: return "TOSTRING_I64";
        case BC_TOSTRING_F64: return "TOSTRING_F64";
        case BC_TOSTRING_BOOL: return "TOSTRING_BOOL";
        case BC_TOSTRING_NULL: return "TOSTRING_NULL";
        case BC_TOSTRING_ANY: return "TOSTRING_ANY";

        case BC_CONCAT_STR: return "CONCAT_STR";
        case BC_CONCAT_ANY: return "CONCAT_ANY";

        case BC_ABS_I64: return "ABS_I64";
        case BC_ABS_F64: return "ABS_F64";
        case BC_LEN: return "LEN";

        case BC_NEW_ARRAY: return "NEW_ARRAY";
        case BC_INDEX_GET: return "INDEX_GET";
        case BC_INDEX_SET: return "INDEX_SET";

        case BC_NEW_STRUCT: return "NEW_STRUCT";
        case BC_FIELD_GET:  return "FIELD_GET";
        case BC_NEW_CLASS:  return "NEW_CLASS";
        case BC_FIELD_SET:  return "FIELD_SET";
        case BC_VIRT_CALL:  return "VIRT_CALL";

        case BC_NEW_CLASS_NAME: return "NEW_CLASS_NAME";
        case BC_CALL_NAME:      return "CALL_NAME";

        case BC_BUILTIN:   return "BUILTIN";

        case BC_JUMP: return "JUMP";
        case BC_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case BC_JUMP_IF_TRUE: return "JUMP_IF_TRUE";

        case BC_CALL: return "CALL";
        case BC_RETURN: return "RETURN";
        case BC_RETURN_VOID: return "RETURN_VOID";

        case BC_PRINT: return "PRINT";
        case BC_HALT: return "HALT";

        // 超指令：名字里带两个下划线表示「由哪两条合并而来」，
        // 反汇编时一眼能看出它替代了什么。
        case BC_LOAD_LOAD:  return "LOAD_LOAD";
        case BC_DUP_STORE:  return "DUP_STORE";
        case BC_STORE_POP:  return "STORE_POP";
        case BC_LOAD_CONST: return "LOAD_CONST";
        case BC_LT_JUMP_FALSE: return "LT_JUMP_FALSE";
        case BC_DUP_STORE_POP: return "DUP_STORE_POP";
        case BC_ADD_STORE: return "ADD_STORE";
        case BC_MOD_STORE: return "MOD_STORE";

        case BC_LT_CONST_JUMP_FALSE:  return "LT_CONST_JUMP_FALSE";
        case BC_ADD_CONST_STORE:      return "ADD_CONST_STORE";
        case BC_ADD_CONST_STORE_JUMP: return "ADD_CONST_STORE_JUMP";
        case BC_SUB_STORE: return "SUB_STORE";
        case BC_SUB_CONST_STORE: return "SUB_CONST_STORE";
        case BC_SUB_CONST_STORE_JUMP: return "SUB_CONST_STORE_JUMP";
        case BC_ARITH_STORE: return "ARITH_STORE";
        case BC_ARITH_CONST_STORE: return "ARITH_CONST_STORE";
        case BC_MOV_LOCAL: return "MOV_LOCAL";
        case BC_NE_CONST_JUMP_FALSE: return "NE_CONST_JUMP_FALSE";
        case BC_NE_JUMP_FALSE: return "NE_JUMP_FALSE";

        case BC_COUNT: break;
    }
    return "???";
}

// 整数运算码的符号表示，供反汇编显示。
//
// 单独一个函数而不是散在 disasm 里写 switch：AOT 后端也要按运算码
// 生成 `+` / `<<` 这类 C 算子，两处必须一致。将来加运算时只改这里
// 和 XArithOp 枚举。
const char* x_arith_op_symbol(XArithOp op) {
    switch (op) {
        case XOP_ADD:  return "+";
        case XOP_SUB:  return "-";
        case XOP_MUL:  return "*";
        case XOP_DIV:  return "/";
        case XOP_MOD:  return "%";
        case XOP_BAND: return "&";
        case XOP_BOR:  return "|";
        case XOP_BXOR: return "^";
        case XOP_SHL:  return "<<";
        case XOP_SHR:  return ">>";
        default:       return "?";
    }
}

// 该指令是否带操作数（影响反汇编格式）
static bool has_operand(OpCode op) {
    switch (op) {
        case BC_PUSH_I64:
        case BC_PUSH_F64:
        case BC_PUSH_STR:
        case BC_LOAD_LOCAL:
        case BC_STORE_LOCAL:
        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_CALL:
        case BC_LOAD_LOAD:      // 显示两个操作数（虽然格式上只显示 a，b 在注释里）
        case BC_DUP_STORE:
        case BC_STORE_POP:
        case BC_LOAD_CONST:
        case BC_LT_JUMP_FALSE:  // a = 跳转目标
        case BC_DUP_STORE_POP:
        case BC_ADD_STORE:
        case BC_MOD_STORE:
        case BC_LT_CONST_JUMP_FALSE:
        case BC_ADD_CONST_STORE:
        case BC_ADD_CONST_STORE_JUMP:
        // 减法组（v0.21 补）与运算族泛化。之前漏了 SUB 那三条，
        // 症状是反汇编里 `SUB_CONST_STORE` 后面什么都不显示 ——
        // 而槽位号正是排查「存错变量」时唯一要看的东西。
        case BC_SUB_STORE:
        case BC_SUB_CONST_STORE:
        case BC_SUB_CONST_STORE_JUMP:
        case BC_ARITH_STORE:
        case BC_ARITH_CONST_STORE:
        // 第五批（v0.22）：变量拷贝与比较跳转。这两条都有操作数，
        // 漏在这里的症状与上次 SUB 组完全一样 —— 反汇编里操作数
        // 一个都不显示，排查「拷错槽位 / 常量索引错位」时完全失明。
        case BC_MOV_LOCAL:
        case BC_NE_CONST_JUMP_FALSE:
        case BC_NE_JUMP_FALSE:
        case BC_BUILTIN:
            return true;
        default:
            return false;
    }
}

// 栈深变化：正数表示净压入，负数表示净弹出。
// P1 用它做编译期栈平衡校验（07 §5「栈深度静态可算」）；
// P0 只用于调试输出，不参与判定。
int bc_op_stack_effect(OpCode op) {
    switch (op) {
        case BC_NOP:
        case BC_JUMP:
        case BC_HALT:
            return 0;

        case BC_PUSH_I64:
        case BC_PUSH_F64:
        case BC_PUSH_STR:
        case BC_PUSH_TRUE:
        case BC_PUSH_FALSE:
        case BC_PUSH_NULL:
        case BC_LOAD_LOCAL:
            return 1;

        case BC_POP:
        case BC_STORE_LOCAL:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_RETURN:
        case BC_RETURN_VOID:
        case BC_PRINT:
            return -1;

        case BC_DUP:
            return 1;

        // 超指令的栈效应必须与它替代的那两条之和完全相同，
        // 否则静态栈平衡检查（P1 启用）会误报。
        case BC_LOAD_LOAD:      // LOAD +1; LOAD +1
            return 2;
        case BC_DUP_STORE:      // DUP +1; STORE -1
            return 0;
        case BC_STORE_POP:      // STORE -1; POP -1
            return -2;
        case BC_LOAD_CONST:     // LOAD +1; PUSH +1
            return 2;
        case BC_LT_JUMP_FALSE:  // LT -1; JUMP_IF_FALSE -1
            return -2;
        case BC_DUP_STORE_POP:  // DUP_STORE 0; POP -1
            return -1;
        case BC_ADD_STORE:      // ADD -1; DUP_STORE_POP -1
            return -2;
        case BC_MOD_STORE:      // MOD -1; DUP_STORE_POP -1
            return -2;

        // 第二批超指令：栈流量同样抵消干净
        case BC_LT_CONST_JUMP_FALSE:  // LOAD_CONST +2; LT_JUMP_FALSE -2
            return 0;
        case BC_ADD_CONST_STORE:      // LOAD_CONST +2; ADD_STORE -2
            return 0;
        case BC_ADD_CONST_STORE_JUMP: // ADD_CONST_STORE 0; JUMP 0
            return 0;

        // 减法组：与 ADD 组逐项对称
        case BC_SUB_STORE:            // SUB -1; DUP_STORE_POP -1
            return -2;
        case BC_SUB_CONST_STORE:      // LOAD_CONST +2; SUB_STORE -2
            return 0;
        case BC_SUB_CONST_STORE_JUMP: // SUB_CONST_STORE 0; JUMP 0
            return 0;

        // 运算族泛化：与 ADD_STORE / LOAD_CONST+ADD_STORE 同构
        case BC_ARITH_STORE:          // <OP> -1; DUP_STORE_POP -1
            return -2;
        case BC_ARITH_CONST_STORE:    // LOAD_CONST +2; <OP> -1; DUP_STORE_POP -1
            return 0;

        // 第五批：变量拷贝 / 比较跳转
        case BC_MOV_LOCAL:            // LOAD +1; STORE -1
            return 0;
        case BC_NE_CONST_JUMP_FALSE:  // LOAD_CONST +2; NE -1; JUMP_IF_FALSE -1
            return 0;
        case BC_NE_JUMP_FALSE:        // LOAD_LOAD +2; NE -1; JUMP_IF_FALSE -1
            return 0;

        case BC_NOT:
        case BC_NEG_I64:
        case BC_NEG_F64:
        case BC_BNOT_I64:
        case BC_TOBOOL:
        case BC_I64_TO_F64:
        case BC_TOINT:
        case BC_TOFLOAT:
        case BC_TOSTRING_I64:
        case BC_TOSTRING_F64:
        case BC_TOSTRING_BOOL:
        case BC_TOSTRING_NULL:
        case BC_TOSTRING_ANY:
        case BC_ABS_I64:
        case BC_ABS_F64:
            return 0;

        // 长度查询：入参出参各一个，净变化 0
        case BC_LEN:
            return 0;

        // 建数组：弹长度、弹填充值，压数组
        case BC_NEW_ARRAY:
            return -1;
        // 读：弹容器、弹下标，压元素
        case BC_INDEX_GET:
            return -1;
        // 写：弹值、弹下标、弹容器，压回容器（赋值表达式的值）
        case BC_INDEX_SET:
            return -2;

        // 建结构体：弹 b 个字段值，压对象。
        // 单参数版本看不到 b，按 1 个字段保守估 —— 真正用它做
        // 栈平衡分析的地方必须走 bc_op_stack_effect_at。
        case BC_NEW_STRUCT:
            return 1 - 1;   // 默认按 1 字段算，at 版本覆盖
        // 读字段：弹对象，压字段值，净 0
        case BC_FIELD_GET:
            return 0;

        // ---- 类（v0.15）----
        // 实例化：弹 b 个构造实参，压实例。at 版本按 b 精确算。
        case BC_NEW_CLASS:
            return 1 - 1;
        // 字段写：弹值、弹对象，压回值，净 -1
        case BC_FIELD_SET:
            return -1;
        // 虚调用：弹 b 个实参（含 self），压返回值，净 1 - b。
        // at 版本按 b 精确算 —— 默认按 1 个实参（只有 self）估。
        case BC_VIRT_CALL:
            return 1 - 1;

        // ---- 动态调用（v0.20）----
        // 按名实例化：弹 [类名, 实参数组?]，压实例。数组按元素展开。
        case BC_NEW_CLASS_NAME:
            return 1;
        // 按名虚调用：弹 [接收者, 方法名, 实参数组?]，压返回值。
        // 数组按元素展开，展开前后的净变化都是 0。
        case BC_CALL_NAME:
            return 0;

        // 平台内建的栈效应取决于具体是哪个内建、几个参数，
        // 单看 opcode 算不出来。真正需要的地方用
        // bc_op_stack_effect_at（它能看到 b = argc）：
        // 弹 argc 个参数，压 1 个返回值。
        case BC_BUILTIN:
            return -1;   // 默认按 2 参数算；实际由 at 版本覆盖

        default:
            // 全部二元运算：弹 2 压 1
            return -1;
    }
}

// 带上下文的栈效应。只有少数指令需要看操作数或函数表。
//
// CALL 是这里唯一的特例，但它是必须的：main 里 `f(3)` 后面接 PRINT，
// 如果 CALL 被当成 -1，main 的栈深会算成负数，静态检查直接判定
// 「栈不平衡」——而用户代码完全正确。这个误报最坑人的地方是
// 错误信息指向「超指令 bug」，把人引向完全错误的方向。
// 平台内建的名字 → 编号。
//
// 这张表是「语言层叫法」与「运行时编号」的唯一对照点。
// 编译器用它把 `winOpen(...)` 转成 BC_BUILTIN 的 a 操作数；
// 反汇编用它把编号转回可读名字。
//
// 表里有名字但运行时没实现（比如类 Unix 上的窗口函数）不会
// 编译失败，而是运行到那一步时返回一个明确的错误 —— 见
// builtin_name() 的说明。
typedef struct {
    const char* name;
    XBuiltin    id;
    int         argc;
    const char* ret;      // 返回类型名，供反汇编注释
} BuiltinInfo;

static const BuiltinInfo g_builtins[] = {
    { "winOpen",        XBI_WIN_OPEN,         3, "Bool"   },
    { "winClose",       XBI_WIN_CLOSE,        0, "Null"   },
    { "winShouldClose", XBI_WIN_SHOULD_CLOSE, 0, "Bool"   },
    { "winBeginFrame",  XBI_WIN_BEGIN_FRAME,  0, "Null"   },
    { "winEndFrame",    XBI_WIN_END_FRAME,    0, "Null"   },
    { "winFillRect",    XBI_WIN_FILL_RECT,    5, "Null"   },
    { "winBorderRect",  XBI_WIN_BORDER_RECT,  6, "Null"   },
    { "winText",        XBI_WIN_TEXT,         4, "Null"   },
    { "winTextWidth",   XBI_WIN_TEXT_WIDTH,   1, "Int"    },
    { "winTextHeight",  XBI_WIN_TEXT_HEIGHT,  0, "Int"    },
    { "winPollClick",   XBI_WIN_POLL_CLICK,   0, "Array"  },
    { "winMouseX",      XBI_WIN_MOUSE_X,      0, "Int"    },
    { "winMouseY",      XBI_WIN_MOUSE_Y,      0, "Int"    },
    { "winMouseDown",   XBI_WIN_MOUSE_DOWN,   1, "Bool"   },
    { "winTime",        XBI_WIN_TIME,         0, "Float"  },
    { "winSleep",       XBI_WIN_SLEEP,        1, "Null"   },
    { "winScreenshot",  XBI_WIN_SCREENSHOT,   1, "Bool"   },

    // ---- 形状 / 样式（v0.17）----
    //
    // 这批的名字前缀全部沿用 win*，因为它们和 winFillRect 一样属于
    // 窗口运行时。argc 必须准确 —— 编译器按它做参数个数检查，
    // 少写一个数字不会报错，只会在调用点报「参数个数不符」，
    // 而那个报错会把人引向「我调用写错了」而不是「内建表写错了」。
    { "winFillRound",   XBI_WIN_FILL_ROUND,    6, "Null"   },
    { "winBorderRound", XBI_WIN_BORDER_ROUND,  7, "Null"   },
    { "winEllipse",     XBI_WIN_ELLIPSE,       5, "Null"   },
    { "winPath",        XBI_WIN_PATH,          2, "Null"   },
    { "winGradient",    XBI_WIN_GRADIENT,      7, "Null"   },
    { "winPushTransform", XBI_WIN_PUSH_TRANSFORM, 4, "Null"},
    { "winPopTransform",  XBI_WIN_POP_TRANSFORM,  0, "Null"},
    { "winAlpha",       XBI_WIN_ALPHA,         1, "Null"   },
    { "winPopAlpha",    XBI_WIN_POP_ALPHA,     0, "Null"   },
    { "winClip",        XBI_WIN_CLIP,          4, "Null"   },
    { "winUnclip",      XBI_WIN_UNCLIP,        0, "Null"   },
    { "winTextRot",     XBI_WIN_TEXT_ROT,      5, "Null"   },

    // ---- 数组 ----
    { "arrPush",     XBI_ARR_PUSH,     2, "Int"    },
    { "arrPop",      XBI_ARR_POP,      1, "Unknown"},
    { "arrInsert",   XBI_ARR_INSERT,   3, "Int"    },
    { "arrRemove",   XBI_ARR_REMOVE,   2, "Unknown"},
    { "arrSlice",    XBI_ARR_SLICE,    3, "Array"  },
    { "arrConcat",   XBI_ARR_CONCAT,   2, "Array"  },
    { "arrReverse",  XBI_ARR_REVERSE,  1, "Null"   },
    { "arrIndexOf",  XBI_ARR_INDEX_OF, 2, "Int"    },
    { "arrContains", XBI_ARR_CONTAINS, 2, "Bool"   },
    { "arrFill",     XBI_ARR_FILL,     2, "Null"   },
    { "arrSort",     XBI_ARR_SORT,     1, "Null"   },
    { "arrCopy",     XBI_ARR_COPY,     1, "Array"  },

    // ---- 字符串 ----
    { "strAt",       XBI_STR_AT,        2, "String" },
    { "strSub",      XBI_STR_SUB,       3, "String" },
    { "strFind",     XBI_STR_FIND,      2, "Int"    },
    { "strFindFrom", XBI_STR_FIND_FROM, 3, "Int"    },
    { "strSplit",    XBI_STR_SPLIT,     2, "Array"  },
    { "strJoin",     XBI_STR_JOIN,      2, "String" },
    { "strUpper",    XBI_STR_UPPER,     1, "String" },
    { "strLower",    XBI_STR_LOWER,     1, "String" },
    { "strTrim",     XBI_STR_TRIM,      1, "String" },
    { "strReplace",  XBI_STR_REPLACE,   3, "String" },
    { "strStarts",   XBI_STR_STARTS,    2, "Bool"   },
    { "strEnds",     XBI_STR_ENDS,      2, "Bool"   },
    { "strRepeat",   XBI_STR_REPEAT,    2, "String" },
    { "strCode",     XBI_STR_CODE,      1, "Int"    },
    { "strFromCode", XBI_STR_FROM_CODE, 1, "String" },
    { "strParseInt", XBI_STR_PARSE_INT, 1, "Int"    },
    { "strParseFloat", XBI_STR_PARSE_FLOAT, 1, "Float" },

    // ---- 数学 ----
    // 这里 argc 填 -1：真实参数个数编码在指令的 a 操作数里
    // （见 x.h 的 xmath_enc）。填固定值会让编译器按错的个数弹参，
    // 导致栈错位 —— 而栈错位的症状是「后面几十条指令全算错」。
    { "math",   XBI_MATH,   -1, "Unknown" },
    { "random", XBI_RANDOM,  0, "Float"   },
    { "randomSeed", XBI_RANDOM_SEED, 1, "Null" },
    { "fail",       XBI_FAIL,        1, "Null" },
    { "typeOf",     XBI_TYPEOF,      1, "String" },

    // ---- 文件 ----
    { "fsRead",     XBI_FS_READ,     1, "String" },
    { "fsReadOr",   XBI_FS_READ_OR,  2, "String" },
    { "fsWrite",    XBI_FS_WRITE,    2, "Bool"   },
    { "fsAppend",   XBI_FS_APPEND,   2, "Bool"   },
    { "fsExists",   XBI_FS_EXISTS,   1, "Bool"   },
    { "fsIsDir",    XBI_FS_IS_DIR,   1, "Bool"   },
    { "fsRemove",   XBI_FS_REMOVE,   1, "Bool"   },
    { "fsSize",     XBI_FS_SIZE,     1, "Int"    },
    { "fsList",     XBI_FS_LIST,     1, "Array"  },
    { "fsMkdir",    XBI_FS_MKDIR,    1, "Bool"   },
    { "fsReplace",  XBI_FS_REPLACE,  2, "Bool"   },

    // ---- TCP ----
    { "netListen",  XBI_NET_LISTEN,  2, "Int"    },
    { "netAccept",  XBI_NET_ACCEPT,  1, "Int"    },
    { "netRecv",    XBI_NET_RECV,    1, "String" },
    { "netSend",    XBI_NET_SEND,    2, "Int"    },
    { "netClose",   XBI_NET_CLOSE,   1, "Null"   },
    { "netConnect", XBI_NET_CONNECT, 2, "Int"    },
    { "netPort",    XBI_NET_PORT,    1, "Int"    },
    { "netPeer",    XBI_NET_PEER,    1, "String" },
    { "netError",   XBI_NET_ERR,     0, "String" },
    { "netPending", XBI_NET_PENDING, 1, "Bool"   },

    // ---- 通用 ----
    //
    // 与 winSleep 的区别只在「算不算 GUI」。把 sleep 放这里，
    // 纯控制台程序就能既睡一会儿又不触发窗口警告。
    { "sleep",      XBI_SLEEP,       1, "Null"   },

    // ---- 时间 ----
    //
    // 与 winTime 的区别只在「算不算 GUI」，理由见 x.h 的枚举注释。
    // 返回 Unix 毫秒/秒数，供耗时统计和时间戳使用。
    { "nowMillis",  XBI_NOW_MILLIS,  0, "Int"    },
    { "nowSeconds", XBI_NOW_SECONDS, 0, "Int"    },
    { "monoMillis", XBI_MONO_MILLIS, 0, "Int"    },

    // ---- 字节 ----
    //
    // 字节 ↔ 字符串的无损互转。理由见 x.h 的说明：没有这一对，
    // 任何按字节处理的协议（URL 编码、二进制字段、base64）都会
    // 在非 ASCII 输入上出错。
    { "strFromBytes", XBI_STR_FROM_BYTES, 1, "String" },
    { "strBytes",     XBI_STR_BYTES,      1, "Array"  },

    // ---- 反射 ----
    //
    // 命名前缀统一用 class* / field* / methodHas，一眼能看出属于
    // 反射而不是普通运算。返回类型都刻意选成 String / Array / Bool
    // 这类**普通值**，反射结果才能继续用语言自身的能力处理
    // （遍历、拼字符串、比较），不需要第二套「元对象」类型。
    { "className",    XBI_CLASS_NAME,    1, "String" },
    { "classParent",  XBI_CLASS_PARENT,  1, "String" },
    { "classIs",      XBI_CLASS_IS,      2, "Bool"   },
    { "classFields",  XBI_CLASS_FIELDS,  1, "Array"  },
    { "classMethods", XBI_CLASS_METHODS, 1, "Array"  },
    { "fieldGet",     XBI_FIELD_GET,     2, "Unknown" },
    { "fieldSet",     XBI_FIELD_SET,     3, "Null"   },
    { "fieldHas",     XBI_FIELD_HAS,     2, "Bool"   },
    { "methodHas",    XBI_METHOD_HAS,    2, "Bool"   },

    // ---- 注解 ----
    //
    // 前缀 anno*。四个函数覆盖「有没有 / 值是什么 / 键值对 / 全列」，
    // 分别对应四种真实用法：
    //   annoHas     条件分支（有没有 @Deprecated）
    //   annoGet     按位置取实参（@Route("/a", "GET") 里的第一个）
    //   annoGetKV   按名字取实参（@Route(path = "/a")）
    //   annoAll     框架扫描时列出目标上所有注解
    //
    // ── member 这一位为什么不能省 ──────────────────────────
    //
    // 没有它，`annoGet(u, "field", "Column")` 在类里有三个字段带
    // @Column 时无法回答「哪一个」。第一版就是这样，实测返回空数组
    // —— 而空数组看起来像「这个注解没有实参」，静默错误。
    //
    // 类目标传 "" 占位。四个函数参数个数一致，弹栈代码只有一套。
    { "annoHas",      XBI_ANNO_HAS,      4, "Bool"  },
    { "annoGet",      XBI_ANNO_GET,      4, "Array" },
    { "annoGetKV",    XBI_ANNO_GET_KV,   4, "Array" },
    { "annoAll",      XBI_ANNO_ALL,      3, "Array" },

    // ---- 动态调用辅助（v0.20，框架支持）----
    //
    // classAll 是组件扫描的第一步：框架拿到全部类名后，用 anno*
    // 查询逐个过滤（它们接受类名字符串），命中的才 classNew。
    // 没有这一步，扫描就得「先实例化所有类再查注解」——构造函数
    // 会带着错误的参数跑起来，副作用不可控。
    { "classAll",     XBI_CLASS_ALL,     0, "Array" },
    { "methodParams", XBI_METHOD_PARAMS, 2, "Array" },

    // ---- 编辑器支持（v0.21）----
    // 键码常量由 stdlib/text/keys.xle 定义，值与 Win32 VK_* 一致。
    { "winPollKey",      XBI_WIN_POLL_KEY,      0, "Array"  },
    { "winPollWheel",    XBI_WIN_POLL_WHEEL,    0, "Int"    },
    { "winKeyDown",      XBI_WIN_KEY_DOWN,      1, "Bool"   },
    { "winWidth",        XBI_WIN_WIDTH,         0, "Int"    },
    { "winHeight",       XBI_WIN_HEIGHT,        0, "Int"    },
    { "winSetTitle",     XBI_WIN_SET_TITLE,     1, "Null"   },
    { "winSetFont",      XBI_WIN_SET_FONT,      3, "Bool"   },
    { "winSetCursor",    XBI_WIN_SET_CURSOR,    1, "Null"   },
    { "winClipGet",      XBI_WIN_CLIP_GET,      0, "String" },
    { "winClipSet",      XBI_WIN_CLIP_SET,      1, "Bool"   },
    { "winOpenFile",     XBI_WIN_OPEN_FILE,     3, "String" },
    { "winSaveFile",     XBI_WIN_SAVE_FILE,     3, "String" },
    { "winMessage",      XBI_WIN_MESSAGE,       3, "Int"    },
    { "winSetResizable", XBI_WIN_SET_RESIZABLE, 1, "Null"   },

    // ---- 裸机（kernel profile 专用）----
    //
    // ── 名字为什么是 memRead8 / portOut8 这种「动词+宽度」形态 ──
    //
    // 内核代码里最危险的事是把宽度写错：往 32 位寄存器地址写 8 位，
    // 或者读端口用错宽度 —— 两者都不报错，只是行为莫名其妙（前者
    // 会静默截断，后者读到的是别的设备）。所以宽度必须是**函数名
    // 的一部分**，让写错在阅读时就能发现，而不是靠记住某个参数。
    //
    // 这也解释了为什么不做 memRead(addr, width)：那个 width 参数
    // 在调用点永远是个字面量，而字面量参数是「可以用名字消除的
    // 参数」—— 消除之后，宽度错误在代码审查里一眼可见。
    //
    // ── 返回类型全部是 Int，没有 Bool ──────────────────────
    //
    // 读端口/内存永远不会「失败」：地址非法会导致 CPU 异常（#GP/#PF），
    // 那由内核的中断处理决定怎么反应，不是返回值能表达的。硬造一个
    // Bool 返回值会让调用方写出 `if (!portIn8(0x3F8))` 这种永远为假的
    // 检查 —— 比不检查更糟，因为它看起来像做了错误处理。
    { "memRead8",   XBI_MEM_READ8,   1, "Int"  },
    { "memRead16",  XBI_MEM_READ16,  1, "Int"  },
    { "memRead32",  XBI_MEM_READ32,  1, "Int"  },
    { "memWrite8",  XBI_MEM_WRITE8,  2, "Null" },
    { "memWrite16", XBI_MEM_WRITE16, 2, "Null" },
    { "memWrite32", XBI_MEM_WRITE32, 2, "Null" },
    { "portIn8",    XBI_PORT_IN8,    1, "Int"  },
    { "portIn16",   XBI_PORT_IN16,   1, "Int"  },
    { "portOut8",   XBI_PORT_OUT8,   2, "Null" },
    { "portOut16",  XBI_PORT_OUT16,  2, "Null" },
    { "cpuHlt",     XBI_CPU_HLT,     0, "Null" },
    { "cpuCli",     XBI_CPU_CLI,     0, "Null" },
    { "cpuSti",     XBI_CPU_STI,     0, "Null" },
    { "cpuInvlpg",  XBI_CPU_INVLPG,  1, "Null" },
};

#define BUILTIN_COUNT ((int)(sizeof(g_builtins) / sizeof(g_builtins[0])))

int bc_builtin_count(void) { return BUILTIN_COUNT; }

const char* bc_builtin_name(int i) {
    if (i < 0 || i >= BUILTIN_COUNT) return NULL;
    return g_builtins[i].name;
}

XBuiltin bc_builtin_id(int i) {
    if (i < 0 || i >= BUILTIN_COUNT) return XBI_NONE;
    return g_builtins[i].id;
}

int bc_builtin_argc(int i) {
    if (i < 0 || i >= BUILTIN_COUNT) return -1;
    return g_builtins[i].argc;
}

// 按 XBI_* 编号反查名字（bc_builtin_name 是按表下标查，两者不同）。
//
// 反汇编与报错都要「id → 名字」，而 id 是枚举值、表下标是数组位置，
// 两者只有在表恰好与枚举同序时才相等 —— 那个巧合不该被依赖。
const char* bc_builtin_name_of(XBuiltin id) {
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (g_builtins[i].id == id) return g_builtins[i].name;
    }
    return "?";
}

const char* bc_builtin_ret(int i) {
    if (i < 0 || i >= BUILTIN_COUNT) return "Unknown";
    return g_builtins[i].ret;
}

// 名字 → 表内下标（不是 XBuiltin，因为需要 argc/ret）。
// 找不到返回 -1。
int bc_builtin_find(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(g_builtins[i].name, name) == 0) return i;
    }
    return -1;
}

int bc_op_stack_effect_at(Bytecode* bc, const Insn* in) {
    if (in->op == BC_BUILTIN) {
        // 弹 argc 个参数、压 1 个返回值
        return 1 - (int)in->b;
    }
    if (in->op == BC_CALL) {
        int argc = 0;
        if (in->a >= 0 && in->a < bc->func_count) {
            argc = bc->funcs[in->a].param_count;
        }
        // 弹出 argc 个实参，压入 1 个返回值
        return 1 - argc;
    }
    if (in->op == BC_NEW_STRUCT) {
        // 弹出 b 个字段值，压入 1 个对象
        return 1 - (int)in->b;
    }
    if (in->op == BC_NEW_CLASS) {
        // 弹出 b 个构造实参，压入 1 个实例
        return 1 - (int)in->b;
    }
    if (in->op == BC_VIRT_CALL) {
        // 弹出 b 个实参（含 self），压入 1 个返回值
        return 1 - (int)in->b;
    }
    if (in->op == BC_NEW_CLASS_NAME) {
        // 弹出类名 + 实参数组（可选），压入 1 个实例
        return 1;
    }
    if (in->op == BC_CALL_NAME) {
        // 弹出接收者 + 方法名 + 实参数组（可选），压入 1 个返回值
        return 0;
    }
    return bc_op_stack_effect((OpCode)in->op);
}

// ============================================================
// 结构体类型表
// ============================================================

int bc_add_struct(Bytecode* bc, const char* name,
                  char** field_names, char** field_types, int count) {
    if (bc->struct_count == bc->struct_cap) {
        int cap = bc->struct_cap ? bc->struct_cap * 2 : 8;
        bc->structs = x_realloc(bc->structs, (size_t)cap * sizeof(StructType));
        bc->struct_cap = cap;
    }
    StructType* st = &bc->structs[bc->struct_count];
    st->name        = x_strdup(name);
    st->field_count = count;
    st->field_names = count ? x_alloc((size_t)count * sizeof(char*)) : NULL;
    st->field_types = count ? x_alloc((size_t)count * sizeof(char*)) : NULL;
    for (int i = 0; i < count; i++) {
        st->field_names[i] = field_names[i] ? x_strdup(field_names[i]) : NULL;
        st->field_types[i] = field_types[i] ? x_strdup(field_types[i]) : NULL;
    }
    return bc->struct_count++;
}

int bc_find_struct(Bytecode* bc, const char* name) {
    if (!name) return -1;
    for (int i = 0; i < bc->struct_count; i++) {
        if (bc->structs[i].name && strcmp(bc->structs[i].name, name) == 0) return i;
    }
    return -1;
}

int bc_find_field(Bytecode* bc, int type_idx, const char* field) {
    if (type_idx < 0 || type_idx >= bc->struct_count) return -1;
    StructType* st = &bc->structs[type_idx];
    for (int i = 0; i < st->field_count; i++) {
        if (st->field_names[i] && strcmp(st->field_names[i], field) == 0) return i;
    }
    return -1;
}

// ============================================================
// 类表（v0.15）
// ============================================================

int bc_add_class(Bytecode* bc, const char* name) {
    if (bc->class_count == bc->class_cap) {
        int cap = bc->class_cap ? bc->class_cap * 2 : 8;
        bc->classes = x_realloc(bc->classes, (size_t)cap * sizeof(ClassType));
        bc->class_cap = cap;
    }
    ClassType* ct = &bc->classes[bc->class_count];
    memset(ct, 0, sizeof(*ct));
    ct->name          = x_strdup(name);
    ct->mangle        = x_strdup(name);
    ct->parent_idx    = -1;
    ct->ctor_fn       = -1;
    ct->vtable        = NULL;
    ct->vtable_len    = 0;
    ct->field_names   = NULL;
    ct->field_types   = NULL;
    ct->field_vis     = NULL;
    ct->field_final   = NULL;
    ct->field_count   = 0;
    ct->own_field_start = 0;
    ct->iface_idx     = NULL;
    ct->iface_count   = 0;
    ct->is_abstract   = false;
    ct->is_interface  = false;
    ct->is_final      = false;
    return bc->class_count++;
}

int bc_find_class(Bytecode* bc, const char* name) {
    if (!name) return -1;
    for (int i = 0; i < bc->class_count; i++) {
        if (bc->classes[i].name && strcmp(bc->classes[i].name, name) == 0) return i;
    }
    return -1;
}

// 全局方法槽。
//
// 同名方法在所有类里共用一个槽号 —— 这是「子类覆写自动生效」
// 的全部机制。见 x.h 里 ClassType 的注释。
int bc_method_slot(Bytecode* bc, const char* name) {
    int found = bc_find_method_slot(bc, name);
    if (found >= 0) return found;

    if (bc->method_slot_count == bc->method_slot_cap) {
        int cap = bc->method_slot_cap ? bc->method_slot_cap * 2 : 16;
        bc->method_slots = x_realloc(bc->method_slots,
                                     (size_t)cap * sizeof(MethodSlot));
        bc->method_slot_cap = cap;
    }
    int slot = bc->method_slot_count++;
    bc->method_slots[slot].name = x_strdup(name);
    return slot;
}

int bc_find_method_slot(Bytecode* bc, const char* name) {
    if (!name) return -1;
    for (int i = 0; i < bc->method_slot_count; i++) {
        if (bc->method_slots[i].name &&
            strcmp(bc->method_slots[i].name, name) == 0) return i;
    }
    return -1;
}

// 在全量字段表里找字段（含继承字段）。
//
// 为什么查的是「全量」而不是「本类自有」：子类方法里写
// `self.parentField` 必须能访问到父类字段，而方法体是按子类
// 编译的 —— 那时只看得见子类自己的声明。全量表在编译期就把
// 父类字段合并进来了，所以这里一次查表就够。
int bc_find_class_field(Bytecode* bc, int cls_idx, const char* field) {
    if (cls_idx < 0 || cls_idx >= bc->class_count || !field) return -1;
    ClassType* ct = &bc->classes[cls_idx];
    for (int i = 0; i < ct->field_count; i++) {
        if (ct->field_names[i] && strcmp(ct->field_names[i], field) == 0) return i;
    }
    return -1;
}

// sub 是否是 base 的子孙（含自身）。
//
// 多态的合法性检查全靠它：`Base b = new Derived()` 合法当且仅当
// is_a(Derived, Base)。参数传递、返回值、数组元素赋值同理。
//
// 用父链上溯而不是缓存一个祖先集合：类数量在单个模块里是几十级
// 的量级，上溯链长度也不过三五层，比维护集合便宜得多。
bool bc_class_is_a(Bytecode* bc, int sub_idx, int base_idx) {
    if (sub_idx < 0 || base_idx < 0) return false;
    if (sub_idx >= bc->class_count || base_idx >= bc->class_count) return false;

    int cur = sub_idx;
    // 上界防护：环状继承在编译期就该被拒绝，但这里是运行期/编译期
    // 共用的函数，多一层计数保护比无限循环好 —— 无限循环的症状是
    // 编译器自己卡死，完全没有诊断信息。
    int guard = 0;
    while (cur >= 0 && guard <= bc->class_count) {
        if (cur == base_idx) return true;
        // 接口匹配：实现了某接口的类，也是那个接口的「子孙」。
        ClassType* ct = &bc->classes[cur];
        for (int i = 0; i < ct->iface_count; i++) {
            if (ct->iface_idx[i] == base_idx) return true;
        }
        cur = ct->parent_idx;
        guard++;
    }
    return false;
}

// ============================================================
// 注解表（v0.16）
// ============================================================

// 把一条 AST 注解拍平进全局表。
//
// 字符串一律 dup 进堆，因为 AST 的字符串活在一个 Arena 里，而注解
// 要跟着 Bytecode 活到程序结束 —— 两者生命周期不同。省下这一次
// dup 的代价是「编译完释放 Arena 之后注解全变成悬空指针」，而症状
// 是打印注解名时出现乱码，不是崩溃。
void bc_add_anno(Bytecode* bc, AnnoTarget target, int class_idx,
                 const char* member, const Annotation* a) {
    if (!bc || !a) return;
    if (bc->anno_count == bc->anno_cap) {
        int nc = bc->anno_cap ? bc->anno_cap * 2 : 16;
        AnnoEntry* ne = x_alloc((size_t)nc * sizeof(AnnoEntry));
        if (bc->annos) memcpy(ne, bc->annos,
                              (size_t)bc->anno_count * sizeof(AnnoEntry));
        bc->annos = ne;
        bc->anno_cap = nc;
    }
    AnnoEntry* e = &bc->annos[bc->anno_count++];
    e->target    = target;
    e->class_idx = class_idx;
    e->member    = member ? x_strdup(member) : NULL;
    e->name      = a->name ? x_strdup(a->name) : x_strdup("");
    e->arg_count = a->arg_count;
    if (a->arg_count > 0) {
        e->arg_keys = x_alloc((size_t)a->arg_count * sizeof(char*));
        e->arg_vals = x_alloc((size_t)a->arg_count * sizeof(char*));
        for (int i = 0; i < a->arg_count; i++) {
            e->arg_keys[i] = a->arg_keys && a->arg_keys[i]
                             ? x_strdup(a->arg_keys[i]) : NULL;
            e->arg_vals[i] = a->arg_vals && a->arg_vals[i]
                             ? x_strdup(a->arg_vals[i]) : x_strdup("");
        }
    } else {
        e->arg_keys = NULL;
        e->arg_vals = NULL;
    }
}

// 查一条注解。
//
// ── 为什么继承来的注解不沿父链找 ────────────────────────────
//
// 字段/方法的注解是**声明点**的属性，不是可继承的行为。子类没有
// 重新标注就意味着「它没有这个注解」—— 这与 `@Deprecated` 这类
// 语义是一致的：父类的旧方法废弃不代表子类重写的方法也废弃。
//
// 类注解同理但更微妙：`@Entity` 标在父类上，子类通常也希望是
// 实体。但那是 ORM 框架该定义的事，语言层不能替它决定 —— 框架
// 想看父类注解，它可以自己沿 classParent 往上走。语言只回答
// 「这个类自己被标了什么」，一个精确而窄的答案。
AnnoEntry* bc_find_anno(Bytecode* bc, AnnoTarget target, int class_idx,
                        const char* member, const char* name) {
    if (!bc || !name) return NULL;
    for (int i = 0; i < bc->anno_count; i++) {
        AnnoEntry* e = &bc->annos[i];
        if (e->target != target) continue;
        if (e->class_idx != class_idx) continue;
        // member 的比较：类注解两边都是 NULL 才算匹配；字段/方法注解
        // 必须名字相同。写成 `strcmp` 前先判 NULL 是因为类注解的
        // member 就是 NULL，直接 strcmp 会崩。
        if (member || e->member) {
            if (!member || !e->member) continue;
            if (strcmp(e->member, member) != 0) continue;
        }
        if (strcmp(e->name, name) != 0) continue;
        return e;
    }
    return NULL;
}

// ============================================================
// 能力探测
// ============================================================

// 扫描字节码，判断程序是否用到了任一窗口内建。
//
// 只看 opcode 与操作数，不做数据流分析：内建编号是静态的，
// 出现即意味着「这条路径存在」。哪怕永远不执行到（比如放在
// 一个 if false 里），也照样算用过 —— 保守方向是「多编一点」，
// 因为两个方向的错误代价不对称：
//
//   多算了  → 产物多几百 KB，功能完全正常
//   漏算了  → 链接期 undefined reference to xgui_open，
//             这个错误信息对用户零指向性（他不会想到「因为
//             我写了 winOpen，所以编译时少了一段看不见的代码」）
//
// 所以这里宁可误报。真正需要精确判断的地方另说 —— 但那是运行期
// 的事，不该让编译期为此做数据流分析。
//
// ── 判据的上界必须是窗口段自己的边界 ────────────────────────
//
// 这里原本写的是 `id < XBI_COUNT`，也就是「用了任何平台内建都算
// 用了窗口」。那时枚举里除了窗口函数只有数学三个，近似还勉强成立。
//
// 但数组与字符串那批内建加进来之后，「用了任何内建」与「用了窗口」
// 成了完全不同的两件事：arrPush、strAt、math 全都会命中原判断。
//
// 症状是每个普通程序都收到一条「程序用到了窗口能力，加
// --profile=desktop 重编」的警告。而**这条建议是错的**：照做会让
// 产物变成 WINDOWS 子系统，那之后所有 println 都写进无效句柄，
// 程序静默地什么都不输出 —— 比不警告糟糕得多。
//
// 所以上界改用 XBI__WIN_END。它是窗口段的结束标记，紧跟在最后一个
// win* 后面。
bool bc_uses_gui(Bytecode* bc) {
    if (!bc) return false;
    for (int i = 0; i < bc->code_count; i++) {
        Insn* in = &bc->code[i];
        if (in->op != BC_BUILTIN) continue;
        XBuiltin id = (XBuiltin)in->a;
        if (id > XBI_NONE && id < XBI__WIN_END) return true;
        // v0.21 的编辑器内建追加在枚举末尾（为了不改旧编号），
        // 所以这里单独列一段，不能靠 XBI__WIN_END 一个上界。
        if (id >= XBI_WIN_POLL_KEY && id <= XBI_WIN_SET_RESIZABLE) return true;
    }
    return false;
}

// 程序是否用到了 TCP。
//
// ── 为什么需要这个判据，而且必须与 bc_uses_gui 分开 ────────
//
// AOT 产物要链接哪些系统库，取决于程序实际用了什么。曾经用一个
// `#pragma comment(lib, "ws2_32.lib")` 让 MSVC 自己解决，但 MinGW
// **静默忽略** 这条 pragma，于是所有用到 TCP 的产物在 MinGW 下
// 都报一堆 `undefined reference to __imp_socket` —— 而源码里明明
// 写着要这个库，这是最费解的一类问题。所以链接选项必须由我们
// 显式加，也就必须能在生成 C 代码之前判出来。
//
// 判 TCP 而不是判 GUI：一个只做 HTTP 客户端的程序既不开窗也不需要
// gdi32/user32，但它必须链接 ws2_32。把两者混成一个判据，会导致
// 「纯命令行 HTTP 客户端」被迫链接整个窗口运行时，交叉编译到
// Linux 时直接失败。
bool bc_uses_net(Bytecode* bc) {
    if (!bc) return false;
    for (int i = 0; i < bc->code_count; i++) {
        Insn* in = &bc->code[i];
        if (in->op != BC_BUILTIN) continue;
        XBuiltin id = (XBuiltin)in->a;
        if (id >= XBI_NET_LISTEN && id <= XBI_NET_PENDING) return true;
    }
    return false;
}

// 程序是否会往 stdout 写东西。
//
// 用于 --profile 的「错配检查」：desktop 档（WINDOWS 子系统）的进程
// 双击启动时没有控制台，stdout 是无效句柄，所有 println 静默丢失。
// 一个既不开窗、又在 println 的程序选了 desktop 档，几乎必然是误用。
//
// 只看 BC_PRINT 一条指令。为什么不用更细的判据（比如「这条 PRINT
// 到底会不会执行」）：那需要数据流分析，而误报的代价只是多一条
// 警告 —— 这条警告本身就写着「如果不改会怎样」，用户一眼能判断。
// 相反，漏报的代价是「程序静默地什么都不输出」，那是最难查的一类。
bool bc_has_print(Bytecode* bc) {
    if (!bc) return false;
    for (int i = 0; i < bc->code_count; i++) {
        if (bc->code[i].op == BC_PRINT) return true;
    }
    return false;
}

void bc_disasm(Bytecode* bc, FILE* out) {
    fprintf(out, "; X bytecode version 0x%04X\n", bc->version);
    fprintf(out, "; %d instructions, %d constants, %d functions\n\n",
            bc->code_count, bc->const_count, bc->func_count);

    // 常量池
    if (bc->const_count > 0) {
        fprintf(out, ".constants\n");
        for (int i = 0; i < bc->const_count; i++) {
            Constant* c = &bc->consts[i];
            switch (c->kind) {
                case CONST_I64:
                    fprintf(out, "  [%d] i64  %lld\n", i, (long long)c->i);
                    break;
                case CONST_F64:
                    fprintf(out, "  [%d] f64  %g\n", i, c->f);
                    break;
                case CONST_STR:
                    fprintf(out, "  [%d] str  \"%.*s\"\n", i, (int)c->str.len, c->str.s);
                    break;
            }
        }
        fprintf(out, "\n");
    }

    // 函数
    for (int fi = 0; fi < bc->func_count; fi++) {
        FuncProto* f = &bc->funcs[fi];
        fprintf(out, ".func %s/%d   ; locals=%d  %s:%d\n",
                f->name, f->param_count, f->local_count,
                f->pos.file ? f->pos.file : "?", f->pos.line);

        for (int i = 0; i < f->code_len; i++) {
            int idx = f->code_start + i;
            Insn* in = &bc->code[idx];
            fprintf(out, "  %04d  %-16s", idx, bc_op_name(in->op));
            if (has_operand(in->op)) {
                // 常量索引的特殊显示
                if ((in->op == BC_PUSH_I64 || in->op == BC_PUSH_F64 || in->op == BC_PUSH_STR)
                    && in->a >= 0 && in->a < bc->const_count) {
                    Constant* c = &bc->consts[in->a];
                    if (c->kind == CONST_STR) {
                        fprintf(out, " [%d] = \"%.*s\"", in->a, (int)c->str.len, c->str.s);
                    } else if (c->kind == CONST_I64) {
                        fprintf(out, " [%d] = %lld", in->a, (long long)c->i);
                    } else {
                        fprintf(out, " [%d] = %g", in->a, c->f);
                    }
                } else if (in->op == BC_JUMP || in->op == BC_JUMP_IF_FALSE ||
                           in->op == BC_JUMP_IF_TRUE || in->op == BC_LT_JUMP_FALSE) {
                    // a 是相对本条下一条的偏移（函数内）。反汇编里
                    // 显示成「实际会跳到的绝对指令号」，读起来直观 ——
                    // 注意换算要加函数基址 f->code_start，只用局部下标 i
                    // 会把所有非首函数的跳转都显示错。
                    fprintf(out, " -> %04d", idx + 1 + in->a);
                } else if (in->op == BC_LOAD_LOAD || in->op == BC_LOAD_CONST) {
                    // 两条装载类超指令有两个操作数，都显示出来
                    fprintf(out, " %d, %d", in->a, in->b);
                } else if (in->op == BC_LT_CONST_JUMP_FALSE ||
                           in->op == BC_ADD_CONST_STORE_JUMP ||
                           in->op == BC_SUB_CONST_STORE_JUMP) {
                    // 偏移在 a（与其它跳转指令一致），另外两个操作数顺延
                    fprintf(out, " -> %04d  slot=%d const=%d",
                            idx + 1 + in->a, (int)in->b, (int)in->c);
                } else if (in->op == BC_ADD_CONST_STORE ||
                           in->op == BC_SUB_CONST_STORE) {
                    fprintf(out, " slot=%d const=%d", (int)in->a, (int)in->b);
                } else if (in->op == BC_ARITH_CONST_STORE) {
                    fprintf(out, " slot=%d const=%d op=%s",
                            (int)in->a, (int)in->b,
                            x_arith_op_symbol((XArithOp)in->c));
                } else if (in->op == BC_ARITH_STORE) {
                    fprintf(out, " slot=%d op=%s",
                            (int)in->a, x_arith_op_symbol((XArithOp)in->b));
                } else if (in->op == BC_MOV_LOCAL) {
                    fprintf(out, " %d -> %d", (int)in->a, (int)in->b);
                } else if (in->op == BC_NE_CONST_JUMP_FALSE) {
                    fprintf(out, " -> %04d  slot=%d const=%d",
                            idx + 1 + in->a, (int)in->b, (int)in->c);
                } else if (in->op == BC_NE_JUMP_FALSE) {
                    fprintf(out, " -> %04d  slot=%d, %d",
                            idx + 1 + in->a, (int)in->b, (int)in->c);
                } else if (in->op == BC_CALL && in->a >= 0 && in->a < bc->func_count) {
                    fprintf(out, " %s/%d", bc->funcs[in->a].name, bc->funcs[in->a].param_count);
                } else if (in->op == BC_BUILTIN) {
                    // b = 参数个数。必须显示 —— 它是调整栈深度的依据，
                    // 而「argc 写成 0」的症状（弹到无关的栈值、报
                    // 「第 1 个参数类型不对」）完全不指向 argc。
                    // 只打 a 的话，这种 bug 在反汇编里看不出来。
                    fprintf(out, " %s argc=%d",
                            bc_builtin_name_of((XBuiltin)in->a), (int)in->b);
                } else {
                    fprintf(out, " %d", in->a);
                }
            }
            // 源码位置作为行尾注释，方便对照。
            // 位置从旁路数组取（Insn 里没有 pos 了，见 x.h 的说明）。
            SrcPos p = bc->pos[idx];
            if (p.line > 0) {
                fprintf(out, "   ; %s:%d:%d",
                        p.file ? p.file : "?", p.line, p.col);
            }
            fprintf(out, "\n");
        }
        fprintf(out, "\n");
    }
}