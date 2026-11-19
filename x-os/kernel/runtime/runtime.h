/* runtime.h —— 内核运行时对外接口
 *
 * 这个头文件是 C 侧与 X 生成代码之间的**唯一契约**。
 *
 * ── 为什么需要这个头，而不是让 X 直接调 runtime.c 里的函数 ──
 *
 * X 代码里能用的只有语言内建（memWrite32/portOut8 等）。它不能
 * 「调用某个 C 函数」—— 语言没有 extern fn（那是 P1 的事）。
 *
 * 所以分工是：
 *   · X 侧写内核逻辑（串口输出、GDT 表、shell）
 *   · C 侧提供 X 生成代码隐式需要的基础设施（malloc/memcpy）
 *
 * 后者不是「内核功能」，是「让 X 代码能跑起来的地基」。两者
 * 混在一个文件里会让「这是什么语言写的」变得含糊，所以分开。
 */
#ifndef XK_RUNTIME_H
#define XK_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/* ── 串口 ── */
void xk_serial_init(void);
void xk_putc(char c);
void xk_puts(const char* s);
void xk_putint(int64_t v);
void xk_puthex(uint32_t v);

/* ── 内存 ── */
void*     xk_alloc(size_t n);
void      xk_free(void* p);
void*     xk_realloc(void* p, size_t n);
uintptr_t xk_heap_used(void);
uintptr_t xk_heap_ptr(void);

/* ── 致命错误 ── */
void xk_panic(const char* msg);

#endif /* XK_RUNTIME_H */