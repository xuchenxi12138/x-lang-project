// codegen_c.c — C 后端：字节码 → 自包含 C 代码 → 原生可执行文件
//
// ── 为什么用 C 作为中间表示，而不是直接发机器码 ──────────────
//
// 直接发机器码要实现：指令编码器、寄存器分配、重定位、目标文件
// 封装（ELF/PE/Mach-O）、链接。前两项约 3000 行，后三项约 3000 行，
// 而且**每个 CPU 架构要重写一遍**。这是 5 人年量级的工作，02 §5.3
// 已经裁过这个问题：不自研汇编器与链接器。
//
// 走 C 后端的好处是数量级的：
//   · 三平台（Windows PE / Linux ELF / macOS Mach-O）由同一个
//     代码路径产出，因为 gcc/clang 已经知道怎么生成它们
//   · 架构支持（x86-64 / ARM64）免费获得
//   · 拿到 gcc -O2 的全部优化（寄存器分配、内联、向量化）——
//     这些正是 02 §1 说的「性能真正来源」
//   · 出错时用户手上是一份可读的 C 代码，能自己定位问题
//
// 代价：目标平台需要有 C 编译器。这在开发机上永远成立，
// 交叉编译时靠交叉工具链（--cc 指定），而工具链缺失会明确报错。
//
// ── 生成的代码长什么样 ──────────────────────────────────────
//
// 关键决定：**做真正的 AOT，不把解释器塞进产物。**
//
// 两种做法都能"跑通"，但差别巨大：
//
//   做法 A（懒）：产物 = 字节码 + 一个内联的解释器
//     优点：生成器 200 行就够
//     缺点：产物里还是 switch 分派，性能与 `x run` 完全一样，
//           「编译成 exe」只是把解释器打包了一遍，没有任何收益
//
//   做法 B（本文件）：产物 = 每条字节码展开成一条 C 语句
//     优点：gcc -O2 能跨指令优化（常量折叠、公共子表达式、
//           寄存器分配、内联），性能进入 C 的量级
//     缺点：生成器要处理全部 70 多个 opcode，且要自己管栈平衡
//
// 选 B。理由：用户要「编译成 exe」的动机就是**性能与可分发**，
// 做法 A 两个都没给。判断标准很简单——做法 A 的产物跑起来
// 跟 `x run` 一样慢，那这个功能存在的意义就只剩"不用装 x.exe"，
// 而 02 文档的 AOT 目标写的是「0.9~1.1 倍 native」。
//
// ── 栈模型 ──────────────────────────────────────────────────
//
// 沿用解释器的「值栈 + 帧基址」模型，但把 sp/base 变成**编译期常量**。
// 这是能做到的关键：字节码是栈式的，任意位置的栈深都是可静态计算的
// （编译器已经保证了栈平衡）。所以生成的不是 `s[sp++]`，而是 `s[3]`
// ——直接编译成内存偏移，gcc 会把它们优化进寄存器。
//
// 具体做法：为每个函数单独计算一张「pc → 栈深」表。遇到栈深
// 不可确定的指令（比如 JUMP 之后），说明代码生成有问题，直接报错
// 而不是猜——猜错会产出静默算错的程序，那种 bug 最贵。

#include "x.h"

#include <errno.h>
#include <time.h>

// 注意：这里**不需要**任何平台的 spawn 头文件。
// 调编译器这件事已经完全封在 plat_spawn_with_tmp() 里
// （见 platform.c），本文件只管拼参数。这条边界由
// tools/check_portable.sh 的第 2 项守着——本次就是它抓到
// 这里残留了一段 `#include <sys/wait.h>`。

// ============================================================
// 输出缓冲
// ============================================================

// 窗口运行时的内嵌副本（构建期由 tools/gen_gui_embed.py 生成）。
// 它是 gui.h + gui_win32.c 的 C 字符串形式，见下方 bc_uses_gui 的说明。
#include "gui/gui_embed.h"

typedef struct {
    FILE* f;
    int   indent;   // 预留：将来做缩进美化时用
} Emit;

// ============================================================
// 运行时预置代码
//
// 这段代码**原样贴进产物**，所以它必须自包含：只依赖 C 标准库，
// 不依赖 x.h、不依赖解释器、不依赖任何 X 的源文件。
//
// 与解释器的关系：语义必须逐位一致（这是「两档输出一致」的基础），
// 但实现可以不同——这里为了性能把字符串做成可变长结构，
// 而解释器用的是引用计数 + 内联数据。两边只要**可观测行为**
// 一致即可（同样的输入产出同样的字节，同样的错误报同样的消息）。
// ============================================================

static const char* PRELUDE =
"/* 由 X 编译器生成。不要手工编辑。 */\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <stdint.h>\n"
"#include <stdbool.h>\n"
"#include <math.h>\n"
"#include <errno.h>\n"
"#include <time.h>\n"
"\n"
"/* ---------- 值 ---------- */\n"
"typedef enum { XV_NULL=0, XV_INT, XV_FLOAT, XV_BOOL, XV_STRING, XV_ARRAY, XV_STRUCT, XV_CLASS } VType;\n"
"\n"
"typedef struct XS {\n"
"    long   rc;\n"
"    size_t len;\n"
"    char   d[];\n"
"} XS;\n"
"\n"
"/* 数组与值互相引用：数组的元素是值，值里可以装数组。\n"
"   两个 typedef 不能直接互相引用，所以值用 tag 形式声明。 */\n"
"typedef struct V_s V;\n"
"typedef struct XO XO;\n"
"typedef struct XK XK;\n"
"/* 数组带 cap，数据区独立分配（不是柔性数组）。\n"
"   必须能增长：strSplit 的结果长度不定、json 解析边扫边追加、\n"
"   矩阵拼接要扩容。柔性数组无法 realloc 尾部，只能整块搬走并\n"
"   更新每个指向它的值，代价比 cap 高得多。 */\n"
"typedef struct XA { long rc; size_t len; size_t cap; V* d; } XA;\n"
"/* 结构体对象。布局与解释器的 XObj 逐字段对应：rc 引用计数，\n"
"   type_name 指向静态字符串（生成的类型名表），field_names 指向\n"
"   静态字段名表（只用于打印），fields 是值数组。\n"
"   \n"
"   为什么 AOT 侧不生成真正的 C struct（每个 X 结构体一个 C 类型）：\n"
"   那样字段访问能变成一次 C 成员读取，快得多。但它要求**生成期**\n"
"   就知道所有字段类型，而 X 的结构体字段可以是动态的（Array 元素\n"
"   装什么类型编译期不定）。更致命的是：值栈 s[] 是 V 的数组，\n"
"   每个 V 必须等大 —— 一旦结构体有自己的 C 类型，栈上就得存指针\n"
"   加偏移，整套栈模型都要改。当前做法与解释器结构完全同构，\n"
"   代价是字段访问多一次间接寻址，收益是两档语义天然一致。 */\n"
"struct XO {\n"
"    long rc;\n"
"    int  ti;                 /* 类型索引 */\n"
"    const char* tname;\n"
"    const char** fnames;\n"
"    int  fc;\n"
"    V*   f;\n"
"};\n"
"struct V_s { VType t; union { int64_t i; double f; bool b; XS* s; XA* a; XO* o; XK* k; }; };\n"
"\n"
"static void* xm(size_t n) {\n"
"    void* p = malloc(n ? n : 1);\n"
"    if (!p) { fputs(\"fatal: out of memory\\n\", stderr); exit(70); }\n"
"    return p;\n"
"}\n"
"static void* xr(void* p, size_t n) {\n"
"    void* q = realloc(p, n ? n : 1);\n"
"    if (!q) { fputs(\"fatal: out of memory\\n\", stderr); exit(70); }\n"
"    return q;\n"
"}\n"
"\n"
"static XS* xs_new(const char* src, size_t n) {\n"
"    XS* s = (XS*)xm(sizeof(XS) + n + 1);\n"
"    s->rc = 1; s->len = n;\n"
"    if (n) memcpy(s->d, src, n);\n"
"    s->d[n] = 0;\n"
"    return s;\n"
"}\n"
"static XS* xs_retain(XS* s) { if (s) s->rc++; return s; }\n"
"static void xs_rel(XS* s) { if (s && --s->rc <= 0) free(s); }\n"
"static XS* xs_cat(XS* a, XS* b) {\n"
"    size_t n = a->len + b->len;\n"
"    XS* s = (XS*)xm(sizeof(XS) + n + 1);\n"
"    s->rc = 1; s->len = n;\n"
"    memcpy(s->d, a->d, a->len);\n"
"    memcpy(s->d + a->len, b->d, b->len);\n"
"    s->d[n] = 0;\n"
"    return s;\n"
"}\n"
"\n"
"/* ---------- 数组 ---------- */\n"
"static void v_rel(V* v);\n"
"static void v_rtn(V* v);\n"
"\n"
"static XA* xa_new(size_t n, V fill) {\n"
"    size_t cap = n ? n : 1;\n"
"    XA* a = (XA*)xm(sizeof(XA));\n"
"    a->rc = 1; a->len = n; a->cap = cap;\n"
"    a->d = (V*)xm(cap * sizeof(V));\n"
"    for (size_t i = 0; i < n; i++) { a->d[i] = fill; v_rtn(&a->d[i]); }\n"
"    return a;\n"
"}\n"
"/* 保证 cap >= need，1.5 倍增长（与解释器 arr_reserve 完全一致，\n"
"   否则同一个程序的峰值内存会因编译方式不同而不同）。 */\n"
"static void xa_reserve(XA* a, size_t need) {\n"
"    if (!a || a->cap >= need) return;\n"
"    size_t ncap = a->cap ? a->cap : 4;\n"
"    while (ncap < need) { size_t nx = ncap + (ncap >> 1); ncap = (nx <= ncap) ? ncap + 1 : nx; }\n"
"    a->d = (V*)xr(a->d, ncap * sizeof(V));\n"
"    a->cap = ncap;\n"
"}\n"
"static void xa_push(XA* a, V v) { xa_reserve(a, a->len + 1); a->d[a->len++] = v; }\n"
"static XA* xa_retain(XA* a) { if (a) a->rc++; return a; }\n"
"static void xa_rel(XA* a) {\n"
"    if (!a) return;\n"
"    if (--a->rc > 0) return;\n"
"    for (size_t i = 0; i < a->len; i++) v_rel(&a->d[i]);\n"
"    free(a->d);\n"
"    free(a);\n"
"}\n"
"\n"
"static V v_int(int64_t i)   { V v; v.t = XV_INT;   v.i = i; return v; }\n"
"static V v_flt(double f)    { V v; v.t = XV_FLOAT; v.f = f; return v; }\n"
"static V v_bool(bool b)     { V v; v.t = XV_BOOL;  v.b = b; return v; }\n"
"static V v_null(void)       { V v; v.t = XV_NULL;  v.i = 0; return v; }\n"
"static V v_str(XS* s)       { V v; v.t = XV_STRING; v.s = s; return v; }\n"
"static V v_arr(XA* a)       { V v; v.t = XV_ARRAY;  v.a = a; return v; }\n"
"static V v_obj(XO* o)       { V v; v.t = XV_STRUCT; v.o = o; return v; }\n"
"static V v_inst(XK* k)      { V v; v.t = XV_CLASS;  v.k = k; return v; }\n"
"\n"
"/* ---------- 类实例 ---------- */\n"
"/* 与解释器的 XInst 逐字段对应。cls 是**实际**类索引 —— 多态的全部\n"
"   依赖它：同一段代码里的 obj.k->cls 不同，就派发到不同实现。 */\n"
"struct XK {\n"
"    long rc;\n"
"    int  cls;\n"
"    const char* cname;\n"
"    const char** fnames;\n"
"    int  fc;\n"
"    V*   f;\n"
"};\n"
"static XK* xk_new(int cls, const char* cname, const char** fnames, int fc);\n"
"static void xk_rel(XK* k);\n"
"\n"
"/* ---------- 结构体 ---------- */\n"
"static void v_rel(V* v);\n"
"static void v_rtn(V* v);\n"
"\n"
"static XO* xo_new(int ti, const char* tname, const char** fnames, int fc) {\n"
"    XO* o = (XO*)xm(sizeof(XO));\n"
"    o->rc = 1; o->ti = ti; o->tname = tname; o->fnames = fnames; o->fc = fc;\n"
"    o->f = (V*)xm((size_t)(fc ? fc : 1) * sizeof(V));\n"
"    for (int i = 0; i < fc; i++) o->f[i] = v_null();\n"
"    return o;\n"
"}\n"
"static XO* xo_retain(XO* o) { if (o) o->rc++; return o; }\n"
"static void xo_rel(XO* o) {\n"
"    if (!o) return;\n"
"    if (--o->rc > 0) return;\n"
"    for (int i = 0; i < o->fc; i++) v_rel(&o->f[i]);\n"
"    free(o->f);\n"
"    free(o);\n"
"}\n"
"\n"
"/* 类实例的分配与释放。\n"
"   字段初值统一是 int 0 —— 与解释器 inst_new 完全一致。原因见那里\n"
"   的注释：没有构造函数的类，字段就保持零值，而零值比 null 更不容\n"
"   易在后续算术里炸。 */\n"
"static XK* xk_new(int cls, const char* cname, const char** fnames, int fc) {\n"
"    XK* k = (XK*)xm(sizeof(XK));\n"
"    k->rc = 1; k->cls = cls; k->cname = cname; k->fnames = fnames; k->fc = fc;\n"
"    k->f = (V*)xm((size_t)(fc ? fc : 1) * sizeof(V));\n"
"    for (int i = 0; i < fc; i++) { V z; z.t = XV_INT; z.i = 0; k->f[i] = z; }\n"
"    return k;\n"
"}\n"
"static XK* xk_retain(XK* k) { if (k) k->rc++; return k; }\n"
"static void xk_rel(XK* k) {\n"
"    if (!k) return;\n"
"    if (--k->rc > 0) return;\n"
"    for (int i = 0; i < k->fc; i++) v_rel(&k->f[i]);\n"
"    free(k->f);\n"
"    free(k);\n"
"}\n"
"\n"
"static void v_rel(V* v) {\n"
"    if (v->t == XV_STRING) { xs_rel(v->s); v->s = 0; }\n"
"    else if (v->t == XV_ARRAY) { xa_rel(v->a); v->a = 0; }\n"
"    else if (v->t == XV_STRUCT) { xo_rel(v->o); v->o = 0; }\n"
"    else if (v->t == XV_CLASS) { xk_rel(v->k); v->k = 0; }\n"
"    v->t = XV_NULL;\n"
"}\n"
"\n"
"/* 引用型类型的统一 retain。与解释器的 value_retain 一一对应，\n"
"   两档语义必须完全一致，否则 AOT 一致性测试会立刻抓到。 */\n"
"static void v_rtn(V* v) {\n"
"    if (v->t == XV_STRING) { v->s->rc++; }\n"
"    else if (v->t == XV_ARRAY) { v->a->rc++; }\n"
"    else if (v->t == XV_STRUCT) { v->o->rc++; }\n"
"    else if (v->t == XV_CLASS) { v->k->rc++; }\n"
"}\n"
"\n"
"static bool v_true(V v) {\n"
"    switch (v.t) {\n"
"        case XV_NULL:   return false;\n"
"        case XV_BOOL:   return v.b;\n"
"        case XV_INT:    return v.i != 0;\n"
"        case XV_FLOAT:  return v.f != 0.0;\n"
"        case XV_STRING: return v.s && v.s->len > 0;\n"
"        case XV_ARRAY:  return v.a && v.a->len > 0;\n"
"        case XV_STRUCT: return v.o != NULL;\n"
"        case XV_CLASS:  return v.k != NULL;\n"
"    }\n"
"    return false;\n"
"}\n"
"\n"
"static const char* v_tname(VType t) {\n"
"    switch (t) {\n"
"        case XV_NULL: return \"Null\";\n"
"        case XV_INT: return \"Int\";\n"
"        case XV_FLOAT: return \"Float\";\n"
"        case XV_BOOL: return \"Bool\";\n"
"        case XV_STRING: return \"String\";\n"
"        case XV_ARRAY:  return \"Array\";\n"
"        case XV_STRUCT: return \"Struct\";\n"
"        case XV_CLASS:  return \"Class\";\n"
"    }\n"
"    return \"?\";\n"
"}\n"
"\n"
"/* 浮点转字符串：最短可往返。与解释器用同一套算法，\n"
"   否则两档在 println(0.1) 上会给出不同位数。 */\n"
"static XS* xs_flt(double f) {\n"
"    if (f != f) return xs_new(\"NaN\", 3);\n"
"    /* 必须用 isinf 而不是 `f > 1e308 * 10`：后者在编译期就算成\n"
"       inf，于是 `f > inf` 恒假，两个 Infinity 分支是死代码，\n"
"       值会掉进下面的 %g 打印成 \"inf\"，与解释器的 \"Infinity\" 不符。\n"
"       这个 bug 只在 ±Infinity 上显形，普通数值完全看不出来。 */\n"
"    if (isinf(f)) return f > 0 ? xs_new(\"Infinity\", 8) : xs_new(\"-Infinity\", 9);\n"
"    char buf[64];\n"
"    /* 整数值先走 %.1f：%g 在指数 >= 精度 时切科学计数法，\n"
"       350.0 会打成 3.5e+02，而三位的数在数值处理里是常态。\n"
"       与解释器 float_to_str 的同一条规则逐字对应。 */\n"
"    if (f >= -9.007199254740992e15 && f <= 9.007199254740992e15) {\n"
"        int64_t ai = (int64_t)f;\n"
"        if ((double)ai == f) {\n"
"            snprintf(buf, sizeof(buf), \"%.1f\", f);\n"
"            return xs_new(buf, strlen(buf));\n"
"        }\n"
"    }\n"
"    for (int p = 1; p <= 17; p++) {\n"
"        snprintf(buf, sizeof(buf), \"%.*g\", p, f);\n"
"        if (strtod(buf, 0) == f) break;\n"
"    }\n"
"    if (!strpbrk(buf, \".eEni\")) strncat(buf, \".0\", sizeof(buf) - strlen(buf) - 1);\n"
"    return xs_new(buf, strlen(buf));\n"
"}\n"
"\n"
"static XS* xs_int(int64_t i) {\n"
"    char buf[32];\n"
"    int n = snprintf(buf, sizeof(buf), \"%lld\", (long long)i);\n"
"    return xs_new(buf, (size_t)n);\n"
"}\n"
"\n"
"static XS* xa_str(XA* a, int depth);\n"
"static XS* xo_str(XO* o, int depth);\n"
"static XS* xk_str(XK* k, int depth);\n"
"static XS* v_str_of(V v) {\n"
"    switch (v.t) {\n"
"        case XV_NULL:   return xs_new(\"null\", 4);\n"
"        case XV_INT:    return xs_int(v.i);\n"
"        case XV_FLOAT:  return xs_flt(v.f);\n"
"        case XV_BOOL:   return v.b ? xs_new(\"true\", 4) : xs_new(\"false\", 5);\n"
"        case XV_STRING: return xs_retain(v.s);\n"
"        case XV_ARRAY:  return xa_str(v.a, 0);\n"
"        case XV_STRUCT: return xo_str(v.o, 0);\n"
"        case XV_CLASS:  return xk_str(v.k, 0);\n"
"    }\n"
"    return xs_new(\"?\", 1);\n"
"}\n"
"\n"
"/* 结构体打印成 `Rect { x: 1, y: 2 }`。格式与解释器 obj_to_str\n"
"   逐字符一致 —— 否则 println(struct) 在两档下输出不同。 */\n"
"static XS* xk_str(XK* k, int depth);\n"
"static XS* xo_str(XO* o, int depth) {\n"
"    if (!o) return xs_new(\"null\", 4);\n"
"    if (depth > 8) return xs_new(\"{...}\", 5);\n"
"    XS* out = xs_new(\"\", 0);\n"
"    { const char* nm = o->tname ? o->tname : \"?\";\n"
"      XS* t = xs_cat(out, xs_new(nm, strlen(nm))); xs_rel(out); out = t; }\n"
"    { XS* t = xs_cat(out, xs_new(\" { \", 3)); xs_rel(out); out = t; }\n"
"    for (int i = 0; i < o->fc; i++) {\n"
"        if (i > 0) { XS* t = xs_cat(out, xs_new(\", \", 2)); xs_rel(out); out = t; }\n"
"        const char* fn = (o->fnames && o->fnames[i]) ? o->fnames[i] : \"?\";\n"
"        { XS* t = xs_cat(out, xs_new(fn, strlen(fn))); xs_rel(out); out = t; }\n"
"        { XS* t = xs_cat(out, xs_new(\": \", 2)); xs_rel(out); out = t; }\n"
"        XS* part = (o->f[i].t == XV_STRUCT) ? xo_str(o->f[i].o, depth + 1)\n"
"                 : (o->f[i].t == XV_CLASS)  ? xk_str(o->f[i].k, depth + 1)\n"
"                                            : v_str_of(o->f[i]);\n"
"        { XS* t = xs_cat(out, part); xs_rel(out); xs_rel(part); out = t; }\n"
"    }\n"
"    { XS* t = xs_cat(out, xs_new(\" }\", 2)); xs_rel(out); out = t; }\n"
"    return out;\n"
"}\n"
"\n"
"/* 类实例打印成 `Dog { name: Rex, legs: 4 }`。\n"
"   用 cname（**实际**类名）而不是静态类型名 —— 这样多态在打印里\n"
"   也能看见：`let a: Animal = new Dog(...)` 打出来是 Dog。 */\n"
"static XS* xk_str(XK* k, int depth) {\n"
"    if (!k) return xs_new(\"null\", 4);\n"
"    if (depth > 8) return xs_new(\"{...}\", 5);\n"
"    XS* out = xs_new(\"\", 0);\n"
"    { const char* nm = k->cname ? k->cname : \"?\";\n"
"      XS* t = xs_cat(out, xs_new(nm, strlen(nm))); xs_rel(out); out = t; }\n"
"    { XS* t = xs_cat(out, xs_new(\" { \", 3)); xs_rel(out); out = t; }\n"
"    for (int i = 0; i < k->fc; i++) {\n"
"        if (i > 0) { XS* t = xs_cat(out, xs_new(\", \", 2)); xs_rel(out); out = t; }\n"
"        const char* fn = (k->fnames && k->fnames[i]) ? k->fnames[i] : \"?\";\n"
"        { XS* t = xs_cat(out, xs_new(fn, strlen(fn))); xs_rel(out); out = t; }\n"
"        { XS* t = xs_cat(out, xs_new(\": \", 2)); xs_rel(out); out = t; }\n"
"        XS* part = (k->f[i].t == XV_STRUCT) ? xo_str(k->f[i].o, depth + 1)\n"
"                 : (k->f[i].t == XV_CLASS)  ? xk_str(k->f[i].k, depth + 1)\n"
"                                            : v_str_of(k->f[i]);\n"
"        { XS* t = xs_cat(out, part); xs_rel(out); xs_rel(part); out = t; }\n"
"    }\n"
"    { XS* t = xs_cat(out, xs_new(\" }\", 2)); xs_rel(out); out = t; }\n"
"    return out;\n"
"}\n"
"\n"
"/* 数组打印成 [1, 2, 3]。深度上限 8，与解释器完全一致。 */\n"
"static XS* xa_str(XA* a, int depth) {\n"
"    if (!a) return xs_new(\"null\", 4);\n"
"    if (depth > 8) return xs_new(\"[...]\", 5);\n"
"    XS* out = xs_new(\"[\", 1);\n"
"    for (size_t i = 0; i < a->len; i++) {\n"
"        if (i > 0) { XS* sep = xs_new(\", \", 2); XS* t = xs_cat(out, sep); xs_rel(out); xs_rel(sep); out = t; }\n"
"        XS* part = (a->d[i].t == XV_ARRAY) ? xa_str(a->d[i].a, depth + 1)\n"
"                 : (a->d[i].t == XV_STRUCT) ? xo_str(a->d[i].o, depth + 1)\n"
"                 : (a->d[i].t == XV_CLASS)  ? xk_str(a->d[i].k, depth + 1)\n"
"                 : v_str_of(a->d[i]);\n"
"        XS* t = xs_cat(out, part); xs_rel(out); xs_rel(part); out = t;\n"
"    }\n"
"    XS* cl = xs_new(\"]\", 1);\n"
"    XS* r = xs_cat(out, cl); xs_rel(out); xs_rel(cl);\n"
"    return r;\n"
"}\n"
"\n"
"static bool v_as_flt(V* v, double* out) {\n"
"    if (v->t == XV_INT)   { *out = (double)v->i; return true; }\n"
"    if (v->t == XV_FLOAT) { *out = v->f; return true; }\n"
"    return false;\n"
"}\n"
"\n"
"/* ---------- 溢出检查 ---------- */\n"
"static bool ovf_add(int64_t a, int64_t b, int64_t* r) { return __builtin_add_overflow(a, b, r); }\n"
"static bool ovf_sub(int64_t a, int64_t b, int64_t* r) { return __builtin_sub_overflow(a, b, r); }\n"
"static bool ovf_mul(int64_t a, int64_t b, int64_t* r) { return __builtin_mul_overflow(a, b, r); }\n"
"\n"
"/* ---------- 诊断 ---------- */\n"
"/* 错误信息里带源码位置，格式与解释档完全一致，\n"
"   这样同一个程序在两档下报的错是同一份文本。\n"
"   注意 g_file 必须在 rt_err 定义之前声明——它被 rt_err 引用，\n"
"   而 C 要求先声明后使用。真正的文件名在 main 里赋值。 */\n"
"static const char* g_file = \"?\";\n"
"static int g_line = 0, g_col = 0;\n"
"\n"
"static void rt_err(const char* msg) {\n"
"    fflush(stdout);\n"
"    fprintf(stderr, \"\\n  %s:%d:%d\\n\", g_file, g_line, g_col);\n"
"    fprintf(stderr, \"  error: %s\\n\", msg);\n"
"    exit(70);\n"
"}\n"
"\n"
"static void ovf_err(const char* op) {\n"
"    fflush(stdout);\n"
"    fprintf(stderr, \"\\n  %s:%d:%d\\n\", g_file, g_line, g_col);\n"
"    fprintf(stderr, \"  error: 整数溢出：`%s` 的结果超出 Int 的表示范围\\n\", op);\n"
"    fprintf(stderr, \"  note: Int 是 64 位有符号整数（01 \\302\2475.4）\\n\");\n"
"    fprintf(stderr, \"  help: 改用 Float，或用 `+%%` 显式要求回绕\\n\");\n"
"    exit(70);\n"
"}\n"
"\n"
"/* ============================================================\n"
"   UTF-8 码点工具\n"
"   字符串内建全部按**码点**索引，不是字节。按字节切会让中文\n"
"   用户拿到「半个人」，而字节层错误在打印时表现为乱码，很难\n"
"   归因到下标。与解释器的 utf8_decode 行为必须一致。\n"
"   ============================================================ */\n"
"static int xu_decode(const char* p, size_t n, int* adv) {\n"
"    if (n == 0) { *adv = 0; return -1; }\n"
"    unsigned char c = (unsigned char)p[0];\n"
"    if (c < 0x80)           { *adv = 1; return c; }\n"
"    if ((c & 0xE0) == 0xC0) { *adv = 2; return ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F); }\n"
"    if ((c & 0xF0) == 0xE0) { *adv = 3; return ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F); }\n"
"    if ((c & 0xF8) == 0xF0) { *adv = 4; return ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) | (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F); }\n"
"    *adv = 1; return c;   /* 非法首字节按单字节吞掉 */\n"
"}\n"
"/* 码点个数：只数首字节（非 10xxxxxx）。 */\n"
"static size_t xu_cplen(const char* p, size_t n) {\n"
"    size_t k = 0;\n"
"    for (size_t i = 0; i < n; i++) if (((unsigned char)p[i] & 0xC0) != 0x80) k++;\n"
"    return k;\n"
"}\n"
"/* 第 idx 个码点的字节偏移，越界返回 n。 */\n"
"static size_t xu_off(const char* p, size_t n, size_t idx) {\n"
"    size_t k = 0;\n"
"    for (size_t i = 0; i < n; i++) {\n"
"        if (((unsigned char)p[i] & 0xC0) == 0x80) continue;\n"
"        if (k == idx) return i;\n"
"        k++;\n"
"    }\n"
"    return n;\n"
"}\n"
"\n"
"/* ============================================================\n"
"   值比较（arrIndexOf / arrContains / arrSort 共用）\n"
"   必须与解释器的 value_equal / value_cmp 逐条一致，否则同一\n"
"   程序在两档下 arrIndexOf 会给出不同答案。\n"
"   ============================================================ */\n"
"static bool v_eq(V* x, V* y) {\n"
"    if (x->t == XV_INT && y->t == XV_FLOAT) return (double)x->i == y->f;\n"
"    if (x->t == XV_FLOAT && y->t == XV_INT) return x->f == (double)y->i;\n"
"    if (x->t != y->t) return false;\n"
"    switch (x->t) {\n"
"        case XV_NULL:  return true;\n"
"        case XV_INT:   return x->i == y->i;\n"
"        case XV_FLOAT: return x->f == y->f;\n"
"        case XV_BOOL:  return x->b == y->b;\n"
"        case XV_STRING:\n"
"            return x->s->len == y->s->len && memcmp(x->s->d, y->s->d, x->s->len) == 0;\n"
"        case XV_ARRAY: {\n"
"            if (x->a->len != y->a->len) return false;\n"
"            for (size_t i = 0; i < x->a->len; i++)\n"
"                if (!v_eq(&x->a->d[i], &y->a->d[i])) return false;\n"
"            return true;\n"
"        }\n"
"        /* 结构体按**结构**比较，类型不同直接不等。\n"
"           与解释器 value_equal 的 VT_STRUCT 分支逐条对应 ——\n"
"           两档若在 `a == b` 上给出不同答案，差分测试会立刻抓到，\n"
"           但那时已经很难判断是哪一边对了。 */\n"
"        case XV_STRUCT: {\n"
"            if (x->o->ti != y->o->ti) return false;\n"
"            if (x->o->fc != y->o->fc) return false;\n"
"            for (int i = 0; i < x->o->fc; i++)\n"
"                if (!v_eq(&x->o->f[i], &y->o->f[i])) return false;\n"
"            return true;\n"
"        }\n"
"        /* 类实例是**身份**比较：同一个对象才相等。\n"
"           与解释器 value_equal 的 VT_CLASS 分支一致。\n"
"           \n"
"           按字段比是错的：两个字段完全相同的按钮是两个不同的按钮，\n"
"           而拖动判定、选中集合这类逻辑一旦按字段比就会静默误判。 */\n"
"        case XV_CLASS:  return x->k == y->k;\n"
"    }\n"
"    return false;\n"
"}\n"
"/* 跨类型全序：null < Bool < 数值 < String < Array。\n"
"   混合类型也能排，因为 JSON 数组天然是异构的。 */\n"
"static int v_rank(VType t) {\n"
"    switch (t) {\n"
"        case XV_NULL: return 0;\n"
"        case XV_BOOL: return 1;\n"
"        case XV_INT: case XV_FLOAT: return 2;\n"
"        case XV_STRING: return 3;\n"
"        case XV_ARRAY: return 4;\n"
"        case XV_STRUCT: return 5;\n"
"        case XV_CLASS:  return 6;\n"
"    }\n"
"    return 7;\n"
"}\n"
"static int v_cmp(V* x, V* y) {\n"
"    int rx = v_rank(x->t), ry = v_rank(y->t);\n"
"    if (rx != ry) return rx < ry ? -1 : 1;\n"
"    switch (x->t) {\n"
"        case XV_NULL: return 0;\n"
"        case XV_BOOL: return (int)x->b - (int)y->b;\n"
"        case XV_INT:\n"
"            if (y->t == XV_FLOAT) { double a = (double)x->i; return a < y->f ? -1 : (a > y->f ? 1 : 0); }\n"
"            return x->i < y->i ? -1 : (x->i > y->i ? 1 : 0);\n"
"        case XV_FLOAT:\n"
"            if (y->t == XV_INT) { double b = (double)y->i; return x->f < b ? -1 : (x->f > b ? 1 : 0); }\n"
"            return x->f < y->f ? -1 : (x->f > y->f ? 1 : 0);\n"
"        case XV_STRING: {\n"
"            size_t n = x->s->len < y->s->len ? x->s->len : y->s->len;\n"
"            int c = memcmp(x->s->d, y->s->d, n);\n"
"            if (c) return c < 0 ? -1 : 1;\n"
"            return x->s->len < y->s->len ? -1 : (x->s->len > y->s->len ? 1 : 0);\n"
"        }\n"
"        case XV_ARRAY: return x->a->len < y->a->len ? -1 : (x->a->len > y->a->len ? 1 : 0);\n"
"        /* 结构体：先按类型名，再按字段。与解释器 value_cmp 的\n"
"           VT_STRUCT 分支一致 —— 按名字而不是 ti，因为 ti 取决于\n"
"           声明顺序，会让排序结果依赖 import 排列。 */\n"
"        case XV_STRUCT: {\n"
"            const char* xn = x->o->tname ? x->o->tname : \"\";\n"
"            const char* yn = y->o->tname ? y->o->tname : \"\";\n"
"            int c = strcmp(xn, yn);\n"
"            if (c) return c < 0 ? -1 : 1;\n"
"            int n = x->o->fc < y->o->fc ? x->o->fc : y->o->fc;\n"
"            for (int i = 0; i < n; i++) {\n"
"                c = v_cmp(&x->o->f[i], &y->o->f[i]);\n"
"                if (c) return c;\n"
"            }\n"
"            return x->o->fc < y->o->fc ? -1 : (x->o->fc > y->o->fc ? 1 : 0);\n"
"        }\n"
"        /* 类实例：先按类名，再按地址。\n"
"           不比字段 —— 字段可变，拿它们做排序键会让 qsort 的\n"
"           比较函数不满足传递性，结果是随机错乱而非稳定排序。\n"
"           按地址保底给一个全序，至少在同一个数组里是稳定的。 */\n"
"        case XV_CLASS: {\n"
"            const char* xn = x->k->cname ? x->k->cname : \"\";\n"
"            const char* yn = y->k->cname ? y->k->cname : \"\";\n"
"            int c = strcmp(xn, yn);\n"
"            if (c) return c < 0 ? -1 : 1;\n"
"            if (x->k == y->k) return 0;\n"
"            return (uintptr_t)x->k < (uintptr_t)y->k ? -1 : 1;\n"
"        }\n"
"    }\n"
"    return 0;\n"
"}\n"
"static int v_cmp_q(const void* pa, const void* pb) { return v_cmp((V*)pa, (V*)pb); }\n"
"\n"
"/* ---------- 参数取出与校验 ----------\n"
"   每个辅助直接对 s[] 槽位操作，返回 V。前缀 xb_ 与解释器的\n"
"   XBI_* 一一对应，便于逐条对照。 */\n"
"static int64_t xb_int(V* v, const char* fn, int which) {\n"
"    if (v->t != XV_INT) {\n"
"        static char buf[256];\n"
"        snprintf(buf, sizeof(buf),\n"
"                 \"`%s` 的第 %d 个参数必须是 Int，实际是 %s\\n\"\n"
"                 \"  note: 这些函数的签名固定，参数类型不会自动转换\\n\"\n"
"                 \"  help: 用 `toInt(x)` 显式转换\", fn, which, v_tname(v->t));\n"
"        rt_err(buf);\n"
"    }\n"
"    return v->i;\n"
"}\n"
"static double xb_num(V* v, const char* fn, int which) {\n"
"    if (v->t == XV_INT)   return (double)v->i;\n"
"    if (v->t == XV_FLOAT) return v->f;\n"
"    {\n"
"        static char buf[256];\n"
"        snprintf(buf, sizeof(buf),\n"
"                 \"`%s` 的第 %d 个参数必须是数值，实际是 %s\\n\"\n"
"                 \"  help: 用 toFloat(x) 或 strParseFloat(s) 先转成数值\", fn, which, v_tname(v->t));\n"
"        rt_err(buf);\n"
"    }\n"
"    return 0.0;\n"
"}\n"
"static XS* xb_str(V* v, const char* fn, int which) {\n"
"    if (v->t != XV_STRING) {\n"
"        static char buf[256];\n"
"        snprintf(buf, sizeof(buf), \"`%s` 的第 %d 个参数必须是 String，实际是 %s\", fn, which, v_tname(v->t));\n"
"        rt_err(buf);\n"
"    }\n"
"    return v->s;\n"
"}\n"
"static XA* xb_arr(V* v, const char* fn, int which) {\n"
"    if (v->t != XV_ARRAY) {\n"
"        static char buf[256];\n"
"        snprintf(buf, sizeof(buf), \"`%s` 的第 %d 个参数必须是 Array，实际是 %s\", fn, which, v_tname(v->t));\n"
"        rt_err(buf);\n"
"    }\n"
"    return v->a;\n"
"}\n"
"\n"
"/* ---------- 数学（25 个子操作） ----------\n"
"   op 从指令的 c 操作数低 8 位解出，与解释器 bi_math 同构。\n"
"   all_int 用于 min/max/clamp/abs 保持输入类型。 */\n"
"static V xb_math(V* args, int argc, int op) {\n"
"    double a[4] = {0,0,0,0};\n"
"    bool all_int = true;\n"
"    for (int i = 0; i < argc; i++) {\n"
"        a[i] = xb_num(&args[i], \"math\", i + 1);\n"
"        if (args[i].t != XV_INT) all_int = false;\n"
"    }\n"
"    switch (op) {\n"
"        case 1:  return v_flt(pow(a[0], a[1]));\n"
"        case 2:  return v_flt(sqrt(a[0]));\n"
"        case 3:  return v_flt(cbrt(a[0]));\n"
"        case 4:  return v_int((int64_t)floor(a[0]));\n"
"        case 5:  return v_int((int64_t)ceil(a[0]));\n"
"        case 6:  return v_int((int64_t)llround(a[0]));\n"
"        case 7:  return v_int((int64_t)trunc(a[0]));\n"
"        case 8:  { double r = fabs(a[0]); return all_int ? v_int((int64_t)r) : v_flt(r); }\n"
"        case 9:  return v_int(a[0] > 0 ? 1 : (a[0] < 0 ? -1 : 0));\n"
"        case 10: { double r = a[0] < a[1] ? a[0] : a[1]; return all_int ? v_int((int64_t)r) : v_flt(r); }\n"
"        case 11: { double r = a[0] > a[1] ? a[0] : a[1]; return all_int ? v_int((int64_t)r) : v_flt(r); }\n"
"        case 12: return v_flt(sin(a[0]));\n"
"        case 13: return v_flt(cos(a[0]));\n"
"        case 14: return v_flt(tan(a[0]));\n"
"        case 15: return v_flt(asin(a[0]));\n"
"        case 16: return v_flt(acos(a[0]));\n"
"        case 17: return v_flt(atan(a[0]));\n"
"        case 18: return v_flt(atan2(a[0], a[1]));\n"
"        case 19: return v_flt(log(a[0]));\n"
"        case 20: return v_flt(log2(a[0]));\n"
"        case 21: return v_flt(log10(a[0]));\n"
"        case 22: return v_flt(exp(a[0]));\n"
"        case 23: return v_flt(hypot(a[0], a[1]));\n"
"        case 24: return v_flt(fmod(a[0], a[1]));\n"
"        case 25: {\n"
"            double x = a[0], lo = a[1], hi = a[2];\n"
"            if (hi < lo) { double t = lo; lo = hi; hi = t; }\n"
"            double r = x < lo ? lo : (x > hi ? hi : x);\n"
"            return all_int ? v_int((int64_t)r) : v_flt(r);\n"
"        }\n"
"    }\n"
"    rt_err(\"内部错误：未知的数学子操作\");\n"
"    return v_null();\n"
"}\n"
"\n"
"/* ---------- 伪随机（xorshift128+，与解释器同一算法） ----------\n"
"   必须同算法同种子序列，否则 randomSeed(1) 之后两档给出的数字\n"
"   不同，而「AOT 与解释一致」是语言承诺。注意这**不是**跨平台\n"
"   一致的保证，只是两档产物内部一致。 */\n"
"static uint64_t g_rng[2] = { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };\n"
"static bool g_rng_seeded = false;\n"
"static void xb_seed(uint64_t seed) {\n"
"    uint64_t z = seed + 0x9E3779B97F4A7C15ULL;\n"
"    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;\n"
"    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;\n"
"    g_rng[0] = z ^ (z >> 31);\n"
"    z = seed + 0x9E3779B97F4A7C15ULL * 2;\n"
"    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;\n"
"    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;\n"
"    g_rng[1] = z ^ (z >> 31);\n"
"    if (g_rng[0] == 0 && g_rng[1] == 0) g_rng[0] = 1;\n"
"    g_rng_seeded = true;\n"
"}\n"
"static double xb_rand(void) {\n"
"    if (!g_rng_seeded) xb_seed((uint64_t)time(NULL));\n"
"    uint64_t x = g_rng[0];\n"
"    const uint64_t y = g_rng[1];\n"
"    g_rng[0] = y;\n"
"    x ^= x << 23;\n"
"    g_rng[1] = x ^ y ^ (x >> 17) ^ (y >> 26);\n"
"    return (double)((g_rng[1] + y) >> 11) * (1.0 / 9007199254740992.0);\n"
"}\n"
"\n"
"/* ---------- 字符串 ---------- */\n"
"static XS* xb_str_at(XS* s, int64_t ix) {\n"
"    if (ix < 0 || (size_t)ix >= xu_cplen(s->d, s->len)) return xs_new(\"\", 0);\n"
"    size_t off = xu_off(s->d, s->len, (size_t)ix);\n"
"    int adv = 0;\n"
"    xu_decode(s->d + off, s->len - off, &adv);\n"
"    return xs_new(s->d + off, (size_t)adv);\n"
"}\n"
"static XS* xb_str_sub(XS* s, int64_t start, int64_t end) {\n"
"    size_t nc = xu_cplen(s->d, s->len);\n"
"    if (start < 0) start = 0;\n"
"    if (end < 0) end = 0;\n"
"    if ((size_t)start > nc) start = (int64_t)nc;\n"
"    if ((size_t)end > nc) end = (int64_t)nc;\n"
"    if (end < start) end = start;\n"
"    size_t b = xu_off(s->d, s->len, (size_t)start);\n"
"    size_t e = xu_off(s->d, s->len, (size_t)end);\n"
"    return xs_new(s->d + b, e - b);\n"
"}\n"
"/* 返回码点下标；找不到返回 -1。from 也是码点下标。 */\n"
"static int64_t xb_str_find(XS* s, XS* sub, int64_t from) {\n"
"    size_t nc = xu_cplen(s->d, s->len);\n"
"    if (from < 0) from = 0;\n"
"    if ((size_t)from > nc) return -1;\n"
"    if (sub->len == 0) return from <= (int64_t)nc ? from : -1;\n"
"    size_t boff = xu_off(s->d, s->len, (size_t)from);\n"
"    for (size_t i = boff; i + sub->len <= s->len; ) {\n"
"        if (memcmp(s->d + i, sub->d, sub->len) == 0)\n"
"            return (int64_t)xu_cplen(s->d, i);\n"
"        /* 前进一个码点，不能按字节：按字节会匹配到半个字符内部 */\n"
"        int adv = 0;\n"
"        xu_decode(s->d + i, s->len - i, &adv);\n"
"        i += (size_t)(adv > 0 ? adv : 1);\n"
"    }\n"
"    return -1;\n"
"}\n"
"static XA* xb_str_split(XS* s, XS* sep) {\n"
"    XA* out = xa_new(0, v_null());\n"
"    /* 空分隔符按**码点**逐字切开（与解释器 bi_str_split 一致）。\n"
"       另一种选择是「整串当一个元素返回」，但那让 strSplit(s,\"\") 与\n"
"       strSplit(s,\",\") 的语义毫无关联，用户无法预期。按码点切是\n"
"       Python / JS / Ruby 的共同行为，也是唯一能解释得通的。 */\n"
"    if (sep->len == 0) {\n"
"        size_t pos = 0;\n"
"        while (pos < s->len) {\n"
"            int adv = 0;\n"
"            xu_decode(s->d + pos, s->len - pos, &adv);\n"
"            if (adv <= 0) adv = 1;\n"
"            xa_push(out, v_str(xs_new(s->d + pos, (size_t)adv)));\n"
"            pos += (size_t)adv;\n"
"        }\n"
"        return out;\n"
"    }\n"
"    size_t i = 0, last = 0;\n"
"    while (i + sep->len <= s->len) {\n"
"        if (memcmp(s->d + i, sep->d, sep->len) == 0) {\n"
"            xa_push(out, v_str(xs_new(s->d + last, i - last)));\n"
"            i += sep->len;\n"
"            last = i;\n"
"        } else i++;\n"
"    }\n"
"    xa_push(out, v_str(xs_new(s->d + last, s->len - last)));\n"
"    return out;\n"
"}\n"
"static XS* xb_str_join(XA* a, XS* sep) {\n"
"    XS* out = xs_new(\"\", 0);\n"
"    for (size_t i = 0; i < a->len; i++) {\n"
"        if (i > 0) { XS* t = xs_cat(out, sep); xs_rel(out); out = t; }\n"
"        XS* part = v_str_of(a->d[i]);\n"
"        XS* t = xs_cat(out, part); xs_rel(out); xs_rel(part); out = t;\n"
"    }\n"
"    return out;\n"
"}\n"
"static XS* xb_str_case(XS* s, bool up) {\n"
"    XS* out = xs_new(s->d, s->len);\n"
"    for (size_t i = 0; i < out->len; i++) {\n"
"        unsigned char c = (unsigned char)out->d[i];\n"
"        if (up) { if (c >= 'a' && c <= 'z') out->d[i] = (char)(c - 32); }\n"
"        else    { if (c >= 'A' && c <= 'Z') out->d[i] = (char)(c + 32); }\n"
"    }\n"
"    return out;\n"
"}\n"
"static XS* xb_str_trim(XS* s) {\n"
"    size_t b = 0, e = s->len;\n"
"    while (b < e) {\n"
"        char c = s->d[b];\n"
"        if (c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\f' || c == '\\v') b++; else break;\n"
"    }\n"
"    while (e > b) {\n"
"        char c = s->d[e - 1];\n"
"        if (c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\f' || c == '\\v') e--; else break;\n"
"    }\n"
"    return xs_new(s->d + b, e - b);\n"
"}\n"
"static XS* xb_str_replace(XS* s, XS* from, XS* to) {\n"
"    if (from->len == 0) return xs_new(s->d, s->len);\n"
"    size_t count = 0;\n"
"    for (size_t i = 0; i + from->len <= s->len; ) {\n"
"        if (memcmp(s->d + i, from->d, from->len) == 0) { count++; i += from->len; } else i++;\n"
"    }\n"
"    if (count == 0) return xs_new(s->d, s->len);\n"
"    /* 精确算总长。写成 s->len + count*(to->len - from->len) 是错的：\n"
"       两者都是 size_t，to 更短时减法回绕成天文数字。 */\n"
"    size_t total = (to->len >= from->len)\n"
"        ? s->len + count * (to->len - from->len)\n"
"        : s->len - count * (from->len - to->len);\n"
"    XS* out = (XS*)xm(sizeof(XS) + total + 1);\n"
"    out->rc = 1; out->len = 0;\n"
"    size_t w = 0;\n"
"    for (size_t i = 0; i < s->len; ) {\n"
"        if (i + from->len <= s->len && memcmp(s->d + i, from->d, from->len) == 0) {\n"
"            memcpy(out->d + w, to->d, to->len); w += to->len; i += from->len;\n"
"        } else out->d[w++] = s->d[i++];\n"
"    }\n"
"    out->d[w] = 0; out->len = w;\n"
"    return out;\n"
"}\n"
"static XS* xb_str_repeat(XS* s, int64_t n) {\n"
"    if (n < 0) n = 0;\n"
"    const int64_t MAXB = 64 * 1024 * 1024;\n"
"    int64_t total = (int64_t)s->len * n;\n"
"    if (total > MAXB) rt_err(\"`strRepeat` 的结果过大（单次上限 64 MiB）\");\n"
"    XS* out = (XS*)xm(sizeof(XS) + (size_t)total + 1);\n"
"    out->rc = 1; out->len = (size_t)total;\n"
"    size_t w = 0;\n"
"    for (int64_t i = 0; i < n; i++) { memcpy(out->d + w, s->d, s->len); w += s->len; }\n"
"    out->d[total] = 0;\n"
"    return out;\n"
"}\n"
"/* 解析整数：允许前导空白与符号，遇非数字停止，无数字返回 0。 */\n"
"static int64_t xb_parse_int(XS* s) {\n"
"    size_t i = 0;\n"
"    while (i < s->len && (s->d[i] == ' ' || s->d[i] == '\\t')) i++;\n"
"    bool neg = false;\n"
"    if (i < s->len && (s->d[i] == '+' || s->d[i] == '-')) { neg = (s->d[i] == '-'); i++; }\n"
"    int64_t v = 0; bool any = false;\n"
"    while (i < s->len && s->d[i] >= '0' && s->d[i] <= '9') { v = v * 10 + (s->d[i] - '0'); i++; any = true; }\n"
"    if (!any) return 0;\n"
"    return neg ? -v : v;\n"
"}\n"
"static double xb_parse_flt(XS* s) {\n"
"    char* tmp = (char*)xm(s->len + 1);\n"
"    memcpy(tmp, s->d, s->len); tmp[s->len] = 0;\n"
"    double v = strtod(tmp, 0);\n"
"    free(tmp);\n"
"    return v;\n"
"}\n"
"\n"
/* ============================================================
   文件与 TCP 原语（AOT 侧）
   ============================================================

   ── 这里为什么是「再实现一遍」而不是调用 platform.c ──────────

   因为 AOT 产出的 exe 必须能拷到别的机器上单独运行。把
   platform.c 链进去会让产物依赖源码树，而「单个自包含可执行
   文件」是本项目对 --emit=exe 的明确承诺。

   所以这批原语在生成的 C 里自己实现一次。代价是两处代码要一起
   改 —— 这是本轮最容易出错的地方（漏了 AOT 侧的症状是「解释器
   正常、--emit=exe 报 undefined reference」）。

   ── 平台差异怎么处理 ────────────────────────────────────────

   生成的 C 里直接写 #ifdef _WIN32。这与「除 platform.c 外不许
   出现平台宏」的铁律冲突 —— 但那条铁律管的是**我们的源码树**，
   而这里是生成给 gcc 的产物代码，它独立于我们的构建体系。
   生成代码里不做分支的代价是要么放弃 Windows、要么放弃 Linux，
   两者都不可接受。
*/
"#ifdef _WIN32\n"
"#  include <winsock2.h>\n"
"#  include <ws2tcpip.h>\n"
"#  include <windows.h>\n"
"#  include <direct.h>\n"
"#  include <sys/stat.h>\n"
"#  define XU_FOPEN(p, m) _wfopen(xu_wide(p), L##m)\n"
"#  define XU_WREMOVE(p) _wremove(xu_wide(p))\n"
"#  define XU_WACCESS(p) _waccess(xu_wide(p), 0)\n"
"#  define XU_WMKDIR(p) _wmkdir(xu_wide(p))\n"
"#  define XU_WSTAT(p, st) _wstat64(xu_wide(p), st)\n"
"  /* _wstat64 要的是 struct _stat64，而 struct stat 在 MinGW 上是\n"
"     另一个（32 位）结构。类型不匹配会被当成指针转换错误，而\n"
"     轻率地强制转换会让 st_size 读到垃圾。所以用 typedef 把它\n"
"     统一成一个名字。 */\n"
"  typedef struct _stat64 xu_stat_t;\n"
"static wchar_t* xu_wide(const char* s);\n"
"static FILE* xu_fopen(const char* p, int app) {\n"
"    /* 不能写成宏 XU_FOPEN(p, \"ab\") 展开成 L##\"ab\"：预处理器在\n"
"       替换之前就解析字符串字面量，L##\"ab\" 会被当成 'L' 加 '##'\n"
"       再加字符串，结果是语法错误 'Lapp' undeclared。用函数避开\n"
"       整个问题，也顺便让「追加/不追加」这个语义在调用点更清楚。 */\n"
"    return _wfopen(xu_wide(p), app ? L\"ab\" : L\"wb\");\n"
"}\n"
"static wchar_t* xu_wide(const char* s) {\n"
"    /* 静态轮转缓冲：调用点都是「一行内用完」，让它管内存只会泄漏。 */\n"
"    static wchar_t buf[4][1024];\n"
"    static int slot = 0;\n"
"    wchar_t* o = buf[slot];\n"
"    slot = (slot + 1) & 3;\n"
"    o[0] = 0;\n"
"    if (s) MultiByteToWideChar(CP_UTF8, 0, s, -1, o, 1023);\n"
"    return o;\n"
"}\n"
"typedef SOCKET xu_sock;\n"
"#  define XU_SOCK_INVALID INVALID_SOCKET\n"
"#  define XU_CLOSE closesocket\n"
"#else\n"
"#  include <unistd.h>\n"
"#  include <sys/stat.h>\n"
"#  include <sys/socket.h>\n"
"#  include <netinet/in.h>\n"
"#  include <netinet/tcp.h>\n"
"#  include <arpa/inet.h>\n"
"#  include <netdb.h>\n"
"#  include <fcntl.h>\n"
"#  include <errno.h>\n"
"#  define XU_FOPEN(p, m) fopen(p, m)\n"
"#  define XU_WREMOVE(p) remove(p)\n"
"#  define XU_WACCESS(p) access(p, 0)\n"
"#  define XU_WMKDIR(p) mkdir(p, 0755)\n"
"#  define XU_WSTAT(p, st) stat(p, st)\n"
"  typedef struct stat xu_stat_t;\n"
"static FILE* xu_fopen(const char* p, int app) {\n"
"    return fopen(p, app ? \"ab\" : \"wb\");\n"
"}\n"
"typedef int xu_sock;\n"
"#  define XU_SOCK_INVALID (-1)\n"
"#  define XU_CLOSE close\n"
"#endif\n"
"\n"
"static char xu_err[256] = \"\";\n"
"const char* xu_net_error(void) { return xu_err; }\n"
"\n"
/* ---- 通用 ---- */
"static void xu_sleep(int ms) {\n"
"    if (ms <= 0) return;\n"
"#ifdef _WIN32\n"
"    Sleep((DWORD)ms);\n"
"#else\n"
"    struct timespec ts;\n"
"    ts.tv_sec = ms / 1000;\n"
"    ts.tv_nsec = (long)(ms % 1000) * 1000000L;\n"
"    /* 被信号打断要重试剩余时间 —— 只调一次会让「睡 50ms」变成\n"
"       实际睡 2ms，事件循环的限速因此失效。 */\n"
"    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }\n"
"#endif\n"
"}\n"
"\n"
/* ---- 墙上时钟 / 单调时钟 ---- */
/* 两者不能混用：墙上时钟会被 NTP 校正（差值可能为负），单调时钟
   的绝对值没有跨进程意义。时间戳用前者，量耗时用后者。 */
"static int64_t xu_now_ms(void) {\n"
"#ifdef _WIN32\n"
"    FILETIME ft;\n"
"    GetSystemTimeAsFileTime(&ft);\n"
"    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;\n"
"    return (int64_t)(t / 10000ULL) - 11644473600000LL;\n"
"#else\n"
"    struct timespec ts;\n"
"    clock_gettime(CLOCK_REALTIME, &ts);\n"
"    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;\n"
"#endif\n"
"}\n"
"static int64_t xu_mono_ms(void) {\n"
"#ifdef _WIN32\n"
"    static LARGE_INTEGER f = {0};\n"
"    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);\n"
"    LARGE_INTEGER c;\n"
"    QueryPerformanceCounter(&c);\n"
"    return (int64_t)(c.QuadPart * 1000 / f.QuadPart);\n"
"#else\n"
"    struct timespec ts;\n"
"    clock_gettime(CLOCK_MONOTONIC, &ts);\n"
"    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;\n"
"#endif\n"
"}\n"
"\n"
/* ---- 文件 ---- */
"static bool xu_isdir(const char* p) {\n"
"#ifdef _WIN32\n"
"    DWORD a = GetFileAttributesW(xu_wide(p));\n"
"    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);\n"
"#else\n"
"    struct stat st;\n"
"    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);\n"
"#endif\n"
"}\n"
"static XS* xu_read(const char* path, int* ok) {\n"
"    *ok = 0;\n"
"    /* 读永远用二进制模式，与追加无关，所以这里不用 xu_fopen。\n"
"       Windows 上仍是宽字符版，路径含中文才打得开。 */\n"
"#ifdef _WIN32\n"
"    FILE* f = _wfopen(xu_wide(path), L\"rb\");\n"
"#else\n"
"    FILE* f = fopen(path, \"rb\");\n"
"#endif\n"
"    if (!f) return xs_new(\"\", 0);\n"
"    fseek(f, 0, SEEK_END);\n"
"    long sz = ftell(f);\n"
"    if (sz < 0) { fclose(f); return xs_new(\"\", 0); }\n"
"    fseek(f, 0, SEEK_SET);\n"
"    XS* s = xs_new(\"\", (size_t)sz);\n"
"    size_t got = 0;\n"
"    while (got < (size_t)sz) {\n"
"        size_t n = fread(s->d + got, 1, (size_t)sz - got, f);\n"
"        if (n == 0) break;\n"
"        got += n;\n"
"    }\n"
"    fclose(f);\n"
"    s->len = got; s->d[got] = 0;\n"
"    *ok = 1;\n"
"    return s;\n"
"}\n"
"static bool xu_write(const char* path, XS* s, bool app) {\n"
"    FILE* f = xu_fopen(path, app ? 1 : 0);\n"
"    if (!f) return false;\n"
"    bool ok = true;\n"
"    if (s->len && fwrite(s->d, 1, s->len, f) != s->len) ok = false;\n"
"    if (fclose(f) != 0) ok = false;\n"
"    return ok;\n"
"}\n"
"static XA* xu_list(const char* dir) {\n"
"    XA* a = xa_new(0, v_str(xs_new(\"\", 0)));\n"
"#ifdef _WIN32\n"
"    char pat[1200];\n"
"    snprintf(pat, sizeof(pat), \"%s\\\\*\", dir);\n"
"    WIN32_FIND_DATAW fd;\n"
"    HANDLE h = FindFirstFileW(xu_wide(pat), &fd);\n"
"    if (h == INVALID_HANDLE_VALUE) return a;\n"
"    do {\n"
"        char nm[1024];\n"
"        int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, 1023, 0, 0);\n"
"        if (n > 0 && strcmp(nm, \".\") != 0 && strcmp(nm, \"..\") != 0)\n"
"            xa_push(a, v_str(xs_new(nm, strlen(nm))));\n"
"    } while (FindNextFileW(h, &fd));\n"
"    FindClose(h);\n"
"#else\n"
"    DIR* d = opendir(dir);\n"
"    if (!d) return a;\n"
"    struct dirent* e;\n"
"    while ((e = readdir(d))) {\n"
"        if (strcmp(e->d_name, \".\") == 0 || strcmp(e->d_name, \"..\") == 0) continue;\n"
"        xa_push(a, v_str(xs_new(e->d_name, strlen(e->d_name))));\n"
"    }\n"
"    closedir(d);\n"
"#endif\n"
"    return a;\n"
"}\n"
"\n"
/* ---- TCP ---- */
"static bool xu_net_init(void) {\n"
"#ifdef _WIN32\n"
"    static int done = 0;\n"
"    if (done) return true;\n"
"    WSADATA w;\n"
"    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {\n"
"        snprintf(xu_err, sizeof(xu_err), \"无法初始化网络子系统\");\n"
"        return false;\n"
"    }\n"
"    done = 1;\n"
"#endif\n"
"    return true;\n"
"}\n"
"static void xu_nonblock(xu_sock s) {\n"
"#ifdef _WIN32\n"
"    u_long m = 1; ioctlsocket(s, FIONBIO, &m);\n"
"#else\n"
"    int fl = fcntl(s, F_GETFL, 0);\n"
"    if (fl >= 0) fcntl(s, F_SETFL, fl | O_NONBLOCK);\n"
"#endif\n"
"}\n"
"static int64_t xu_listen(const char* addr, int64_t port) {\n"
"    if (!xu_net_init()) return -1;\n"
"    xu_sock s = socket(AF_INET, SOCK_STREAM, 0);\n"
"    if (s == XU_SOCK_INVALID) { snprintf(xu_err, sizeof(xu_err), \"创建 socket 失败\"); return -1; }\n"
"    int yes = 1;\n"
"    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));\n"
"    struct sockaddr_in a; memset(&a, 0, sizeof(a));\n"
"    a.sin_family = AF_INET;\n"
"    a.sin_port = htons((unsigned short)port);\n"
"    if (addr && addr[0] && strcmp(addr, \"0.0.0.0\") != 0) {\n"
"        a.sin_addr.s_addr = inet_addr(addr);\n"
"        if (a.sin_addr.s_addr == INADDR_NONE) a.sin_addr.s_addr = INADDR_ANY;\n"
"    } else a.sin_addr.s_addr = INADDR_ANY;\n"
"    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {\n"
"        snprintf(xu_err, sizeof(xu_err), \"绑定 %s:%lld 失败\", addr ? addr : \"0.0.0.0\", (long long)port);\n"
"        XU_CLOSE(s); return -1;\n"
"    }\n"
"    if (listen(s, 64) != 0) { snprintf(xu_err, sizeof(xu_err), \"监听失败\"); XU_CLOSE(s); return -1; }\n"
"    xu_nonblock(s);\n"
"    return (int64_t)s;\n"
"}\n"
"static int64_t xu_accept(int64_t l) {\n"
"    if (l < 0) return -1;\n"
"    xu_sock c = accept((xu_sock)l, NULL, NULL);\n"
"    if (c == XU_SOCK_INVALID) return -1;\n"
"    xu_nonblock(c);\n"
"    int yes = 1;\n"
"    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));\n"
"    return (int64_t)c;\n"
"}\n"
"static XS* xu_recv(int64_t sd) {\n"
"    if (sd < 0) return xs_new(\"\", 0);\n"
"    char buf[65536];\n"
"    long n = (long)recv((xu_sock)sd, buf, sizeof(buf), 0);\n"
"    if (n <= 0) return xs_new(\"\", 0);\n"
"    return xs_new(buf, (size_t)n);\n"
"}\n"
"static int64_t xu_send(int64_t sd, XS* s) {\n"
"    if (sd < 0 || !s) return -1;\n"
"#ifdef _WIN32\n"
"    int n = send((SOCKET)sd, s->d, (int)s->len, 0);\n"
"    return (n == SOCKET_ERROR) ? -1 : (int64_t)n;\n"
"#else\n"
"    ssize_t n = send((int)sd, s->d, s->len, MSG_NOSIGNAL);\n"
"    return (n < 0) ? -1 : (int64_t)n;\n"
"#endif\n"
"}\n"
"static bool xu_alive(int64_t sd) {\n"
"    if (sd < 0) return false;\n"
"    char p[1];\n"
"#ifdef _WIN32\n"
"    int n = recv((SOCKET)sd, p, 1, MSG_PEEK);\n"
"    if (n == 0) return false;\n"
"    if (n == SOCKET_ERROR) {\n"
"        int e = WSAGetLastError();\n"
"        return e != WSAECONNRESET && e != WSAENOTCONN && e != WSAECONNABORTED;\n"
"    }\n"
"    return true;\n"
"#else\n"
"    ssize_t n = recv((int)sd, p, 1, MSG_PEEK);\n"
"    if (n == 0) return false;\n"
"    if (n < 0) return errno != ECONNRESET && errno != ENOTCONN;\n"
"    return true;\n"
"#endif\n"
"}\n"
"static int64_t xu_connect(const char* host, int64_t port) {\n"
"    if (!host || port <= 0 || port > 65535) {\n"
"        snprintf(xu_err, sizeof(xu_err), \"地址或端口非法\");\n"
"        return -1;\n"
"    }\n"
"    if (!xu_net_init()) return -1;\n"
"    struct sockaddr_in a; memset(&a, 0, sizeof(a));\n"
"    a.sin_family = AF_INET;\n"
"    a.sin_port = htons((unsigned short)port);\n"
"    a.sin_addr.s_addr = inet_addr(host);\n"
"    if (a.sin_addr.s_addr == INADDR_NONE) {\n"
"        struct hostent* he = gethostbyname(host);\n"
"        if (!he) { snprintf(xu_err, sizeof(xu_err), \"无法解析主机 %s\", host); return -1; }\n"
"        memcpy(&a.sin_addr, he->h_addr_list[0], sizeof(a.sin_addr));\n"
"    }\n"
"    xu_sock s = socket(AF_INET, SOCK_STREAM, 0);\n"
"    if (s == XU_SOCK_INVALID) { snprintf(xu_err, sizeof(xu_err), \"创建 socket 失败\"); return -1; }\n"
"    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {\n"
"        snprintf(xu_err, sizeof(xu_err), \"连接 %s:%lld 失败\", host, (long long)port);\n"
"        XU_CLOSE(s); return -1;\n"
"    }\n"
"    xu_nonblock(s);\n"
"    int yes = 1;\n"
"    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));\n"
"    return (int64_t)s;\n"
"}\n"
"static int64_t xu_port(int64_t l) {\n"
"    if (l < 0) return -1;\n"
"    struct sockaddr_in a;\n"
"#ifdef _WIN32\n"
"    int al = sizeof(a);\n"
"    if (getsockname((SOCKET)l, (struct sockaddr*)&a, &al) != 0) return -1;\n"
"#else\n"
"    socklen_t al = sizeof(a);\n"
"    if (getsockname((int)l, (struct sockaddr*)&a, &al) != 0) return -1;\n"
"#endif\n"
"    return (int64_t)ntohs(a.sin_port);\n"
"}\n"
"static XS* xu_peer(int64_t sd) {\n"
"    char out[64]; out[0] = 0;\n"
"    if (sd >= 0) {\n"
"        struct sockaddr_in a;\n"
"#ifdef _WIN32\n"
"        int al = sizeof(a);\n"
"        if (getpeername((SOCKET)sd, (struct sockaddr*)&a, &al) == 0) {\n"
"#else\n"
"        socklen_t al = sizeof(a);\n"
"        if (getpeername((int)sd, (struct sockaddr*)&a, &al) == 0) {\n"
"#endif\n"
"            unsigned char* b = (unsigned char*)&a.sin_addr.s_addr;\n"
"            snprintf(out, sizeof(out), \"%u.%u.%u.%u:%d\", b[0], b[1], b[2], b[3],\n"
"                     (int)ntohs(a.sin_port));\n"
"        }\n"
"    }\n"
"    return xs_new(out, strlen(out));\n"
"}\n"
"\n";

// ============================================================
// 窗口运行时的内嵌
// ============================================================
//
// ── 为什么要内嵌，而不是编译时再链接 ───────────────────────
//
// 语言对 `x build --emit=exe` 的承诺是「产出单个自包含的可执行
// 文件」。把 src/gui/gui_win32.c 加进 gcc 命令行也能编出可跑的
// 产物，但那个产物依赖源码树还在原处 —— 拷到别人机器上就再也
// 编译不了。所以窗口实现必须**进到生成的 C 代码里**。
//
// ── 为什么不在这里再写一份 Win32 实现 ──────────────────────
//
// 两份代码必然漂移。改解释器那份忘了这份，症状是「同一程序
// 解释跑得好、AOT 产物行为不同」——而普通的一致性测试（只比
// stdout）抓不到图形差异。
//
// 所以真源只有 src/gui/gui_win32.c，由 tools/gen_gui_embed.py
// 在构建期转成 src/gui/gui_embed.h 里的两个字符串常量。
// 这里只负责在程序用到窗口能力时把它们吐出来。
//
// ── 为什么按需内嵌 ─────────────────────────────────────────
//
// 不用窗口的程序（绝大多数测试）不该为它多编译几千行 GDI 代码，
// 也不该被逼着链接 gdi32/user32。按需内嵌之后，hello world 的
// 产物和以前逐字节相同。
//
// 判据 bc_uses_gui 在 bytecode.c —— 它不是代码生成器独有的知识，
// 参数检查（main.c 的 --profile 语义检查）也要用同一份判断。
// 两处各写一份必然漂移，漂移的症状是「警告说没用到窗口，但产物里
// 内嵌了 500KB GDI 代码」，排查起来毫无线索。

// ============================================================
// 栈深分析
// ============================================================

// 每个函数需要一张「pc → 相对栈深」表。为什么必须静态算出：
// 生成本地变量直访（s[N] 而不是 s[sp++]）是性能的关键，
// 而这要求每个位置的栈深在编译期就确定。
//
// 如果某个位置因为控制流汇合而栈深不唯一，说明字节码有问题
// ——编译器保证了栈平衡，正常情况下不会发生。所以这里不做
// 「取最大值」之类的兜底，而是明确报错。

static bool compute_stack_depths(Bytecode* bc, FuncProto* fn, int* depth) {
    int n = fn->code_len;
    for (int i = 0; i < n; i++) depth[i] = -1;

    // 工作队列：从入口开始做一遍传播。跳转会把深度带到目标处，
    // 若目标处已有不同深度 → 字节码不平衡 → 报错。
    //
    // ── 起始深度为什么是 local_count 而不是 0 ─────────────────
    //
    // 解释器的帧布局是「局部变量槽在最底下，操作数栈长在它们上面」：
    // 调用时先把 argc 个实参压栈，再看作参数槽位，其余局部变量补 null
    // （见 interp.c 的 BC_CALL）。所以 pc 0 处的栈顶位置 = local_count。
    //
    // 生成代码时用同一个数组 s[] 同时表示两者，于是 s[0..local_count-1]
    // 是局部变量、s[local_count..] 才是操作数栈。起始深度写 0 会让
    // 每条指令的下标整体偏低 local_count，第一个 LOAD_LOCAL 就写到
    // s[-2] —— 越界写、结果全错、但不崩溃（栈外内存恰好可写）。
    int* work = x_alloc((size_t)n * sizeof(int));
    int wn = 0;
    depth[0] = fn->local_count;
    work[wn++] = 0;

    while (wn > 0) {
        int pc = work[--wn];
        int d = depth[pc];
        Insn* in = &bc->code[fn->code_start + pc];

        // 这条指令之后的新深度
        int nd = d + bc_op_stack_effect_at(bc, in);
        if (nd < 0) {
            free(work);
            return false;
        }

        // 本指令的跳转目标（若有）
        int target = -1;
        switch (in->op) {
            case BC_JUMP:
            case BC_JUMP_IF_FALSE:
            case BC_JUMP_IF_TRUE:
            case BC_LT_JUMP_FALSE:
                // 操作数是「相对本条的下一条」的函数内偏移。
                target = pc + 1 + in->a;
                break;
            default: break;
        }

        // JUMP 之后不落空（无 fallthrough）；条件跳两条路都要走
        bool falls = (in->op != BC_JUMP && in->op != BC_RETURN &&
                      in->op != BC_RETURN_VOID && in->op != BC_HALT);
        bool branches = (in->op == BC_JUMP_IF_FALSE || in->op == BC_JUMP_IF_TRUE);

        // 条件跳：条件本身被弹出（stack_effect 已算），目标处深度 = nd
        // JUMP：目标处深度 = d（JUMP 本身不改变栈）
        int tdepth = (in->op == BC_JUMP) ? d : nd;

        if (target >= 0 && target < n) {
            if (depth[target] < 0) {
                depth[target] = tdepth;
                work[wn++] = target;
            } else if (depth[target] != tdepth) {
                free(work);
                return false;   // 栈不平衡
            }
        }

        if (falls && pc + 1 < n) {
            if (depth[pc + 1] < 0) {
                depth[pc + 1] = nd;
                work[wn++] = pc + 1;
            } else if (depth[pc + 1] != nd) {
                free(work);
                return false;
            }
        }
        (void)branches;
    }

    free(work);
    return true;
}

// ============================================================
// 生成
// ============================================================

typedef struct {
    Bytecode* bc;
    Emit*     e;
    int       main_idx;
    int       tmp;          // 临时值计数器
} Cg;

static void gen_function(Cg* g, int fi, int* depths);

static int codegen_body(Cg* g, int fi);

// 生成整个程序
//
// prof 决定三件事，全部在编译期定死：
//   1. 内不内嵌窗口运行时（体积与依赖的来源）
//   2. 入口的栈容量（换成调用深度上限乘单帧大小）
//   3. main() 的形状（是不是要把输出收进 stderr 之类）
//
// 传 NULL 等价于 console 档 —— 这样所有既有的调用点（包括测试里
// 直接调 codegen_c_emit 的地方）行为不变，不必为了一个可选参数
// 去改十几处。
// 生成结构体的静态元数据表。
//
// ── 为什么必须是静态表而不是运行时构造 ──────────────────────
//
// 结构体的类型名与字段名是**编译期常量**，而 BC_NEW_STRUCT 在
// 每次执行时都要拿到它们（对象里存的是指针，不是拷贝）。运行时
// 构造意味着每次 new 都要分配字符串、并且要考虑谁释放 —— 而
// 这些内容在整个进程生命周期里都不变，静态表是唯一合理的形态。
//
// 表名用 xo_ti / xo_tn / xo_fn 前缀，避免与用户函数的 xf_N 冲突，
// 也避免与 PRELUDE 里的诊断函数撞名。
static void gen_struct_tables(Bytecode* bc, FILE* out) {
    int n = bc->struct_count;
    fprintf(out, "/* ---------- 结构体元数据 ---------- */\n");
    if (n == 0) {
        // 没有结构体时也要给一个空表：gen_function 里的常量索引
        // 用不到它，但空数组在 C 里是非法扩展（gcc 会警告
        // 「zero-size array」），给一个哑元项最省事。
        fputs("static const char* xo_tn[1] = { \"\" };\n", out);
        return;
    }
    fprintf(out, "static const char* xo_tn[%d] = {\n", n);
    for (int i = 0; i < n; i++) {
        fputs("    ", out);
        // 转义：结构体名是标识符，理论上不含特殊字符，
        // 但用户可能用中文名（lexer 允许），那要走八进制转义，
        // 否则生成的 C 源码编码依赖编辑器保存格式。
        const char* s = bc->structs[i].name ? bc->structs[i].name : "?";
        fputc('"', out);
        for (const char* p = s; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == '"')       fputs("\\\"", out);
            else if (ch == '\\') fputs("\\\\", out);
            else if (ch < 0x20 || ch >= 0x7F) fprintf(out, "\\%03o", ch);
            else fputc((int)ch, out);
        }
        fprintf(out, "\",%s\n", i + 1 < n ? "" : "");
    }
    fputs("};\n", out);

    // 字段名表：每个结构体一个数组，再用指针数组把它们串起来。
    // 指针数组的存在是因为 XObj 里存的是 `const char**` ——
    // 字段数可变，没法用一个二维定长数组表达。
    for (int i = 0; i < n; i++) {
        fprintf(out, "static const char* xo_fn_%d[%d] = {\n", i,
                bc->structs[i].field_count ? bc->structs[i].field_count : 1);
        for (int k = 0; k < bc->structs[i].field_count; k++) {
            const char* s = bc->structs[i].field_names[k]
                            ? bc->structs[i].field_names[k] : "?";
            fputs("    \"", out);
            for (const char* p = s; *p; p++) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '"')       fputs("\\\"", out);
                else if (ch == '\\') fputs("\\\\", out);
                else if (ch < 0x20 || ch >= 0x7F) fprintf(out, "\\%03o", ch);
                else fputc((int)ch, out);
            }
            fputs("\",\n", out);
        }
        if (bc->structs[i].field_count == 0) fputs("    0,\n", out);
        fputs("};\n", out);
    }

    fprintf(out, "static const char** xo_fns[%d] = {\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(out, "    xo_fn_%d,\n", i);
    }
    fputs("};\n", out);

    fprintf(out, "static const int xo_fc[%d] = {\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(out, "    %d,\n", bc->structs[i].field_count);
    }
    fputs("};\n\n", out);
}

// 把一个字符串字面量按 C 语法写进生成的文件。
//
// 必须转义非 ASCII：类名/字段名可能是中文（lexer 允许），直接写
// 进去会让生成的 .c 文件编码依赖保存格式 —— 换个编辑器打开保存
// 一次就变成乱码，而报错信息指向的是一行看似正确的代码。
static void gen_c_string_lit(FILE* out, const char* s, const char* indent) {
    fputs(indent, out);
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"')       fputs("\\\"", out);
        else if (ch == '\\') fputs("\\\\", out);
        else if (ch == '\n') fputs("\\n", out);
        else if (ch < 0x20 || ch >= 0x7F) fprintf(out, "\\%03o", ch);
        else fputc((int)ch, out);
    }
    fputs("\",\n", out);
}

// 类的元数据表。
//
// 与结构体那套一一对应，但多了**虚表**：多态完全靠它，所以它必须以
// 静态数组的形式落到生成的 C 里。反射与注解查询同样只能靠静态表 ——
// AOT 产物里没有 Bytecode，所有元数据必须在生成期变成 C 数组。
//
// 表结构：
//   xc_tn  [n]        类名（实际类型名，打印用）
//   xc_fn_i[n_i]      第 i 个类的字段名表
//   xc_fns [n]        上面那些表的指针（XK 里存 const char**）
//   xc_fc  [n]        字段数
//   xc_ft_i[n_i]      字段类型名表（反射要返回 "Int" 这类）
//   xc_fts [n]        ↑ 的指针数组
//   xc_fvis[n_i]      字段可见性（0=public 1=protected 2=private）
//   xc_fviss[n]       ↑ 的指针数组
//   xc_ffin[n_i]      字段 final 标志
//   xc_ffins[n]       ↑ 的指针数组
//   xc_pi  [n]        父类索引（-1 = 没有）
//   xc_vt_i[n_i]      第 i 个类的虚表（-1 = 未实现）
//   xc_vts [n]        虚表指针数组
//   xc_vtl [n]        每个类的虚表长度
//   xc_mvis[n_i]      方法可见性（与虚表平行）
//   xc_mviss[n]       ↑ 的指针数组
//   xc_vcall(cls,slot) 查表函数
//   xc_find(cls,name)  按名字找字段后缀（反射）
static void gen_class_tables(Bytecode* bc, FILE* out) {
    int n = bc->class_count;
    fputs("/* ---------- 类元数据 ---------- */\n", out);
    if (n == 0) {
        // 空表也要给哑元：C 不允许零长数组，而且生成的代码可能
        // 在「程序里没有类」的情况下仍然引用这些符号（比如某个
        // 函数体里有 VIRT_CALL 但类被优化掉了 —— 不会发生，但
        // 让符号永远存在比推理「一定不引用」便宜）。
        fputs("static const char* xc_tn[1] = { \"\" };\n", out);
        fputs("static const char** xc_fns[1] = { 0 };\n", out);
        fputs("static const int xc_fc[1] = { 0 };\n", out);
        fputs("static const char** xc_fts[1] = { 0 };\n", out);
        fputs("static const unsigned char* xc_fviss[1] = { 0 };\n", out);
        fputs("static const unsigned char* xc_ffins[1] = { 0 };\n", out);
        fputs("static const int xc_pi[1] = { -1 };\n", out);
        fputs("static const unsigned char* xc_mviss[1] = { 0 };\n", out);
        fputs("static const char* xc_aname[1] = { \"\" };\n", out);
        fputs("static const int xc_atgt[1] = { 0 };\n", out);
        fputs("static const int xc_aci[1] = { -1 };\n", out);
        fputs("static const char* xc_amenber[1] = { 0 };\n", out);
        fputs("static const int xc_aac[1] = { 0 };\n", out);
        fputs("static const char* const* xc_akeys[1] = { 0 };\n", out);
        fputs("static const char* const* xc_avals[1] = { 0 };\n", out);
        fputs("static int xc_anc = 0;\n", out);
        fputs("static int xc_vcall(int cls, int slot) "
              "{ (void)cls; (void)slot; return -1; }\n", out);
        fputs("static int xc_find_fld(int cls, const char* nm) "
              "{ (void)cls; (void)nm; return -1; }\n", out);
        fputs("static int xc_find_slot(const char* nm) "
              "{ (void)nm; return -1; }\n", out);
        fputs("static const char* xc_slotname(int s) "
              "{ (void)s; return \"?\"; }\n\n", out);
        return;
    }

    // 类名
    fprintf(out, "static const char* xc_tn[%d] = {\n", n);
    for (int i = 0; i < n; i++) gen_c_string_lit(out, bc->classes[i].name ? bc->classes[i].name : "?", "    ");
    fputs("};\n", out);

    // 字段名表（每个类一个）
    for (int i = 0; i < n; i++) {
        int fc = bc->classes[i].field_count;
        fprintf(out, "static const char* xc_fn_%d[%d] = {\n", i, fc ? fc : 1);
        for (int k = 0; k < fc; k++) {
            gen_c_string_lit(out, bc->classes[i].field_names[k]
                                  ? bc->classes[i].field_names[k] : "?", "    ");
        }
        if (fc == 0) fputs("    0,\n", out);
        fputs("};\n", out);
    }
    fprintf(out, "static const char** xc_fns[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_fn_%d,\n", i);
    fputs("};\n", out);

    fprintf(out, "static const int xc_fc[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    %d,\n", bc->classes[i].field_count);
    fputs("};\n", out);

    // ---- 字段类型 / 可见性 / final ----
    //
    // 三张平行表，反射要按字段下标同时取到它们。数组元素是
    // unsigned char 而不是枚举：生成的 C 里不需要知道 Visibility
    // 这个类型（那是编译器的概念），而 0/1/2 的取值在两侧是同一套
    // 约定（见 vis_label）。
    for (int i = 0; i < n; i++) {
        ClassType* ct = &bc->classes[i];
        int fc = ct->field_count;

        fprintf(out, "static const char* xc_ft_%d[%d] = {\n", i, fc ? fc : 1);
        for (int k = 0; k < fc; k++) {
            gen_c_string_lit(out, ct->field_types && ct->field_types[k]
                                  ? ct->field_types[k] : "", "    ");
        }
        if (fc == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const unsigned char xc_fv_%d[%d] = {\n", i, fc ? fc : 1);
        for (int k = 0; k < fc; k++) {
            fprintf(out, "    %d,\n", ct->field_vis ? (int)ct->field_vis[k] : 0);
        }
        if (fc == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const unsigned char xc_ff_%d[%d] = {\n", i, fc ? fc : 1);
        for (int k = 0; k < fc; k++) {
            fprintf(out, "    %d,\n",
                    (ct->field_final && ct->field_final[k]) ? 1 : 0);
        }
        if (fc == 0) fputs("    0,\n", out);
        fputs("};\n", out);
    }
    fprintf(out, "static const char** xc_fts[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_ft_%d,\n", i);
    fputs("};\n", out);
    fprintf(out, "static const unsigned char* xc_fviss[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_fv_%d,\n", i);
    fputs("};\n", out);
    fprintf(out, "static const unsigned char* xc_ffins[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_ff_%d,\n", i);
    fputs("};\n", out);

    // 父类索引（classParent 用）
    fprintf(out, "static const int xc_pi[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    %d,\n", bc->classes[i].parent_idx);
    fputs("};\n", out);

    // 虚表
    for (int i = 0; i < n; i++) {
        int vl = bc->classes[i].vtable_len;
        fprintf(out, "static const int xc_vt_%d[%d] = {\n", i, vl ? vl : 1);
        for (int k = 0; k < vl; k++) {
            fprintf(out, "    %d,\n", bc->classes[i].vtable ? bc->classes[i].vtable[k] : -1);
        }
        if (vl == 0) fputs("    -1,\n", out);
        fputs("};\n", out);
    }
    fprintf(out, "static const int* xc_vts[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_vt_%d,\n", i);
    fputs("};\n", out);
    fprintf(out, "static const int xc_vtl[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    %d,\n", bc->classes[i].vtable_len);
    fputs("};\n", out);

    // 方法可见性（与虚表平行）
    for (int i = 0; i < n; i++) {
        ClassType* ct = &bc->classes[i];
        int vl = ct->vtable_len;
        fprintf(out, "static const unsigned char xc_mv_%d[%d] = {\n", i, vl ? vl : 1);
        for (int k = 0; k < vl; k++) {
            fprintf(out, "    %d,\n", ct->method_vis ? (int)ct->method_vis[k] : 0);
        }
        if (vl == 0) fputs("    0,\n", out);
        fputs("};\n", out);
    }
    fprintf(out, "static const unsigned char* xc_mviss[%d] = {\n", n);
    for (int i = 0; i < n; i++) fprintf(out, "    xc_mv_%d,\n", i);
    fputs("};\n", out);

    // ---- 注解表 ----
    //
    // 与 Bytecode 里那张扁平表同构：一条记录一行，靠 target +
    // class_idx + member 定位。读侧用线性扫描 —— 注解是几十条
    // 的量级，索引没有意义。
    //
    // 空表也要给出符号：程序里一条注解都没有时，查询代码仍然会
    // 引用 xc_aname / xc_anc，让符号永远存在比推理「一定不引用」便宜。
    {
        int an = bc->anno_count;
        for (int i = 0; i < an; i++) {
            AnnoEntry* e = &bc->annos[i];
            int ac = e->arg_count;
            // 每个注解的实参键/值数组。键可能是 NULL（位置实参），
            // 用空串占位并在 xc_akeys 里体现「无键」——查询侧按空串
            // 判断即可（注解键名不可能是空字符串，解析器就不允许）。
            if (ac > 0) {
                fprintf(out, "static const char* xc_ak_%d[%d] = {\n", i, ac);
                for (int k = 0; k < ac; k++) {
                    gen_c_string_lit(out, (e->arg_keys && e->arg_keys[k])
                                          ? e->arg_keys[k] : "", "    ");
                }
                fputs("};\n", out);
                fprintf(out, "static const char* xc_av_%d[%d] = {\n", i, ac);
                for (int k = 0; k < ac; k++) {
                    gen_c_string_lit(out, (e->arg_vals && e->arg_vals[k])
                                          ? e->arg_vals[k] : "", "    ");
                }
                fputs("};\n", out);
            }
        }
        fprintf(out, "static const char* xc_aname[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) gen_c_string_lit(out, bc->annos[i].name ? bc->annos[i].name : "", "    ");
        if (an == 0) fputs("    \"\",\n", out);
        fputs("};\n", out);

        fprintf(out, "static const int xc_atgt[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) fprintf(out, "    %d,\n", (int)bc->annos[i].target);
        if (an == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const int xc_aci[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) fprintf(out, "    %d,\n", bc->annos[i].class_idx);
        if (an == 0) fputs("    -1,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const char* xc_amenber[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) gen_c_string_lit(out, bc->annos[i].member ? bc->annos[i].member : "", "    ");
        if (an == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const int xc_aac[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) fprintf(out, "    %d,\n", bc->annos[i].arg_count);
        if (an == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const char* const* xc_akeys[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) {
            if (bc->annos[i].arg_count > 0) fprintf(out, "    xc_ak_%d,\n", i);
            else                            fputs("    0,\n", out);
        }
        if (an == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static const char* const* xc_avals[%d] = {\n", an ? an : 1);
        for (int i = 0; i < an; i++) {
            if (bc->annos[i].arg_count > 0) fprintf(out, "    xc_av_%d,\n", i);
            else                            fputs("    0,\n", out);
        }
        if (an == 0) fputs("    0,\n", out);
        fputs("};\n", out);

        fprintf(out, "static int xc_anc = %d;\n", an);
    }

    // 查表函数。越界的 cls/slot 一律返回 -1 —— 让运行时报
    // 「没有实现该虚方法」，而不是跳进一个随机函数。
    fprintf(out,
            "static int xc_vcall(int cls, int slot) {\n"
            "    if (cls < 0 || cls >= %d) return -1;\n"
            "    if (slot < 0 || slot >= xc_vtl[cls]) return -1;\n"
            "    return xc_vts[cls][slot];\n"
            "}\n", n);

    // 按名字找字段（反射用）。字段表含继承，父类在前。
    fprintf(out,
            "static int xc_find_fld(int cls, const char* nm) {\n"
            "    if (cls < 0 || cls >= %d || !nm) return -1;\n"
            "    for (int i = 0; i < xc_fc[cls]; i++)\n"
            "        if (xc_fns[cls][i] && strcmp(xc_fns[cls][i], nm) == 0) return i;\n"
            "    return -1;\n"
            "}\n", n);

    // 按名字找方法槽（methodHas 用）。槽表被编译期压平成名字数组。
    {
        int ns = bc->method_slot_count;
        fprintf(out, "static const char* xc_sn[%d] = {\n", ns ? ns : 1);
        for (int i = 0; i < ns; i++) {
            gen_c_string_lit(out, bc->method_slots[i].name ? bc->method_slots[i].name : "", "    ");
        }
        if (ns == 0) fputs("    \"\",\n", out);
        fputs("};\n", out);
        fprintf(out, "static int xc_nslot = %d;\n", ns);
        fprintf(out,
                "static int xc_find_slot(const char* nm) {\n"
                "    if (!nm) return -1;\n"
                "    for (int i = 0; i < xc_nslot; i++)\n"
                "        if (strcmp(xc_sn[i], nm) == 0) return i;\n"
                "    return -1;\n"
                "}\n");
        fprintf(out,
                "static const char* xc_slotname(int s) {\n"
                "    if (s < 0 || s >= xc_nslot) return \"?\";\n"
                "    return xc_sn[s];\n"
                "}\n\n");
    }
}

static bool gen_program(Bytecode* bc, FILE* out, const XProfileDef* prof) {
    if (!prof) prof = profile_get(PROF_CONSOLE);

    Emit e = { out, 0 };
    Cg g;
    g.bc = bc;
    g.e = &e;
    g.main_idx = -1;
    g.tmp = 0;

    for (int i = 0; i < bc->func_count; i++) {
        if (strcmp(bc->funcs[i].name, "main") == 0) { g.main_idx = i; break; }
    }
    if (g.main_idx < 0) return false;

    // ---- 头部 ----
    fprintf(out, "%s", PRELUDE);

    // ---- 结构体元数据 ----
    // 必须在用户函数之前：BC_NEW_STRUCT 生成的代码引用 xo_tn / xo_fns / xo_fc。
    gen_struct_tables(bc, out);

    // ---- 类元数据 ----
    // 同样必须在用户函数之前：BC_NEW_CLASS / BC_VIRT_CALL 引用
    // xc_tn / xc_fns / xc_fc / xc_vcall。
    gen_class_tables(bc, out);

    // ---- 窗口运行时（按需） ----
    //
    // 放在 PRELUDE 之后、函数体之前。位置上没得选：gui 的实现
    // 要用到 PRELUDE 里的 xs_new / rt_err 之类，必须排在后面。
    //
    // 注意两段之间的衔接：GUI_IMPL_HEAD 以 gui.h 的 #ifndef
    // 守卫结尾，GUI_IMPL_BODY 里已经去掉了 `#include "gui.h"`
    // （换成了一个注释），所以直接顺序拼接即可。
    //
    // ── 判据为什么是「程序用没用」而不是「档位要不要」────────
    //
    // 这两者是**不同性质**的事实，混在一起会做错决定：
    //
    //   程序用没用窗口   —— 关于这份代码的事实，由字节码决定
    //   档位要不要窗口   —— 关于使用场景的声明，由 --profile 决定
    //
    // 按档位决定内嵌，会出现两种坏结果：用 --profile=desktop 编一个
    // hello world，产物从 47 KB 涨到 500 KB，里面一大半是永远不执行的
    // GDI 代码；而用 --profile=console 编扫雷，窗口代码被裁掉，链接期
    // 直接报 undefined reference to xgui_open —— 一个对用户毫无指向性
    // 的错误（他要的是「别弹黑框」，不是「我的窗口不见了」）。
    //
    // 所以内嵌只看向量里有没有窗口内建（事实），档位只负责入口形状
    // （子系统、栈、诊断）。两者不匹配时给警告，不擅自替用户改代码。
    bool gui = bc_uses_gui(bc);
    if (gui) {
        fputs("/* ===== 窗口运行时（由 src/gui/gui_win32.c 内嵌）===== */\n",
              out);
        fprintf(out, "%s", GUI_IMPL_HEAD);
        fprintf(out, "%s", GUI_IMPL_BODY);
        fputs("/* ===== 窗口运行时结束 ===== */\n\n", out);
    }

    // ---- 窗口相关的辅助函数 ----
    //
    // 必须排在 GUI_IMPL_HEAD **之后**：它调用 xgui_path，而那个声明
    // 在内嵌的 gui.h 里。先前把它放在 PRELUDE（也就是整段窗口运行时
    // 之前）时，AOT 编译报
    //     implicit declaration of function 'xgui_path'
    // —— 这个报错看起来像「忘了 include」，实际原因是声明顺序，
    // 而解释档完全不受影响（那边是直接调 C 函数，没有生成代码这层）。
    // 所以只有 AOT 会暴露它。
    if (gui) {
        fputs(
"/* ---------- 窗口：路径 ---------- */\n"
"/*\n"
" * winPath 的数组→int[] 转换。\n"
" *\n"
" * 放在运行时辅助函数里而不是内联进生成的调用点，原因是这段\n"
" * 需要 malloc + 逐项类型校验 + free，三条都是错误路径 —— 内联\n"
" * 会让每个调用点都复制一遍错误处理，而且任何一处漏 free 就是\n"
" * 每帧漏一次。集中在一个函数里，漏不掉。\n"
" *\n"
" * 参数用 unsigned int 而不是 XColor：这里虽然排在 gui.h 之后，\n"
" * 但其实也不该依赖那个 typedef —— 换个内嵌顺序就会坏。\n"
" * unsigned int 与 XColor 在这里完全等价\n"
" * （gui.h 里就是 typedef unsigned int XColor），且没有顺序依赖。\n"
" *\n"
" * 类型校验必须逐个元素做：X 的数组是异构的（Value[]），\n"
" * 直接按 int64 读 XV_FLOAT 的槽位会拿到浮点数的位模式，\n"
" * 画出一个完全无关的形状 —— 而且不报错。\n"
" */\n"
"static void xb_win_path(V* a, unsigned int color) {\n"
"    if (a->t != XV_ARRAY) return;\n"
"    XA* arr = a->a;\n"
"    int n = (int)arr->len;\n"
"    if (n < 6 || (n % 2) != 0) {\n"
"        rt_err(\"winPath 的点集长度必须是偶数且至少 6 个数字（3 个点）\");\n"
"    }\n"
"    int* flat = (int*)malloc((size_t)n * sizeof(int));\n"
"    if (!flat) rt_err(\"winPath 分配临时缓冲失败\");\n"
"    for (int i = 0; i < n; i++) {\n"
"        if (arr->d[i].t != XV_INT) {\n"
"            free(flat);\n"
"            rt_err(\"winPath 点集的每一项都必须是 Int\");\n"
"        }\n"
"        flat[i] = (int)arr->d[i].i;\n"
"    }\n"
"    xgui_path(flat, n / 2, color);\n"
"    free(flat);\n"
"}\n"
"\n"
        , out);
    }

    // 源文件名在 PRELUDE 里已声明为 "?"，这里赋值成真实路径。
    // 不能在 PRELUDE 里直接写死：PRELUDE 是常量字符串，
    // 而文件名是每次编译都变的。也不能在这里重新声明——
    // 那会变成重复定义（同一个翻译单元里不能有两次外部定义）。
    fprintf(out, "static void x_set_file(void) { g_file = \"");
    const char* sf = bc->source_file ? bc->source_file : "?";
    for (const char* p = sf; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"')       fputs("\\\"", out);
        else if (ch == '\\') fputs("\\\\", out);
        else if (ch < 0x20 || ch >= 0x7F) fprintf(out, "\\%03o", ch);
        else fputc((int)ch, out);
    }
    fprintf(out, "\"; }\n\n");

    // ---- 前置声明 ----
    // 所有用户函数先声明一遍，这样互递归与「先调用后定义」都不成问题。
    // 参数统一走值栈，所以签名只有参数个数差异，但为了可读性仍显式写出。
    for (int i = 0; i < bc->func_count; i++) {
        FuncProto* fn = &bc->funcs[i];
        fprintf(out, "static V xf_%d(V* s);\n", i);
        (void)fn;
    }
    fprintf(out, "\n");

    // ---- 每个函数 ----
    for (int i = 0; i < bc->func_count; i++) {
        gen_function(&g, i, NULL);
    }

    // ---- 入口 ----
    //
    // 值栈大小取所有函数 local_count 的最大值，再乘**档位给的调用深度**。
    // 不能只按 main 算：调用深时每个帧都要占位置。
    //
    // ── 为什么深度上限由档位给，而不是写死 4096 ──────────────
    //
    // 这个数组是 static 的，也就是说它是镜像里的 .bss 段 —— 大小
    // 直接进 PE/ELF 的内存映像。max_local 通常是 20~60，4096 档位下
    // 就是 80K~250K 个 V（每个 V 至少 16 字节，实测 sizeof(V)=16），
    // 即 1.3~4 MB 的虚拟内存，全部是零页，第一次触到才真分配。
    //
    // 控制台工具 4 MB 是浪费但无害（零页不占物理内存）。桌面程序的
    // 情况不同：它的调用栈天生更深（消息泵 → 派发 → 业务 → 重绘），
    // 而且递归画树、递归求值的 GUI 代码很常见，所以桌面档给 8192。
    // 服务端反过来：长驻进程的线程栈是有限的（Linux 默认 8 MB，
    // Windows 默认 1 MB），开太大反而让嵌套调用更容易撞墙。
    //
    // 上限 4M 个 V 是原来就有的兜底，保留：函数局部变量特别多
    // （比如一个 2 万行的生成代码函数）时不能无限涨。
    int max_local = 1;
    for (int i = 0; i < bc->func_count; i++) {
        if (bc->funcs[i].local_count > max_local) max_local = bc->funcs[i].local_count;
    }
    long stack_sz = (long)max_local * (long)prof->call_depth;
    if (stack_sz < 256) stack_sz = 256;
    if (stack_sz > 4L * 1024 * 1024) stack_sz = 4L * 1024 * 1024;

    // 入口函数体。
    //
    // ── 三种档位在 main 里的差别 ───────────────────────────
    //
    // console / desktop：一模一样。子系统差异写在 PE 头里（链接参数），
    //   源码层面没有任何区别 —— 这正是「同一份函数体两种子系统通用」
    //   的好处，不必为桌面档另写一个 WinMain。
    //
    // service：输出策略不同 —— stdout 关掉缓冲（_IONBF）。
    //
    //   ── 为什么这里是 stdout 不是 stderr ─────────────────────
    //
    //   最初的写法是给 stderr 加 _IONBF，注释里写着「崩溃前的最后几条
    //   错误不能丢」。这个想法是错的，而且是**可实测证伪**的错：
    //
    //     C 标准规定 stderr 在启动时就是无缓冲的。MinGW 实测：
    //     2000 行 stderr 输出后 TerminateProcess 硬杀，两种写法都
    //     存活 2000 行 —— setvbuf(stderr, _IONBF) 是个空操作。
    //
    //   也就是说那一行代码在「看起来做了保护」和「什么都没做」之间，
    //   属于后者。留着它比删掉更坏：它让人以为 stderr 已经安全了。
    //
    //   真正有差异的是 stdout。实测（同样硬杀）：
    //
    //     _IOLBF（行缓冲）  半行 "no-newline-tail" 丢失
    //     _IONBF（无缓冲）  同一份输出完整保留
    //
    //   服务进程的日志走 stdout（journald / supervisord 都是抓 stdout），
    //   而服务的典型死法是**在某一行写了一半时被 OOM killer 或服务管理器
    //   干掉**。行缓冲会把那半行留在缓冲区里，于是日志的最后一条恰恰是
    //   缺失的那条 —— 而它是排查「为什么挂了」的唯一线索。
    //
    //   代价是每条输出一次 write，服务进程的输出量不是热路径，可以付。
    //
    //   console / desktop 档保持 _IOLBF：终端见 \n 即刷，重定向到文件时
    //   也只需要「行级完整」，而全缓冲能省系统调用。
    //
    // 判据从表里取（prof->stdout_buf），不在这里按名字判断 ——
    // 按名字判断的意思是「档位表管一半、代码管一半」，加档位时必漏。
    bool no_buf = (prof && prof->stdout_buf == XBUF_NONE);

    fprintf(out,
        "\nint main(int argc, char** argv) {\n"
        "    (void)argc; (void)argv;\n"
        "%s"
        "    x_set_file();\n"
        "    static V s[%ld];\n"
        "    for (long i = 0; i < %ld; i++) s[i].t = XV_NULL;\n"
        "    for (int i = 0; i < %d; i++) s[i].t = XV_NULL;\n"
        "    V r = xf_%d(s);\n"
        "    int code = (r.t == XV_INT) ? (int)(r.i & 0xFF) : 0;\n"
        "    v_rel(&r);\n"
        "    fflush(stdout);\n"
        "    return code;\n"
        "}\n",
        no_buf ? "    setvbuf(stdout, NULL, _IONBF, 0);   /* service: 崩溃不留半行 */\n"
               : "    setvbuf(stdout, NULL, _IOLBF, 0);\n",
        stack_sz, stack_sz, bc->funcs[g.main_idx].local_count, g.main_idx);

    return true;
}

// 生成单个函数体。
//
// 核心思路：每个函数是 `static V xf_N(V* s)`，参数与局部变量都在
// 传入的栈区里（调用方已把实参压好）。函数体是一段 C 代码，
// 判断函数是否为叶子：函数体内没有任何 CALL。
//
// ── 为什么叶子这件事值得单独判断 ────────────────────────────
//
// 生成的代码把整个值栈放在一个数组 s 里，局部变量和操作数栈共用。
// 只要 s 的地址被传出去过（即函数里调用了别的函数，`xf_N(s + k)`），
// gcc 就必须保守地假设任意 s[i] 都可能被那次调用改掉，于是**每个槽
// 都留在内存里**，每条字节码都变成一次 load + 一次 store。
//
// 叶子函数没有这个问题。把它的 s 变成局部数组后，gcc 的 SRA
// (Scalar Replacement of Aggregates) 能把那些常量下标的槽直接提升成
// 寄存器，循环体里的加减就回到了原生整数运算。
//
// 实测收益见 spec/07 §3.9。这不是「顺手优化」，而是让 AOT 这个
// 做法本身站得住的必要条件——否则产物比解释器还慢（0.8×）。
static bool func_is_leaf(Bytecode* bc, FuncProto* fn) {
    for (int pc = 0; pc < fn->code_len; pc++) {
        Insn* in = &bc->code[fn->code_start + pc];
        if (in->op == BC_CALL) return false;
    }
    return true;
}

// 用 goto 实现跳转（字节码的 pc 就是 label 号）。
static void gen_function(Cg* g, int fi, int* depths) {
    (void)depths;
    Bytecode* bc = g->bc;
    FuncProto* fn = &bc->funcs[fi];
    Emit* e = g->e;

    bool leaf = func_is_leaf(bc, fn);

    fprintf(e->f, "\n/* %s/%d  locals=%d%s */\n",
            fn->name, fn->param_count, fn->local_count,
            leaf ? " leaf" : "");

    // ── 叶子函数加 __restrict 的理由 ──────────────────────────
    //
    // 值栈是一个共享数组，函数的 s 是它中间的一段。对非叶子函数，
    // s 被传给被调函数（xf_N(s + k)），gcc 于是必须假设任意 s[i]
    // 都可能在某次调用后被改写，**每个槽都留在内存里**——每条字节码
    // 变成一次 load 加一次 store。
    //
    // 叶子函数（体内无 CALL）没有这个约束：整个函数执行期间，那块
    // 内存只能经 s 访问。__restrict 正是把这个事实告诉编译器，于是
    // 常量下标的槽可以被提升到寄存器，循环体回到原生整数运算。
    //
    // 用 restrict 而不是「给叶子分配局部数组」是因为后者会改变参数
    // 的所有权：调用方在 CALL 之后会 v_rel 掉实参槽，若叶子已把参数
    // 复制进自己的数组，字符串的引用计数就被两个所有者同时管理，
    // 必然出现 use-after-free 或泄漏。restrict 不动内存布局，
    // 所有权约定原封不动，只是让编译器多知道一件事。
    // ── 叶子函数标记的价值与边界 ──────────────────────────────
    //
    // 值栈是一个共享数组，函数的 s 是它中间的一段。对非叶子函数，
    // s 被传给被调函数（xf_N(s + k)），gcc 必须假设任意 s[i] 都可能
    // 在某次调用后被改写。叶子函数（体内无 CALL）没有这个约束，
    // 加 __restrict 能让优化器少些顾虑。
    //
    // 但实测这**没能带来可测量的加速**（spec/07 §3.9 有数据）：
    // 瓶颈不在别名分析，而在「用数组下标模拟栈」这个模型本身——
    // 每一层的值都要在内存里存在确定的槽位上，所以每次操作都是一次
    // 16 字节 load 加一次 store。手工用同样的 V 结构写纯 C 循环，
    // 速度只比生成的代码快 12%，可见生成质量不是主要矛盾。
    //
    // 保留 leaf 标记是为了让生成的 C 代码可读（注释里能看出函数性质），
    // 也为将来做寄存器分配留一个判断依据。不保留无效的 __restrict，
    // 避免读者误以为它有效。
    if (leaf) {
        fprintf(e->f, "static V xf_%d(V* s) {   /* leaf */\n", fi);
    } else {
        fprintf(e->f, "static V xf_%d(V* s) {\n", fi);
    }
    // 局部变量槽：参数在前，其余预填 null
    fprintf(e->f, "    for (int i = %d; i < %d; i++) s[i].t = XV_NULL;\n",
            fn->param_count, fn->local_count);

    codegen_body(g, fi);

    fprintf(e->f, "}\n");
}

// 生成函数体指令。返回实际使用的临时值数。
static int codegen_body(Cg* g, int fi) {
    Bytecode* bc = g->bc;
    FuncProto* fn = &bc->funcs[fi];
    Emit* e = g->e;

    // 计算栈深。失败说明字节码栈不平衡 —— 这是编译器 bug，
    // 必须明确报错，不能猜。
    int* depth = x_alloc((size_t)fn->code_len * sizeof(int));
    if (!compute_stack_depths(bc, fn, depth)) {
        free(depth);
        diag_error(fn->pos,
                   "内部错误：函数 `%s` 的字节码栈不平衡，无法生成代码",
                   fn->name);
        diag_note("这通常是超指令合并或跳转重映射的 bug，不是用户代码的问题。");
        diag_help("请把这个源文件报告为编译器 bug。用 `x disasm` 查看字节码，"
                  "并用 X_NO_SUPER=1 试一次以缩小范围。");
        fprintf(e->f, "    return v_null();\n");
        return 0;
    }

    fprintf(e->f, "    V t;\n    (void)t;\n");

    for (int pc = 0; pc < fn->code_len; pc++) {
        Insn* in = &bc->code[fn->code_start + pc];
        SrcPos pos = bc->pos[fn->code_start + pc];
        int d = depth[pc];
        if (d < 0) {
            // 不可达指令（死代码）：保留 label 但生成 nop，
            // 避免 goto 到一个不存在的标签。
            fprintf(e->f, "L%d_%d: ;\n", fi, pc);
            continue;
        }

        fprintf(e->f, "L%d_%d: ;\n", fi, pc);

        // 位置更新：只在可能出错或不改动控制流的地方插，
        // 避免每条指令都写两个全局变量（那会成为性能杀手）。
        if (pos.file) {
            fprintf(e->f, "    g_line = %d; g_col = %d;\n", pos.line, pos.col);
        }

        switch ((OpCode)in->op) {
            // ---------- 压栈类 ----------
            case BC_PUSH_I64:
                fprintf(e->f, "    s[%d] = v_int(%lldLL);\n", d,
                        (long long)bc->consts[in->a].i);
                break;
            case BC_PUSH_F64:
                fprintf(e->f, "    s[%d] = v_flt(%a);\n", d, bc->consts[in->a].f);
                break;
            case BC_PUSH_STR: {
                Constant* c = &bc->consts[in->a];
                fprintf(e->f, "    s[%d] = v_str(xs_new(\"", d);
                for (size_t i = 0; i < c->str.len; i++) {
                    unsigned char ch = (unsigned char)c->str.s[i];
                    if (ch == '"')       fputs("\\\"", e->f);
                    else if (ch == '\\') fputs("\\\\", e->f);
                    else if (ch == '\n') fputs("\\n", e->f);
                    else if (ch == '\r') fputs("\\r", e->f);
                    else if (ch == '\t') fputs("\\t", e->f);
                    else if (ch < 0x20 || ch >= 0x7F) fprintf(e->f, "\\%03o", ch);
                    else fputc((int)ch, e->f);
                }
                fprintf(e->f, "\", %zu));\n", c->str.len);
                break;
            }
            case BC_PUSH_TRUE:  fprintf(e->f, "    s[%d] = v_bool(true);\n", d); break;
            case BC_PUSH_FALSE: fprintf(e->f, "    s[%d] = v_bool(false);\n", d); break;
            case BC_PUSH_NULL:  fprintf(e->f, "    s[%d] = v_null();\n", d); break;

            // ---------- 栈操作 ----------
            case BC_POP:
                fprintf(e->f, "    v_rel(&s[%d]);\n", d - 1);
                break;
            case BC_DUP:
                // depth[pc] 是**执行前**的栈深，所以栈顶值在 d-1，
                // 复制到 d（执行后栈顶）。写 d-1/d-2 会把值原地复制
                // 一次再覆盖，等于什么都没做。
                fprintf(e->f,
                        "    s[%d] = s[%d];\n"
                        "    v_rtn(&s[%d]);\n",
                        d, d - 1, d);
                break;

            // ---------- 局部变量 ----------
            case BC_LOAD_LOCAL:
                fprintf(e->f,
                        "    s[%d] = s[%d];\n"
                        "    v_rtn(&s[%d]);\n",
                        d, in->a, d);
                break;
            case BC_STORE_LOCAL:
                fprintf(e->f,
                        "    { V v_ = s[%d]; v_rel(&s[%d]); s[%d] = v_; }\n",
                        d - 1, in->a, in->a);
                break;

            // ---------- 整数算术 ----------
            case BC_ADD_I64:
                fprintf(e->f,
                        "    { int64_t r_; if (ovf_add(s[%d].i, s[%d].i, &r_)) ovf_err(\"+\");\n"
                        "      s[%d].i = r_; }\n", d - 2, d - 1, d - 2);
                break;
            case BC_SUB_I64:
                fprintf(e->f,
                        "    { int64_t r_; if (ovf_sub(s[%d].i, s[%d].i, &r_)) ovf_err(\"-\");\n"
                        "      s[%d].i = r_; }\n", d - 2, d - 1, d - 2);
                break;
            case BC_MUL_I64:
                fprintf(e->f,
                        "    { int64_t r_; if (ovf_mul(s[%d].i, s[%d].i, &r_)) ovf_err(\"*\");\n"
                        "      s[%d].i = r_; }\n", d - 2, d - 1, d - 2);
                break;
            case BC_DIV_I64:
                fprintf(e->f,
                        "    if (s[%d].i == 0) rt_err(\"整数除以零\");\n"
                        "    if (s[%d].i == INT64_MIN && s[%d].i == -1) ovf_err(\"/\");\n"
                        "    s[%d].i = s[%d].i / s[%d].i;\n",
                        d - 1, d - 2, d - 1, d - 2, d - 2, d - 1);
                break;
            case BC_MOD_I64:
                fprintf(e->f,
                        "    if (s[%d].i == 0) rt_err(\"整数取模的除数为零\");\n"
                        "    s[%d].i = (s[%d].i == INT64_MIN && s[%d].i == -1) ? 0 : (s[%d].i %% s[%d].i);\n",
                        d - 1, d - 2, d - 2, d - 1, d - 2, d - 1);
                break;
            case BC_NEG_I64:
                fprintf(e->f,
                        "    if (s[%d].i == INT64_MIN) ovf_err(\"-\");\n"
                        "    s[%d].i = -s[%d].i;\n", d - 1, d - 1, d - 1);
                break;

            // ---------- 浮点算术 ----------
            case BC_ADD_F64:
                fprintf(e->f, "    s[%d].f += s[%d].f;\n", d - 2, d - 1); break;
            case BC_SUB_F64:
                fprintf(e->f, "    s[%d].f -= s[%d].f;\n", d - 2, d - 1); break;
            case BC_MUL_F64:
                fprintf(e->f, "    s[%d].f *= s[%d].f;\n", d - 2, d - 1); break;
            case BC_DIV_F64:
                fprintf(e->f, "    s[%d].f /= s[%d].f;\n", d - 2, d - 1); break;
            case BC_NEG_F64:
                fprintf(e->f, "    s[%d].f = -s[%d].f;\n", d - 1, d - 1); break;

            // ---------- 数值双分派 ----------
            case BC_ADD_NUM: case BC_SUB_NUM: case BC_MUL_NUM: {
                const char* opn = (in->op == BC_ADD_NUM) ? "+"
                                : (in->op == BC_SUB_NUM) ? "-" : "*";
                fprintf(e->f,
                    "    if (s[%d].t == XV_INT && s[%d].t == XV_INT) {\n"
                    "        int64_t r_; if (ovf_%s(s[%d].i, s[%d].i, &r_)) ovf_err(\"%s\");\n"
                    "        s[%d].i = r_;\n"
                    "    } else {\n"
                    "        double a_, b_;\n"
                    "        if (!v_as_flt(&s[%d], &a_) || !v_as_flt(&s[%d], &b_))\n"
                    "            rt_err(\"运算符需要数值\");\n"
                    "        s[%d].t = XV_FLOAT; s[%d].f = a_ %s b_;\n"
                    "    }\n",
                    d - 2, d - 1,
                    (in->op == BC_ADD_NUM) ? "add" : (in->op == BC_SUB_NUM) ? "sub" : "mul",
                    d - 2, d - 1, opn, d - 2,
                    d - 2, d - 1, d - 2, d - 2, opn);
                break;
            }
            case BC_DIV_NUM:
                fprintf(e->f,
                    "    if (s[%d].t == XV_INT && s[%d].t == XV_INT) {\n"
                    "        if (s[%d].i == 0) rt_err(\"整数除以零\");\n"
                    "        if (s[%d].i == INT64_MIN && s[%d].i == -1) ovf_err(\"/\");\n"
                    "        s[%d].i = s[%d].i / s[%d].i;\n"
                    "    } else {\n"
                    "        double a_, b_;\n"
                    "        if (!v_as_flt(&s[%d], &a_) || !v_as_flt(&s[%d], &b_))\n"
                    "            rt_err(\"`/` 需要数值\");\n"
                    "        s[%d].t = XV_FLOAT; s[%d].f = a_ / b_;\n"
                    "    }\n",
                    d - 2, d - 1, d - 1, d - 2, d - 1, d - 2, d - 2, d - 1,
                    d - 2, d - 1, d - 2, d - 2);
                break;

            // ---------- 全动态 + ----------
            //
            // 这里的输出拆成多次 fprintf 而不是一个超长格式串。
            // 理由：这类代码块有十几个栈槽下标要填，一个格式串里塞
            // 十几个 %d 时，参数顺序写错编译器也只会警告「参数个数不符」，
            // 而一旦个数恰好对上、顺序错了，就是**生成的代码行为错误**
            // 而生成器本身不报错——这种 bug 只能靠读生成物发现。
            // 拆开写每个 %d 都能就地看见它填的是什么。
            case BC_ADD_DYN: {
                int A = d - 2, B = d - 1;
                fprintf(e->f,
                    "    if (s[%d].t == XV_INT && s[%d].t == XV_INT) {\n", A, B);
                fprintf(e->f,
                    "        int64_t r_; if (ovf_add(s[%d].i, s[%d].i, &r_)) ovf_err(\"+\");\n",
                    A, B);
                fprintf(e->f,
                    "        s[%d].t = XV_INT; s[%d].i = r_;\n", A, A);
                fprintf(e->f,
                    "    } else if ((s[%d].t == XV_INT || s[%d].t == XV_FLOAT) &&\n", A, A);
                fprintf(e->f,
                    "               (s[%d].t == XV_INT || s[%d].t == XV_FLOAT)) {\n", B, B);
                fprintf(e->f,
                    "        double a_, b_; v_as_flt(&s[%d], &a_); v_as_flt(&s[%d], &b_);\n",
                    A, B);
                fprintf(e->f,
                    "        s[%d].t = XV_FLOAT; s[%d].f = a_ + b_;\n", A, A);
                fprintf(e->f,
                    "    } else {\n");
                fprintf(e->f,
                    "        XS* a_ = v_str_of(s[%d]); XS* b_ = v_str_of(s[%d]);\n", A, B);
                fprintf(e->f,
                    "        XS* r_ = xs_cat(a_, b_); xs_rel(a_); xs_rel(b_);\n");
                fprintf(e->f,
                    "        v_rel(&s[%d]); v_rel(&s[%d]);\n", A, B);
                fprintf(e->f,
                    "        s[%d].t = XV_STRING; s[%d].s = r_;\n", A, A);
                fprintf(e->f, "    }\n");
                break;
            }

            // ---------- 相等 / 不等 ----------
            case BC_EQ: case BC_NE: {
                bool is_eq = (in->op == BC_EQ);
                int A = d - 2, B = d - 1;
                fprintf(e->f, "    { bool eq_;\n");
                fprintf(e->f,
                    "      if (s[%d].t == XV_STRING && s[%d].t == XV_STRING)\n", A, B);
                fprintf(e->f,
                    "          eq_ = (s[%d].s->len == s[%d].s->len) &&\n", A, B);
                fprintf(e->f,
                    "                (memcmp(s[%d].s->d, s[%d].s->d, s[%d].s->len) == 0);\n",
                    A, B, A);
                fprintf(e->f,
                    "      else if ((s[%d].t == XV_INT || s[%d].t == XV_FLOAT) &&\n", A, A);
                fprintf(e->f,
                    "               (s[%d].t == XV_INT || s[%d].t == XV_FLOAT)) {\n", B, B);
                fprintf(e->f,
                    "          double a_, b_; v_as_flt(&s[%d], &a_); v_as_flt(&s[%d], &b_);\n",
                    A, B);
                fprintf(e->f, "          eq_ = (a_ == b_);\n");
                fprintf(e->f, "      } else if (s[%d].t == s[%d].t) {\n", A, B);
                fprintf(e->f,
                    "          if (s[%d].t == XV_NULL) eq_ = true;\n", A);
                fprintf(e->f,
                    "          else if (s[%d].t == XV_BOOL) eq_ = (s[%d].b == s[%d].b);\n", A, A, B);
                // 数组按元素深度比较，与解释器逐条对应。
                // 落到 `eq_ = false` 会让 `a == a` 返回 false。
                fprintf(e->f,
                    "          else if (s[%d].t == XV_ARRAY) eq_ = v_eq(&s[%d], &s[%d]);\n", A, A, B);
                fprintf(e->f,
                    "          else if (s[%d].t == XV_STRUCT) eq_ = v_eq(&s[%d], &s[%d]);\n", A, A, B);
                // 类实例：身份比较。与解释器 BC_EQ 的 VT_CLASS 分支
                // 一致 —— 漏掉这一支的后果是 `a == a` 恒为 false，
                // 而这是最容易被信任的表达式。
                fprintf(e->f,
                    "          else if (s[%d].t == XV_CLASS) eq_ = v_eq(&s[%d], &s[%d]);\n", A, A, B);
                fprintf(e->f, "          else eq_ = false;\n");
                fprintf(e->f, "      } else eq_ = false;\n");
                fprintf(e->f, "      v_rel(&s[%d]); v_rel(&s[%d]);\n", A, B);
                fprintf(e->f,
                    "      s[%d].t = XV_BOOL; s[%d].b = %s; }\n",
                    A, A, is_eq ? "eq_" : "!eq_");
                break;
            }

            // ---------- 整数比较 ----------
            case BC_LT_I64: case BC_LE_I64: case BC_GT_I64: case BC_GE_I64: {
                const char* op = (in->op == BC_LT_I64) ? "<"
                               : (in->op == BC_LE_I64) ? "<="
                               : (in->op == BC_GT_I64) ? ">" : ">=";
                fprintf(e->f,
                    "    s[%d].t = XV_BOOL;\n"
                    "    s[%d].b = (s[%d].i %s s[%d].i);\n",
                    d - 2, d - 2, d - 2, op, d - 1);
                break;
            }
            // ---------- 浮点比较 ----------
            case BC_LT_F64: case BC_LE_F64: case BC_GT_F64: case BC_GE_F64: {
                const char* op = (in->op == BC_LT_F64) ? "<"
                               : (in->op == BC_LE_F64) ? "<="
                               : (in->op == BC_GT_F64) ? ">" : ">=";
                fprintf(e->f,
                    "    s[%d].t = XV_BOOL;\n"
                    "    s[%d].b = (s[%d].f %s s[%d].f);\n",
                    d - 2, d - 2, d - 2, op, d - 1);
                break;
            }
            // ---------- 数值比较 ----------
            case BC_LT_NUM: case BC_LE_NUM: case BC_GT_NUM: case BC_GE_NUM: {
                const char* op = (in->op == BC_LT_NUM) ? "<"
                               : (in->op == BC_LE_NUM) ? "<="
                               : (in->op == BC_GT_NUM) ? ">" : ">=";
                fprintf(e->f,
                    "    { double a_, b_;\n"
                    "      if (!v_as_flt(&s[%d], &a_) || !v_as_flt(&s[%d], &b_))\n"
                    "          rt_err(\"比较运算需要数值\");\n"
                    "      s[%d].t = XV_BOOL; s[%d].b = (a_ %s b_); }\n",
                    d - 2, d - 1, d - 2, d - 2, op);
                break;
            }

            // ---------- 位运算 ----------
            case BC_BAND_I64: fprintf(e->f, "    s[%d].i &= s[%d].i;\n", d - 2, d - 1); break;
            case BC_BOR_I64:  fprintf(e->f, "    s[%d].i |= s[%d].i;\n", d - 2, d - 1); break;
            case BC_BXOR_I64: fprintf(e->f, "    s[%d].i ^= s[%d].i;\n", d - 2, d - 1); break;
            case BC_BNOT_I64: fprintf(e->f, "    s[%d].i = ~s[%d].i;\n", d - 1, d - 1); break;
            case BC_SHL_I64:
                fprintf(e->f,
                    "    { int64_t b_ = s[%d].i; uint64_t a_ = (uint64_t)s[%d].i;\n"
                    "      if (b_ < 0 || b_ >= 64) s[%d].i = 0;\n"
                    "      else { uint64_t sh_ = a_ << b_;\n"
                    "             s[%d].i = (b_ > 0 && (sh_ >> b_) != a_) ? 0 : (int64_t)sh_; } }\n",
                    d - 1, d - 2, d - 2, d - 2);
                break;
            case BC_SHR_I64:
                fprintf(e->f,
                    "    { int64_t b_ = s[%d].i, a_ = s[%d].i;\n"
                    "      s[%d].i = (b_ < 0 || b_ >= 64) ? (a_ < 0 ? -1 : 0) : (a_ >> b_); }\n",
                    d - 1, d - 2, d - 2);
                break;

            // ---------- 逻辑 ----------
            case BC_NOT:
                fprintf(e->f,
                    "    { bool t_ = v_true(s[%d]); v_rel(&s[%d]);\n"
                    "      s[%d].t = XV_BOOL; s[%d].b = !t_; }\n",
                    d - 1, d - 1, d - 1, d - 1);
                break;
            case BC_TOBOOL:
                fprintf(e->f,
                    "    { bool t_ = v_true(s[%d]); v_rel(&s[%d]);\n"
                    "      s[%d].t = XV_BOOL; s[%d].b = t_; }\n",
                    d - 1, d - 1, d - 1, d - 1);
                break;

            // ---------- 类型转换 ----------
            case BC_I64_TO_F64:
                fprintf(e->f, "    s[%d].f = (double)s[%d].i; s[%d].t = XV_FLOAT;\n",
                        d - 1, d - 1, d - 1);
                break;
            case BC_TOINT:
                fprintf(e->f,
                    "    switch (s[%d].t) {\n"
                    "      case XV_INT: break;\n"
                    "      case XV_FLOAT:\n"
                    "        if (s[%d].f != s[%d].f || s[%d].f > 9.3e18 || s[%d].f < -9.3e18)\n"
                    "            rt_err(\"`toInt` 无法转换 NaN 或 Infinity\");\n"
                    "        s[%d].i = (int64_t)s[%d].f; s[%d].t = XV_INT; break;\n"
                    "      case XV_BOOL: s[%d].i = s[%d].b ? 1 : 0; s[%d].t = XV_INT; break;\n"
                    "      case XV_STRING: {\n"
                    "        char* end_ = 0; errno = 0;\n"
                    "        long long p_ = strtoll(s[%d].s->d, &end_, 10);\n"
                    "        if (end_ == s[%d].s->d || *end_ != 0 || errno == ERANGE)\n"
                    "            rt_err(\"`toInt` 无法把字符串解析为整数\");\n"
                    "        xs_rel(s[%d].s); s[%d].i = (int64_t)p_; s[%d].t = XV_INT; break; }\n"
                    "      default: rt_err(\"`toInt` 无法转换 null\");\n"
                    "    }\n",
                    d - 1,
                    d - 1, d - 1, d - 1, d - 1,
                    d - 1, d - 1, d - 1,
                    d - 1, d - 1, d - 1,
                    d - 1, d - 1,
                    d - 1, d - 1, d - 1);
                break;
            case BC_TOFLOAT:
                fprintf(e->f,
                    "    switch (s[%d].t) {\n"
                    "      case XV_FLOAT: break;\n"
                    "      case XV_INT: s[%d].f = (double)s[%d].i; s[%d].t = XV_FLOAT; break;\n"
                    "      case XV_BOOL: s[%d].f = s[%d].b ? 1.0 : 0.0; s[%d].t = XV_FLOAT; break;\n"
                    "      case XV_STRING: {\n"
                    "        char* end_ = 0;\n"
                    "        double p_ = strtod(s[%d].s->d, &end_);\n"
                    "        if (end_ == s[%d].s->d || *end_ != 0)\n"
                    "            rt_err(\"`toFloat` 无法把字符串解析为浮点数\");\n"
                    "        xs_rel(s[%d].s); s[%d].f = p_; s[%d].t = XV_FLOAT; break; }\n"
                    "      default: rt_err(\"`toFloat` 无法转换 null\");\n"
                    "    }\n",
                    d - 1,
                    d - 1, d - 1, d - 1,
                    d - 1, d - 1, d - 1,
                    d - 1, d - 1,
                    d - 1, d - 1, d - 1);
                break;

            // ---------- 转字符串 ----------
            case BC_TOSTRING_I64:
                fprintf(e->f, "    s[%d].s = xs_int(s[%d].i); s[%d].t = XV_STRING;\n",
                        d - 1, d - 1, d - 1);
                break;
            case BC_TOSTRING_F64:
                fprintf(e->f, "    s[%d].s = xs_flt(s[%d].f); s[%d].t = XV_STRING;\n",
                        d - 1, d - 1, d - 1);
                break;
            case BC_TOSTRING_BOOL:
                fprintf(e->f,
                        "    s[%d].s = s[%d].b ? xs_new(\"true\",4) : xs_new(\"false\",5);\n"
                        "    s[%d].t = XV_STRING;\n",
                        d - 1, d - 1, d - 1);
                break;
            case BC_TOSTRING_NULL:
                fprintf(e->f, "    s[%d].s = xs_new(\"null\",4); s[%d].t = XV_STRING;\n",
                        d - 1, d - 1);
                break;
            case BC_TOSTRING_ANY:
                fprintf(e->f,
                        "    { XS* r_ = v_str_of(s[%d]); v_rel(&s[%d]);\n"
                        "      s[%d].s = r_; s[%d].t = XV_STRING; }\n",
                        d - 1, d - 1, d - 1, d - 1);
                break;

            // ---------- 字符串拼接 ----------
            case BC_CONCAT_STR:
                fprintf(e->f,
                        "    { XS* r_ = xs_cat(s[%d].s, s[%d].s);\n"
                        "      xs_rel(s[%d].s); xs_rel(s[%d].s);\n"
                        "      s[%d].s = r_; s[%d].t = XV_STRING; }\n",
                        d - 2, d - 1, d - 2, d - 1, d - 2, d - 2);
                break;
            case BC_CONCAT_ANY:
                fprintf(e->f,
                        "    { XS* a_ = v_str_of(s[%d]); XS* b_ = v_str_of(s[%d]);\n"
                        "      XS* r_ = xs_cat(a_, b_); xs_rel(a_); xs_rel(b_);\n"
                        "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                        "      s[%d].s = r_; s[%d].t = XV_STRING; }\n",
                        d - 2, d - 1, d - 2, d - 1, d - 2, d - 2);
                break;

            // ---------- 内建 ----------
            case BC_ABS_I64:
                fprintf(e->f,
                        "    if (s[%d].i == INT64_MIN) ovf_err(\"abs\");\n"
                        "    s[%d].i = s[%d].i < 0 ? -s[%d].i : s[%d].i;\n",
                        d - 1, d - 1, d - 1, d - 1, d - 1);
                break;
            case BC_ABS_F64:
                fprintf(e->f, "    s[%d].f = fabs(s[%d].f);\n", d - 1, d - 1);
                break;
            case BC_LEN:
                // String 与 Array 都返回长度。分支结构必须与解释器
                // 完全一致（同样的报错、同样的行为），否则一致性测试会挂。
                //
                // 字符串数码点不数字节：len("张三") == 2，与 strAt /
                // strSub / strFind 的码点索引同一套坐标系。解释器侧
                // 的同一处注释记录了这条 bug 的完整后果。
                fprintf(e->f,
                        "    if (s[%d].t == XV_STRING) { int64_t n_ = (int64_t)xu_cplen(s[%d].s->d, s[%d].s->len); xs_rel(s[%d].s);\n"
                        "      s[%d].t = XV_INT; s[%d].i = n_; }\n"
                        "    else if (s[%d].t == XV_ARRAY) { int64_t n_ = (int64_t)s[%d].a->len; xa_rel(s[%d].a);\n"
                        "      s[%d].t = XV_INT; s[%d].i = n_; }\n"
                        "    else rt_err(\"`len` 只接受 String 或 Array\");\n",
                        d - 1, d - 1, d - 1, d - 1, d - 1,
                        d - 1, d - 1, d - 1, d - 1, d - 1, d - 1);
                break;

            // ---------- 数组 ----------
            case BC_NEW_ARRAY:
                // 栈上从下到上是 [长度, 填充值]；填充值在 d-1，长度在 d-2。
                fprintf(e->f,
                        "    if (s[%d].t != XV_INT) rt_err(\"数组长度必须是 Int\");\n"
                        "    if (s[%d].i < 0) rt_err(\"数组长度不能为负数\");\n"
                        "    if (s[%d].i > 100000000) rt_err(\"数组长度超出上限（1 亿）\");\n"
                        "    { XA* a_ = xa_new((size_t)s[%d].i, s[%d]);\n"
                        "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                        "      s[%d].t = XV_ARRAY; s[%d].a = a_; }\n",
                        d - 2, d - 2, d - 2, d - 2, d - 1,
                        d - 1, d - 2,
                        d - 2, d - 2);
                break;

            case BC_INDEX_GET:
                // 容器在 d-2，下标在 d-1。结果放回 d-2。
                //
                // 与解释器一样「读不消耗」：容器的引用是 LOAD 借出来的，
                // 读完之后 sp 下移自然失效，不能在这里 release。
                fprintf(e->f,
                        "    if (s[%d].t != XV_ARRAY) rt_err(\"只能用 [] 索引数组\");\n"
                        "    if (s[%d].t != XV_INT) rt_err(\"数组下标必须是 Int\");\n"
                        "    if (s[%d].i < 0 || (size_t)s[%d].i >= s[%d].a->len) rt_err(\"数组下标越界\");\n"
                        "    { V g_ = s[%d].a->d[s[%d].i]; v_rtn(&g_);\n"
                        "      s[%d] = g_; }\n",
                        d - 2, d - 1, d - 1, d - 1, d - 2,
                        d - 2, d - 1,
                        d - 2);
                break;

            case BC_INDEX_SET:
                // 栈上：[容器(d-3), 下标(d-2), 值(d-1)]
                // 结束后容器留在 d-3，净效果 -2。
                //
                // 先把数组指针和下标取到局部变量，再动值。直接写
                // `v_rel(&s[d-3].a->d[s[d-2].i])` 是错的：一旦值槽
                // 与容器槽有重叠（或将来某次优化重排），拿到的就不是
                // 原本那个元素。局部变量让求值顺序显式且安全。
                fprintf(e->f,
                        "    if (s[%d].t != XV_ARRAY) rt_err(\"只能用 [] 索引数组\");\n"
                        "    if (s[%d].t != XV_INT) rt_err(\"数组下标必须是 Int\");\n"
                        "    { XA* a_ = s[%d].a; int64_t ix_ = s[%d].i; V v_ = s[%d];\n"
                        "      if (ix_ < 0 || (size_t)ix_ >= a_->len) rt_err(\"数组下标越界\");\n"
                        "      v_rel(&a_->d[ix_]); a_->d[ix_] = v_; }\n",
                        d - 3, d - 2,
                        d - 3, d - 2, d - 1);
                break;

            // ---------- 结构体 ----------
            case BC_NEW_STRUCT: {
                // 栈上从下到上：按声明顺序的 N 个字段值，最后压的（最后一个
                // 字段）在最顶上。结果写到 d-N。
                int cnt = in->b;
                int base = d - cnt;
                fprintf(e->f,
                        "    { XO* o_ = xo_new(%d, xo_tn[%d], xo_fns[%d], xo_fc[%d]);\n",
                        (int)in->a, (int)in->a, (int)in->a, (int)in->a);
                // 从后往前搬：栈顶对应最后一个字段。
                // 所有权直接转给对象，不 retain 也不 release ——
                // 与解释器的 BC_NEW_STRUCT 完全同一条规则。
                for (int i = cnt - 1; i >= 0; i--) {
                    fprintf(e->f, "      o_->f[%d] = s[%d];\n", i, base + i);
                }
                fprintf(e->f, "      s[%d] = v_obj(o_); }\n", base);
                break;
            }

            case BC_FIELD_GET:
                // 对象在 d-1，结果写回 d-1（净栈效应 0）。
                //
                // 与解释器同一条规则：读不消耗宿主，字段值要 retain
                // （栈上那份是独立持有）。宿主的引用随 sp 下移自然失效。
                //
                // 类实例共用这条指令 —— 偏移算法完全一样（父类在前、
                // 本类在后），只有宿主类型不同，所以这里分成两支。
                fprintf(e->f,
                        "    if (s[%d].t == XV_CLASS) {\n"
                        "        if (%d >= s[%d].k->fc) rt_err(\"类字段下标越界\");\n"
                        "        { V g_ = s[%d].k->f[%d]; v_rtn(&g_); s[%d] = g_; }\n"
                        "    } else if (s[%d].t == XV_STRUCT) {\n"
                        "        if (%d >= s[%d].o->fc) rt_err(\"结构体字段下标越界\");\n"
                        "        { V g_ = s[%d].o->f[%d]; v_rtn(&g_); s[%d] = g_; }\n"
                        "    } else rt_err(\"只能用 `.` 访问结构体或类的字段\");\n",
                        d - 1, (int)in->b, d - 1,
                        d - 1, (int)in->b, d - 1,
                        d - 1, (int)in->b, d - 1,
                        d - 1, (int)in->b, d - 1);
                break;

            // ---------- 类 ----------
            case BC_NEW_CLASS: {
                // a = 类索引，b = 实参个数（不含 self）
                //
                // ── 与解释器的分工差异 ──────────────────────────
                //
                // 解释器在 BC_RETURN 里专门处理「构造函数返回」：丢弃
                // 返回值、改压暂存的实例、清理 ctor 帧留下的实参副本。
                //
                // AOT 不需要这套机制 —— 生成期就能看到 xf_N 是一个
                // 普通 C 函数，它的返回值由 BC_RETURN 生成的代码负责
                // （那条代码会释放 0..d-1 并把栈顶值 return 出来）。
                // 生成器只要保证「调用前后 s 的布局符合约定」即可。
                int ci   = in->a;
                int argc = in->b;
                int base = d - argc;
                int ctor = bc->classes[ci].ctor_fn;

                fprintf(e->f,
                        "    { XK* k_ = xk_new(%d, xc_tn[%d], xc_fns[%d], xc_fc[%d]);\n",
                        ci, ci, ci, ci);

                if (ctor >= 0) {
                    // 有构造函数：栈上已压好 arg0..argN-1，而形参要求
                    // 第 0 位是 self。栈不能从中间插入，所以整体上移一格。
                    //
                    // 上移必须在写 self 之前完成，否则 s[base] 会被覆盖，
                    // 而 arg0 正好要搬到 s[base+1] —— 顺序写反就是典型的
                    // 「第一个实参变成 self」。
                    for (int i = argc - 1; i >= 0; i--) {
                        fprintf(e->f, "      s[%d] = s[%d];\n", base + 1 + i, base + i);
                    }
                    // ── 为什么要 retain 再调 ────────────────────────
                    //
                    // xf_N 的 BC_RETURN 会释放它自己那一帧的**全部**槽位
                    // （0..d-1，含形参 self）。也就是说 self 那份引用在
                    // 构造函数返回时就被释放了。
                    //
                    // 如果只靠 s[base] 那一份，rc 会从 1 变成 0，实例在
                    // 构造返回的瞬间就被 free —— 下一行 v_inst(k_) 装的
                    // 是野指针。所以这里额外持有一份，函数返回后再把
                    // 栈上那份重新装上。
                    fprintf(e->f,
                            "      xk_retain(k_);\n"
                            "      s[%d] = v_inst(k_);\n"
                            "      { V r_ = xf_%d(s + %d);\n"
                            // 构造函数声明返回自身类型，但语言层面没有
                            // 返回值（编译器补 return null）。r_ 用不上。
                            "        v_rel(&r_);\n"
                            "      }\n"
                            "      s[%d] = v_inst(k_); }\n",
                            base, ctor, base, base);
                } else {
                    fprintf(e->f, "      s[%d] = v_inst(k_); }\n", base);
                }
                break;
            }

            case BC_FIELD_SET: {
                // 栈上 [对象(d-2), 值(d-1)]，结束后值留 d-2（净 -1）。
                //
                // 与解释器同一条规则：旧值 release，新值所有权转移给
                // 实例字段，**然后 retain 一份压回栈** —— 少了这一步
                // 栈上那份和字段那份会共用同一个引用计数，POP 时字段
                // 立刻悬空（Int 看不出来，String 直接段错误）。
                int fi = in->a;
                fprintf(e->f,
                        "    if (s[%d].t != XV_CLASS) rt_err(\"只能用 `.` 给类字段赋值\");\n"
                        "    if (%d >= s[%d].k->fc) rt_err(\"类字段下标越界\");\n"
                        "    { V v_ = s[%d];\n"
                        "      v_rel(&s[%d].k->f[%d]);\n"
                        "      s[%d].k->f[%d] = v_;\n"
                        "      v_rtn(&v_);\n"
                        "      v_rel(&s[%d]);\n"
                        "      s[%d] = v_; }\n",
                        d - 2, fi, d - 2,
                        d - 1,
                        d - 2, fi,
                        d - 2, fi,
                        d - 2,
                        d - 2);
                break;
            }

            case BC_VIRT_CALL: {
                // 虚调用 —— 多态的全部。
                //
                // 生成期只知道槽号，具体调哪个函数取决于**运行时**
                // 对象的实际类，所以这里生成一次查表加一次 switch。
                //
                // ── 为什么用 switch 而不是函数指针 ────────────────
                //
                // 各目标方法的实参个数不同（由 argc 决定），C 里没有
                // 一个统一的函数指针类型能表达；而 xf_N 的签名都是
                // `V xf_N(V*)`，参数从共享栈按基址偏移取 —— 这点与
                // BC_CALL 完全一致，所以 switch 分支里直接调 xf_N。
                //
                // 调用方不需要手动清理实参：BC_RETURN 生成的代码会
                // 释放被调函数自己的帧（base..d-1）。这里只把返回值
                // 写到 self 的位置，净栈效应 1 - argc 自然成立。
                int slot = in->a;
                int argc = in->b;
                int base = d - argc;

                fprintf(e->f,
                        "    if (s[%d].t != XV_CLASS) rt_err(\"虚调用的接收者不是类实例\");\n"
                        "    { int fi_ = xc_vcall(s[%d].k->cls, %d);\n"
                        "      if (fi_ < 0) rt_err(\"类没有实现该虚方法\");\n"
                        "      switch (fi_) {\n"
                        "        default: rt_err(\"内部错误：虚表索引非法\");\n",
                        base, base, slot);

                for (int f = 0; f < bc->func_count; f++) {
                    bool used = false;
                    for (int k = 0; k < bc->class_count && !used; k++) {
                        ClassType* ct = &bc->classes[k];
                        if (slot < ct->vtable_len && ct->vtable[slot] == f) used = true;
                    }
                    if (!used) continue;
                    fprintf(e->f,
                            "        case %d: s[%d] = xf_%d(s + %d); break;\n",
                            f, base, f, base);
                }
                fprintf(e->f, "      } }\n");
                break;
            }

            // ---------- 平台内建 ----------
            case BC_BUILTIN: {
                // 实参在 s[d-argc .. d-1]，返回值写到 s[d-argc]。
                //
                // ── 为什么先整理成 C 局部变量再调 ──────────────
                //
                // 直接写 `xgui_fill_rect(s[d-5].i, s[d-4].i, ...)` 表面上
                // 更短，但有两个真问题：
                //   · C 不规定实参求值顺序。这些槽位互不重叠所以目前
                //     没问题，但参数一多（borderRect 有 6 个）时，
                //     一旦将来做「结果写回 s 的同时读 s」的优化就会出事。
                //   · 参数来自 union 的 .i 成员，先落到具名变量上，
                //     类型错误时的错误信息才能指到具体哪个参数。
                //
                // 所以这里生成成「取参数 → 校验 → 调用」三段。
                int argc = in->b;
                int base = d - argc;

                switch ((XBuiltin)in->a) {
                    // ---- 无参数、无返回值 ----
                    case XBI_WIN_CLOSE:
                        fprintf(e->f, "    xgui_close(); s[%d] = v_null();\n", d);
                        break;
                    case XBI_WIN_BEGIN_FRAME:
                        fprintf(e->f, "    xgui_begin_frame(); s[%d] = v_null();\n", d);
                        break;
                    case XBI_WIN_END_FRAME:
                        fprintf(e->f, "    xgui_end_frame(); s[%d] = v_null();\n", d);
                        break;

                    // ---- 无参数、有返回值 ----
                    case XBI_WIN_SHOULD_CLOSE:
                        fprintf(e->f, "    s[%d] = v_bool(xgui_should_close() != 0);\n", d);
                        break;
                    case XBI_WIN_TEXT_HEIGHT:
                        fprintf(e->f, "    s[%d] = v_int(xgui_text_height());\n", d);
                        break;
                    case XBI_WIN_MOUSE_X:
                        fprintf(e->f, "    s[%d] = v_int(xgui_mouse_x());\n", d);
                        break;
                    case XBI_WIN_MOUSE_Y:
                        fprintf(e->f, "    s[%d] = v_int(xgui_mouse_y());\n", d);
                        break;
                    case XBI_WIN_TIME:
                        fprintf(e->f, "    s[%d] = v_flt(xgui_time());\n", d);
                        break;

                    // ---- winPollClick：无参，返回 Array 或 Null ----
                    case XBI_WIN_POLL_CLICK:
                        fprintf(e->f,
                                "    { int cx_ = 0, cy_ = 0, cb_ = 0;\n"
                                "      if (xgui_poll_click(&cx_, &cy_, &cb_)) {\n"
                                "        XA* a_ = xa_new(3, v_null());\n"
                                "        a_->d[0] = v_int(cx_);\n"
                                "        a_->d[1] = v_int(cy_);\n"
                                "        a_->d[2] = v_int(cb_);\n"
                                "        s[%d].t = XV_ARRAY; s[%d].a = a_;\n"
                                "      } else s[%d] = v_null(); }\n",
                                d, d, d);
                        break;

                    // ---- winOpen(title, w, h) ----
                    case XBI_WIN_OPEN:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_STRING) rt_err(\"winOpen 的第 1 个参数必须是 String\");\n"
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winOpen 的第 2、3 个参数必须是 Int\");\n"
                                "    if (!xgui_open(s[%d].s->d, (int)s[%d].i, (int)s[%d].i))\n"
                                "        rt_err(\"无法创建窗口（当前环境没有可用的图形界面）\");\n"
                                "    v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "    s[%d] = v_bool(true);\n",
                                base, base + 1, base + 2,
                                base, base + 1, base + 2,
                                base, base + 1, base + 2,
                                base);
                        break;

                    // ---- winFillRect(x,y,w,h,color) ----
                    case XBI_WIN_FILL_RECT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winFillRect 的 5 个参数都必须是 Int\");\n"
                                "    xgui_fill_rect((int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                   (int)s[%d].i, (XColor)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4,
                                base, base + 1, base + 2, base + 3, base + 4,
                                base);
                        break;

                    // ---- winBorderRect(x,y,w,h,thickness,color) ----
                    case XBI_WIN_BORDER_RECT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winBorderRect 的 6 个参数都必须是 Int\");\n"
                                "    xgui_border_rect((int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                     (int)s[%d].i, (int)s[%d].i, (XColor)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base);
                        break;

                    // ---- winText(x, y, text, color) ----
                    case XBI_WIN_TEXT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_STRING || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winText 的参数是 (Int, Int, String, Int)\");\n"
                                "    xgui_text((int)s[%d].i, (int)s[%d].i, s[%d].s->d,\n"
                                "              (XColor)s[%d].i);\n"
                                "    v_rel(&s[%d]); s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3,
                                base, base + 1, base + 2, base + 3,
                                base + 2, base);
                        break;

                    // ---- winTextWidth(text) ----
                    case XBI_WIN_TEXT_WIDTH:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_STRING)\n"
                                "        rt_err(\"winTextWidth 的参数必须是 String\");\n"
                                "    { int w_ = xgui_text_width(s[%d].s->d);\n"
                                "      v_rel(&s[%d]); s[%d] = v_int(w_); }\n",
                                base, base, base, base);
                        break;

                    // ---- winMouseDown(button) ----
                    case XBI_WIN_MOUSE_DOWN:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT)\n"
                                "        rt_err(\"winMouseDown 的参数必须是 Int\");\n"
                                "    s[%d] = v_bool(xgui_mouse_down((int)s[%d].i) != 0);\n",
                                base, base, base);
                        break;

                    // ---- winSleep(ms) ----
                    case XBI_WIN_SLEEP:
                        // 接受 Int 或 Float（见解释器同名分支的说明）。
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT && s[%d].t != XV_FLOAT)\n"
                                "        rt_err(\"winSleep 的参数必须是数值\");\n"
                                "    xgui_sleep((int)(s[%d].t == XV_INT ? (double)s[%d].i : s[%d].f));\n"
                                "    s[%d] = v_null();\n",
                                base, base, base, base, base, base);
                        break;

                    // ---- winScreenshot(path) ----
                    case XBI_WIN_SCREENSHOT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_STRING)\n"
                                "        rt_err(\"winScreenshot 的参数必须是 String\");\n"
                                "    if (!xgui_screenshot(s[%d].s->d))\n"
                                "        rt_err(\"截图失败：无法写入目标路径\");\n"
                                "    v_rel(&s[%d]); s[%d] = v_bool(true);\n",
                                base, base, base, base);
                        break;

                    // ============================================================
                    // 形状 / 样式（v0.17）
                    // ============================================================
                    //
                    // 这一批的生成代码比数组原语直白：没有所有权转移，
                    // 没有返回堆对象，只是「校验 Int 槽 → 调 xgui_*」。
                    // 所以每个 case 都是一行类型检查 + 一行调用。
                    //
                    // 参数从 s[base+0] 起按源码顺序排列，这一点与解释器
                    // 的**弹栈逆序**正好相反 —— 两侧写法不同不是疏漏，
                    // 而是各自的自然形式：解释器从栈顶弹，生成代码直接
                    // 索引槽位。改动时别把一侧的顺序照抄到另一侧。

                    // ---- winFillRound(x,y,w,h,radius,color) ----
                    case XBI_WIN_FILL_ROUND:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winFillRound 的 6 个参数都必须是 Int\");\n"
                                "    xgui_fill_round((int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                    (int)s[%d].i, (int)s[%d].i, (XColor)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base);
                        break;

                    // ---- winBorderRound(x,y,w,h,radius,thickness,color) ----
                    case XBI_WIN_BORDER_ROUND:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winBorderRound 的 7 个参数都必须是 Int\");\n"
                                "    xgui_border_round((int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                      (int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                      (XColor)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4, base + 5, base + 6,
                                base, base + 1, base + 2, base + 3, base + 4, base + 5, base + 6,
                                base);
                        break;

                    // ---- winEllipse(x,y,w,h,color) ----
                    case XBI_WIN_ELLIPSE:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winEllipse 的 5 个参数都必须是 Int\");\n"
                                "    xgui_ellipse((int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                 (int)s[%d].i, (XColor)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4,
                                base, base + 1, base + 2, base + 3, base + 4,
                                base);
                        break;

                    // ---- winPath(points: Array, color: Int) ----
                    //
                    // 这条比别的复杂：要把 X 的数组转成 C 的 int[]。
                    // 生成一个变长栈数组不现实（点数运行时才知道），
                    // 所以走 malloc。为了不把这段逻辑写进生成代码，
                    // 调用一个辅助函数 xb_win_path（见下面的 emit 头）。
                    case XBI_WIN_PATH:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_ARRAY)\n"
                                "        rt_err(\"winPath 的第 1 个参数必须是 Array\");\n"
                                "    if (s[%d].t != XV_INT)\n"
                                "        rt_err(\"winPath 的第 2 个参数必须是 Int（颜色）\");\n"
                                "    xb_win_path(&s[%d], (XColor)s[%d].i);\n"
                                "    v_rel(&s[%d]); s[%d] = v_null();\n",
                                base, base + 1,
                                base, base + 1,
                                base, base);
                        break;

                    // ---- winGradient(x,y,w,h,c1,c2,vertical: Bool) ----
                    case XBI_WIN_GRADIENT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winGradient 的前 6 个参数都必须是 Int\");\n"
                                "    if (s[%d].t != XV_BOOL && s[%d].t != XV_INT)\n"
                                "        rt_err(\"winGradient 的第 7 个参数必须是 Bool\");\n"
                                "    xgui_gradient((int)s[%d].i, (int)s[%d].i, (int)s[%d].i, (int)s[%d].i,\n"
                                "                  (XColor)s[%d].i, (XColor)s[%d].i,\n"
                                "                  s[%d].t == XV_BOOL ? (s[%d].b ? 1 : 0) : (int)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base + 6, base + 6,
                                base, base + 1, base + 2, base + 3, base + 4, base + 5,
                                base + 6, base + 6, base + 6,
                                base);
                        break;

                    // ---- winPushTransform(pivotX, pivotY, rotDeg, scale) ----
                    //
                    // 四个参数都接受 Int 或 Float：`winPushTransform(100, 100, 45, 1)`
                    // 是最自然的写法，而 45 和 1 是 Int。强制调用方写 45.0
                    // 会让这个 API 变成「必须先查签名」的东西。
                    //
                    // 生成一个内联的取值表达式：XV_INT 取 .i 转 double，
                    // XV_FLOAT 取 .f。写成宏会更短，但内联能让生成的 C
                    // 自己可读 —— 出问题时人要看的就是这段。
                    case XBI_WIN_PUSH_TRANSFORM:
                        fprintf(e->f,
                                "    xgui_push_transform(xb_num(&s[%d], \"winPushTransform\", 1),\n"
                                "                        xb_num(&s[%d], \"winPushTransform\", 2),\n"
                                "                        xb_num(&s[%d], \"winPushTransform\", 3),\n"
                                "                        xb_num(&s[%d], \"winPushTransform\", 4));\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3, base);
                        break;

                    // ---- winPopTransform() ----
                    case XBI_WIN_POP_TRANSFORM:
                        fprintf(e->f,
                                "    xgui_pop_transform();\n"
                                "    s[%d] = v_null();\n",
                                base);
                        break;

                    // ---- winAlpha(a) ----
                    case XBI_WIN_ALPHA:
                        fprintf(e->f,
                                "    xgui_set_alpha(xb_num(&s[%d], \"winAlpha\", 1));\n"
                                "    s[%d] = v_null();\n",
                                base, base);
                        break;

                    // ---- winPopAlpha() ----
                    case XBI_WIN_POP_ALPHA:
                        fprintf(e->f,
                                "    xgui_pop_alpha();\n"
                                "    s[%d] = v_null();\n",
                                base);
                        break;

                    // ---- winClip(x,y,w,h) ----
                    case XBI_WIN_CLIP:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_INT || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winClip 的 4 个参数都必须是 Int\");\n"
                                "    xgui_clip((int)s[%d].i, (int)s[%d].i, (int)s[%d].i, (int)s[%d].i);\n"
                                "    s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3,
                                base, base + 1, base + 2, base + 3,
                                base);
                        break;

                    // ---- winUnclip() ----
                    case XBI_WIN_UNCLIP:
                        fprintf(e->f,
                                "    xgui_unclip();\n"
                                "    s[%d] = v_null();\n",
                                base);
                        break;

                    // ---- winTextRot(x,y,text,color,rotDeg) ----
                    case XBI_WIN_TEXT_ROT:
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT || s[%d].t != XV_INT\n"
                                "        || s[%d].t != XV_STRING || s[%d].t != XV_INT)\n"
                                "        rt_err(\"winTextRot 的参数是 (Int, Int, String, Int, 数值)\");\n"
                                "    xgui_text_rot((int)s[%d].i, (int)s[%d].i, s[%d].s->d,\n"
                                "                  (XColor)s[%d].i,\n"
                                "                  xb_num(&s[%d], \"winTextRot\", 5));\n"
                                "    v_rel(&s[%d]); s[%d] = v_null();\n",
                                base, base + 1, base + 2, base + 3,
                                base, base + 1, base + 2, base + 3,
                                base + 4,
                                base + 2, base);
                        break;

                    case XBI_NONE:
                    case XBI__WIN_END:   // 窗口段边界哨兵，不可执行
                    case XBI_COUNT:
                        fprintf(e->f,
                                "    rt_err(\"内部错误：未知的平台内建编号 %d\");\n",
                                (int)in->a);
                        break;

                    // ============================================================
                    // 数组原语
                    // ============================================================
                    //
                    // 每个 case 都按「取参数 → 校验 → 调用辅助 → 写回 s[d]」
                    // 四步生成。不复用大段内联 C，因为参数个数与语义
                    // 各不相同，而辅助函数（xb_*）已经承载了全部逻辑 ——
                    // 这里只负责把 s[] 槽位接到辅助函数的入参上。
                    //
                    // base 是第一个实参的槽位号，槽位顺序与源码参数顺序
                    // 一致（编译器从左到右压栈）。返回值写到 s[base]，
                    // 因为 d == base + argc 时栈顶正好是最后一个实参之后
                    // 的位置，直接复用 base 槽即可。

                    case XBI_ARR_PUSH:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrPush\", 1);\n"
                                "      xa_push(a_, s[%d]);\n"
                                "      s[%d] = v_int((int64_t)a_->len); }\n",
                                base, base + 1, base);
                        break;

                    case XBI_ARR_POP:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrPop\", 1);\n"
                                "      if (a_->len == 0) s[%d] = v_null();\n"
                                "      else { s[%d] = a_->d[--a_->len]; }\n"
                                "      xa_rel(a_); }\n",
                                base, base, base);
                        break;

                    case XBI_ARR_INSERT:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrInsert\", 1);\n"
                                "      int64_t ix_ = xb_int(&s[%d], \"arrInsert\", 2);\n"
                                "      if (ix_ < 0 || (size_t)ix_ > a_->len)\n"
                                "          rt_err(\"`arrInsert` 的下标越界\");\n"
                                "      xa_reserve(a_, a_->len + 1);\n"
                                "      memmove(&a_->d[ix_ + 1], &a_->d[ix_], (a_->len - (size_t)ix_) * sizeof(V));\n"
                                "      a_->d[ix_] = s[%d];\n"
                                "      a_->len++;\n"
                                "      s[%d] = v_int((int64_t)a_->len);\n"
                                "      xa_rel(a_); }\n",
                                base, base + 1, base + 2, base);
                        break;

                    case XBI_ARR_REMOVE:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrRemove\", 1);\n"
                                "      int64_t ix_ = xb_int(&s[%d], \"arrRemove\", 2);\n"
                                "      if (ix_ < 0 || (size_t)ix_ >= a_->len)\n"
                                "          rt_err(\"`arrRemove` 的下标越界\");\n"
                                "      V out_ = a_->d[ix_];\n"
                                "      memmove(&a_->d[ix_], &a_->d[ix_ + 1], (a_->len - (size_t)ix_ - 1) * sizeof(V));\n"
                                "      a_->len--;\n"
                                "      s[%d] = out_;\n"
                                "      xa_rel(a_); }\n",
                                base, base + 1, base);
                        break;

                    case XBI_ARR_SLICE:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrSlice\", 1);\n"
                                "      int64_t st_ = xb_int(&s[%d], \"arrSlice\", 2);\n"
                                "      int64_t en_ = xb_int(&s[%d], \"arrSlice\", 3);\n"
                                "      size_t n_ = a_->len;\n"
                                "      if (st_ < 0) st_ = 0; if (en_ < 0) en_ = 0;\n"
                                "      if ((size_t)st_ > n_) st_ = (int64_t)n_;\n"
                                "      if ((size_t)en_ > n_) en_ = (int64_t)n_;\n"
                                "      if (en_ < st_) en_ = st_;\n"
                                "      XA* o_ = xa_new(0, v_null());\n"
                                "      xa_reserve(o_, (size_t)(en_ - st_));\n"
                                "      for (int64_t i_ = st_; i_ < en_; i_++) {\n"
                                "          V el_ = a_->d[i_]; v_rtn(&el_); o_->d[o_->len++] = el_;\n"
                                "      }\n"
                                "      s[%d].t = XV_ARRAY; s[%d].a = o_;\n"
                                "      xa_rel(a_); }\n",
                                base, base + 1, base + 2, base, base);
                        break;

                    case XBI_ARR_CONCAT:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrConcat\", 1);\n"
                                "      XA* b_ = xb_arr(&s[%d], \"arrConcat\", 2);\n"
                                "      XA* o_ = xa_new(0, v_null());\n"
                                "      xa_reserve(o_, a_->len + b_->len);\n"
                                "      for (size_t i_ = 0; i_ < a_->len; i_++) { V x_ = a_->d[i_]; v_rtn(&x_); o_->d[o_->len++] = x_; }\n"
                                "      for (size_t i_ = 0; i_ < b_->len; i_++) { V x_ = b_->d[i_]; v_rtn(&x_); o_->d[o_->len++] = x_; }\n"
                                "      s[%d].t = XV_ARRAY; s[%d].a = o_;\n"
                                "      xa_rel(a_); xa_rel(b_); }\n",
                                base, base + 1, base, base);
                        break;

                    case XBI_ARR_REVERSE:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrReverse\", 1);\n"
                                "      size_t i_ = 0, j_ = a_->len ? a_->len - 1 : 0;\n"
                                "      while (i_ < j_) { V t_ = a_->d[i_]; a_->d[i_] = a_->d[j_]; a_->d[j_] = t_; i_++; j_--; }\n"
                                "      s[%d] = v_null();\n"
                                "      xa_rel(a_); }\n",
                                base, base);
                        break;

                    case XBI_ARR_INDEX_OF:
                    case XBI_ARR_CONTAINS:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"%s\", 1);\n"
                                "      int64_t f_ = -1;\n"
                                "      for (size_t i_ = 0; i_ < a_->len; i_++)\n"
                                "          if (v_eq(&a_->d[i_], &s[%d])) { f_ = (int64_t)i_; break; }\n"
                                "      s[%d] = %s;\n"
                                "      xa_rel(a_); }\n",
                                base, (int)in->a == XBI_ARR_CONTAINS ? "arrContains" : "arrIndexOf",
                                base + 1,
                                base,
                                (int)in->a == XBI_ARR_CONTAINS ? "v_bool(f_ >= 0)" : "v_int(f_)");
                        break;

                    case XBI_ARR_FILL:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrFill\", 1);\n"
                                "      for (size_t i_ = 0; i_ < a_->len; i_++) {\n"
                                "          v_rel(&a_->d[i_]);\n"
                                "          a_->d[i_] = s[%d]; v_rtn(&a_->d[i_]);\n"
                                "      }\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_null();\n"
                                "      xa_rel(a_); }\n",
                                base, base + 1, base + 1, base);
                        break;

                    case XBI_ARR_SORT:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrSort\", 1);\n"
                                "      qsort(a_->d, a_->len, sizeof(V), v_cmp_q);\n"
                                "      s[%d] = v_null();\n"
                                "      xa_rel(a_); }\n",
                                base, base);
                        break;

                    case XBI_ARR_COPY:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"arrCopy\", 1);\n"
                                "      XA* o_ = xa_new(0, v_null());\n"
                                "      xa_reserve(o_, a_->len);\n"
                                "      for (size_t i_ = 0; i_ < a_->len; i_++) { V x_ = a_->d[i_]; v_rtn(&x_); o_->d[o_->len++] = x_; }\n"
                                "      s[%d].t = XV_ARRAY; s[%d].a = o_;\n"
                                "      xa_rel(a_); }\n",
                                base, base, base);
                        break;

                    // ============================================================
                    // 字符串原语（全部按码点索引）
                    // ============================================================

                    case XBI_STR_AT:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strAt\", 1);\n"
                                "      int64_t ix_ = xb_int(&s[%d], \"strAt\", 2);\n"
                                "      XS* r_ = xb_str_at(s_, ix_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base + 1, base, base + 1, base);
                        break;

                    case XBI_STR_SUB:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strSub\", 1);\n"
                                "      int64_t a_ = xb_int(&s[%d], \"strSub\", 2);\n"
                                "      int64_t b_ = xb_int(&s[%d], \"strSub\", 3);\n"
                                "      XS* r_ = xb_str_sub(s_, a_, b_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base + 1, base + 2, base, base + 1, base + 2, base);
                        break;

                    case XBI_STR_FIND:
                    case XBI_STR_FIND_FROM:
                        if ((XBuiltin)in->a == XBI_STR_FIND_FROM) {
                            fprintf(e->f,
                                    "    { XS* s_ = xb_str(&s[%d], \"strFindFrom\", 1);\n"
                                    "      XS* p_ = xb_str(&s[%d], \"strFindFrom\", 2);\n"
                                    "      int64_t f_ = xb_int(&s[%d], \"strFindFrom\", 3);\n"
                                    "      int64_t r_ = xb_str_find(s_, p_, f_);\n"
                                    "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                    "      s[%d] = v_int(r_); }\n",
                                    base, base + 1, base + 2, base, base + 1, base + 2, base);
                        } else {
                            fprintf(e->f,
                                    "    { XS* s_ = xb_str(&s[%d], \"strFind\", 1);\n"
                                    "      XS* p_ = xb_str(&s[%d], \"strFind\", 2);\n"
                                    "      int64_t r_ = xb_str_find(s_, p_, 0);\n"
                                    "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                    "      s[%d] = v_int(r_); }\n",
                                    base, base + 1, base, base + 1, base);
                        }
                        break;

                    case XBI_STR_SPLIT:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strSplit\", 1);\n"
                                "      XS* p_ = xb_str(&s[%d], \"strSplit\", 2);\n"
                                "      XA* o_ = xb_str_split(s_, p_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d].t = XV_ARRAY; s[%d].a = o_; }\n",
                                base, base + 1, base, base + 1, base, base);
                        break;

                    case XBI_STR_JOIN:
                        fprintf(e->f,
                                "    { XA* a_ = xb_arr(&s[%d], \"strJoin\", 1);\n"
                                "      XS* p_ = xb_str(&s[%d], \"strJoin\", 2);\n"
                                "      XS* r_ = xb_str_join(a_, p_);\n"
                                "      xa_rel(a_); v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base + 1, base + 1, base);
                        break;

                    case XBI_STR_UPPER:
                    case XBI_STR_LOWER:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"%s\", 1);\n"
                                "      XS* r_ = xb_str_case(s_, %s);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, (int)in->a == XBI_STR_UPPER ? "strUpper" : "strLower",
                                (int)in->a == XBI_STR_UPPER ? "true" : "false",
                                base, base);
                        break;

                    case XBI_STR_TRIM:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strTrim\", 1);\n"
                                "      XS* r_ = xb_str_trim(s_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base, base);
                        break;

                    case XBI_STR_REPLACE:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strReplace\", 1);\n"
                                "      XS* f_ = xb_str(&s[%d], \"strReplace\", 2);\n"
                                "      XS* t_ = xb_str(&s[%d], \"strReplace\", 3);\n"
                                "      XS* r_ = xb_str_replace(s_, f_, t_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base + 1, base + 2, base, base + 1, base + 2, base);
                        break;

                    case XBI_STR_STARTS:
                    case XBI_STR_ENDS: {
                        bool starts = ((XBuiltin)in->a == XBI_STR_STARTS);
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"%s\", 1);\n"
                                "      XS* a_ = xb_str(&s[%d], \"%s\", 2);\n"
                                "      bool ok_ = s_->len >= a_->len && %s;\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(ok_); }\n",
                                base, starts ? "strStarts" : "strEnds",
                                base + 1, starts ? "strStarts" : "strEnds",
                                starts ? "memcmp(s_->d, a_->d, a_->len) == 0"
                                       : "memcmp(s_->d + s_->len - a_->len, a_->d, a_->len) == 0",
                                base, base + 1, base);
                        break;
                    }

                    case XBI_STR_REPEAT:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strRepeat\", 1);\n"
                                "      int64_t n_ = xb_int(&s[%d], \"strRepeat\", 2);\n"
                                "      XS* r_ = xb_str_repeat(s_, n_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(r_); }\n",
                                base, base + 1, base, base + 1, base);
                        break;

                    case XBI_STR_CODE:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strCode\", 1);\n"
                                "      int64_t r_ = 0;\n"
                                "      if (s_->len > 0) { int ad_ = 0; r_ = xu_decode(s_->d, s_->len, &ad_); }\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_int(r_); }\n",
                                base, base, base);
                        break;

                    case XBI_STR_FROM_CODE:
                        fprintf(e->f,
                                "    { int64_t cp_ = xb_int(&s[%d], \"strFromCode\", 1);\n"
                                "      char buf_[4]; size_t n_ = 0;\n"
                                "      unsigned int u_ = (unsigned int)cp_;\n"
                                "      if (u_ < 0x80) buf_[n_++] = (char)u_;\n"
                                "      else if (u_ < 0x800) { buf_[n_++] = (char)(0xC0 | (u_ >> 6)); buf_[n_++] = (char)(0x80 | (u_ & 0x3F)); }\n"
                                "      else if (u_ < 0x10000) { buf_[n_++] = (char)(0xE0 | (u_ >> 12)); buf_[n_++] = (char)(0x80 | ((u_ >> 6) & 0x3F)); buf_[n_++] = (char)(0x80 | (u_ & 0x3F)); }\n"
                                "      else if (u_ < 0x110000) { buf_[n_++] = (char)(0xF0 | (u_ >> 18)); buf_[n_++] = (char)(0x80 | ((u_ >> 12) & 0x3F)); buf_[n_++] = (char)(0x80 | ((u_ >> 6) & 0x3F)); buf_[n_++] = (char)(0x80 | (u_ & 0x3F)); }\n"
                                "      else rt_err(\"`strFromCode` 的码点非法\");\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(xs_new(buf_, n_)); }\n",
                                base, base, base);
                        break;

                    case XBI_STR_PARSE_INT:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strParseInt\", 1);\n"
                                "      int64_t r_ = xb_parse_int(s_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_int(r_); }\n",
                                base, base, base);
                        break;

                    case XBI_STR_PARSE_FLOAT:
                        fprintf(e->f,
                                "    { XS* s_ = xb_str(&s[%d], \"strParseFloat\", 1);\n"
                                "      double r_ = xb_parse_flt(s_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_flt(r_); }\n",
                                base, base, base);
                        break;

                    // ============================================================
                    // 数学与随机
                    // ============================================================

                    case XBI_MATH:
                        // 子操作编码在 c 操作数：低 8 位是 XMathOp，高位是 argc。
                        // 这里直接把 s[base] 起的 argc 个槽交给 xb_math。
                        fprintf(e->f,
                                "    { V r_ = xb_math(&s[%d], %d, %d);\n"
                                "      for (int i_ = 0; i_ < %d; i_++) v_rel(&s[%d + i_]);\n"
                                "      s[%d] = r_; }\n",
                                base, (int)in->b, (int)(in->c & 0xFF), (int)in->b, base, base);
                        break;

                    case XBI_RANDOM:
                        fprintf(e->f, "    s[%d] = v_flt(xb_rand());\n", d);
                        break;

                    case XBI_RANDOM_SEED:
                        fprintf(e->f,
                                "    { int64_t sd_ = xb_int(&s[%d], \"randomSeed\", 1);\n"
                                "      xb_seed((uint64_t)sd_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_null(); }\n",
                                base, base, base);
                        break;

                    case XBI_FAIL:
                        // 标准库的报错出口。rt_err 带上 g_file/g_line/
                        // g_col 后终止，与解释器同一份错误格式。
                        // 注意不能直接把 s[base].s->d 传进去后就释放：
                        // rt_err 会先 fflush 再 fprintf，指针必须活到它返回。
                        fprintf(e->f,
                                "    { XS* m_ = xb_str(&s[%d], \"fail\", 1);\n"
                                "      rt_err(m_->d);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_null(); }\n",
                                base, base, base);
                        break;

                    case XBI_TYPEOF:
                        // 返回值类型名。v_tname 与解释器的 type_label
                        // 逐字对应，两档输出才能一致。
                        fprintf(e->f,
                                "    { XS* t_ = xs_new(v_tname(s[%d].t), strlen(v_tname(s[%d].t)));\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(t_); }\n",
                                base, base, base, base);
                        break;

                    // ============================================================
                    // 文件
                    // ============================================================
                    //
                    // 与解释器侧逐条对应。参数下标：第 1 个参数在 base，
                    // 第 2 个在 base+1（压栈是从左到右）。

                    case XBI_FS_READ:
                        fprintf(e->f,
                                "    { int ok_ = 0;\n"
                                "      XS* t_ = xu_read(xb_str(&s[%d], \"fsRead\", 1)->d, &ok_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(t_); }\n",
                                base, base, base);
                        break;

                    case XBI_FS_READ_OR:
                        // 打不开就返回默认值。
                        //
                        // 默认值可能是任意类型，所以直接把 s[base+1]
                        // 搬到 s[base]。这里**不能**写成 s[base] = s[base] ——
                        // 那是一次自赋值（编译器会给警告，但更糟的是
                        // 运行期读的是已经 v_rel 过的槽位，拿到的默认值
                        // 永远是 Null）。
                        //
                        // 顺序也要注意：先 rel 掉槽 0 的旧值（路径字符串），
                        // 再把槽 1 搬过来，最后 rel 槽 1。搬过来之后槽 1
                        // 与槽 0 指向同一份数据，所以槽 1 不能再被释放 ——
                        // 这里用 v_null 把它清成 Null，v_rel 就成空操作。
                        fprintf(e->f,
                                "    { int ok_ = 0;\n"
                                "      XS* t_ = xu_read(xb_str(&s[%d], \"fsReadOr\", 1)->d, &ok_);\n"
                                "      v_rel(&s[%d]);\n"
                                "      if (ok_) { s[%d] = v_str(t_); v_rel(&s[%d]); }\n"
                                "      else { s[%d] = s[%d]; s[%d] = v_null(); xs_rel(t_); }\n"
                                "    }\n",
                                base, base, base, base + 1, base, base + 1, base + 1);
                        break;

                    case XBI_FS_WRITE:
                    case XBI_FS_APPEND:
                        // 内容参数接受任意类型（与解释器一致）：
                        // fsWrite(p, 42) 写出的应该是 "42"。
                        //
                        // 不能直接用 xb_str —— 它会拒绝非字符串参数并
                        // 报「第 2 个参数必须是 String」。解释器那边是
                        // 宽松的，两侧必须以同一份语义为准，否则同一个
                        // 程序在 --emit=exe 下行为不同。
                        //
                        // v_str_of 是生成代码里已有的「任意值转文本」，
                        // 输出格式与解释器的 value_to_str 逐字对应。
                        fprintf(e->f,
                                "    { XS* body_ = v_str_of(s[%d]);\n"
                                "      bool ok_ = xu_write(xb_str(&s[%d], \"%s\", 1)->d, body_, %s);\n"
                                "      xs_rel(body_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(ok_); }\n",
                                base + 1,
                                base,
                                (int)in->a == XBI_FS_WRITE ? "fsWrite" : "fsAppend",
                                (int)in->a == XBI_FS_WRITE ? "false" : "true",
                                base, base + 1, base);
                        break;

                    case XBI_FS_REPLACE:
                        // 原子替换：写临时文件再改名。与解释器同一条路径。
                        fprintf(e->f,
                                "    { XS* body_ = xb_str(&s[%d], \"fsReplace\", 2);\n"
                                "      const char* p_ = xb_str(&s[%d], \"fsReplace\", 1)->d;\n"
                                "      char tmp_[1200];\n"
                                "      snprintf(tmp_, sizeof(tmp_), \"%%s.tmp%%d\", p_, 0);\n"
                                "      bool ok_ = xu_write(tmp_, body_, false);\n"
                                "      if (ok_) { XU_WREMOVE(p_); ok_ = rename(tmp_, p_) == 0; }\n"
                                "      if (!ok_) XU_WREMOVE(tmp_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(ok_); }\n",
                                base + 1, base, base, base + 1, base);
                        break;

                    case XBI_FS_EXISTS:
                    case XBI_FS_IS_DIR:
                    case XBI_FS_REMOVE:
                    case XBI_FS_MKDIR: {
                        const char* nm = "fsExists";
                        const char* expr = "XU_WACCESS(p_) == 0";
                        switch ((XBuiltin)in->a) {
                        case XBI_FS_IS_DIR:  nm = "fsIsDir";  expr = "xu_isdir(p_)"; break;
                        case XBI_FS_REMOVE:  nm = "fsRemove"; expr = "XU_WREMOVE(p_) == 0"; break;
                        case XBI_FS_MKDIR:   nm = "fsMkdir";  expr = "XU_WMKDIR(p_) == 0 || xu_isdir(p_)"; break;
                        default: break;
                        }
                        fprintf(e->f,
                                "    { const char* p_ = xb_str(&s[%d], \"%s\", 1)->d;\n"
                                "      bool b_ = (%s);\n"
                                "      v_rel(&s[%d]); s[%d] = v_bool(b_); }\n",
                                base, nm, expr, base, base);
                        break;
                    }

                    case XBI_FS_SIZE:
                        fprintf(e->f,
                                "    { xu_stat_t st_;\n"
                                "      int64_t n_ = XU_WSTAT(xb_str(&s[%d], \"fsSize\", 1)->d, &st_) == 0\n"
                                "                    ? (int64_t)st_.st_size : (int64_t)-1;\n"
                                "      v_rel(&s[%d]); s[%d] = v_int(n_); }\n",
                                base, base, base);
                        break;

                    case XBI_FS_LIST:
                        fprintf(e->f,
                                "    { XA* a_ = xu_list(xb_str(&s[%d], \"fsList\", 1)->d);\n"
                                "      v_rel(&s[%d]); s[%d] = v_arr(a_); }\n",
                                base, base, base);
                        break;

                    // ============================================================
                    // TCP
                    // ============================================================

                    case XBI_NET_LISTEN:
                        fprintf(e->f,
                                "    { int64_t n_ = xu_listen(xb_str(&s[%d], \"netListen\", 1)->d,\n"
                                "                             xb_int(&s[%d], \"netListen\", 2));\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_int(n_); }\n",
                                base, base + 1, base, base + 1, base);
                        break;

                    case XBI_NET_ACCEPT:
                        fprintf(e->f,
                                "    { int64_t n_ = xu_accept(xb_int(&s[%d], \"netAccept\", 1));\n"
                                "      v_rel(&s[%d]); s[%d] = v_int(n_); }\n",
                                base, base, base);
                        break;

                    case XBI_NET_RECV:
                        fprintf(e->f,
                                "    { XS* t_ = xu_recv(xb_int(&s[%d], \"netRecv\", 1));\n"
                                "      v_rel(&s[%d]); s[%d] = v_str(t_); }\n",
                                base, base, base);
                        break;

                    case XBI_NET_SEND:
                        fprintf(e->f,
                                "    { int64_t n_ = xu_send(xb_int(&s[%d], \"netSend\", 1),\n"
                                "                            xb_str(&s[%d], \"netSend\", 2));\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_int(n_); }\n",
                                base, base + 1, base, base + 1, base);
                        break;

                    case XBI_NET_CLOSE:
                        fprintf(e->f,
                                "    { int64_t h_ = xb_int(&s[%d], \"netClose\", 1);\n"
                                "      if (h_ >= 0) XU_CLOSE((xu_sock)h_);\n"
                                "      v_rel(&s[%d]); s[%d] = v_null(); }\n",
                                base, base, base);
                        break;

                    case XBI_NET_CONNECT:
                        fprintf(e->f,
                                "    { int64_t n_ = xu_connect(xb_str(&s[%d], \"netConnect\", 1)->d,\n"
                                "                             xb_int(&s[%d], \"netConnect\", 2));\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_int(n_); }\n",
                                base, base + 1, base, base + 1, base);
                        break;

                    case XBI_NET_PORT:
                        fprintf(e->f,
                                "    { int64_t n_ = xu_port(xb_int(&s[%d], \"netPort\", 1));\n"
                                "      v_rel(&s[%d]); s[%d] = v_int(n_); }\n",
                                base, base, base);
                        break;

                    case XBI_NET_PEER:
                        fprintf(e->f,
                                "    { XS* t_ = xu_peer(xb_int(&s[%d], \"netPeer\", 1));\n"
                                "      v_rel(&s[%d]); s[%d] = v_str(t_); }\n",
                                base, base, base);
                        break;

                    case XBI_NET_ERR:
                        fprintf(e->f,
                                "    { const char* m_ = xu_net_error();\n"
                                "      s[%d] = v_str(xs_new(m_, strlen(m_))); }\n",
                                base);
                        break;

                    case XBI_NET_PENDING:
                        fprintf(e->f,
                                "    { bool b_ = xu_alive(xb_int(&s[%d], \"netPending\", 1));\n"
                                "      v_rel(&s[%d]); s[%d] = v_bool(b_); }\n",
                                base, base, base);
                        break;

                    // ---- 通用 ----
                    //
                    // 睡眠不需要窗口运行时，所以在生成代码里也不走
                    // xu_gui_sleep，直接调产物自带的小函数 xu_sleep。
                    // 这样「只 sleep 不开窗」的程序不必链接 GUI 那套。
                    case XBI_SLEEP:
                        // 与 winSleep 同一套参数语义（Int 或 Float），
                        // 区别只是不需要窗口运行时。
                        fprintf(e->f,
                                "    if (s[%d].t != XV_INT && s[%d].t != XV_FLOAT)\n"
                                "        rt_err(\"sleep 的参数必须是数值（毫秒）\");\n"
                                "    xu_sleep((int)(s[%d].t == XV_INT ? (double)s[%d].i : s[%d].f));\n"
                                "    v_rel(&s[%d]); s[%d] = v_null();\n",
                                base, base, base, base, base, base, base);
                        break;

                    // ---- 时间 ----
                    //
                    // 与解释器同名分支一一对应。三个都无参数，直接在
                    // 产物里调 xu_now_ms / xu_mono_ms（由 prelude 提供
                    // 的小函数），不经过窗口运行时。
                    case XBI_NOW_MILLIS:
                        fprintf(e->f, "    s[%d] = v_int(xu_now_ms());\n", d);
                        break;
                    case XBI_NOW_SECONDS:
                        fprintf(e->f, "    s[%d] = v_int(xu_now_ms() / 1000);\n", d);
                        break;
                    case XBI_MONO_MILLIS:
                        fprintf(e->f, "    s[%d] = v_int(xu_mono_ms());\n", d);
                        break;

                    // ---- 字节 ----
                    //
                    // 与解释器同名分支一一对应。注意 strFromBytes 里
                    // **不能**用 xb_int 取元素：数组元素可能是 Float 或
                    // Bool（解释器都接受），而 xb_int 遇到非 Int 会
                    // 直接报运行时错误 —— 那就与解释器行为不一致了。
                    case XBI_STR_FROM_BYTES:
                        fprintf(e->f,
                                "    { if (s[%d].t != XV_ARRAY)\n"
                                "          rt_err(\"strFromBytes 的参数必须是 Array（字节数组）\");\n"
                                "      XA* a_ = s[%d].a;\n"
                                "      char* b_ = (char*)malloc(a_->len > 0 ? a_->len : 1);\n"
                                "      size_t w_ = 0;\n"
                                "      for (size_t i_ = 0; i_ < a_->len; i_++) {\n"
                                "          V el_ = a_->d[i_];\n"
                                "          long long t_ = 0;\n"
                                "          if (el_.t == XV_INT) t_ = el_.i;\n"
                                "          else if (el_.t == XV_FLOAT) t_ = (long long)el_.f;\n"
                                "          else if (el_.t == XV_BOOL) t_ = el_.b ? 1 : 0;\n"
                                "          if (t_ < 0) t_ = 0;\n"
                                "          if (t_ > 255) t_ = 255;\n"
                                "          b_[w_++] = (char)(unsigned char)t_;\n"
                                "      }\n"
                                "      XS* o_ = xs_new(b_, w_);\n"
                                "      free(b_);\n"
                                "      v_rel(&s[%d]); s[%d] = v_str(o_); }\n",
                                base, base, base, base);
                        break;

                    case XBI_STR_BYTES:
                        fprintf(e->f,
                                "    { if (s[%d].t != XV_STRING)\n"
                                "          rt_err(\"strBytes 的参数必须是 String\");\n"
                                "      XS* t_ = s[%d].s;\n"
                                "      XA* a_ = xa_new(0, v_null());\n"
                                "      for (size_t i_ = 0; i_ < t_->len; i_++)\n"
                                "          xa_push(a_, v_int((long long)(unsigned char)t_->d[i_]));\n"
                                "      v_rel(&s[%d]); s[%d] = v_arr(a_); }\n",
                                base, base, base, base);
                        break;

                    // ---- 反射 ----
                    //
                    // ── AOT 侧为什么必须全部实现，而不是「不支持就算了」 ──
                    //
                    // 两档语义必须一致，否则同一份源码在 `x run` 与
                    // `x build` 之后行为不同 —— 这是一致性测试会立刻
                    // 抓到的差异，而且是那种「本地跑得好、发版就坏」的
                    // 最坏形态。
                    //
                    // 元数据全部来自 gen_class_tables 生成的静态表，
                    // 不依赖 Bytecode（AOT 产物里根本没有它）。
                    //
                    // ── 可见性检查这里**不做** ─────────────────────
                    //
                    // 解释器侧用帧栈判「当前在哪个类的方法里」，AOT 侧
                    // 要还原同样的信息就得给每个函数加一个隐式的类索引
                    // 参数，牵动整条调用链。当前取舍是：AOT 的 fieldGet/
                    // fieldSet 不做访问控制。
                    //
                    // 这条取舍必须写清楚，不能假装不存在：编译器在编译期
                    // 已经拒绝了直接访问 private 字段，所以绕过路径只有
                    // 「显式用反射」，那是一个用户主动写的动作。等真要
                    // 收紧时，做法是给 xf_N 加一个 cls 参数（解释器侧的
                    // Frame.cls_idx 已经把语义定下来了）。
                    case XBI_CLASS_NAME:
                        // className(x) -> String
                        //
                        // ── 顺序陷阱：必须先取到 cname 再释放 ──────────
                        //
                        // 第一版写成「先给 s[base] 赋新字符串，再 v_rel
                        // s[base]」，结果把刚建好的那个字符串释放掉了 ——
                        // className 返回 null。
                        //
                        // 原因：s[base] 是同一个槽位，赋值后它已经指向新
                        // 字符串，此时 v_rel 释放的就是新字符串。正确做法
                        // 是先把字符串指针/长度取到 C 局部变量，再释放旧
                        // 值，最后赋新值。
                        //
                        // 这个 bug 只在 AOT 侧存在（解释器用 pop 拿到值再
                        // 构造返回，天然没有这个问题），所以它是一致性
                        // 测试才会抓到的那类偏移。
                        fprintf(e->f,
                                "    { const char* nm_;\n"
                                "      if (s[%d].t == XV_CLASS) nm_ = s[%d].k->cname;\n"
                                "      else nm_ = v_tname(s[%d].t);\n"
                                "      XS* o_ = xs_new(nm_, strlen(nm_));\n"
                                "      v_rel(&s[%d]);\n"
                                "      s[%d] = v_str(o_); }\n",
                                base, base, base, base, base);
                        break;

                    case XBI_CLASS_PARENT:
                        fprintf(e->f,
                                "    { const char* pn_ = \"\";\n"
                                "      if (s[%d].t == XV_CLASS) {\n"
                                "          int pi_ = xc_pi[s[%d].k->cls];\n"
                                "          if (pi_ >= 0 && pi_ < %d) pn_ = xc_tn[pi_];\n"
                                "      }\n"
                                "      XS* o_ = xs_new(pn_, strlen(pn_));\n"
                                "      v_rel(&s[%d]); s[%d] = v_str(o_); }\n",
                                base, base, bc->class_count, base, base);
                        break;

                    case XBI_CLASS_IS:
                        fprintf(e->f,
                                "    { XS* w_ = xb_str(&s[%d], \"classIs\", 2);\n"
                                "      bool r_ = false;\n"
                                "      if (s[%d].t == XV_CLASS) {\n"
                                "          int c_ = s[%d].k->cls;\n"
                                "          for (int g_ = 0; g_ < 64 && c_ >= 0 && c_ < %d; g_++) {\n"
                                "              if (strcmp(xc_tn[c_], w_->d) == 0) { r_ = true; break; }\n"
                                "              c_ = xc_pi[c_];\n"
                                "          }\n"
                                "      }\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(r_); }\n",
                                base + 1, base, base, bc->class_count,
                                base, base + 1, base);
                        break;

                    case XBI_CLASS_FIELDS:
                        // classFields(obj) -> [[name, type, vis, final], ...]
                        fprintf(e->f,
                                "    { XA* o_ = xa_new(0, v_null());\n"
                                "      if (s[%d].t == XV_CLASS) {\n"
                                "          int c_ = s[%d].k->cls;\n"
                                "          if (c_ >= 0 && c_ < %d) {\n"
                                "              for (int i_ = 0; i_ < xc_fc[c_]; i_++) {\n"
                                "                  XA* r_ = xa_new(4, v_null());\n"
                                "                  const char* fnn_ = xc_fns[c_][i_] ? xc_fns[c_][i_] : \"\";\n"
                                "                  const char* ftt_ = xc_fts[c_][i_] ? xc_fts[c_][i_] : \"\";\n"
                                "                  int v_ = xc_fviss[c_] ? xc_fviss[c_][i_] : 0;\n"
                                "                  const char* vs_ = (v_ == 1) ? \"protected\" :"
                                " (v_ == 2) ? \"private\" : \"public\";\n"
                                "                  r_->d[0] = v_str(xs_new(fnn_, strlen(fnn_)));\n"
                                "                  r_->d[1] = v_str(xs_new(ftt_, strlen(ftt_)));\n"
                                "                  r_->d[2] = v_str(xs_new(vs_, strlen(vs_)));\n"
                                "                  r_->d[3] = v_bool(xc_ffins[c_] && xc_ffins[c_][i_]);\n"
                                "                  xa_push(o_, v_arr(r_));\n"
                                "              }\n"
                                "          }\n"
                                "      }\n"
                                "      v_rel(&s[%d]); s[%d] = v_arr(o_); }\n",
                                base, base, bc->class_count, base, base);
                        break;

                    case XBI_CLASS_METHODS:
                        // classMethods(obj) -> [[name, vis], ...]
                        //
                        // 只列虚表里有实现的槽 —— 与解释器同一判据。
                        fprintf(e->f,
                                "    { XA* o_ = xa_new(0, v_null());\n"
                                "      if (s[%d].t == XV_CLASS) {\n"
                                "          int c_ = s[%d].k->cls;\n"
                                "          if (c_ >= 0 && c_ < %d) {\n"
                                "              for (int i_ = 0; i_ < xc_vtl[c_]; i_++) {\n"
                                "                  if (xc_vts[c_][i_] < 0) continue;\n"
                                "                  XA* r_ = xa_new(2, v_null());\n"
                                "                  const char* mn_ = xc_slotname(i_);\n"
                                "                  int v_ = xc_mviss[c_] ? xc_mviss[c_][i_] : 0;\n"
                                "                  const char* vs_ = (v_ == 1) ? \"protected\" :"
                                " (v_ == 2) ? \"private\" : \"public\";\n"
                                "                  r_->d[0] = v_str(xs_new(mn_, strlen(mn_)));\n"
                                "                  r_->d[1] = v_str(xs_new(vs_, strlen(vs_)));\n"
                                "                  xa_push(o_, v_arr(r_));\n"
                                "              }\n"
                                "          }\n"
                                "      }\n"
                                "      v_rel(&s[%d]); s[%d] = v_arr(o_); }\n",
                                base, base, bc->class_count, base, base);
                        break;

                    case XBI_FIELD_HAS:
                        fprintf(e->f,
                                "    { XS* fn_ = xb_str(&s[%d], \"fieldHas\", 2);\n"
                                "      bool r_ = false;\n"
                                "      if (s[%d].t == XV_CLASS)\n"
                                "          r_ = xc_find_fld(s[%d].k->cls, fn_->d) >= 0;\n"
                                "      /* xb_str 不 retain，所以这里**不能** xs_rel(fn_) —— */\n"
                                "      /* 释放它等于把 s[base+1] 还持有的字符串释放掉。 */\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(r_); }\n",
                                base + 1, base, base, base, base + 1, base);
                        break;

                    case XBI_METHOD_HAS:
                        fprintf(e->f,
                                "    { XS* mn_ = xb_str(&s[%d], \"methodHas\", 2);\n"
                                "      bool r_ = false;\n"
                                "      if (s[%d].t == XV_CLASS) {\n"
                                "          int sl_ = xc_find_slot(mn_->d);\n"
                                "          int c_ = s[%d].k->cls;\n"
                                "          r_ = sl_ >= 0 && c_ >= 0 && c_ < %d &&"
                                " sl_ < xc_vtl[c_] && xc_vts[c_][sl_] >= 0;\n"
                                "      }\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_bool(r_); }\n",
                                base + 1, base, base, bc->class_count,
                                base, base + 1, base);
                        break;

                    case XBI_FIELD_GET:
                        fprintf(e->f,
                                "    { XS* fn_ = xb_str(&s[%d], \"fieldGet\", 2);\n"
                                "      if (s[%d].t != XV_CLASS) rt_err(\"fieldGet 的第 1 个参数必须是类实例\");\n"
                                "      int fi_ = xc_find_fld(s[%d].k->cls, fn_->d);\n"
                                "      if (fi_ < 0) rt_err(\"类没有这个字段\");\n"
                                "      V got_ = s[%d].k->f[fi_];\n"
                                "      v_rtn(&got_);\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = got_; }\n",
                                base + 1, base, base, base,
                                base, base + 1, base);
                        break;

                    case XBI_FIELD_SET:
                        // fieldSet(obj, "name", v)。三个实参：
                        //   base+0 = obj，base+1 = 字段名，base+2 = 新值
                        //
                        // ── 释放顺序不能反 ────────────────────────────
                        //
                        // 必须先 v_rel 旧字段值、再接管新值。反过来的话
                        // `fieldSet(a, "x", fieldGet(a, "x"))` 会让新值
                        // 与旧值是同一个对象，先接管再释放就把刚存进去
                        // 的那份释放掉了 —— 引用计数减到 0，字段变悬空。
                        fprintf(e->f,
                                "    { XS* fn_ = xb_str(&s[%d], \"fieldSet\", 2);\n"
                                "      if (s[%d].t != XV_CLASS) rt_err(\"fieldSet 的第 1 个参数必须是类实例\");\n"
                                "      int fi_ = xc_find_fld(s[%d].k->cls, fn_->d);\n"
                                "      if (fi_ < 0) rt_err(\"类没有这个字段\");\n"
                                "      if (xc_ffins[s[%d].k->cls] && xc_ffins[s[%d].k->cls][fi_])\n"
                                "          rt_err(\"字段是 final 的，反射也不能改\");\n"
                                "      v_rel(&s[%d].k->f[fi_]);\n"
                                "      s[%d].k->f[fi_] = s[%d];\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_null(); }\n",
                                base + 1,               /* fn_  */
                                base,                   /* 类型检查 */
                                base,                   /* 找字段 */
                                base, base,             /* final 检查 ×2 */
                                base,                   /* v_rel 旧值 */
                                base, base + 2,         /* 接管新值 */
                                base, base + 1,         /* 释放 obj / name */
                                base);                  /* 结果槽 */
                        break;

                    // ---- 注解 ----
                    //
                    // 签名统一是 (x, target, member, name)，annoAll 少一个
                    // name —— 也就是 (x, target, member)。
                    //
                    // ── member 这一位为什么必须有 ────────────────────
                    //
                    // 没有它，`annoGet(u, "field", "Column")` 在同一个类
                    // 里有多个字段带 @Column 时无法回答「你要哪一个」。
                    // 第一版就是这样，实测返回空数组 —— 而「空数组」看起来
                    // 完全像「这个注解没有实参」，是最容易被当成正常结果的
                    // 静默错误。类目标传 "" 占位。
                    //
                    // ── ANNO_ALL 单独一个分支 ────────────────────────
                    //
                    // 它比另外三个少一个参数。想用一个分支靠三元表达式糊
                    // 过去的话，「取不存在的第 4 个参数」会读到上一个表达式
                    // 残留在栈上的值 —— 那种 bug 表现为「annoAll 偶尔返回
                    // 了别人的注解名」，极难定位。分开写，让参数个数在
                    // 生成期就定死。
                    case XBI_CLASS_ALL:
                        // classAll() -> 字节码里全部类名（AOT 侧同语义）。
                        //
                        // 注意：AOT 侧的 anno*/classFields 等查询**只支持
                        // 实例接收者**，尚不支持解释器已有的类名字符串
                        // 接收者（TODO 与 BC_NEW_CLASS_NAME/BC_CALL_NAME
                        // 的 AOT 支持一起补）。走 x run（解释器）时框架
                        // 全功能可用。
                        fprintf(e->f,
                                "    { XA* o_ = xa_new(0, v_null());\n"
                                "      for (int i_ = 0; i_ < %d; i_++) {\n"
                                "          const char* cn_ = xc_tn[i_] ? xc_tn[i_] : \"\";\n"
                                "          xa_push(o_, v_str(xs_new(cn_, strlen(cn_))));\n"
                                "      }\n"
                                "      s[%d] = v_arr(o_); }\n",
                                bc->class_count, base);
                        break;

                    case XBI_METHOD_PARAMS:
                        // methodParams / 按名查注解 / classNew / methodCall：
                        // AOT 侧尚未支持（需要给生成代码配函数指针表
                        // 与参数名表）。使用的测试打 // aot-skip 豁免，
                        // 详见 run_tests.sh 里 aot-skip 的注释。
                        fprintf(e->f,
                                "#error 框架动态调用内建（%d）的 AOT 支持尚未实现\n",
                                (int)in->a);
                        break;

                    case XBI_ANNO_ALL:
                        fprintf(e->f,
                                "    { XS* tg_ = xb_str(&s[%d], \"annoAll\", 2);\n"
                                "      XS* mb_ = xb_str(&s[%d], \"annoAll\", 3);\n"
                                "      int t_ = -1;\n"
                                "      if (strcmp(tg_->d, \"class\") == 0) t_ = 0;\n"
                                "      else if (strcmp(tg_->d, \"field\") == 0) t_ = 1;\n"
                                "      else if (strcmp(tg_->d, \"method\") == 0) t_ = 2;\n"
                                "      if (t_ < 0) rt_err(\"target 只能是 class / field / method\");\n"
                                "      if (t_ == 0 && mb_->len > 0)\n"
                                "          rt_err(\"target 是 class 时 member 必须是空字符串\");\n"
                                "      if (s[%d].t != XV_CLASS) rt_err(\"注解查询的第 1 个参数必须是类实例\");\n"
                                "      int ci_ = s[%d].k->cls;\n"
                                "      XA* o_ = xa_new(0, v_null());\n"
                                "      for (int i_ = 0; i_ < xc_anc; i_++) {\n"
                                "          if (xc_atgt[i_] != t_ || xc_aci[i_] != ci_) continue;\n"
                                "          /* 类目标的 member 在表里是空串 */\n"
                                "          const char* em_ = xc_amenber[i_] ? xc_amenber[i_] : \"\";\n"
                                "          if (t_ != 0 && strcmp(em_, mb_->d) != 0) continue;\n"
                                "          xa_push(o_, v_str(xs_new(xc_aname[i_], strlen(xc_aname[i_]))));\n"
                                "      }\n"
                                "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                "      s[%d] = v_arr(o_); }\n",
                                base + 1, base + 2, base, base,
                                base, base + 1, base + 2, base);
                        break;

                    case XBI_ANNO_HAS:
                    case XBI_ANNO_GET:
                    case XBI_ANNO_GET_KV: {
                        bool want_kv = ((XBuiltin)in->a == XBI_ANNO_GET_KV);
                        bool want_b  = ((XBuiltin)in->a == XBI_ANNO_HAS);
                        fprintf(e->f,
                                "    { XS* tg_ = xb_str(&s[%d], \"anno\", 2);\n"
                                "      XS* mb_ = xb_str(&s[%d], \"anno\", 3);\n"
                                "      XS* nm_ = xb_str(&s[%d], \"anno\", 4);\n"
                                "      int t_ = -1;\n"
                                "      if (strcmp(tg_->d, \"class\") == 0) t_ = 0;\n"
                                "      else if (strcmp(tg_->d, \"field\") == 0) t_ = 1;\n"
                                "      else if (strcmp(tg_->d, \"method\") == 0) t_ = 2;\n"
                                "      if (t_ < 0) rt_err(\"target 只能是 class / field / method\");\n"
                                "      if (t_ == 0 && mb_->len > 0)\n"
                                "          rt_err(\"target 是 class 时 member 必须是空字符串\");\n"
                                "      if (s[%d].t != XV_CLASS) rt_err(\"注解查询的第 1 个参数必须是类实例\");\n"
                                "      int ci_ = s[%d].k->cls;\n"
                                "      int hi_ = -1;\n"
                                "      for (int i_ = 0; i_ < xc_anc; i_++) {\n"
                                "          if (xc_atgt[i_] != t_ || xc_aci[i_] != ci_) continue;\n"
                                "          if (strcmp(xc_aname[i_], nm_->d) != 0) continue;\n"
                                "          const char* em_ = xc_amenber[i_] ? xc_amenber[i_] : \"\";\n"
                                "          if (t_ != 0 && strcmp(em_, mb_->d) != 0) continue;\n"
                                "          hi_ = i_; break;\n"
                                "      }\n",
                                base + 1, base + 2, base + 3, base, base);
                        if (want_b) {
                            fprintf(e->f,
                                    "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                    "      s[%d] = v_bool(hi_ >= 0); }\n",
                                    base, base + 1, base + 2, base + 3, base);
                        } else if (want_kv) {
                            fprintf(e->f,
                                    "      XA* o_ = xa_new(0, v_null());\n"
                                    "      if (hi_ >= 0) {\n"
                                    "          for (int k_ = 0; k_ < xc_aac[hi_]; k_++) {\n"
                                    "              XA* r_ = xa_new(2, v_null());\n"
                                    "              const char* kk_ = xc_akeys[hi_] ? xc_akeys[hi_][k_] : \"\";\n"
                                    "              const char* vv_ = xc_avals[hi_] ? xc_avals[hi_][k_] : \"\";\n"
                                    "              /* 空串表示「位置实参，没有键」→ null。\n"
                                    "                 注解键名不可能是空串（解析器要求 IDENT），\n"
                                    "                 所以这个哨兵不会与真实键冲突。 */\n"
                                    "              if (kk_ && kk_[0])\n"
                                    "                  r_->d[0] = v_str(xs_new(kk_, strlen(kk_)));\n"
                                    "              r_->d[1] = v_str(xs_new(vv_, strlen(vv_)));\n"
                                    "              xa_push(o_, v_arr(r_));\n"
                                    "          }\n"
                                    "      }\n"
                                    "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                    "      s[%d] = v_arr(o_); }\n",
                                    base, base + 1, base + 2, base + 3, base);
                        } else {
                            fprintf(e->f,
                                    "      XA* o_ = xa_new(0, v_null());\n"
                                    "      if (hi_ >= 0) {\n"
                                    "          for (int k_ = 0; k_ < xc_aac[hi_]; k_++) {\n"
                                    "              const char* vv_ = xc_avals[hi_] ? xc_avals[hi_][k_] : \"\";\n"
                                    "              xa_push(o_, v_str(xs_new(vv_, strlen(vv_))));\n"
                                    "          }\n"
                                    "      }\n"
                                    "      v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]); v_rel(&s[%d]);\n"
                                    "      s[%d] = v_arr(o_); }\n",
                                    base, base + 1, base + 2, base + 3, base);
                        }
                        break;
                    }
                }
                break;
            }

            // ---------- 控制流 ----------
            // 跳转操作数是「相对本条的下一条」的函数内偏移，
            // 所以目标是 pc + 1 + a。见 compile.c 里 patch_to 的说明。
            case BC_JUMP:
                fprintf(e->f, "    goto L%d_%d;\n",
                        fi, pc + 1 + in->a);
                break;
            case BC_JUMP_IF_FALSE:
                fprintf(e->f,
                        "    { bool t_ = v_true(s[%d]); v_rel(&s[%d]);\n"
                        "      if (!t_) goto L%d_%d; }\n",
                        d - 1, d - 1, fi, pc + 1 + in->a);
                break;
            case BC_JUMP_IF_TRUE:
                fprintf(e->f,
                        "    { bool t_ = v_true(s[%d]); v_rel(&s[%d]);\n"
                        "      if (t_) goto L%d_%d; }\n",
                        d - 1, d - 1, fi, pc + 1 + in->a);
                break;

            // ---------- 调用 / 返回 ----------
            case BC_CALL: {
                FuncProto* cf = &bc->funcs[in->a];
                // 实参已在栈上（从 d - param_count 开始），正好是被调函数
                // 的槽位 0..param_count-1 —— 因为两边用的是同一个栈区
                // 并且帧基址就是 arg_base。所以这里不需要搬参数。
                //
                // local_count 之外的槽位由被调函数自己清（它的序言里做）。
                fprintf(e->f,
                        "    { V r_ = xf_%d(s + %d);\n"
                        "      for (int i_ = %d; i_ < %d; i_++) v_rel(&s[i_]);\n"
                        "      s[%d] = r_; }\n",
                        in->a, d - cf->param_count,
                        d - cf->param_count, d,
                        d - cf->param_count);
                break;
            }
            case BC_RETURN:
                fprintf(e->f,
                        "    { V r_ = s[%d];\n"
                        "      for (int i_ = %d; i_ < %d; i_++) v_rel(&s[i_]);\n"
                        "      return r_; }\n",
                        d - 1, 0, d - 1);
                // 注意：这里从 0 开始释放到 d-1。因为本函数的槽位从
                // 基址 0 开始（s 指针已由调用方偏移到本帧基址）。
                break;
            case BC_RETURN_VOID:
                fprintf(e->f,
                        "    { for (int i_ = 0; i_ < %d; i_++) v_rel(&s[i_]);\n"
                        "      return v_null(); }\n", d);
                break;

            // ---------- 输出 ----------
            case BC_PRINT:
                fprintf(e->f,
                        "    if (s[%d].t == XV_STRING) fwrite(s[%d].s->d, 1, s[%d].s->len, stdout);\n"
                        "    else { XS* p_ = v_str_of(s[%d]); fwrite(p_->d, 1, p_->len, stdout); xs_rel(p_); }\n"
                        "    v_rel(&s[%d]);\n",
                        d - 1, d - 1, d - 1, d - 1, d - 1);
                break;
            case BC_HALT:
                fprintf(e->f, "    return v_null();\n");
                break;

            // ---------- 超指令 ----------
            case BC_LOAD_LOAD:
                // 压两个值到 d 和 d+1（d 是执行前栈深）
                fprintf(e->f,
                        "    s[%d] = s[%d]; v_rtn(&s[%d]);\n"
                        "    s[%d] = s[%d]; v_rtn(&s[%d]);\n",
                        d, in->a, d,
                        d + 1, in->b, d + 1);
                break;
            case BC_DUP_STORE:
                fprintf(e->f,
                        "    { V v_ = s[%d]; v_rel(&s[%d]);\n"
                        "      v_rtn(&v_);\n"
                        "      s[%d] = v_; }\n",
                        d - 1, in->a, in->a);
                break;
            case BC_STORE_POP:
                fprintf(e->f,
                        "    { V v_ = s[%d]; v_rel(&s[%d]); s[%d] = v_; }\n",
                        d - 1, in->a, in->a);
                break;
            case BC_LOAD_CONST: {
                Constant* c = &bc->consts[in->b];
                // LOAD 压到 d，PUSH 压到 d+1（d 是执行前栈深）
                fprintf(e->f,
                        "    s[%d] = s[%d];\n"
                        "    v_rtn(&s[%d]);\n",
                        d, in->a, d);
                switch (c->kind) {
                    case CONST_I64:
                        fprintf(e->f, "    s[%d] = v_int(%lldLL);\n", d + 1, (long long)c->i);
                        break;
                    case CONST_F64:
                        fprintf(e->f, "    s[%d] = v_flt(%a);\n", d + 1, c->f);
                        break;
                    default:
                        fprintf(e->f, "    s[%d] = v_str(xs_new(\"\",0));\n", d + 1);
                        break;
                }
                break;
            }
            case BC_LT_JUMP_FALSE:
                fprintf(e->f,
                        "    { bool c_ = (s[%d].i < s[%d].i);\n"
                        "      if (!c_) goto L%d_%d; }\n",
                        d - 2, d - 1, fi, pc + 1 + in->a);
                break;
            case BC_DUP_STORE_POP:
                // DUP_STORE + POP ≡ STORE_LOCAL：弹出栈顶存进槽位
                fprintf(e->f,
                        "    { V v_ = s[%d]; v_rel(&s[%d]); s[%d] = v_; }\n",
                        d - 1, in->a, in->a);
                break;
            case BC_ADD_STORE:
                fprintf(e->f,
                        "    { int64_t r_; if (ovf_add(s[%d].i, s[%d].i, &r_)) ovf_err(\"+\");\n"
                        "      v_rel(&s[%d]); s[%d].t = XV_INT; s[%d].i = r_; }\n",
                        d - 2, d - 1, in->a, in->a, in->a);
                break;
            case BC_MOD_STORE:
                fprintf(e->f,
                        "    { int64_t a_ = s[%d].i, b_ = s[%d].i, r_;\n"
                        "      if (b_ == 0) rt_err(\"整数取模的除数为零\");\n"
                        "      r_ = (a_ == INT64_MIN && b_ == -1) ? 0 : (a_ %% b_);\n"
                        "      v_rel(&s[%d]); s[%d].t = XV_INT; s[%d].i = r_; }\n",
                        d - 2, d - 1, in->a, in->a, in->a);
                break;

            case BC_NOP:
                break;

            default:
                fprintf(e->f, "    rt_err(\"尚未在 C 后端实现的指令\");\n");
                break;
        }
    }

    // 兜底：函数体结尾必须有 return（编译器已保证有 RETURN，
    // 但不让 C 编译器报 "control reaches end of non-void function"）
    fprintf(e->f, "    return v_null();\n");

    free(depth);
    return 0;
}

// ============================================================
// 对外入口
// ============================================================

int codegen_c_emit(Bytecode* bc, const char* out_path, const XProfileDef* prof) {
    FILE* f = x_fopen(out_path, "wb");
    if (!f) {
        diag_error((SrcPos){ bc->source_file, 0, 0 },
                   "无法写入 C 文件 `%s`", out_path);
        diag_help("检查目录是否存在、是否有写权限。");
        return 1;
    }
    bool ok = gen_program(bc, f, prof);
    fclose(f);
    if (!ok) return 1;
    printf("已生成 %s  （C 代码，可交给任意 C 编译器）\n", out_path);
    return 0;
}

// 找可用的 C 编译器。顺序：--cc 指定 > CC 环境变量 > 平台默认。
static const char* pick_cc(const char* override) {
    if (override && *override) return override;
    const char* env = getenv("CC");
    if (env && *env) return env;
#if X_HOST_WINDOWS
    return "gcc";
#else
    return "cc";
#endif
}

// 判断一个编译器名是否存在（用 --version 探测）
static bool cc_exists(const char* cc) {
    char* argv[3];
    argv[0] = (char*)cc;
    argv[1] = (char*)"--version";
    argv[2] = NULL;
    return plat_spawn(argv, NULL) == 0;
}

// 把 target 的三元组翻译成 gcc/clang 的 --target 值。
//
// GCC 与 Clang 的写法不同：
//   clang 用 --target=x86_64-linux-gnu（完整三元组，原生支持交叉）
//   gcc  用 -m64 / 交叉前缀的编译器名（x86_64-linux-gnu-gcc）
//
// 这里走 gcc 的方式：交叉编译时按 cc_prefix 拼编译器名，
// 因为那个方式对 gcc 和 clang 都成立，而且错误信息更直接。
static void cc_for_target(const XTarget* t, const char* base,
                          char* out, size_t out_len) {
    if (target_is_host(t)) {
        snprintf(out, out_len, "%s", base);
        return;
    }
    // 交叉编译：优先拼前缀（x86_64-linux-gnu-gcc）
    if (t->cc_prefix && t->cc_prefix[0]) {
        // base 可能是路径，只取文件名部分
        const char* bn = base;
        for (const char* p = base; *p; p++) {
            if (*p == '/' || *p == '\\') bn = p + 1;
        }
        snprintf(out, out_len, "%s%s", t->cc_prefix, bn);
    } else {
        snprintf(out, out_len, "%s", base);
    }
}

// 链接参数：把「场景」翻译成工具链开关。
//
// ── 桌面档为什么要动子系统，而不是换入口函数 ────────────────
//
// Windows 的 PE 头里有个 Subsystem 字段，由加载器读取，运行期无法
// 修改。值为 3（CONSOLE）时系统给进程挂一个控制台窗口；值为 2
// （WINDOWS_GUI）时不给。默认 gcc（MinGW）生成的是 3，所以一个
// 双击就弹黑框的桌面程序，必须显式改这个字段。
//
// 传统做法是「subsystem=2 的 PE 必须用 WinMain」，于是代码要写两个
// 入口、或者在 main 外面包一层 WinMain 去调 main。这里不走那条路：
// 直接 `-Wl,--entry=mainCRTStartup`，让链接器用 CRT 的标准启动例程
// 但走 WINDOWS 子系统。CRT 的 mainCRTStartup 做完堆/全局初始化之后
// 会调 main(argc, argv)，于是**同一份 main() 在两种子系统下都成立**，
// 不用为桌面档写第二套入口，也不用为了拿命令行参数去调
// GetCommandLineW 自己切分（那个解析是错的温床，引号规则跟 CRT 不同）。
//
// 两条参数都实测过（tools 里留有对照实验）：`-mwindows` 是 MinGW
// 的语法糖，等价于 --subsystem,windows，但它**不改入口符号**，
// 于是链接期报 `undefined reference to WinMain`。所以必须两条都给。
//
// 非 Windows 目标：完全不加。ELF/Mach-O 没有「子系统」这个概念，
// 桌面程序与命令行程序在链接层面没有区别（窗口由 libX11/Cocoa
// 提供，那是普通库，不是加载器约定）。
static void add_profile_flags(char** argv, int* n, const XTarget* tgt,
                              const XProfileDef* prof) {
    if (!prof || !prof->gui_subsystem) return;
#if X_HOST_WINDOWS
    bool is_pe = (!tgt || target_is_host(tgt) || tgt->format == FMT_PE);
    if (!is_pe) return;
    argv[(*n)++] = (char*)"-Wl,--subsystem,windows";
    argv[(*n)++] = (char*)"-Wl,--entry=mainCRTStartup";
#else
    // 宿主不是 Windows 时，交叉编到 Windows 也需要这套开关，
    // 但那是「用 mingw 交叉工具链在 Linux 上产出 PE」的情形，
    // 目前工具链层不支持，所以这里不做空转。
    (void)argv; (void)n; (void)tgt;
#endif
}

int codegen_c_build(Bytecode* bc, const char* src_path, const char* out_path,
                    const XTarget* tgt, const char* cc_override,
                    const XProfileDef* prof) {
    // ---- 1. 生成 C 代码到临时文件 ----
    //
    // 临时文件放在**输出文件旁边**，而不是系统临时目录。
    //
    // 这个决定来自实测踩坑：系统 TEMP 在 Windows 上常常是
    // C:\Users\<用户名>\AppData\Local\Temp，用户名含中文时
    // MinGW 工具链会出问题（tools/BUILD.md 坑 3）；而且某些环境下
    // 往用户 Temp 目录写文件还会被安全策略拦。
    //
    // 放在输出旁边有三个好处：
    //   · 用户既然能往这里写产物，就一定能写中间文件
    //   · 路径与产物同源，不会出现「产物在 D 盘、临时文件在 C 盘」
    //     这种跨盘 mv 问题（跨盘 rename 会失败）
    //   · 编译失败时用户能在产物目录里直接看到那份 C 代码
    char c_path[1200];
    {
        // 从输出路径推出中间文件名：foo.exe → foo.exe.gen.c
        // 加 .gen 前缀是为了让它在目录列表里明确是「中间产物」，
        // 不会和用户自己的 foo.c 混淆。
        snprintf(c_path, sizeof(c_path), "%s.gen.c", out_path);
    }

    FILE* f = x_fopen(c_path, "wb");
    if (!f) {
        diag_error((SrcPos){ src_path, 0, 0 },
                   "无法创建临时 C 文件 `%s`", c_path);
        diag_help("检查 TEMP 环境变量是否指向可写目录。");
        return 1;
    }
    bool ok = gen_program(bc, f, prof);
    fclose(f);
    if (!ok) {
        x_remove(c_path);
        return 1;
    }

    // ---- 2. 选编译器 ----
    const char* base_cc = pick_cc(cc_override);
    char cc[512];
    cc_for_target(tgt, base_cc, cc, sizeof(cc));

    if (!cc_exists(cc)) {
        // 明确报错，不静默降级。这里的诊断必须把「缺什么」说清楚，
        // 因为交叉编译失败最常见的原因就是缺工具链。
        diag_error((SrcPos){ src_path, 0, 0 },
                   "找不到 C 编译器 `%s`", cc);
        if (tgt && !target_is_host(tgt)) {
            diag_note("目标平台是 %s，需要对应的交叉编译工具链。", tgt->triple);
            // Debian/Ubuntu 的包名是 gcc-<arch>-linux-gnu，即 cc_prefix
            // 去掉末尾那个连字符。原来这里写成「以 - 结尾就返回空串」，
            // 于是提示成了 `apt install gcc-`——少了最关键的部分，
            // 用户照着敲会得到一个毫无意义的包名。
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s",
                     tgt->cc_prefix ? tgt->cc_prefix : "");
            size_t plen = strlen(prefix);
            if (plen > 0 && prefix[plen - 1] == '-') prefix[plen - 1] = '\0';
            diag_help("安装交叉工具链，或用 `--cc=<path>` 指定。"
                      "例如 Debian/Ubuntu 上：apt install gcc-%s",
                      prefix[0] ? prefix : "<arch>-linux-gnu");
            diag_note("也可以先 `--emit=c` 生成 C 代码，拿到目标机器上自己编译。");
        } else {
            diag_help("安装 gcc 或 clang，或用 `--cc=<path>` 指定编译器路径。");
        }
        diag_note("生成的 C 代码保留在 %s，可以直接手工编译。", c_path);
        return 1;
    }

    // ---- 3. 组装编译命令 ----
    //
    // 参数设计要点：
    //   -O2            与解释档的 5~11× 差距主要来自这里
    //   -std=c11       生成的代码用了匿名 union，需要 C11
    //   -fno-strict-aliasing  生成代码里有通过 union 的类型双关，
    //                  虽然 C11 允许（union 成员互读是合法的），
    //                  但 gcc 的严格别名优化在某些边界情况下会误判
    //   -lm            fabs / isnan 需要
    //   -lgdi32 -luser32 -lmsimg32  仅当程序用到窗口能力时需要（见下）
    //   --subsystem / --entry  仅桌面档（见 add_profile_flags）
    //
    // argv 的容量按「最多参数」留足：编译选项 5 个 + target 1 个
    // + 源文件 1 个 + -o 1 个 + -lm 1 个 + 三个窗口库 + 两个档位开关
    // + NULL。留 16 个够用，但既然档位可能继续加参数，给到 20
    // 免得下次加东西时静默越界（这种越界在 Windows 上是栈污染，
    // 症状是「编译随机失败」，极难查）。
    char* argv[20];
    int n = 0;
    static char arg_o[1100];
    static char arg_std[] = "-std=c11";
    static char arg_opt[] = "-O2";
    static char arg_alias[] = "-fno-strict-aliasing";
    // -pipe：让 cc1 直接把汇编通过管道交给 as，不落 .o 到磁盘。
    //
    // 这一条是 Windows + 中文路径下唯一可靠的解法。试过但不够的：
    //   · 改 TMP/TEMP 环境变量 → as 仍把 .o 放在**当前工作目录**
    //   · 把源文件复制到 ASCII 目录 → 缓解但不彻底，-o 目标仍需写
    //   · 让 gcc 用 -save-temps=obj → 中间文件反而更多
    //
    // -pipe 之后 as 从 stdin 读汇编、只需写最终的 -o 目标，
    // 而那次写由我们的 out_path 决定，可以控制。
    // 代价：极少数情况下（磁盘满）错误信息不如落盘时清晰，可接受。
    static char arg_pipe[] = "-pipe";

    argv[n++] = cc;
    argv[n++] = arg_std;
    argv[n++] = arg_opt;
    argv[n++] = arg_alias;
    argv[n++] = arg_pipe;
    // 交叉编译目标明确指定 target 时，额外给 clang 用的 --target
    static char arg_target[256];
    if (tgt && !target_is_host(tgt) && strstr(cc, "clang")) {
        snprintf(arg_target, sizeof(arg_target), "--target=%s", tgt->triple);
        argv[n++] = arg_target;
    }
    argv[n++] = c_path;
    snprintf(arg_o, sizeof(arg_o), "-o%s", out_path);
    argv[n++] = arg_o;
    argv[n++] = (char*)"-lm";

    // 窗口库只在使用到窗口能力时加。
    //
    // ── 为什么按需加而不是无条件加 ───────────────────────────
    //
    // 无条件加会让所有程序（包括 hello world）都依赖 gdi32/user32。
    // 在 Windows 上这两个库总是存在，看不出问题；但交叉编译到
    // Linux 时 `-lgdi32` 会直接链接失败，把「这个程序根本不用窗口」
    // 的事实变成一个莫名其妙的错误。
    //
    // 判据用 bc_uses_gui 而不是「产物里有 xgui_ 符号」——后者要
    // 先编译一次才能知道，顺序上就绕不开了。
    if (bc_uses_gui(bc)) {
#if X_HOST_WINDOWS
        // 只在目标也是 Windows（或本机）时加。
        // 交叉编译到非 Windows 平台时 gui_win32.c 会走 #else 空实现，
        // 那时不需要这两个库，而且目标平台也没有它们。
        if (!tgt || target_is_host(tgt) || tgt->format == FMT_PE) {
            argv[n++] = (char*)"-lgdi32";
            argv[n++] = (char*)"-luser32";
            // v0.17 起窗口段还要 msimg32：GradientFill 与 AlphaBlend
            // 都在那个库里。漏了它报的是 `undefined reference to
            // __imp_GradientFill` —— 一个只在用到渐变时才出现的链接
            // 错误，很容易被当成「我的调用写错了」。
            //
            // 为什么无条件加（而不是只在用到 winGradient 时加）：
            // GradientFill 是唯一新增的外部符号，但 winAlpha 走的是
            // 预混色近似（见 gui_win32.c），不依赖 AlphaBlend。
            // 为一个库名做「是否用到」的判定不值得 —— msimg32 只有
            // 几 KB，而漏判的后果是链接失败。
            argv[n++] = (char*)"-lmsimg32";
        }
#endif
    }

    // TCP 库只在使用到 net* 内建时加。
    //
    // ── 这里踩过一个必须记下来的坑 ──────────────────────────
    //
    // 原来以为 `#pragma comment(lib, "ws2_32.lib")` 就够了 —— MSVC
    // 认它，MinGW **不认且不报错**。于是「用到了 TCP 的 AOT 产物」
    // 在 MinGW 下链接期报一串
    //     undefined reference to `__imp_WSAStartup`
    //     undefined reference to `__imp_recv`
    // 而源码里明明写着要这个库。这类「源码说做了、链接器说没做」
    // 的问题最费解，因为读代码的人会以为已经处理过了。
    //
    // 所以链接选项统一由这里显式加，pragma 一律不用。
    if (bc_uses_net(bc)) {
#if X_HOST_WINDOWS
        if (!tgt || target_is_host(tgt) || tgt->format == FMT_PE) {
            argv[n++] = (char*)"-lws2_32";
        }
#endif
        // 类 Unix 上 socket 在 libc 里，不需要额外库。
    }
    // 档位带来的链接开关（桌面档的子系统与入口符号）。
    add_profile_flags(argv, &n, tgt, prof);

    argv[n] = NULL;

    // ---- 调用编译器 ----
    //
    // 在中文路径的 Windows 上，这里有一个必须绕开的坑：
    // MinGW 的 as.exe 把 .o 中间文件放在**子进程的当前工作目录**，
    // 而不是 TMP。项目在 F://项目//... 时，无论怎么改 TMP/TEMP 都会得到
    //     Fatal error: can't create F://<乱码>//ccXXXXXX.o
    // 唯一可靠的办法是把子进程的 cwd 切到纯 ASCII 目录，并把源文件
    // 也放到那里。-spawnvp 做不到（它不控制 cwd），所以这一层
    // 走 plat_spawn（内部用 CreateProcess）。
    //
    // 类 Unix 上没有这个问题（文件系统本来就是字节串），
    // plat_spawn 会退化成普通的 posix_spawnp。
    int rc;

    // 产物的最终落点。中文路径在这里是个硬约束：
    // ld.exe 打不开含非 ASCII 字节的输出路径（实测
    // `cannot open output file C:/xtmp/<乱码>/out.exe`），
    // 而我们的 x_fopen 走 UTF-8 感知的宽字符 API，能写。
    // 所以策略是「ld 在 ASCII 目录里生成 → 我们自己的文件 API 搬过去」。
    char abs_out[1200];        // 用户要的最终路径（绝对）
    char build_out[1200];      // ld 实际写入的路径（保证 ASCII）
    char cwd[1300];
    char workdir[1300];
    workdir[0] = '\0';

    plat_abs_path(out_path, abs_out, sizeof(abs_out));

#if X_HOST_WINDOWS
    // 选编译目录的优先级：
    //   1) 产物自身的目录，若已是纯 ASCII（最常见情形）
    //   2) C:/xtmp 等已知可写的 ASCII 目录
    //
    // 注意 out_path 可能是裸文件名（如 `01-hello.exe`），此时目录是「当前
    // 目录」。必须先取绝对路径再判断 ASCII —— 直接用裸名判断会得到 "."，
    // 而 "." 当然是 ASCII，于是误判成「当前目录可用」，可它的真实路径
    // 可能满是中文。这个误判是实际踩到的 bug。
    snprintf(cwd, sizeof(cwd), "%s", abs_out);
    {
        char* slash = strrchr(cwd, '/');
        char* bslash = strrchr(cwd, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) *slash = '\0';
        else snprintf(cwd, sizeof(cwd), ".");
    }

    if (plat_is_ascii(cwd)) {
        snprintf(workdir, sizeof(workdir), "%s", cwd);
    } else {
        static const char* cands[] = { "C:/xtmp", "C:/Temp", "C:/Windows/Temp", NULL };
        for (int ci = 0; cands[ci]; ci++) {
            char probe[1200];
            snprintf(probe, sizeof(probe), "%s/.xprobe", cands[ci]);
            FILE* pf = x_fopen(probe, "wb");
            if (pf) { fclose(pf); x_remove(probe);
                      snprintf(workdir, sizeof(workdir), "%s", cands[ci]);
                      break; }
        }
    }

    if (!workdir[0]) {
        diag_error((SrcPos){ src_path, 0, 0 },
                   "找不到可用于编译的纯 ASCII 目录");
        diag_note("MinGW 的 cc1/as 在处理非 ASCII 路径时会产出不存在的中间"
                  "文件名，所以编译必须在纯 ASCII 目录下进行。");
        diag_help("创建 C:/xtmp 目录，或把产物输出到纯 ASCII 路径下，"
                  "例如 -o C:/out/app.exe");
        x_remove(c_path);
        return 1;
    }

    // ld 的产物名固定在 ASCII 目录里；用完搬到 abs_out。
    // 用进程号做后缀，两个 x.exe 同时编译不会互相踩。
    snprintf(build_out, sizeof(build_out), "%s/xgen_%ld.exe", workdir, plat_pid());

    // 源文件也复制到编译目录。虽然 with_tmp 已经把 TEMP 指过去了，
    // 但源文件路径本身含中文时 cc1 读它同样会失败（它的路径处理
    // 也是 ANSI 的）。这一步不能省。
    {
        char ascii_c[1200];
        snprintf(ascii_c, sizeof(ascii_c), "%s/xgen_%ld.c", workdir, plat_pid());
        bool copied = false;
        FILE* fin  = x_fopen(c_path, "rb");
        FILE* fout = x_fopen(ascii_c, "wb");
        if (fin && fout) {
            char buf[8192];
            size_t nb;
            copied = true;
            while ((nb = fread(buf, 1, sizeof(buf), fin)) > 0) {
                if (fwrite(buf, 1, nb, fout) != nb) { copied = false; break; }
            }
        }
        if (fin)  fclose(fin);
        if (fout) fclose(fout);

        if (!copied) {
            diag_error((SrcPos){ src_path, 0, 0 },
                       "无法把生成的 C 代码写入编译目录 `%s`", ascii_c);
            diag_help("检查该目录的写权限。");
            x_remove(c_path);
            return 1;
        }

        // 改写 argv 里的源与产物路径为绝对路径（cwd 要切到 workdir）
        static char fix_c[1200];
        static char fix_o[1300];
        snprintf(fix_c, sizeof(fix_c), "%s", ascii_c);
        snprintf(fix_o, sizeof(fix_o), "-o%s", build_out);
        for (int i = 0; argv[i]; i++) {
            if (argv[i] == c_path)     argv[i] = fix_c;
            else if (argv[i] == arg_o) argv[i] = fix_o;
        }
    }

    rc = plat_spawn_with_tmp(argv, workdir, workdir);
    {
        char ascii_c[1200];
        snprintf(ascii_c, sizeof(ascii_c), "%s/xgen_%ld.c", workdir, plat_pid());
        x_remove(ascii_c);
    }
#else
    // 类 Unix：文件系统是字节串，直接原地编译，产物就写在用户要的位置。
    snprintf(build_out, sizeof(build_out), "%s", abs_out);
    rc = plat_spawn_with_tmp(argv, NULL, NULL);
#endif

    // 把 ASCII 目录里的产物搬到用户要的位置。
    //
    // 跨盘 rename 会失败（EXDEV），所以不能直接用 x_rename；
    // 统一走「流式复制 + 删除源」，代价是一次多余的文件写入，
    // 换来的是在任何盘符组合下都成立。
    if (rc == 0 && strcmp(build_out, abs_out) != 0) {
        FILE* fin  = x_fopen(build_out, "rb");
        FILE* fout = x_fopen(abs_out, "wb");
        bool moved = false;
        if (fin && fout) {
            char buf[65536];
            size_t nb;
            moved = true;
            while ((nb = fread(buf, 1, sizeof(buf), fin)) > 0) {
                if (fwrite(buf, 1, nb, fout) != nb) { moved = false; break; }
            }
        }
        if (fin)  fclose(fin);
        if (fout) fclose(fout);

        if (!moved) {
            diag_error((SrcPos){ src_path, 0, 0 },
                       "编译成功，但无法把产物写到 `%s`", abs_out);
            diag_help("检查目标目录的写权限。");
            if (fin)  { /* 已关 */ }
            x_remove(build_out);
            x_remove(c_path);
            return 1;
        }
        x_remove(build_out);
    }

    // ---- 4. 产物自检 ----
    //
    // 交叉编译配错工具链时，gcc 常常「成功」退出但产出的是宿主格式。
    // 只看退出码会全绿，把产物拿到目标机器上才发现跑不了。
    // 读一次文件头只要几行，能挡住整类事故。
    if (rc == 0) {
        XObjFormat got = fmt_detect_file(out_path);
        if (got == FMT_UNKNOWN) {
            diag_error((SrcPos){ src_path, 0, 0 },
                       "编译器报告成功，但 `%s` 不是可识别的可执行文件", out_path);
            diag_note("生成的可执行文件格式无法识别（读不出 PE/ELF/Mach-O 文件头）。");
            diag_help("可能是链接步骤静默失败。检查 `%s` 是否真是 C 编译器。", cc);
            rc = 1;
        } else if (got != tgt->format) {
            diag_error((SrcPos){ src_path, 0, 0 },
                       "产物格式不符：期望 %s，实际 %s",
                       fmt_name(tgt->format), fmt_name(got));
            diag_note("目标平台 %s 的可执行文件必须是 %s 格式。",
                      tgt->triple, fmt_name(tgt->format));
            diag_help("这通常是交叉工具链实际产出的是宿主格式。检查 `%s` 是否"
                      "真的是针对 %s 的交叉编译器（用 `%s --version` 看目标）。",
                      cc, tgt->triple, cc);
            diag_note("快捷验证：`%s -dumpmachine` 会打印它默认的目标三元组。", cc);
            rc = 1;
        }
    }

    // ---- 5. 清理与报告 ----
    if (rc == 0) {
        printf("已生成 %s  （%s，%d 条指令 → %s）\n",
               out_path, fmt_name(tgt->format), bc->code_count, tgt->triple);
        x_remove(c_path);
    } else {
        diag_error((SrcPos){ src_path, 0, 0 },
                   "C 编译器返回 %d，编译失败", rc);
        diag_note("生成的 C 代码保留在 %s，便于定位。", c_path);
        diag_help("先手工跑一遍 `%s %s -o test` 看真实错误。", cc, c_path);
    }
    return rc == 0 ? 0 : 1;
}