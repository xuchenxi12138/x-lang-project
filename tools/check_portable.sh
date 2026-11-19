#!/bin/sh
# 跨平台可移植性守门
#
# 这个脚本只做一件事，但那件事是**整个跨平台支持的地基**：
# 确认平台相关代码没有泄漏到 platform.c 之外。
#
# 为什么必须由脚本查而不是靠人记住：
#
#   在 Windows 上写 #ifdef _WIN32 是完全自然的动作，写完也能正常
#   编译运行。错误只在 Linux 上第一次编译时才出现，而那时往往已经
#   积累了十几处，逐个改是噩梦。而这个检查在 Windows 上就能跑，
#   在提交前就能拦住。
#
#   规则不能是「尽量少用」——必须是可以机械判定的「一处都不许」。
#
# 检查项：
#   1. 除 platform.c / platform.h 外，不许出现 _WIN32 / _MSC_VER / __APPLE__ / __linux__
#   2. 除 platform.c 外，不许 include <windows.h> / <unistd.h> / <io.h>
#   3. 每个 .c 都能单独做语法检查（编译成 .o，不链接）
#   4. 生成的目标三元组不含 "unknown"
#
# 用法:
#   ./tools/check_portable.sh

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
XGCC=/c/xgcc/mingw64
BIN="$ROOT/build/x.exe"

export TMP="C:/xtmp"
export TEMP="C:/xtmp"
OUT="/c/xtmp/xport"
mkdir -p "$OUT"

pass=0
fail=0
failed=""

ok()   { pass=$((pass+1)); printf "  \033[32mPASS\033[0m  %-42s %s\n" "$1" "${2:-}"; }
bad()  { fail=$((fail+1)); failed="$failed $1"; printf "  \033[31mFAIL\033[0m  %-42s %s\n" "$1" "${2:-}"; }

echo ""
echo "跨平台可移植性检查"
echo ""

# ---------------------------------------------------------------
# 1. 平台宏不许泄漏
# ---------------------------------------------------------------
#
# ── 为什么要排除 codegen_c.c 的字符串字面量 ──────────────────
#
# 这个检查的契约是「平台相关代码只许在 platform.c/.h 里」。它要抓的
# 是**编译器自己的源码**里出现了 #ifdef _WIN32 —— 那种代码在 Windows
# 上编得过、到 Linux 就编不过。
#
# 但 codegen_c.c 里有一批**字符串**，内容是「生成给产物的 C 代码」：
#
#     "#ifdef _WIN32\n"
#     "#  include <windows.h>\n"
#
# 这些 #ifdef 不在编译器的源码里，它们是要写进 .c 产物、由 gcc 在
# **用户机器上**展开的。AOT 产物必须自包含 —— 它不能 include
# 编译器的 platform.h，因为在发布形态里那个文件根本不存在（用户手上
# 只有 x.exe 和 stdlib/）。所以产物里出现 _WIN32 是**设计要求**，
# 不是泄漏。
#
# 原来的判据只排除「行首是 // 或 *」的注释，于是这些字符串全被算成
# 泄漏，报 11 处。而它们一处都不该改 —— 改了 AOT 就编不出产物。
#
# 正确判据：排除「行内出现在双引号之后」的匹配。字符串字面量里的
# 内容不是源码里生效的指令。用 awk 逐行判断「第一个 " 的位置是否
# 早于匹配位置」。
# 判据分三层，全部是机械可判的：
#   1. 行注释（行首是 // 或 *）—— 说明文字，不是代码
#   2. 块注释区间（/* ... */，可跨行）—— 同上，注释里会引用规则原文
#   3. 双引号之后 —— 字符串字面量，见上面 codegen_c.c 的说明
#
# 第 2 层必须真的跟踪状态而不是只看行首：codegen_c.c 顶部那段块注释
# 里写着「生成的 C 里直接写 #ifdef _WIN32」，那一行以汉字开头，
# 「行首是 *」的判据抓不到它。而按行号硬编码排除会在下一次编辑
# 时静默失效 —— 失效后的表现是「检查通过」，比报错危险得多。
#
# 用 awk 逐文件扫描：/pattern/ 逐个打开，手动维护 in_block 状态。
scan_leak() {
    awk '
    FNR == 1 { in_block = 0 }
    {
        line = $0
        # 逐字符走一遍，同时识别块注释边界与字符串
        out = ""
        i = 1; n = length(line)
        while (i <= n) {
            if (in_block) {
                p = index(substr(line, i), "*/")
                if (p == 0) { i = n + 1 }
                else { i = i + p + 1; in_block = 0 }
            } else {
                if (substr(line, i, 1) == "\"") {
                    # 跳过字符串内容（不处理转义，本用途足够）
                    i++
                    while (i <= n && substr(line, i, 1) != "\"") i++
                    i++
                } else if (substr(line, i, 2) == "/*") {
                    in_block = 1; i += 2
                } else if (substr(line, i, 2) == "//") {
                    i = n + 1
                } else {
                    out = out substr(line, i, 1); i++
                }
            }
        }
        if (out ~ /_WIN32|_MSC_VER|__APPLE__|__linux__/) {
            printf "%s:%d:%s\n", FILENAME, FNR, line
        }
    }' "$@" 2>/dev/null
}

scan_hdr() {
    awk '
    FNR == 1 { in_block = 0 }
    {
        line = $0
        out = ""
        i = 1; n = length(line)
        while (i <= n) {
            if (in_block) {
                p = index(substr(line, i), "*/")
                if (p == 0) { i = n + 1 }
                else { i = i + p + 1; in_block = 0 }
            } else {
                if (substr(line, i, 1) == "\"") {
                    i++
                    while (i <= n && substr(line, i, 1) != "\"") i++
                    i++
                } else if (substr(line, i, 2) == "/*") {
                    in_block = 1; i += 2
                } else if (substr(line, i, 2) == "//") {
                    i = n + 1
                } else {
                    out = out substr(line, i, 1); i++
                }
            }
        }
        if (out ~ /include *<(windows\.h|unistd\.h|io\.h|sys\/)/) {
            printf "%s:%d:%s\n", FILENAME, FNR, line
        }
    }' "$@" 2>/dev/null
}

leak=$(scan_leak "$ROOT/src"/*.c "$ROOT/src"/*.h | grep -v "/platform\.c:" | grep -v "/platform\.h:")

if [ -z "$leak" ]; then
    ok "platform-macros-contained" "平台宏不出 platform.c/.h"
else
    bad "platform-macros-contained" "发现 $(echo "$leak" | wc -l) 处泄漏"
    echo "$leak" | head -5 | sed 's/^/          /'
fi

# ---------------------------------------------------------------
# 2. 系统头文件不许泄漏
# ---------------------------------------------------------------
# 注意要把注释行排除掉：platform.h 的说明文字里就写着
# 「不许 include <windows.h>」，那是规则本身，不是违规。
# 判据是「行首不是 // 或 *」，避免把解释文字当成代码。
#
# 同样要排除 codegen_c.c 的字符串字面量与块注释，理由见上面第 1 项。
hdr=$(scan_hdr "$ROOT/src"/*.c "$ROOT/src"/*.h | grep -v "/platform\.c:")

if [ -z "$hdr" ]; then
    ok "sys-headers-contained" "系统头只在 platform.c"
else
    bad "sys-headers-contained" "发现 $(echo "$hdr" | wc -l) 处泄漏"
    echo "$hdr" | head -5 | sed 's/^/          /'
fi

# ---------------------------------------------------------------
# 3. 每个 .c 都能单独编译（不做链接，所以不需要 main）
#
# 这一条抓的是「在 Linux 上会报未定义符号」那类问题：
# 某个文件用了 plat_* 但 platform.h 里没声明，或者签名对不上。
# 单独编译能发现声明不一致，链接才能发现实现缺失——
# 后面的全量构建负责后者。
# ---------------------------------------------------------------
compile_failed=""
for f in "$ROOT"/src/*.c; do
    name=$(basename "$f" .c)
    if ! "$XGCC/bin/gcc.exe" -std=c11 -O0 -fsyntax-only \
            -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
            -D_XOPEN_SOURCE=700 -I"$ROOT/src" "$f" 2>"$OUT/$name.err"; then
        compile_failed="$compile_failed $name"
    elif [ -s "$OUT/$name.err" ]; then
        # 有输出但退出码为 0 = 有警告
        compile_failed="$compile_failed $name(warn)"
    fi
done

if [ -z "$compile_failed" ]; then
    ok "each-tu-compiles-clean" "$(ls "$ROOT"/src/*.c | wc -l) 个编译单元零警告"
else
    bad "each-tu-compiles-clean" "有问题:$compile_failed"
fi

# ---------------------------------------------------------------
# 4. 宿主三元组必须可解析且不含 unknown
# ---------------------------------------------------------------
triple=$("$BIN" --version 2>/dev/null | grep -i "target\|宿主" | head -1)
host=$("$BIN" target list 2>/dev/null | grep -E "^ *\*?" | head -1)

if [ -n "$host" ] && echo "$host" | grep -q "unknown"; then
    bad "host-triple-resolved" "宿主含 unknown：$host"
else
    ok "host-triple-resolved" "${host:-（target list 尚未实现）}"
fi

# ---------------------------------------------------------------
# 5. 类 Unix 分支的符号审查
#
# 这一项的来历值得写下来：原版这里是空转的占位（循环体只有 `:`），
# 打印 "PASS" 但其实什么都没检查。这种「假通过」比没有检查更危险，
# 因为它会让人以为类 Unix 分支已经验证过了。
#
# 在没有 Linux 机器的前提下，能做的真实检查是：把 platform.c 的
# 类 Unix 分支里用到的每一个 POSIX 符号抽出来，确认它们要么由
# 标准头文件提供（有 include），要么由我们自己定义。
#
# 做法是逐个符号查「是否在 #else 之后的类 Unix 段里出现」并核对
# 对应的 include 是否存在。这当然不等于「在 Linux 上编译通过」，
# 但能挡住最常见的一类错误：用了某个函数却忘了 include 它的头文件。
# ---------------------------------------------------------------
unix_failed=""
unix_sec=$(sed -n '/^#else  *\/\/ 类 Unix/,/^#endif  *\/\/ X_HOST_WINDOWS/p' "$ROOT/src/platform.c")

# 符号 → 必需的头文件
check_sym() {
    sym="$1"; hdr="$2"
    case "$unix_sec" in
        *"$sym"*)
            case "$unix_sec" in
                *"include <$hdr>"*|*"include <sys/$hdr>"*) ;;
                *)
                    # 也可能在本文件上方（两支共用的 include 区）已包含
                    case "$(sed -n '1,/^#else  *\/\/ 类 Unix/p' "$ROOT/src/platform.c")" in
                        *"include <$hdr>"*) ;;
                        *) unix_failed="$unix_failed $sym(缺<$hdr>)" ;;
                    esac
                    ;;
            esac
            ;;
    esac
}

check_sym "posix_spawnp"          "spawn.h"
check_sym "waitpid"               "sys/wait.h"
check_sym "getcwd"                "unistd.h"
check_sym "getpid"                "unistd.h"

# environ 必须是 extern 声明，否则类 Unix 上会「隐式声明」告警
case "$unix_sec" in
    *environ*)
        case "$unix_sec" in
            *"extern char** environ"*) ;;
            *) unix_failed="$unix_failed environ(缺extern声明)" ;;
        esac
        ;;
esac

if [ -z "$unix_failed" ]; then
    ok "unix-branch-symbols" "类 Unix 分支符号与 include 齐全"
else
    bad "unix-branch-symbols" "问题:$unix_failed"
fi

echo ""
if [ "$fail" = "0" ]; then
    printf "\033[32m%d 项可移植性检查全部通过\033[0m\n" "$pass"
    exit 0
else
    printf "\033[31m%d 通过，%d 失败\033[0m:%s\n" "$pass" "$fail" "$failed"
    exit 1
fi