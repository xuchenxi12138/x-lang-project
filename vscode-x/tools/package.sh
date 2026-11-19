#!/bin/sh
# tools/package.sh — 验证 + 打包成 .vsix
#
# ── 为什么打包前要跑一遍验证 ────────────────────────────────
#
# .vsix 一旦发出去，装的人遇到「代码提示不出现」只会怀疑自己的配置。
# 而这类问题的根因在这里只有两处：
#
#   1. src/lspClient.js 的映射表（CompletionItemKind / SymbolKind /
#      Position 单位）—— 全是手写的数字，编译器管不到
#   2. lsp.c 的响应拼装 —— 手写 JSON 字符串，写错一个逗号只有客户端
#      那边才会发现
#
# 两处都不会在构建时报错。所以打包前必须用真实协议跑一遍，把
# 「装完才发现不能用」变成「打包时就失败」。
#
# 用法：sh tools/package.sh
set -e

cd "$(dirname "$0")/.."

echo "── 1. 检查 x.exe ──────────────────────────────"
EXE="../build/x.exe"
if [ ! -f "$EXE" ]; then
    echo "FAIL: 找不到 $EXE，先跑 x-lang/tools/build.sh"
    exit 1
fi
echo "ok: $EXE"

echo
echo "── 2. 服务端协议验证 ──────────────────────────"
if ! command -v node >/dev/null 2>&1; then
    echo "FAIL: 没有 node，无法验证。装 node 后再打包。"
    echo "      （这不是「可跳过」的步骤 —— 见本脚本开头的说明）"
    exit 1
fi

# ── 三个验证都手工查退出码，不用 `cmd | tail` ───────────────
#
# 管道会把左边命令的退出码吃掉（拿到的是 tail 的 0），于是
# `set -e` 形同虚设 —— 验证全挂也照样打包出 .vsix。那正是本脚本
# 存在的意义所要防的事，所以这里必须先落文件、先取码、后显示。
check() {
    _label="$1"
    _out="$2"
    shift 2
    # 必须先单独跑再取 $? —— 写成 `if cmd; then ... else _rc=$?` 时，
    # else 分支里的 $? 已经是 if 判断本身的返回值，不是 cmd 的。
    "$@" > "$_out" 2>&1
    _rc=$?
    if [ "$_rc" = "0" ]; then
        tail -3 "$_out"
        return 0
    fi
    echo "FAIL: $_label（退出码 $_rc）"
    grep -E 'FAIL|错误' "$_out" | head -8 | sed 's/^/  /'
    return "$_rc"
}

check "服务端协议" ".pk-s.$$.log" node tools/check-lsp.js || { rm -f ".pk-s.$$.log"; exit 1; }
rm -f ".pk-s.$$.log"

echo
echo "── 3. 客户端链路验证 ──────────────────────────"
check "客户端链路" ".pk-c.$$.log" node tools/check-client.js || { rm -f ".pk-c.$$.log"; exit 1; }
rm -f ".pk-c.$$.log"

echo
echo "── 4. 清单自洽 ────────────────────────────────"
check "清单自洽" ".pk-m.$$.log" node tools/check-manifest.js || { rm -f ".pk-m.$$.log"; exit 1; }
rm -f ".pk-m.$$.log"

echo
echo "── 5. 打包 ────────────────────────────────────"
VER=$(node -e 'console.log(require("./package.json").version)')
VSIX="x-lang-$VER.vsix"

# ── 已装同版本就拒绝打包 ────────────────────────────────────
#
# 这是本次「改了代码却依然没提示」的第二个根因：打包脚本照常出包，
# 但 code --install-extension 看到版本号相同就跳过安装，于是装着的
# 还是旧代码。界面上什么都没发生，而所有验证都是绿的。
#
# 所以这里查一下本机扩展目录：同版本已存在就要求先升版本号。
# 这个检查只在「装到本机」这个场景有意义，异地分发不受影响 ——
# 但改版本号同样是对的（否则对方同样装不上）。
EXTDIR="$HOME/.vscode/extensions"
INSTALLED="$EXTDIR/x-lang.x-lang-$VER"
if [ -d "$INSTALLED" ]; then
    echo "FAIL: 本机已装着同版本 $VER（$INSTALLED）"
    echo "      code --install-extension 会认为「同版本已装」而跳过，"
    echo "      你会以为更新了，实际跑的还是旧代码。"
    echo "      先改 package.json 的 version（例如 $VER → 加一），再打包。"
    exit 1
fi

if [ -f "$VSIX" ]; then
    echo "已存在 $VSIX，先挪走（不覆盖，便于对比）"
    mv "$VSIX" "$VSIX.bak.$$" 2>/dev/null || true
fi

if command -v vsce >/dev/null 2>&1; then
    vsce package
elif npx --yes @vscode/vsce --version >/dev/null 2>&1; then
    npx --yes @vscode/vsce package
else
    echo "FAIL: 找不到 vsce，也没法用 npx 临时下载。"
    echo "      装一个：npm install -g @vscode/vsce"
    exit 1
fi

if [ ! -f "$VSIX" ]; then
    echo "FAIL: 打包命令结束但没产出 $VSIX"
    exit 1
fi

echo
echo "── 完成 ──────────────────────────────────────"
SIZE=$(wc -c < "$VSIX" | tr -d ' ')
echo "$VSIX（$SIZE 字节）"
echo
echo "检查包内文件（应只有 5~8 个、几十 KB）："
echo "  unzip -l $VSIX"
echo
echo "装到本机："
echo "  code --install-extension $VSIX"
echo "然后 Ctrl+Shift+P → Developer: Reload Window"