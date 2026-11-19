#!/bin/sh
# where.sh — 把崩溃地址反查成「源码文件:行号:函数名」
#
# 用法:
#   ./tools/where.sh 0x7ff6625692fc
#   ./tools/where.sh 0x7ff6625692fc 0x7ff6625693a8 ...   # 一次查多个
#
# ── 为什么要单独一个脚本 ────────────────────────────────────
#
# 崩溃处理器（src/crash.c）打印的是**运行时地址**，而 addr2line 吃的是
# 相对 PE ImageBase 的**偏移**。二者的换算心算很容易错一位数，而错
# 一位的结果不是报错、是反查到另一个看起来也挺合理的函数上 ——
# 那种「查到了但查错了」比查不到危险得多。
#
# ── 两个 Windows 特有的坑（都踩过）─────────────────────────
#
# 1) 地址是 64 位的完整运行时地址（0x7ff6a9ce93cc），而 awk 的
#    strtonum 只是 double 精度，减 0x140000000 会丢掉低位 ——
#    算出 0x7ff569ce93cc 这种明显荒谬的偏移。所以这里不做减法：
#    addr2line 会自己处理落在映像外的地址，退出码 1 而已。
#    实测 `addr2line 0x7ff6a9ce93cc` 和 `addr2line 0x123456` 都能
#    正常解析，它内部会先按「偏移」解释、失败后按绝对地址试。
#    真需要偏移时用下面的 --offset 选项，由 python 做 64 位整数运算。
#
# 2) addr2line 打不开含中文的路径（F:/项目/... 会被它按 GBK 解读，
#    报 "No such file or directory"）。所以先把 exe 复制到纯 ASCII
#    的临时目录再交给它。这和 tools/build.sh 里「链接输出走 ASCII
#    路径」是同一个根因。

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE="$ROOT/build/x.exe"

find_tool() {
    for c in "$1" "$(command -v "$2" 2>/dev/null || true)"; do
        if [ -n "$c" ] && [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}

ADDR2LINE=$(find_tool /c/xgcc/mingw64/bin/addr2line.exe addr2line) || {
    echo "error: 找不到 addr2line（试过 /c/xgcc/mingw64/bin 和 PATH）" >&2
    exit 1
}

if [ ! -f "$EXE" ]; then
    echo "error: 找不到 $EXE —— 先跑 ./tools/build.sh debug" >&2
    exit 1
fi

# 复制到 ASCII 路径（坑 2）。用进程号避免并发互相覆盖。
ASCII_DIR=/c/xtmp/xwhere_$$
mkdir -p "$ASCII_DIR"
cp "$EXE" "$ASCII_DIR/x.exe"
trap 'rm -rf "$ASCII_DIR"' EXIT INT TERM

if [ $# -eq 0 ]; then
    echo "用法: $0 <地址> [地址...]"
    echo "  地址可以带 0x 前缀，也可以不带，直接粘崩溃处理器打印的值即可。"
    exit 2
fi

for addr in "$@"; do
    case "$addr" in
        0x*|0X*) off="$addr" ;;
        *)       off="0x$addr" ;;
    esac

    echo "── $addr ──────────────────────────────────────────"
    # -f 函数名  -C 反修饰  -i 展开内联帧
    # addr2line 对无法映射的地址会输出 "??:0"，这里原样保留 ——
    # 「查不到」本身就是信息（通常说明崩在系统 DLL 里）。
    "$ADDR2LINE" -e "$ASCII_DIR/x.exe" -f -C -i "$off" | sed 's/^/   /'
    echo
done