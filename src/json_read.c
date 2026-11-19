// json_read.c — 极简 JSON 读取（见 json_read.h 的说明）

#include "json_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 内存：一次解析一块 arena，整块丢弃 ──────────────────────
//
// 用简单的「大块 + 线性分配」而不是逐节点 malloc：
// LSP 每条消息解析一次，生命周期极短且边界清晰。线性分配不会漏，
// 也不会在出错路径上留下半棵树。
typedef struct {
    char*  buf;
    size_t cap;
    size_t used;
} Arena2;

#define ARENA2_CHUNK (64 * 1024)

static void* a2_alloc(Arena2* a, size_t n) {
    // 8 字节对齐
    n = (n + 7) & ~(size_t)7;
    if (a->used + n > a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : ARENA2_CHUNK;
        while (ncap < a->used + n) ncap *= 2;
        char* nb = (char*)realloc(a->buf, ncap);
        if (!nb) return NULL;
        a->buf = nb;
        a->cap = ncap;
    }
    void* p = a->buf + a->used;
    a->used += n;
    memset(p, 0, n);
    return p;
}

static void a2_free(Arena2* a) {
    free(a->buf);
    a->buf = NULL;
    a->cap = a->used = 0;
}

// ── 解析器状态 ──────────────────────────────────────────────
typedef struct {
    char*   p;
    char*   end;
    Arena2* a;
    char*   err;
    size_t  errlen;
    bool    failed;
    int     line;
} P;

static void fail(P* p, const char* msg) {
    if (!p->failed) {
        p->failed = true;
        if (p->err && p->errlen) {
            snprintf(p->err, p->errlen, "第 %d 行：%s", p->line, msg);
        }
    }
}

static void skip_ws(P* p) {
    while (p->p < p->end) {
        char c = *p->p;
        if (c == '\n') { p->line++; p->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') p->p++;
        else break;
    }
}

static Jv* parse_value(P* p, int depth);

// ── 字符串：原地去转义 ──────────────────────────────────────
//
// 返回值是**串首**，不是写指针。这两者一开始重合，随着去转义推进
// dst 只会往后退或持平，于是 strlen(串首) 就是解码后的长度。
// 早先这里直接 return dst，等于返回串尾 —— 于是每个字符串都成了空串，
// 而 kind 全对，只有内容空。这个 bug 在 LSP 里的表现是「收到消息、
// 所有键都读成空」，非常难往回推。
static char* parse_string_raw(P* p) {
    if (p->p >= p->end || *p->p != '"') { fail(p, "期望字符串"); return NULL; }
    p->p++;
    char* start = p->p;
    char* dst = p->p;
    char* src = p->p;

    while (src < p->end) {
        char c = *src;
        if (c == '"') {
            *dst = '\0';
            p->p = src + 1;
            return start;
        }
        if (c == '\\') {
            src++;
            if (src >= p->end) { fail(p, "字符串在转义符后意外结束"); return NULL; }
            char e = *src++;
            switch (e) {
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case '/':  *dst++ = '/';  break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'n':  *dst++ = '\n'; break;
            case 'r':  *dst++ = '\r'; break;
            case 't':  *dst++ = '\t'; break;
            case 'u': {
                // \uXXXX → UTF-8
                //
                // LSP 的位置用的是 UTF-16 码元，而 JSON 里 \u 也是
                // UTF-16 —— 这里只处理 BMP（常见情形）。代理对
                // （surrogate pair）在 LSP 消息里只可能出现在
                // 文件路径/文本中，而那种情况实际是直接传 UTF-8
                // 明文，不是 \u 转义。
                if (src + 4 > p->end) { fail(p, "\\u 后不足 4 位"); return NULL; }
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = src[k];
                    unsigned d;
                    if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                    else { fail(p, "\\u 后不是十六进制"); return NULL; }
                    cp = cp * 16 + d;
                }
                src += 4;
                if (cp < 0x80) {
                    *dst++ = (char)cp;
                } else if (cp < 0x800) {
                    *dst++ = (char)(0xC0 | (cp >> 6));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *dst++ = (char)(0xE0 | (cp >> 12));
                    *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                fail(p, "不认识的转义");
                return NULL;
            }
        } else {
            if (c == '\n') p->line++;
            *dst++ = c;
            src++;
        }
    }
    fail(p, "字符串没有闭合");
    return NULL;
}

static Jv* new_jv(P* p, JvKind k) {
    Jv* v = (Jv*)a2_alloc(p->a, sizeof(Jv));
    if (!v) { fail(p, "内存不足"); return NULL; }
    v->kind = k;
    v->line = p->line;
    return v;
}

static Jv* parse_object(P* p, int depth) {
    Jv* v = new_jv(p, JV_OBJ);
    if (!v) return NULL;
    p->p++;   // 吃掉 '{'
    skip_ws(p);

    if (p->p < p->end && *p->p == '}') { p->p++; return v; }

    int cap = 0;
    for (;;) {
        skip_ws(p);
        char* key = parse_string_raw(p);
        if (!key) return NULL;

        skip_ws(p);
        if (p->p >= p->end || *p->p != ':') { fail(p, "键之后期望 ':'"); return NULL; }
        p->p++;
        skip_ws(p);

        Jv* val = parse_value(p, depth + 1);
        if (!val) return NULL;

        if (v->n >= cap) {
            int ncap = cap ? cap * 2 : 8;
            Jv** ni = (Jv**)a2_alloc(p->a, sizeof(Jv*) * (size_t)ncap);
            char** nk = (char**)a2_alloc(p->a, sizeof(char*) * (size_t)ncap);
            if (!ni || !nk) { fail(p, "内存不足"); return NULL; }
            if (v->n > 0) {
                memcpy(ni, v->items, sizeof(Jv*) * (size_t)v->n);
                memcpy(nk, v->keys, sizeof(char*) * (size_t)v->n);
            }
            v->items = ni;
            v->keys = nk;
            cap = ncap;
        }
        v->keys[v->n] = key;
        v->items[v->n] = val;
        v->n++;

        skip_ws(p);
        if (p->p >= p->end) { fail(p, "对象没有闭合"); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == '}') { p->p++; return v; }
        fail(p, "对象里期望 ',' 或 '}'");
        return NULL;
    }
}

static Jv* parse_array(P* p, int depth) {
    Jv* v = new_jv(p, JV_ARR);
    if (!v) return NULL;
    p->p++;   // 吃掉 '['
    skip_ws(p);

    if (p->p < p->end && *p->p == ']') { p->p++; return v; }

    int cap = 0;
    for (;;) {
        skip_ws(p);
        Jv* val = parse_value(p, depth + 1);
        if (!val) return NULL;

        if (v->n >= cap) {
            int ncap = cap ? cap * 2 : 8;
            Jv** ni = (Jv**)a2_alloc(p->a, sizeof(Jv*) * (size_t)ncap);
            if (!ni) { fail(p, "内存不足"); return NULL; }
            if (v->n > 0) memcpy(ni, v->items, sizeof(Jv*) * (size_t)v->n);
            v->items = ni;
            cap = ncap;
        }
        v->items[v->n++] = val;

        skip_ws(p);
        if (p->p >= p->end) { fail(p, "数组没有闭合"); return NULL; }
        if (*p->p == ',') { p->p++; continue; }
        if (*p->p == ']') { p->p++; return v; }
        fail(p, "数组里期望 ',' 或 ']'");
        return NULL;
    }
}

static Jv* parse_number(P* p) {
    Jv* v = new_jv(p, JV_NUM);
    if (!v) return NULL;
    bool neg = false;
    if (p->p < p->end && *p->p == '-') { neg = true; p->p++; }
    long long x = 0;
    bool any = false;
    while (p->p < p->end && *p->p >= '0' && *p->p <= '9') {
        x = x * 10 + (*p->p - '0');
        p->p++;
        any = true;
    }
    if (!any) { fail(p, "数字格式错误"); return NULL; }
    // 小数与指数：LSP 里不用，但要能吃掉，否则解析会在合法 JSON 上失败
    if (p->p < p->end && *p->p == '.') {
        p->p++;
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-')) p->p++;
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    v->i = neg ? -x : x;
    return v;
}

static bool lit(P* p, const char* s) {
    size_t n = strlen(s);
    if ((size_t)(p->end - p->p) < n) return false;
    if (memcmp(p->p, s, n) != 0) return false;
    p->p += n;
    return true;
}

static Jv* parse_value(P* p, int depth) {
    if (depth > 200) { fail(p, "嵌套太深"); return NULL; }
    skip_ws(p);
    if (p->p >= p->end) { fail(p, "意外的结尾"); return NULL; }

    char c = *p->p;
    if (c == '{') return parse_object(p, depth);
    if (c == '[') return parse_array(p, depth);
    if (c == '"') {
        Jv* v = new_jv(p, JV_STR);
        if (!v) return NULL;
        v->s = parse_string_raw(p);
        if (!v->s) return NULL;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (lit(p, "true"))  { Jv* v = new_jv(p, JV_BOOL); if (v) v->b = true;  return v; }
    if (lit(p, "false")) { Jv* v = new_jv(p, JV_BOOL); if (v) v->b = false; return v; }
    if (lit(p, "null"))  return new_jv(p, JV_NULL);

    fail(p, "无法识别的值");
    return NULL;
}

Jv* jv_parse(char* buf, size_t len, char* err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    Arena2 a;
    memset(&a, 0, sizeof(a));

    P p;
    memset(&p, 0, sizeof(p));
    p.p = buf;
    p.end = buf + len;
    p.a = &a;
    p.err = err;
    p.errlen = err_len;
    p.line = 1;

    Jv* root = parse_value(&p, 0);
    if (p.failed) { a2_free(&a); return NULL; }

    // 顶层之后的空白可以接受，其它内容不行
    skip_ws(&p);
    if (p.p != p.end) {
        fail(&p, "JSON 结束后还有多余内容");
        a2_free(&a);
        return NULL;
    }

    // ── arena 的所有权 ────────────────────────────────────
    //
    // 这里有个所有权问题：整棵树都在 arena 里，而 arena 是局部变量。
    // 函数返回后 a 本身没了，但 a.buf 指向的堆内存还在 —— 只要不
    // free 就有效。代价是调用方无处释放它。
    //
    // 对这个程序是可接受的：LSP 每条消息解析一次，进程寿命跟着
    // 编辑器，而单条消息最大 8 MB（我们对 Content-Length 设了上限）。
    // 真正要解决泄漏的话应该把 arena 交给调用方，但那样每个调用点
    // 都要写一遍释放，而其中有几个是错误路径 —— 错误路径上漏释放
    // 才是这里最可能发生的事。
    //
    // 折中：用一个静态指针记录最近一次的 arena，下次解析时释放
    // 上一个。这样任意时刻最多只有一份 arena 活着，正确性上等价于
    // 「只保留最后一条消息的树」，而 LSP 的请求/响应模型正是如此。
    static Arena2* g_live = NULL;
    static Arena2  g_store;
    if (g_live) a2_free(g_live);
    g_store = a;
    g_live = &g_store;

    return root;
}

Jv* jv_get(const Jv* obj, const char* key) {
    if (!obj || obj->kind != JV_OBJ || !key) return NULL;
    for (int i = 0; i < obj->n; i++) {
        if (obj->keys[i] && strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

const char* jv_str(const Jv* v) {
    if (!v || v->kind != JV_STR) return NULL;
    return v->s;
}

long long jv_int(const Jv* v, long long def) {
    if (!v) return def;
    if (v->kind == JV_NUM) return v->i;
    if (v->kind == JV_BOOL) return v->b ? 1 : 0;
    return def;
}

int jv_len(const Jv* v) {
    if (!v || (v->kind != JV_ARR && v->kind != JV_OBJ)) return 0;
    return v->n;
}

Jv* jv_at(const Jv* v, int i) {
    if (!v || v->kind != JV_ARR) return NULL;
    if (i < 0 || i >= v->n) return NULL;
    return v->items[i];
}