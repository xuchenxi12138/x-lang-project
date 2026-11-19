// interp.c — 栈式字节码解释器（P0）
//
// 设计要点（与 07-高性能解释器.md 对齐）：
//
//   1. **带标签的值 + 但热路径不查标签**
//      每个 Value 带一个 8 位类型标签和 8 字节载荷。这是「P0 最笨但正确」的做法。
//      07 §3.3 的未装箱 + NaN boxing 在 P1 做，那时 Int/Float 直接存在
//      NaN 的 payload 里，连标签都不用查。P0 的 _I64/_F64 特化指令
//      已经在指令选择层面避免了类型判断，标签只在需要提升/输出时才读。
//
//   2. **预分配值栈，不做栈检查**
//      栈深在 P1 会静态算出来（bc_op_stack_effect 已备好）。
//      P0 先给一个足够大的固定栈（默认 64K 值 = 512KB），
//      溢出就报运行时错误而不是崩溃。
//
//   3. **字符串用引用计数**
//      这是 01 §6 的 ARC 模型的最小可行版本。真正的 ARC（含逃逸分析、
//      并发域、weak 引用）在 P1/P2 做。P0 只需要保证拼接不泄漏。
//
//   4. **运行时错误带 SrcPos**
//      06 文档的核心承诺：任何失败都要给到精确到列的位置。
//      Insn 里存了 SrcPos，出错时直接用它。

#include "x.h"
#include "gui/gui.h"
#include <math.h>
#include <stdarg.h>
#include <errno.h>
// time() 用于 random() 的首次自动播种。放在这里而不是靠间接包含，
// 是因为 Windows 的 CRT 头文件包含顺序敏感 —— 少一层依赖少一类怪问题。
#include <time.h>

// ============================================================
// 运行时值
// ============================================================

typedef enum {
    VT_NULL = 0,
    VT_INT,
    VT_FLOAT,
    VT_BOOL,
    VT_STRING,
    VT_ARRAY,
    VT_STRUCT,
    VT_CLASS,      // 类实例（引用语义，字段可变，可多态）
} ValueType;

// 字符串对象：引用计数 + 长度 + 内联数据
typedef struct XStr {
    int    rc;
    size_t len;
    char   data[];     // 以 NUL 结尾，方便直接当 C 字符串用
} XStr;

typedef struct XArr XArr;
typedef struct XObj XObj;
typedef struct XInst XInst;

typedef struct {
    ValueType type;
    union {
        int64_t  i;
        double   f;
        bool     b;
        XStr*    s;
        XArr*    a;
        XObj*    o;
        XInst*   k;     // 类实例
    };
} Value;

// 数组对象：引用计数 + 长度 + 容量 + 元素。
//
// ── 为什么元素是 Value 而不是 int64 ─────────────────────────
//
// 扫雷的棋盘本质是 int 网格，看起来 int64 数组就够。但一门语言的
// 数组不应该只装 int：一旦只支持一种元素类型，之后加 Float 数组、
// String 数组就要再写一遍长度、扩容、引用计数逻辑。用 Value 统一，
// 代价是每个元素 16 字节而不是 8 —— 对这个阶段完全不重要，
// 而省下的是一整套重复实现。
//
// 元素存的是值拷贝（Int/Bool/Float 直接复制）。String/Array 元素
// 在写入和读取时都要 retain：写入时 retain（数组持有一份），
// 读取时也 retain（栈上那份是独立持有），销毁数组时逐个 release。
// 这个规则与字符串局部变量的处理完全一致，不引入第二套心智模型。
//
// ── cap 为什么必须有 ────────────────────────────────────────
//
// P0 原来只有 len，数组定长（array(n, fill) 之后不能再长）。写标准库
// 时这条立刻撞墙：json 解析要边扫边追加、字符串 split 要产出不定长
// 结果、矩阵拼接要扩容 —— 全都需要「长度能变」的数组。
//
// 加 cap 而不是改成链表/树：连续内存对遍历和下标访问最快，而这两个
// 是库代码里最频繁的操作（每次 JSON 遍历、每次矩阵乘法）。扩容按
// 1.5 倍增长（不是 2 倍）：2 倍在长期增长时平均浪费 50% 内存，
// 1.5 倍浪费 33%，而摊还复杂度同样是 O(1)。
struct XArr {
    int    rc;
    size_t len;      // 当前元素个数
    size_t cap;      // 已分配的槽数，cap >= len。push 用它判断要不要扩容
    Value* data;     // 独立指针，不再用柔性数组 —— 扩容要 realloc
};

// ---- 结构体对象 ----
//
// ── 为什么字段不可变 + 引用计数共享，等价于值语义 ──────────
//
// 教科书上的「值语义」是「赋值即深拷贝」。但深拷贝的代价随嵌套
// 深度指数增长，而且它解决的问题（改副本不影响原件）在字段
// **不可变**时根本不存在 —— 既然 `r.x = 1` 是编译错误，就没有
// 「改了副本」这回事。
//
// 于是剩下的唯一区别是「两个变量指向同一块内存」。这在字段
// 不可变、且相等性按结构比较（不是按地址）的前提下**不可观测**。
// 所以这里用引用计数共享，拿到的语义和深拷贝完全一样，成本却
// 是 O(1)。等字段可变了（需要 `&T` 引用类型），再把这里换成
// 写时复制即可 —— 那时 `rc == 1` 的快速路径仍然能省掉拷贝。
struct XObj {
    int        rc;
    int        type_idx;   // bc->structs 的下标，决定字段布局
    // 类型名。**不拥有**，指向 bc->structs[type_idx].name ——
    // 那份内存在字节码生命周期内一直有效。
    //
    // 为什么每个对象都存一份而不是用到时去查 type_idx：
    // value_cmp / value_to_str 这些函数拿不到 Bytecode 指针，
    // 而为了让它们能查就必须传一个全局 bc —— 那是隐式依赖，
    // 而且 AOT 侧根本没有 Bytecode 对象。多存一个指针（8 字节）
    // 换掉一整类「怎么找到类型表」的问题，值。
    const char* type_name;
    // 字段名数组，同样**不拥有**。指向 bc->structs[type_idx].field_names。
    // 只被 obj_to_str 用来打印字段名；为 NULL 时退化成 `?`。
    char**     field_names;
    int        field_count;
    Value*     fields;
};

// ---- 字符串 ----

static XStr* str_new(const char* src, size_t len) {
    XStr* s = x_alloc(sizeof(XStr) + len + 1);
    s->rc  = 1;
    s->len = len;
    if (len) memcpy(s->data, src, len);
    s->data[len] = '\0';
    return s;
}

static XStr* str_retain(XStr* s) {
    if (s) s->rc++;
    return s;
}

static void str_release(XStr* s) {
    if (!s) return;
    if (--s->rc <= 0) free(s);
}

// ---- 数组 ----
//
// 前向声明：arr_release 要逐个 release 元素，而元素本身可能是数组
// （嵌套数组），所以这两者互相引用。
static void value_release(Value* v);

static XArr* arr_new(size_t len, Value fill) {
    // 容量等于长度：array(n, fill) 是一次性建好的定长数组，多分配
    // 反而浪费。要增长的走 arr_reserve。
    size_t cap = len ? len : 1;
    XArr* a = x_alloc(sizeof(XArr));
    a->rc   = 1;
    a->len  = len;
    a->cap  = cap;
    a->data = x_alloc(cap * sizeof(Value));
    for (size_t i = 0; i < len; i++) {
        a->data[i] = fill;
        // fill 若是字符串/数组，每个槽各自持有一份引用
        if (fill.type == VT_STRING && fill.s)     fill.s->rc++;
        else if (fill.type == VT_ARRAY && fill.a) fill.a->rc++;
    }
    return a;
}

// 保证 cap >= need。不够时按 1.5 倍增长。
//
// 返回 true 表示成功。失败只可能是内存不足 —— 那条路径 x_alloc
// 内部已经 exit(70)，所以这里实际总是成功；保留 bool 是为了
// 将来换成可恢复的分配失败时不用改调用点。
static bool arr_reserve(XArr* a, size_t need) {
    if (!a) return false;
    if (a->cap >= need) return true;

    size_t ncap = a->cap ? a->cap : 4;
    while (ncap < need) {
        // 1.5 倍且至少 +1，避免 cap==0 时死循环
        size_t next = ncap + (ncap >> 1);
        if (next <= ncap) next = ncap + 1;
        ncap = next;
    }
    a->data = x_realloc(a->data, ncap * sizeof(Value));
    a->cap  = ncap;
    return true;
}

// 追加一个元素。value 的所有权**转移**给数组（调用方不再 release）。
static void arr_push(XArr* a, Value v) {
    arr_reserve(a, a->len + 1);
    a->data[a->len++] = v;
}

static XArr* arr_retain(XArr* a) {
    if (a) a->rc++;
    return a;
}

static void arr_release(XArr* a) {
    if (!a) return;
    if (--a->rc > 0) return;
    for (size_t i = 0; i < a->len; i++) value_release(&a->data[i]);
    free(a->data);
    free(a);
}

// ---- 结构体 ----

static XObj* obj_new(int type_idx, const char* type_name,
                     char** field_names, int field_count) {
    XObj* o = x_alloc(sizeof(XObj));
    o->rc          = 1;
    o->type_idx    = type_idx;
    o->type_name   = type_name;
    o->field_names = field_names;
    o->field_count = field_count;
    o->fields      = field_count ? x_alloc((size_t)field_count * sizeof(Value)) : NULL;
    // 默认值一律 null。字段全都会被 BC_NEW_STRUCT 立刻填满，
    // 这里初始化是为了「万一某个字段没被填」时是 null 而不是
    // 堆上的垃圾指针 —— 后者会让 obj_release 去 free 一个野指针。
    for (int i = 0; i < field_count; i++) {
        o->fields[i].type = VT_NULL;
        o->fields[i].i    = 0;
    }
    return o;
}

static XObj* obj_retain(XObj* o) {
    if (o) o->rc++;
    return o;
}

static void obj_release(XObj* o) {
    if (!o) return;
    if (--o->rc > 0) return;
    for (int i = 0; i < o->field_count; i++) value_release(&o->fields[i]);
    free(o->fields);
    free(o);
}

// ---- 类实例 ----
//
// ── 与 XObj 的关系：完全不同，不是「可变版的结构体」 ────────
//
// XObj 是不可变的值，所以可以随便共享（多个变量指向同一个对象
// 也观察不到差异）。XInst 是可变的对象 —— 共享**必须**可观测，
// 那正是引用语义。
//
// 两者字段布局的来源也不同：
//   XObj  → bc->structs[i]，只含本结构体声明的字段
//   XInst → bc->classes[i]，含**继承来的**字段（父类在前）
//
// ── 为什么实例里存 cls_idx 而不是存一份虚表指针 ─────────────
//
// 存索引：8 字节，虚表在 bc->classes[cls_idx].vtable 里，虚调用
// 是「先取 cls_idx，再查 vtable[slot]」，两次间接寻址。
//
// 存指针：还是 8 字节，但指向哪里？虚表属于**类**而不属于实例，
// 所有同类的实例共享同一份 —— 那就得在分配实例时从类表里把
// 指针抄过来。这样能省一次间接寻址，代价是 .xbc 加载后必须
// 逐类重建指针（否则指向的是编译期的表），而两条路径各写一遍
// 「重建」就有漂移风险。
//
// 选存索引：省一次间接寻址在这个阶段毫无意义（虚调用本身已经是
// 一次 BC_VIRT_CALL 分派），而省掉一整类「指针在加载后失效」的
// 问题是实打实的。
struct XInst {
    int        rc;
    int        cls_idx;      // bc->classes 的下标 —— **实际**类型
                             // 这是多态的全部：静态类型在编译期，
                             // 实际类型在这里。
    const char* cls_name;    // 不拥有，指向 bc->classes[cls_idx].name
    char**     field_names;  // 不拥有，诊断和打印用
    int        field_count;
    Value*     fields;       // 可变 —— 这就是它和 XObj 的核心差别
};

static XInst* inst_new(int cls_idx, const char* cls_name,
                       char** field_names, int field_count) {
    XInst* k = x_alloc(sizeof(XInst));
    k->rc          = 1;
    k->cls_idx     = cls_idx;
    k->cls_name    = cls_name;
    k->field_names = field_names;
    k->field_count = field_count;
    k->fields      = field_count ? x_alloc((size_t)field_count * sizeof(Value)) : NULL;
    // 全部清零。
    //
    // ── 为什么这里必须是「零值」而不是 null ─────────────────
    //
    // 没有显式构造函数的类，字段就保持这个初值。`Int` 字段拿 null
    // 会让后续算术报「不能用 null 做加法」—— 而用户根本没写构造
    // 函数，无从得知要给初值。零值是「尽量不惊讶」的选择。
    //
    // 但这里拿不到字段类型（只有名字），所以统一按「Int 0」初始化：
    // 0 对 Int 正确，对 Float 会被当成 Int 0（算术里会提升，无害），
    // 对 Bool 是 false，对 String/类引用是「非 null 的 0」——最后
    // 这一类是有问题的，所以带构造函数才是推荐用法。
    //
    // 真正的修法：BC_NEW_CLASS 之后由构造函数逐字段赋值；而
    // 「没有构造函数的类」应当由编译期强制要求所有字段都有默认值。
    // 当前版本用零值兜底，并在文档里写明「没有构造函数时字段是零值」。
    for (int i = 0; i < field_count; i++) {
        // 直接构值而不调 v_int()：v_int 定义在值构造区（本函数之后），
        // 这里只能就地写。多一行注释好过把整个值构造区往前搬。
        Value z;
        z.type = VT_INT;
        z.i    = 0;
        k->fields[i] = z;
    }
    return k;
}

static XInst* inst_retain(XInst* k) {
    if (k) k->rc++;
    return k;
}

static void inst_release(XInst* k) {
    if (!k) return;
    if (--k->rc > 0) return;
    for (int i = 0; i < k->field_count; i++) value_release(&k->fields[i]);
    free(k->fields);
    free(k);
}

// 值层面的 retain：只对「带引用计数」的类型生效，其余静默忽略。
//
// ── 为什么必须有这个函数 ────────────────────────────────────
//
// 加了数组之后，原本写 `if (v.type == VT_STRING) str_retain(v.s);`
// 的地方全部变成了漏点：漏一处就是 use-after-free，而且症状极隐蔽
// ——值读出来是对的，程序在之后某一步才崩。
//
// 本次实测一共漏了 5 处（LOAD_LOCAL / LOAD_LOAD / LOAD_CONST /
// DUP / DUP_STORE）。它们的共同点是「把栈上的值复制一份给别处」，
// 每一处都独立写了 type 判断。收敛成一个函数之后，将来再加
// 引用类型（比如 Map、闭包捕获）只需要改这一个地方。
static void value_retain(const Value* v) {
    switch (v->type) {
        case VT_STRING: str_retain(v->s); break;
        case VT_ARRAY:  arr_retain(v->a); break;
        case VT_STRUCT: obj_retain(v->o); break;
        case VT_CLASS:  inst_retain(v->k); break;
        default: break;   // Int/Float/Bool/Null 是值类型，无引用计数
    }
}

static XStr* str_concat(XStr* a, XStr* b) {
    size_t total = a->len + b->len;
    XStr* s = x_alloc(sizeof(XStr) + total + 1);
    s->rc  = 1;
    s->len = total;
    memcpy(s->data, a->data, a->len);
    memcpy(s->data + a->len, b->data, b->len);
    s->data[total] = '\0';
    return s;
}

// ---- 值构造与销毁 ----

static Value v_int(int64_t i)     { Value v; v.type = VT_INT;   v.i = i; return v; }
static Value v_float(double f)    { Value v; v.type = VT_FLOAT; v.f = f; return v; }
static Value v_bool(bool b)       { Value v; v.type = VT_BOOL;  v.b = b; return v; }
static Value v_null(void)         { Value v; v.type = VT_NULL;  v.i = 0; return v; }

static Value v_str(XStr* s) {
    Value v; v.type = VT_STRING; v.s = s; return v;
}

static Value v_obj(XObj* o) {
    Value v; v.type = VT_STRUCT; v.o = o; return v;
}

static Value v_inst(XInst* k) {
    Value v; v.type = VT_CLASS; v.k = k; return v;
}

static void value_release(Value* v) {
    if (v->type == VT_STRING) {
        str_release(v->s);
        v->s = NULL;
    } else if (v->type == VT_ARRAY) {
        arr_release(v->a);
        v->a = NULL;
    } else if (v->type == VT_STRUCT) {
        obj_release(v->o);
        v->o = NULL;
    } else if (v->type == VT_CLASS) {
        // 漏掉这一支的后果不是崩溃，而是所有实例永不回收——
        // 程序跑得越久越慢，最后在离原因很远的地方 OOM。
        inst_release(v->k);
        v->k = NULL;
    }
    v->type = VT_NULL;
}

// ============================================================
// 真值规则（01 §3.4）
// ============================================================
// null 为假；0 与 0.0 为假；空字符串为假；其余为真。
// 这条规则同时服务于 if 条件与 && / || 短路。

static bool truthy(Value v) {
    switch (v.type) {
        case VT_NULL:   return false;
        case VT_BOOL:   return v.b;
        case VT_INT:    return v.i != 0;
        case VT_FLOAT:  return v.f != 0.0;
        case VT_STRING: return v.s && v.s->len > 0;
        // 空数组为假，与空字符串一致。这是唯一自洽的选择：
        // 如果空数组为真，那 `if arr { }` 在数组为空时仍进入分支，
        // 而 `if str { }` 不进入 —— 两个「空的容器」行为不一致，
        // 用户必须记特例。
        case VT_ARRAY:  return v.a && v.a->len > 0;
        // 结构体一律为真。理由：结构体是「有身份的实体」，
        // 不是容器。一个 Rect{0,0,0,0} 是零尺寸矩形，但它存在。
        // 而「对象是否为 null」在 P0.5 里构造不出来（结构体值
        // 不可能为 null：字面量必然产出对象，字段也不可置 null
        // 到整个结构体上）。
        case VT_STRUCT: return v.o != NULL;
        // 实例永远为真 —— 造出来就存在。没有「空实例」这种状态：
        // 要么指向一个对象，要么是 null（那 VT_NULL）。
        case VT_CLASS:  return v.k != NULL;
    }
    return false;
}

static Value v_arr(XArr* a) {
    Value v; v.type = VT_ARRAY; v.a = a; return v;
}

// ============================================================
// 值比较（arrIndexOf / arrContains / arrSort 用）
// ============================================================

// 相等判定。与 `==` 运算符的语义**必须一致**，否则
// `arrContains(xs, 1)` 与 `any(x == 1)` 会给出不同答案，
// 用户会以为是库的 bug。
//
// Int 与 Float 之间按数值比较（1 == 1.0 为真）。这是 X 的
// `==` 的规则：数值类型之间允许比较，不做严格的类型相等 ——
// 否则 `1 == 1.0` 会让用户困惑，而它没有实际好处。
static bool value_equal(const Value* x, const Value* y) {
    if (x->type == VT_INT && y->type == VT_FLOAT) return (double)x->i == y->f;
    if (x->type == VT_FLOAT && y->type == VT_INT) return x->f == (double)y->i;
    if (x->type != y->type) {
        return false;
    }
    switch (x->type) {
        case VT_NULL:   return true;
        case VT_INT:    return x->i == y->i;
        case VT_FLOAT:  return x->f == y->f;
        case VT_BOOL:   return x->b == y->b;
        case VT_STRING:
            if (x->s->len != y->s->len) return false;
            return memcmp(x->s->data, y->s->data, x->s->len) == 0;
        case VT_ARRAY: {
            // 数组按元素逐个比。深度不设限 —— 递归比较的栈深由
            // 数据嵌套深度决定，而正常数据不会嵌套到爆栈。
            if (x->a->len != y->a->len) return false;
            for (size_t i = 0; i < x->a->len; i++) {
                if (!value_equal(&x->a->data[i], &y->a->data[i])) return false;
            }
            return true;
        }
        case VT_STRUCT: {
            // 结构体按**结构**比较，不是按地址。
            //
            // 这是「值语义」这条承诺的核心：`a == b` 必须回答
            // 「两个矩形是否相同」，而不是「它们是不是同一个对象」。
            // 引用计数共享是内部实现，用户观测不到，也不该观测到。
            //
            // 类型不同直接不等 —— 即使字段值恰好一致。`Rect{x:1,y:2}`
            // 和 `Point{x:1,y:2}` 是不同的东西，把它们的 x/y 拿来
            // 逐个比较然后说「相等」，会让类型系统失去意义。
            if (x->o->type_idx != y->o->type_idx) return false;
            if (x->o->field_count != y->o->field_count) return false;
            for (int i = 0; i < x->o->field_count; i++) {
                if (!value_equal(&x->o->fields[i], &y->o->fields[i])) return false;
            }
            return true;
        }
        // 类实例：**按身份**比较。
        //
        // ── 这是与结构体最重要的一处语义分歧，不是偷懒 ──────────
        //
        // 结构体是值：两个 Rect{x:1} 就是同一个值，按字段比。
        // 实例是对象：两个字段完全相同的 Button 是**两个不同按钮**
        // （各自的悬停状态、各自的回调），它们不该相等。
        //
        // 如果按字段比，会带来一个静默的严重后果：`if (a == b)`
        // 在「两个不同对象恰好字段相同」时为真，于是「这个按钮
        // 是否就是正在拖动的那个」这类判断全部出错 —— 而且完全不
        // 报错。Java 的 `==` 与 `Object.equals` 之争就是这个坑。
        //
        // 这里选 `==` 表示身份（与 Java/C# 默认一致）：需要按内容
        // 比较时，用户显式写一个比较方法，语义清楚。
        case VT_CLASS:
            return x->k == y->k;
    }
    return false;
}

// 排序用的全序比较。
//
// ── 为什么需要一个「跨类型」的全序 ──────────────────────────
//
// qsort 的比较函数必须满足：对任意两个元素都能给出确定的 -/0/+。
// 如果只处理同类型（遇到异构就返回 0），`[1, "a", null]` 的排序
// 结果取决于 qsort 的初始顺序 —— 同一个数组排两次可能得到不同结果，
// 这在测试里会表现为「偶尔失败」。
//
// 类型优先级：null < Bool < Int/Float < String < Array。
// 数值之间按大小（Int 与 Float 互比）。这个顺序没有「正确的」答案，
// 但**必须有**一个确定的顺序。
static int value_cmp(const void* pa, const void* pb) {
    const Value* a = (const Value*)pa;
    const Value* b = (const Value*)pb;

    // 数值类型彼此可比，先处理（Int/Float 同层）
    bool an = (a->type == VT_INT || a->type == VT_FLOAT);
    bool bn = (b->type == VT_INT || b->type == VT_FLOAT);
    if (an && bn) {
        double av = (a->type == VT_INT) ? (double)a->i : a->f;
        double bv = (b->type == VT_INT) ? (double)b->i : b->f;
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }
    // Bool 当 0/1，归到数值层比较好理解
    if (a->type == VT_BOOL || b->type == VT_BOOL) {
        bool an2 = an || a->type == VT_BOOL;
        bool bn2 = bn || b->type == VT_BOOL;
        if (an2 && bn2) {
            double av = (a->type == VT_BOOL) ? (a->b ? 1.0 : 0.0)
                                             : ((a->type == VT_INT) ? (double)a->i : a->f);
            double bv = (b->type == VT_BOOL) ? (b->b ? 1.0 : 0.0)
                                             : ((b->type == VT_INT) ? (double)b->i : b->f);
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }
    }

    // 跨类型：按类型编号排。VT_NULL=0 < VT_INT=1 ... 直接用枚举值，
    // 它本身就是一个稳定顺序。
    if (a->type != b->type) {
        return (int)a->type - (int)b->type;
    }

    switch (a->type) {
        case VT_NULL: return 0;
        case VT_STRING: {
            size_t n = a->s->len < b->s->len ? a->s->len : b->s->len;
            int c = memcmp(a->s->data, b->s->data, n);
            if (c != 0) return c;
            if (a->s->len < b->s->len) return -1;
            if (a->s->len > b->s->len) return 1;
            return 0;
        }
        case VT_ARRAY: {
            // 先长度后逐元素。给数组一个全序是必要的 ——
            // arrSort 排数组的数组时，没有全序就会因为 qsort 的
            // 初始顺序不同而产出不同结果（那种「偶尔失败」的测试）。
            if (a->a->len != b->a->len) return a->a->len < b->a->len ? -1 : 1;
            for (size_t i = 0; i < a->a->len; i++) {
                int c = value_cmp(&a->a->data[i], &b->a->data[i]);
                if (c != 0) return c;
            }
            return 0;
        }
        case VT_STRUCT: {
            // 先按**类型名**，再按字段。
            //
            // 按名字而不是 type_idx：idx 取决于声明顺序，而声明顺序
            // 随 import 排列变化。那会让排序结果依赖「先 import
            // 哪个文件」—— 一种没人能推理的语义。
            int c = strcmp(a->o->type_name ? a->o->type_name : "",
                           b->o->type_name ? b->o->type_name : "");
            if (c != 0) return c < 0 ? -1 : 1;
            int n = a->o->field_count < b->o->field_count
                    ? a->o->field_count : b->o->field_count;
            for (int i = 0; i < n; i++) {
                c = value_cmp(&a->o->fields[i], &b->o->fields[i]);
                if (c != 0) return c;
            }
            if (a->o->field_count < b->o->field_count) return -1;
            if (a->o->field_count > b->o->field_count) return 1;
            return 0;
        }
        // 类实例的全序：按类型名，再按地址。
        //
        // 按地址是为了「同一类型的不同实例有个稳定顺序」。这个顺序
        // 没有语义（不是用户能预期的），只是为了让 arrSort 有确定
        // 行为、不产生「同一份数据排序结果随运行变化」的诡异现象。
        //
        // 为什么不按字段：字段可变，排序过程中比较结果可能变化，
        // 会让 qsort 的行为未定义。按一个不变的东西（地址）排
        // 至少是自洽的。
        case VT_CLASS: {
            int c2 = strcmp(a->k->cls_name ? a->k->cls_name : "",
                            b->k->cls_name ? b->k->cls_name : "");
            if (c2 != 0) return c2 < 0 ? -1 : 1;
            if (a->k == b->k) return 0;
            return (uintptr_t)a->k < (uintptr_t)b->k ? -1 : 1;
        }
        default: return 0;
    }
}

// ============================================================
// 字符串工具（内建用）
// ============================================================

// 取第 ix 个码点（从 0 数）。越界返回空串而不是报错 ——
// strAt 在「逐字符扫描」的循环里用得最多，每次都要判边界
// 会让调用方代码变得很啰嗦。
static Value bi_str_codepoint_at(XStr* s, int64_t ix) {
    if (!s || ix < 0) return v_str(str_new("", 0));
    size_t pos = 0;
    int64_t idx = 0;
    while (pos < s->len) {
        int adv = 0;
        utf8_decode(s->data + pos, s->len - pos, &adv);
        if (adv <= 0) adv = 1;   // 非法字节按 1 步前进，避免死循环
        if (idx == ix) return v_str(str_new(s->data + pos, (size_t)adv));
        pos += (size_t)adv;
        idx++;
    }
    return v_str(str_new("", 0));
}

// 按码点切片，左闭右开。越界钳制（理由同 arrSlice）。
static Value bi_str_slice(XStr* s, int64_t start, int64_t end) {
    if (!s) return v_str(str_new("", 0));
    if (start < 0) start = 0;
    if (end < start) end = start;

    size_t pos = 0;
    int64_t idx = 0;
    size_t byte_start = s->len, byte_end = s->len;
    while (pos < s->len) {
        if (idx == start) byte_start = pos;
        if (idx == end) { byte_end = pos; break; }
        int adv = 0;
        utf8_decode(s->data + pos, s->len - pos, &adv);
        if (adv <= 0) adv = 1;
        pos += (size_t)adv;
        idx++;
    }
    if (idx == start) byte_start = pos;
    if (byte_end == s->len && idx < end) byte_end = s->len;
    if (byte_start > byte_end) byte_start = byte_end;

    return v_str(str_new(s->data + byte_start, byte_end - byte_start));
}

// 查找子串，返回**码点下标**（不是字节偏移）。
// 返回码点下标而不是字节偏移的理由：调用方拿到之后几乎总是
// 传给 strSub / strAt，那两个都按码点算。如果这里返回字节偏移，
// `strSub(s, 0, strFind(s, "中"))` 会切出半个字符。
static int64_t bi_str_find(XStr* s, XStr* sub, int64_t from) {
    if (!s || !sub) return -1;
    if (sub->len == 0) return from <= 0 ? 0 : -1;

    // 先按字节找（快），再把字节偏移转成码点序号
    size_t bfrom = 0;
    if (from > 0) {
        // 把码点下标转成字节偏移
        size_t pos = 0;
        int64_t idx = 0;
        while (pos < s->len && idx < from) {
            int adv = 0;
            utf8_decode(s->data + pos, s->len - pos, &adv);
            if (adv <= 0) adv = 1;
            pos += (size_t)adv;
            idx++;
        }
        bfrom = pos;
    }
    if (bfrom + sub->len > s->len) return -1;

    for (size_t i = bfrom; i + sub->len <= s->len; i++) {
        if (memcmp(s->data + i, sub->data, sub->len) == 0) {
            // 字节偏移 i → 码点下标
            int64_t cp = 0;
            size_t pos = 0;
            while (pos < i) {
                int adv = 0;
                utf8_decode(s->data + pos, s->len - pos, &adv);
                if (adv <= 0) adv = 1;
                pos += (size_t)adv;
                cp++;
            }
            return cp;
        }
    }
    return -1;
}

// 按分隔符切分。分隔符为空时按码点逐个切（与 Python 的
// "".join 反向行为接近，也是用户最容易预期的）。
static XArr* bi_str_split(XStr* s, XStr* sep) {
    XArr* out = arr_new(0, v_null());

    if (sep->len == 0) {
        size_t pos = 0;
        while (pos < s->len) {
            int adv = 0;
            utf8_decode(s->data + pos, s->len - pos, &adv);
            if (adv <= 0) adv = 1;
            arr_push(out, v_str(str_new(s->data + pos, (size_t)adv)));
            pos += (size_t)adv;
        }
        return out;
    }

    size_t start = 0;
    size_t i = 0;
    while (i + sep->len <= s->len) {
        if (memcmp(s->data + i, sep->data, sep->len) == 0) {
            arr_push(out, v_str(str_new(s->data + start, i - start)));
            i += sep->len;
            start = i;
        } else {
            i++;
        }
    }
    // 最后一段（包括「没有分隔符」的情况）
    arr_push(out, v_str(str_new(s->data + start, s->len - start)));
    return out;
}

// 用分隔符连接数组元素。非字符串元素会被转成字符串
// （与 println 的格式化规则一致），这样 strJoin([1,2], "-") 能用。
static XStr* bi_str_join(XArr* a, XStr* sep) {
    // 先算总长，一次性分配
    size_t total = 0;
    for (size_t i = 0; i < a->len; i++) {
        if (i) total += sep->len;
        Value* v = &a->data[i];
        if (v->type == VT_STRING) {
            total += v->s->len;
        } else {
            // 非字符串：先格式化成字符串再算长度。
            // 直接估算 32 字节够用（数字最长也就这个量级）。
            total += 32;
        }
    }

    XStr* out = x_alloc(sizeof(XStr) + total + 1);
    out->rc  = 1;
    out->len = 0;
    size_t w = 0;
    for (size_t i = 0; i < a->len; i++) {
        if (i) {
            memcpy(out->data + w, sep->data, sep->len);
            w += sep->len;
        }
        Value* v = &a->data[i];
        if (v->type == VT_STRING) {
            memcpy(out->data + w, v->s->data, v->s->len);
            w += v->s->len;
        } else {
            char tmp[64];
            int n = 0;
            switch (v->type) {
                case VT_NULL:  n = snprintf(tmp, sizeof(tmp), "null"); break;
                case VT_INT:   n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v->i); break;
                case VT_FLOAT: n = snprintf(tmp, sizeof(tmp), "%g", v->f); break;
                case VT_BOOL:  n = snprintf(tmp, sizeof(tmp), "%s", v->b ? "true" : "false"); break;
                default:       n = snprintf(tmp, sizeof(tmp), "?"); break;
            }
            if (n < 0) n = 0;
            if ((size_t)n > sizeof(tmp)) n = (int)sizeof(tmp);
            memcpy(out->data + w, tmp, (size_t)n);
            w += (size_t)n;
        }
    }
    out->data[w] = '\0';
    out->len = w;
    return out;
}

// 大小写转换。**只处理 ASCII 字母** —— 这是刻意的限制：
// 土耳其语的 i/İ、德语的 ß、希腊语的 Σ/σ/ς 都有语言相关的
// 大小写规则，实现错了比不实现更糟（用户会拿到「看起来变了
// 但语义不对」的字符串）。ASCII 覆盖 99% 的编程场景。
static XStr* bi_str_case(XStr* s, bool upper) {
    XStr* out = x_alloc(sizeof(XStr) + s->len + 1);
    out->rc  = 1;
    out->len = s->len;
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (upper && c >= 'a' && c <= 'z')      c = (unsigned char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        out->data[i] = (char)c;
    }
    out->data[s->len] = '\0';
    return out;
}

// 去首尾空白。空白集合用 C 的 isspace 集合（空格、\t、\n、\r、\f、\v）。
static XStr* bi_str_trim(XStr* s) {
    size_t b = 0, e = s->len;
    while (b < e) {
        char c = s->data[b];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') b++;
        else break;
    }
    while (e > b) {
        char c = s->data[e - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') e--;
        else break;
    }
    return str_new(s->data + b, e - b);
}

// 全部替换。from 为空时**原样返回**（不是死循环，也不是插在
// 每个字符之间）—— 后者是 Python 的行为但很少有用，而前者是
// 明确的安全选择。
static XStr* bi_str_replace(XStr* s, XStr* from, XStr* to) {
    if (from->len == 0) return str_new(s->data, s->len);

    // 先数出现次数，算总长
    size_t count = 0;
    for (size_t i = 0; i + from->len <= s->len; ) {
        if (memcmp(s->data + i, from->data, from->len) == 0) { count++; i += from->len; }
        else i++;
    }
    if (count == 0) return str_new(s->data, s->len);

    // 总长必须精确算。写成 `s->len + count * (to->len - from->len)` 是
    // 错的：两者都是 size_t，to 更短时减法会回绕成天文数字，
    // 随后 x_alloc 直接 abort。所以按两种情况分开算。
    size_t total;
    if (to->len >= from->len) {
        total = s->len + count * (to->len - from->len);
    } else {
        total = s->len - count * (from->len - to->len);
    }

    XStr* out = x_alloc(sizeof(XStr) + total + 1);
    out->rc = 1;
    size_t w = 0;
    for (size_t i = 0; i < s->len; ) {
        if (i + from->len <= s->len && memcmp(s->data + i, from->data, from->len) == 0) {
            memcpy(out->data + w, to->data, to->len);
            w += to->len;
            i += from->len;
        } else {
            out->data[w++] = s->data[i++];
        }
    }
    out->data[w] = '\0';
    out->len = w;
    return out;
}

// 解析整数。允许前导空白与正负号；遇到第一个非数字字符停止。
// 无法解析任何数字时返回 0 —— 不用 -1 表示错误，因为 -1 也可能是
// 合法结果（字符串 "-1"）。需要区分「失败」的场景用 strParseOk。
static int64_t bi_parse_int(XStr* s) {
    size_t i = 0;
    while (i < s->len && (s->data[i] == ' ' || s->data[i] == '\t')) i++;
    bool neg = false;
    if (i < s->len && (s->data[i] == '+' || s->data[i] == '-')) {
        neg = (s->data[i] == '-');
        i++;
    }
    int64_t v = 0;
    bool any = false;
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
        v = v * 10 + (s->data[i] - '0');
        i++;
        any = true;
    }
    if (!any) return 0;
    return neg ? -v : v;
}

static double bi_parse_float(XStr* s) {
    // 直接交给 strtod。它处理指数、小数点、inf/nan。
    // 复制到临时缓冲是因为 XStr 的数据虽以 NUL 结尾，但可能
    // 含有内嵌 NUL（虽然我们的字符串不会有），保险起见。
    char* tmp = x_alloc(s->len + 1);
    memcpy(tmp, s->data, s->len);
    tmp[s->len] = '\0';
    double v = strtod(tmp, NULL);
    free(tmp);
    return v;
}

// ============================================================
// 格式化
// ============================================================

static const char* type_label(ValueType t) {
    switch (t) {
        case VT_NULL:   return "Null";
        case VT_INT:    return "Int";
        case VT_FLOAT:  return "Float";
        case VT_BOOL:   return "Bool";
        case VT_STRING: return "String";
        case VT_ARRAY:  return "Array";
        case VT_STRUCT: return "Struct";
        case VT_CLASS:  return "Class";
    }
    return "?";
}

// 浮点转字符串：不用 %g（会丢精度且指数形式怪异），
// 而是「最短可往返」策略 —— 先试 %g，再用 strtod 读回验证。
static XStr* float_to_str(double f) {
    if (isnan(f)) return str_new("NaN", 3);
    if (isinf(f)) return f > 0 ? str_new("Infinity", 8) : str_new("-Infinity", 9);

    char buf[64];
    // 整数值得浮点直接打印成 "3.0" 而非 "3"，以区分 Int 与 Float（01 §4.1）
    //
    // ── 为什么先试 %.17g 之外的路径 ─────────────────────────
    //
    // %g 在指数 >= 精度 时会切到科学计数法。于是 350.0 用 %.2g 得到
    // "3.5e+02"，strtod 读回等于 350，循环就停在这里，用户看到的是
    // 科学计数法而不是 350.0。数值处理（矩阵、统计、金融）里
    // 三位数以上是常态，这个表现会持续困扰人。
    //
    // 修正：先判断「这个 double 是否恰好是某个 int64 值」，是就用
    // %.1f 打印（350.0 / -7.0 / 0.0），既精确又不会跑进指数形式。
    // 非整数才走 %g 的最短往返。
    if (f >= -9.007199254740992e15 && f <= 9.007199254740992e15) {
        int64_t as_int = (int64_t)f;
        if ((double)as_int == f) {
            snprintf(buf, sizeof(buf), "%.1f", f);
            return str_new(buf, strlen(buf));
        }
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, f);
        double back = strtod(buf, NULL);
        if (back == f) break;
    }
    // 若结果里没有 '.'、'e'、'E'、'n'(nan)、'i'(inf)，补上 ".0"
    if (!strpbrk(buf, ".eEni")) {
        strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
    }
    return str_new(buf, strlen(buf));
}

static XStr* int_to_str(int64_t i) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)i);
    return str_new(buf, (size_t)n);
}

static XStr* value_to_str(Value v);

// 数组 → 字符串。深度上限防止自引用（当前语言还构造不出自引用数组，
// 但嵌套数组已经可以，无限递归必须有界）。
static XStr* arr_to_str(XArr* a, int depth) {
    if (!a) return str_new("null", 4);
    if (depth > 8) return str_new("[...]", 5);

    XStr* out = str_new("[", 1);
    for (size_t i = 0; i < a->len; i++) {
        if (i > 0) {
            XStr* sep = str_new(", ", 2);
            XStr* np = str_concat(out, sep);
            str_release(out); str_release(sep);
            out = np;
        }
        Value el = a->data[i];
        XStr* part;
        if (el.type == VT_ARRAY) part = arr_to_str(el.a, depth + 1);
        else                     part = value_to_str(el);
        XStr* np = str_concat(out, part);
        str_release(out); str_release(part);
        out = np;
    }
    XStr* close = str_new("]", 1);
    XStr* r = str_concat(out, close);
    str_release(out); str_release(close);
    return r;
}

// 结构体 → 字符串：`Rect { x: 1, y: 2 }`。
//
// 格式为什么要带类型名和字段名：结构体的用途就是「让数据自解释」，
// 打印成 `<struct>` 或 `{1, 2}` 会把这份信息丢掉。而调试时最常问
// 的恰恰是「这个对象到底是什么类型、每个字段什么值」。
//
// 与 AOT 侧 xo_str 的格式必须逐字一致 —— 否则同一个程序在
// println 上的输出会因编译方式不同而不同，差分测试会直接抓到。
static XStr* obj_to_str(XObj* o, int depth) {
    if (!o) return str_new("null", 4);
    if (depth > 8) return str_new("{...}", 5);

    XStr* out = str_new("", 0);
    #define OBJ_APPEND(ptr, len_) do {                       \
        XStr* _p = str_new((ptr), (len_));                   \
        XStr* _n = str_concat(out, _p);                      \
        str_release(out); str_release(_p); out = _n;          \
    } while (0)

    OBJ_APPEND(o->type_name ? o->type_name : "?",
               o->type_name ? strlen(o->type_name) : 1);
    OBJ_APPEND(" { ", 3);
    for (int i = 0; i < o->field_count; i++) {
        if (i > 0) OBJ_APPEND(", ", 2);
        // 字段名。只在解释器侧能拿到 —— 见 BC_FIELD_GET 的实现说明。
        if (o->field_names && o->field_names[i]) {
            OBJ_APPEND(o->field_names[i], strlen(o->field_names[i]));
        } else {
            OBJ_APPEND("?", 1);
        }
        OBJ_APPEND(": ", 2);
        Value fv = o->fields[i];
        XStr* part = (fv.type == VT_STRUCT) ? obj_to_str(fv.o, depth + 1)
                                            : value_to_str(fv);
        XStr* np = str_concat(out, part);
        str_release(out); str_release(part);
        out = np;
    }
    OBJ_APPEND(" }", 2);
    #undef OBJ_APPEND
    return out;
}

// 类实例的字符串表示。
//
// ── 为什么格式与结构体一致（`名字 { 字段: 值 }`）────────────
//
// 输出格式的价值在于「debug 时一眼看懂」。既然用户看到 struct 时
// 已经熟悉这个形状，实例用同一个形状就省掉一次学习成本 —— 而
// 两者本来也是同一种东西的两种内存模型。
//
// 类名用的是 `cls_name`（实际类型），不是静态类型。这一条很关键：
// `let s: Shape = new Circle(...)` 打印出 `Circle { ... }` 而不是
// `Shape { ... }`，正好在多态里向用户证明「派发是按实际类型走的」。
// 如果打印静态类型，用户会以为多态没生效。
static XStr* inst_to_str(XInst* k, int depth) {
    if (!k) return str_new("null", 4);
    if (depth > 8) return str_new("{...}", 5);

    XStr* out = str_new("", 0);
    #define INST_APPEND(ptr, len_) do {                      \
        XStr* _p = str_new((ptr), (len_));                   \
        XStr* _n = str_concat(out, _p);                      \
        str_release(out); str_release(_p); out = _n;          \
    } while (0)

    INST_APPEND(k->cls_name ? k->cls_name : "?",
                k->cls_name ? strlen(k->cls_name) : 1);
    INST_APPEND(" { ", 3);
    for (int i = 0; i < k->field_count; i++) {
        if (i > 0) INST_APPEND(", ", 2);
        if (k->field_names && k->field_names[i]) {
            INST_APPEND(k->field_names[i], strlen(k->field_names[i]));
        } else {
            INST_APPEND("?", 1);
        }
        INST_APPEND(": ", 2);
        Value fv = k->fields[i];
        XStr* part;
        if (fv.type == VT_STRUCT)      part = obj_to_str(fv.o, depth + 1);
        else if (fv.type == VT_CLASS)  part = inst_to_str(fv.k, depth + 1);
        else                           part = value_to_str(fv);
        XStr* np = str_concat(out, part);
        str_release(out); str_release(part);
        out = np;
    }
    INST_APPEND(" }", 2);
    #undef INST_APPEND
    return out;
}

// 任意值 → 字符串（新引用）
static XStr* value_to_str(Value v) {
    switch (v.type) {
        case VT_NULL:   return str_new("null", 4);
        case VT_INT:    return int_to_str(v.i);
        case VT_FLOAT:  return float_to_str(v.f);
        case VT_BOOL:   return v.b ? str_new("true", 4) : str_new("false", 5);
        case VT_STRING: return str_retain(v.s);
        case VT_ARRAY: {
            // 数组打印成 [1, 2, 3]。嵌套数组会递归展开。
            //
            // 为什么不用 `<array len=3>` 这种形式：调试时最想要的是
            // 「里面到底是什么」，而扫雷这类程序里 `toString(board)`
            // 直接看棋盘就很有用。深度上限 8 层防止自引用死循环
            // （现在还不能创建自引用数组，但将来能）。
            return arr_to_str(v.a, 0);
        }
        case VT_STRUCT: return obj_to_str(v.o, 0);
        case VT_CLASS:  return inst_to_str(v.k, 0);
    }
    return str_new("?", 1);
}

// ============================================================
// 数值提升
// ============================================================
// `ADD_NUM` 这类双分派指令内部使用。
// 07 §3.2 的目标就是让这条路径尽可能少走——静态类型已经分流出去了。

static bool as_float(const Value* v, double* out) {
    if (v->type == VT_FLOAT) { *out = v->f; return true; }
    if (v->type == VT_INT)   { *out = (double)v->i; return true; }
    return false;
}

// ============================================================
// 溢出检查（01 §5.4 的裁决：默认 trap，release 也不关闭）
// ============================================================
// 为什么 P0 就实现：这是一条**语义**约定，不是优化。
// 如果 P0 先做成回绕，后面所有测试的期望值都按回绕写，
// P1 改成 trap 时会一次性打破所有既有用例——这正是 05 文档
// 「先做最笨的版本把语义跑对」要避免的事。
//
// `+%`（wrapping）/ `+|`（saturating）/ `+?`（checked）三个显式算子留 P1。

static void rt_error(SrcPos pos, const char* fmt, ...);

// 分支预测提示。
//
// 为什么需要它：溢出、除零、类型错这些错误分支在真实程序里
// **一次都不会执行**，但它们在代码里紧挨着热路径，不带提示时
// 编译器会把「出错」和「正常」两条路径同等对待——错误路径的代码
// 会被放在热路径的直线上，挤占 icache 并干扰分支预测器。
//
// 标了 LIKELY / UNLIKELY 之后，编译器会把冷路径挪到函数的尾部
// （GCC 的 __builtin_expect 会触发 block reordering），热路径
// 变成一段连续的、没有跳转打断的直线代码。
#if defined(__GNUC__) || defined(__clang__)
#  define X_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define X_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define X_LIKELY(x)   (x)
#  define X_UNLIKELY(x) (x)
#endif

// 把溢出检查交给编译器内建函数。
//
// 关键写法：**错误处理不在这里做**。这几个函数只返回「有没有溢出」，
// 出错的报告由调用点做——因为调用点才有 pos，而这个 pos 是从
// 旁路数组读的，只有真出错时才值得读。若在这里做报告，编译器就
// 必须保证 pos 在函数入口处已就绪，于是又回到「每条指令都读 pos」。
static inline bool add_ovf(int64_t a, int64_t b, int64_t* r) {
    return __builtin_add_overflow(a, b, r);
}
static inline bool sub_ovf(int64_t a, int64_t b, int64_t* r) {
    return __builtin_sub_overflow(a, b, r);
}
static inline bool mul_ovf(int64_t a, int64_t b, int64_t* r) {
    return __builtin_mul_overflow(a, b, r);
}

// 溢出时的报告。标了 noinline：这段代码只该出现在冷路径上，
// 内联进主循环会让热路径体积膨胀、把指令推出 icache。
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static void overflow_error(SrcPos pos, const char* op) {
    rt_error(pos,
             "整数溢出：`%s` 的结果超出 Int 的表示范围\n"
             "  note: 01 §5.4 规定整数溢出默认 trap，release 也不关闭\n"
             "  help: 确认数值不会超出 ±9223372036854775807；\n"
             "        若要允许回绕，P1 会提供 `+%%` 显式算子（当前未实现）",
             op);
}

// ============================================================
// 运行时错误
// ============================================================
// 06 文档要求的六段式诊断，P0 只实现前四段：
//   位置 / 原因 / 相关值 / 建议
// 剩下两段（调用栈、相关文档链接）在 P1 补。

static void rt_error(SrcPos pos, const char* fmt, ...) {
    // stdout 是行缓冲（重定向时甚至是全缓冲），stderr 无缓冲。
    // 不先 flush 的话，错误信息会跑到正常输出前面，看起来像「先报错后打印」。
    // 这个细节在 06 文档里没写，但实测很难受，所以这里显式处理。
    fflush(stdout);

    va_list ap;
    fprintf(stderr, "\n");
    // 位置行在前、error 在后：这是运行时诊断的既有格式（与 06 §4.1
    // 的「位置 / 原因 / 解释 / 建议」顺序一致），编译期则是
    // 「error / 位置」——两者刻意不同：运行时用户最先要知道「哪一行崩了」，
    // 编译期最先要知道「错在哪类问题」。
    // 标签对齐统一交给 diag_label，避免两处各算一遍补空数而错位。
    print_src_pos(stderr, pos);
    diag_label(stderr, "error");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

// ============================================================
// 帧与栈
// ============================================================

#define STACK_MAX   (64 * 1024)     // 值栈容量
#define FRAME_MAX   4096            // 调用深度上限

typedef struct {
    int      func_index;
    int      ret_pc;        // 返回后要跳到的指令索引
    int      base;          // 本帧局部变量在值栈上的起始位置
    SrcPos   call_pos;      // 调用点（用于运行时错误回溯）
    // 这一帧是不是构造函数。
    //
    // ── 为什么需要这个标记 ──────────────────────────────────
    //
    // 构造函数在语言层面**没有返回值** —— 用户写
    //     constructor(w: Int) { self.w = w }
    // 而编译器给它的函数体末尾补的是 `return null`（兜底返回）。
    //
    // 如果按普通函数处理，`new Foo(3)` 的结果就是那个 null，而不是
    // 刚造出来的实例。实例会立刻泄漏（refcount 永远归 1 而无人
    // 持有），而用户拿到 null 之后一步就崩 —— 崩的地方离真正的
    // 原因极远。
    //
    // 所以 BC_RETURN 必须知道「这次返回要丢掉返回值、改用待压栈的
    // 实例」。这就是 is_ctor 的用途。
    bool     is_ctor;

    // 本帧正在构造的实例（仅 is_ctor 帧有意义）。
    //
    // ── 为什么挂在帧上而不是 VM 上的单槽 ────────────────────
    //
    // 最初用的是 VM 里一个 `ctor_result` 单槽。但构造函数里完全可以
    // 再 `new` 另一个类：
    //     constructor() { self.child = new Btn() }
    // 内层构造把外层的实例覆盖掉，外层实例 refcount 停在 1 而无人
    // 持有 —— 永久泄漏，而且没有任何报错。
    //
    // 挂到帧上就等于「按栈深度自动分层」，嵌套多少层都对。VM 里
    // 那个槽位因此被删掉了：留着它只会诱使后来的人再踩一次。
    Value    this_inst;

    // 本帧是哪个类的方法（类索引；普通函数为 -1）。
    //
    // ── 反射为什么需要它 ────────────────────────────────────
    //
    // `fieldGet(obj, "secret")` 必须知道调用方是谁，才能判 private
    // 该不该放行。判据是「调用点所在的类」，而这在运行时唯一可靠的
    // 来源就是当前帧。拿实例自己的类去判是错的 —— 那样 private
    // 就变成「只要能拿到对象就能读」，封装直接失效。
    //
    // 编译器为类方法建帧时填这个值。它不改任何执行语义，纯粹是
    // 给反射提供上下文。
    int      cls_idx;
} Frame;

typedef struct {
    Bytecode* bc;
    Value     stack[STACK_MAX];
    int       sp;
    Frame     frames[FRAME_MAX];
    int       fp;
} VM;

static void vm_push(VM* vm, Value v, SrcPos pos) {
    if (vm->sp >= STACK_MAX) {
        rt_error(pos, "值栈溢出（上限 %d 个值）", STACK_MAX);
        exit(70);
    }
    vm->stack[vm->sp++] = v;
}

static Value vm_pop(VM* vm, SrcPos pos) {
    if (vm->sp <= 0) {
        rt_error(pos, "内部错误：值栈下溢（这通常是编译器栈平衡 bug，请报告）");
        exit(70);
    }
    return vm->stack[--vm->sp];
}

static Value* vm_top(VM* vm) {
    return vm->sp > 0 ? &vm->stack[vm->sp - 1] : NULL;
}

// 建帧。所有调用点都必须走这里。
//
// ── 为什么不能就地写 `Frame* nf = &vm->frames[vm->fp++];` ──────
//
// Frame 现在带 is_ctor / this_inst。vm->frames 是复用的数组，同一
// 个槽位会被后来的调用反复覆盖。只要有一个建帧点忘了把 is_ctor
// 写成 false，那一帧就会继承上一个函数留下的 true —— BC_RETURN
// 于是把它的正常返回值丢掉，改压一个早已释放的实例。
//
// 这种 bug 只在「前一次调用碰巧是构造函数」时出现，测不出来也复
// 现不了。统一入口比逐个补赋值可靠。
static Frame* vm_push_frame(VM* vm, int func_index, int ret_pc,
                            int base, SrcPos call_pos) {
    Frame* nf = &vm->frames[vm->fp++];
    nf->func_index = func_index;
    nf->ret_pc     = ret_pc;
    nf->base       = base;
    nf->call_pos   = call_pos;
    nf->is_ctor    = false;
    nf->this_inst  = v_null();
    nf->cls_idx    = -1;
    return nf;
}

// 反射用的「当前类」：只看**当前帧**属于哪个类。
//
// ── 为什么不能扫全栈 ────────────────────────────────────────
//
// 第一版是「从栈顶往下找第一个 cls_idx >= 0 的帧」，想支持「类方法
// 调了 helper，helper 里再调反射也算在类内」。但它有一个致命漏洞：
// 只要**任何**祖先帧是类方法，类外代码里的反射就被算作类内。
//
// 具体症状：main 调 v.reveal()（Vault 的方法，帧里 cls_idx=Vault），
// reveal 返回后那个帧已经弹掉了 —— 但如果 main 自己是某个类的方法，
// 后面所有反射都会以那个类为当前类。而更常见的形态是：
//
//     class Vault { fn reveal() { ... } }     // 类内读 private 正常
//     fn main() { fieldGet(v, "secret") }     // 类外，应当被拒
//
// 扫全栈时，main 的帧 cls_idx = -1，但**上一次**调用的类帧如果
// 因为某种原因还在栈上，就会被误认成当前类。
//
// 所以收紧成「只看栈顶帧」。代价是「helper 函数里调反射」会被当成
// 类外 —— 那个场景可以显式传对象、由类方法代劳，而**漏掉一次
// 越权检查**是不可接受的：private 形同虚设。
//
// 保守方向永远选「更严格」：误拒是可见的编译期/运行期错误，
// 误放是静默的安全漏洞。
static int vm_cur_class(VM* vm) {
    if (vm->fp <= 0) return -1;
    return vm->frames[vm->fp - 1].cls_idx;
}

// 从函数名推断它属于哪个类，返回类索引；不是类方法则返回 -1。
//
// ── 为什么靠名字而不是给 FuncProto 加字段 ──────────────────
//
// 方法在编译期就被改写成了 `Class.method` 这种普通函数名（与结构体
// 方法同一套规则），所以名字里已经带着归属信息。加字段的话要动
// .xbc 的函数原型段、AOT 的代码发射、以及所有构造 FuncProto 的地方，
// 而信息本来就是冗余的。
//
// 判据是「点号之前的部分是个已知类名」。用点号而不是「第一个点」，
// 是因为类名里不会有点（标识符不允许），所以第一个点就是分隔符。
//
// 找不到返回 -1。不报错：绝大多数函数都不属于任何类，那是正常情况。
static int func_owner_class(Bytecode* bc, int func_index) {
    if (!bc || func_index < 0 || func_index >= bc->func_count) return -1;
    const char* fn = bc->funcs[func_index].name;
    if (!fn) return -1;

    const char* dot = strchr(fn, '.');
    if (!dot || dot == fn) return -1;

    size_t n = (size_t)(dot - fn);
    for (int i = 0; i < bc->class_count; i++) {
        const char* cn = bc->classes[i].name;
        if (cn && strlen(cn) == n && memcmp(cn, fn, n) == 0) return i;
    }
    return -1;
}

// ============================================================
// 平台内建（BC_BUILTIN）
// ============================================================
//
// 一条 BC_BUILTIN 承载 17 个窗口函数：`a` 是 XBI_*（函数编号），
// `b` 是参数个数。实参已经由前面的指令按顺序压进栈，这里负责
// 「弹 b 个、压 1 个返回值」。
//
// ── 为什么参数校验要写出这么啰嗦的信息 ──────────────────────
//
// 这些函数是语言里唯一「参数类型不像普通函数那样被编译器检查」
// 的部分：编译器只数了个数，没管类型（因为参数类型写在
// g_builtins 表里，compile.c 不做逐参数检查）。所以类型错误
// 只能在运行时被发现，而运行时用户手上只有一行报错。
//
// 于是这里对每个参数都给「实际是什么 → 期望是什么 → 怎么办」。
// 代价是几十行，收益是用户在第一次写错时就能自己改对。

// 弹出一个参数并断言类型为 Int，同时把值取出来。
// 用宏而不是函数，是为了让 rt_error 拿到正确的 pos 与 param 名。
//
// 第二个实参是**参数序号（1 起）**，只用于报错信息。它跟弹栈顺序
// 相反：winFillRect(x,y,w,h,color) 先弹 color（第 5 个）。
// 写成 `BI_ARG_INT(1)` 而不是算偏移，是因为偏移量算错时症状是
// 「报错说第 5 个参数错，其实是第 1 个」，比 bug 本身还难查。
#define BI_ARG_INT(n)    bi_take_int(vm, pos, (n), name)
#define BI_ARG_NUM(n)    bi_take_num(vm, pos, (n), name)

static int64_t bi_take_int(VM* vm, SrcPos pos, int which, const char* fn) {
    Value v = vm_pop(vm, pos);
    if (v.type != VT_INT) {
        rt_error(pos,
                 "`%s` 的第 %d 个参数必须是 Int，实际是 %s\n"
                 "  note: 这些函数的签名固定，参数类型不会自动转换\n"
                 "  help: 用 `toInt(x)` 显式转换，或检查是否传了字符串/数组",
                 fn, which, type_label(v.type));
        exit(70);
    }
    return v.i;
}

// 接受 Int 或 Float，统一按 double 返回。
// 只有 winSleep 用得上：`winSleep(0.5)` 给出的是 Float，
// 按 Int 严格校验会报错，但「睡 0.5 毫秒」语义上合理（截断为 0）。
static double bi_take_num(VM* vm, SrcPos pos, int which, const char* fn) {
    Value v = vm_pop(vm, pos);
    if (v.type == VT_INT)   return (double)v.i;
    if (v.type == VT_FLOAT) return v.f;
    rt_error(pos,
             "`%s` 的第 %d 个参数必须是数值（Int 或 Float），实际是 %s\n"
             "  help: 颜色是 0xRRGGBB 形式的 Int，坐标也是 Int",
             fn, which, type_label(v.type));
    exit(70);
}

// ============================================================
// 数学内建
// ============================================================

// 执行一条数学指令。
//
// 参数从栈里按**从右往左**弹，所以有多个参数时要倒着取。
// 这里用一个小数组一次弹完再按正序用，比逐个倒序写更不容易错。
//
// ── 为什么要保留原始 Value 而不是只留 double ─────────────────
//
// min/max/clamp 在编译期声明为「保持输入类型」（TY_UNKNOWN 走推断，
// 两个 Int 得 Int）。如果运行时统一返回 Float，那么
//   let i: Int = math(min, 2, 3)
// 会在运行时拿到 Float 3.0，与编译期的判断矛盾 —— 类型检查放过了，
// 值却不匹配，后续 Int 运算会得到 Float 结果而悄悄污染整条链。
// 所以这里同时留 Value（判类型）和 double（做计算）。
static Value bi_math(VM* vm, SrcPos pos, int encoded) {
    int op   = xmath_op(encoded);
    int argc = xmath_argc(encoded);
    double args[4] = {0, 0, 0, 0};
    bool   all_int = true;

    // 参数从右往左弹，但下标要正着填。
    for (int i = argc - 1; i >= 0; i--) {
        Value v = vm_pop(vm, pos);
        if (v.type != VT_INT && v.type != VT_FLOAT) {
            rt_error(pos,
                     "`math` 的第 %d 个参数必须是数值（Int 或 Float），实际是 %s\n"
                     "  help: 用 toFloat(x) 或 strParseFloat(s) 先转成数值",
                     i + 1, type_label(v.type));
            exit(70);
        }
        if (v.type != VT_INT) all_int = false;
        args[i] = (v.type == VT_INT) ? (double)v.i : v.f;
    }

    switch (op) {
        case XMO_POW:   return v_float(pow(args[0], args[1]));
        case XMO_SQRT:  return v_float(sqrt(args[0]));
        case XMO_CBRT:  return v_float(cbrt(args[0]));
        case XMO_FLOOR: return v_int((int64_t)floor(args[0]));
        case XMO_CEIL:  return v_int((int64_t)ceil(args[0]));
        case XMO_ROUND: return v_int((int64_t)llround(args[0]));
        case XMO_TRUNC: return v_int((int64_t)trunc(args[0]));
        case XMO_ABS: {
            // 保持输入类型。`math(abs, -3)` 得到 Int 3，
            // `math(abs, -3.5)` 得到 Float 3.5 —— 与 min/max 同一套规则。
            double r = fabs(args[0]);
            return all_int ? v_int((int64_t)r) : v_float(r);
        }
        case XMO_SIGN:  return v_int(args[0] > 0 ? 1 : (args[0] < 0 ? -1 : 0));

        // min/max/clamp 保持输入类型。编译期已经算好返回类型，
        // 但运行时也要按同样规则给值，否则 `let i: Int = math(min,2,3)`
        // 会拿到 Float 而通不过类型检查。
        //
        // 全 Int → 返回 Int；混入任何 Float → 返回 Float。这与编译期
        // compile_math_call 的推断规则逐字对应（a0==INT && a1==INT）。
        case XMO_MIN: {
            double r = args[0] < args[1] ? args[0] : args[1];
            return all_int ? v_int((int64_t)r) : v_float(r);
        }
        case XMO_MAX: {
            double r = args[0] > args[1] ? args[0] : args[1];
            return all_int ? v_int((int64_t)r) : v_float(r);
        }
        case XMO_CLAMP: {
            double x = args[0], lo = args[1], hi = args[2];
            if (hi < lo) { double t = lo; lo = hi; hi = t; }   // 容错：反了就换
            double r = x < lo ? lo : (x > hi ? hi : x);
            return all_int ? v_int((int64_t)r) : v_float(r);
        }

        case XMO_SIN:   return v_float(sin(args[0]));
        case XMO_COS:   return v_float(cos(args[0]));
        case XMO_TAN:   return v_float(tan(args[0]));
        case XMO_ASIN:  return v_float(asin(args[0]));
        case XMO_ACOS:  return v_float(acos(args[0]));
        case XMO_ATAN:  return v_float(atan(args[0]));
        case XMO_ATAN2: return v_float(atan2(args[0], args[1]));
        case XMO_LOG:   return v_float(log(args[0]));
        case XMO_LOG2:  return v_float(log2(args[0]));
        case XMO_LOG10: return v_float(log10(args[0]));
        case XMO_EXP:   return v_float(exp(args[0]));
        case XMO_HYPOT: return v_float(hypot(args[0], args[1]));
        case XMO_FMOD:  return v_float(fmod(args[0], args[1]));
        default:
            rt_error(pos, "内部错误：未知的数学子操作 %d", op);
            exit(70);
    }
    return v_float(0.0);
}

// xorshift128+ 伪随机数。
//
// ── 为什么不用 rand() ───────────────────────────────────────
//
// MSVCRT 的 rand() 周期只有 32768，低位几乎不随机。标准库里的
// shuffle / sample / 随机矩阵初始化都会立刻暴露这一点：洗出来的
// 牌有明显的规律。xorshift128+ 状态 128 位、周期 2^128-1，
// 十来行代码，质量足够。
static uint64_t s_rng_s[2] = { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };
static bool     s_rng_seeded = false;

static void bi_random_seed(uint64_t seed) {
    // SplitMix64 把单个 64 位种子摊开到两个状态字。
    // 直接 s[0]=seed, s[1]=1 会让相邻种子产生高度相关的序列。
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    s_rng_s[0] = z ^ (z >> 31);

    z = seed + 0x9E3779B97F4A7C15ULL * 2;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    s_rng_s[1] = z ^ (z >> 31);

    if (s_rng_s[0] == 0 && s_rng_s[1] == 0) s_rng_s[0] = 1;   // 全零状态会卡死
    s_rng_seeded = true;
}

static double bi_random_unit(void) {
    // 首次使用时按时间播种。用 time(NULL) 而不是 rand()：
    // rand() 未播种时在每个进程里都是同一个序列，两次运行结果相同，
    // 而用户对 random() 的预期是「每次不一样」。
    if (!s_rng_seeded) {
        bi_random_seed((uint64_t)time(NULL) * 2654435761u + (uint64_t)plat_pid());
    }
    uint64_t x = s_rng_s[0];
    const uint64_t y = s_rng_s[1];
    s_rng_s[0] = y;
    x ^= x << 23;
    s_rng_s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    // 取高 53 位映射到 [0,1)：double 的有效尾数是 52 位，
    // 用 53 位保证不损失精度，且右端开区间（不会返回 1.0）。
    return (double)((s_rng_s[1] + y) >> 11) * (1.0 / 9007199254740992.0);
}


// 取出字符串参数，把 Value 写进出参 out，返回其数据指针。
//
// ── 谁负责释放 ──────────────────────────────────────────────
//
// 返回的 Value 由调用方 value_release。本函数不释放，因为调用点
// 可能还要用 len 之类的字段。约定：拿到 ptr 就必须管住 out。
// 不这么做的话，窗口一帧画几十行字，每个字符串泄漏一次。
static const char* bi_take_str(VM* vm, SrcPos pos, int which, const char* fn, Value* out) {
    Value v = vm_pop(vm, pos);
    if (v.type != VT_STRING) {
        rt_error(pos,
                 "`%s` 的第 %d 个参数必须是 String，实际是 %s\n"
                 "  help: 文字参数要加引号，例如 winText(10, 10, \"HELLO\", 0xFFFFFF)",
                 fn, which, type_label(v.type));
        exit(70);
    }
    *out = v;
    return v.s->data;
}

// 数组元素写入辅助：`[x, y, button]`
static Value bi_click_array(int x, int y, int button) {
    XArr* a = arr_new(3, v_null());
    a->data[0] = v_int(x);
    a->data[1] = v_int(y);
    a->data[2] = v_int(button);
    return (Value){ .type = VT_ARRAY, .a = a };
}

// ============================================================
// 反射辅助
// ============================================================

// 可见性的可读名字。反射把它作为字符串返回给语言层 ——
// 用字符串而不是数字的理由见 run_builtin 里 XBI_CLASS_FIELDS 的说明。
static const char* vis_label(Visibility v) {
    switch (v) {
        case VIS_PROTECTED: return "protected";
        case VIS_PRIVATE:   return "private";
        default:            return "public";
    }
}

// 在类（含继承来的字段）里按名字找字段下标，找不到返回 -1。
//
// 字段表本来就是全量（父类在前），所以这里不需要沿父链走 ——
// 一次线性扫描就够。字段数量级是「几个到几十个」，二分没有意义。
static int rt_find_field(ClassType* ct, const char* name) {
    if (!ct || !ct->field_names) return -1;
    for (int i = 0; i < ct->field_count; i++) {
        if (ct->field_names[i] && strcmp(ct->field_names[i], name) == 0) return i;
    }
    return -1;
}

// 反射访问的可见性检查。
//
// ── 为什么反射必须守可见性 ──────────────────────────────────
//
// 否则封装就是个装饰：`fieldGet(obj, "password")` 一行就把 private
// 绕过去了。反射是「运行时按名字访问」的能力，不是「无视访问控制」
// 的能力 —— 这两件事经常被混为一谈，代价是整个私有体系失效。
//
// 判据与编译期完全一致：private 只有本类，protected 本类+子类，
// public 谁都能。当前类从帧栈取（见 vm_cur_class）。
//
// 注意「本类」的判据是 cur == owner，「子类」用 bc_class_is_a ——
// 与 compile.c 的字段检查用的是同一套算法，两处不一致的话，
// 同一个访问「直接写被拒、反射放行」就成了绕过封装的漏洞。
static void rt_check_field_access(VM* vm, int ci, int fi,
                                  SrcPos pos, const char* act) {
    ClassType* ct = &vm->bc->classes[ci];
    Visibility fv = ct->field_vis ? ct->field_vis[fi] : VIS_PUBLIC;
    if (fv == VIS_PUBLIC) return;

    // ── 字段属于哪个类 ──────────────────────────────────────
    //
    // 字段表是**全量**的（父类在前），所以 fi 是一个全局下标，
    // 不是「本类内的偏移」。要找声明者，就从 ct 开始沿父链往上，
    // 第一个满足「fi 落在它的自有字段区间内」的类就是它。
    //
    // ── 第一版写错的地方 ────────────────────────────────────
    //
    // 第一版从 ct->parent_idx 起步，于是「字段属于 ct 自己」这个
    // 最常见的情况被漏掉了（它直接去父类找，找不到就退化成
    // allowed = fi >= ct->own_field_start）。而那个退化判据恰好
    // 在单类场景下总成立 —— 于是检查彻底失效，private 字段谁都
    // 能反射读到，且没有任何报错。
    //
    // 正确起点是 ct 自己：`fi >= ct->own_field_start` 就意味着
    // 它是本类声明的。
    int owner = -1;
    for (int c = ci; c >= 0; c = vm->bc->classes[c].parent_idx) {
        ClassType* o = &vm->bc->classes[c];
        if (fi >= o->own_field_start) { owner = c; break; }
    }
    int cur = vm_cur_class(vm);

    bool allowed = false;
    if (cur >= 0 && owner >= 0) {
        if (cur == owner) {
            allowed = true;              // 本类
        } else if (fv == VIS_PROTECTED) {
            // 子类可以访问父类的 protected（IS-A 关系）
            allowed = bc_class_is_a(vm->bc, cur, owner);
        }
    }
    if (allowed) return;

    rt_error(pos,
             "反射%s字段 `%s.%s` 被拒绝：它是 %s 的，而当前不在允许的类里\n"
             "  note: 反射不绕过访问控制 —— 否则 private 就形同虚设\n"
             "  help: 需要外部访问就提供公开方法，或在同类内部使用",
             act, ct->name, ct->field_names[fi], vis_label(fv));
    exit(70);
}

// 把 target 字符串转成 AnnoTarget。不认识就报错。
//
// 用字符串而不是数字，理由与 classFields 里 vis 用字符串一致：
// 调用点写 `annoHas(u, "class", "Entity")` 读起来就是结论，写错
// 会在运行时给出一条列出全部合法取值的错误，而不是静默取错。
static bool anno_target_from(const char* s, AnnoTarget* out) {
    if (strcmp(s, "class") == 0)  { *out = ANNO_TARGET_CLASS;  return true; }
    if (strcmp(s, "field") == 0)  { *out = ANNO_TARGET_FIELD;  return true; }
    if (strcmp(s, "method") == 0) { *out = ANNO_TARGET_METHOD; return true; }
    return false;
}

// 反射查询的接收者解析：接受类实例或类名字符串，返回类索引。
//
// ── 为什么除了实例还要收类名（v0.20）──────────────────────
//
// 这批查询原本只收实例。注解驱动的框架在扫描 @Component 时手里
// 只有**类名**（classAll() 的输出）——如果必须先 new 一个实例才能
// 查注解，构造函数就会带着尚未注入的依赖跑起来（连数据库、开
// 监听都是副作用），顺序整个反了。收类名之后，扫描才能是
// 「查名字 → 过滤 → 命中的才实例化」。
//
// x 既不是实例也不是类名时报错退出，与内建层的错误路径一致。
static int rt_cls_index(Bytecode* bc, Value v, SrcPos pos, const char* fn) {
    if (v.type == VT_CLASS) {
        int ci = v.k->cls_idx;
        if (ci < 0 || ci >= bc->class_count) {
            rt_error(pos, "内部错误：实例的类索引 %d 越界", ci);
            exit(70);
        }
        return ci;
    }
    if (v.type == VT_STRING) {
        for (int i = 0; i < bc->class_count; i++) {
            const char* cn = bc->classes[i].name;
            if (cn && strcmp(cn, v.s->data) == 0) return i;
        }
        rt_error(pos,
                 "`%s` 找不到类 `%s`\n"
                 "  help: 用 classAll() 看字节码里有哪些类名",
                 fn, v.s->data);
        exit(70);
    }
    rt_error(pos,
             "`%s` 的接收者必须是类实例或类名字符串，实际是 %s\n"
             "  help: 类实例是 new 出来的对象；类名是带引号的字符串",
             fn, type_label(v.type));
    exit(70);
}

// 执行一条 BC_BUILTIN。返回要压入栈的返回值。
//
// 出错时直接 exit(70)，与解释器其余部分一致 —— 平台内建的错误
// 无法恢复（窗口建不出来、参数类型错的程序继续跑只会更乱）。
static Value run_builtin(VM* vm, SrcPos pos, const Insn* in) {
    XBuiltin id  = (XBuiltin)in->a;
    // 反射内建要读类表 —— 从 VM 取，不额外传参（调用点有十几处，
    // 加参数全都要改，而 VM 里本来就有 bc）。
    Bytecode* bc = vm->bc;
    const char* name = "builtin";
    // 从编号反查名字，只为了错误信息可读。
    // 这一步是 O(17)，但只在出错路径上走。
    for (int i = 0; i < bc_builtin_count(); i++) {
        if (bc_builtin_id(i) == id) { name = bc_builtin_name(i); break; }
    }

    switch (id) {
        // ---------------- 窗口生命周期 ----------------
        case XBI_WIN_OPEN: {
            // 参数顺序与签名一致：(title, w, h)。
            // 弹栈顺序是反的，所以先弹 h、再 w、最后 title。
            int64_t h = BI_ARG_INT(3);
            int64_t w = BI_ARG_INT(2);
            Value tv; const char* title = bi_take_str(vm, pos, 1, name, &tv);
            if (w <= 0 || h <= 0) {
                rt_error(pos,
                         "窗口尺寸必须是正数，实际是 %lld × %lld\n"
                         "  help: winOpen(title, width, height) 的单位是像素",
                         (long long)w, (long long)h);
                exit(70);
            }
            int ok = xgui_open(title, (int)w, (int)h);
            value_release(&tv);
            if (!ok) {
                rt_error(pos,
                         "无法创建窗口\n"
                         "  note: 当前环境没有可用的图形界面，或窗口系统拒绝创建\n"
                         "  help: 确认不是在没有显示器的会话里运行；"
                         "类 Unix 上窗口功能尚未实现");
                // 这里不退化成失败返回值：调用者几乎一定没有检查它，
                // 继续跑会在下一次 fillRect 上崩在更莫名其妙的地方。
                // 直接给一条准确的原因更有用。
                exit(70);
            }
            return v_bool(true);
        }

        case XBI_WIN_CLOSE:
            xgui_close();
            return v_null();

        case XBI_WIN_SHOULD_CLOSE:
            return v_bool(xgui_should_close() != 0);

        case XBI_WIN_BEGIN_FRAME:
            xgui_begin_frame();
            return v_null();

        case XBI_WIN_END_FRAME:
            xgui_end_frame();
            return v_null();

        // ---------------- 绘制 ----------------
        case XBI_WIN_FILL_RECT: {
            int64_t color = BI_ARG_INT(5);
            int64_t h     = BI_ARG_INT(4);
            int64_t w     = BI_ARG_INT(3);
            int64_t y     = BI_ARG_INT(2);
            int64_t x     = BI_ARG_INT(1);
            xgui_fill_rect((int)x, (int)y, (int)w, (int)h, (XColor)color);
            return v_null();
        }

        case XBI_WIN_BORDER_RECT: {
            int64_t color     = BI_ARG_INT(6);
            int64_t thickness = BI_ARG_INT(5);
            int64_t h         = BI_ARG_INT(4);
            int64_t w         = BI_ARG_INT(3);
            int64_t y         = BI_ARG_INT(2);
            int64_t x         = BI_ARG_INT(1);
            xgui_border_rect((int)x, (int)y, (int)w, (int)h, (int)thickness, (XColor)color);
            return v_null();
        }

        case XBI_WIN_TEXT: {
            int64_t color = BI_ARG_INT(4);
            Value sv; const char* text = bi_take_str(vm, pos, 3, name, &sv);
            int64_t y = BI_ARG_INT(2);
            int64_t x = BI_ARG_INT(1);
            xgui_text((int)x, (int)y, text, (XColor)color);
            value_release(&sv);
            return v_null();
        }

        // ---------------- 形状 / 样式（v0.17）----------------
        //
        // ── 参数弹出的顺序规则 ──────────────────────────────
        //
        // 全部按「从最后一个参数往第一个」弹。这不是风格问题：
        // 栈上实参按位置压入，弹栈必然逆序。写错顺序的症状是
        // 「报错说第 3 个参数类型不对，其实是我传的第 5 个」——
        // 因为 BI_ARG_INT(n) 里的 n 只是报错标签，不参与取值。
        //
        // 所以每个 case 里都能看到「先写最后一个、最后写第一个」
        // 这个倒序结构，它是刻意的。

        case XBI_WIN_FILL_ROUND: {
            int64_t color  = BI_ARG_INT(6);
            int64_t radius = BI_ARG_INT(5);
            int64_t h      = BI_ARG_INT(4);
            int64_t w      = BI_ARG_INT(3);
            int64_t y      = BI_ARG_INT(2);
            int64_t x      = BI_ARG_INT(1);
            xgui_fill_round((int)x, (int)y, (int)w, (int)h, (int)radius, (XColor)color);
            return v_null();
        }

        case XBI_WIN_BORDER_ROUND: {
            int64_t color     = BI_ARG_INT(7);
            int64_t thickness = BI_ARG_INT(6);
            int64_t radius    = BI_ARG_INT(5);
            int64_t h         = BI_ARG_INT(4);
            int64_t w         = BI_ARG_INT(3);
            int64_t y         = BI_ARG_INT(2);
            int64_t x         = BI_ARG_INT(1);
            xgui_border_round((int)x, (int)y, (int)w, (int)h, (int)radius,
                              (int)thickness, (XColor)color);
            return v_null();
        }

        case XBI_WIN_ELLIPSE: {
            int64_t color = BI_ARG_INT(5);
            int64_t h     = BI_ARG_INT(4);
            int64_t w     = BI_ARG_INT(3);
            int64_t y     = BI_ARG_INT(2);
            int64_t x     = BI_ARG_INT(1);
            xgui_ellipse((int)x, (int)y, (int)w, (int)h, (XColor)color);
            return v_null();
        }

        case XBI_WIN_PATH: {
            // (points: Array, color: Int)
            //
            // 点集是「平铺的一维数组」[x0,y0,x1,y1,...]，不是「点对象数组」。
            // 后者要每个点建一个对象，而绘制路径每帧都在建 —— 分配量太大。
            // 平铺数组还能直接 memcpy 成 POINT[]，省一层转换。
            int64_t color = BI_ARG_INT(2);
            Value av = vm_pop(vm, pos);          // points
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`winPath` 的第 1 个参数必须是 Array，实际是 %s\n"
                              "  help: 写成 [x0, y0, x1, y1, ...] 的平铺数组",
                         type_label(av.type));
                exit(70);
            }
            int n2 = av.a ? (int)av.a->len : 0;
            if (n2 < 6 || (n2 % 2) != 0) {
                rt_error(pos, "`winPath` 的点集长度必须是偶数且至少 6 个数字（3 个点），实际 %d 个\n"
                              "  note: 2 个点连不成面，画出来是一条线 —— 那不是路径",
                         n2);
                exit(70);
            }
            // 逐项取 Int。这里不能直接读数组内部的 Value 数组长度就完事：
            // 元素可能是 Float 或别的类型，必须逐个校验，否则会把
            // 一个 Float 的位模式当 Int 读，画出完全无关的形状。
            int pn = n2 / 2;
            int* flat = (int*)malloc((size_t)n2 * sizeof(int));
            if (!flat) { rt_error(pos, "`winPath` 分配临时缓冲失败"); exit(70); }
            for (int i = 0; i < n2; i++) {
                Value e = av.a->data[i];
                if (e.type != VT_INT) {
                    free(flat);
                    value_release(&av);
                    rt_error(pos, "`winPath` 点集的第 %d 项必须是 Int，实际是 %s\n"
                                  "  help: 坐标写成整数，或先用 toInt(x) 转换",
                             i + 1, type_label(e.type));
                    exit(70);
                }
                flat[i] = (int)e.i;
            }
            xgui_path(flat, pn, (XColor)color);
            free(flat);
            value_release(&av);
            return v_null();
        }

        case XBI_WIN_GRADIENT: {
            // (x, y, w, h, c1, c2, vertical: Bool)
            Value bv = vm_pop(vm, pos);          // vertical
            int vertical = 0;
            if (bv.type == VT_BOOL) vertical = bv.b ? 1 : 0;
            else if (bv.type == VT_INT) vertical = bv.i ? 1 : 0;
            else {
                rt_error(pos, "`winGradient` 的第 7 个参数必须是 Bool，实际是 %s\n"
                              "  help: 纵向渐变写 true，横向写 false",
                         type_label(bv.type));
                exit(70);
            }
            int64_t c2 = BI_ARG_INT(6);
            int64_t c1 = BI_ARG_INT(5);
            int64_t h  = BI_ARG_INT(4);
            int64_t w  = BI_ARG_INT(3);
            int64_t y  = BI_ARG_INT(2);
            int64_t x  = BI_ARG_INT(1);
            xgui_gradient((int)x, (int)y, (int)w, (int)h,
                          (XColor)c1, (XColor)c2, vertical);
            return v_null();
        }

        case XBI_WIN_PUSH_TRANSFORM: {
            // (pivotX, pivotY, rotDeg: Float, scale: Float)
            double scale = BI_ARG_NUM(4);
            double rot   = BI_ARG_NUM(3);
            double py    = BI_ARG_NUM(2);
            double px    = BI_ARG_NUM(1);
            xgui_push_transform(px, py, rot, scale);
            return v_null();
        }

        case XBI_WIN_POP_TRANSFORM:
            xgui_pop_transform();
            return v_null();

        case XBI_WIN_ALPHA: {
            double a = BI_ARG_NUM(1);
            xgui_set_alpha(a);
            return v_null();
        }

        case XBI_WIN_POP_ALPHA:
            xgui_pop_alpha();
            return v_null();

        case XBI_WIN_CLIP: {
            int64_t h = BI_ARG_INT(4);
            int64_t w = BI_ARG_INT(3);
            int64_t y = BI_ARG_INT(2);
            int64_t x = BI_ARG_INT(1);
            xgui_clip((int)x, (int)y, (int)w, (int)h);
            return v_null();
        }

        case XBI_WIN_UNCLIP:
            xgui_unclip();
            return v_null();

        case XBI_WIN_TEXT_ROT: {
            double rot = BI_ARG_NUM(5);
            int64_t color = BI_ARG_INT(4);
            Value sv; const char* text = bi_take_str(vm, pos, 3, name, &sv);
            int64_t y = BI_ARG_INT(2);
            int64_t x = BI_ARG_INT(1);
            xgui_text_rot((int)x, (int)y, text, (XColor)color, rot);
            value_release(&sv);
            return v_null();
        }

        case XBI_WIN_TEXT_WIDTH: {
            Value sv; const char* text = bi_take_str(vm, pos, 1, name, &sv);
            int w = xgui_text_width(text);
            value_release(&sv);
            return v_int(w);
        }

        case XBI_WIN_TEXT_HEIGHT:
            return v_int(xgui_text_height());

        // ---------------- 输入 ----------------
        case XBI_WIN_POLL_CLICK: {
            int x = 0, y = 0, btn = 0;
            if (!xgui_poll_click(&x, &y, &btn)) return v_null();
            return bi_click_array(x, y, btn);
        }

        case XBI_WIN_MOUSE_X:
            return v_int(xgui_mouse_x());

        case XBI_WIN_MOUSE_Y:
            return v_int(xgui_mouse_y());

        case XBI_WIN_MOUSE_DOWN: {
            int64_t btn = BI_ARG_INT(1);
            return v_bool(xgui_mouse_down((int)btn) != 0);
        }

        // ---------------- 时间 ----------------
        case XBI_WIN_TIME:
            return v_float(xgui_time());

        case XBI_WIN_SLEEP: {
            // 用 num 而不是 int：`winSleep(0.5)` 这种写法给出的是
            // Float 0.5，按 Int 严格校验会报错，但语义上「睡 0.5 毫秒」
            // 是合理的（截断成 0 就是不休眠，符合直觉）。
            double ms = BI_ARG_NUM(1);
            xgui_sleep((int)ms);
            return v_null();
        }

        case XBI_WIN_SCREENSHOT: {
            Value sv; const char* path = bi_take_str(vm, pos, 1, name, &sv);
            int ok = xgui_screenshot(path);
            value_release(&sv);
            if (!ok) {
                rt_error(pos, "截图失败：无法写入 `%s`\n"
                              "  help: 确认目录存在；窗口尚未打开时也会失败", path);
                exit(70);
            }
            return v_bool(true);
        }

        // ---- 通用 ----
        //
        // 与 winSleep 语义相同（都是睡 ms 毫秒），区别只在它不算窗口
        // 能力。参数按数值取：`sleep(0.5)` 给出的是 Float，严格按 Int
        // 校验会报错，而「睡半毫秒」截断成 0 是符合直觉的。
        case XBI_SLEEP: {
            double ms = BI_ARG_NUM(1);
            plat_sleep_ms((int)ms);
            return v_null();
        }

        // ---- 时间 ----
        //
        // 通用墙上时钟。和 winTime 一样走平台层的时钟，但不属于窗口
        // 能力 —— 纯控制台程序也能用它量耗时。
        case XBI_NOW_MILLIS:
            return v_int(plat_now_ms());

        case XBI_NOW_SECONDS:
            return v_int(plat_now_ms() / 1000);

        // 单调时钟（毫秒）。仅供 elapsedMs 这类差值使用。
        case XBI_MONO_MILLIS:
            return v_int(plat_mono_ms());

        // ---- 字节 ----
        //
        // strFromBytes(arr)：把 0..255 的整数数组按字节原样拼成字符串。
        //
        // 不做字符语义、不做编码转换 —— 契约就是「这些字节，原样」。
        // 越界值钳到 0..255 而不是报错：调用方算错一个索引时，
        // 让它拿到一个可预测的值比让整个程序停掉更有用（与服务端
        // 处理坏请求的态度一致）。
        case XBI_STR_FROM_BYTES: {
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "strFromBytes 的参数必须是 Array（字节数组），实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            XArr* a = av.a;
            // 先按元素个数分配上限（每个元素最多贡献 1 字节）
            char* buf = (char*)x_alloc(a->len > 0 ? a->len : 1);
            size_t w = 0;
            for (size_t i = 0; i < a->len; i++) {
                Value el = a->data[i];
                int64_t b = 0;
                if (el.type == VT_INT) b = el.i;
                else if (el.type == VT_FLOAT) b = (int64_t)el.f;
                else if (el.type == VT_BOOL) b = el.b ? 1 : 0;
                if (b < 0) b = 0;
                if (b > 255) b = 255;
                buf[w++] = (char)(unsigned char)b;
            }
            XStr* out = str_new(buf, w);
            free(buf);
            value_release(&av);
            return v_str(out);
        }

        // strBytes(s)：把字符串拆成字节值数组（0..255）。
        //
        // 与 strFromBytes 成对。注意它返回的是**字节**，不是码点：
        // "中" 给出 [228, 184, 173] 三个元素。要码点请用 strAt。
        case XBI_STR_BYTES: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "strBytes 的参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            XArr* a = arr_new(0, v_null());
            for (size_t i = 0; i < sv.s->len; i++) {
                arr_push(a, v_int((int64_t)(unsigned char)sv.s->data[i]));
            }
            value_release(&sv);
            return v_arr(a);
        }

        case XBI_NONE:
        case XBI__WIN_END:      // 窗口段的边界哨兵，没有可执行语义
        case XBI_COUNT:
            break;

        // ============================================================
        // 数组
        // ============================================================
        //
        // 这批函数的参数弹栈顺序与其它内建一致：从右往左。
        // 统一用 BI_ARG_* 宏，报错时能指出是第几个参数。
        //
        // ── 为什么 arrPush 返回新长度 ────────────────────────────
        //
        // 返回值是表达式必须有值这条规则的要求（X 没有 void
        // 的表达式值，void 用 null 表示）。顺手返回长度实用：
        // `if arrPush(a, x) > 10 { ... }` 可以直接写，不用再 len(a)。
        // 而且它让「push 成功了」这件事在类型上是 Int（非 null），
        // 比返回 null 更容易在调试时看见。

        case XBI_ARR_PUSH: {
            // (arr, value) -> Int
            Value v = vm_pop(vm, pos);          // value
            Value av = vm_pop(vm, pos);         // arr
            if (av.type != VT_ARRAY) {
                value_release(&v);
                rt_error(pos, "`arrPush` 的第 1 个参数必须是 Array，实际是 %s\n"
                              "  help: 数组要先 `let a = array(0, 0)` 建出来",
                         type_label(av.type));
                exit(70);
            }
            // v 的所有权直接转给数组，不 retain 也不再 release ——
            // 栈上那份已经弹出来了，是唯一持有者。
            arr_push(av.a, v);
            return v_int((int64_t)av.a->len);
        }

        case XBI_ARR_POP: {
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrPop` 的参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            if (av.a->len == 0) return v_null();   // 空数组返回 null，不报错
            // 弹出的值连同数组持有的那份引用一起转移给调用方：
            // 直接返回槽里的 Value，不做 release —— len 减一之后那个
            // 槽不再属于数组，所有权跟着返回值走。
            Value out = av.a->data[--av.a->len];
            return out;
        }

        case XBI_ARR_INSERT: {
            // (arr, index, value) -> Int
            Value v  = vm_pop(vm, pos);
            int64_t ix = BI_ARG_INT(2);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                value_release(&v);
                rt_error(pos, "`arrInsert` 的第 1 个参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            // 允许在 len 处插入（等价于 push），这是最常用的边界情况：
            // `arrInsert(a, len(a), x)`。负数与超出 len 都报错，不做
            // 「负数从尾部数」那套 —— 那会让 arrInsert(a,-1,x) 有两种
            // 可能的解释，用户必须查文档。
            if (ix < 0 || (size_t)ix > av.a->len) {
                value_release(&v);
                rt_error(pos, "`arrInsert` 的下标越界：写入 [%lld]，数组长度 %zu\n"
                              "  note: 合法下标是 0 到 %zu（含，等于在末尾追加）",
                         (long long)ix, av.a->len, av.a->len);
                exit(70);
            }
            arr_reserve(av.a, av.a->len + 1);
            // 后移腾位。memmove 而不是 memcpy：源与目标重叠，
            // memcpy 在重叠区的行为未定义 —— 而这段代码「看起来」没问题，
            // 在部分平台上碰巧能跑，是最难查的一类 bug。
            memmove(&av.a->data[ix + 1], &av.a->data[ix],
                    (av.a->len - (size_t)ix) * sizeof(Value));
            av.a->data[ix] = v;
            av.a->len++;
            return v_int((int64_t)av.a->len);
        }

        case XBI_ARR_REMOVE: {
            // 弹栈顺序：下标在栈顶（最后求值），数组在其下。
            int64_t ix = BI_ARG_INT(2);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrRemove` 的第 1 个参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            if (ix < 0 || (size_t)ix >= av.a->len) {
                rt_error(pos, "`arrRemove` 的下标越界：删除 [%lld]，数组长度 %zu\n"
                              "  note: 合法下标是 0 到 %zu",
                         (long long)ix, av.a->len, av.a->len ? av.a->len - 1 : 0);
                exit(70);
            }
            Value out = av.a->data[ix];   // 所有权转移给返回值
            memmove(&av.a->data[ix], &av.a->data[ix + 1],
                    (av.a->len - (size_t)ix - 1) * sizeof(Value));
            av.a->len--;
            return out;
        }

        case XBI_ARR_SLICE: {
            // (arr, start, end) -> Array，左闭右开，与 for i in a..b 一致
            int64_t end = BI_ARG_INT(3);
            int64_t start = BI_ARG_INT(2);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrSlice` 的第 1 个参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            size_t n = av.a->len;
            // 负数与越界做钳制而不是报错。理由：切片最常用的场景是
            // 「取前 N 个」和「取后 N 个」，调用方常常算不出精确的 N
            // （比如 len(xs) - 3 在 xs 很短时是负数）。钳制让这类代码
            // 自然工作，而报错会让每个调用点都写一遍边界判断。
            if (start < 0) start = 0;
            if (end < 0) end = 0;
            if ((size_t)start > n) start = (int64_t)n;
            if ((size_t)end > n) end = (int64_t)n;
            if (end < start) end = start;

            XArr* out = arr_new(0, v_null());
            arr_reserve(out, (size_t)(end - start));
            for (int64_t i = start; i < end; i++) {
                Value el = av.a->data[i];
                value_retain(&el);   // 新数组持有一份
                out->data[out->len++] = el;
            }
            return v_arr(out);
        }

        case XBI_ARR_CONCAT: {
            Value bv = vm_pop(vm, pos);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY || bv.type != VT_ARRAY) {
                rt_error(pos, "`arrConcat` 的两个参数都必须是 Array，实际是 %s 和 %s",
                         type_label(av.type), type_label(bv.type));
                exit(70);
            }
            size_t total = av.a->len + bv.a->len;
            XArr* out = arr_new(0, v_null());
            arr_reserve(out, total);
            for (size_t i = 0; i < av.a->len; i++) {
                Value el = av.a->data[i];
                value_retain(&el);
                out->data[out->len++] = el;
            }
            for (size_t i = 0; i < bv.a->len; i++) {
                Value el = bv.a->data[i];
                value_retain(&el);
                out->data[out->len++] = el;
            }
            return v_arr(out);
        }

        case XBI_ARR_REVERSE: {
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrReverse` 的参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            // 原地反转。元素只换位置、不动引用计数 ——
            // 引用计数属于「这个数组持有它」，位置变化不影响持有关系。
            size_t i = 0, j = av.a->len ? av.a->len - 1 : 0;
            while (i < j) {
                Value t = av.a->data[i];
                av.a->data[i] = av.a->data[j];
                av.a->data[j] = t;
                i++; j--;
            }
            return v_null();
        }

        case XBI_ARR_INDEX_OF:
        case XBI_ARR_CONTAINS: {
            // (arr, value) —— 两个函数共用一个实现，只是返回不同
            Value needle = vm_pop(vm, pos);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                value_release(&needle);
                rt_error(pos, "`%s` 的第 1 个参数必须是 Array，实际是 %s",
                         name, type_label(av.type));
                exit(70);
            }
            int64_t found = -1;
            for (size_t i = 0; i < av.a->len; i++) {
                if (value_equal(&av.a->data[i], &needle)) { found = (int64_t)i; break; }
            }
            value_release(&needle);
            if (id == XBI_ARR_CONTAINS) return v_bool(found >= 0);
            return v_int(found);
        }

        case XBI_ARR_FILL: {
            Value v = vm_pop(vm, pos);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                value_release(&v);
                rt_error(pos, "`arrFill` 的第 1 个参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            // 先把每个旧元素 release，再逐槽写入并 retain 新值。
            // 顺序不能反：如果先 retain 再把同一个对象写进所有槽，
            // 槽里已有的旧引用会漏 release，字符串数组会稳定泄漏。
            for (size_t i = 0; i < av.a->len; i++) {
                value_release(&av.a->data[i]);
                av.a->data[i] = v;
                value_retain(&av.a->data[i]);
            }
            // 栈上那份 v 也要释放：每槽各持有一份，栈上这份是额外的
            value_release(&v);
            return v_null();
        }

        case XBI_ARR_SORT: {
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrSort` 的参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            // 排序规则（与 Python 的 sorted 对同质列表的行为一致）：
            //   数值 < 字符串；数值之间按大小；字符串之间按字节序；
            //   null 排最前；Bool 当数值（false=0, true=1）。
            //
            // 混合类型的顺序是**全序**，所以排序不会因为元素类型
            // 不同而出现「不稳定的结果」。若只支持同质列表，遇到
            // 异构数组就得报错 —— 而 JSON 数组天然是异构的。
            qsort(av.a->data, av.a->len, sizeof(Value), value_cmp);
            return v_null();
        }

        case XBI_ARR_COPY: {
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY) {
                rt_error(pos, "`arrCopy` 的参数必须是 Array，实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            XArr* out = arr_new(0, v_null());
            arr_reserve(out, av.a->len);
            for (size_t i = 0; i < av.a->len; i++) {
                Value el = av.a->data[i];
                value_retain(&el);
                out->data[out->len++] = el;
            }
            return v_arr(out);
        }

        // ============================================================
        // 字符串
        // ============================================================
        //
        // 全部按 UTF-8 **码点**索引。按字节切会让中文用户立刻遇到
        // 「strAt(\"你好\", 1) 返回半个人」—— 而 Byte 层的错误在
        // 打印时表现为乱码，很难归因到下标。

        case XBI_STR_AT: {
            int64_t ix = BI_ARG_INT(2);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strAt` 的第 1 个参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            return bi_str_codepoint_at(sv.s, ix);
        }

        case XBI_STR_SUB: {
            int64_t end = BI_ARG_INT(3);
            int64_t start = BI_ARG_INT(2);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strSub` 的第 1 个参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            return bi_str_slice(sv.s, start, end);
        }

        case XBI_STR_FIND:
        case XBI_STR_FIND_FROM: {
            // 栈顺序（从下到上）：s, sub [, from]
            // 所以先弹 from（若有），再弹 sub，最后弹 s。
            //
            // 这里踩过一次：原本先弹 sub 再按 id 分支，导致 findFrom
            // 的三个参数整体错位 —— sub 变量里装的是 from 的值。
            // 因为两个都是 Value，类型检查发现不了（Int 也是合法类型），
            // 表现为「strFindFrom 永远返回 -1」。所以下面把两种情况的
            // 弹栈顺序写死，不做「先弹一个再判断」。
            if (id == XBI_STR_FIND_FROM) {
                int64_t from_ = BI_ARG_INT(3);
                Value sub2 = vm_pop(vm, pos);
                Value sv = vm_pop(vm, pos);
                if (sv.type != VT_STRING || sub2.type != VT_STRING) {
                    rt_error(pos, "`strFindFrom` 的前两个参数必须是 String，实际是 %s 和 %s",
                             type_label(sv.type), type_label(sub2.type));
                    exit(70);
                }
                return v_int(bi_str_find(sv.s, sub2.s, from_));
            }
            Value sub = vm_pop(vm, pos);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING || sub.type != VT_STRING) {
                rt_error(pos, "`strFind` 的两个参数都必须是 String，实际是 %s 和 %s",
                         type_label(sv.type), type_label(sub.type));
                exit(70);
            }
            return v_int(bi_str_find(sv.s, sub.s, 0));
        }

        case XBI_STR_SPLIT: {
            Value sep = vm_pop(vm, pos);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING || sep.type != VT_STRING) {
                rt_error(pos, "`strSplit` 的参数必须是 (String, String)，实际是 %s 和 %s",
                         type_label(sv.type), type_label(sep.type));
                exit(70);
            }
            return v_arr(bi_str_split(sv.s, sep.s));
        }

        case XBI_STR_JOIN: {
            Value sep = vm_pop(vm, pos);
            Value av = vm_pop(vm, pos);
            if (av.type != VT_ARRAY || sep.type != VT_STRING) {
                rt_error(pos, "`strJoin` 的参数必须是 (Array, String)，实际是 %s 和 %s",
                         type_label(av.type), type_label(sep.type));
                exit(70);
            }
            return v_str(bi_str_join(av.a, sep.s));
        }

        case XBI_STR_UPPER:
        case XBI_STR_LOWER: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`%s` 的参数必须是 String，实际是 %s",
                         name, type_label(sv.type));
                exit(70);
            }
            return v_str(bi_str_case(sv.s, id == XBI_STR_UPPER));
        }

        case XBI_STR_TRIM: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strTrim` 的参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            return v_str(bi_str_trim(sv.s));
        }

        case XBI_STR_REPLACE: {
            Value to = vm_pop(vm, pos);
            Value from = vm_pop(vm, pos);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING || from.type != VT_STRING || to.type != VT_STRING) {
                rt_error(pos, "`strReplace` 的三个参数都必须是 String");
                exit(70);
            }
            return v_str(bi_str_replace(sv.s, from.s, to.s));
        }

        case XBI_STR_STARTS:
        case XBI_STR_ENDS: {
            Value affix = vm_pop(vm, pos);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING || affix.type != VT_STRING) {
                rt_error(pos, "`%s` 的两个参数都必须是 String", name);
                exit(70);
            }
            bool ok;
            if (id == XBI_STR_STARTS) {
                ok = sv.s->len >= affix.s->len &&
                     memcmp(sv.s->data, affix.s->data, affix.s->len) == 0;
            } else {
                ok = sv.s->len >= affix.s->len &&
                     memcmp(sv.s->data + sv.s->len - affix.s->len,
                            affix.s->data, affix.s->len) == 0;
            }
            return v_bool(ok);
        }

        case XBI_STR_REPEAT: {
            int64_t n = BI_ARG_INT(2);
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strRepeat` 的第 1 个参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            if (n < 0) n = 0;
            // 上限保护：strRepeat("x", 1e9) 会尝试分配 1GB。
            // 这个上限不是语言限制，是防止一次手误把机器拖死。
            const int64_t MAX_REPEAT_BYTES = 64 * 1024 * 1024;
            int64_t total = (int64_t)sv.s->len * n;
            if (total > MAX_REPEAT_BYTES) {
                rt_error(pos, "`strRepeat` 的结果过大：%lld × %zu = %lld 字节\n"
                              "  help: 单次重复的结果上限是 64 MiB",
                         (long long)n, sv.s->len, (long long)total);
                exit(70);
            }
            // 一次性按最终长度分配，避免循环里反复 realloc。
            XStr* res = x_alloc(sizeof(XStr) + (size_t)total + 1);
            res->rc  = 1;
            res->len = (size_t)total;
            size_t w = 0;
            for (int64_t i = 0; i < n; i++) {
                memcpy(res->data + w, sv.s->data, sv.s->len);
                w += sv.s->len;
            }
            res->data[total] = '\0';
            return v_str(res);
        }

        case XBI_STR_CODE: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strCode` 的参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            if (sv.s->len == 0) return v_int(0);
            int adv = 0;
            int cp = utf8_decode(sv.s->data, sv.s->len, &adv);
            return v_int(cp);
        }

        case XBI_STR_FROM_CODE: {
            int64_t cp = BI_ARG_INT(1);
            char buf[4];
            size_t n = 0;
            // 手写 UTF-8 编码。不调库是为了不依赖 locale/宽字符 API ——
            // 那两个在 Windows 与类 Unix 上行为不同。
            uint32_t c = (uint32_t)cp;
            if (c < 0x80) { buf[n++] = (char)c; }
            else if (c < 0x800) {
                buf[n++] = (char)(0xC0 | (c >> 6));
                buf[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                buf[n++] = (char)(0xE0 | (c >> 12));
                buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x110000) {
                buf[n++] = (char)(0xF0 | (c >> 18));
                buf[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                rt_error(pos, "`strFromCode` 的码点非法：%lld（合法范围 0..0x10FFFF）",
                         (long long)cp);
                exit(70);
            }
            return v_str(str_new(buf, n));
        }

        case XBI_STR_PARSE_INT: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strParseInt` 的参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            return v_int(bi_parse_int(sv.s));
        }

        case XBI_STR_PARSE_FLOAT: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_STRING) {
                rt_error(pos, "`strParseFloat` 的参数必须是 String，实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            return v_float(bi_parse_float(sv.s));
        }

        // ============================================================
        // 数学
        // ============================================================
        //
        // 一条 BC_BUILTIN 承载 25 个子操作：a 是 XBI_MATH（函数编号，
        // 必须留在这里 —— 反汇编和能力探测都靠它认函数），
        // 子操作编码在 c 操作数里：低 8 位是 XMathOp，高位是参数个数。
        // 见 x.h 的 xmath_enc。
        case XBI_MATH:
            return bi_math(vm, pos, in->c);

        case XBI_RANDOM: {
            // [0, 1) 的均匀分布。用 xorshift128+ 而不是 rand()：
            // rand() 的质量在 MSVCRT 上很差（低位周期极短），
            // 而标准库的 shuffle / 采样会直接暴露这一点。
            return v_float(bi_random_unit());
        }

        case XBI_RANDOM_SEED: {
            int64_t seed = BI_ARG_INT(1);
            bi_random_seed((uint64_t)seed);
            return v_null();
        }

        case XBI_FAIL: {
            // 标准库的报错出口。消息由调用方拼好，这里只负责
            // 带上源码位置后终止 —— 与运行时自身的报错走同一条路径，
            // 所以用户在终端看到的格式是一致的。
            Value mv = vm_pop(vm, pos);
            if (mv.type != VT_STRING) {
                rt_error(pos, "`fail` 的参数必须是 String，实际是 %s",
                         type_label(mv.type));
                exit(70);
            }
            // 复制一份再报错：rt_error 内部会 fprintf，而 mv.s
            // 随后就要被释放，不能把指针交出去。
            size_t n = mv.s->len;
            char* msg = x_alloc(n + 1);
            memcpy(msg, mv.s->data, n);
            msg[n] = '\0';
            rt_error(pos, "%s", msg);
            exit(70);   // rt_error 已 exit，这行只为让编译器安心
        }

        case XBI_TYPEOF: {
            // 类型名与 type_label 一致，但不接受参数类型检查 ——
            // 它存在的意义就是「什么类型都能吃」。
            Value v = vm_pop(vm, pos);
            const char* t = type_label(v.type);
            value_release(&v);
            return v_str(str_new(t, strlen(t)));
        }

        // ============================================================
        // 反射
        // ============================================================
        //
        // ── 设计原则：只暴露元数据，不暴露内部结构 ──────────────
        //
        // 返回值全部是普通值（String / Array / Bool / 字段原值）。
        // 没有「Class 对象」「Field 对象」这类新类型，因为那会把
        // 反射变成第二套类型系统：用户要为它写类型标注、要处理
        // 「反射对象为 null」，而收益只是少几个函数。
        //
        // 字段描述用 `[name, type, vis, final]` 四元数组。选数组而不是
        // 结构体：结构体要求调用方声明类型，而反射的结果天然是动态的
        // —— 用户往往只是遍历打印。需要类型时用 `toString(f[1])` 即可。
        //
        // ── vis 用字符串而不是数字 ────────────────────────────
        //
        // "public" / "protected" / "private" 直接可读，打印出来就是
        // 结论。用数字（0/1/2）的话，每个调用点都要记一遍映射 ——
        // 而记错的表现是「把 private 当成 public 打印出来」，看不出错。
        case XBI_CLASS_NAME: {
            Value v = vm_pop(vm, pos);
            const char* s = NULL;
            if (v.type == VT_CLASS)      s = v.k->cls_name;
            else                         s = type_label(v.type);
            Value r = v_str(str_new(s, strlen(s)));
            value_release(&v);
            return r;
        }

        case XBI_CLASS_PARENT: {
            Value v = vm_pop(vm, pos);
            const char* s = "";
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    int pi = bc->classes[ci].parent_idx;
                    // 接口没有父类（IS-A 关系靠 iface_idx 表达），
                    // 所以这里给空串而不是去查 interfaces 表。
                    if (pi >= 0 && pi < bc->class_count) s = bc->classes[pi].name;
                }
            }
            Value r = v_str(str_new(s, strlen(s)));
            value_release(&v);
            return r;
        }

        case XBI_CLASS_IS: {
            // classIs(obj, "Base") —— 沿继承链判 IS-A。
            //
            // 与 `typeof(obj) == "Base"` 的区别：后者只判**完全相等**，
            // 子类实例返回 false。而 IS-A 语义下子类当然「是」父类。
            // 两个都要提供，因为两种问法都是真实需求：
            //   · 「它到底是不是这个类」→ className(x) == "Foo"
            //   · 「它能不能当 Foo 用」  → classIs(x, "Foo")
            Value tv;
            const char* want = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);

            bool res = false;
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int cur = rt_cls_index(bc, v, pos, name);
                // 沿父链走。上限 64 只是防环 —— 正常的类层次不会有
                // 这么深，真出现说明父类索引被写坏了。
                for (int guard = 0; guard < 64 && cur >= 0 &&
                     cur < bc->class_count; guard++) {
                    ClassType* ct = &bc->classes[cur];
                    if (ct->name && strcmp(ct->name, want) == 0) { res = true; break; }
                    for (int k = 0; k < ct->iface_count; k++) {
                        int ii = ct->iface_idx[k];
                        if (ii >= 0 && ii < bc->class_count &&
                            bc->classes[ii].name &&
                            strcmp(bc->classes[ii].name, want) == 0) {
                            res = true;
                            break;
                        }
                    }
                    if (res) break;
                    cur = ct->parent_idx;
                }
            }
            value_release(&v);
            value_release(&tv);
            return v_bool(res);
        }

        case XBI_CLASS_FIELDS: {
            // classFields(obj) -> [[name, type, vis, final], ...]
            //
            // 顺序就是内存布局顺序（父类字段在前）。这一点很实用：
            // 想按声明顺序打印对象的全部字段，直接遍历即可。
            // 接收者也可以是类名字符串（v0.20）——框架在实例化之前
            // 就要读实体元数据（生成 SQL、做参数绑定），此时只有类名。
            Value v = vm_pop(vm, pos);
            XArr* out = arr_new(0, v_null());
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    ClassType* ct = &bc->classes[ci];
                    for (int i = 0; i < ct->field_count; i++) {
                        XArr* row = arr_new(4, v_null());
                        const char* fn = ct->field_names[i] ? ct->field_names[i] : "";
                        const char* ft = ct->field_types && ct->field_types[i]
                                         ? ct->field_types[i] : "";
                        row->data[0] = v_str(str_new(fn, strlen(fn)));
                        row->data[1] = v_str(str_new(ft, strlen(ft)));
                        const char* vs = vis_label(ct->field_vis
                                                   ? ct->field_vis[i] : VIS_PUBLIC);
                        row->data[2] = v_str(str_new(vs, strlen(vs)));
                        row->data[3] = v_bool(ct->field_final && ct->field_final[i]);
                        arr_push(out, (Value){ .type = VT_ARRAY, .a = row });
                    }
                }
            }
            value_release(&v);
            return (Value){ .type = VT_ARRAY, .a = out };
        }

        case XBI_CLASS_METHODS: {
            // classMethods(obj) -> [[name, vis], ...]
            //
            // 只列**虚表里有实现**的方法（含继承来的）。抽象方法不列：
            // 它们没有实现，列出来会让人以为能调。
            //
            // 同一个槽在不同类里可能指向不同实现，但名字只有一个 ——
            // 所以按槽遍历天然不会重复。
            // 接收者也可以是类名字符串（v0.20）—— 注解路由扫描
            // @RestController 的方法列表时还没有实例。
            Value v = vm_pop(vm, pos);
            XArr* out = arr_new(0, v_null());
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    ClassType* ct = &bc->classes[ci];
                    for (int s = 0; s < ct->vtable_len && s < bc->method_slot_count; s++) {
                        if (ct->vtable[s] < 0) continue;
                        XArr* row = arr_new(2, v_null());
                        const char* mn = bc->method_slots[s].name;
                        row->data[0] = v_str(str_new(mn, strlen(mn)));
                        const char* vs = vis_label(ct->method_vis
                                                   ? ct->method_vis[s] : VIS_PUBLIC);
                        row->data[1] = v_str(str_new(vs, strlen(vs)));
                        arr_push(out, (Value){ .type = VT_ARRAY, .a = row });
                    }
                }
            }
            value_release(&v);
            return (Value){ .type = VT_ARRAY, .a = out };
        }

        case XBI_FIELD_HAS: {
            Value tv;
            const char* fname = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);
            bool res = false;
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    res = rt_find_field(&bc->classes[ci], fname) >= 0;
                }
            }
            value_release(&v);
            value_release(&tv);
            return v_bool(res);
        }

        case XBI_METHOD_HAS: {
            Value tv;
            const char* mname = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);
            bool res = false;
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    int slot = -1;
                    for (int s = 0; s < bc->method_slot_count; s++) {
                        if (strcmp(bc->method_slots[s].name, mname) == 0) {
                            slot = s;
                            break;
                        }
                    }
                    res = slot >= 0 &&
                          slot < bc->classes[ci].vtable_len &&
                          bc->classes[ci].vtable[slot] >= 0;
                }
            }
            value_release(&v);
            value_release(&tv);
            return v_bool(res);
        }

        case XBI_FIELD_GET: {
            Value tv;
            const char* fname = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);
            if (v.type != VT_CLASS) {
                rt_error(pos, "`fieldGet` 的第 1 个参数必须是类实例，实际是 %s",
                         type_label(v.type));
                value_release(&v);
                value_release(&tv);
                exit(70);
            }
            int ci = v.k->cls_idx;
            if (ci < 0 || ci >= bc->class_count) {
                rt_error(pos, "内部错误：实例的类索引 %d 越界", ci);
                value_release(&v);
                value_release(&tv);
                exit(70);
            }
            ClassType* ct = &bc->classes[ci];
            int fi = rt_find_field(ct, fname);
            if (fi < 0) {
                rt_error(pos, "类 `%s` 没有字段 `%s`\n"
                              "  help: 用 classFields(obj) 看有哪些字段",
                         ct->name, fname);
                value_release(&v);
                value_release(&tv);
                exit(70);
            }
            rt_check_field_access(vm, ci, fi, pos, "读");
            // 读出来的值必须 retain：实例仍持有它，栈上这份是新的
            // 一份引用。不 retain 的话，实例被释放时字段的值跟着
            // 释放，而栈上那份还在用 —— 悬空。
            Value got = v.k->fields[fi];
            value_retain(&got);
            value_release(&v);
            value_release(&tv);
            return got;
        }

        case XBI_FIELD_SET: {
            Value nv = vm_pop(vm, pos);
            Value tv;
            const char* fname = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);
            if (v.type != VT_CLASS) {
                rt_error(pos, "`fieldSet` 的第 1 个参数必须是类实例，实际是 %s",
                         type_label(v.type));
                value_release(&v); value_release(&tv); value_release(&nv);
                exit(70);
            }
            int ci = v.k->cls_idx;
            if (ci < 0 || ci >= bc->class_count) {
                rt_error(pos, "内部错误：实例的类索引 %d 越界", ci);
                value_release(&v); value_release(&tv); value_release(&nv);
                exit(70);
            }
            ClassType* ct = &bc->classes[ci];
            int fi = rt_find_field(ct, fname);
            if (fi < 0) {
                rt_error(pos, "类 `%s` 没有字段 `%s`",
                         ct->name, fname);
                value_release(&v); value_release(&tv); value_release(&nv);
                exit(70);
            }
            rt_check_field_access(vm, ci, fi, pos, "写");
            // final 字段：反射也不能改。
            //
            // 编译器在直接赋值处已经挡了，但反射是一条绕过路径 ——
            // 不挡的话 `fieldSet(obj, "id", 999)` 就能改掉本该不可变
            // 的字段，而所有「id 是 final 所以不会变」的推理全部失效。
            if (ct->field_final && ct->field_final[fi]) {
                rt_error(pos, "字段 `%s.%s` 是 final 的，反射也不能改\n"
                              "  help: final 表示「构造完就不再变」。"
                              "需要可变请去掉 final",
                         ct->name, fname);
                value_release(&v); value_release(&tv); value_release(&nv);
                exit(70);
            }
            // 所有权转移：新值归实例，调用方那份（nv）要被消费掉。
            // 顺序是先释放旧值、再接管新值 —— 反了的话如果新值
            // 就是旧值本身（fieldSet(a, "x", fieldGet(a, "x"))），
            // 会先接管再释放，把刚存进去的那份释放掉。
            value_release(&v.k->fields[fi]);
            v.k->fields[fi] = nv;      // nv 的所有权转给字段，不再 release
            value_release(&v);
            value_release(&tv);
            return v_null();
        }

        // ============================================================
        // 注解查询
        // ============================================================
        //
        // ── 为什么三个查询共用一个「先解 x, target, name」的骨架 ──
        //
        // 它们的前置处理完全一样：校验接收者是实例、把 target 字符串
        // 转成枚举、取出类索引。三个 case 各写一遍的话，「谁是第几个
        // 参数」这件事会被复制三份 —— 而弹栈顺序是反的，复制时最容易
        // 错位的地方恰恰就是这个。
        //
        // ── 语义边界：只回答「自己被标了什么」 ──────────────────
        //
        // 不沿父链找注解（理由见 bc_find_anno）。框架要用继承语义，
        // 自己用 classParent 往上走 —— 语言给一个精确的窄答案，
        // 而不是替所有框架猜一个折中语义。
        case XBI_ANNO_HAS:
        case XBI_ANNO_GET:
        case XBI_ANNO_GET_KV:
        case XBI_ANNO_ALL: {
            // 参数形状：
            //   annoHas / annoGet / annoGetKV  (x, target, member, name)
            //   annoAll                        (x, target, member)
            //
            // member 对类目标传 ""。没有它对字段/方法注解无意义 ——
            // 详见 bytecode.c 里签名表的说明。
            Value nv;  const char* aname = NULL;
            Value mv;  const char* member = "";
            if (id != XBI_ANNO_ALL) {
                aname = bi_take_str(vm, pos, 4, name, &nv);
                member = bi_take_str(vm, pos, 3, name, &mv);
            } else {
                nv = v_null();
                member = bi_take_str(vm, pos, 3, name, &mv);
            }
            Value tv;
            const char* tgt = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);

            AnnoTarget at;
            if (!anno_target_from(tgt, &at)) {
                rt_error(pos,
                         "`%s` 的 target 只能是 \"class\" / \"field\" / \"method\"，"
                         "实际是 \"%s\"\n"
                         "  help: 它们分别对应类、字段、方法上的注解",
                         name, tgt);
                value_release(&v); value_release(&tv); value_release(&mv);
                value_release(&nv);
                exit(70);
            }
            if (v.type != VT_CLASS && v.type != VT_STRING) {
                rt_error(pos, "`%s` 的第 1 个参数必须是类实例或类名字符串，实际是 %s\n"
                              "  help: 注解挂在类型上；传实例或 \"类名\" 都可以查询",
                         name, type_label(v.type));
                value_release(&v); value_release(&tv); value_release(&mv);
                value_release(&nv);
                exit(70);
            }
            int ci = rt_cls_index(bc, v, pos, name);

            // 类目标的 member 只能是空串 —— 类没有「哪个成员」可言。
            // 传了个非空名字说明调用点写错了（比如把字段名传到了
            // 类目标上），那条查询永远不会命中，静默给出空结果。
            const char* mem = (at == ANNO_TARGET_CLASS) ? NULL : member;
            if (at == ANNO_TARGET_CLASS && member && member[0] != '\0') {
                rt_error(pos,
                         "target 是 \"class\" 时 member 必须是空字符串，"
                         "实际是 \"%s\"\n"
                         "  help: 类注解与具体成员无关，写成 annoHas(x, \"class\", \"\", \"Name\")",
                         member);
                value_release(&v); value_release(&tv); value_release(&mv);
                value_release(&nv);
                exit(70);
            }

            Value r;
            if (id == XBI_ANNO_HAS) {
                r = v_bool(bc_find_anno(bc, at, ci, mem, aname) != NULL);
            } else if (id == XBI_ANNO_GET) {
                // 位置实参列表。
                //
                // 具名实参也按源码顺序放进这个数组（键被丢掉），因为
                // 「按顺序取第 n 个值」在两边是同一个含义；要读键名
                // 就用 annoGetKV。
                AnnoEntry* e = bc_find_anno(bc, at, ci, mem, aname);
                XArr* out = arr_new(0, v_null());
                if (e) {
                    for (int i = 0; i < e->arg_count; i++) {
                        const char* s = e->arg_vals[i] ? e->arg_vals[i] : "";
                        arr_push(out, v_str(str_new(s, strlen(s))));
                    }
                }
                r = (Value){ .type = VT_ARRAY, .a = out };
            } else if (id == XBI_ANNO_GET_KV) {
                AnnoEntry* e = bc_find_anno(bc, at, ci, mem, aname);
                XArr* out = arr_new(0, v_null());
                if (e) {
                    for (int i = 0; i < e->arg_count; i++) {
                        XArr* row = arr_new(2, v_null());
                        // 位置实参的键是 null —— 用 null 而不是 ""，
                        // 因为「没有键」和「键是空字符串」在过滤时
                        // 必须能区分开。
                        if (e->arg_keys && e->arg_keys[i]) {
                            const char* k = e->arg_keys[i];
                            row->data[0] = v_str(str_new(k, strlen(k)));
                        }
                        const char* s = e->arg_vals[i] ? e->arg_vals[i] : "";
                        row->data[1] = v_str(str_new(s, strlen(s)));
                        arr_push(out, (Value){ .type = VT_ARRAY, .a = row });
                    }
                }
                r = (Value){ .type = VT_ARRAY, .a = out };
            } else {
                // ANNO_ALL：这个目标上所有注解的名字，按声明顺序。
                //
                // 重复标注同名注解时全部列出（不去重）—— 「标了两次」
                // 本身是用户可能想检测的事实，去重会把它藏起来。
                XArr* out = arr_new(0, v_null());
                for (int i = 0; i < bc->anno_count; i++) {
                    AnnoEntry* e = &bc->annos[i];
                    if (e->target != at) continue;
                    if (e->class_idx != ci) continue;
                    if (mem || e->member) {
                        if (!mem || !e->member) continue;
                        if (strcmp(mem, e->member) != 0) continue;
                    }
                    const char* s = e->name ? e->name : "";
                    arr_push(out, v_str(str_new(s, strlen(s))));
                }
                r = (Value){ .type = VT_ARRAY, .a = out };
            }
            value_release(&v);
            value_release(&tv);
            value_release(&mv);
            return r;
        }

        case XBI_CLASS_ALL: {
            // classAll() -> 字节码里全部类名，按编译期注册顺序。
            //
            // ── 为什么返回名字而不是「类对象」 ──────────────────
            //
            // 语言没有独立的类值类型（反射内建全收实例或名字），
            // 返回名字数组让结果直接能用普通能力处理：遍历、比较、
            // 塞进别的数据结构。要元数据就拿着名字去问 anno*/class*，
            // 它们（v0.20 起）接受类名字符串。
            //
            // ── 为什么不做「只扫某包」的过滤 ────────────────────
            //
            // 字节码里没有包路径信息（import 在编译前就被文本级展开
            // 了）。过滤只能靠注解：框架约定组件必须带 @Component
            // 系注解，扫描器逐个 annoHas 过滤。误伤面收敛到「忘了
            // 删掉注解的类」，而不是「整个程序所有类」。
            XArr* out = arr_new(0, v_null());
            for (int i = 0; i < bc->class_count; i++) {
                const char* cn = bc->classes[i].name;
                if (cn) arr_push(out, v_str(str_new(cn, strlen(cn))));
            }
            return (Value){ .type = VT_ARRAY, .a = out };
        }

        case XBI_METHOD_PARAMS: {
            // methodParams(obj_or_name, "methodName") -> [[name, type], ...]
            //
            // 「按名绑定」的元数据来源。路由收到 GET /users/42、
            // 处理器声明 fn byId(id: Int)，框架拿路径变量 {id} 与
            // 这里返回的参数名对号入座，类型名负责字符串 → Int。
            //
            // 方法按虚表槽解析（与 methodCall 同一条查找路径）：
            // 继承来的方法返回**实现方**的参数表 —— 绑定数据给谁，
            // 就该按谁的签名来。
            Value tv;
            const char* mname = bi_take_str(vm, pos, 2, name, &tv);
            Value v = vm_pop(vm, pos);

            XArr* out = arr_new(0, v_null());
            if (v.type == VT_CLASS || v.type == VT_STRING) {
                int ci = rt_cls_index(bc, v, pos, name);
                if (ci >= 0 && ci < bc->class_count) {
                    int slot = -1;
                    for (int s = 0; s < bc->method_slot_count; s++) {
                        if (bc->method_slots[s].name &&
                            strcmp(bc->method_slots[s].name, mname) == 0) {
                            slot = s;
                            break;
                        }
                    }
                    if (slot >= 0 && slot < bc->classes[ci].vtable_len &&
                        bc->classes[ci].vtable[slot] >= 0) {
                        FuncProto* fn = &bc->funcs[bc->classes[ci].vtable[slot]];
                        for (int k = 0; k < fn->param_count; k++) {
                            XArr* row = arr_new(0, v_null());
                            const char* pn =
                                fn->param_names && fn->param_names[k]
                                    ? fn->param_names[k] : "?";
                            const char* pt =
                                fn->param_types && fn->param_types[k]
                                    ? fn->param_types[k] : "Null";
                            arr_push(row, v_str(str_new(pn, strlen(pn))));
                            arr_push(row, v_str(str_new(pt, strlen(pt))));
                            arr_push(out, (Value){ .type = VT_ARRAY, .a = row });
                        }
                    }
                }
            }
            value_release(&v);
            value_release(&tv);
            return (Value){ .type = VT_ARRAY, .a = out };
        }

        // ============================================================
        // 文件
        // ============================================================
        //
        // 全部走 plat_file_*（platform.c）。这里只做两件事：
        // 参数类型校验、把 C 字符串包成 X 字符串。
        //
        // ── 一条贯穿这批函数的规则 ──────────────────────────────
        //
        // 「能不能失败」与「失败怎么表达」必须分开决定：
        //
        //   fsRead   打不开 → fail（调用方写 fsRead 就表示他认为文件该在）
        //   fsReadOr 打不开 → 默认值（读配置的用法，缺失是正常的）
        //   fsWrite  写不了 → 返回 false（权限、磁盘满都是要处理的情况）
        //
        // 把三者统一成任何一种都会出错：全 fail 让「读可选配置」没法写，
        // 全返回默认值让「文件该在但不在」这种真错误被静默吞掉。

        case XBI_FS_READ: {
            Value pv = vm_pop(vm, pos);
            if (pv.type != VT_STRING) {
                rt_error(pos, "fsRead 的参数必须是 String（文件路径），实际是 %s",
                         type_label(pv.type));
                exit(70);
            }
            size_t n = 0;
            char* data = plat_file_read(pv.s->data, &n);
            char* path = pv.s->data;
            size_t plen = pv.s->len;
            if (!data) {
                rt_error(pos, "读不了文件 `%.*s`\n"
                              "  help: 确认路径存在且可读；父目录是否忘了建（fsMkdir）",
                         (int)plen, path);
                exit(70);
            }
            Value r = v_str(str_new(data, n));
            free(data);
            value_release(&pv);
            return r;
        }

        case XBI_FS_READ_OR: {
            Value dv = vm_pop(vm, pos);      // 默认值在栈顶
            Value pv = vm_pop(vm, pos);
            if (pv.type != VT_STRING) {
                rt_error(pos, "fsReadOr 的第 1 个参数必须是 String（路径），实际是 %s",
                         type_label(pv.type));
                exit(70);
            }
            size_t n = 0;
            char* data = plat_file_read(pv.s->data, &n);
            Value r;
            if (data) {
                r = v_str(str_new(data, n));
                free(data);
            } else {
                // 文件不存在时把默认值原样返回。不能假设默认值是字符串 ——
                // 那样 fsReadOr(path, 0) 这种「缺失当 0」的用法就废了。
                r = dv;
                dv = v_null();      // 所有权转移给 r，下面不要再释放
            }
            value_release(&pv);
            value_release(&dv);
            return r;
        }

        case XBI_FS_WRITE:
        case XBI_FS_APPEND: {
            Value tv = vm_pop(vm, pos);      // 内容在栈顶
            Value pv = vm_pop(vm, pos);
            if (pv.type != VT_STRING) {
                rt_error(pos, "%s 的第 1 个参数必须是 String（路径），实际是 %s",
                         id == XBI_FS_WRITE ? "fsWrite" : "fsAppend",
                         type_label(pv.type));
                exit(70);
            }
            // 内容接受任意类型：fsWrite(p, 42) 是有用的写法。
            // 但只有 String 能原样写，其它类型先转成文本。
            //
            // 为什么要区分：String 走零拷贝（直接用 s->data），
            // 其它类型得先构造 XStr。混在一起写会让 String 也多一次
            // 分配 + 释放，那是最高频的路径。
            XStr* tmp = NULL;
            const char* body = "";
            size_t blen = 0;
            if (tv.type == VT_STRING) {
                body = tv.s->data;
                blen = tv.s->len;
            } else {
                tmp = value_to_str(tv);
                body = tmp->data;
                blen = tmp->len;
            }
            bool ok = (id == XBI_FS_WRITE)
                        ? plat_file_write(pv.s->data, body, blen)
                        : plat_file_append(pv.s->data, body, blen);
            if (tmp) str_release(tmp);
            value_release(&tv);
            value_release(&pv);
            return v_bool(ok);
        }

        case XBI_FS_REPLACE: {
            Value tv = vm_pop(vm, pos);
            Value pv = vm_pop(vm, pos);
            if (pv.type != VT_STRING) {
                rt_error(pos, "fsReplace 的第 1 个参数必须是 String（路径），实际是 %s",
                         type_label(pv.type));
                exit(70);
            }
            XStr* tmp = NULL;
            const char* body = "";
            size_t blen = 0;
            if (tv.type == VT_STRING) {
                body = tv.s->data;
                blen = tv.s->len;
            } else {
                tmp = value_to_str(tv);
                body = tmp->data;
                blen = tmp->len;
            }
            bool ok = plat_file_replace(pv.s->data, body, blen);
            if (tmp) str_release(tmp);
            value_release(&tv);
            value_release(&pv);
            return v_bool(ok);
        }

        case XBI_FS_EXISTS:
        case XBI_FS_IS_DIR:
        case XBI_FS_REMOVE:
        case XBI_FS_SIZE:
        case XBI_FS_MKDIR:
        case XBI_FS_LIST: {
            Value pv = vm_pop(vm, pos);
            if (pv.type != VT_STRING) {
                rt_error(pos, "%s 的参数必须是 String（路径），实际是 %s",
                         bc_builtin_name(id), type_label(pv.type));
                exit(70);
            }
            const char* p = pv.s->data;
            Value r;
            switch (id) {
            case XBI_FS_EXISTS: r = v_bool(plat_file_exists(p)); break;
            case XBI_FS_IS_DIR: r = v_bool(plat_is_dir(p)); break;
            case XBI_FS_REMOVE: r = v_bool(plat_file_remove(p)); break;
            case XBI_FS_SIZE:   r = v_int((int64_t)plat_file_size(p)); break;
            case XBI_FS_MKDIR:  r = v_bool(plat_mkdir(p)); break;
            default: {
                // fsList：只列名字，不含 "." 与 ".."
                //
                // ── 这里踩过一个坑 ──────────────────────────────
                //
                // 原来写的是 arr_new(0, v_str(str_new("", 0)))。
                // arr_new 在 len=0 时**不 retain** 填充值（循环一次都不
                // 进），于是那个新建的空字符串既没有进数组、也没有人
                // 释放它 —— 每次调用泄漏一个字符串对象。
                //
                // 单个泄漏不致命，但它会把堆的状态搅乱：后续
                // malloc/free 的布局与预期不同，而栈破坏类 bug 的表现
                // 恰恰依赖具体布局 —— 于是「同一个程序换个长度就崩」。
                // 排查这类问题花的时间远超过写对它的时间。
                //
                // 现在用 v_null() 作填充（值类型，零成本零所有权），
                // 元素只通过 arr_push 进来，所有权转移清晰。
                XArr* a = arr_new(0, v_null());
                PlatDir d;
                if (plat_dir_open(&d, p, NULL)) {
                    while (plat_dir_next(&d)) {
                        arr_push(a, v_str(str_new(d.name, strlen(d.name))));
                    }
                    plat_dir_close(&d);
                }
                r = (Value){ .type = VT_ARRAY, .a = a };
                break;
            }
            }
            value_release(&pv);
            return r;
        }

        // ============================================================
        // TCP
        // ============================================================
        //
        // 句柄在语言层是 Int，-1 表示无效。平台层把 Windows 的
        // UINT_PTR 塞进 long long，所以 64 位句柄不会被截断。
        //
        // ── netRecv 的返回值语义 ────────────────────────────────
        //
        //   ""      没有数据（也可能对端已关闭 —— 用 netPending 区分）
        //   非空串  收到的字节
        //
        // 为什么把「对端关闭」也归到 ""：语言里没有「也许」这种类型，
        // 而调用方真正需要的是「这一轮有没有拿到东西」。要精确知道
        // 连接是否已断，用 netPending(sock)。

        case XBI_NET_LISTEN: {
            Value pv = vm_pop(vm, pos);      // 端口在栈顶
            Value av = vm_pop(vm, pos);
            if (av.type != VT_STRING) {
                rt_error(pos, "netListen 的第 1 个参数必须是 String（绑定地址），实际是 %s",
                         type_label(av.type));
                exit(70);
            }
            int64_t port = (pv.type == VT_INT) ? pv.i : (int64_t)(pv.type == VT_FLOAT ? pv.f : 0.0);
            // 先把地址拷出来：value_release 之后 av.s 就没了，而下面的
            // 错误信息还要用到它。原实现直接在释放后拼消息，读的是
            // 已释放内存 —— 那正是「错误路径上才崩」这类 bug 的典型来源。
            char addr[128];
            snprintf(addr, sizeof(addr), "%s", av.s->data);
            PlatSock s = plat_tcp_listen(addr, (int)port);
            value_release(&pv);
            value_release(&av);
            if (!plat_sock_valid(s)) {
                rt_error(pos, "监听 %s:%d 失败：%s\n"
                              "  help: 端口被别的进程占用时换一个端口，或用 0 让系统分配",
                         addr, (int)port, plat_net_error());
                exit(70);
            }
            return v_int((int64_t)s.v);
        }

        case XBI_NET_ACCEPT: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_INT) {
                rt_error(pos, "netAccept 的参数必须是 Int（监听句柄），实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            PlatSock l = { .v = (long long)sv.i };
            PlatSock c = plat_tcp_accept(l);
            value_release(&sv);
            if (!plat_sock_valid(c)) return v_int(-1);   // 没有待处理连接
            return v_int((int64_t)c.v);
        }

        case XBI_NET_RECV: {
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_INT) {
                rt_error(pos, "netRecv 的参数必须是 Int（连接句柄），实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            PlatSock s = { .v = (long long)sv.i };
            value_release(&sv);
            // 64 KB 一次。HTTP 请求头通常几百字节，一次就读完；
            // 大 body 由调用方循环读（配合 netPending 判有没有更多）。
            char buf[65536];
            long n = plat_tcp_recv(s, buf, sizeof(buf));
            if (n <= 0) return v_str(str_new("", 0));
            return v_str(str_new(buf, (size_t)n));
        }

        case XBI_NET_SEND: {
            Value tv = vm_pop(vm, pos);      // 内容在栈顶
            Value sv = vm_pop(vm, pos);
            if (sv.type != VT_INT) {
                rt_error(pos, "netSend 的第 1 个参数必须是 Int（连接句柄），实际是 %s",
                         type_label(sv.type));
                exit(70);
            }
            PlatSock s = { .v = (long long)sv.i };
            XStr* tmp = NULL;
            const char* body = "";
            size_t blen = 0;
            if (tv.type == VT_STRING) {
                body = tv.s->data;
                blen = tv.s->len;
            } else {
                tmp = value_to_str(tv);
                body = tmp->data;
                blen = tmp->len;
            }
            long n = plat_tcp_send(s, body, blen);
            if (tmp) str_release(tmp);
            value_release(&tv);
            value_release(&sv);
            return v_int((int64_t)n);
        }

        case XBI_NET_CLOSE: {
            Value sv = vm_pop(vm, pos);
            if (sv.type == VT_INT) {
                PlatSock s = { .v = (long long)sv.i };
                plat_tcp_close(s);
            }
            value_release(&sv);
            return v_null();
        }

        case XBI_NET_CONNECT: {
            Value pv = vm_pop(vm, pos);
            Value hv = vm_pop(vm, pos);
            if (hv.type != VT_STRING) {
                rt_error(pos, "netConnect 的第 1 个参数必须是 String（主机），实际是 %s",
                         type_label(hv.type));
                exit(70);
            }
            int64_t port = (pv.type == VT_INT) ? pv.i : (int64_t)(pv.type == VT_FLOAT ? pv.f : 0.0);
            PlatSock s = plat_tcp_connect(hv.s->data, (int)port);
            value_release(&pv);
            value_release(&hv);
            if (!plat_sock_valid(s)) return v_int(-1);
            return v_int((int64_t)s.v);
        }

        case XBI_NET_PORT: {
            Value sv = vm_pop(vm, pos);
            int64_t port = -1;
            if (sv.type == VT_INT) {
                PlatSock s = { .v = (long long)sv.i };
                port = plat_tcp_local_port(s);
            }
            value_release(&sv);
            return v_int(port);
        }

        case XBI_NET_PEER: {
            Value sv = vm_pop(vm, pos);
            char out[64];
            out[0] = '\0';
            if (sv.type == VT_INT) {
                PlatSock s = { .v = (long long)sv.i };
                plat_tcp_peer_addr(s, out, sizeof(out));
            }
            value_release(&sv);
            return v_str(str_new(out, strlen(out)));
        }

        case XBI_NET_ERR: {
            const char* e = plat_net_error();
            return v_str(str_new(e, strlen(e)));
        }

        case XBI_NET_PENDING: {
            // 对端连接是否还活着。
            //
            // 判据在平台层（plat_tcp_alive 用 MSG_PEEK 读一字节），
            // 因为「没数据」与「对端已关闭」这两种情况在 netRecv 里
            // 都表现为空串，而服务端必须能区分它们 —— 否则死连接
            // 永远留在句柄表里，长期运行的服务最终耗尽句柄。
            Value sv = vm_pop(vm, pos);
            bool alive = false;
            if (sv.type == VT_INT) {
                PlatSock s = { .v = (long long)sv.i };
                alive = plat_tcp_alive(s);
            }
            value_release(&sv);
            return v_bool(alive);
        }
    }

    rt_error(pos,
             "内部错误：收到未知的平台内建编号 %d\n"
             "  note: 这通常意味着编译器与运行时版本不匹配\n"
             "  help: 重新编译整个项目，不要混用旧的 .xbc 与新的 x.exe",
             (int)id);
    exit(70);
    return v_null();   // 不可达
}

// ============================================================
// 解释循环
// ============================================================

// prof 是使用场景，只影响一件事：stdout 的缓冲策略。
// **不影响语言语义** —— 同一个程序在任何档位下算出的结果必须一样，
// 这是「两档逐位一致」的前提，不能让场景渗进语义。
//
// 传 NULL 等价于 console 档，这样不需要场景的调用点不必改动。
int run_bytecode(Bytecode* bc, const XProfileDef* prof) {
    // service 档：stdout 关掉缓冲（_IONBF）。
    //
    // 与 AOT 侧 codegen_c.c 生成的 main 里那一行**必须一致**，否则
    // 同一个程序在两档下「崩溃前能看到多少输出」会不同 —— 而 AOT
    // 一致性测试只比 stdout，抓不到这个差异（它比的是正常退出时的
    // 完整输出，两边都有）。
    //
    // ── 为什么是 stdout，不是 stderr ──────────────────────────
    //
    // 这里原本写的是 setvbuf(stderr, _IONBF)，理由是「服务崩溃前的
    // 最后几条错误不能丢」。实测证伪：C 标准要求 stderr 启动即无
    // 缓冲，MinGW 上 2000 行 stderr + TerminateProcess 硬杀，加不加
    // 这一行都存活 2000 行。那一行代码是空操作。
    //
    // 有差异的是 stdout。同样硬杀：_IOLBF 下输出半行（无 \n 结尾）
    // 会丢，_IONBF 下完整保留。服务的典型死法就是在写某一行写到一半
    // 时被杀，而那半行正是排查所需的线索。
    // 判据从表里取（prof->stdout_buf），不按名字判断 —— 否则加档位时
    // 这里、codegen_c.c、profile.c 三处都要改，必漏一处。
    if (prof && prof->stdout_buf == XBUF_NONE) {
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    // 找 main
    int main_idx = -1;
    for (int i = 0; i < bc->func_count; i++) {
        if (strcmp(bc->funcs[i].name, "main") == 0) { main_idx = i; break; }
    }
    if (main_idx < 0) {
        fprintf(stderr, "error: 字节码里没有 main 函数\n");
        return 70;
    }

    VM* vm = x_alloc(sizeof(VM));
    memset(vm, 0, sizeof(*vm));
    vm->bc = bc;

    // 建立初始帧
    FuncProto* mainf = &bc->funcs[main_idx];
    if (mainf->local_count > 0) {
        // 预填 null，让局部变量有确定初值
        for (int i = 0; i < mainf->local_count; i++) {
            vm->stack[vm->sp++].type = VT_NULL;
        }
    }
    vm->frames[0].func_index = main_idx;
    vm->frames[0].ret_pc     = -1;
    vm->frames[0].base       = 0;
    vm->frames[0].call_pos   = (SrcPos){ NULL, 0, 0 };
    // is_ctor / this_inst / cls_idx 也必须初始化。
    //
    // ── 这三行是「必须走统一入口」这条规则的又一次教训 ──────
    //
    // vm_push_frame 存在的原因就是「就地写 Frame 会漏字段」，而
    // 入口帧因为不是「调用」产生的，长期是唯一一处仍然就地写
    // 的地方。加了 cls_idx 之后，它立刻变成了脏值来源：main 的
    // 帧里 cls_idx 是上一个字节留下的数字，反射读到就把 main
    // 当成「在某个类里」，private 字段被放行 —— 而且哪一次编译
    // 泄漏哪个值完全看运气。
    //
    // 之所以没用 vm_push_frame：它在 vm->frames[vm->fp++] 上写，
    // 而这里 vm->frames[0] 需要在 fp=0 时就要就位。硬套的话得先把
    // fp 设成 0 再调，可读性反而更差。所以就地写，但把三个新增
    // 字段一起写全 —— 并且注释在这里，提醒下一个人。
    vm->frames[0].is_ctor    = false;
    vm->frames[0].this_inst  = v_null();
    vm->frames[0].cls_idx    = -1;
    vm->fp = 1;

    int pc = mainf->code_start;
    int ret_code = 0;

    Insn* code = bc->code;
    SrcPos* code_pos = bc->pos;      // 旁路位置数组，只在出错分支里读
    Constant* consts = bc->consts;

    // ---- 指令对直方图（07 §3.3.1）----
    //
    // 选哪些超指令**不能靠猜**。人眼看起来高频的组合，实际跑起来可能
    // 一次都不出现（比如 `ADD; STORE` 在 X 里因为赋值表达式先 DUP
    // 而变成 `ADD; DUP; STORE_LOCAL`，两两配对配不上）。
    //
    // 所以这里在分派点统计相邻指令对，跑完真实程序按频率排序，
    // 取前几名固化成超指令。代价是每轮多记一次表，只在 X_PROFILE
    // 打开时发生，正常路径零开销 —— 用 `if (unlikely)` 保证分支预测
    // 器不把它放进热路径。
    //
    // 表用一维稀疏数组：key = (prev_op << 8) | cur_op，只统计 256×256
    // 里实际出现的组合，用线性探测的开放寻址法，不做 resize
    // （P0 的指令种类不到 100 种，实际出现的对不超过几百个）。
    bool profiling = getenv("X_PROFILE") != NULL;
    static int  pair_keys[4096];
    static long pair_cnts[4096];
    int prev_op = -1;
    if (profiling) memset(pair_keys, 0xff, sizeof(pair_keys));  // -1 = 空

    for (;;) {
        Insn* in = &code[pc];
        // 位置不再从指令里取（Insn 只有 8 字节，没有 pos 字段了）。
        // 这里仍取一次是为了让下面所有 rt_error(pos, ...) 不用改，
        // 但编译器会把这个 load 沉到错误分支里做——因为 pos 在
        // 成功路径上没有使用者。真正的收益在于 code 数组小了 4 倍，
        // cache line 能装 8 条指令而不是 2 条。
        SrcPos pos = code_pos[pc];

        if (X_UNLIKELY(profiling)) {
            if (prev_op >= 0) {
                int key = (prev_op << 8) | (int)in->op;
                int slot = (key * 2654435761u) & 4095;
                while (pair_keys[slot] != -1 && pair_keys[slot] != key) {
                    slot = (slot + 1) & 4095;
                }
                pair_keys[slot] = key;
                pair_cnts[slot]++;
            }
            prev_op = (int)in->op;
        }

        switch (in->op) {

            // ---------------- 栈操作 ----------------
            case BC_NOP:
                pc++;
                break;

            case BC_PUSH_I64:
                vm_push(vm, v_int(consts[in->a].i), pos);
                pc++;
                break;

            case BC_PUSH_F64:
                vm_push(vm, v_float(consts[in->a].f), pos);
                pc++;
                break;

            case BC_PUSH_STR:
                vm_push(vm, v_str(str_new(consts[in->a].str.s,
                                          consts[in->a].str.len)), pos);
                pc++;
                break;

            case BC_PUSH_TRUE:
                vm_push(vm, v_bool(true), pos);
                pc++;
                break;

            case BC_PUSH_FALSE:
                vm_push(vm, v_bool(false), pos);
                pc++;
                break;

            case BC_PUSH_NULL:
                vm_push(vm, v_null(), pos);
                pc++;
                break;

            case BC_POP: {
                Value v = vm_pop(vm, pos);
                value_release(&v);
                pc++;
                break;
            }

            case BC_DUP: {
                if (vm->sp <= 0) {
                    rt_error(pos, "内部错误：DUP 时栈为空");
                    exit(70);
                }
                Value v = vm->stack[vm->sp - 1];
                // 复制出来的那份是独立持有，两种引用类型都要 retain
                value_retain(&v);
                vm_push(vm, v, pos);
                pc++;
                break;
            }

            // ---------------- 局部变量 ----------------
            case BC_LOAD_LOCAL: {
                Frame* f = &vm->frames[vm->fp - 1];
                Value v = vm->stack[f->base + in->a];
                // 栈上那份要独立持有引用。
                //
                // 这里曾经只处理 VT_STRING，加数组后忘了同步，
                // 结果 `let a = array(...)` 之后 len(a) 会崩：
                // LOAD 出来的那份没增引用，LEN 里 value_release 把它
                // 释放掉，数组对象直接没了。**编译器不会报这个错**
                // —— 类型是运行时判定的，gcc 看不见。
                // 这类 bug 只能靠「每种引用类型都在这里出现一次」守住。
                value_retain(&v);
                vm_push(vm, v, pos);
                pc++;
                break;
            }

            case BC_STORE_LOCAL: {
                Frame* f = &vm->frames[vm->fp - 1];
                Value v = vm_pop(vm, pos);
                Value* slot = &vm->stack[f->base + in->a];
                value_release(slot);        // 释放旧值
                *slot = v;                  // 转移所有权，不再 retain
                pc++;
                break;
            }

            // ---------------- 超指令（07 §3.3 L3）----------------
            // 这三条替代了实测最高频的三对相邻指令。收益来自两处：
            //   1. 少一次 switch 分派（少一次间接跳转 + 一次边界检查）
            //   2. 少一次 pc++ 与回跳
            // 语义与拆开执行**完全等价**，包括引用计数的行为——
            // 这是必须保证的：超指令只是把两条指令的代码抄到一起，
            // 任何观测差异（包括 GC 时机、错误位置）都算 bug。

            case BC_LOAD_LOAD: {
                // LOAD_LOCAL a; LOAD_LOCAL b
                // 两次压栈合并成一次 sp 推进，省一次写 sp 的内存操作。
                Frame* f = &vm->frames[vm->fp - 1];
                Value va = vm->stack[f->base + in->a];
                Value vb = vm->stack[f->base + in->b];
                value_retain(&va);
                value_retain(&vb);
                if (X_UNLIKELY(vm->sp + 2 > STACK_MAX)) {
                    rt_error(pos, "值栈溢出（上限 %d 个值）", STACK_MAX);
                    ret_code = 70;
                    goto done;
                }
                vm->stack[vm->sp++] = va;
                vm->stack[vm->sp++] = vb;
                pc++;
                break;
            }

            case BC_DUP_STORE: {
                // DUP; STORE_LOCAL a —— 赋值表达式（x = ...）的形态。
                //
                // 语义必须与「先 DUP 再 STORE_LOCAL」逐位等价：
                //   DUP        → 栈上变成两份，两份**各持一个引用**
                //   STORE_LOCAL → 弹出顶上那份，槽位接管它的所有权
                // 合并后栈上只留一份（就是原来那份），槽位需要**自己**
                // 持有一个引用——所以必须 retain 一次给槽位，而不是
                // 让栈上和槽位共享同一个未加计数的指针。
                //
                // 这个区别只在字符串上显现：数值是值语义，共享无所谓；
                // 字符串是引用计数对象，少 retain 会导致提前释放，
                // 表现为「拼接时拿到已释放内存」（实测报 CONCAT_STR 遇到
                // 非字符串值，因为内存在别名后被打乱）。
                Frame* f = &vm->frames[vm->fp - 1];
                if (X_UNLIKELY(vm->sp <= 0)) {
                    rt_error(pos, "内部错误：DUP_STORE 时栈为空");
                    ret_code = 70;
                    goto done;
                }
                Value* slot = &vm->stack[f->base + in->a];
                // 先取栈顶（不弹出——栈上要留着当表达式的值）
                Value v = vm->stack[vm->sp - 1];
                // 槽位的旧值先释放（可能和 v 是同一个对象，所以顺序很重要：
                // 先 release 旧值再 retain 新值，若同一对象则 rc 净不变，
                // 不会出现「释放后又 retain 已失效指针」）
                value_release(slot);
                value_retain(&v);      // 槽位自己持一个
                *slot = v;
                pc++;
                break;
            }

            case BC_STORE_POP: {
                // STORE_LOCAL a; POP
                // 赋值语句的收尾：值存进局部变量后直接弹掉。
                // 合并后不需要先 pop 再 release，省一次所有权转移。
                Frame* f = &vm->frames[vm->fp - 1];
                if (X_UNLIKELY(vm->sp <= 0)) {
                    rt_error(pos, "内部错误：STORE_POP 时栈为空");
                    ret_code = 70;
                    goto done;
                }
                Value v = vm->stack[--vm->sp];
                Value* slot = &vm->stack[f->base + in->a];
                value_release(slot);
                *slot = v;
                pc++;
                break;
            }

            case BC_DUP_STORE_POP: {
                // DUP_STORE a; POP —— 语义等价于 STORE_LOCAL a。
                //
                // 为什么需要这一条：赋值语句原本编成 DUP; STORE; POP 三条，
                // 上一轮超指令把 DUP; STORE 合成 DUP_STORE，于是变成
                // DUP_STORE; POP 两条，再合一次就只剩一条。
                // 实测这是循环体里的第一名（3000 万次循环出现 6000 万次）。
                //
                // 与 PC_STORE_POP 的区别值得注意：STORE_POP 是
                // 「STORE_LOCAL + POP」，DUP_STORE_POP 是
                // 「DUP + STORE_LOCAL + POP」，两者语义**完全相同**，
                // 都是「弹出栈顶，存进槽位」。之所以保留两个 opcode，
                // 是因为合并规则从不同的前驱形态出发，无法在
                // try_merge_pair 里统一（一个是 STORE 在左，一个是
                // DUP_STORE 在左）。解释器实现可以共用一套逻辑。
                Frame* f = &vm->frames[vm->fp - 1];
                if (X_UNLIKELY(vm->sp <= 0)) {
                    rt_error(pos, "内部错误：DUP_STORE_POP 时栈为空");
                    ret_code = 70;
                    goto done;
                }
                Value v = vm->stack[--vm->sp];
                Value* slot = &vm->stack[f->base + in->a];
                value_release(slot);
                *slot = v;
                pc++;
                break;
            }

            case BC_ADD_STORE: {
                // ADD_I64; DUP_STORE_POP a —— `x += i` 的终态。
                //
                // 净效果：弹出栈顶两个整数，相加，结果存进槽位 a，
                // 栈上不留值。这比「ADD_I64 → DUP_STORE → POP」省掉
                // 两次 sp 调整和一次中间值落地。
                Value* av = &vm->stack[vm->sp - 2];
                Value* bv = &vm->stack[vm->sp - 1];
                int64_t r;
                if (X_UNLIKELY(add_ovf(av->i, bv->i, &r))) {
                    overflow_error(pos, "+");
                    ret_code = 70;
                    goto done;
                }
                vm->sp -= 2;
                Frame* f = &vm->frames[vm->fp - 1];
                Value* slot = &vm->stack[f->base + in->a];
                value_release(slot);
                *slot = v_int(r);
                pc++;
                break;
            }

            case BC_MOD_STORE: {
                // MOD_I64; DUP_STORE_POP a
                // 与 ADD_STORE 同构，只是运算是取模（含除零检查）。
                int64_t b = vm->stack[vm->sp - 1].i;
                int64_t a = vm->stack[vm->sp - 2].i;
                if (X_UNLIKELY(b == 0)) {
                    rt_error(pos, "整数取模的除数为零");
                    ret_code = 70;
                    goto done;
                }
                int64_t r = (a == INT64_MIN && b == -1) ? 0 : (a % b);
                vm->sp -= 2;
                Frame* f = &vm->frames[vm->fp - 1];
                Value* slot = &vm->stack[f->base + in->a];
                value_release(slot);
                *slot = v_int(r);
                pc++;
                break;
            }

            case BC_LOAD_CONST: {
                // LOAD_LOCAL a; PUSH_I64 c
                // 注意常量不是 Int 时（P0 里 PUSH_I64 一定带 CONST_I64）无需判断，
                // 但为了语义等价仍然按常量池的真实类型构造值——
                // 若将来 PUSH_F64 也参与合并，这里不用改。
                Frame* f = &vm->frames[vm->fp - 1];
                Value va = vm->stack[f->base + in->a];
                value_retain(&va);
                Constant* c = &consts[in->b];
                Value vb;
                switch (c->kind) {
                    case CONST_I64: vb = v_int(c->i); break;
                    case CONST_F64: vb = v_float(c->f); break;
                    default:        vb = v_str(str_new(c->str.s, c->str.len)); break;
                }
                if (X_UNLIKELY(vm->sp + 2 > STACK_MAX)) {
                    rt_error(pos, "值栈溢出（上限 %d 个值）", STACK_MAX);
                    ret_code = 70;
                    goto done;
                }
                vm->stack[vm->sp++] = va;
                vm->stack[vm->sp++] = vb;
                pc++;
                break;
            }

            case BC_LT_JUMP_FALSE: {
                // LT_I64; JUMP_IF_FALSE t
                // 语义必须与「先 LT_I64 再 JUMP_IF_FALSE」完全一致：
                // 弹出两个整数比较，若条件为假则跳转到 t。
                // LT_I64 本身也会把 Bool 压栈再由 JUMP_IF_FALSE 弹掉，
                // 合并后这个中间 Bool 完全不落地——这是收益的主要来源。
                if (X_UNLIKELY(vm->sp < 2)) {
                    rt_error(pos, "内部错误：LT_JUMP_FALSE 时栈内不足两个值");
                    ret_code = 70;
                    goto done;
                }
                Value* bv = &vm->stack[--vm->sp];
                Value* av = &vm->stack[--vm->sp];
                bool cond;
                if (X_LIKELY(av->type == VT_INT && bv->type == VT_INT)) {
                    cond = av->i < bv->i;
                } else {
                    // 非整数比较（P0 里编译器只在这两个都是 Int 时才生成
                    // LT_I64，走到这里说明是 .xbc 被外部篡改或是编译器 bug）
                    rt_error(pos, "`<` 需要两个整数，实际是 %s 和 %s\n"
                              "  help: 01 §12 禁止隐式类型转换，浮点比较请用 `<` 的浮点形态",
                              type_label(av->type), type_label(bv->type));
                    ret_code = 70;
                    goto done;
                }
                pc = cond ? pc + 1 : pc + 1 + in->a;
                break;
            }

            // ---------------- 整数算术 ----------------
            // 这里是热路径中的热路径。特化指令直接用 native 运算，
            // 连类型检查都不做——编译器已经保证栈顶两个都是 Int。
            // 但溢出必须检查（01 §5.4）：用 __builtin_*_overflow，
            // 在 x86-64 上编译成几条 `jo` 指令，成本被分支预测吃掉。
            case BC_ADD_I64: {
                // 热路径这样写的原因（比原来的 add_checked 快一截）：
                //  · 栈顶两个值只解引用一次，指针留在寄存器里
                //  · 溢出检查是纯计算，不带函数调用、不碰 pos
                //  · 出错才 goto 到冷路径（标了 cold+noinline）
                //
                // 注意 sp_ 的类型是 Value*，不是 int64_t*。
                // 若写成 int64_t* 再去取 sp_[1]，会跨过 Value 之间的
                // padding 读到 type 字段的低位——实测表现为循环静默
                // 不执行（sum 永远是初值），这种 bug 极难从现象反推。
                Value* av = &vm->stack[vm->sp - 2];
                Value* bv = &vm->stack[vm->sp - 1];
                int64_t r;
                if (X_UNLIKELY(add_ovf(av->i, bv->i, &r))) {
                    overflow_error(pos, "+");
                    ret_code = 70;
                    goto done;
                }
                av->i = r;
                vm->sp--;
                pc++;
                break;
            }
            case BC_SUB_I64: {
                Value* av = &vm->stack[vm->sp - 2];
                Value* bv = &vm->stack[vm->sp - 1];
                int64_t r;
                if (X_UNLIKELY(sub_ovf(av->i, bv->i, &r))) {
                    overflow_error(pos, "-");
                    ret_code = 70;
                    goto done;
                }
                av->i = r;
                vm->sp--;
                pc++;
                break;
            }
            case BC_MUL_I64: {
                Value* av = &vm->stack[vm->sp - 2];
                Value* bv = &vm->stack[vm->sp - 1];
                int64_t r;
                if (X_UNLIKELY(mul_ovf(av->i, bv->i, &r))) {
                    overflow_error(pos, "*");
                    ret_code = 70;
                    goto done;
                }
                av->i = r;
                vm->sp--;
                pc++;
                break;
            }
            case BC_DIV_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                int64_t a = vm->stack[vm->sp - 1].i;
                if (b == 0) {
                    rt_error(pos, "整数除以零\n  note: Int 的 `/` 是截断除法，除数为 0 无法定义\n  help: 用 `if b != 0 { ... }` 先判断，或改用 Float 除法");
                    ret_code = 70;
                    goto done;
                }
                // INT64_MIN / -1 会溢出（唯一一个除法溢出情形）
                if (a == INT64_MIN && b == -1) {
                    overflow_error(pos, "/");
                    ret_code = 70;
                    goto done;
                }
                // 截断向零（C99 的 / 就是这个语义），与 01 §5.4 一致
                vm->stack[vm->sp - 1].i = a / b;
                pc++;
                break;
            }
            case BC_MOD_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                int64_t a = vm->stack[vm->sp - 1].i;
                if (b == 0) {
                    rt_error(pos, "整数取模的除数为零");
                    ret_code = 70;
                    goto done;
                }
                // INT64_MIN % -1 在 C 里是 UB，结果为 0
                vm->stack[vm->sp - 1].i = (a == INT64_MIN && b == -1) ? 0 : (a % b);
                pc++;
                break;
            }
            case BC_NEG_I64: {
                int64_t* a = &vm->stack[vm->sp - 1].i;
                if (*a == INT64_MIN) {
                    overflow_error(pos, "-");
                    ret_code = 70;
                    goto done;
                }
                *a = -*a;
                pc++;
                break;
            }

            // ---------------- 浮点算术 ----------------
            case BC_ADD_F64: {
                double b = vm->stack[--vm->sp].f;
                vm->stack[vm->sp - 1].f += b;
                pc++;
                break;
            }
            case BC_SUB_F64: {
                double b = vm->stack[--vm->sp].f;
                vm->stack[vm->sp - 1].f -= b;
                pc++;
                break;
            }
            case BC_MUL_F64: {
                double b = vm->stack[--vm->sp].f;
                vm->stack[vm->sp - 1].f *= b;
                pc++;
                break;
            }
            case BC_DIV_F64: {
                double b = vm->stack[--vm->sp].f;
                // IEEE 754：Float 除零得 Inf，不报错（与 01 §4.3 一致）
                vm->stack[vm->sp - 1].f /= b;
                pc++;
                break;
            }
            case BC_NEG_F64:
                vm->stack[vm->sp - 1].f = -vm->stack[vm->sp - 1].f;
                pc++;
                break;

            // ---------------- 数值双分派 ----------------
            // 只在编译期类型不明、或 Int/Float 混算时走到这里。
            // 这就是 07 §3.2 说的「性格代价」——静态类型确定时永不到达。
            //
            // 注意 Int∩Int 分支也要做溢出检查：这条路径虽然不比特化指令常用，
            // 但语义必须与 ADD_I64 完全一致，否则「换个写法结果不同」。
            case BC_ADD_NUM:
            case BC_SUB_NUM:
            case BC_MUL_NUM: {
                Value bv = vm->stack[--vm->sp];
                Value* av = &vm->stack[vm->sp - 1];

                if (av->type == VT_INT && bv.type == VT_INT) {
                    int64_t r = 0;
                    bool ovf;
                    const char* opname = "+";
                    if (in->op == BC_ADD_NUM) {
                        ovf = add_ovf(av->i, bv.i, &r);
                    } else if (in->op == BC_SUB_NUM) {
                        opname = "-";
                        ovf = sub_ovf(av->i, bv.i, &r);
                    } else {
                        opname = "*";
                        ovf = mul_ovf(av->i, bv.i, &r);
                    }
                    if (X_UNLIKELY(ovf)) {
                        overflow_error(pos, opname);
                        ret_code = 70;
                        goto done;
                    }
                    av->i = r;
                } else {
                    double a, b;
                    if (!as_float(av, &a) || !as_float(&bv, &b)) {
                        rt_error(pos, "运算符需要数值，实际是 %s 和 %s\n"
                                  "  help: 01 §12 禁止隐式类型转换，请用 `toInt` 或 `toFloat` 显式转换",
                                  type_label(av->type), type_label(bv.type));
                        ret_code = 70; goto done;
                    }
                    av->type = VT_FLOAT;
                    if (in->op == BC_ADD_NUM)      av->f = a + b;
                    else if (in->op == BC_SUB_NUM) av->f = a - b;
                    else                           av->f = a * b;
                }
                pc++;
                break;
            }

            case BC_DIV_NUM: {
                Value bv = vm->stack[--vm->sp];
                Value* av = &vm->stack[vm->sp - 1];
                if (av->type == VT_INT && bv.type == VT_INT) {
                    if (bv.i == 0) {
                        rt_error(pos, "整数除以零");
                        ret_code = 70;
                        goto done;
                    }
                    if (av->i == INT64_MIN && bv.i == -1) {
                        overflow_error(pos, "/");
                        ret_code = 70;
                        goto done;
                    }
                    av->i = av->i / bv.i;
                } else {
                    double a, b;
                    if (!as_float(av, &a) || !as_float(&bv, &b)) {
                        rt_error(pos, "`/` 需要数值，实际是 %s 和 %s",
                                 type_label(av->type), type_label(bv.type));
                        ret_code = 70;
                        goto done;
                    }
                    av->type = VT_FLOAT;
                    av->f = a / b;
                }
                pc++;
                break;
            }

            // ---------------- 全动态 `+` ----------------
            // 可能是数值加法，也可能是字符串拼接。
            // 字符串参与时按「左边是字符串就拼，否则先转字符串」处理。
            case BC_ADD_DYN: {
                Value bv = vm_pop(vm, pos);
                Value av = vm_pop(vm, pos);

                if (av.type == VT_INT && bv.type == VT_INT) {
                    // 整数路径同样要检查溢出，语义与 ADD_I64 一致
                    int64_t r;
                    if (X_UNLIKELY(add_ovf(av.i, bv.i, &r))) {
                        overflow_error(pos, "+");
                        value_release(&av);
                        value_release(&bv);
                        ret_code = 70;
                        goto done;
                    }
                    vm_push(vm, v_int(r), pos);
                } else if ((av.type == VT_INT || av.type == VT_FLOAT) &&
                           (bv.type == VT_INT || bv.type == VT_FLOAT)) {
                    double a, b;
                    as_float(&av, &a);
                    as_float(&bv, &b);
                    vm_push(vm, v_float(a + b), pos);
                } else {
                    // 拼接：任一边是 String（或任一边是 null/bool）
                    XStr* as = value_to_str(av);
                    XStr* bs = value_to_str(bv);
                    XStr* r  = str_concat(as, bs);
                    str_release(as);
                    str_release(bs);
                    vm_push(vm, v_str(r), pos);
                }
                value_release(&av);
                value_release(&bv);
                pc++;
                break;
            }

            // ---------------- 相等 / 不等 ----------------
            case BC_EQ:
            case BC_NE: {
                Value bv = vm_pop(vm, pos);
                Value av = vm_pop(vm, pos);
                bool eq;

                if (av.type == VT_STRING && bv.type == VT_STRING) {
                    eq = (av.s->len == bv.s->len) &&
                         (memcmp(av.s->data, bv.s->data, av.s->len) == 0);
                } else if ((av.type == VT_INT || av.type == VT_FLOAT) &&
                           (bv.type == VT_INT || bv.type == VT_FLOAT)) {
                    // Int 与 Float 比较时按数值比较（3 == 3.0 为真）
                    double a, b;
                    as_float(&av, &a);
                    as_float(&bv, &b);
                    eq = (a == b);
                } else if (av.type == bv.type) {
                    switch (av.type) {
                        case VT_NULL: eq = true; break;
                        case VT_BOOL: eq = (av.b == bv.b); break;
                        // 数组按元素深度比较。曾经这里落到 default 的
                        // `eq = false`，于是 `a == a` 都返回 false ——
                        // 而这是最容易被信任的表达式，用户不会去验证它。
                        //
                        // 用与 arrIndexOf 相同的 value_equal：
                        // 两处规则不一致会让 arrContains(x, v) 与
                        // `any(x == v)` 给出不同答案，那是更难查的 bug。
                        case VT_ARRAY: eq = value_equal(&av, &bv); break;
                        // 结构体按字段逐个比（value_equal 里已按类型名
                        // 区分不同结构体）。漏了这一条的症状和数组当年
                        // 一模一样：`a == a` 返回 false，而这是最容易被
                        // 信任的表达式，用户不会去验证它。
                        case VT_STRUCT: eq = value_equal(&av, &bv); break;
                        // 类实例是**身份比较**：同一个对象才相等。
                        //
                        // 落到 default 的后果不是「比较不准」，而是
                        // `a == a` 恒为 false —— 这是最容易被信任的
                        // 表达式，用户不会去验证它，于是「判断两个变量
                        // 是不是同一个对象」这类逻辑静默失效。
                        case VT_CLASS:  eq = value_equal(&av, &bv); break;
                        default:      eq = false; break;
                    }
                } else {
                    eq = false;   // 类型不同 → 不相等，不报错
                }

                value_release(&av);
                value_release(&bv);
                vm_push(vm, v_bool(in->op == BC_EQ ? eq : !eq), pos);
                pc++;
                break;
            }

            // ---------------- 关系比较 ----------------
#define CMP_I64(name, op)                                                      \
            case name##_I64: {                                                 \
                int64_t b = vm->stack[--vm->sp].i;                             \
                int64_t a = vm->stack[vm->sp - 1].i;                           \
                vm->stack[vm->sp - 1].type = VT_BOOL;                          \
                vm->stack[vm->sp - 1].b = (a op b);                            \
                pc++;                                                          \
                break;                                                         \
            }

            CMP_I64(BC_LT, <)
            CMP_I64(BC_LE, <=)
            CMP_I64(BC_GT, >)
            CMP_I64(BC_GE, >=)

#define CMP_F64(name, op)                                                      \
            case name##_F64: {                                                 \
                double b = vm->stack[--vm->sp].f;                              \
                double a = vm->stack[vm->sp - 1].f;                            \
                vm->stack[vm->sp - 1].type = VT_BOOL;                          \
                vm->stack[vm->sp - 1].b = (a op b);                            \
                pc++;                                                          \
                break;                                                         \
            }

            CMP_F64(BC_LT, <)
            CMP_F64(BC_LE, <=)
            CMP_F64(BC_GT, >)
            CMP_F64(BC_GE, >=)

#define CMP_NUM(name, op)                                                      \
            case name##_NUM: {                                                 \
                Value bv = vm_pop(vm, pos);                                    \
                Value av = vm_pop(vm, pos);                                    \
                double a, b;                                                   \
                if (!as_float(&av, &a) || !as_float(&bv, &b)) {                \
                    rt_error(pos, "比较运算需要数值，实际是 %s 和 %s",          \
                             type_label(av.type), type_label(bv.type));        \
                    ret_code = 70; goto done;                                  \
                }                                                              \
                vm_push(vm, v_bool(a op b), pos);                              \
                pc++;                                                          \
                break;                                                         \
            }

            CMP_NUM(BC_LT, <)
            CMP_NUM(BC_LE, <=)
            CMP_NUM(BC_GT, >)
            CMP_NUM(BC_GE, >=)

            // ---------------- 位运算 ----------------
            case BC_BAND_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                vm->stack[vm->sp - 1].i &= b;
                pc++;
                break;
            }
            case BC_BOR_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                vm->stack[vm->sp - 1].i |= b;
                pc++;
                break;
            }
            case BC_BXOR_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                vm->stack[vm->sp - 1].i ^= b;
                pc++;
                break;
            }
            case BC_SHL_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                int64_t a = vm->stack[vm->sp - 1].i;
                // 01 §4.3 的明确规定（避免 UB）：移位数超出 [0,63] 时结果为 0；
                // 移位后若有效位被推出 64 位，也定义结果为 0，而不是回绕。
                // 后者是 X 与 C 的关键差异 —— C 里 `1 << 63` 是 UB，
                // 实测在 x86 上得到 INT64_MIN，这让「可移植的正确性」无法成立。
                if (b < 0 || b >= 64) {
                    vm->stack[vm->sp - 1].i = 0;
                } else {
                    uint64_t ua = (uint64_t)a;
                    uint64_t shifted = ua << b;
                    // 检查是否有位被移出：只有当 b > 0 时才需要
                    if (b > 0 && (shifted >> b) != ua) {
                        vm->stack[vm->sp - 1].i = 0;   // 溢出，定为 0
                    } else {
                        vm->stack[vm->sp - 1].i = (int64_t)shifted;
                    }
                }
                pc++;
                break;
            }
            case BC_SHR_I64: {
                int64_t b = vm->stack[--vm->sp].i;
                int64_t a = vm->stack[vm->sp - 1].i;
                // 算术右移（保留符号位）
                if (b < 0 || b >= 64) {
                    vm->stack[vm->sp - 1].i = (a < 0) ? -1 : 0;
                } else {
                    vm->stack[vm->sp - 1].i = a >> b;
                }
                pc++;
                break;
            }
            case BC_BNOT_I64:
                vm->stack[vm->sp - 1].i = ~vm->stack[vm->sp - 1].i;
                pc++;
                break;

            // ---------------- 逻辑 ----------------
            case BC_NOT: {
                Value v = vm_pop(vm, pos);
                bool t = truthy(v);
                value_release(&v);
                vm_push(vm, v_bool(!t), pos);
                pc++;
                break;
            }

            // ---------------- 类型转换 ----------------
            case BC_TOBOOL: {
                Value v = vm_pop(vm, pos);
                bool t = truthy(v);
                value_release(&v);
                vm_push(vm, v_bool(t), pos);
                pc++;
                break;
            }

            case BC_I64_TO_F64: {
                Value* v = vm_top(vm);
                if (!v || v->type != VT_INT) {
                    rt_error(pos, "内部错误：I64_TO_F64 遇到非 Int 值");
                    ret_code = 70;
                    goto done;
                }
                v->type = VT_FLOAT;
                v->f = (double)v->i;
                pc++;
                break;
            }

            case BC_TOINT: {
                Value v = vm_pop(vm, pos);
                int64_t r;
                switch (v.type) {
                    case VT_INT:   r = v.i; break;
                    case VT_FLOAT:
                        if (isnan(v.f) || isinf(v.f)) {
                            rt_error(pos, "`toInt` 无法转换 %s\n"
                                      "  reason: NaN 与 Infinity 没有对应的整数值",
                                      v.f != v.f ? "NaN" : "Infinity");
                            ret_code = 70; goto done;
                        }
                        r = (int64_t)v.f;
                        break;
                    case VT_BOOL:  r = v.b ? 1 : 0; break;
                    case VT_STRING: {
                        char* end = NULL;
                        errno = 0;
                        long long parsed = strtoll(v.s->data, &end, 10);
                        if (end == v.s->data || *end != '\0' || errno == ERANGE) {
                            rt_error(pos, "`toInt` 无法把字符串 \"%.*s\" 解析为整数\n"
                                      "  help: 字符串必须整体是一个十进制整数字面量",
                                      (int)v.s->len, v.s->data);
                            ret_code = 70; goto done;
                        }
                        r = (int64_t)parsed;
                        break;
                    }
                    case VT_NULL:
                        rt_error(pos, "`toInt` 无法转换 null");
                        ret_code = 70; goto done;
                    default:
                        r = 0; break;
                }
                value_release(&v);
                vm_push(vm, v_int(r), pos);
                pc++;
                break;
            }

            case BC_TOFLOAT: {
                Value v = vm_pop(vm, pos);
                double r;
                switch (v.type) {
                    case VT_FLOAT: r = v.f; break;
                    case VT_INT:   r = (double)v.i; break;
                    case VT_BOOL:  r = v.b ? 1.0 : 0.0; break;
                    case VT_STRING: {
                        char* end = NULL;
                        r = strtod(v.s->data, &end);
                        if (end == v.s->data || *end != '\0') {
                            rt_error(pos, "`toFloat` 无法把字符串 \"%.*s\" 解析为浮点数",
                                      (int)v.s->len, v.s->data);
                            ret_code = 70; goto done;
                        }
                        break;
                    }
                    case VT_NULL:
                        rt_error(pos, "`toFloat` 无法转换 null");
                        ret_code = 70; goto done;
                    default: r = 0.0; break;
                }
                value_release(&v);
                vm_push(vm, v_float(r), pos);
                pc++;
                break;
            }

            // ---------------- 转字符串 ----------------
            case BC_TOSTRING_I64: {
                Value* v = vm_top(vm);
                XStr* s = int_to_str(v->i);
                v->type = VT_STRING;
                v->s = s;
                pc++;
                break;
            }
            case BC_TOSTRING_F64: {
                Value* v = vm_top(vm);
                XStr* s = float_to_str(v->f);
                v->type = VT_STRING;
                v->s = s;
                pc++;
                break;
            }
            case BC_TOSTRING_BOOL: {
                Value* v = vm_top(vm);
                XStr* s = v->b ? str_new("true", 4) : str_new("false", 5);
                v->type = VT_STRING;
                v->s = s;
                pc++;
                break;
            }
            case BC_TOSTRING_NULL: {
                Value* v = vm_top(vm);
                v->type = VT_STRING;
                v->s = str_new("null", 4);
                pc++;
                break;
            }
            case BC_TOSTRING_ANY: {
                Value v = vm_pop(vm, pos);
                XStr* s = value_to_str(v);
                value_release(&v);
                vm_push(vm, v_str(s), pos);
                pc++;
                break;
            }

            // ---------------- 字符串拼接 ----------------
            case BC_CONCAT_STR: {
                Value bv = vm_pop(vm, pos);
                Value av = vm_pop(vm, pos);
                if (av.type != VT_STRING || bv.type != VT_STRING) {
                    rt_error(pos, "内部错误：CONCAT_STR 遇到非字符串值");
                    ret_code = 70; goto done;
                }
                XStr* r = str_concat(av.s, bv.s);
                str_release(av.s);
                str_release(bv.s);
                vm_push(vm, v_str(r), pos);
                pc++;
                break;
            }

            case BC_CONCAT_ANY: {
                Value bv = vm_pop(vm, pos);
                Value av = vm_pop(vm, pos);
                XStr* as = value_to_str(av);
                XStr* bs = value_to_str(bv);
                XStr* r  = str_concat(as, bs);
                str_release(as);
                str_release(bs);
                value_release(&av);
                value_release(&bv);
                vm_push(vm, v_str(r), pos);
                pc++;
                break;
            }

            // ---------------- 内建 ----------------
            case BC_ABS_I64: {
                int64_t v = vm->stack[vm->sp - 1].i;
                vm->stack[vm->sp - 1].i = v < 0 ? -v : v;
                pc++;
                break;
            }
            case BC_ABS_F64:
                vm->stack[vm->sp - 1].f = fabs(vm->stack[vm->sp - 1].f);
                pc++;
                break;

            case BC_LEN: {
                Value v = vm_pop(vm, pos);
                // String 与 Array 都能取长度。两者语义相同（元素个数），
                // 没有理由让用户记两套写法。
                //
                // ── 字符串必须数码点，不是字节 ──────────────────
                //
                // 曾经这里返回 v.s->len（字节数）。于是 len("张三") == 6，
                // 而 strAt("张三", 1) 按码点索引返回「三」—— 两个函数
                // 用了两套坐标系。后果是所有「for i in 0..len(s) 然后
                // strAt(s,i)」的代码在遇到中文时越界取空串，而 ASCII
                // 输入完全正常，所以测试里看不出来。
                //
                // 标准库的 JSON 解析就是被这个坑到的：中文键名解析到
                // 一半把后面的内容当成「多余字符」。
                int64_t n;
                if (v.type == VT_STRING) {
                    n = (int64_t)utf8_cplen(v.s->data, v.s->len);
                } else if (v.type == VT_ARRAY) {
                    n = (int64_t)v.a->len;
                } else {
                    rt_error(pos, "`len` 只接受 String 或 Array，实际是 %s\n"
                             "  note: `len` 返回元素个数，数字没有长度\n"
                             "  help: 字符串与数组都能取长度；数字请直接用，不需要 len",
                             type_label(v.type));
                    ret_code = 70;
                    goto done;
                }
                value_release(&v);
                vm_push(vm, v_int(n), pos);
                pc++;
                break;
            }

            // ---------------- 数组 ----------------
            case BC_NEW_ARRAY: {
                // 栈上从下到上是 [长度, 填充值]，填充值在最顶上。
                // 与 compile.c 的 array() 压栈顺序对应。
                Value fv = vm_pop(vm, pos);
                Value nv = vm_pop(vm, pos);

                if (nv.type != VT_INT) {
                    rt_error(pos, "数组长度必须是 Int，实际是 %s", type_label(nv.type));
                    ret_code = 70;
                    goto done;
                }
                if (nv.i < 0) {
                    rt_error(pos, "数组长度不能为负数，实际是 %lld\n"
                                  "  help: 检查长度表达式，或先用 `if n > 0` 守住",
                             (long long)nv.i);
                    ret_code = 70;
                    goto done;
                }
                // 上限保护：一个元素 16 字节，1 亿个就是 1.6 GB。
                // 不设上限的话，`array(9999999999, 0)` 会让进程被 OOM
                // 杀掉，用户看到的是「程序莫名消失」而不是错误信息。
                if (nv.i > 100 * 1000 * 1000) {
                    rt_error(pos, "数组长度 %lld 超出上限（1 亿）\n"
                                  "  help: 这么长的数组通常意味着长度算错了",
                             (long long)nv.i);
                    ret_code = 70;
                    goto done;
                }

                XArr* a = arr_new((size_t)nv.i, fv);
                vm_push(vm, (Value){ .type = VT_ARRAY, .a = a }, pos);
                // arr_new 已经替每个槽 retain 了 fill，这里释放栈上那份
                value_release(&fv);
                pc++;
                break;
            }

            case BC_INDEX_GET: {
                Value idx = vm_pop(vm, pos);
                Value obj = vm_pop(vm, pos);

                if (obj.type != VT_ARRAY) {
                    rt_error(pos, "只能用 `[]` 索引数组，实际是 %s", type_label(obj.type));
                    ret_code = 70;
                    goto done;
                }
                if (idx.type != VT_INT) {
                    rt_error(pos, "数组下标必须是 Int，实际是 %s", type_label(idx.type));
                    ret_code = 70;
                    goto done;
                }
                if (idx.i < 0 || (size_t)idx.i >= obj.a->len) {
                    // 越界必须报错，不能像 C 那样返回垃圾值。
                    // 报错信息里给出实际长度，用户一眼能看出差多少。
                    rt_error(pos, "数组下标越界：访问 [%lld]，数组长度 %zu\n"
                                  "  note: 合法下标是 0 到 %zu\n"
                                  "  help: 循环上界常见错误是用了 `<=` 而不是 `<`",
                             (long long)idx.i, obj.a->len,
                             obj.a->len ? obj.a->len - 1 : 0);
                    ret_code = 70;
                    goto done;
                }

                Value got = obj.a->data[idx.i];
                // 栈上那份是独立持有：String/Array 元素要 retain
                value_retain(&got);

                // ── 关键：这里**不能** release 容器 ─────────────────
                //
                // 容器的引用是 LOAD_LOCAL 为「借出来读一下」而 retain 的，
                // 读完之后栈上那份就该消失，不该去动容器本身的引用计数。
                //
                // 之前的写法在这里 value_release(&obj)，把 rc 从 2 减到 1
                // ——看起来没问题，但 vm_pop 只是 sp--，并不清空原槽位，
                // 于是同一个对象在「已弹出区域」里还留着一份引用。
                // 之后 vm_push 覆盖那块内存时那份引用凭空消失，
                // 净效果是少减一次；下一次 LOAD_LOCAL + INDEX_GET 就会
                // 把 rc 减到 0，数组在任何变量还指向它的时候被销毁。
                // 实测表现：`a[0]` 读出的值是对的，但程序立刻崩在退出时。
                //
                // 正确做法是**读不消耗**：容器那份引用随 sp-- 自然失效，
                // 我们只负责给元素 retain。

                vm_push(vm, got, pos);
                pc++;
                break;
            }

            case BC_INDEX_SET: {
                // 栈上：[容器, 下标, 值]（值最后压，所以先弹）
                Value val = vm_pop(vm, pos);
                Value idx = vm_pop(vm, pos);
                Value obj = vm_pop(vm, pos);

                if (obj.type != VT_ARRAY) {
                    rt_error(pos, "只能用 `[]` 索引数组，实际是 %s", type_label(obj.type));
                    ret_code = 70;
                    goto done;
                }
                if (idx.type != VT_INT) {
                    rt_error(pos, "数组下标必须是 Int，实际是 %s", type_label(idx.type));
                    ret_code = 70;
                    goto done;
                }
                if (idx.i < 0 || (size_t)idx.i >= obj.a->len) {
                    rt_error(pos, "数组下标越界：写入 [%lld]，数组长度 %zu\n"
                                  "  note: 合法下标是 0 到 %zu",
                             (long long)idx.i, obj.a->len,
                             obj.a->len ? obj.a->len - 1 : 0);
                    ret_code = 70;
                    goto done;
                }

                Value* slot = &obj.a->data[idx.i];
                value_release(slot);          // 释放旧值
                *slot = val;                  // 数组接管 val 的引用
                // 不动 val 的引用计数：所有权从栈转到数组

                // 赋值表达式把容器原样还回栈上。
                //
                // 这里**不 retain**：obj 是从栈上弹出时带过来的那份引用，
                // 现在原样放回去，引用数净变化为 0。若在这里 retain，
                // 每次 `a[i] = v` 都会多一个引用，数组永不被回收。
                //
                // 这也正好对上 BC_INDEX_SET 的栈效应 -2：
                // 弹 3 个（容器/下标/值）、压 1 个（容器），净 -2。
                vm_push(vm, obj, pos);
                pc++;
                break;
            }

            // ---------------- 结构体 ----------------
            case BC_NEW_STRUCT: {
                // 栈上从下到上是「按声明顺序的 N 个字段值」，
                // 最后压的（最后一个字段）在最顶上。
                // 编译期已经把用户写的字面量按声明顺序排好了，
                // 所以这里直接按顺序弹。
                int ti    = in->a;
                int count = in->b;

                if (ti < 0 || ti >= bc->struct_count) {
                    rt_error(pos, "内部错误：结构体类型索引 %d 越界（共 %d 个类型）\n"
                                  "  note: 这通常是 .xbc 文件与解释器版本不匹配",
                             ti, bc->struct_count);
                    ret_code = 70;
                    goto done;
                }
                StructType* st = &bc->structs[ti];
                if (count != st->field_count) {
                    rt_error(pos, "内部错误：结构体 `%s` 的字段数不一致"
                                  "（指令里 %d 个，类型表里 %d 个）",
                             st->name, count, st->field_count);
                    ret_code = 70;
                    goto done;
                }

                XObj* o = obj_new(ti, st->name, st->field_names, count);
                // 从后往前填：栈顶是最后一个字段。
                // 所有权直接转移给对象 —— 不 retain 也不 release，
                // 栈上那份引用随 sp 下移自然失效。
                for (int i = count - 1; i >= 0; i--) {
                    o->fields[i] = vm_pop(vm, pos);
                }
                vm_push(vm, v_obj(o), pos);
                pc++;
                break;
            }

            case BC_FIELD_GET: {
                Value obj = vm_pop(vm, pos);

                // ---- 类实例：字段读（引用语义） ----
                //
                // 与结构体共用同一条指令，因为偏移的计算方式完全一样
                // （父类在前、本类在后的全局下标）。差别只在**值怎么
                // 存**：结构体是一块不可变数据，类是可变对象的槽位。
                //
                // 这里刻意不 release obj：读取不消耗对象。栈上那份
                // obj 是表达式求值的临时结果，随 sp 下移自然失效 ——
                // 与 BC_FIELD_GET 的结构体分支、BC_INDEX_GET 同一条
                // 规则。提前 release 会让 `a.x + a.y` 里的第二次访问
                // 读到已释放对象。
                if (obj.type == VT_CLASS) {
                    int fi = in->b;
                    if (fi < 0 || fi >= obj.k->field_count) {
                        rt_error(pos,
                                 "内部错误：类 `%s` 的字段下标 %d 越界（共 %d 个）",
                                 obj.k->cls_name ? obj.k->cls_name : "?",
                                 fi, obj.k->field_count);
                        ret_code = 70;
                        goto done;
                    }
                    Value got = obj.k->fields[fi];
                    value_retain(&got);
                    vm_push(vm, got, pos);
                    pc++;
                    break;
                }

                if (obj.type != VT_STRUCT) {
                    rt_error(pos, "只能用 `.` 访问结构体或类的字段，实际是 %s\n"
                                  "  note: 数组用 `[]`，结构体/类用 `.`",
                             type_label(obj.type));
                    ret_code = 70;
                    goto done;
                }
                int fi = in->b;
                if (fi < 0 || fi >= obj.o->field_count) {
                    rt_error(pos, "内部错误：结构体 `%s` 的字段下标 %d 越界（共 %d 个字段）",
                             obj.o->type_name ? obj.o->type_name : "?",
                             fi, obj.o->field_count);
                    ret_code = 70;
                    goto done;
                }

                Value got = obj.o->fields[fi];
                // 与 BC_INDEX_GET 同一条规则：栈上那份是独立持有，
                // 元素/字段要 retain；容器本身**不 release**（读不消耗）。
                value_retain(&got);
                vm_push(vm, got, pos);
                pc++;
                break;
            }

            // ---------------- 类（v0.15）----------------
            case BC_NEW_CLASS: {
                int ci    = in->a;
                int argc  = in->b;

                if (ci < 0 || ci >= bc->class_count) {
                    rt_error(pos, "内部错误：类索引 %d 越界（共 %d 个类）\n"
                                  "  note: 通常是 .xbc 与解释器版本不匹配",
                             ci, bc->class_count);
                    ret_code = 70;
                    goto done;
                }
                ClassType* ct = &bc->classes[ci];

                // ── 分配 ────────────────────────────────────────
                //
                // 字段总数是**全量**（含继承）。分配之后全零，
                // 构造函数负责填真实初值。没有构造函数时零值就是终值。
                XInst* k = inst_new(ci, ct->name, ct->field_names,
                                    ct->field_count);
                Value kv = v_inst(k);

                // ── 调构造函数 ──────────────────────────────────
                //
                // 构造函数在函数表里的名字是 `Class.constructor`，
                // 第一个形参是 self。所以要把实例当作第 0 个实参
                // 压到栈上，再调。
                //
                // ── 为什么实参顺序是「先弹出、再压入」────────────
                //
                // 构造实参已经在栈上了（编译期按顺序压的），而
                // self 必须排在它们**下面**（槽位 0）。栈不能从中间
                // 插入，所以只能先全部弹出到临时数组，再依次压入
                // self + 实参。
                //
                // argc 是用户实参个数，实际形参个数是 argc + 1。
                {
                    // 找构造函数索引
                    size_t nl = strlen(ct->name);
                    char ctor_name[512];
                    int ctor_fi = -1;
                    if (nl + sizeof(".constructor") <= sizeof(ctor_name)) {
                        memcpy(ctor_name, ct->name, nl);
                        memcpy(ctor_name + nl, ".constructor", sizeof(".constructor"));
                        for (int i = 0; i < bc->func_count; i++) {
                            if (bc->funcs[i].name &&
                                strcmp(bc->funcs[i].name, ctor_name) == 0) {
                                ctor_fi = i;
                                break;
                            }
                        }
                    }

                    if (ctor_fi >= 0) {
                        // 弹出用户实参（逆序弹出，存进临时缓冲）
                        Value argv[16];
                        if (argc > 16) {
                            rt_error(pos,
                                     "构造函数实参过多（%d 个，上限 16）", argc);
                            ret_code = 70;
                            inst_release(k);
                            goto done;
                        }
                        for (int i = argc - 1; i >= 0; i--) {
                            argv[i] = vm_pop(vm, pos);
                        }
                        // 压 self
                        vm_push(vm, v_inst(inst_retain(k)), pos);
                        // 压实参
                        for (int i = 0; i < argc; i++) {
                            vm_push(vm, argv[i], pos);
                        }

                        FuncProto* cf = &bc->funcs[ctor_fi];
                        if (vm->fp >= FRAME_MAX) {
                            rt_error(pos, "调用深度超过上限 %d", FRAME_MAX);
                            ret_code = 70;
                            inst_release(k);
                            goto done;
                        }
                        int arg_base = vm->sp - cf->param_count;
                        for (int i = cf->param_count; i < cf->local_count; i++) {
                            vm->stack[vm->sp++].type = VT_NULL;
                        }
                        Frame* nf = vm_push_frame(vm, ctor_fi, pc + 1, arg_base, pos);
                        // 记录「这一帧是构造函数」，返回值要丢掉。
                        // 注意顺序：必须在 vm_push_frame 之后设，否则
                        // 会被那句 is_ctor = false 覆盖。
                        nf->is_ctor    = true;

                        // 构造帧同样要记归属类：构造函数里完全可以
                        // 调反射（比如按字段名批量取值做校验），而
                        // 那时「我在哪个类里」的答案就是正在构造的类。
                        //
                        // 这里直接写 ci 而不是走 func_owner_class ——
                        // 类索引就在手边，没必要再按名字反查一遍。
                        nf->cls_idx    = ci;

                        // 实例挂在这一帧上，BC_RETURN 时那一份直接转
                        // 交给栈。
                        //
                        // retain 一次是必需的：kv 这个 C 局部变量在图
                        // 中会随 break 消失，若不另留一份，构造函数体
                        // 里一旦有人把 self 的引用丢掉，实例就没了。
                        nf->this_inst  = v_inst(inst_retain(k));

                        // 然后立刻交还 kv 那一份 —— 帧里已经持有，
                        // 不还的话 rc 停在 2 而只有 1 个真实持有者，
                        // 每次 new 泄漏一个实例。
                        inst_release(k);

                        pc = cf->code_start;
                        break;
                    }
                }

                // 没有构造函数：实例直接可用，字段已是零值。
                vm_push(vm, kv, pos);
                pc++;
                break;
            }

            case BC_FIELD_SET: {
                Value val = vm_pop(vm, pos);
                Value obj = vm_pop(vm, pos);

                if (obj.type != VT_CLASS) {
                    rt_error(pos, "只能用 `.` 给类字段赋值，实际是 %s\n"
                                  "  note: 结构体字段不可变（值语义）；"
                                  "可变的共享状态请用 class",
                             type_label(obj.type));
                    value_release(&val);
                    value_release(&obj);
                    ret_code = 70;
                    goto done;
                }
                int fi = in->a;
                if (fi < 0 || fi >= obj.k->field_count) {
                    rt_error(pos, "内部错误：类 `%s` 的字段下标 %d 越界（共 %d 个）",
                             obj.k->cls_name ? obj.k->cls_name : "?",
                             fi, obj.k->field_count);
                    value_release(&val);
                    value_release(&obj);
                    ret_code = 70;
                    goto done;
                }

                // 原地改字段。旧值 release、新值所有权转移给实例。
                value_release(&obj.k->fields[fi]);
                obj.k->fields[fi] = val;    // 所有权归实例

                // 赋值表达式的值是**赋进去的那个值**（不是对象）。
                // 这样 `a.x = b.y = 1` 能正确串起来。
                //
                // ── retain 是必须的，否则 use-after-free ──────────
                //
                // val 的所有权刚刚已经交给实例字段。如果直接把它压
                // 回栈，就是「栈上 1 份 + 字段 1 份」共用同一个引用
                // 计数 —— 栈上那份被 POP 释放时 rc 归零，实例字段
                // 立刻悬空。
                //
                // Int 是不会暴露这个 bug 的（值类型没有引用计数），
                // 但 String / 数组 / 类实例字段会直接段错误，而且崩
                // 在后面的读取处，离这一行很远。
                value_retain(&val);
                vm_push(vm, val, pos);

                // obj 是 vm_pop 拿到的，用完要还 —— 不还就是每次
                // 字段赋值泄漏一份对象引用。
                value_release(&obj);
                pc++;
                break;
            }

            case BC_VIRT_CALL: {
                // 虚调用 —— 多态的全部实现。
                //
                // ── 派发过程 ────────────────────────────────────
                //
                // 1. 从栈上找到 self（它在所有实参之下）
                // 2. 取 self 的**实际类**（obj.k->cls_idx）
                // 3. 查该类的虚表，取槽 a
                // 4. 调用那个函数
                //
                // 第 2 步是关键字：静态类型在编译期就定死了，运行时
                // 只有实际类型能决定该执行谁。
                int slot = in->a;
                int argc = in->b;

                if (argc <= 0) {
                    rt_error(pos, "内部错误：虚调用缺少 self（argc=%d）", argc);
                    ret_code = 70;
                    goto done;
                }
                int self_pos = vm->sp - argc;
                Value self = vm->stack[self_pos];

                if (self.type != VT_CLASS) {
                    rt_error(pos, "内部错误：虚调用的接收者不是类实例，实际是 %s",
                             type_label(self.type));
                    ret_code = 70;
                    goto done;
                }
                int act_cls = self.k->cls_idx;
                if (act_cls < 0 || act_cls >= bc->class_count) {
                    rt_error(pos, "内部错误：实例的类索引 %d 越界", act_cls);
                    ret_code = 70;
                    goto done;
                }
                ClassType* act = &bc->classes[act_cls];
                if (slot < 0 || slot >= act->vtable_len) {
                    rt_error(pos, "内部错误：虚表槽 %d 越界（类 `%s` 有 %d 个槽）",
                             slot, act->name, act->vtable_len);
                    ret_code = 70;
                    goto done;
                }
                int callee = act->vtable[slot];
                if (callee < 0) {
                    const char* mname = (slot < bc->method_slot_count)
                                        ? bc->method_slots[slot].name : "?";
                    rt_error(pos,
                             "类 `%s` 没有实现方法 `%s`\n"
                             "  note: 静态类型上声明了这个方法，但实际类型没有实现。\n"
                             "        抽象方法必须由具体子类实现；接口方法必须在 implements 的类里实现",
                             act->name, mname);
                    ret_code = 70;
                    goto done;
                }

                FuncProto* cf = &bc->funcs[callee];
                if (vm->fp >= FRAME_MAX) {
                    rt_error(pos, "调用深度超过上限 %d，可能是无限递归", FRAME_MAX);
                    ret_code = 70;
                    goto done;
                }
                int arg_base = vm->sp - cf->param_count;
                for (int i = cf->param_count; i < cf->local_count; i++) {
                    vm->stack[vm->sp++].type = VT_NULL;
                }
                Frame* nf = vm_push_frame(vm, callee, pc + 1, arg_base, pos);
                // 记录「这一帧在哪个类的方法里」—— 反射判可见性要用。
                //
                // 用 act_cls（实际类）而不是静态类型：方法体里的
                // `self` 就是那个实际类的对象，反射看到它也应该是
                // 实际类。用静态类型会让「子类方法里读自己的 private
                // 字段」被误判成越权。
                nf->cls_idx = act_cls;
                pc = cf->code_start;
                break;
            }

            // ---------------- 动态调用（v0.20，框架支持）----------------
            case BC_NEW_CLASS_NAME: {
                // classNew("Name" [, 实参数组]) —— 按名字实例化。
                //
                // 与 BC_NEW_CLASS 的唯一差别：类索引不是编译期常量，
                // 而是栈顶的字符串，要现场查类表。其余流程（分配、
                // 找构造函数、推帧）逐行同构 —— 保持两处一致是有意的，
                // 改一处不改另一处会造出「new 正常、classNew 行为不同」
                // 这种最难查的分叉。
                //
                // b 是标志位：1 表示栈顶还有一个实参数组，弹出后按
                // 元素展开成构造实参。实参个数因此是运行时的 ——
                // 这正是反射调用点需要的（容器按 bean 定义传依赖）。
                int argc = 0;

                Value argv[16];
                if (in->b) {
                    Value av = vm_pop(vm, pos);
                    if (av.type != VT_ARRAY) {
                        rt_error(pos,
                                 "classNew 的第 2 个参数必须是实参数组，实际是 %s",
                                 type_label(av.type));
                        value_release(&av);
                        ret_code = 70;
                        goto done;
                    }
                    argc = av.a->len;
                    if (argc > 16) {
                        rt_error(pos,
                                 "classNew 构造实参过多（%d 个，上限 16）", argc);
                        value_release(&av);
                        ret_code = 70;
                        goto done;
                    }
                    // 元素所有权转移到 argv，数组壳随即释放。
                    for (int i = 0; i < argc; i++) argv[i] = av.a->data[i];
                    av.a->len = 0;
                    value_release(&av);
                }
                Value nv = vm_pop(vm, pos);
                if (nv.type != VT_STRING) {
                    rt_error(pos,
                             "classNew 的第 1 个参数必须是类名字符串，实际是 %s",
                             type_label(nv.type));
                    value_release(&nv);
                    ret_code = 70;
                    goto done;
                }

                int ci = -1;
                for (int i = 0; i < bc->class_count; i++) {
                    const char* cn = bc->classes[i].name;
                    if (cn && strcmp(cn, nv.s->data) == 0) { ci = i; break; }
                }
                if (ci < 0) {
                    rt_error(pos,
                             "classNew 找不到类 `%s`\n"
                             "  help: 用 classAll() 看字节码里有哪些类名",
                             nv.s->data);
                    value_release(&nv);
                    ret_code = 70;
                    goto done;
                }
                if (bc->classes[ci].is_interface) {
                    rt_error(pos,
                             "classNew 不能实例化接口 `%s`\n"
                             "  note: 接口没有字段也没有构造函数，"
                             "实现类的实例才是可用的对象",
                             bc->classes[ci].name);
                    value_release(&nv);
                    ret_code = 70;
                    goto done;
                }
                value_release(&nv);

                ClassType* ct = &bc->classes[ci];
                XInst* k = inst_new(ci, ct->name, ct->field_names,
                                    ct->field_count);
                Value kv = v_inst(k);

                // 找构造函数（与 BC_NEW_CLASS 相同的命名约定）。
                {
                    size_t nl = strlen(ct->name);
                    char ctor_name[512];
                    int ctor_fi = -1;
                    if (nl + sizeof(".constructor") <= sizeof(ctor_name)) {
                        memcpy(ctor_name, ct->name, nl);
                        memcpy(ctor_name + nl, ".constructor", sizeof(".constructor"));
                        for (int i = 0; i < bc->func_count; i++) {
                            if (bc->funcs[i].name &&
                                strcmp(bc->funcs[i].name, ctor_name) == 0) {
                                ctor_fi = i;
                                break;
                            }
                        }
                    }

                    if (ctor_fi >= 0) {
                        FuncProto* cf = &bc->funcs[ctor_fi];
                        if (vm->fp >= FRAME_MAX) {
                            rt_error(pos, "调用深度超过上限 %d", FRAME_MAX);
                            ret_code = 70;
                            inst_release(k);
                            goto done;
                        }
                        if (cf->param_count != argc + 1) {
                            rt_error(pos,
                                     "类 `%s` 的构造函数要 %d 个实参，"
                                     "classNew 给了 %d 个",
                                     ct->name, cf->param_count - 1, argc);
                            ret_code = 70;
                            inst_release(k);
                            goto done;
                        }
                        // 栈布局与 BC_NEW_CLASS 相同：self 在槽 0，
                        // 用户实参在其上。刚才弹进 argv 的值按序压回。
                        vm_push(vm, v_inst(inst_retain(k)), pos);
                        for (int i = 0; i < argc; i++) {
                            vm_push(vm, argv[i], pos);
                        }
                        int arg_base = vm->sp - cf->param_count;
                        for (int i = cf->param_count; i < cf->local_count; i++) {
                            vm->stack[vm->sp++].type = VT_NULL;
                        }
                        Frame* nf = vm_push_frame(vm, ctor_fi, pc + 1, arg_base, pos);
                        nf->is_ctor = true;
                        nf->cls_idx = ci;
                        nf->this_inst = v_inst(inst_retain(k));
                        inst_release(k);
                        pc = cf->code_start;
                        break;
                    }
                }

                // 没有构造函数：实例直接可用，字段已是零值。
                // argc > 0 却没有构造函数是调用点写错了。
                if (argc > 0) {
                    rt_error(pos,
                             "类 `%s` 没有构造函数，classNew 却给了 %d 个实参",
                             ct->name, argc);
                    ret_code = 70;
                    inst_release(k);
                    goto done;
                }
                vm_push(vm, kv, pos);
                pc++;
                break;
            }

            case BC_CALL_NAME: {
                // methodCall(obj, "name" [, 实参数组]) —— 按名字的方法调用。
                //
                // 派发路径与 BC_VIRT_CALL 一致：先找全局方法槽，再按
                // 接收者的**实际类**查虚表。名字来自运行时（注解里
                // 声明的路由方法名），槽号没法在编译期定下来。
                //
                // b 是标志位：1 表示栈顶还有一个实参数组，弹出后按
                // 元素展开成方法实参（与 BC_NEW_CLASS_NAME 同一约定）。
                int argc = 0;

                Value argv[16];
                if (in->b) {
                    Value av = vm_pop(vm, pos);
                    if (av.type != VT_ARRAY) {
                        rt_error(pos,
                                 "methodCall 的第 3 个参数必须是实参数组，实际是 %s",
                                 type_label(av.type));
                        value_release(&av);
                        ret_code = 70;
                        goto done;
                    }
                    argc = av.a->len;
                    if (argc > 16) {
                        rt_error(pos,
                                 "methodCall 实参过多（%d 个，上限 16）", argc);
                        value_release(&av);
                        ret_code = 70;
                        goto done;
                    }
                    for (int i = 0; i < argc; i++) argv[i] = av.a->data[i];
                    av.a->len = 0;
                    value_release(&av);
                }
                Value mv = vm_pop(vm, pos);
                if (mv.type != VT_STRING) {
                    rt_error(pos,
                             "methodCall 的第 2 个参数必须是方法名字符串，实际是 %s",
                             type_label(mv.type));
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }
                Value self = vm_pop(vm, pos);
                if (self.type != VT_CLASS) {
                    rt_error(pos,
                             "methodCall 的接收者必须是类实例，实际是 %s",
                             type_label(self.type));
                    value_release(&self);
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }
                int act_cls = self.k->cls_idx;
                if (act_cls < 0 || act_cls >= bc->class_count) {
                    rt_error(pos, "内部错误：实例的类索引 %d 越界", act_cls);
                    value_release(&self);
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }

                // 按名找全局方法槽。
                int slot = -1;
                for (int s = 0; s < bc->method_slot_count; s++) {
                    if (bc->method_slots[s].name &&
                        strcmp(bc->method_slots[s].name, mv.s->data) == 0) {
                        slot = s;
                        break;
                    }
                }
                if (slot < 0) {
                    rt_error(pos,
                             "methodCall 找不到方法 `%s`\n"
                             "  help: 方法名不带类名前缀；"
                             "用 classMethods(obj) 看有哪些方法",
                             mv.s->data);
                    value_release(&self);
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }

                ClassType* act = &bc->classes[act_cls];
                if (slot >= act->vtable_len) {
                    rt_error(pos,
                             "类 `%s` 没有方法 `%s`",
                             act->name, mv.s->data);
                    value_release(&self);
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }
                int callee = act->vtable[slot];
                if (callee < 0) {
                    rt_error(pos,
                             "类 `%s` 没有实现方法 `%s`\n"
                             "  note: 抽象方法必须由具体子类实现",
                             act->name, mv.s->data);
                    value_release(&self);
                    value_release(&mv);
                    ret_code = 70;
                    goto done;
                }

                // ── 可见性：反射调用不绕过封装 ──────────────────
                //
                // 与 rt_check_field_access 同一条原则：methodCall 是
                // 「运行时按名字调用」，不是「无视访问控制」。框架
                // 分派路由用的是 public 方法，这条检查拦住的是
                // `methodCall(obj, "secretOp")` 这种绕行。
                {
                    Visibility mv_vis = act->method_vis
                                        ? act->method_vis[slot] : VIS_PUBLIC;
                    if (mv_vis != VIS_PUBLIC) {
                        int owner = -1;
                        for (int c = act_cls; c >= 0;
                             c = bc->classes[c].parent_idx) {
                            if (bc->classes[c].method_vis &&
                                slot < bc->classes[c].vtable_len &&
                                bc->classes[c].method_vis[slot] == mv_vis &&
                                bc->classes[c].vtable[slot] == callee) {
                                owner = c;
                                break;
                            }
                        }
                        int cur = vm_cur_class(vm);
                        bool allowed = (cur >= 0 && owner >= 0 && cur == owner);
                        if (!allowed && cur >= 0 && owner >= 0 &&
                            mv_vis == VIS_PROTECTED) {
                            allowed = bc_class_is_a(bc, cur, owner);
                        }
                        if (!allowed) {
                            rt_error(pos,
                                     "反射调用方法 `%s.%s` 被拒绝：它是 %s 的\n"
                                     "  note: methodCall 不绕过访问控制 —— "
                                     "否则 private 方法形同虚设",
                                     act->name, mv.s->data, vis_label(mv_vis));
                            value_release(&self);
                            value_release(&mv);
                            ret_code = 70;
                            goto done;
                        }
                    }
                }
                value_release(&mv);

                FuncProto* cf = &bc->funcs[callee];
                if (vm->fp >= FRAME_MAX) {
                    rt_error(pos, "调用深度超过上限 %d，可能是无限递归", FRAME_MAX);
                    value_release(&self);
                    ret_code = 70;
                    goto done;
                }
                if (cf->param_count != argc + 1) {
                    rt_error(pos,
                             "方法 `%s.%s` 要 %d 个实参，methodCall 给了 %d 个",
                             act->name,
                             bc->method_slots[slot].name
                                 ? bc->method_slots[slot].name : "?",
                             cf->param_count - 1, argc);
                    value_release(&self);
                    ret_code = 70;
                    goto done;
                }

                // 把 self + 实参压回栈（与 BC_VIRT_CALL 的栈布局一致：
                // self 在最下，实参在上），再推帧。
                vm_push(vm, v_inst(inst_retain(self.k)), pos);
                value_release(&self);
                for (int i = 0; i < argc; i++) {
                    vm_push(vm, argv[i], pos);
                }
                int arg_base = vm->sp - cf->param_count;
                for (int i = cf->param_count; i < cf->local_count; i++) {
                    vm->stack[vm->sp++].type = VT_NULL;
                }
                Frame* nf = vm_push_frame(vm, callee, pc + 1, arg_base, pos);
                nf->cls_idx = act_cls;
                pc = cf->code_start;
                break;
            }

            // ---------------- 平台内建 ----------------
            case BC_BUILTIN: {
                Value rv = run_builtin(vm, pos, in);
                vm_push(vm, rv, pos);
                pc++;
                break;
            }

            // ---------------- 控制流 ----------------
            case BC_JUMP:
                // a 是**函数内相对偏移**，基准是本条指令的**下一条**。
                // 见 x.h 里 Insn 的说明：绝对下标会让「全程序超过
                // 32767 条指令」变成硬上限，而一个组件库就吃掉了
                // 大半预算。
                //
                // `pc + 1 + a` 里的 +1 不能省：pc 此刻指着本条指令，
                // 而 a 是相对它下一条的距离。漏掉 +1 的症状是每次
                // 跳转都少走一条 —— 循环跑到第二次就栈下溢。
                pc = pc + 1 + in->a;
                break;

            case BC_JUMP_IF_FALSE: {
                Value v = vm_pop(vm, pos);
                bool t = truthy(v);
                value_release(&v);
                pc = t ? pc + 1 : pc + 1 + in->a;
                break;
            }

            case BC_JUMP_IF_TRUE: {
                Value v = vm_pop(vm, pos);
                bool t = truthy(v);
                value_release(&v);
                pc = t ? pc + 1 + in->a : pc + 1;
                break;
            }

            // ---------------- 调用 ----------------
            case BC_CALL: {
                int callee = in->a;
                FuncProto* cf = &bc->funcs[callee];

                if (vm->fp >= FRAME_MAX) {
                    rt_error(pos, "调用深度超过上限 %d，可能是无限递归\n"
                              "  help: 检查递归的终止条件，或改用循环",
                              FRAME_MAX);
                    ret_code = 70;
                    goto done;
                }

                // 实参已经在栈上（按顺序压入），它们会成为被调函数的参数槽位。
                // 新帧的 base 指向第一个实参的位置。
                int arg_base = vm->sp - cf->param_count;

                // 剩余局部变量（非参数部分）预填 null
                for (int i = cf->param_count; i < cf->local_count; i++) {
                    vm->stack[vm->sp++].type = VT_NULL;
                }

                Frame* nf = vm_push_frame(vm, callee, pc + 1, arg_base, pos);
                // 普通调用也可能是类方法：
                //   · `self.foo()` —— 编译器把 self 当第一个参数，
                //     走的就是 BC_CALL（静态类型已知，不需要派发）
                //   · `Klass.staticFn()` —— 静态方法
                //   · 构造函数 `Klass.constructor` —— 见 BC_NEW_CLASS
                //
                // 不填的话，类方法里的反射会把「我自己」当成类外，
                // 于是读自己的 private 字段被拒 —— 反向 bug，同样
                // 难查。所以这里必须按名字反查归属类。
                nf->cls_idx = func_owner_class(bc, callee);

                pc = cf->code_start;
                break;
            }

            case BC_RETURN: {
                Value rv = vm_pop(vm, pos);
                Frame* f = &vm->frames[vm->fp - 1];

                // 构造函数没有返回值：丢掉栈上的值，改压暂存的实例。
                //
                // 不这么做的后果见 Frame.is_ctor 的注释 —— 用户拿到
                // null，然后在离原因极远的地方崩。
                bool was_ctor = f->is_ctor;
                if (was_ctor) {
                    // rv 一定是编译器补的 `return null`，但仍按通用
                    // 路径释放：万一将来允许构造函数 `return`，这里
                    // 不会泄漏。
                    value_release(&rv);
                    // 所有权从帧转给栈。
                    rv = f->this_inst;
                    f->this_inst = v_null();
                }

                // 释放本帧的局部变量
                for (int i = f->base; i < vm->sp; i++) {
                    value_release(&vm->stack[i]);
                }
                vm->sp = f->base;

                int ret_pc = f->ret_pc;
                int callee_idx = f->func_index;
                vm->fp--;

                // main 返回值即进程退出码
                if (callee_idx == main_idx && vm->fp == 0) {
                    if (rv.type == VT_INT) ret_code = (int)(rv.i & 0xFF);
                    else ret_code = 0;
                    value_release(&rv);
                    goto done;
                }

                vm_push(vm, rv, pos);
                pc = ret_pc;
                break;
            }

            case BC_RETURN_VOID: {
                Frame* f = &vm->frames[vm->fp - 1];

                // 空构造函数的函数体也会走这里（编译器补的兜底返回
                // 可能是 RETURN 也可能是 RETURN_VOID）。两者必须都处
                // 理 —— 只处理 RETURN 的话，写一个空 constructor 的
                // 类就会 `new` 出 null。
                Value rv;
                if (f->is_ctor) {
                    rv = f->this_inst;
                    f->this_inst = v_null();
                } else {
                    rv = v_null();
                }

                for (int i = f->base; i < vm->sp; i++) {
                    value_release(&vm->stack[i]);
                }
                vm->sp = f->base;

                int ret_pc = f->ret_pc;
                int callee_idx = f->func_index;
                vm->fp--;

                if (callee_idx == main_idx && vm->fp == 0) {
                    ret_code = 0;
                    goto done;
                }

                vm_push(vm, rv, pos);
                pc = ret_pc;
                break;
            }

            // ---------------- 输出 ----------------
            case BC_PRINT: {
                Value v = vm_pop(vm, pos);
                if (v.type == VT_STRING) {
                    fwrite(v.s->data, 1, v.s->len, stdout);
                } else {
                    XStr* s = value_to_str(v);
                    fwrite(s->data, 1, s->len, stdout);
                    str_release(s);
                }
                value_release(&v);
                pc++;
                break;
            }

            case BC_HALT:
                goto done;

            case BC_COUNT:
                rt_error(pos, "内部错误：遇到非法指令 BC_COUNT");
                ret_code = 70;
                goto done;
        }
    }

done:
    fflush(stdout);

    // 打印指令对直方图（07 §3.3.1）
    //
    // 输出到 stderr，不干扰程序本身的 stdout —— 这样才能放进管子里
    // 做 `x run foo.xle 2>&1 >/dev/null | head` 之类的分析。
    // 只打印出现次数 > 0 的，按频率降序，前 20 名。
    if (profiling) {
        // 收集非空槽位
        typedef struct { long cnt; int key; } PairRow;
        PairRow rows[4096];
        int rc_ = 0;
        for (int i = 0; i < 4096; i++) {
            if (pair_keys[i] != -1 && pair_cnts[i] > 0) {
                rows[rc_].cnt = pair_cnts[i];
                rows[rc_].key = pair_keys[i];
                rc_++;
            }
        }
        // 简单插入排序（几百个元素，不值得上 qsort 写比较函数）
        for (int i = 1; i < rc_; i++) {
            PairRow t = rows[i];
            int j = i - 1;
            while (j >= 0 && rows[j].cnt < t.cnt) {
                rows[j + 1] = rows[j];
                j--;
            }
            rows[j + 1] = t;
        }

        fprintf(stderr, "\n--- 指令对直方图（前 20 名）---\n");
        fprintf(stderr, "%-22s %-22s %10s\n", "前一条", "后一条", "次数");
        for (int i = 0; i < rc_ && i < 20; i++) {
            int a = rows[i].key >> 8;
            int b = rows[i].key & 0xff;
            fprintf(stderr, "%-22s %-22s %10ld\n",
                    bc_op_name((OpCode)a), bc_op_name((OpCode)b), rows[i].cnt);
        }
        fprintf(stderr, "（共 %d 种不同组合）\n\n", rc_);
    }

    // 清理剩余栈上的值（main 的局部变量）
    for (int i = 0; i < vm->sp; i++) {
        value_release(&vm->stack[i]);
    }
    free(vm);
    return ret_code;
}