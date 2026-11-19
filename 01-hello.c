/* 由 X 编译器生成。不要手工编辑。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>

/* ---------- 值 ---------- */
typedef enum { VT_NULL=0, VT_INT, VT_FLOAT, VT_BOOL, VT_STRING } VType;

typedef struct XS {
    long   rc;
    size_t len;
    char   d[];
} XS;

typedef struct { VType t; union { int64_t i; double f; bool b; XS* s; }; } V;

static void* xm(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) { fputs("fatal: out of memory\n", stderr); exit(70); }
    return p;
}

static XS* xs_new(const char* src, size_t n) {
    XS* s = (XS*)xm(sizeof(XS) + n + 1);
    s->rc = 1; s->len = n;
    if (n) memcpy(s->d, src, n);
    s->d[n] = 0;
    return s;
}
static XS* xs_retain(XS* s) { if (s) s->rc++; return s; }
static void xs_rel(XS* s) { if (s && --s->rc <= 0) free(s); }
static XS* xs_cat(XS* a, XS* b) {
    size_t n = a->len + b->len;
    XS* s = (XS*)xm(sizeof(XS) + n + 1);
    s->rc = 1; s->len = n;
    memcpy(s->d, a->d, a->len);
    memcpy(s->d + a->len, b->d, b->len);
    s->d[n] = 0;
    return s;
}

static V v_int(int64_t i)   { V v; v.t = VT_INT;   v.i = i; return v; }
static V v_flt(double f)    { V v; v.t = VT_FLOAT; v.f = f; return v; }
static V v_bool(bool b)     { V v; v.t = VT_BOOL;  v.b = b; return v; }
static V v_null(void)       { V v; v.t = VT_NULL;  v.i = 0; return v; }
static V v_str(XS* s)       { V v; v.t = VT_STRING; v.s = s; return v; }

static void v_rel(V* v) {
    if (v->t == VT_STRING) { xs_rel(v->s); v->s = 0; }
    v->t = VT_NULL;
}

static bool v_true(V v) {
    switch (v.t) {
        case VT_NULL:   return false;
        case VT_BOOL:   return v.b;
        case VT_INT:    return v.i != 0;
        case VT_FLOAT:  return v.f != 0.0;
        case VT_STRING: return v.s && v.s->len > 0;
    }
    return false;
}

static const char* v_tname(VType t) {
    switch (t) {
        case VT_NULL: return "Null";
        case VT_INT: return "Int";
        case VT_FLOAT: return "Float";
        case VT_BOOL: return "Bool";
        case VT_STRING: return "String";
    }
    return "?";
}

/* 浮点转字符串：最短可往返。与解释器用同一套算法，
   否则两档在 println(0.1) 上会给出不同位数。 */
static XS* xs_flt(double f) {
    if (f != f) return xs_new("NaN", 3);
    if (f > 1e308 * 10) return xs_new("Infinity", 8);
    if (f < -1e308 * 10) return xs_new("-Infinity", 9);
    char buf[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof(buf), "%.*g", p, f);
        if (strtod(buf, 0) == f) break;
    }
    if (!strpbrk(buf, ".eEni")) strncat(buf, ".0", sizeof(buf) - strlen(buf) - 1);
    return xs_new(buf, strlen(buf));
}

static XS* xs_int(int64_t i) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)i);
    return xs_new(buf, (size_t)n);
}

static XS* v_str_of(V v) {
    switch (v.t) {
        case VT_NULL:   return xs_new("null", 4);
        case VT_INT:    return xs_int(v.i);
        case VT_FLOAT:  return xs_flt(v.f);
        case VT_BOOL:   return v.b ? xs_new("true", 4) : xs_new("false", 5);
        case VT_STRING: return xs_retain(v.s);
    }
    return xs_new("?", 1);
}

static bool v_as_flt(V* v, double* out) {
    if (v->t == VT_INT)   { *out = (double)v->i; return true; }
    if (v->t == VT_FLOAT) { *out = v->f; return true; }
    return false;
}

/* ---------- 溢出检查 ---------- */
static bool ovf_add(int64_t a, int64_t b, int64_t* r) { return __builtin_add_overflow(a, b, r); }
static bool ovf_sub(int64_t a, int64_t b, int64_t* r) { return __builtin_sub_overflow(a, b, r); }
static bool ovf_mul(int64_t a, int64_t b, int64_t* r) { return __builtin_mul_overflow(a, b, r); }

/* ---------- 诊断 ---------- */
/* 错误信息里带源码位置，格式与解释档完全一致，
   这样同一个程序在两档下报的错是同一份文本。
   注意 g_file 必须在 rt_err 定义之前声明——它被 rt_err 引用，
   而 C 要求先声明后使用。真正的文件名在 main 里赋值。 */
static const char* g_file = "?";
static int g_line = 0, g_col = 0;

static void rt_err(const char* msg) {
    fflush(stdout);
    fprintf(stderr, "\n  %s:%d:%d\n", g_file, g_line, g_col);
    fprintf(stderr, "  error: %s\n", msg);
    exit(70);
}

static void ovf_err(const char* op) {
    fflush(stdout);
    fprintf(stderr, "\n  %s:%d:%d\n", g_file, g_line, g_col);
    fprintf(stderr, "  error: 整数溢出：`%s` 的结果超出 Int 的表示范围\n", op);
    fprintf(stderr, "  note: Int 是 64 位有符号整数（01 \302�5.4）\n");
    fprintf(stderr, "  help: 改用 Float，或用 `+%%` 显式要求回绕\n");
    exit(70);
}

static void x_set_file(void) { g_file = "tests/p0/01-hello.xla"; }

static V xf_0(V* s);


/* main/0  locals=0 */
static V xf_0(V* s) {
    for (int i = 0; i < 0; i++) s[i].t = VT_NULL;
    V t;
    (void)t;
L0_0: ;
    g_line = 8; g_col = 9;
    s[0] = v_str(xs_new("Hello, X!", 9));
L0_1: ;
    g_line = 8; g_col = 9;
    if (s[0].t == VT_STRING) fwrite(s[0].s->d, 1, s[0].s->len, stdout);
    else { XS* p_ = v_str_of(s[0]); fwrite(p_->d, 1, p_->len, stdout); xs_rel(p_); }
    v_rel(&s[0]);
L0_2: ;
    g_line = 8; g_col = 8;
    s[0] = v_str(xs_new("\n", 1));
L0_3: ;
    g_line = 8; g_col = 8;
    if (s[0].t == VT_STRING) fwrite(s[0].s->d, 1, s[0].s->len, stdout);
    else { XS* p_ = v_str_of(s[0]); fwrite(p_->d, 1, p_->len, stdout); xs_rel(p_); }
    v_rel(&s[0]);
L0_4: ;
    g_line = 8; g_col = 8;
    s[0] = v_null();
L0_5: ;
    g_line = 8; g_col = 1;
    v_rel(&s[0]);
L0_6: ;
    g_line = 9; g_col = 9;
    s[0] = v_str(xs_new("\344\275\240\345\245\275\357\274\214\344\270\226\347\225\214\357\274\201", 18));
L0_7: ;
    g_line = 9; g_col = 9;
    if (s[0].t == VT_STRING) fwrite(s[0].s->d, 1, s[0].s->len, stdout);
    else { XS* p_ = v_str_of(s[0]); fwrite(p_->d, 1, p_->len, stdout); xs_rel(p_); }
    v_rel(&s[0]);
L0_8: ;
    g_line = 9; g_col = 8;
    s[0] = v_str(xs_new("\n", 1));
L0_9: ;
    g_line = 9; g_col = 8;
    if (s[0].t == VT_STRING) fwrite(s[0].s->d, 1, s[0].s->len, stdout);
    else { XS* p_ = v_str_of(s[0]); fwrite(p_->d, 1, p_->len, stdout); xs_rel(p_); }
    v_rel(&s[0]);
L0_10: ;
    g_line = 9; g_col = 8;
    s[0] = v_null();
L0_11: ;
    g_line = 9; g_col = 1;
    v_rel(&s[0]);
L0_12: ;
    g_line = 8; g_col = 1;
    s[0] = v_int(0LL);
L0_13: ;
    g_line = 8; g_col = 1;
    { V r_ = s[0];
      for (int i_ = 0; i_ < 0; i_++) v_rel(&s[i_]);
      return r_; }
    return v_null();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    x_set_file();
    static V s[4096];
    for (long i = 0; i < 4096; i++) s[i].t = VT_NULL;
    for (int i = 0; i < 0; i++) s[i].t = VT_NULL;
    V r = xf_0(s);
    int code = (r.t == VT_INT) ? (int)(r.i & 0xFF) : 0;
    v_rel(&r);
    fflush(stdout);
    return code;
}
