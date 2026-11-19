#!/bin/sh
# X 语言 P0 编译器构建脚本（Windows / Git Bash）
#
# 为什么需要这个脚本而不是一行 gcc —— 三个 MinGW 在中文 Windows 上的坑：
#
#   1. 工具链装在 C:/xgcc，不在系统 PATH 里，避免污染用户环境。
#   2. MinGW 的 gcc 驱动 fork cc1.exe 时会丢掉 PATH，
#      所以 DLL 必须物理复制到 cc1.exe 同目录（已处理，见 tools/setup_toolchain.md）。
#   3. 用户名含中文时：
#      a) 汇编器无法在 %TEMP% 下建 .o 文件 → TMP 必须指到 ASCII 路径
#      b) 链接器无法写含中文的输出路径 → 先输出到 ASCII 路径再复制回来
#
# 用法:
#   ./tools/build.sh          构建
#   ./tools/build.sh clean    清理后重建
#   ./tools/build.sh debug    带 ASan 的调试构建（需要支持 ASan 的 MinGW）

set -e

XGCC=/c/xgcc/mingw64
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src"
OUT="$ROOT/build"

# 坑 3a：临时目录必须纯 ASCII
export TMP="C:/xtmp"
export TEMP="C:/xtmp"
mkdir -p "$TMP"
mkdir -p "$OUT"

# 坑 3b：链接输出走 ASCII 路径
LINK_OUT="/c/xtmp/x.exe"

if [ "$1" = "clean" ]; then
    rm -f "$LINK_OUT"
    echo "cleaned"
fi

CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
DEFS="-D_XOPEN_SOURCE=700"

# 窗口运行时要吃系统库。
#
# 只有 Windows 上才需要，所以按平台加 —— 直接无条件写 -lgdi32 -luser32
# 会让 Linux/macOS 上的构建直接失败（那两个平台上没有这些库）。
#
#   gdi32    —— GDI 绘制：字体、位图、BitBlt、GetDIBits、RoundRect
#   user32   —— 窗口与消息：CreateWindow、PeekMessage、鼠标
#   ws2_32   —— Winsock：socket/bind/listen/accept/recv/send
#   msimg32  —— GradientFill（v0.17 渐变）。只在 gui_win32.c 里用到，
#               但 gui_win32.c 无条件参与链接，所以这里也必须加。
#
# 这三项是 Win32 的最小集合，不加的话链接期会报一堆
# `undefined reference to __imp_CreateCompatibleDC` 之类。
#
# ── ws2_32 为什么要在这里加，而不是靠源码里的 #pragma comment ──
#
# MSVC 认 `#pragma comment(lib, "ws2_32.lib")`，MinGW 不认（它会
# 静默忽略），于是 TCP 那批函数全部变成 undefined reference。
# 「在 MSVC 上能编过、在 MinGW 上编不过」是最费解的一类问题，
# 因为源码里明明写着要这个库。所以链接选项统一放这里。
#
# 类 Unix 上 socket 在 libc 里，不需要额外库。
SYSLIBS=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) SYSLIBS="-lgdi32 -luser32 -lws2_32 -lmsimg32 -lcomdlg32" ;;
esac

if [ "$1" = "debug" ]; then
    # 调试构建不带 -O2：优化会把变量下沉、把调用内联，栈回溯里的
    # 帧对不上源码。代价是慢几倍，但找 bug 时值得。
    CFLAGS="-std=c11 -O0 -g3 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
    DEFS="$DEFS -DX_DEBUG=1"
fi

# ── 重新生成窗口运行时的内嵌副本 ────────────────────────────
#
# src/gui/gui_embed.h 是 tools/gen_gui_embed.py 从 gui.h + gui_win32.c
# 生成的。它必须每次构建都重跑，理由：
#
#   AOT 产物（--emit=exe）用的是**内嵌副本**里的 gui.h 声明，而不是
#   源码树的 src/gui/gui.h。改了 gui.h 加新函数却忘了重跑这一步，
#   症状是「解释档能跑、AOT 编不过」，报的还是
#       implicit declaration of function 'xgui_fill_round'
#   —— 一个看起来像「忘了 include」的错误，而实际原因是内嵌副本过期。
#
# 这个提示曾经真的发生过（v0.17 加 12 个形状原语时），所以把它从
# 「手动记得跑」改成构建的一部分。python 不在时跳过并明确告知：
# 静默跳过会让上面的问题重新出现。
if command -v python >/dev/null 2>&1; then
    python "$ROOT/tools/gen_gui_embed.py" >/dev/null
elif command -v python3 >/dev/null 2>&1; then
    python3 "$ROOT/tools/gen_gui_embed.py" >/dev/null
else
    echo "warn: 没找到 python，跳过 gui_embed.h 重新生成" >&2
    echo "      改了 src/gui/gui.h 的话，AOT 产物会用到过期的内嵌副本" >&2
fi

# ── 重新生成热循环展开体 ────────────────────────────────────
#
# src/loop_jit_gen.h 是 tools/gen_loop_jit.py 生成的 lf_run2() 函数体：
# 2880 份「具名变量、零分派」的循环展开，是 10 亿次循环从 1909ms 降到
# 677ms 的全部原因（原理见该脚本顶部）。
#
# 它必须每次构建都重跑：脚本里的 KINDS 顺序**必须**与 interp.c 的
# LfOpKind 头三个枚举值（ADD=0/SUB=1/MUL=2）一致。有人调整枚举顺序
# 却忘了重跑，症状是「算出来的结果是错的」—— 而且只在热循环上错，
# 冷路径（纯解释）完全正常，极难定位。
if command -v python >/dev/null 2>&1; then
    python "$ROOT/tools/gen_loop_jit.py"
elif command -v python3 >/dev/null 2>&1; then
    python3 "$ROOT/tools/gen_loop_jit.py"
else
    echo "warn: 没找到 python，跳过 loop_jit_gen.h 重新生成" >&2
    echo "      用现有的 src/loop_jit_gen.h（若与 interp.c 的枚举不同步会算错）" >&2
fi

echo "building x.exe ..."
"$XGCC/bin/gcc.exe" $CFLAGS $DEFS \
    "$SRC/platform.c" \
    "$SRC/crash.c" \
    "$SRC/trace.c" \
    "$SRC/target.c" \
    "$SRC/profile.c" \
    "$SRC/pkg.c" \
    "$SRC/semver.c" \
    "$SRC/registry.c" \
    "$SRC/lockfile.c" \
    "$SRC/json_read.c" \
    "$SRC/lsp.c" \
    "$SRC/utf8.c" \
    "$SRC/util.c" \
    "$SRC/lexer.c" \
    "$SRC/parser.c" \
    "$SRC/bytecode.c" \
    "$SRC/xbc.c" \
    "$SRC/compile.c" \
    "$SRC/interp.c" \
    "$SRC/gui/gui_win32.c" \
    "$SRC/codegen_c.c" \
    "$SRC/main.c" \
    $SYSLIBS \
    -o "$LINK_OUT"

# 复制回项目目录（此时 exe 已生成完毕，cp 不经过链接器，中文路径没问题）
#
# ── 为什么失败要给出「谁占着它」而不是只报 cp 错误 ──────────
#
# 直接 cp 会以 `cp: cannot create regular file ...: Device or resource
# busy` 失败。这条消息只说「忙」，不说「谁在忙」，而真实原因通常
# 是编辑器把 `x.exe lsp` 长期跑着（VS Code 的 X 扩展会这么做）。
#
# 那种情况下最自然的反应是「再试一次」—— 而它会一直失败，因为
# 进程不会自己退出。所以这里显式翻译一次：cp 失败时把占用的
# 进程列出来，并给出结束命令。诊断的目标是让人**下一步就知道
# 该做什么**，而不是知道「出错了」。
if ! cp "$LINK_OUT" "$OUT/x.exe" 2>/dev/null; then
    # 回退：mv 走的是 MoveFile，语义与 cp 不同 —— 它不需要在目标
    # 位置新建文件再写，而是把目录项指过去。在 Windows 上，一个
    # 「被映射但当前没在执行」的 exe 能被 mv 覆盖，cp 不能。
    # LSP 进程刚刚退出、编辑器还没重启它，就是这种窗口期。
    if mv -f "$LINK_OUT" "$OUT/x.exe" 2>/dev/null; then
        echo "ok -> build/x.exe（经 mv 替换旧文件）"
        exit 0
    fi
    echo "error: 无法写入 build/x.exe —— 文件被占用" >&2
    echo "       最常见的原因：编辑器把 \`x.exe lsp\` 作为语言服务跑着。" >&2
    echo "       Windows 会锁住正在执行的 exe，所以链接产物换不上去。" >&2
    echo "" >&2
    echo "       解决办法（任选其一）：" >&2
    echo "         1. 关掉编辑器里的 X 语言扩展，或重启编辑器" >&2
    echo "         2. 手工结束它：" >&2
    echo "            powershell -c \"Get-Process x -EA 0 | Where-Object {\$_.CommandLine -like '*lsp*'} | Stop-Process -Force\"" >&2
    echo "" >&2
    echo "       注：这只影响「覆盖旧 x.exe」，新产物已经生成在：" >&2
    echo "           $LINK_OUT" >&2
    exit 1
fi

echo "ok -> build/x.exe"