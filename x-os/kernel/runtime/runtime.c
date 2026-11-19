/* ============================================================
 * runtime.c —— 内核运行时：替代 libc 的最小实现
 * ============================================================
 *
 * 这个文件存在的理由只有一句：**X 生成的代码需要 malloc 与 memcpy**。
 *
 * ── 为什么不能链接 libc ──────────────────────────────────
 *
 * 试过：`gcc -m32 -ffreestanding -nostdlib ... -lmsvcrt` 可以链接
 * 成功，但产物里带着对 kernel32.dll / msvcrt.dll 的导入表。裸机
 * 上没有 PE 加载器去解析这些导入，于是第一条调用就是 #PF。
 *
 * 更根本的是 libc 自己也需要操作系统：malloc 要 brk/mmap，printf
 * 要 write，fopen 要 open。裸机上这些全都不存在，所以「链接 libc」
 * 不是配置问题，是逻辑上不可能。
 *
 * ── 分配器为什么这么简单 ─────────────────────────────────
 *
 * 这是 bump 分配器：一个指针，分配就是往回退指针并返回旧值。
 * 不回收、不合并、不分裂。
 *
 * 为什么不写真正的 malloc：内核第一版的目标是「能启动、能跑通
 * 一条命令」。在还没有分页、没有中断的状态下写一个正确的
 * free-list 分配器，唯一的收益是内存利用率 —— 而那要等到内核
 * 真的跑起多个任务时才成为瓶颈。过早优化在这里的代价是把
 * 「为什么启动不了」的排查范围扩大一倍。
 *
 * free 是空操作。这在内核里是可接受的（很多东西活到关机），
 * 但**必须明确记下来** —— 因为 X 的字符串是不可变的引用计数
 * 对象，一个循环里拼字符串会持续吃内存。等有了分页，这块要
 * 换成 slab 或 buddy。
 */

#include "runtime.h"

/* ============================================================
 * 串口输出
 * ============================================================ */

/* COM1 的五个寄存器。偏移固定，由 16550 UART 的规定决定。 */
#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

void xk_serial_init(void) {
    outb(COM1 + 1, 0x00);   /* 关中断 —— 内核第一版用轮询，不用串口中断 */
    outb(COM1 + 3, 0x80);   /* DLAB=1，接下来写的是除数 */
    outb(COM1 + 0, 0x03);   /* 除数低字节 3 → 115200/3 = 38400 baud */
    outb(COM1 + 1, 0x00);   /* 除数高字节 */
    outb(COM1 + 3, 0x03);   /* 8 数据位、无校验、1 停止位，DLAB=0 */
    outb(COM1 + 2, 0xC7);   /* 开 FIFO、清空、14 字节触发 */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2（OUT2 不开收不到中断，
                               虽然现在是轮询，但保持与常规定义一致） */
}

/* 等发送保持寄存器空。
 *
 * ── volatile 与 memory clobber 都不能省 ──────────────────
 * volatile 保证每次都真读硬件；memory clobber 阻止编译器把
 * 后面的内存写移到这个循环之前（那会变成「先写数据再等」）。
 * 少了任何一个，症状都是「-O2 下偶发丢字符」。 */
static void serial_wait(void) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
        __asm__ volatile ("" ::: "memory");
    }
}

void xk_putc(char c) {
    serial_wait();
    outb(COM1, (uint8_t)c);
}

void xk_puts(const char* s) {
    while (*s) {
        /* 换行要发 \r\n：串口终端只有 \n 不会回到行首，
         * 表现为输出斜着往下走。 */
        if (*s == '\n') { xk_putc('\r'); }
        xk_putc(*s++);
    }
}

void xk_puthex(uint32_t v) {
    static const char* hx = "0123456789ABCDEF";
    xk_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        xk_putc(hx[(v >> i) & 0xF]);
    }
}

void xk_putint(int64_t v) {
    char b[24];
    int n = 0;
    if (v < 0) { xk_putc('-'); v = -v; }
    if (v == 0) { xk_putc('0'); return; }
    while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) { xk_putc(b[--n]); }
}

/* ============================================================
 * 内存分配：bump allocator
 * ============================================================ */

/* 堆从 1MB 开始。选 1MB 而不是紧跟内核之后，是因为：
 *   · 1MB 以下有大量固定用途（VGA、BIOS 数据区、EBDA、
 *     以及我们自己用的 0x10000 内核加载区），避开它们更省事
 *   · 1MB 是传统上「低端内存/高端内存」的分界，语义清晰
 *   · 内核本身很小（约 30KB），浪费这点空间无所谓
 *
 * 上限 16MB 是暂时值：还没做 E820 内存探测，不知道实际有多少。
 * 写死一个上限比「一直往上长直到踩到别的东西」安全 —— 后者
 * 的症状是随机崩溃，而前者是确定的「内存不足」。 */
#define HEAP_START 0x00100000u
#define HEAP_LIMIT 0x01000000u

static uintptr_t heap_ptr = HEAP_START;

/* 已分配字节数，只增不减。用于诊断输出。 */
static uintptr_t heap_used = 0;

void* xk_alloc(size_t n) {
    /* 8 字节对齐：x86 对 double 的对齐要求是 4，但 X 的值结构
     * 里有 int64_t，某些 SSE 指令（movq）要求 8。对齐到 8 的
     * 代价是平均浪费 4 字节，收益是避免 #GP 与对齐惩罚。 */
    uintptr_t p = (heap_ptr + 7u) & ~(uintptr_t)7u;
    uintptr_t end = p + (uintptr_t)n;
    if (end > HEAP_LIMIT || end < p) {   /* end < p 检查回绕 */
        xk_panic("heap exhausted (no paging yet, fixed 16MB limit)");
    }
    heap_ptr = end;
    heap_used += (uintptr_t)n;
    return (void*)p;
}

void xk_free(void* p) {
    /* 有意为空。理由见文件头的「分配器为什么这么简单」。
     * 保留这个函数而不是 #define 成空，是为了让调用点不改动 ——
     * 将来换成真正的分配器时，这一处改动就够。 */
    (void)p;
}

void* xk_realloc(void* p, size_t n) {
    /* bump 分配器无法原地扩展（后面的内存可能已经分给别人），
     * 所以 realloc 一律「新分配 + 拷贝 + 放弃旧的」。
     * 旧块大小未知，这里只能保守地拷 n 字节 —— 会造成读越过
     * 旧块末尾。这个缺陷是真实存在的，记录在此而不是假装没有。
     *
     * 缓解：X 生成的代码里 realloc 只用于数组扩容（XA），
     * 每次扩容后立刻会覆盖新区域，读到超出旧块末尾的那部分是
     * 即将被覆盖的垃圾，不会传播。这不是「正确」，是「当前
     * 使用形态下不出错」。 */
    void* q = xk_alloc(n);
    if (p) {
        uint8_t* d = (uint8_t*)q;
        const uint8_t* s = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) { d[i] = s[i]; }
    }
    return q;
}

uintptr_t xk_heap_used(void) { return heap_used; }
uintptr_t xk_heap_ptr(void)  { return heap_ptr; }

/* ── libc 名字的包装 ─────────────────────────────────────
 *
 * X 生成的代码调用的是 malloc/free/realloc —— 那些名字来自
 * PRELUDE 里对 libc 的调用，改不了（改了就不叫「同一份代码」）。
 *
 * 所以在内核侧提供同名函数，把调用转给 xk_* 实现。
 * 这是「同名替换」而不是「改名适配」，好处是生成代码一个字都不用动。
 *
 * 注意不能 #define malloc xk_alloc —— 那样 runtime.c 自己的
 * 函数定义也会被替换，变成 xk_alloc 自己定义自己。
 */
void* malloc(size_t n) { return xk_alloc(n); }
void  free(void* p) { xk_free(p); }
void* realloc(void* p, size_t n) { return xk_realloc(p, n); }

/* ============================================================
 * 字符串与内存（供 X 生成代码调用，签名必须与 libc 一致）
 * ============================================================ */

/* 这些不用 __builtin_ 或 SSE：freestanding 下 -m32 的
 * __builtin_memcpy 可能展开成对 memcpy 自身的调用（递归），
 * 在 -O0 下尤其容易。手写循环最稳，且内核里这些调用量很小。 */

void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    while (n--) { *dp++ = *sp++; }
    return d;
}

void* memset(void* d, int c, size_t n) {
    uint8_t* dp = (uint8_t*)d;
    while (n--) { *dp++ = (uint8_t)c; }
    return d;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) {
        if (*x != *y) { return (int)*x - (int)*y; }
        x++; y++;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) { n++; }
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* strncat(char* d, const char* s, size_t n) {
    char* p = d + strlen(d);
    while (n-- && *s) { *p++ = *s++; }
    *p = 0;
    return d;
}

char* strpbrk(const char* s, const char* set) {
    for (; *s; s++) {
        for (const char* q = set; *q; q++) {
            if (*s == *q) { return (char*)s; }
        }
    }
    return 0;
}

/* 最简 strtod：只处理 [+-]?digits[.digits]。
 *
 * 不支持指数、inf、nan、十六进制浮点。理由：内核里用到它的地方
 * 只有「读配置文件里的样式参数」，而配置文件是内核自己写的。
 * 但这一点必须明确 —— 用户写了 "1e10" 会得到 1.0，这不是
 * 四舍五入误差，是静默错解。所以文档里要写清楚。 */
double strtod(const char* s, char** end) {
    const char* p = s;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    double v = 0.0;
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (double)(*p - '0'); p++; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') {
            v += (double)(*p - '0') * f;
            f *= 0.1;
            p++;
        }
    }
    if (end) { *end = (char*)p; }
    return neg ? -v : v;
}

/* ============================================================
 * 内核 panic
 * ============================================================ */

/* 与生成代码里的 rt_err 是两条独立的路径：
 *   rt_err    由 X 代码触发（整数溢出、类型错、裸机内建误用）
 *   xk_panic  由内核自己的 C 侧触发（堆耗尽、页表错误）
 * 两条都终止系统，但消息来源不同，分开便于定位。 */
void xk_panic(const char* msg) {
    __asm__ volatile ("cli");
    xk_puts("\n  KERNEL PANIC: ");
    xk_puts(msg);
    xk_puts("\n  system halted.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}