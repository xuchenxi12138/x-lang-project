#!/bin/sh
# UTF-8 端到端检查
#
# 覆盖 01 §2.1 的承诺：「源文件编码 UTF-8，允许 BOM（读取时剥离）」。
# 这一组测试专门守 UTF-8 相关的正确性，因为它们都**不会让编译器崩溃**，
# 只会让行为悄悄错掉——最容易在重构中退化。
#
# 逐项说明为什么值得一个测试：
#   1. 输出字节必须是合法 UTF-8（不是 GBK 也不是混合）
#   2. BOM 必须被剥掉（否则第一行 println 变成 \uFEFFprintln）
#   3. 列号必须按**字符**而非字节（中文前缀下差 3 倍）
#   4. GBK 源文件必须报「编码错误」而不是「未定义变量」
#   5. 中文路径必须能打开（Windows 的 fopen 只认 ANSI 代码页）
#   6. 中文标识符的显示宽度必须算 2 列（诊断标签对齐）
#   7. 纯 ASCII 源文件不能受影响（回归保护）

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
TMPD="C:/xtmp/xutf8"
rm -rf "$TMPD"
mkdir -p "$TMPD"

export TMP="C:/xtmp"

pass=0
fail=0

ok() { pass=$((pass+1)); printf "  \033[32mPASS\033[0m  %-28s %s\n" "$1" "${2:-}"; }
ng() { fail=$((fail+1)); printf "  \033[31mFAIL\033[0m  %-28s %s\n" "$1" "${2:-}"; }

echo "UTF-8 端到端检查"
echo

# ---- 1. 输出字节是合法 UTF-8 ----
printf 'let a = 10\nlet b = 0\nprintln(a / b)\n' > "$TMPD/err.xle"
"$X" run "$TMPD/err.xle" > "$TMPD/o1.txt" 2>&1
if iconv -f UTF-8 -t UTF-8 "$TMPD/o1.txt" > /dev/null 2>&1; then
    # 再确认里面确实有中文（不是全 ASCII 而蒙混过关）
    if grep -q "整数除以零" "$TMPD/o1.txt"; then
        ok "output-is-valid-utf8"
    else
        ng "output-is-valid-utf8" "字节合法但中文内容不对"
    fi
else
    ng "output-is-valid-utf8" "输出不是合法 UTF-8"
fi

# ---- 2. BOM 被剥离 ----
printf '\xef\xbb\xbfprintln(1)\n' > "$TMPD/bom.xle"
out=$("$X" run "$TMPD/bom.xle" 2>&1)
if [ "$out" = "1" ]; then
    ok "bom-stripped"
else
    ng "bom-stripped" "得到「$out」，应为「1」"
fi

# ---- 2b. BOM + 中文源文件 ----
printf '\xef\xbb\xbf\xe4\xbb\xa4 \xe5\xa7\x93\xe5\x90\x8d = "\xe5\xbc\xa0\xe4\xb8\x89"\nprintln(\xe5\xa7\x93\xe5\x90\x8d)\n' > "$TMPD/bom-zh.xlc"
out=$("$X" run "$TMPD/bom-zh.xlc" 2>&1)
if [ "$out" = "张三" ]; then
    ok "bom-zh-source"
else
    ng "bom-zh-source" "得到「$out」，应为「张三」"
fi

# ---- 3. 列号按字符计 ----
# 第 2 行「令 年龄 = 姓名 + 不存在的」：`不存在的` 在第 13 个字符位
# 若是按字节计则会报 23 —— 那正是修之前的 bug。
printf '\xe4\xbb\xa4 \xe5\xa7\x93\xe5\x90\x8d = 1\n\xe4\xbb\xa4 \xe5\xb9\xb4\xe9\xbe\x84 = \xe5\xa7\x93\xe5\x90\x8d + \xe4\xb8\x8d\xe5\xad\x98\xe5\x9c\xa8\xe7\x9a\x84\n' > "$TMPD/col.xlc"
out=$("$X" check "$TMPD/col.xlc" 2>&1)
if printf '%s' "$out" | grep -q ':2:13'; then
    ok "column-is-chars" "报出 2:13"
elif printf '%s' "$out" | grep -q ':2:23'; then
    ng "column-is-chars" "报出 2:23（按字节计，应 13）"
else
    ng "column-is-chars" "$(printf '%s' "$out" | grep '位置' | head -1)"
fi

# ---- 3b. 运行时错误的位置也按字符计 ----
printf '\xe4\xbb\xa4 \xe6\x80\xbb\xe8\xae\xa1 = 10\n\xe4\xbb\xa4 \xe9\x99\xa4\xe6\x95\xb0 = 0\nprintln(\xe6\x80\xbb\xe8\xae\xa1 / \xe9\x99\xa4\xe6\x95\xb0)\n' > "$TMPD/col-rt.xlc"
out=$("$X" run "$TMPD/col-rt.xlc" 2>&1)
# `println(总计 / 除数)` 里 `/` 在第 12 个字符位；按字节是第 16
if printf '%s' "$out" | grep -q ':3:12'; then
    ok "column-rt-is-chars" "报出 3:12"
else
    ng "column-rt-is-chars" "$(printf '%s' "$out" | grep '位置' | head -1)"
fi

# ---- 4. GBK 源文件报编码错误 ----
# 「令 姓名 = 1」的 GBK 字节：c1 ee 20 d0 d5 c3 fb ...
printf '\xc1\xee \xd0\xd5\xc3\xfb = 1\n' > "$TMPD/gbk.xlc"
out=$("$X" run "$TMPD/gbk.xlc" 2>&1)
if printf '%s' "$out" | grep -q 'UTF-8'; then
    ok "gbk-detected" "明确报编码错误"
elif printf '%s' "$out" | grep -q '未定义'; then
    ng "gbk-detected" "报成「未定义变量」——真正原因未说明"
else
    ng "gbk-detected" "$(printf '%s' "$out" | head -1)"
fi

# ---- 5. 中文路径 ----
mkdir -p "$TMPD/中文目录"
printf 'println("ok")\n' > "$TMPD/中文目录/测试.xle"
out=$("$X" run "$TMPD/中文目录/测试.xle" 2>&1)
if [ "$out" = "ok" ]; then
    ok "chinese-path"
else
    ng "chinese-path" "$out"
fi

# ---- 5b. 中文路径 + 中文内容 ----
printf '\xe4\xbb\xa4 \xe4\xba\xba = "\xe6\x9d\x8e\xe5\x9b\x9b"\nprintln(\xe4\xba\xba)\n' > "$TMPD/中文目录/中文名.xlc"
out=$("$X" run "$TMPD/中文目录/中文名.xlc" 2>&1)
if [ "$out" = "李四" ]; then
    ok "chinese-path-zh-src"
else
    ng "chinese-path-zh-src" "$out"
fi

# ---- 6. 中文标签宽度算 2 列 ----
# 诊断里「位置」占 4 显示列，应与「  error:」对齐到同一内容列。
# 判据：两者内容起点相同。这里用「位置」后跟的路径前缀列位来验。
out=$("$X" run "$TMPD/err.xle" 2>&1)
line=$(printf '%s' "$out" | grep '位置' | head -1)
# 「位置」2 字应算 4 列，加行首 1 空格 = 5，补齐到 8 需 3 个空格
if printf '%s' "$line" | grep -q '^ 位置   '; then
    ok "label-width" "「位置」后 3 空格（4 显示列 + 1 缩进 = 5，补到 8）"
else
    ng "label-width" "「位置」后空格数不对：$(printf '%s' "$line" | head -c 20 | cat -A)"
fi

# ---- 7. 纯 ASCII 源文件不受影响 ----
printf 'let x = 1 + 2\nprintln(x)\n' > "$TMPD/ascii.xle"
out=$("$X" run "$TMPD/ascii.xle" 2>&1)
if [ "$out" = "3" ]; then
    ok "ascii-unaffected"
else
    ng "ascii-unaffected" "$out"
fi

# ---- 8. .xbc 往返：中文路径下的字节码 ----
"$X" build "$TMPD/中文目录/中文名.xlc" -o "$TMPD/中文目录/中文名.xbc" > /dev/null 2>&1
out=$("$X" run "$TMPD/中文目录/中文名.xbc" 2>&1)
if [ "$out" = "李四" ]; then
    ok "xbc-chinese-path"
else
    ng "xbc-chinese-path" "$out"
fi

echo
if [ "$fail" = "0" ]; then
    printf "\033[32m%d 项 UTF-8 检查全部通过\033[0m\n" "$pass"
    exit 0
else
    printf "\033[31m%d 通过，%d 失败\033[0m\n" "$pass" "$fail"
    exit 1
fi