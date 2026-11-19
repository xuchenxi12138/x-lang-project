/* ============================================================
 * boot.s —— X-OS 引导扇区 + 保护模式切换
 * ============================================================
 *
 * ── 为什么这一段必须用汇编，不能是 X ──────────────────────
 *
 * 这里做的四件事全部发生在「C 语言假设还能成立」之前：
 *
 *   1. 复位后 CPU 处于 16 位实模式，段寄存器语义是 seg<<4+off，
 *      而 GCC 生成的 -m32 代码假定平坦 32 位保护模式。语义不同。
 *   2. 加载 GDT 并置 CR0.PE —— 一条 lgdt + 一次 mov cr0 的事，
 *      但 C 里没有任何语法能表达「我现在要改变 CPU 的运行模式」。
 *   3. 远跳转刷新 CS 影子寄存器 —— 必须用 ljmp $sel,$off 形式。
 *   4. 建立栈。C 的第一条指令就可能用栈，而 BIOS 给的栈位置
 *      与大小都不受控。
 *
 * 这四件事是「任何内核的汇编都必须做的事」，不是 X 语言缺失 ——
 * 即便用 C 写内核（Linux/Windows），这一层也永远是 .s 文件。
 *
 * ── 镜像布局 ──────────────────────────────────────────────
 *
 *   扇区 0        本文件（512 字节，末尾 0xAA55）
 *   扇区 1..N     kernel.bin（X 编译产物 + runtime 桩）
 *
 * 引导扇区把后续扇区读到物理地址 0x10000 再跳过去。选 0x10000
 * 而不是 0x7E00（紧接引导扇区）是因为 0x7E00 只有 512 字节余量，
 * 内核很快就会超过；0x10000 是 64KB 边界，不与任何 BIOS 结构冲突。
 *
 * ── 内存地图（改动前务必与 kernel.ld / runtime.c 对齐）────
 *
 *   0x00400 - 0x004FF   BIOS 数据区（不碰）
 *   0x04000             内核参数块（本文件写入 magic）
 *   0x10000             kernel.bin 加载起点（kernel.ld 的 . = 0x10000）
 *   0x213D8 起           .vstack 值栈 786 KB（不导出，运行时清零）
 *   0xE13D8             .vstack 结束
 *   0xF8000             内核栈底（栈向下长，用不到就只是保留）
 *   0x100000            内核栈顶 = 1MB 边界；同时是堆的起点
 *   0x100000 - 0x1000000  内核堆（runtime.c 的 HEAP_START/LIMIT）
 *
 * 栈顶取 0x100000 而不是原来的 0x90000：值栈（.vstack）从
 * 0x213D8 一路铺到 0xE13D8，0x90000 正好**落在值栈内部** ——
 * 栈一压下去就会踩掉值栈里的 V 结构，症状是随机崩溃且崩在
 * 与栈毫无关系的函数里。这是本文件上一版的真 bug，靠 build.sh
 * 的「值栈末端必须低于栈底」断言守住。
 *
 * ── 读盘为什么用 LBA 而不用 CHS ───────────────────────────
 *
 * INT 13h 的 CHS 寻址在软盘/老硬盘上有 1024 柱面限制，而 LBA
 * 扩展读（AH=0x42）在所有 2000 年后的 BIOS 上都可用，且没有
 * 几何参数换算问题。QEMU 与真机都支持。
 *
 * ── 每次只读 1 个扇区（而不是一次读 160 个）──────────────
 *
 * 上一版用「一次 DAP 传输 160 扇区」，有两个硬错误：
 *
 *   · DAP 的传输数上限在许多 BIOS 上是 127 扇区，超过会
 *     **静默截断** —— 返回成功但只搬了一部分，症状是内核
 *     后半段是垃圾。
 *   · 更严重的是那次传输会**跨越 64KB 边界**（0x10000 + 80KB），
 *     而实模式下 DMA 不能跨 64KB 段边界，部分 BIOS 直接失败。
 *
 * 改成每轮读 1 个扇区、手动推进目标段与 LBA：
 * 每扇区 1 次 INT 13h，138 个扇区就是 138 次调用。慢（几十毫秒）
 * 但绝对正确，且永远不会跨 64KB 边界。引导阶段的几十毫秒
 * 完全不可感知，不值得为它引入边界风险。
 *
 * 所有状态（剩余扇区数、当前 LBA、目标段/偏移）都放在**内存变量**
 * 里而不是寄存器里 —— INT 13h 会破坏一部分寄存器的约定在
 * 不同 BIOS 上不一致，读内存是唯一不依赖约定的做法。
 */

.code16                         /* 复位后是实模式 */
.section .text
.global _boot_start

_boot_start:
    cli                         /* 关中断：此时 IVT 还没建立 */
    xorw    %ax, %ax
    movw    %ax, %ds            /* 所有段清零，得到平坦的 16 位空间 */
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp        /* 栈向下长，起点放在引导扇区下方 */

    movb    %dl, boot_drive     /* BIOS 在 dl 里给启动盘号，必须存下来 */

    /* ── 打开 A20 地址线 ────────────────────────────────────
     * 不打开的话，地址第 20 位被强制为 0，于是 1MB 以上的内存
     * 「回绕」到 0 —— 内核被加载到 0x10000 之后一旦超过 0xFFFFF
     * 就会覆盖自己。症状是随机崩溃，且崩溃位置随代码大小变化。
     * 用 BIOS 的 INT 15h AX=2401（比键盘控制器法通用）。 */
    movw    $0x2401, %ax
    int     $0x15

    /* ── 写内核参数块（0x4000）──────────────────────────────
     *
     * 内核的入口签名是 void _start(void) —— 没有传参通道，
     * 所以引导信息只能通过**约定地址**交接。这与 Linux 的
     * boot_params（zero page）、Multiboot 的信息结构是同一
     * 思路：引导器与内核之间靠固定的内存布局通信。
     *
     * 写入三个 32 位字：
     *   +0  magic "XOS1"（0x584F5331），内核用它判断参数是否有效
     *   +4  内存大小 MB（0 = 未知，本版不探测）
     *   +8  启动盘号（内核将来做文件系统时需要）
     *
     * 注意 ds 已经是 0，所以 0x4000 直接就是物理地址 0x4000。 */
    movl    $0x584F5331, 0x4000
    movl    $0,          0x4004
    xorw    %ax, %ax
    movb    boot_drive, %al
    movl    %eax,        0x4008

    /* ── 用 LBA 扩展读把内核读进来 ───────────────────────── */
    movw    $0x1000, %ax        /* 目标段：0x1000:0x0000 = 物理 0x10000 */
    movw    %ax, dest_seg
    movw    $1, cur_lba         /* 从 LBA 1 开始（0 是本扇区） */

read_loop:
    movw    sectors_left, %ax
    testw   %ax, %ax
    jz      read_done

    /* ── 填 DAP 的可变字段 ──────────────────────────────────
     * 固定字段（长度 0x10、保留 0、传输数 1）只在 dap 里写
     * 一次，每轮只改 LBA 与目标缓冲区。 */
    movw    cur_lba, %ax
    movw    %ax, dap_lba           /* LBA 低 16 位（镜像 < 32MB，够用） */
    movw    $0,   dap_lba+2        /* LBA 高 16 位 */
    movw    dest_off, %ax
    movw    %ax, dap_buf_off
    movw    dest_seg, %ax
    movw    %ax, dap_buf_seg

    movw    $dap, %si
    movb    boot_drive, %dl
    movb    $0x42, %ah
    int     $0x13
    jc      disk_error

    /* ── 推进：LBA+1，缓冲区 +512，必要时换段 ───────────────
     * 段内偏移加 512；一旦溢出（超过 0xFFFF）说明跨了 64KB，
     * 段基址加 0x1000（= 64KB），偏移自然回绕到 0。 */
    incw    cur_lba
    addw    $512, dest_off
    jnc     no_seg_advance
    movw    dest_seg, %ax
    addw    $0x1000, %ax
    movw    %ax, dest_seg
no_seg_advance:
    decw    sectors_left
    jmp     read_loop

read_done:
    /* ── 进入保护模式 ───────────────────────────────────── */
    cli
    lgdt    gdt_desc

    movl    %cr0, %eax
    orl     $1, %eax            /* CR0.PE = 1 */
    movl    %eax, %cr0

    /* 远跳转：刷新 CS 并切到 32 位段。
     * 这一步不能省 —— PE 置位后当前 CS 仍是 16 位描述符，
     * 下一条指令的解码宽度还是 16 位。 */
    ljmp    $0x08, $pm_entry

disk_error:
    /* 读盘失败只可能发生在很早期，此时还没有串口。
     * 用 BIOS 的 TTY 输出打一行，然后停住。 */
    movw    $disk_msg, %si
1:  lodsb
    testb   %al, %al
    jz      2f
    movb    $0x0E, %ah
    int     $0x10
    jmp     1b
2:  cli
    hlt
    jmp     2b

/* ── 32 位段 ────────────────────────────────────────────── */
.code32
pm_entry:
    movw    $0x10, %ax          /* 数据段选择子 */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* 内核栈：顶在 1MB 边界，向下长到 0xF8000（32 KB 预算）。
     * 为什么不是 0x90000 —— 见文件头内存地图，那个地址落在
     * 值栈 .vstack（0x213D8..0xE13D8）内部。 */
    movl    $0x100000, %esp

    /* 清屏（VGA 文本模式 80x25，0xB8000）。
     * 用 0x0F20（黑底白字空格）填满 2000 个字符单元。
     * 注意 VGA 卡的颜色是「属性在奇字节、字符在偶字节」，
     * 小端下 0x0F20 正好是 字符=0x20 属性=0x0F。 */
    movl    $0xB8000, %edi
    movl    $2000, %ecx
    movw    $0x0F20, %ax
    rep     stosw

    /* 跳到 X 编译产出的入口。
     * 注意这里是 call 而不是 jmp：_start 末尾是死循环，
     * 用 call 是为了让返回地址留在栈上 —— 万一有人改成 return，
     * 会跳到下面这个 hlt 而不是随机地址。 */
    call    _start
3:  cli
    hlt
    jmp     3b

/* ── GDT ──────────────────────────────────────────────── */
.align 8
gdt:
    .quad 0                     /* 空描述符：CPU 要求第 0 项全零 */
gdt_code:
    /* 代码段：基址 0，界限 4G，可读可执行，32 位，粒度 4K
     * 0x00CF9A000000FFFF：
     *   FFFF  界限低 16 位
     *   9A    访问字节 = P|DPL0|S|E|R
     *   CF    粒度字节 = G|D/B|界限高 4 位
     *   00    基址高 8 位 */
    .quad 0x00CF9A000000FFFF
gdt_data:
    /* 数据段：同基址界限，可读写
     * 0x00CF92000000FFFF */
    .quad 0x00CF92000000FFFF
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1   /* 界限 = 大小 - 1 */
    .long   gdt

/* ── DAP（Disk Address Packet，16 字节）─────────────────
 *
 * 布局（BIOS 规定，不可改）：
 *   +0  byte  包长度 = 0x10
 *   +1  byte  保留 = 0
 *   +2  word  传输扇区数（本版恒为 1）
 *   +4  word  目标缓冲区偏移
 *   +6  word  目标缓冲区段
 *   +8  qword 起始 LBA
 *
 * 上一版的 bug 就在这里：`movb %cl, 2(%si)` 把 LBA 写进了
 * 偏移 +2，也就是**传输扇区数**字段 —— 于是每轮实际传输的
 * 扇区数等于 LBA 的低字节，且 LBA 恒为 0（一直读引导扇区）。
 * 症状是内核内容全是引导扇区。 */
.align 4
dap:
    .byte   0x10                /* 包长度 */
    .byte   0x00                /* 保留 */
dap_count:
    .word   1                   /* 传输扇区数 */
dap_buf_off:
    .word   0                   /* 目标偏移（每轮改写） */
dap_buf_seg:
    .word   0x1000              /* 目标段（每轮改写） */
dap_lba:
    .quad   1                   /* 起始 LBA（每轮改写） */

/* ── 读盘状态（放在内存而不是寄存器）─────────────────────
 *
 * KERNEL_SECTORS 必须覆盖 kernel.bin。build.sh 里有断言：
 * kernel.bin 超过这个数就直接报错，而不是产出一个「读不全」
 * 的镜像。160 扇区 = 81920 字节，当前内核 70616 字节。 */
.set KERNEL_SECTORS, 160

sectors_left:
    .word   KERNEL_SECTORS
cur_lba:
    .word   1
dest_off:
    .word   0
dest_seg:
    .word   0x1000

boot_drive:
    .byte   0
disk_msg:
    .asciz  "X-OS: disk read failed"

/* 引导扇区必须以 0xAA55 结尾，且总长 512 字节 */
.org 510
.word 0xAA55