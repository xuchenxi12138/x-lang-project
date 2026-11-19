// semver.c — 语义化版本与约束的实现

#include "semver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// 解析
// ============================================================

// 读一段数字，返回读到的值；*pp 前移。
// 没有数字可读时返回 -1（用来区分「读了 0」和「没读到」）。
static int read_num(const char** pp, int* parts) {
    const char* p = *pp;
    if (!isdigit((unsigned char)*p)) return -1;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        // 溢出保护。版本号的某一段超过 100 万，基本可以断定写错了。
        if (v > 1000000L) v = 1000000L;
        p++;
    }
    (*parts)++;
    *pp = p;
    return (int)v;
}

bool sem_parse(const char* s, SemVer* out) {
    if (!out) return false;
    out->major = out->minor = out->patch = 0;
    out->pre[0] = '\0';
    if (!s) return false;

    // 跳过前导空白与可选的 'v' 前缀。
    //
    // 'v' 是 git tag 的惯例（v1.2.3），用户会顺手写进清单。
    // 因为一个 v 就报「版本号格式错误」不值得。
    const char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'v' || *p == 'V') {
        // 只有当 v 后面紧跟数字时才当版本前缀。
        // 否则 "version" 这种词会被误吃。
        if (isdigit((unsigned char)p[1])) p++;
    }

    int parts = 0;
    int a = read_num(&p, &parts);
    if (a < 0) return false;
    out->major = a;

    if (*p == '.') {
        p++;
        int b = read_num(&p, &parts);
        if (b < 0) return false;        // "1." 这种
        out->minor = b;
        if (*p == '.') {
            p++;
            int c = read_num(&p, &parts);
            if (c < 0) return false;    // "1.2." 这种
            out->patch = c;
        }
    }

    // 预发布标识：-之后到结尾（或 + 之前）
    if (*p == '-') {
        p++;
        const char* st = p;
        while (*p && *p != '+' && *p != ' ') p++;
        size_t L = (size_t)(p - st);
        if (L == 0) return false;       // "1.2.3-" 空标识
        if (L >= sizeof(out->pre)) L = sizeof(out->pre) - 1;
        memcpy(out->pre, st, L);
        out->pre[L] = '\0';
    }

    // 构建元数据（+xxx）直接丢弃：按 semver 规范它不参与比较。
    while (*p == ' ' || *p == '\t') p++;
    if (*p && *p != '+') return false;  // 后面还有垃圾字符

    (void)parts;
    return true;
}

// 预发布标识的比较。
//
// semver 的规则：
//   · 没有预发布标识 > 有预发布标识（1.0.0 > 1.0.0-rc1）
//   · 两边都有时，逐段用 '.' 分隔比较
//   · 纯数字段按数值比，其它按字典序比
//   · 数字段 < 字母段（1.0.0-1 < 1.0.0-alpha）
//   · 段数少的在前时更小（1.0.0-alpha < 1.0.0-alpha.1）
static int cmp_pre(const char* a, const char* b) {
    bool ea = (a[0] == '\0');
    bool eb = (b[0] == '\0');
    if (ea && eb) return 0;
    // 空的大于非空的
    if (ea) return 1;
    if (eb) return -1;

    while (*a && *b) {
        // 取一段
        const char* as = a;
        while (*a && *a != '.') a++;
        size_t al = (size_t)(a - as);

        const char* bs = b;
        while (*b && *b != '.') b++;
        size_t bl = (size_t)(b - bs);

        bool an = true, bn = true;
        for (size_t i = 0; i < al; i++) if (!isdigit((unsigned char)as[i])) { an = false; break; }
        for (size_t i = 0; i < bl; i++) if (!isdigit((unsigned char)bs[i])) { bn = false; break; }

        int r;
        if (an && bn) {
            // 都纯数字：按数值比。手写循环而不是 strtol，避免溢出。
            // 去前导零后先比长度，长度相同再逐字节比 —— 这就是大数比较。
            size_t az = 0, bz = 0;
            while (az + 1 < al && as[az] == '0') az++;
            while (bz + 1 < bl && bs[bz] == '0') bz++;
            size_t an2 = al - az, bn2 = bl - bz;
            if (an2 != bn2) r = (an2 < bn2) ? -1 : 1;
            else r = memcmp(as + az, bs + bz, an2);
            if (r != 0) return r < 0 ? -1 : 1;
        } else if (an != bn) {
            // 数字段 < 字母段
            return an ? -1 : 1;
        } else {
            size_t m = al < bl ? al : bl;
            r = memcmp(as, bs, m);
            if (r != 0) return r < 0 ? -1 : 1;
            if (al != bl) return (al < bl) ? -1 : 1;
        }

        // 跳过点号
        if (*a == '.') a++;
        if (*b == '.') b++;
        // 一方还有、另一方没了 → 段数少的更小
        if (*a && !*b) return 1;
        if (!*a && *b) return -1;
    }
    return 0;
}

int sem_cmp(const SemVer* a, const SemVer* b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return cmp_pre(a->pre, b->pre);
}

// ============================================================
// 约束
// ============================================================

bool sem_parse_req(const char* s, SemReq* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->op = SV_ANY;
    out->parts = 0;
    out->raw[0] = '\0';
    if (!s) return true;

    const char* p = s;
    while (*p == ' ' || *p == '\t') p++;

    // 原始文本留档（去掉首尾空白）
    {
        const char* e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
        size_t L = (size_t)(e - p);
        if (L >= sizeof(out->raw)) L = sizeof(out->raw) - 1;
        memcpy(out->raw, p, L);
        out->raw[L] = '\0';
    }

    if (*p == '\0' || (*p == '*' && p[1] == '\0')) {
        out->op = SV_ANY;
        return true;
    }

    // 运算符
    if (*p == '=')      { out->op = SV_EXACT;  p++; }
    else if (*p == '^') { out->op = SV_CARET;  p++; }
    else if (*p == '~') { out->op = SV_TILDE;  p++; }
    else if (*p == '>' && p[1] == '=') { out->op = SV_GE; p += 2; }
    else if (*p == '<' && p[1] == '=') { out->op = SV_LE; p += 2; }
    else if (*p == '>') { out->op = SV_GT; p++; }
    else if (*p == '<') { out->op = SV_LT; p++; }
    else {
        // 没有运算符 → 裸版本号，默认 ^（见头文件里的论证）
        out->op = SV_CARET;
    }

    while (*p == ' ' || *p == '\t') p++;

    // 数一下写了几段数字（^ 和 ~ 算上界要用）
    {
        int parts = 0;
        const char* q = p;
        if (*q == 'v' || *q == 'V') { if (isdigit((unsigned char)q[1])) q++; }
        int a = read_num(&q, &parts);
        if (a >= 0) {
            if (*q == '.') {
                q++;
                if (read_num(&q, &parts) >= 0 && *q == '.') {
                    q++;
                    read_num(&q, &parts);
                }
            }
        }
        out->parts = parts;
    }

    if (!sem_parse(p, &out->ver)) return false;
    // sem_parse 允许 "1" 这种，parts 由上面的扫描给出；
    // 但如果 sem_parse 成功而 parts==0，说明有蹊跷，兜个底。
    if (out->parts <= 0) out->parts = 3;
    return true;
}

bool sem_satisfies(const SemVer* v, const SemReq* r) {
    if (!v || !r) return false;

    // ── 预发布版本的特殊规则 ────────────────────────────────
    //
    // v 带预发布标识时，只有「约束本身也带预发布标识、且主版本号
    // 部分相同」才算匹配。否则 ^1.2.0 会匹配 1.3.0-rc1 —— 那是个
    // 没跑完测试的版本。
    if (v->pre[0] != '\0' && r->op != SV_ANY) {
        if (r->ver.pre[0] == '\0') return false;
    }
    // SV_ANY（"*"）匹配一切，包括预发布 —— 用户明说「随便」就真的随便。

    int c = sem_cmp(v, &r->ver);

    switch (r->op) {
    case SV_ANY:
        return true;

    case SV_EXACT:
        return c == 0;

    case SV_GE:
        return c >= 0;

    case SV_GT:
        return c > 0;

    case SV_LE:
        return c <= 0;

    case SV_LT:
        return c < 0;

    case SV_CARET: {
        // ^ 的上界取决于**第一个非零位**：
        //   ^1.2.3  → <2.0.0
        //   ^0.2.3  → <0.3.0
        //   ^0.0.3  → <0.0.4
        //
        // 为什么 0.x 这么特殊：semver 规定 0.x 是「开发中，随时可能
        // 不兼容」。所以 ^0.2.3 允许 0.2.4 但绝不允许 0.3.0。
        if (c < 0) return false;

        SemVer hi = r->ver;
        hi.pre[0] = '\0';

        // parts 参与判断：^0 表示「0.x 都行」，但 ^0.0 表示「0.0.x」。
        // 三个数字全是 0 时，光看数值区分不了这两种写法。
        if (hi.major != 0 || r->parts == 1) {
            hi.major++; hi.minor = 0; hi.patch = 0;
        } else if (hi.minor != 0 || r->parts == 2) {
            hi.minor++; hi.patch = 0;
        } else {
            hi.patch++;
        }
        return sem_cmp(v, &hi) < 0;
    }

    case SV_TILDE: {
        // ~ 的上界：只放开最后一位
        //   ~1.2.3 → <1.3.0
        //   ~1.2   → <1.3.0
        //   ~1     → <2.0.0
        if (c < 0) return false;

        SemVer hi = r->ver;
        hi.pre[0] = '\0';
        if (r->parts == 1) {
            hi.major++; hi.minor = 0; hi.patch = 0;
        } else {
            hi.minor++; hi.patch = 0;
        }
        return sem_cmp(v, &hi) < 0;
    }
    }

    return false;
}

// ============================================================
// 格式化
// ============================================================

void sem_format(const SemVer* v, char* out, size_t n) {
    if (!out || n == 0) return;
    if (!v) { out[0] = '\0'; return; }
    if (v->pre[0]) {
        snprintf(out, n, "%d.%d.%d-%s", v->major, v->minor, v->patch, v->pre);
    } else {
        snprintf(out, n, "%d.%d.%d", v->major, v->minor, v->patch);
    }
}

void sem_format_req(const SemReq* r, char* out, size_t n) {
    if (!out || n == 0) return;
    if (!r) { out[0] = '\0'; return; }

    // 用户写了原样就还他原样 —— 诊断里的版本好对上号。
    // 唯一的例外是 SV_ANY 且 raw 为空（清单里 dep = ""）。
    if (r->op == SV_ANY) {
        snprintf(out, n, "*");
        return;
    }
    if (r->raw[0]) {
        snprintf(out, n, "%s", r->raw);
        return;
    }

    const char* pre = "";
    switch (r->op) {
    case SV_EXACT: pre = "=";  break;
    case SV_CARET: pre = "^";  break;
    case SV_TILDE: pre = "~";  break;
    case SV_GE:    pre = ">="; break;
    case SV_GT:    pre = ">";  break;
    case SV_LE:    pre = "<="; break;
    case SV_LT:    pre = "<";  break;
    case SV_ANY:   pre = "";   break;
    }
    char vb[96];
    sem_format(&r->ver, vb, sizeof(vb));
    snprintf(out, n, "%s%s", pre, vb);
}