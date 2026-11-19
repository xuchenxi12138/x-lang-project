// trace.c — 编译流水线打点（只有 -DX_DEBUG 构建才有实质内容）
//
// ── 这个文件解决什么问题 ────────────────────────────────────
//
// 编译器崩溃时，崩溃处理器能给出异常码和地址。但如果崩溃发生在
// 没有帧指针的 CRT 代码里（实测过：崩在 msvcrt 的堆管理内部），
// 地址反查只能得到 `??:0`，rbp 链也走不下去 —— 那时唯一的线索是
// 「崩之前最后走到了哪一步」。
//
// 而「最后走到哪一步」只能靠在代码里埋点。埋点最怕两件事：
//   1. 忘了关，带着一堆调试输出发布
//   2. 输出量大到把真正有用的信息淹没
//
// 所以这里的设计是：
//   · 只有 -DX_DEBUG 构建才编译进来（release 里全是空函数，零开销）
//   · 还要**再**用环境变量 X_TRACE=1 显式打开（默认不开）
//   · 输出带**深度缩进**，一眼能看出调用层级
//   · 每条打点自动 flush —— 崩溃时缓冲区里的东西不会丢
//
// ── 为什么用固定大小的名字数组而不是 malloc ─────────────────
//
// 打点是为了调试，而调试的对象常常是内存管理本身。如果打点自己
// 调 malloc，崩在堆里的时候就会连打点都跟着炸，把现场彻底搞乱。
// 固定数组 + 只存指针，保证这个文件的代码路径永远不会触发分配。

#include "platform.h"

#if X_DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRACE_MAX_DEPTH 64

static int  s_enabled = -1;    // -1 = 还没查过环境变量
static int  s_depth   = 0;
static const char* s_stack[STRACE_MAX_DEPTH];

static int trace_on(void) {
    if (s_enabled < 0) {
        const char* v = getenv("X_TRACE");
        s_enabled = (v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return s_enabled;
}

void strace_enter(const char* what) {
    if (!trace_on()) return;
    // 一个 call 在崩溃前最后打印的那条就是「最后成功进入的函数」。
    // 所以连进入也要 flush —— 缓冲的话最后一次打点必然丢失，
    // 而那一次恰恰最有用。
    for (int i = 0; i < s_depth; i++) fputs("│ ", stderr);
    fputs("→ ", stderr);
    fputs(what, stderr);
    fputc('\n', stderr);
    if (s_depth < STRACE_MAX_DEPTH) s_stack[s_depth] = what;
    s_depth++;
    fflush(stderr);
}

void strace_leave(void) {
    if (!trace_on()) return;
    if (s_depth > 0) s_depth--;
    fflush(stderr);
}

#else

// release 构建：全部为空。写成函数而不是宏，是为了让调用点
// 不用加 #if —— 否则每个调用点都要判断一次，代码立刻变脏。
void strace_enter(const char* what) { (void)what; }
void strace_leave(void) { }

#endif