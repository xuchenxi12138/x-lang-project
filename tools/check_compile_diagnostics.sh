#!/bin/sh
# 编译期诊断完整性检查
#
# 与 check_diagnostics.sh（运行时）配对。
# 06 文档要求编译期诊断同样给全「位置 / 原因 / 建议」三段。
# P0 的编译期诊断目前大多只有「位置 + 原因」，「建议」还不完整 ——
# 这个脚本如实记录哪些缺建议，作为 P1 的待办清单，而不是假装通过。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
TMPD="C:/xtmp/xcdiag"
mkdir -p "$TMPD"

total=0
with_help=0

# check_compile_error <名称> <源码>
check_compile_error() {
    name="$1"
    src="$2"

    printf '%s\n' "$src" > "$TMPD/$name.xle"

    out=$("$X" check "$TMPD/$name.xle" 2>&1)
    rc=$?

    total=$((total+1))

    has_pos=no
    has_help=no
    printf '%s' "$out" | grep -qE '[^ ]+\.xl[ec]:[0-9]+:[0-9]+' && has_pos=yes
    printf '%s' "$out" | grep -qE 'help:|note:|建议' && has_help=yes
    [ "$has_help" = "yes" ] && with_help=$((with_help+1))

    msg=$(printf '%s' "$out" | grep -E '^error:' | head -1 | sed 's/^error: //')

    printf "  %-24s pos=%-3s help=%-3s  %s\n" "$name" "$has_pos" "$has_help" "$msg"
}

echo "编译期诊断检查"
echo
echo "  场景                    位置     建议     错误信息"
echo "  ─────────────────────────────────────────────────────────────────────"

check_compile_error "undefined-var" 'println(nothere)'
check_compile_error "undefined-fn"  'nothere()'
check_compile_error "assign-undefined" 'nothere = 1'
check_compile_error "missing-brace" 'if true {'
check_compile_error "missing-rparen" 'println(1'
check_compile_error "expr-expected" 'let x = +'
check_compile_error "no-entry" 'fn foo() -> Int { return 1 }'
check_compile_error "dup-fn" 'fn f() -> Int { return 1 }
fn f() -> Int { return 2 }'
check_compile_error "break-outside" 'break'
check_compile_error "continue-outside" 'continue'
check_compile_error "arg-count" 'fn f(a: Int) -> Int { return a }
println(f(1, 2))'
check_compile_error "uninit-no-type" 'let x'
check_compile_error "defer-unsupported" 'defer println(1)'
check_compile_error "index-non-array" 'let a = 1
println(a[0])'
# 下标必须是 Int。这一条之前只在运行时检查，编译期放过去了。
check_compile_error "index-non-int" 'let a = array(3, 0)
println(a[1.5])'

# ---- 后缀校验（v0.8：.xle / .xlc）----
# 这三个不能用 check_compile_error（它统一写 .xle），单独处理。
check_suffix() {
    name="$1"
    keywords="$2"
    file="$3"
    content="$4"

    printf '%s\n' "$content" > "$TMPD/$file"
    out=$("$X" check "$TMPD/$file" 2>&1)
    rc=$?
    total=$((total+1))

    has_pos=no
    has_help=no
    printf '%s' "$out" | grep -qE '[^ ]+: ?[0-9]+:[0-9]+|位置' && has_pos=yes
    printf '%s' "$out" | grep -qE 'help:|note:|建议' && has_help=yes
    [ "$has_help" = "yes" ] && with_help=$((with_help+1))

    found=0
    printf '%s' "$out" | grep -q "$keywords" && found=1
    if [ "$rc" != "0" ] && [ "$found" = "1" ] && [ "$has_help" = "yes" ]; then
        printf "  \033[32m%-24s\033[0m pos=%-3s help=%-3s  %s\n" "$name" "$has_pos" "$has_help" \
            "$(printf '%s' "$out" | grep -E '^error:' | head -1 | sed 's/^error: //')"
    else
        printf "  \033[31m%-24s\033[0m 退出码=%s 关键词=%s help=%s\n" "$name" "$rc" "$found" "$has_help"
        printf '%s\n' "$out" | sed 's/^/        /' | head -5
    fi
}

echo
echo "  --- 后缀校验 ---"
check_suffix "suffix-old-x"   "不是 X 的源文件" "legacy.x"  'println(1)'
check_suffix "suffix-none"    "不是 X 的源文件" "noext"      'println(1)'
check_suffix "suffix-txt"     "不是 X 的源文件" "note.txt"   'println(1)'

echo
echo "  共 $total 个场景，$with_help 个带建议（help/note 段）"
echo

# 有建议的比例低于 50% 时不算失败，只提示 —— P1 的待办
pct=$((with_help * 100 / total))
if [ "$pct" -lt 50 ]; then
    printf "\033[33m提示：建议覆盖率 %d%%，P1 需为剩余场景补 help 段（06 §3 六段式）\033[0m\n" "$pct"
else
    printf "\033[32m建议覆盖率 %d%%\033[0m\n" "$pct"
fi