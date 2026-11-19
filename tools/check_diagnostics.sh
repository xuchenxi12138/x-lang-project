#!/bin/sh
# 运行时诊断质量检查
#
# 06-异常与诊断.md 要求每个运行时错误都必须回答三件事：
#   1. 在哪（精确到列）
#   2. 为什么（通俗原因，不是内部术语）
#   3. 怎么办（可执行建议）
#
# 本脚本逐个构造错误场景，检查输出里是否有这三段。
# 不是单元测试（不比对具体文案），而是「诊断完整性」的守门人 ——
# 允许文案演进，不允许缺段。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
TMPD="C:/xtmp/xdiag"
mkdir -p "$TMPD"

pass=0
fail=0

# check_diag <名称> <期望的关键词用|分隔> <源码>
check_diag() {
    name="$1"
    keywords="$2"
    src="$3"

    printf '%s\n' "$src" > "$TMPD/$name.xle"

    out=$("$X" run "$TMPD/$name.xle" 2>&1)
    rc=$?

    ok=1
    # 必须非零退出
    if [ "$rc" = "0" ]; then ok=0; fi
    # 必须有位置行（含 :行:列）
    #
    # 字符类是 [ec] 不是 [ac]：扩展名是 .xle / .xlc，`xl` 之后
    # 只可能是 `e` 或 `c`。写成 [ac] 时永不匹配 `.xle:`，
    # 于是下面每条诊断都被判成「缺位置」——整整 10 项运行时诊断
    # 全红，而真正的输出其实是完整的。这个笔误在 Windows 上
    # 一直没被发现，因为……（它确实就是从来没通过）。
    if ! printf '%s' "$out" | grep -qE '[^ ]+\.xl[ec]:[0-9]+:[0-9]+'; then ok=0; fi
    # 必须有关键词（任一）
    found=0
    echo "$keywords" | tr '|' '\n' | while IFS= read -r kw; do
        [ -z "$kw" ] && continue
        printf '%s' "$out" | grep -q "$kw" && echo HIT > "$TMPD/.hit"
    done
    if [ -f "$TMPD/.hit" ]; then found=1; rm -f "$TMPD/.hit"; fi
    if [ "$found" = "0" ]; then ok=0; fi

    if [ "$ok" = "1" ]; then
        pass=$((pass+1))
        printf "  \033[32mPASS\033[0m  %-22s 退出码=%s  %s\n" "$name" "$rc" \
            "$(printf '%s' "$out" | grep -E '^  error:' | head -1 | sed 's/^  error: //')"
    else
        fail=$((fail+1))
        printf "  \033[31mFAIL\033[0m  %-22s 退出码=%s\n" "$name" "$rc"
        printf '%s\n' "$out" | sed 's/^/        /' | head -8
    fi
}

echo "运行时诊断完整性检查"
echo

check_diag "int-div-zero" "除以零|除数为 0" \
'let a = 1
let b = 0
println(a / b)'

check_diag "int-mod-zero" "零|取模" \
'let a = 1
let b = 0
println(a % b)'

check_diag "overflow-add" "溢出" \
'let m = 9223372036854775807
println(m + 1)'

check_diag "overflow-mul" "溢出" \
'let m = 9223372036854775807
println(m * 2)'

check_diag "overflow-neg" "溢出" \
'let m = -9223372036854775807 - 1
println(-m)'

check_diag "type-mismatch-add" "Int|Float|数值|类型" \
'let a = "text"
let b = 1
println(a - b)'

check_diag "len-non-string" "String|字符串" \
'println(len(42))'

check_diag "abs-bad-type" "Int|Float|类型" \
'println(abs("x"))'

check_diag "toint-bad-string" "解析|整数" \
'println(toInt("abc"))'

check_diag "tofloat-bad-string" "解析|浮点" \
'println(toFloat("1.2.3"))'

# ---- .xbc 加载错误（02 §2 的字节码分发场景）----
# 这三条不走 check_diag（它们不是源码，无法 printf 生成），单独判。

echo
echo "  --- .xbc 加载诊断 ---"

check_xbc() {
    name="$1"
    keywords="$2"
    shift 2
    # 剩下的参数是构造该文件的 shell 命令
    out=$(sh -c "$*" 2>&1)
    rc=$?

    ok=1
    [ "$rc" = "0" ] && ok=0
    found=0
    echo "$keywords" | tr '|' '\n' | while IFS= read -r kw; do
        [ -z "$kw" ] && continue
        printf '%s' "$out" | grep -q "$kw" && echo HIT > "$TMPD/.hit"
    done
    [ -f "$TMPD/.hit" ] && { found=1; rm -f "$TMPD/.hit"; }
    [ "$found" = "0" ] && ok=0

    if [ "$ok" = "1" ]; then
        pass=$((pass+1))
        printf "  \033[32mPASS\033[0m  %-22s 退出码=%s  %s\n" "$name" "$rc" \
            "$(printf '%s' "$out" | grep -E '^error:' | head -1 | sed 's/^error: //')"
    else
        fail=$((fail+1))
        printf "  \033[31mFAIL\033[0m  %-22s 退出码=%s\n" "$name" "$rc"
        printf '%s\n' "$out" | sed 's/^/        /' | head -6
    fi
}

check_xbc "xbc-bad-magic" "不是 X 字节码|XBC1" \
    "echo hello > '$TMPD/badmagic.xbc' && '$X' run '$TMPD/badmagic.xbc'"

check_xbc "xbc-truncated" "不完整|截断" \
    "'$X' build '$ROOT/tests/p0/01-hello.xle' -o '$TMPD/full.xbc' >/dev/null 2>&1 && head -c 50 '$TMPD/full.xbc' > '$TMPD/trunc.xbc' && '$X' run '$TMPD/trunc.xbc'"

check_xbc "xbc-bad-version" "版本|0x" \
    "'$X' build '$ROOT/tests/p0/01-hello.xle' -o '$TMPD/ver.xbc' >/dev/null 2>&1 && printf '\\x02\\x00\\x00\\x00' | dd of='$TMPD/ver.xbc' bs=1 seek=4 conv=notrunc 2>/dev/null && '$X' run '$TMPD/ver.xbc'"

echo
if [ "$fail" = "0" ]; then
    printf "\033[32m%d 项诊断全部完整\033[0m\n" "$pass"
    exit 0
else
    printf "\033[31m%d 通过，%d 不完整\033[0m\n" "$pass" "$fail"
    exit 1
fi