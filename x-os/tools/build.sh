#!/bin/sh
# ============================================================
# build.sh —— 构建 X-OS 内核镜像
# ============================================================
#
# ── 为什么不直接用一个 gcc 命令 ──────────────────────────
#
# 构建链有六步，每步都有非平凡的理由，把它们写在一个脚本里
# 而不是 makefile，是因为要能在 Windows 的 Git Bash 里跑 ——
# 而 makefile 的制表符/换行在这些环境里更容易出问题。
#
# 六步：
#   1. X 源码 → C              （x build --emit=c）
#   2. C → freestanding 目标文件（gcc -m32 -ffreestanding -nostdlib）
#   3. 汇编 → 目标文件          （引导扇区 + 入口桩）
#   4. 链接                    （自定义链接脚本，加载地址 0x10000）
#   5. 导出裸二进制             （objcopy -O binary）
#   6. 拼成可引导镜像           （引导扇区 + 内核 + 填充 + 0xAA55）
#
# ── 工具链为什么指向 C:/xgcc ──────────────────────────────
#
# MinGW 装在 C:/xgcc/mingw64，不在系统 PATH 里（避免污染用户
# 环境）。脚本显式把它加进 PATH，而不是要求用户先改环境变量。
# 这与 x-lang 自己的 build.sh 保持一致的做法。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# x-os 是 x-lang 的子目录（同仓），不是兄弟目录。
# 写成 ../x-lang 会解析到 x-lang/x-lang —— 一个不存在的路径。
XLANG=$(cd "$ROOT/.." && pwd)

# ── 构建目录必须在纯 ASCII 路径下 ─────────────────────────
#
# 这是本项目已知的 MinGW 坑（x-lang/tools/build.sh 顶部记的是同一个）：
# cc1.exe 在含中文的路径里建不出中间 .o 文件，报的是
#
#   Fatal error: can't create ...\kmain.o: No such file or directory
#
# 这条消息说的是「目录不存在」，而目录明明存在 —— 真正的原因是
# 路径编码。更麻烦的是它会先把源码路径里的中文显示成乱码
# （F:/é¡¹ç›®/...），于是排查方向容易偏到「源文件读错了」。
#
# 所以：中间产物与最终镜像都放在 C:/xtmp/xos-build，纯 ASCII。
# 源码仍留在项目里（源码路径有中文没问题 —— 编译器读文件用的是
# Windows 原生 API，不经过 cc1 的路径处理）。
OUT="/c/xtmp/xos-build"
MINGW="/c/xgcc/mingw64/bin"

# 临时目录必须纯 ASCII：MinGW 的 cc1 在含中文的 %TEMP% 里
# 建不出 .o（报 "can't create ccXXX.o: No such file"，
# 看起来像目录被删，实际是编码问题）。
export TMP="C:/xtmp"
export TEMP="C:/xtmp"
mkdir -p "$TMP" "$OUT"

# ── PATH 为什么这样写 ─────────────────────────────────────
#
# 不能写 `PATH="$MINGW:$PATH"` —— 在 POSIX sh 里那是给**当前 shell**
# 的变量赋值，但它不会导出给子进程，于是 gcc 找不到。（bash 里
# 直接赋值会自动导出，sh 里不会 —— 这个差别让同一段脚本在两种
# shell 下行为不同。）必须先赋值再 export。
PATH="$MINGW:$PATH"
export PATH

# ── 共用编译选项 ──────────────────────────────────────────
#
# -m32              32 位保护模式。64 位内核需要配置长模式、
#                   改写 GDT 的 L 位、重设页表，是第二阶段的事。
# -ffreestanding    告诉 GCC「没有宿主环境」：不假设 main、
#                   不内置 memcpy 等库函数存在。
# -nostdlib         不链接任何库（连 libgcc 都不要）。
# -fno-builtin      不把 memset/memcpy 调用替换成「GCC 认为更好」的
#                   实现 —— 那些替换往往依赖 SSE，而内核可能还没开。
# -fno-stack-protector  不生成 __stack_chk_fail 调用（那是 libc 函数）。
# -fno-pic / -fno-pie   不生成 GOT/PLT 相对寻址 —— 内核是固定地址加载，
#                   用 GOT 会要求一个额外的基址寄存器，而我们没设。
CFLAGS="-m32 -O2 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
        -fno-pic -fno-pie -ffunction-sections -fdata-sections \
        -DX_FREESTANDING -Wall -Wextra \
        -Wno-array-bounds"

echo "[1/6] X 源码 → C"
"$XLANG/build/x.exe" build "$ROOT/kernel/src/kmain.xle" \
    --emit=c -o "$OUT/kmain.c"

echo "[2/6] C → 目标文件"
# shellcheck disable=SC2086
gcc $CFLAGS -c "$OUT/kmain.c" -o "$OUT/kmain.o"

echo "[3/6] 内核运行时"
gcc $CFLAGS -c "$ROOT/kernel/runtime/runtime.c" -o "$OUT/runtime.o"

echo "[4/6] 引导与入口汇编"
# ── 读盘扇区数由这里注入，不在 boot.s 里写死 ──────────────
#
# 上一版 boot.s 里写死 KERNEL_SECTORS=160（80 KB）。内核长大
# 到超过它就**静默读到一半**，症状是内核后半段是垃圾、随机崩，
# 且崩溃位置随内核大小漂移 —— 极难定位。
#
# 现在扇区数从编译期常量注入，且下面 [6/6] 有断言：
# 内核字节数超过 BOOT_SECTORS × 512 就直接报错。两处口径
# 一致（都用这个变量），不存在「改了一处忘另一处」。
BOOT_SECTORS=160
gcc -m32 -c "$ROOT/kernel/boot/entry.s" -o "$OUT/entry.o"
gcc -m32 -DKERNEL_SECTORS=$BOOT_SECTORS -c "$ROOT/kernel/boot/boot.s" -o "$OUT/boot.o"

echo "[5/6] 链接内核"
# --image-base 0：PE 默认镜像基址是 0x400000，而我们把段放在
# 0x10000。不设成 0 会报 "section below image base"，且导出的
# 裸二进制里会带上巨大的空洞。
#
# ── 为什么**不开** --gc-sections（重要，别加回去）─────────
#
# 本来开了，想省掉没被引用的函数。结果产出的 kernel.exe 里有
# 9 个未解析符号，而且它们恰恰是**被引用的** runtime 函数：
#
#   U _xk_alloc  U _xk_free   U _xk_heap_ptr  U _xk_heap_used
#   U _xk_putc   U _xk_puthex U _xk_putint    U _xk_realloc
#   U _xk_serial_init
#
# 原因：MinGW 的 PE 后端对 `$` 分段（.text$函数名）的 GC 可达性
# 分析不可靠 —— 它把 .text$xk_alloc 这类段当成独立可回收单元，
# 而引用方在另一个 .text$ 段里，跨段的引用没被算进可达集合。
# 用 --undefined 显式把符号钉成 GC 根能修好一部分（实测从 33 个
# 降到 9 个），但剩下的 xk_* 仍然丢 —— 因为它们是 .text$ 段内
# 的局部可见符号，钉不住。
#
# 代价对比：不开启时 .text 从 0x105D0 涨到 0x10A20（+1.1 KB），
# 换来 0 个未解析符号。70 KB 的镜像里 1.1 KB 毫无意义，
# 而一个「链接成功但符号缺失」的镜像是直接跑不起来的。
#
# 如果将来确实需要 GC：改用 ELF 目标（i386-elf-gcc）而不是
# PE，或者给 runtime.c 的函数去掉 -ffunction-sections。不要
# 在 PE 上给 --gc-sections 加更多 --undefined 硬凑。
ld -m i386pe -T "$ROOT/kernel/boot/kernel.ld" --image-base 0 \
   -e _entry --undefined=__start \
   -o "$OUT/kernel.exe" "$OUT/entry.o" "$OUT/kmain.o" "$OUT/runtime.o"

echo "[6/6] 拼装可引导镜像"
# ── 未解析符号必须为 0（这条比大小断言重要）──────────────
#
# `ld` 对 PE 的未定义符号**默认不报错**，它只把这些符号标成 U
# 留在符号表里，等运行时由加载器解析 —— 而内核没有加载器。
# 症状是调用到该符号时跳到地址 0，CPU 报三声蜂鸣或直接复位。
#
# 实测踩过一次：开了 --gc-sections 后 runtime.c 的 xk_alloc
# 等 9 个函数被误回收，链接器一声不响，产出一个「成功」的镜像。
# 所以必须自己查，而且要在符号表还在的时候查（strip 之后就没了）。
UNDEF=$(nm "$OUT/kernel.exe" 2>/dev/null | grep -c " U " || true)
if [ "$UNDEF" -ne 0 ]; then
    echo "error: 链接产物有 $UNDEF 个未解析符号（内核没有动态加载器）" >&2
    nm "$OUT/kernel.exe" | grep " U " | sed 's/^/       /' >&2
    echo "       常见原因：runtime.c 里少了对应实现，或链接时" >&2
    echo "       --gc-sections 误回收了被引用的段。" >&2
    exit 1
fi
echo "  未解析符号: 0"
# ── 为什么只导出三段，不导出 .vstack ──────────────────────
#
# 链接脚本的顺序是：.text → .rodata → .data → .vstack。
# 前三个是连续的（地址紧挨着），所以一次
# `objcopy -O binary -j .text -j .rodata -j .data` 导出的
# 就是「从 0x10000 开始的连续 ~70 KB」，没有空洞。
#
# 值栈段（.vstack，786 KB 全零）排在最后，不进镜像 —— 它的
# 786 KB 地址空间由链接器算好（.data 之后），运行时由 entry.s
# 的 rep stosl 清零。因为它排在所有导出段之后，「不导出」不会
# 造成地址空洞。
#
# ── 前提与脆弱之处 ───────────────────────────────────────
#
# 前提：.text/.rodata/.data 三段的 VMA 必须严格连续。
# 任何一段被对齐填充（比如 .data 要求 32 字节对齐而 .rodata
# 结束时偏移是奇数）就会产生几字节的空洞 —— objcopy 会用零
# 填上，这几字节的错位会让所有绝对地址引用偏移。
#
# 所以下面有断言：导出后的 bin 大小必须等于三段实际大小之和。
# 不等就说明有空隙，当场报错而不是产出一个地址错位的镜像。
objcopy -O binary -j .text -j .rodata -j .data \
        "$OUT/kernel.exe" "$OUT/kernel.bin"

objcopy -O binary -j .text "$OUT/boot.o" "$OUT/boot.bin"

SIZE=$(wc -c < "$OUT/boot.bin" | tr -d ' ')
if [ "$SIZE" -ne 512 ]; then
    echo "error: 引导扇区是 $SIZE 字节，必须是 512" >&2
    echo "       检查 boot.s 末尾的 .org 510 与 .word 0xAA55" >&2
    exit 1
fi

# ── 布局检查：一口气查四件事 ──────────────────────────────
#
# 用 awk 而不是 shell 算术，因为 POSIX sh 的 `[ ]` 不认 0x 前缀
# （bash 的 $((0x...)) 认，但 `sh` 在部分环境下是 dash，不认）。
# 早先那版就踩了这个坑：报 "[: 0x10000: integer expected"。
#
# 四个判据，各自抓不同的错：
#
#   ① .text 的加载地址必须是 0x10000
#      → 引导扇区跳这里。错一位就崩在最开头。
#
#   ② 三个导出段的地址必须递增，且每段都完整落在导出区内
#      → 抓「段被放错位置」。
#
#   ③ 段间空隙必须很小（< 4 KB）
#      → 抓「大段夹在导出段之间」。上一版 .vstack 就夹在
#        .rodata 与 .data 之间，空隙 786 KB，镜像必然爆掉。
#        几十字节的对齐填充无害（objcopy 用零填，没人引用）。
#
#   ④ kernel.bin 的字节数 == .data 结束地址 - 0x10000
#      → 这是最强的一条：objcopy 按 LMA 铺开，导出区总长
#        必须恰好等于「最后一段的结束地址减起始地址」。
#        少了说明段跑到导出区外，多了说明有空隙被填了零。
LAYOUT=$(objdump -h "$OUT/kernel.exe" |
    awk '$2==".text"||$2==".rodata"||$2==".data"||$2==".vstack" {
             print $2, strtonum("0x" $3), strtonum("0x" $4) }')

if [ -z "$LAYOUT" ]; then
    echo "error: 链接产物里一个预期段都没找到" >&2
    objdump -h "$OUT/kernel.exe" | grep -E "^ +[0-9]" >&2
    exit 1
fi

BIN_SZ=$(wc -c < "$OUT/kernel.bin" | tr -d ' ')

echo "$LAYOUT" | awk -v binsz="$BIN_SZ" -v base=65536 '
function die(m) { printf "error: %s\n", m > "/dev/stderr"; bad=1; exit 1 }
BEGIN { prev_end = base; prev_name = "(起始)" ; n = 0 }
{
    name = $1; sz = $2; addr = $3;
    n++;

    # ① 入口地址
    if (name == ".text" && addr != base)
        die(sprintf(".text 的加载地址是 0x%x，必须是 0x10000（引导扇区跳这里）", addr))

    # ② 地址递增 / 不许重叠
    if (addr < prev_end)
        die(sprintf("%s 的地址 0x%x 落在 %s 的范围内（%s 结束于 0x%x）", name, addr, prev_name, prev_name, prev_end))

    # ③ 空隙
    gap = addr - prev_end;
    if (gap > 4096)
        die(sprintf("%s 与 %s 之间有 %d 字节空隙 —— 说明有大段被塞进了导出区，镜像会变大", prev_name, name, gap))

    if (name == ".vstack") {
        # 值栈必须排在所有导出段之后，否则「不导出它」会在
        # 导出区里留下 786 KB 空洞。
        printf "  note: .vstack 排在 0x%x（导出区之后，正确）\n", addr
    } else {
        export_end = addr + sz;
    }

    prev_end = addr + sz; prev_name = name;
}
END {
    if (bad) exit 1
    # ④ 导出长度
    want = export_end - base;
    if (binsz != want)
        die(sprintf("kernel.bin 是 %d 字节，但导出区（0x10000..0x%x）应为 %d 字节", binsz, export_end, want))
    printf "  导出区: 0x10000 .. 0x%x（%d 字节）\n", export_end, want
}'

if [ $? -ne 0 ]; then
    echo "       段布局如下：" >&2
    objdump -h "$OUT/kernel.exe" | grep -E "^ +[0-9]" >&2
    exit 1
fi

# ── 镜像必须装得进引导读盘窗口 ────────────────────────────
#
# boot.s 每轮读 1 个扇区、共 KERNEL_SECTORS 轮。这个数由
# BOOT_SECTORS 注入，这里用**同一个变量**做上限检查 ——
# 两处口径一致，不会出现「boot.s 改了脚本没改」。
#
# 超过就**直接失败**而不是打警告：一个读过头的镜像在真机上
# 只会显示一堆乱码或三声蜂鸣，没人会想到「内核比读盘窗口大」。
KERNEL_MAX=$((BOOT_SECTORS * 512))
KSIZE=$(wc -c < "$OUT/kernel.bin" | tr -d ' ')
if [ "$KSIZE" -gt "$KERNEL_MAX" ]; then
    echo "error: kernel.bin 是 $KSIZE 字节，超过引导读盘窗口 $KERNEL_MAX 字节" >&2
    echo "       （BOOT_SECTORS=$BOOT_SECTORS 扇区 × 512）" >&2
    echo "       把 tools/build.sh 里的 BOOT_SECTORS 调大，或减小内核。" >&2
    exit 1
fi

# ── 值栈不能与内核栈重叠 ──────────────────────────────────
#
# 这条断言是本轮最重要的：值栈 .vstack 在 0x213D8 起、786 KB，
# 一直铺到 0xE13D8。而上一版 boot.s 把内核栈顶设在 0x90000 ——
# **0x90000 落在值栈内部**。症状是：栈一压下去就踩掉 V 结构，
# 表现为随机崩溃，且崩在与栈毫无关系的函数里（因为被踩的是
# 值栈里的临时值，不是栈自己的内容）。
#
# 现在栈顶取 0x100000（1MB），栈向下最多用到 0xF8000。
# 值栈末端必须低于 0xF8000，否则报错。
VSTACK_END=$(objdump -h "$OUT/kernel.exe" |
    awk '$2==".vstack" { print strtonum("0x" $4) + strtonum("0x" $3) }')
KSTACK_FLOOR=1015808          # 0xF8000：内核栈最多向下用到这里
if [ -n "$VSTACK_END" ] && [ "$VSTACK_END" -gt "$KSTACK_FLOOR" ]; then
    echo "error: 值栈结束于 0x$(printf '%x' "$VSTACK_END")，压到了内核栈区（底 0xF8000）" >&2
    echo "       值栈 786 KB 从 .data 之后一直铺下去，必须整体留在" >&2
    echo "       0xF8000 以下。解决办法：减小档位调用深度、或把内核" >&2
    echo "       栈挪到 1MB 以上（改 boot.s 的 pm_entry）。" >&2
    exit 1
fi
echo "  值栈结束: 0x$(printf '%x' "$VSTACK_END")（内核栈底 0xF8000，无重叠）"

# 拼装：boot.bin (512) + kernel.bin + 填充到 1.44MB 软盘大小
#
# 为什么填充到 1.44MB：这样镜像可以直接当软盘插入 QEMU
# （-fda 参数）或真机（USB 模拟软盘）。不填充也能启动，
# 但某些 BIOS 会因为镜像太小而拒绝。
{
    cat "$OUT/boot.bin"
    cat "$OUT/kernel.bin"
} > "$OUT/xos.img"

echo ""
echo "ok -> $OUT/xos.img"
echo "     boot sector : 512 字节"
echo "     kernel      : $KSIZE 字节"
echo "     总计        : $(wc -c < "$OUT/xos.img" | tr -d ' ') 字节"
echo ""
echo "运行方式见 docs/运行说明.md"