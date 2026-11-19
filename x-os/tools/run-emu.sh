#!/bin/sh
# ============================================================
# run-emu.sh —— 在 PC 上跑 X 内核（宿主仿真档）
# ============================================================
#
# ── 与 build.sh 的关系 ────────────────────────────────────
#
# build.sh 产出**能引导的真镜像**（xos.img），给 QEMU/真机。
# 本脚本产出**能在 PC 上直接执行的 exe**，目的是开发期高频验证
# 内核逻辑（VGA 排版、滚动、串口格式化、shell 解析、自检）。
#
# 两者共用同一份 kernel/src/kmain.xle —— 这是关键：
# 同一份源码，两边都能编，语义由编译器保证一致（生成器只有
# 一套，差异只在 xe_* 访问器背后是硬件还是模拟硬件）。
#
# ── 为什么能做到「同一份源码两边编」─────────────────────
#
# 内建 memRead8 / portOut8 等在生成代码里统一走 xe_mr/xe_mw/
# xe_pi/xe_po，这三个函数的实现由宏决定：
#
#   X_FREESTANDING          → 真的解引用 / in、out 指令（内核）
#   X_EMU                   → 转到 xemu.c 的模拟硬件（PC 验证）
#   两个都没有              → 直接 rt_err（用错了 API）
#
# 所以本脚本编译时定义 X_EMU（**不定义** X_FREESTANDING ——
# 那样会走真硬件的 in/out 指令，在宿主进程里是非法操作）。
#
# ── 它验证什么 / 不验证什么 ──────────────────────────────
#
# 验证：字符落位、滚动、光标、串口字节序列、数字格式化、
#       自检逻辑、shell 命令解析与输出、引导参数读取。
# 不验证：中断、分页、GDT/特权级、时序、真实地址解码。
#
# 后者必须 QEMU 或真机。本脚本是**开发期工具**，不是验收替代品。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
XLANG=$(cd "$ROOT/.." && pwd)

OUT="/c/xtmp/xos-emu"
MINGW="/c/xgcc/mingw64/bin"

export TMP="C:/xtmp"
export TEMP="C:/xtmp"
mkdir -p "$TMP" "$OUT"

PATH="$MINGW:$PATH"
export PATH

# 宿主档：**不指定 -m32**。
#
# 一开始想用 -m32 保持与内核同字长，但本机 MinGW 只装了 64 位
# 运行时（libmsvcrt.a / libkernel32.a 都只有 64 位版），
# 带 -m32 链接时 ld 报一屏 "skipping incompatible ..."，最后
# collect2 失败。为一个开发期工具装整套 32 位交叉库不值得。
#
# 而且真的不需要：X 的 Int 是 int64_t，两种字长下都是 8 字节；
# 仿真层内部所有地址都用 uint32_t 承载，截断行为与内核一致。
# 唯一差异是指针宽度（宿主 8 字节 vs 内核 4 字节），而生成代码
# 里地址运算已经全部走 uint32_t 中转，不依赖指针宽度。
CFLAGS="-O1 -g -DX_EMU -Wall -Wextra -Wno-unused-function \
        -Wno-unused-variable -Wno-unused-label"

echo "[1/4] X 源码 → C（宿主仿真档）"
"$XLANG/build/x.exe" build "$ROOT/kernel/src/kmain.xle" \
    --emit=c -o "$OUT/kmain_emu.c"

# ── 生成的 C 里 main 与内核的 main 撞名 ──────────────────
#
# 生成代码里的入口叫 main（宿主档）。而驱动 run-emu.c 也有
# main。做法：把生成的 main 改名为 xk_main，驱动里声明
# extern int xk_main(void) 再调用它。
#
# 三处必须一起改（漏一处就编译不过，而且报错位置互不相关）：
#   1. 函数头   int main(int argc, char** argv) {
#   2. 形参使用 (void)argc; (void)argv;
#   3. 尾部的说明注释若提到 main 可不管（注释不参与编译）
#
# 不能用 -Dmain=xk_main：那会把所有标识符 main 都改掉，包括
# 字符串字面量里的 "main"（错误消息、帮助文本都会变），
# 于是「编译能过但输出错」。sed 精确改这两行更安全。
sed -i 's/^int main(int argc, char\*\* argv) {/int xk_main(void) {/' "$OUT/kmain_emu.c"
sed -i 's/^    (void)argc; (void)argv;$/    \/* 形参已去掉：宿主仿真档由 run-emu.c 驱动 *\//' "$OUT/kmain_emu.c"
grep -q "int xk_main(void)" "$OUT/kmain_emu.c" || {
    echo "error: 没能把生成的 main 改名为 xk_main" >&2
    grep -n "int main(" "$OUT/kmain_emu.c" | head >&2
    exit 1
}
if grep -q '(void)argc' "$OUT/kmain_emu.c"; then
    echo "error: 生成的 main 里还有 argc/argv 引用，改名后编不过" >&2
    grep -n 'argc\|argv' "$OUT/kmain_emu.c" | head >&2
    exit 1
fi

echo "[2/4] C → 目标文件"
# shellcheck disable=SC2086
gcc $CFLAGS -c "$OUT/kmain_emu.c" -o "$OUT/kmain_emu.o"
gcc $CFLAGS -c "$ROOT/tools/xemu.c"  -o "$OUT/xemu.o"
gcc $CFLAGS -c "$ROOT/tools/run-emu.c" -I"$ROOT/tools" -o "$OUT/run-emu.o"

echo "[3/4] 链接"
gcc -o "$OUT/xos-emu.exe" \
    "$OUT/run-emu.o" "$OUT/kmain_emu.o" "$OUT/xemu.o"

echo "[4/4] 运行"
echo ""
"$OUT/xos-emu.exe" "$@"