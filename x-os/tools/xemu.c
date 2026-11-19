/* ============================================================
 * xemu.c —— 让 X 写的内核在 PC 上跑起来（宿主仿真档）
 * ============================================================
 *
 * ── 这个文件解决什么问题 ──────────────────────────────────
 *
 * 内核代码用的 memRead8 / memWrite16 / portIn8 / portOut8 这些
 * 内建，语义是「访问物理地址 / I/O 端口」。在 PC 上跑就意味着
 * 解引用 0xB8000 —— 那是别人的进程空间，立刻段错误。
 *
 * 于是「想验证内核逻辑就必须先装 QEMU」，而 QEMU 只能告诉你
 * 「屏幕上出现了什么」，看不到内部状态、不能单步、不能断言。
 *
 * 这个文件提供 fake hardware：一块内存里的 VGA 文本帧缓冲 +
 * 一个 16550 串口的寄存器模型。只要编译时定义 X_EMU，生成的
 * 代码就把那些访问转到这里，于是**同一份 kmain.xle** 能在 PC 上
 * 跑起来，而 VGA 排版 / 滚动 / 串口格式化 / shell 解析 / 自检
 * 这些逻辑全都是真的。
 *
 * ── 它验证什么，不验证什么（必须说清楚）──────────────────
 *
 * 验证：
 *   · 字符写到 VGA 缓冲的正确位置（80x25、字符+属性两字节）
 *   · 满屏后的滚动行为
 *   · 光标位置追踪
 *   · 串口输出的字节序列（含数字格式化、十六进制）
 *   · 内存探测、自检逻辑、shell 命令解析与输出
 *
 * 不验证：
 *   · 中断（IDT/PIC）、分页、特权级切换、GDT 的实际效果
 *   · 时序（串口等待、键盘扫描的竞态）
 *   · 真实硬件的地址解码与对齐陷阱
 *
 * 这些必须真机或 QEMU。所以本文件是**开发期的高频验证工具**，
 * 不是「替代品」—— 交付前仍要在 QEMU 上过一遍。
 *
 * ── 为什么 VGA 用「只读文本镜像」而不是真分配 0xB8000 ────
 *
 * 我们不去 mmap 固定地址（Windows 上很难，且会与 ASLR 打架），
 * 而是把 [0xB8000, 0xB8000+80*25*2) 这段地址**拦截**下来，
 * 落到一个独立的数组里。写进去的字节可以随时 dump 成屏幕文本。
 * 读回来的也是这个数组 —— 内核里「读 VGA 缓冲看有没有写成功」
 * 这类代码因此也能工作。
 *
 * ── 端口模型的取舍 ────────────────────────────────────────
 *
 * 16550 串口只实现内核对它的实际用法：
 *   · 0x3FD (LSR)：bit5 = THR 空，恒为 1（因为我们不收硬件缓冲，
 *     写完就当发走了）
 *   · 0x3F8 (THR/RBR)：写 = 输出一个字节，读 = 0
 *   · 其余寄存器（分频、FCR、MCR）当作普通存储
 * 故意**不**模拟「忙」状态：如果模拟了，内核里那行
 * `while ((portIn8(0x3FD) & 0x20) == 0) {}` 在 bug 时会死循环，
 * 而仿真档的目的是跑通逻辑，不是复现硬件时序。
 *
 * 键盘端口 0x60/0x64 的返回值由 set_kbd 注入，用于测 kbdPoll。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xemu.h"

/* ── 内存窗口 ─────────────────────────────────────────────
 *
 * 只拦截需要的区域，其余地址一律报「未映射」而不是静默返回 0。
 *
 * 静默返回 0 会让「内核访问了不该访问的地址」看起来一切正常，
 * 而那正是裸机上会 triple fault 的错。仿真档的价值就在于
 * **能在这个错发生的瞬间报出来**。
 */
#define VGA_BASE   0xB8000u
#define VGA_SIZE   (80 * 25 * 2)

static uint8_t  g_vga[VGA_SIZE];
static uint32_t g_vga_attr = 0x0F;      /* 默认属性：黑底白字 */

/* 通用小内存区：0x4000（引导参数）、0x5000（光标）、以及
 * 堆区 0x100000..0x1000000。内核只碰这几处，其余报错。
 *
 * ── 堆上限必须与 runtime.c 的 HEAP_LIMIT **完全一致** ────
 *
 * 这不是设计选择，是硬约束：仿真层映射的区间如果比内核认为
 * 可用的区间大，就永远测不出「堆越界」；如果比它小，内核
 * 正常分配就会在这里被误判成非法访问。
 *
 * 上一版这里写成 `HEAP_SIZE = 16MB`，于是映射区间变成
 * 0x100000..0x1100000，比内核的 0x01000000 多出 1 MB。
 * 内核第一次分配就越界到 0x1100000 附近 —— 那不是内核的 bug，
 * 是我把「16 MB 大小」和「上限是绝对地址 0x01000000」搞混了：
 * 从 0x100000 起算 16 MB 的终点是 0x1100000，而内核写死的
 * 上限是 0x1000000（15 MB 可用）。
 *
 * 现在用 HEAP_LIMIT - HEAP_BASE 推导，两处口径一致。 */
#define HEAP_BASE  0x100000u
#define HEAP_LIMIT 0x1000000u           /* 必须等于 runtime.c 的同名常量 */
#define HEAP_SIZE  (HEAP_LIMIT - HEAP_BASE)
static uint8_t* g_heap;

#define LOWR_BASE  0x4000u
#define LOWR_SIZE  0x2000u              /* 覆盖 0x4000..0x6000（含光标 0x5000） */
static uint8_t  g_low[LOWR_SIZE];

/* ── 越界访问的兜底区 ─────────────────────────────────────
 *
 * 内核的**值栈**在 .vstack 段（0x21880 起 786 KB），它由链接器
 * 安排地址，代码通过绝对地址读写它。仿真档没有那个物理地址，
 * 所以必须给它一块落地空间。
 *
 * 用「地址哈希到 2 MB 窗口」而不是精确映射：值栈访问是
 * 密集且连续的，只要有个可写的落地处就不会崩，而精确映射
 * 需要跟着链接脚本走（内核一改大小就失效）。
 *
 * ⚠️ 这会让「值栈越界」这类 bug 在仿真档里不被发现 ——
 * 记录在 notes 里，由 QEMU 上的测试兜底。 */
#define SCRATCH_SIZE (2u * 1024u * 1024u)
static uint8_t* g_scratch;

/* 上一次越界访问的地址，用于诊断输出 */
static uint32_t g_last_oob;
static uint64_t g_oob_count;

/* ── 仿真层声明的物理内存上限 ─────────────────────────────
 *
 * 超过这里的地址「写了不存、读了全 1」，于是内核的内存探测
 * （逐 MB 写 magic 再读回）会在这里停下，报出一个真实的数字。
 *
 * 128 MB 是随手选的合理值：够大（不会让内核的堆真的不够用），
 * 又不是 4096（那会让「探测」这件事失去意义 —— 探到顶就等于
 * 没探）。改这个值只需要改这一处。 */
#define SIM_MEM_TOP 0x08000000u          /* 128 MB */

/* 越界读返回的字节：全 1。与 magic 0x58534F53 不同，探测在此失败。 */
static uint8_t g_ones[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
/* 越界写落地处：写进这里，写完就忘。 */
static uint8_t g_sink[8];

/* ── 串口输出收集 ─────────────────────────────────────────
 *
 * 内核往 0x3F8 写的字节全存这里，测试可以断言内容。
 * 同时**实时**往宿主 stdout 打一份（可选，由 -q 关闭），
 * 这样直接跑就能看见内核在说什么。 */
#define SER_BUF_MAX (256 * 1024)
static char   g_ser[SER_BUF_MAX];
static size_t g_ser_len;
static int    g_ser_echo = 1;           /* 1 = 同时打到屏幕 */

/* 串口寄存器 8 个：0x3F8..0x3FF */
static uint8_t g_uart[8];

/* 键盘注入队列 */
#define KBD_MAX 256
static uint8_t g_kbd[KBD_MAX];
static size_t  g_kbd_head, g_kbd_tail;
/* 0x64 的状态字节：bit0=输出缓冲有数据 */
static uint8_t g_kbd_status;

/* ── 逃逸钩子（由驱动设置）───────────────────────────────
 *
 * 内核的 shellLoop 是 `for(;;) { ... kbdPoll() ... }` —— 永不返回。
 * 仿真档必须在某个时刻从里面出来，否则进程卡死。
 *
 * 拦在 **kbdPoll 读端口** 这一层是唯一不带侵入性的位置：
 *   · 不改内核源码（不能为测试改产品逻辑）
 *   · 不依赖内核命令表里有 quit
 *   · 时机精确 —— 每次轮询都是一次「内核还活着」的证明
 *
 * 注入队列空了之后，第一次读 0x60 就触发逃逸。这样「所有
 * 脚本命令都被处理完」与「退出」之间没有竞态。 */
static void (*g_escape_fn)(void);
static int g_escape_armed;

void xemu_set_escape(void (*fn)(void))
{
    g_escape_fn = fn;
    g_escape_armed = 1;
}

/* CPU 指令计数与停机标志 */
static uint64_t g_hlt_count;
static int      g_stopped;
static int      g_irq_enabled = 1;      /* cli/sti 的效果，仅供观察 */

/* 未映射访问计数 —— 测试用它断言「内核没有乱访问」 */
static uint64_t g_unmapped;
static uint32_t g_unmapped_last;

/* ── 内部工具 ──────────────────────────────────────────── */

static uint8_t* resolve(uint32_t a, int w, int for_write)
{
    (void)for_write;
    /* 必须整段落在同一个窗口内，否则一次 32 位读会跨窗口 —— 
     * 真实硬件上跨设备边界读是非法操作，仿真这里也拒。 */
    if (a >= VGA_BASE && (uint64_t)a + w <= (uint64_t)VGA_BASE + VGA_SIZE)
        return g_vga + (a - VGA_BASE);
    if (a >= LOWR_BASE && (uint64_t)a + w <= (uint64_t)LOWR_BASE + LOWR_SIZE)
        return g_low + (a - LOWR_BASE);
    if (g_heap && a >= HEAP_BASE && (uint64_t)a + w <= HEAP_LIMIT)
        return g_heap + (a - HEAP_BASE);

    /* ── 落到兜底区，记一笔但不中止 ────────────────────────
     *
     * 上一版在这里 exit(71)。问题：内核值栈（.vstack，786 KB）
     * 按链接器给的绝对地址访问，仿真档没有那个地址，于是第一
     * 次值栈读写就中止 —— 什么输出都拿不到，等于仿真档没用。
     *
     * 现在映射到一块 2 MB 的 scratch（地址取模落地）。代价是
     * 「值栈越界」在仿真档里测不出来，收益是内核逻辑能完整跑。
     * 这个取舍写在这里，不假装没有。
     *
     * ── 但探测量内存的地址必须**真的**读不回 magic ────────
     *
     * memProbeLimit() 的做法是「逐 MB 写 magic 再读回，读到
     * 不一样就说明这里没有内存」。如果我们对所有越界地址都
     * 老实地存下来，探测会一路成功到 4096 MB，内核于是报告
     * 「~4096 MB detected」—— 一个假的数字。
     *
     * 所以超过 SIM_MEM_TOP 的地址一律「写了不存、读了返回
     * 全 1」：全 1 不等于 magic，探测就在那里停下，报出的
     * 数字是仿真层声明的容量。这与真机上探到空槽的表现一致。 */
    if (a >= SIM_MEM_TOP) {
        g_oob_count++;
        g_last_oob = a;
        return for_write ? g_sink : g_ones;
    }

    g_oob_count++;
    g_last_oob = a;
    if (g_scratch) {
        uint32_t off = (a * 2654435761u) % (SCRATCH_SIZE - 4u);
        return g_scratch + off;
    }
    fprintf(stderr, "\n  [xemu] 未映射的物理地址访问: 0x%X（宽度 %d）\n", a, w);
    exit(71);
}

/* ── 内存访问（供生成代码调用）────────────────────────── */

uint32_t xe_mem_read(uint32_t a, int w)
{
    const uint8_t* p = resolve(a, w, 0);
    switch (w) {
    case 1: return *p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    default: { uint32_t v; memcpy(&v, p, 4); return v; }
    }
}

void xe_mem_write(uint32_t a, uint32_t v, int w)
{
    uint8_t* p = resolve(a, w, 1);
    switch (w) {
    case 1: *p = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    default: { uint32_t x = v; memcpy(p, &x, 4); break; }
    }
}

/* ── I/O 端口 ──────────────────────────────────────────── */

#define COM1 0x3F8

uint32_t xe_port_in(uint16_t p, int w)
{
    if (w == 2) {
        /* 16 位读很少见（内核没有这种用法），拼两次 8 位即可。 */
        return xe_port_in(p, 1) | (xe_port_in((uint16_t)(p + 1), 1) << 8);
    }
    if (p >= COM1 && p <= COM1 + 7) {
        uint8_t idx = (uint8_t)(p - COM1);
        if (idx == 5) {
            /* LSR：bit5 = THR 空（恒 1），bit6 = 发送器空（恒 1）。
             * 故意不模拟 bit5=0 的忙碌态，理由见文件头。
             *
             * bit0 = 接收数据就绪。**这一位必须真的模拟** ——
             * 内核的自检用 MCR 的 LOOP 位做串口回环：置 LOOP、
             * 发一个 0x5A、等 LSR bit0、读回、比对。如果这里
             * 恒返回「无数据」，自检就永远是「不支持」。 */
            uint8_t lsr = 0x60;
            if (g_uart[4] & 0x10) lsr |= 0x01;   /* LOOP 模式下 RBR 总有数据 */
            return lsr;
        }
        if (idx == 0) {
            /* RBR（接收缓冲）：只有 LOOP 模式才有意义 */
            if (g_uart[4] & 0x10) return g_uart[0];
            return 0;
        }
        return g_uart[idx];
    }
    if (p == 0x64) {
        /* 键盘控制器状态：bit0 = 输出缓冲满
         *
         * ── 逃逸钩子为什么挂在**这个**端口 ────────────────
         *
         * 内核的 kbdPoll 是这样的：
         *
         *     if ((portIn8(0x64) & 0x01) == 0) { return -1 }   ← 先查状态
         *     let sc = portIn8(0x60)                           ← 再读数据
         *
         * 队列空的时候状态位是 0，第一行就 return 了，**根本走不到
         * 读 0x60**。上一版把钩子挂在 0x60 上，于是永远不触发 ——
         * 表现是仿真进程在 shellLoop 里死循环，超时被杀。
         *
         * 挂在 0x64 上才是「每次轮询必经」的位置。 */
        if (g_kbd_status == 0 && g_escape_armed && g_escape_fn) {
            g_escape_armed = 0;
            g_escape_fn();          /* 不返回 */
        }
        return g_kbd_status;
    }
    if (p == 0x60) {
        /* 键盘数据口 */
        if (g_kbd_head != g_kbd_tail) {
            uint8_t c = g_kbd[g_kbd_head];
            g_kbd_head = (g_kbd_head + 1) % KBD_MAX;
            if (g_kbd_head == g_kbd_tail) g_kbd_status = 0;
            return c;
        }
        g_kbd_status = 0;
        /* ── 队列空了：内核已经处理完全部脚本，可以退出了 ──
         *
         * 为什么是这里而不是别处：kbdPoll 的循环体里读 0x60 是
         * 「这一刻没有输入」的唯一信号，且它一定在内核把前面的
         * 命令处理完之后才发生（命令是同步处理的）。所以在这里
         * 逃逸，退出时机是确定的。 */
        if (g_escape_armed && g_escape_fn) {
            g_escape_armed = 0;
            g_escape_fn();          /* 不返回 */
        }
        return 0;
    }
    /* 其他端口读回 0，但记一笔 —— 真机上这些端口可能有意义。 */
    return 0;
}

void xe_port_out(uint16_t p, uint32_t v, int w)
{
    if (w == 2) {
        xe_port_out(p, v & 0xFF, 1);
        xe_port_out((uint16_t)(p + 1), (v >> 8) & 0xFF, 1);
        return;
    }
    if (p == COM1) {                     /* THR：发送一个字节 */
        char c = (char)(uint8_t)v;
        /* ── LOOP 模式：字节不出门，回到接收寄存器 ──────────
         *
         * 16550 的 MCR bit4（LOOP）置位后，芯片内部把 TX 接到 RX，
         * 用于自检。真实硬件上这时字节**不会**出现在发送线上，
         * 所以串口终端看不到它。
         *
         * 上一版没模拟这一位，于是内核自检里那一个 0x5A 直接
         * 打到了宿主屏幕上 —— 输出里凭空多出一个 'Z'，而且
         * 位置在 "[ok] serial loopback" 之前，看起来像内核输出
         * 了脏数据。它其实不是 bug，是仿真层漏了一个硬件特性。
         *
         * 回环读回走 xe_port_in 的 idx==0 分支。 */
        if (g_uart[4] & 0x10) {
            g_uart[0] = (uint8_t)v;      /* 存进 RBR，LSR bit0 置位 */
            return;
        }
        if (g_ser_len + 1 < SER_BUF_MAX) g_ser[g_ser_len++] = c;
        if (g_ser_echo) {
            /* 串口在内核里是「日志通道」。仿真档把它接到宿主
             * stdout，于是直接运行就能看见内核输出。
             * 换行用 \r\n 会多一个空行，这里原样输出。 */
            fputc(c, stdout);
            fflush(stdout);
        }
        return;
    }
    if (p >= COM1 && p <= COM1 + 7) {
        g_uart[p - COM1] = (uint8_t)v;
        return;
    }
    /* 其他端口忽略 */
}

/* ── CPU 指令 ──────────────────────────────────────────── */

void xe_cpu_hlt(void)
{
    g_hlt_count++;
    /* 真机上 hlt 会停到下一次中断。仿真档没有中断源，若真停住
     * 就死锁了 —— 所以记一次数后立刻返回，由调用方的循环继续。
     * 这也是为什么仿真档**不能**验证 idle 循环的行为。 */
}

void xe_cpu_cli(void) { g_irq_enabled = 0; }
void xe_cpu_sti(void) { g_irq_enabled = 1; }
void xe_cpu_invlpg(uint32_t a) { (void)a; /* 无分页，空操作 */ }

/* ── 对外 API（测试/驱动代码用）────────────────────────── */

void xemu_init(void)
{
    if (!g_heap)    g_heap    = (uint8_t*)calloc(HEAP_SIZE, 1);
    if (!g_scratch) g_scratch = (uint8_t*)calloc(SCRATCH_SIZE, 1);
    memset(g_vga, 0, sizeof(g_vga));
    memset(g_low, 0, sizeof(g_low));
    memset(g_uart, 0, sizeof(g_uart));
    g_ser_len = 0;
    g_ser[0] = '\0';
    g_hlt_count = 0;
    g_stopped = 0;
    g_irq_enabled = 1;
    g_unmapped = 0;
    g_unmapped_last = 0;
    g_oob_count = 0;
    g_last_oob = 0;
    g_kbd_head = g_kbd_tail = 0;
    g_kbd_status = 0;
}

void xemu_set_echo(int on) { g_ser_echo = on; }

const char* xemu_serial(void) { g_ser[g_ser_len] = '\0'; return g_ser; }
size_t      xemu_serial_len(void) { return g_ser_len; }

void xemu_serial_clear(void) { g_ser_len = 0; g_ser[0] = '\0'; }

uint64_t xemu_hlt_count(void) { return g_hlt_count; }
uint64_t xemu_unmapped_count(void) { return g_oob_count; }
uint32_t xemu_last_oob(void) { return g_last_oob; }
int      xemu_irq_enabled(void) { return g_irq_enabled; }

/* 把 VGA 缓冲转成 25 行文本（去掉行尾空格，保留每行内容）。
 * 属性字节单独提供，用于验证颜色。 */
const char* xemu_screen(void)
{
    static char out[25 * 81 + 1];
    char* o = out;
    for (int y = 0; y < 25; y++) {
        int last = -1;
        for (int x = 0; x < 80; x++) {
            uint8_t ch = g_vga[(y * 80 + x) * 2];
            if (ch != 0x20 && ch != 0x00) last = x;
        }
        for (int x = 0; x <= last; x++) *o++ = (char)g_vga[(y * 80 + x) * 2];
        *o++ = '\n';
    }
    *o = '\0';
    return out;
}

/* 取某个坐标的属性字节，用于断言「标题是蓝底白字」。 */
uint8_t xemu_attr_at(int x, int y)
{
    if (x < 0 || x >= 80 || y < 0 || y >= 25) return 0;
    return g_vga[(y * 80 + x) * 2 + 1];
}

uint8_t xemu_char_at(int x, int y)
{
    if (x < 0 || x >= 80 || y < 0 || y >= 25) return 0;
    return g_vga[(y * 80 + x) * 2];
}

/* 注入一个键盘扫描码（模拟用户敲键），供 kbdPoll 的测试用。 */
void xemu_push_key(uint8_t scancode)
{
    size_t next = (g_kbd_tail + 1) % KBD_MAX;
    if (next == g_kbd_head) return;       /* 满了就丢，和真机缓冲溢出一样 */
    g_kbd[g_kbd_tail] = scancode;
    g_kbd_tail = next;
    g_kbd_status |= 1;
}

/* 直接把低内存区（0x4000 引导参数）写进去，模拟引导扇区的行为。 */
void xemu_set_boot_params(uint32_t magic, uint32_t mem_mb, uint32_t drive)
{
    uint8_t* p = g_low + (0x4000 - LOWR_BASE);
    memcpy(p + 0, &magic,  4);
    memcpy(p + 4, &mem_mb, 4);
    memcpy(p + 8, &drive,  4);
}