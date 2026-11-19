#!/bin/sh
# X 语言基准测试 —— 给出「X 解释器 vs C」的可复现数字
#
# ── 为什么需要这个脚本而不是随手 time 一下 ──────────────────────
#
#   1. 绝对时间在 Windows 上不可复现。这台机器常驻 ToDesk / aTrust /
#      TQClient / 杀软，CPU 抖动能到 1.8 倍。同一个二进制连续三轮测出
#      596ms / 1076ms / 1129ms。
#      → 对策：所有候选**交替测量**（见 bench_timer.py），各取最小值。
#        最小值是「系统最干净那一刻」的成本，最接近硬件真实能力；
#        交替则保证 A 和 B 面对的是同一段系统状态，比值才有意义。
#
#   2. 进程启动有固定开销（约 50ms：控制台初始化、UTF-8 切换、
#      内存池）。不扣掉它会把启动算进循环，得出完全错误的倍数。
#      → 对策：额外测「空程序」作为基线，全部数字先减基线。
#
#   3. 规模必须远超启动开销。原本用 fib(25)，C 侧只要 1.4ms，
#      减完基线得到 0.1ms，算出的倍数纯是噪声。
#      → 对策：循环 3000 万次、fib(33)，两侧执行时间都在 60ms 以上。
#
#   4. C 对照不能用字面量。`fib(33)` 会被 gcc -O2 在**编译期**求值，
#      运行时只花 0.1ms —— 那不是在比较执行速度。
#      → 对策：n 从 argv 读，编译器无法预知。
#
#   5. 解释器要先预编译成 .xbc。否则计的是「编译 + 执行」，
#      而编译期开销在不同规模下占比不同，会污染倍数。
#
# ── 用法 ────────────────────────────────────────────────────
#   ./tools/bench.sh              跑全部基准（11 轮）
#   ./tools/bench.sh --quick      5 轮，快速验证
#   ./tools/bench.sh --nosuper    关掉超指令，对比合并收益
#   ./tools/bench.sh --profile    额外打印指令分派直方图
#
# ── 环境变量 ────────────────────────────────────────────────
#   X_NO_SUPER=1   关掉超指令
#   X_PROFILE=1    打印指令分派统计

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
XGCC=/c/xgcc/mingw64
TMP_DIR="/c/xtmp/xbench"
TIMER="$ROOT/tools/bench_timer.py"
mkdir -p "$TMP_DIR"

export TMP="C:/xtmp"
export TEMP="C:/xtmp"

ROUNDS=11
# 用 for 遍历全部参数而不是 case "$1" ——
# 后者会让 `--quick --nosuper` 里的第二个开关被静默忽略，
# 结果就是「以为在测关闭超指令，其实还是在测开启的」，
# 而且数字看起来完全正常，极难发现。
for arg in "$@"; do
    case "$arg" in
        --quick)   ROUNDS=5 ;;
        --nosuper) export X_NO_SUPER=1 ;;
        --profile) export X_PROFILE=1 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

N=30000000
FIB_N=33

# ---------------------------------------------------------------
# 生成基准源码
# ---------------------------------------------------------------
cat > "$TMP_DIR/loop.xle" <<EOF
fn main() {
    var sum = 1
    var i = 0
    while i < $N {
        sum = (sum + i) % 1000003
        i = i + 1
    }
    println(sum)
}
EOF

cat > "$TMP_DIR/empty.xle" <<'EOF'
fn main() {
    println(1)
}
EOF

cat > "$TMP_DIR/call.xle" <<EOF
fn fib(n: Int) -> Int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

fn main() {
    println(fib($FIB_N))
}
EOF

# C 对照：与 X 版本逐行对应的同一份算法
cat > "$TMP_DIR/loop.c" <<EOF
#include <stdio.h>
#include <stdint.h>
volatile int64_t sink;
int main(void) {
    int64_t sum = 1;
    for (int64_t i = 0; i < $N; i++) sum = (sum + i) % 1000003;
    sink = sum;
    printf("%lld\n", (long long)sum);
    return 0;
}
EOF

cat > "$TMP_DIR/fib.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
volatile int64_t sink;
static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
int main(int argc, char** argv) {
    int64_t n = (argc > 1) ? strtoll(argv[1], 0, 10) : 33;
    sink = fib(n);
    printf("%lld\n", (long long)sink);
    return 0;
}
EOF

cat > "$TMP_DIR/noop.c" <<'EOF'
int main(void) { return 0; }
EOF

echo "编译 C 对照 (-O0 / -O2) ..."
for f in loop fib noop; do
    "$XGCC/bin/gcc.exe" -O0 -o "$TMP_DIR/${f}_O0.exe" "$TMP_DIR/$f.c"
    "$XGCC/bin/gcc.exe" -O2 -o "$TMP_DIR/${f}_O2.exe" "$TMP_DIR/$f.c"
done

# 预编译字节码：把编译排除在计时之外
for f in loop call empty; do
    "$X" build "$TMP_DIR/$f.xle" -o "$TMP_DIR/$f.xbc" >/dev/null 2>&1
done

# ---------------------------------------------------------------
# 交替测量
# ---------------------------------------------------------------
echo "测量中（$ROUNDS 轮，交替测量）..."

LOOP_OUT=$(python "$TIMER" "$ROUNDS" \
    "X::$X|run|$TMP_DIR/loop.xbc" \
    "C0::$TMP_DIR/loop_O0.exe" \
    "C2::$TMP_DIR/loop_O2.exe")

FIB_OUT=$(python "$TIMER" "$ROUNDS" \
    "X::$X|run|$TMP_DIR/call.xbc" \
    "C0::$TMP_DIR/fib_O0.exe|$FIB_N" \
    "C2::$TMP_DIR/fib_O2.exe|$FIB_N")

BASE_OUT=$(python "$TIMER" "$ROUNDS" \
    "X::$X|run|$TMP_DIR/empty.xbc" \
    "C::$TMP_DIR/noop_O0.exe")

get() { echo "$1" | awk -v k="$2" '$1==k{print $2}'; }

XL_RAW=$(get "$LOOP_OUT" X);  CL0_RAW=$(get "$LOOP_OUT" C0); CL2_RAW=$(get "$LOOP_OUT" C2)
XF_RAW=$(get "$FIB_OUT"  X);  CF0_RAW=$(get "$FIB_OUT"  C0); CF2_RAW=$(get "$FIB_OUT"  C2)
XB=$(get "$BASE_OUT" X);      CB=$(get "$BASE_OUT" C)

# 扣掉进程启动基线。
#
# ── 为什么这个函数必须报出原始值 ────────────────────────────
#
# 第一版是 `v=a-b; if(v<0.1)v=0.1`。看起来无害，实际会**把真实的
# 负数差值伪装成 0.1ms**，于是表格里出现「fib(33) 的 C 版本只要
# 0.1ms」这种离谱数字，比值算成 1654×。
#
# 成因：rob 的耗时和空程序基线是两次独立测量，各自受噪声影响。
# 空程序在 Windows 上光是加载 CRT 就要 45~60ms，当它某一轮读高、
# 而 fib 某一轮读低时，差值就是负的。这时 0.1 不再是「很小」，
# 而是「测量失效」的信号 —— 而它被静默吞掉了。
#
# 所以分两种情况：
#   · 差值 >= 5ms        → 正常，用差值（扣除基线的本意）
#   · 差值 < 5ms 或为负  → 打 [!] 标记并保留原始值，让人看得见
#                          这里出了问题，而不是拿一个漂亮但假的数
#                          字去算比值。
sub() {
    awk -v a="$1" -v b="$2" -v tag="${3:-}" 'BEGIN{
        v = a - b
        if (v < 5) {
            printf "%s[!]%.1f", (tag=="" ? "" : tag), a
        } else {
            printf "%.1f", v
        }
    }'
}
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", a/b}'; }
# 比值只在两侧数字都可信时才有意义。带 [!] 的格子说明扣除基线失败，
# 这时算出来的倍数纯是噪声放大 —— 宁可显示 n/a，也不给一个假数字。
ratio_strict() {
    case "$1$2" in
        *'[!]'*) printf "n/a" ;;
        *) awk -v a="$1" -v b="$2" 'BEGIN{ if (b<=0) printf "n/a"; else printf "%.1f", a/b }' ;;
    esac
}

XL=$(sub "$XL_RAW" "$XB");  CL0=$(sub "$CL0_RAW" "$CB"); CL2=$(sub "$CL2_RAW" "$CB")
XF=$(sub "$XF_RAW" "$XB");  CF0=$(sub "$CF0_RAW" "$CB"); CF2=$(sub "$CF2_RAW" "$CB")

echo ""
echo "X 语言基准（$ROUNDS 轮交替测量取最小值，已扣进程启动基线）"
echo ""
printf "  %-24s %9s %9s %9s %9s\n" "基准" "X" "C -O0" "C -O2" "X/C-O0"
echo "  ──────────────────────────────────────────────────────────────────"
printf "  %-24s %7s ms %7s ms %7s ms %8s×\n" "整数循环 3000 万次" \
    "$XL" "$CL0" "$CL2" "$(ratio_strict "$XL" "$CL0")"
printf "  %-24s %7s ms %7s ms %7s ms %8s×\n" "fib(33) 700 万次调用" \
    "$XF" "$CF0" "$CF2" "$(ratio_strict "$XF" "$CF0")"
echo ""
echo "  基线：X 空程序 ${XB}ms / C 空程序 ${CB}ms"
echo "  原始值：循环 X=${XL_RAW} C0=${CL0_RAW} C2=${CL2_RAW}"
echo "          fib  X=${XF_RAW} C0=${CF0_RAW} C2=${CF2_RAW}"
if echo "$XL$CL0$CL2$XF$CF0$CF2" | grep -q '\[!\]'; then
    echo ""
    echo "  [!] 有格子带 [!]：该值的原始耗时与空程序基线差不到 5ms，"
    echo "      扣除基线后不可信（噪声已经把真实差淹没了）。"
    echo "      此时看「原始值」那一行，别用倍数做判断。"
fi
if [ -n "$X_NO_SUPER" ]; then
    echo "  * 本轮数据为【关闭超指令】的结果，用于对比合并收益"
fi
echo ""

if [ -n "$X_PROFILE" ]; then
    echo "--- 指令分派统计（loop 基准）---"
    X_PROFILE=1 "$X" run "$TMP_DIR/loop.xbc" 2>&1 >/dev/null | head -25
    echo ""
fi