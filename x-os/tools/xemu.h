/* ============================================================
 * xemu.h —— 宿主仿真层对外接口
 * ============================================================
 *
 * 与 xemu.c 配套。定义 X_EMU 编译出来的内核代码会自动调用
 * xe_mem_read / xe_port_out 等钩子；本头文件把「测试/驱动」
 * 需要的那部分暴露出来。
 *
 * ── 为什么分成两层（生成代码的钩子 / 测试 API）──────────
 *
 * 钩子（xe_*）是**被调用方**，签名必须稳定 —— 生成代码里写死
 * 了它们。测试 API（xemu_*）是**调用方**，可以随测试需要增长。
 * 混在一起会让「改测试」有风险动到「内核怎么访问硬件」。
 */
#ifndef XEMU_H
#define XEMU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 被生成代码调用的硬件钩子 ───────────────────────────── */
uint32_t xe_mem_read(uint32_t a, int w);
void     xe_mem_write(uint32_t a, uint32_t v, int w);
uint32_t xe_port_in(uint16_t p, int w);
void     xe_port_out(uint16_t p, uint32_t v, int w);
void     xe_cpu_hlt(void);
void     xe_cpu_cli(void);
void     xe_cpu_sti(void);
void     xe_cpu_invlpg(uint32_t a);

/* ── 测试/驱动 API ─────────────────────────────────────── */

/* 复位所有模拟硬件状态。每个测试用例开头都应调一次。 */
void xemu_init(void);

/* 串口输出是否同时打到宿主 stdout（默认开）。 */
void xemu_set_echo(int on);

/* 内核通过 COM1 输出的全部字节（NUL 结尾的字符串）。 */
const char* xemu_serial(void);
size_t      xemu_serial_len(void);
void        xemu_serial_clear(void);

/* VGA 文本缓冲的 25 行快照（每行去掉行尾空格，行间 \n）。
 * 返回值指向静态缓冲区，下次调用会覆盖。 */
const char* xemu_screen(void);

/* 指定坐标上的字符/属性字节，用于断言颜色与排版。 */
uint8_t xemu_char_at(int x, int y);
uint8_t xemu_attr_at(int x, int y);

/* hlt 被执行了多少次（判断 idle 循环是否真的在等）。 */
uint64_t xemu_hlt_count(void);

/* 访问未映射物理地址的次数（应恒为 0）。 */
uint64_t xemu_unmapped_count(void);
uint32_t xemu_last_oob(void);

/* cli/sti 之后的关中断状态。 */
int xemu_irq_enabled(void);

/* 设置逃逸回调：注入队列空了之后，内核第一次读键盘数据口时
 * 调用它（通常在里面对 longjmp，从 shellLoop 的死循环里出来）。
 * 设为 NULL 取消。 */
void xemu_set_escape(void (*fn)(void));

/* 注入一个键盘扫描码，模拟用户敲键。 */
void xemu_push_key(uint8_t scancode);

/* 写引导参数块（0x4000）：magic / 内存 MB / 启动盘号。 */
void xemu_set_boot_params(uint32_t magic, uint32_t mem_mb, uint32_t drive);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_H */