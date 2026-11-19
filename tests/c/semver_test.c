// tests/c/semver_test.c — 语义化版本与约束的单元测试
//
// ── 为什么这一部分用 C 直接测，而不用 .xle ──────────────────
//
// semver.c 是包管理里唯一纯计算的部分：没有文件系统、没有全局
// 状态、没有诊断打印。它可测的边界极多（约束与上界的组合、
// 预发布比较、^0 的特例），而且**每一条都真的会错**。
//
// 用 X 源码测它就绕了一圈：得先造注册表目录、写 x.toml、
// 让 CLI 把结果打印成某种格式，再 grep 输出。这样测到的其实是
// 「CLI 输出格式」而不是「比较逻辑」，改个打印格式测试就红。
//
// 直接 include semver.c 的接口，可以用一条断言覆盖一个边界，
// 失败时能指到具体那一行。这是这个文件唯一的存在理由。
//
// 编译方式（见 tools/run_tests.sh 的 semver 段）：
//   gcc -std=c11 -O2 -I../../src ../../src/semver.c semver_test.c

#include <stdio.h>
#include <string.h>

#include "semver.h"

static int g_pass = 0;
static int g_fail = 0;

// 断言失败时打印「期望 / 实际」，而不是只报一个 FAIL。
// 对版本比较这种「差一个 0 就全错」的逻辑，看到实际值才能判断
// 是解析错了还是比较错了。
static void ok(bool cond, const char* what) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL  %s\n", what);
    }
}

// 比较两个版本串：期望负 / 零 / 正
static void cmp_expect(const char* a, const char* b, int want,
                       const char* what) {
    SemVer va, vb;
    if (!sem_parse(a, &va)) {
        g_fail++;
        printf("  FAIL  %s — 解析不了 `%s`\n", what, a);
        return;
    }
    if (!sem_parse(b, &vb)) {
        g_fail++;
        printf("  FAIL  %s — 解析不了 `%s`\n", what, b);
        return;
    }
    int got = sem_cmp(&va, &vb);
    int sgn = (got < 0) ? -1 : (got > 0 ? 1 : 0);
    if (sgn != want) {
        g_fail++;
        char fa[96], fb[96];
        sem_format(&va, fa, sizeof(fa));
        sem_format(&vb, fb, sizeof(fb));
        printf("  FAIL  %s — cmp(%s, %s) 期望 %d 实际 %d\n",
               what, fa, fb, want, sgn);
    } else {
        g_pass++;
    }
}

// 约束匹配断言
static void sat_expect(const char* ver, const char* req, bool want,
                       const char* what) {
    SemVer v;
    SemReq r;
    if (!sem_parse(ver, &v)) {
        g_fail++;
        printf("  FAIL  %s — 版本解析失败 `%s`\n", what, ver);
        return;
    }
    if (!sem_parse_req(req, &r)) {
        g_fail++;
        printf("  FAIL  %s — 约束解析失败 `%s`\n", what, req);
        return;
    }
    bool got = sem_satisfies(&v, &r);
    if (got != want) {
        g_fail++;
        printf("  FAIL  %s — %s %s %s 期望 %s 实际 %s\n",
               what, ver, want ? "应满足" : "不应满足", req,
               want ? "true" : "false", got ? "true" : "false");
    } else {
        g_pass++;
    }
}

static void parse_expect(const char* s, int ma, int mi, int pa,
                         const char* pre, const char* what) {
    SemVer v;
    if (!sem_parse(s, &v)) {
        g_fail++;
        printf("  FAIL  %s — `%s` 应该能解析\n", what, s);
        return;
    }
    if (v.major != ma || v.minor != mi || v.patch != pa ||
        strcmp(v.pre, pre) != 0) {
        g_fail++;
        printf("  FAIL  %s — `%s` 解析成 %d.%d.%d-%s，期望 %d.%d.%d-%s\n",
               what, s, v.major, v.minor, v.patch, v.pre, ma, mi, pa, pre);
    } else {
        g_pass++;
    }
}

static void parse_fail_expect(const char* s, const char* what) {
    SemVer v;
    if (sem_parse(s, &v)) {
        g_fail++;
        printf("  FAIL  %s — `%s` 应该解析失败\n", what, s);
    } else {
        g_pass++;
    }
}

int main(void) {
    printf("semver 单元测试\n\n");

    // ── 解析 ────────────────────────────────────────────────
    parse_expect("1.2.3", 1, 2, 3, "", "完整三段");
    parse_expect("1.2", 1, 2, 0, "", "缺 patch 补 0");
    parse_expect("1", 1, 0, 0, "", "只有 major");
    parse_expect("v1.2.3", 1, 2, 3, "", "v 前缀");
    parse_expect("1.2.3-rc1", 1, 2, 3, "rc1", "预发布标识");
    parse_expect("1.2.3-alpha.beta", 1, 2, 3, "alpha.beta", "多段预发布");
    parse_expect("1.2.3+build99", 1, 2, 3, "", "构建元数据被丢弃");
    parse_expect("1.2.3-rc1+build", 1, 2, 3, "rc1", "预发布 + 构建");

    parse_fail_expect("", "空串不是版本");
    parse_fail_expect("abc", "非数字");
    parse_fail_expect("1.", "点后无数字");
    parse_fail_expect("1.2.", "第二个点后无数字");
    parse_fail_expect("1.2.3-", "空预发布标识");

    // ── 比较：基本序 ────────────────────────────────────────
    cmp_expect("1.0.0", "1.0.1", -1, "patch 递增");
    cmp_expect("1.0.1", "1.0.0", 1,  "patch 递减");
    cmp_expect("1.0.1", "1.0.1", 0,  "相等");
    cmp_expect("1.9.9", "2.0.0", -1, "minor 不越权到 major");
    cmp_expect("2.0.0", "10.0.0", -1, "numeric 而非字典序（2 < 10）");

    // ── 比较：预发布 ────────────────────────────────────────
    //
    // 这几条是 semver 最容易写错的地方：1.0.0-rc1 < 1.0.0，
    // 而不是反过来。直觉上「rc1 是更新的东西」会让人写成更大。
    cmp_expect("1.0.0-rc1", "1.0.0", -1, "预发布 < 正式版");
    cmp_expect("1.0.0", "1.0.0-rc1", 1,  "正式版 > 预发布");
    cmp_expect("1.0.0-alpha", "1.0.0-beta", -1, "字母序");
    cmp_expect("1.0.0-alpha", "1.0.0-alpha.1", -1, "段数少的更小");
    cmp_expect("1.0.0-1", "1.0.0-alpha", -1, "数字段 < 字母段");
    cmp_expect("1.0.0-2", "1.0.0-10", -1, "预发布里的数字按数值比");
    cmp_expect("1.0.0-alpha.2", "1.0.0-alpha.10", -1, "多段里的数字");
    cmp_expect("1.0.0-alpha", "1.0.0-alpha", 0, "预发布相等");

    // ── 约束：^ ─────────────────────────────────────────────
    sat_expect("1.2.3", "^1.2.0", true,  "^ 下界含自己");
    sat_expect("1.9.9", "^1.2.0", true,  "^1 允许 minor 上升");
    sat_expect("2.0.0", "^1.2.0", false, "^1 不允许 major 上升");
    sat_expect("1.1.9", "^1.2.0", false, "^1.2 拒绝低于下界");

    // ^0 的特例：semver 规定 0.x 是「开发中，随时可不兼容」
    sat_expect("0.2.4", "^0.2.3", true,  "^0.2 允许 patch 上升");
    sat_expect("0.3.0", "^0.2.3", false, "^0.2 不允许 minor 上升");
    sat_expect("0.9.0", "^0", true,      "^0（只写 1 段）允许 0.x");
    sat_expect("1.0.0", "^0", false,     "^0 不允许 1.0");

    // ^0.0.x：第一个非零位就是 patch，所以 patch 一动就不兼容。
    //   ^0.0.3 等价于 >=0.0.3 <0.0.4 —— 只允许 0.0.3 自己。
    //
    // 这一条我第一版写成「0.0.4 应该满足」，是错的。npm 与 Cargo
    // 都是这个规则：0.0.x 里每个 patch 都算不兼容变更。写测试时
    // 靠直觉会得出正好相反的断言 —— 所以这条值得单独留着，它是
    // 唯一能证明「特殊规则真的被实现成特殊规则」的用例。
    sat_expect("0.0.3", "^0.0.3", true,  "^0.0.3 含自己");
    sat_expect("0.0.4", "^0.0.3", false, "^0.0.x 的 patch 一动就不兼容");

    // ── 约束：~ ─────────────────────────────────────────────
    sat_expect("1.2.9", "~1.2.3", true,  "~1.2 允许 patch 上升");
    sat_expect("1.3.0", "~1.2.3", false, "~1.2 不允许 minor 上升");
    sat_expect("1.9.0", "~1", true,      "~1 允许 minor 上升");
    sat_expect("2.0.0", "~1", false,     "~1 不允许 major 上升");

    // ── 约束：其它运算符 ────────────────────────────────────
    sat_expect("1.2.3", "=1.2.3", true,  "= 精确匹配");
    sat_expect("1.2.4", "=1.2.3", false, "= 拒绝别的版本");
    sat_expect("1.2.3", ">=1.2.3", true, ">= 含等号");
    sat_expect("1.2.3", ">1.2.3", false, "> 不含等号");
    sat_expect("1.2.3", "<=1.2.3", true, "<= 含等号");
    sat_expect("1.2.3", "<1.2.3", false, "< 不含等号");
    sat_expect("99.0.0", "*", true,      "* 匹配一切");
    sat_expect("99.0.0", "", true,       "空约束等价 *");

    // 裸版本号默认 ^（Cargo / npm 的习惯）
    sat_expect("1.5.0", "1.2", true,  "裸版本号是 ^ 语义");
    sat_expect("2.0.0", "1.2", false, "裸版本号不允许跨 major");

    // ── 预发布不参与普通约束 ────────────────────────────────
    //
    // 这条规则挡住的是真实事故：作者半夜推的 2.0.0-beta 不该被
    // 任何写 ^1 或 ^2 的人自动选中。
    sat_expect("2.0.0-beta", "^1.0.0", false, "预发布不满足普通约束");
    sat_expect("2.0.0-beta", "^2.0.0", false, "预发布不满足同主版本约束");
    sat_expect("2.0.0-beta", "*",      true,  "* 就连预发布也匹配");

    // ── 约束解析 ────────────────────────────────────────────
    {
        SemReq r;
        ok(sem_parse_req("^1.2.3", &r) && r.op == SV_CARET, "^ 解析成 CARET");
        ok(sem_parse_req("~1.2.3", &r) && r.op == SV_TILDE, "~ 解析成 TILDE");
        ok(sem_parse_req("1.2.3",  &r) && r.op == SV_CARET, "裸版本号默认 CARET");
        ok(sem_parse_req(">=1.2",  &r) && r.op == SV_GE,    ">= 解析");
        ok(sem_parse_req(">1.2",   &r) && r.op == SV_GT,    "> 解析（不是 >== 误判）");
        ok(sem_parse_req("*",      &r) && r.op == SV_ANY,   "* 解析");
        ok(sem_parse_req("^1",     &r) && r.parts == 1,     "1 段");
        ok(sem_parse_req("^1.2",   &r) && r.parts == 2,     "2 段");
        ok(sem_parse_req("^1.2.3", &r) && r.parts == 3,     "3 段");
        ok(sem_parse_req(">= 1.2.0", &r) && r.op == SV_GE,  "运算符与版本间有空格");
    }

    // ── 格式化往返 ──────────────────────────────────────────
    {
        SemVer v;
        char buf[96];
        sem_parse("1.2.3", &v);
        sem_format(&v, buf, sizeof(buf));
        ok(strcmp(buf, "1.2.3") == 0, "格式化三段");

        sem_parse("1.2.3-rc1", &v);
        sem_format(&v, buf, sizeof(buf));
        ok(strcmp(buf, "1.2.3-rc1") == 0, "格式化预发布");

        // 关键性质：格式化后再解析必须得回同一个版本。
        // 这条抓的是「格式化漏了 pre 字段」这类静默数据丢失。
        SemVer back;
        ok(sem_parse(buf, &back) && sem_cmp(&v, &back) == 0,
           "格式化 → 解析 往返一致");
    }

    printf("\n  %d 项通过", g_pass);
    if (g_fail > 0) printf("，%d 项失败", g_fail);
    printf("\n");
    printf("semver ok\n");
    return g_fail == 0 ? 0 : 1;
}