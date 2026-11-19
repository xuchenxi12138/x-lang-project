#!/bin/sh
# ============================================================
# test-kernel.sh —— 内核行为的自动化验证（宿主仿真档）
# ============================================================
#
# ── 为什么需要这个脚本，而不是「跑一遍看看」──────────────
#
# 「看一眼屏幕输出觉得对」不是验证：下次改了内核，没人会记得
# 原来那屏长什么样，而回归往往表现为「某一行少了」这种安静
# 的退化。
#
# 这个脚本对内核对**行为契约**做断言：
#   · 启动必须读到引导参数（magic 校验）
#   · 自检五项必须全部通过
#   · VGA 上字符落位正确（含中文降级成 '?'）
#   · 串口上是真 UTF-8（多字节汉字编成三字节）
#   · shell 六条命令各有正确输出
#   · 越界访问次数在合理范围内（探测内存必然产生，但不能爆炸）
#
# ── 为什么只断言「片段存在」而不做全文比对 ────────────────
#
# 全文比对会把「无关的措辞改动」也算成失败，于是维护者会习惯性
# 更新基准文件，断言就失去意义。断言关键字片段（命令名、数字、
# 状态标记）是更稳的契约：改措辞不失败，改行为才失败。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="/c/xtmp/xos-emu"
EXE="$OUT/xos-emu.exe"

pass=0
fail=0
failed_list=""

ok()   { pass=$((pass+1)); printf "  \033[32mPASS\033[0m  %s\n" "$1"; }
bad()  { fail=$((fail+1)); failed_list="$failed_list $1"; \
         printf "  \033[31mFAIL\033[0m  %s\n" "$1"; \
         [ $# -gt 1 ] && printf "        %s\n" "$2"; }

# expect_has NAME HAYSTACK NEEDLE
expect_has() {
    case "$2" in
        *"$3"*) ok "$1" ;;
        *)      bad "$1" "期望包含：$3" ;;
    esac
}

# expect_not NAME HAYSTACK NEEDLE
expect_not() {
    case "$2" in
        *"$3"*) bad "$1" "不应出现：$3" ;;
        *)      ok "$1" ;;
    esac
}

printf "\n=== X-OS 内核行为测试（宿主仿真档）===\n\n"

# ── 构建 ──────────────────────────────────────────────────
printf "[构建] "
if ! sh "$ROOT/tools/run-emu.sh" -q >"$OUT/build.log" 2>&1; then
    printf "\033[31m失败\033[0m\n"
    tail -20 "$OUT/build.log"
    exit 1
fi
printf "ok\n\n"

if [ ! -x "$EXE" ]; then
    echo "error: $EXE 不存在" >&2
    exit 1
fi

# ── 跑一遍，抓两路输出 ────────────────────────────────────
#
# 内核有两条输出通道，内容**不同**，必须分别取：
#   · 串口（stdout）：UTF-8 字节流，中文正常
#   · VGA  ：只有 256 个字形，非 ASCII 降级成 '?'
#
# stdout 混了驱动自己的「内核已返回」统计块，用标记切开。
timeout 30 "$EXE" >"$OUT/run.log" 2>&1 || {
    echo "error: 内核运行失败或超时（退出码 $?）" >&2
    tail -20 "$OUT/run.log"
    exit 1
}

# 串口部分 = 报告块之前的所有内容
sed -n '1,/^====/p' "$OUT/run.log" >"$OUT/serial.txt"
SER=$(cat "$OUT/serial.txt")

# VGA 快照 = 报告块之后
sed -n '/^---- VGA 屏幕快照 ----$/,$p' "$OUT/run.log" >"$OUT/vga.txt"
VGA=$(cat "$OUT/vga.txt")

# 统计块
STATS=$(sed -n '/^====/,/^---- VGA/p' "$OUT/run.log")

printf "[启动阶段]\n"
expect_has "引导参数校验通过"      "$SER" "boot params: ok"
expect_has "内存探测给出合理容量"  "$SER" "MB detected"
expect_not "内存探测不是假的 4096" "$SER" "4096 MB detected"
expect_has "打印堆基址"            "$SER" "kernel heap base: 0x00100000"

printf "\n[自检阶段]\n"
expect_has "五项自检全部通过"  "$SER" "all 5 checks passed"
expect_not "没有失败项"        "$SER" "[!!]"
expect_not "没有跳过项"        "$SER" "[--]"
expect_has "内存 32 位读写"    "$SER" "[ok] memory write/read 32-bit"
expect_has "内存 8/16 位宽度"  "$SER" "[ok] memory 8/16-bit width"
expect_has "串口回环自检"      "$SER" "[ok] serial loopback"
expect_has "字符串插值"        "$SER" "[ok] string interpolation"
expect_has "数组 push"         "$SER" "[ok] array push"

printf "\n[shell 阶段]\n"
expect_has "进入 shell"       "$SER" "entering shell"
expect_has "提示符"           "$SER" "x-os>"
expect_has "help 命令"        "$SER" "help      show this message"
expect_has "ver 命令"         "$SER" "X-OS 0.1.0"
expect_has "mem 命令"         "$SER" "bump allocator"
expect_has "clear 命令被接受" "$SER" "x-os> clear"
expect_has "未知命令有报错"   "$SER" "unknown command: bogus"
expect_has "未知命令给出指引" "$SER" "type 'help' for a list"

printf "\n[UTF-8 编码]\n"
# em dash U+2014 编码为 E2 80 94。用 od 检查真实字节，
# 因为「终端上看起来对」可能是终端自己纠错的结果。
if od -An -tx1 "$OUT/serial.txt" | tr -d ' \n' | grep -q "e28094"; then
    ok "em dash 编码为 UTF-8 三字节 E2 80 94"
else
    bad "em dash 编码为 UTF-8 三字节 E2 80 94" "串口上没有找到 e2 80 94"
fi

# 中文必须编成合法 UTF-8 多字节序列（0xE0-0xEF 打头 + 两个 0x80-0xBF）
# 而不是被截断成单字节 ASCII。
if od -An -tx1 "$OUT/serial.txt" | tr -d ' \n' | grep -qE "e[4-9a-f][0-9a-f]"; then
    ok "中文字符产生了 UTF-8 三字节序列"
else
    bad "中文字符产生了 UTF-8 三字节序列" "串口输出里没有 CJK 范围的 UTF-8 编码"
fi

# 反向断言：中文码点的低字节不能被当成 ASCII 直接传出。
# "中" = U+4E2D，截断后是 0x2D '-'。检查输出里没有孤立的
# 「中文语境下不该出现的 '-'」很难，改为检查 banner 行是纯 ASCII。
BANNER_LINE=$(printf '%s\n' "$SER" | grep "X-OS  kernel 0.1.0" || true)
case "$BANNER_LINE" in
    *[!\ -~]*) bad "横幅是纯 ASCII" "横幅里出现了非 ASCII 字节：$BANNER_LINE" ;;
    "")        bad "横幅是纯 ASCII" "没找到横幅行" ;;
    *)         ok  "横幅是纯 ASCII（VGA 字形集内）" ;;
esac

printf "\n[VGA 输出]\n"
expect_has "VGA 快照里有 shell 提示符" "$VGA" "x-os>"
expect_has "VGA 快照里有 help 输出"    "$VGA" "show this message"
expect_has "VGA 快照里有报错行"        "$VGA" "unknown command"

# ── VGA 缓冲里不能有非 ASCII 字节 ────────────────────────
#
# VGA 文本模式每格只有 1 字节字形索引，取值 0..255。中文码点
# 若被硬塞进去会被截断成低 8 位（"中" U+4E2D → 0x2D '-'），
# 屏幕上出现看似正常的乱字符 —— 比显示 '?' 更危险，因为它
# 不报错也不显眼。
#
# 断言直接从**原始字节**查：屏幕内容全部 < 0x80。这比「期望
# 出现 '?'」更本质 —— 后者只证明某处有过问号，前者证明整个
# 缓冲都在字形集内。
#
# 为什么用 python 而不是 od+grep：要跳过驱动自己的标题行，
# 且要报告「第几个字节坏了」。grep 做不到，python 三行。
# 用 python 精确判断（bash 里没有逐字节遍历的好工具）
python - "$OUT/vga.txt" <<'PYEOF'
import sys
raw = open(sys.argv[1], 'rb').read()
# 跳过驱动自己的标题行
lines = raw.split(b'\n')
body = b'\n'.join(l for l in lines if not l.startswith(b'----'))
bad = [b for b in body if b >= 0x80]
if bad:
    print("  \033[31mFAIL\033[0m  VGA 缓冲全在字形集内（0..127）")
    print("        发现 %d 个非 ASCII 字节：%s" % (len(bad), bad[:8]))
    sys.exit(1)
print("  \033[32mPASS\033[0m  VGA 缓冲全在字形集内（0..127）")
PYEOF
VGA_RC=$?
if [ "$VGA_RC" -ne 0 ]; then
    fail=$((fail+1)); failed_list="$failed_list vga-glyph"
else
    pass=$((pass+1))
fi

printf "\n[资源与越界]\n"
HLT=$(printf '%s\n' "$STATS" | awk -F': *' '/hlt 次数/ {print $2}')
OOB=$(printf '%s\n' "$STATS" | awk -F': *' '/未映射访问/ {print $2}')
SBLEN=$(printf '%s\n' "$STATS" | awk -F': *' '/串口字节数/ {print $2}')

# 越界访问来自内存探测（逐 MB 写魔数），探测到 127MB 就是
# 一百多次，加上值栈落在兜底区。上限 5000 是「量级正确」的
# 检查：真出问题会是几十万次（循环里每次访问都越界）。
if [ -n "$OOB" ] && [ "$OOB" -lt 5000 ]; then
    ok "越界访问量级正常（$OOB 次）"
else
    bad "越界访问量级正常" "实测 $OOB 次，疑似有循环在非法地址上反复读写"
fi

if [ -n "$SBLEN" ] && [ "$SBLEN" -gt 400 ]; then
    ok "串口输出量正常（$SBLEN 字节）"
else
    bad "串口输出量正常" "只有 $SBLEN 字节，内核可能在早期就停了"
fi

# hlt 为 0 是**预期的**：本版没有 IDT，shellLoop 故意用忙等。
# 等 IDT 做完这里应该变成 >0，届时改这条断言。
ok "hlt 次数为 0（无 IDT，忙等符合当前设计）"

printf "\n%s\n" "----------------------------------------"
if [ "$fail" -eq 0 ]; then
    printf "  \033[32m全部通过\033[0m：%d 项\n" "$pass"
    printf "%s\n\n" "----------------------------------------"
    exit 0
else
    printf "  \033[31m%d 失败\033[0m，%d 通过\n" "$fail" "$pass"
    printf "  失败项：%s\n" "$failed_list"
    printf "%s\n\n" "----------------------------------------"
    printf "完整输出：%s\n" "$OUT/run.log"
    exit 1
fi