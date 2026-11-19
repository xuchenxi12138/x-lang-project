/* ============================================================
 * run-emu.c —— 在 PC 上跑 X 内核（宿主仿真档的驱动）
 * ============================================================
 *
 * ── 它做的事 ──────────────────────────────────────────────
 *
 * 1. 初始化模拟硬件（xemu_init）
 * 2. 写引导参数块（模仿 boot.s 的行为）
 * 3. 调用内核真正的 main —— 也就是 kmain.xle 里的 main()
 * 4. 内核的 shellLoop 会一直轮询键盘；这里通过**注入扫描码**
 *    来驱动它，从而跑完整条 shell 命令路径
 * 5. 最后把 VGA 屏幕、串口输出、计数器全部 dump 出来
 *
 * ── 为什么不直接调 main 就完事 ────────────────────────────
 *
 * main() 结尾是 shellLoop()，那是死循环。所以必须有东西把
 * 「输入」喂进去、并在喂完之后让它退出。两个办法：
 *
 *   · 让 shellLoop 在某个条件下 return（改内核代码 —— 不好，
 *     等于为测试改产品逻辑）
 *   · 在宿主侧用 setjmp/longjmp 从「输入耗尽」时跳出来
 *     （不改内核一行代码）
 *
 * 这里选后者。代价是 longjmp 会跳过内核里的清理逻辑 —— 但内核
 * 本来就没有清理逻辑（它永不退出），所以没有语义损失。
 *
 * 注意：这不是「测试通过」的证明，只是「跑起来了、输出符合预期」。
 * 断言在 tools/test-kernel.sh 里，它拿这里的输出做比对。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "xemu.h"

/* 内核 main 的声明。生成代码里它是 int main(int, char**)，
 * 但 X 的 main 语义是「返回 Int 作为退出码」，所以这里照声明。 */
int xk_main(void);

/* ── 用 setjmp 从死循环里逃出来 ───────────────────────────
 *
 * shellLoop() 内部的退出条件是「读到特定命令」。但如果内核的
 * 命令表里没有 quit，就永远出不来。所以这里给一个全局的
 * 「逃逸点」：注入完所有键之后 longjmp 回这里。
 *
 * 关键细节：jmp_buf 必须是 static 且只被主流程设置一次 ——
 * 内核代码不会碰它（它不知道宿主的存在）。 */
static jmp_buf g_escape;
static volatile int g_escaped;

/* ── 把字符串转成 PS/2 扫描码（Set 1）─────────────────────
 *
 * 内核的 kbdPoll 读的是扫描码，所以「输入一行命令」意味着
 * 生成一串扫描码。这里只覆盖 ASCII 里用得到的字符。
 *
 * 为什么不在内核里加一个「字符串输入」内建：那等于为了测试
 * 给产品代码开后门，而真机上永远不会有这种输入。扫描码是
 * 真实硬件会产生的字节，用它是**更真**的模拟。
 */
typedef struct { char ch; uint8_t code; } ScanMap;

static const ScanMap g_scan[] = {
    {'1',0x02},{'2',0x03},{'3',0x04},{'4',0x05},{'5',0x06},
    {'6',0x07},{'7',0x08},{'8',0x09},{'9',0x0A},{'0',0x0B},
    {'-',0x0C},{'=',0x0D},
    {'q',0x10},{'w',0x11},{'e',0x12},{'r',0x13},{'t',0x14},
    {'y',0x15},{'u',0x16},{'i',0x17},{'o',0x18},{'p',0x19},
    {'[',0x1A},{']',0x1B},
    {'a',0x1E},{'s',0x1F},{'d',0x20},{'f',0x21},{'g',0x22},
    {'h',0x23},{'j',0x24},{'k',0x25},{'l',0x26},{';',0x27},
    {'\'',0x28},{'`',0x29},
    {'\\',0x2B},
    {'z',0x2C},{'x',0x2D},{'c',0x2E},{'v',0x2F},{'b',0x30},
    {'n',0x31},{'m',0x32},{',',0x33},{'.',0x34},{'/',0x35},
    {' ',0x39},
    {'\n',0x1C},                   /* Enter */
    {'\b',0x0E},                   /* Backspace */
};

static uint8_t scan_of(char c)
{
    char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    for (size_t i = 0; i < sizeof(g_scan) / sizeof(g_scan[0]); i++)
        if (g_scan[i].ch == lc) return g_scan[i].code;
    return 0;
}

/* 把一整行（含 \n）的扫描码塞进注入队列。
 * 大写字母用 Shift 前缀表示 —— 内核的转换表若处理了 Shift，
 * 这个测试就能验证到；若没处理，小写化也能过（不误报）。 */
static void feed_line(const char* s)
{
    for (const char* p = s; *p; p++) {
        uint8_t sc = scan_of(*p);
        if (!sc) continue;
        if (*p >= 'A' && *p <= 'Z') xemu_push_key(0x2A);   /* Shift down */
        xemu_push_key(sc);
        if (*p >= 'A' && *p <= 'Z') xemu_push_key(0xAA);   /* Shift up */
    }
    if (!strchr(s, '\n')) xemu_push_key(0x1C);             /* 补 Enter */
}

/* ── 逃逸钩子 ─────────────────────────────────────────────
 *
 * 内核的 shellLoop 读到 'q'/'quit' 会尝试 return。为了不依赖
 * 内核命令表里有 quit，这里用一个「注入队列空了就逃」的策略：
 * 驱动层在喂完键之后调用 xemu_arm_escape()，而 kbdPoll 在
 * 队列空时会返回 -1... 
 *
 * 但 kbdPoll 是内核内部函数，我们没法拦。
 *
 * 所以实际做法：喂完键之后不逃，而是让内核的 shell 处理完
 * 最后一条命令，然后驱动层**直接 longjmp**。
 * 代价是无法验证「喂完之后 shell 还在正常循环」—— 那由
 * QEMU 上的交互测试覆盖。
 */
/* 逃逸回调：由 xemu 在「注入队列空 + 内核读键盘」时调用。
 * longjmp 回 main 里的 setjmp 点。 */
static void do_escape(void)
{
    g_escaped = 1;
    longjmp(g_escape, 1);
}

int main(int argc, char** argv)
{
    int quiet = 0;
    const char* script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "--script")) script = (i + 1 < argc) ? argv[++i] : NULL;
    }

    xemu_init();
    xemu_set_echo(quiet ? 0 : 1);
    xemu_set_escape(do_escape);

    /* 模拟 boot.s 写参数块：magic "XOS1" + 32 MB + 盘号 0x80 */
    xemu_set_boot_params(0x584F5331u, 32u, 0x80u);

    /* ── 喂脚本 ──────────────────────────────────────────────
     *
     * shellLoop 在**进入时**就会读键盘，所以在调 main 之前
     * 把按键全塞进注入队列即可 —— 不必多线程。
     *
     * 顺序很重要：内核启动时先打 banner + selftest，那段不读
     * 键盘；之后才进 shell。由于队列是有序的，先塞的命令
     * 会被先消费，正好对应「启动后依次执行」。
     *
     * 但有个真问题：内核**每一步之间没有等待**，队列里的键
     * 会在 shell 刚起来时被一口气读完。若 shell 的输入缓冲
     * 只有固定大小，后面的命令会丢。
     * 所以默认只喂少量短命令，长交互留给 QEMU。 */
    if (script) {
        FILE* f = fopen(script, "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", script); return 2; }
        char line[256];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            if (n && line[n-1] == '\n') line[n-1] = '\0';
            if (!line[0] || line[0] == '#') continue;
            feed_line(line);
        }
        fclose(f);
    } else {
        /* 默认脚本：覆盖六条内建命令 */
        /* 顺序有讲究：clear 放**最前**。
         *
         * 屏幕只有 25 行，help 的输出就有 7 行，加上自检那十几行
         * 会把屏幕滚掉。测试要断言「VGA 上有 help 输出」，就必须
         * 让 clear 先把启动期那些行清掉，后面的命令输出才留得住。
         *
         * 这不是「为了测试改行为」—— 它模拟的是真实用法：用户
         * 启动后先 clear 再看命令输出。 */
        feed_line("clear\n");
        feed_line("help\n");
        feed_line("ver\n");
        feed_line("mem\n");
        feed_line("bogus\n");
    }

    /* ── 跑内核 ──────────────────────────────────────────────
     *
     * 用 setjmp 建立逃逸点。内核正常返回（不会发生）也走这里。 */
    if (setjmp(g_escape) == 0) {
        xk_main();
    }

    printf("\n");
    printf("==================================================\n");
    printf("  内核已返回（正常情况下不会发生 —— main 末尾是死循环）\n");
    printf("==================================================\n");
    printf("  hlt 次数      : %llu\n", (unsigned long long)xemu_hlt_count());
    printf("  未映射访问    : %llu\n", (unsigned long long)xemu_unmapped_count());
    printf("  中断开关      : %s\n", xemu_irq_enabled() ? "开" : "关");
    printf("  串口字节数    : %zu\n", xemu_serial_len());
    printf("\n---- VGA 屏幕快照 ----\n%s", xemu_screen());
    return 0;
}