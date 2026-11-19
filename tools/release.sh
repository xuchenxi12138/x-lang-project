#!/bin/sh
# tools/release.sh — 打一个可直接拷走的发行包到 release/xlang-<版本>/
#
# ── 目录布局（为什么是这几个名字）────────────────────────────
#
#     release/xlang-0.8.0-p0/
#     ├── bin/          x.exe —— 唯一需要执行的东西
#     ├── lib/          stdlib/*.xle —— 标准库源码（编译器直接读）
#     ├── conf/         配置文件（目前只有 xlang.env 示例）
#     ├── src/          编译器+C运行时的完整源码（能重新构建）
#     ├── docs/         规格与打包文档
#     ├── tests/        P0 测试集（拷走也能自己跑一遍）
#     ├── games/        示例程序（扫雷）
#     ├── vscode-x/     编辑器扩展（含已打包的 .vsix）
#     ├── tools/        构建与测试脚本（配合 src/ 使用）
#     ├── README.md
#     └── VERSION        版本号与打包时间
#
# ── 两个必须说清的取舍 ───────────────────────────────────
#
# 1. `lib/stdlib/` 而不是 `lib/` 直接放 .xle
#
#    编译器本来就按 `<root>/stdlib` 找标准库，root 由
#    find_stdlib_root() 决定。放在 lib/stdlib 的好处是：lib 以后还能
#    装别的东西（第三方库、静态库里），std 与第三方不会混在一层。
#
# 2. `bin/x.exe` 而不是根目录 x.exe
#
#    与 JDK / Go / Python 的习惯一致，也避免和 src/ 平级的文件混在
#    一起。配套改动在 src/main.c 的 find_stdlib_root()：新增
#    `<exe>/../lib/stdlib` 与 `<exe>/lib/stdlib` 两个候选。
#
# ── 用法 ────────────────────────────────────────────────
#   sh tools/release.sh            打当前版本
#   sh tools/release.sh --zip      额外产出一个 .zip
#
# 脚本是**幂等**的：每次都先清空目标版本目录。清理不依赖 rm 成功
# （环境的删除守卫会拦 rm -rf），改用 mv 挪到 .old 再重建 —— 与
# tools/run_tests.sh 里 pkg 用例的做法一致。

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
REL="$ROOT/release"

# ── 版本号从 x.exe 自己取，不手写 ─────────────────────────
#
# 手写版本号必然滞后：改了代码忘了改脚本，发行包名字就骗人。
# `x version` 的输出第一行形如 `x 0.8.0-p0 — X 语言编译与运行时`。
if [ ! -x "$X" ]; then
    echo "error: 找不到 $X，先跑 ./tools/build.sh" >&2
    exit 1
fi
VER=$("$X" 2>&1 | head -1 | sed 's/^x \([^ ]*\).*/\1/')
if [ -z "$VER" ]; then
    echo "error: 取不到版本号（x 输出第一行不是预期的格式）" >&2
    exit 1
fi

NAME="xlang-$VER"
DEST="$REL/$NAME"
OLD="$REL/.$NAME.old"

echo "== 打包 $NAME =="

# ── 幂等清理：mv 优先，不假设 rm 生效 ────────────────────
mkdir -p "$REL"
if [ -e "$DEST" ]; then
    rm -rf "$OLD" 2>/dev/null || true
    mv "$DEST" "$OLD" 2>/dev/null || true
    rm -rf "$DEST" 2>/dev/null || true
fi
rm -rf "$OLD" 2>/dev/null || true

mkdir -p "$DEST/bin" "$DEST/lib" "$DEST/conf" "$DEST/src" \
         "$DEST/docs" "$DEST/tests" "$DEST/games" "$DEST/tools"

# ── bin/：只放可执行文件 ────────────────────────────────
# x.exe 静态链接了 mingw 运行时（只依赖 Windows 自带的
# KERNEL32/USER32/GDI32/WS2_32/msimg32/msvcrt），所以这里不需要
# 附任何 DLL。哪天它开始依赖 libgcc_s_seh-1.dll 之类，就必须把
# 那个 DLL 也放进来，否则在没装 mingw 的机器上双击就闪退。
cp "$X" "$DEST/bin/x.exe"

# ── lib/stdlib/：标准库源码 ─────────────────────────────
mkdir -p "$DEST/lib/stdlib"
cp "$ROOT"/stdlib/*.xle "$DEST/lib/stdlib/"

# ── bin/packages/：内置包注册表（zh 中文别名包）──────────
#
# reg_root() 在 **<exe 目录>/packages** 找注册表 —— exe 在 bin/ 里，
# 所以注册表就是 bin/packages/。旧布局的 lib/packages 是个摆设，
# reg_root 根本不看它，已删除以免误导。
#
# zh 包按 <registry>/<名字>/<版本>/src/lib.xle 摆进去，用户项目里
# 写 `zh = "0.1"` 即可用；想要最新版也可以写成 path 依赖
# `zh = { path = "bin/packages/zh" }`。
ZH="$ROOT/pkg/zh"
if [ -f "$ZH/src/lib.xle" ]; then
    ZHV=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$ZH/x.toml" 2>/dev/null | head -1)
    [ -z "$ZHV" ] && ZHV="0.1.0"
    mkdir -p "$DEST/bin/packages/zh/$ZHV/src"
    cp "$ZH/src/lib.xle" "$DEST/bin/packages/zh/$ZHV/src/"
    cp "$ZH/x.toml"      "$DEST/bin/packages/zh/$ZHV/" 2>/dev/null || true
    echo "  zh 包            ->  bin/packages/zh/$ZHV"
else
    echo "  warn: 找不到 $ZH/src/lib.xle，发行包不含 zh 包" >&2
fi

# ── conf/：配置文件 ────────────────────────────────────
cat > "$DEST/conf/xlang.env" <<'ENVEOF'
# X 语言环境配置（示例）
#
# 这些变量都不是必需的 —— 编译器的默认查找顺序已经能覆盖标准布局：
#
#   1. <x.exe 所在目录>/stdlib            源码树布局
#   2. <x.exe 所在目录>/../lib/stdlib     发行版布局（本包就是这种）
#   3. <x.exe 所在目录>/lib/stdlib        扁平式布局
#   4. <x.exe 所在目录>/../stdlib         源码树的另一种摆法
#   5. 当前工作目录/stdlib
#
# 只有当标准库被挪到非常规位置时，才需要设下面这个。
#
# X_STDLIB_PATH 指向**包含 stdlib 的父目录**还是 stdlib 本身？
# 答：指向 stdlib 本身（目录里直接就是 math.xle / json.xle）。
# 写错的话编译器会明确报「该目录不存在」，不会静默回退。
#
# X_STDLIB_PATH=/opt/xlang/lib/stdlib

# 把 bin 加进 PATH 之后，在任何目录都能直接敲 x（Windows 用 setx）：
#   export PATH="$PATH:/path/to/xlang/bin"
ENVEOF

# ── src/：完整源码 ─────────────────────────────────────
#
# 用 cp -r 而不是 git archive：这个仓库里 src/ 有被 git 跟踪的部分，
# 也有生成的（gui_embed.h 由 gen_gui_embed.py 生成）。发行包要的是
# 「能重新构建」的完整树，不是「能 git checkout」的树。
cp "$ROOT"/src/*.c   "$DEST/src/" 2>/dev/null || true
cp "$ROOT"/src/*.h   "$DEST/src/" 2>/dev/null || true
mkdir -p "$DEST/src/gui"
cp "$ROOT"/src/gui/*.c "$DEST/src/gui/" 2>/dev/null || true
cp "$ROOT"/src/gui/*.h "$DEST/src/gui/" 2>/dev/null || true

# ── docs/ ─────────────────────────────────────────────
cp "$ROOT"/spec/*.md  "$DEST/docs/" 2>/dev/null || true
cp "$ROOT"/docs/*.md  "$DEST/docs/" 2>/dev/null || true

# ── tests/ ────────────────────────────────────────────
#
# 排除 `_*.tmp`：那是 run_tests.sh 跑测试时的中间产物（AOT 比对用的
# 临时 .exe），不属于测试集本身。把它们带进包里只是垃圾。
cp -r "$ROOT"/tests/p0 "$DEST/tests/" 2>/dev/null || true
rm -f "$DEST"/tests/p0/_*.tmp 2>/dev/null || true
rm -f "$DEST"/tests/p0/*.xbc 2>/dev/null || true

# ── examples/：示例项目 ───────────────────────────────
#
# 与 games/ 的区别：games 是单文件演示，examples 是**带 x.toml 的
# 完整项目**——zh-demo 演示包依赖（path 引用包内注册表里的 zh），
# 其它子目录随仓库演进。构建痕迹不进包。
if [ -d "$ROOT/examples" ]; then
    cp -r "$ROOT"/examples "$DEST/examples" 2>/dev/null || true
    find "$DEST/examples" -name "*.xbc" -delete 2>/dev/null || true
    find "$DEST/examples" -name "x.lock" -delete 2>/dev/null || true
    # zh-demo 的依赖在仓库里是 path 引用（../../pkg/zh）；发行包里 zh
    # 在内置注册表（bin/packages/zh/0.1.0）—— 按发行布局改写成版本
    # 依赖。只改包内副本，仓库原文件不动。
    if [ -f "$DEST/examples/zh-demo/x.toml" ]; then
        sed -i 's|^zh = { path = .*}|zh = "0.1"|' \
            "$DEST/examples/zh-demo/x.toml" 2>/dev/null || true
    fi
fi

# ── games/ ────────────────────────────────────────────
cp -r "$ROOT"/games/* "$DEST/games/" 2>/dev/null || true

# 示例说明。不直接放编译好的 .exe：build/ 里那两个是十几天前构建的，
# 与本包里的编译器不一定同源。让用户用**包内**的编译器现编，既保证
# 一致，也顺便验证了工具链能生成原生可执行文件。
cat > "$DEST/games/README.md" <<'GAMEEOF'
# 示例程序

## 扫雷（图形界面）

```sh
cd games/minesweeper
../../bin/x.exe run minesweeper.xle --profile=desktop
```

**`--profile=desktop` 不能省。** 默认场景是 `console`，它不内嵌窗口
运行时，于是 `winOpen` 之类的调用不起作用，还会给一条警告：

```
warn: 程序用到了窗口能力，但场景 `console` 不内嵌窗口运行时
```

这不是错误，是场景选错了。三个场景的名字用 `x profile list` 查看。

也可以编成独立 exe：

```sh
../../bin/x.exe build minesweeper.xle -o minesweeper.exe --emit=exe \
    --profile=desktop --cc=<你的 gcc 路径>
```

`--cc` 需要一个能用的 gcc（mingw-w64）。**不装 gcc 也能玩** —— 用
`x run` 直接解释执行，不需要任何外部工具链。

## 界面样式演示

```sh
cd games/settings
../../bin/x.exe run demo.xle --profile=desktop
../../bin/x.exe run demo-class.xle --profile=desktop
```

这两份演示了形状原语、样式层与类封装的 UI 组件。

## 逻辑测试（无窗口）

```sh
cd games/minesweeper
../../bin/x.exe run board_test.xle
```

`board_test.xle` 只测棋盘逻辑，不开窗口也不要用 `--profile`，
适合在无图形环境（CI）里跑。
GAMEEOF

# ── tools/：构建与测试脚本 ────────────────────────────
cp "$ROOT"/tools/*.sh "$DEST/tools/" 2>/dev/null || true
cp "$ROOT"/tools/*.py "$DEST/tools/" 2>/dev/null || true
cp "$ROOT"/tools/BUILD.md "$DEST/tools/" 2>/dev/null || true
chmod +x "$DEST"/tools/*.sh 2>/dev/null || true

# ── vscode-x/：编辑器扩展 ─────────────────────────────
#
# 只挑「能用」的部分：.vsix 安装包 + 源码 + 说明。
# node_modules / *.bak / 历史 .vsix 一律不进包 —— 那些是开发痕迹，
# 只会让发行包体积失控（node_modules 有 20 多 MB）。
mkdir -p "$DEST/vscode-x"
cp "$ROOT"/vscode-x/package.json "$DEST/vscode-x/" 2>/dev/null || true
cp "$ROOT"/vscode-x/README.md   "$DEST/vscode-x/" 2>/dev/null || true
cp "$ROOT"/vscode-x/LICENSE     "$DEST/vscode-x/" 2>/dev/null || true
latest_vsix=$(ls -1 "$ROOT"/vscode-x/x-lang-*.vsix 2>/dev/null | sort -V | tail -1)
[ -n "${latest_vsix:-}" ] && cp "$latest_vsix" "$DEST/vscode-x/"
cp -r "$ROOT"/vscode-x/syntaxes "$DEST/vscode-x/" 2>/dev/null || true
cp -r "$ROOT"/vscode-x/language-configuration "$DEST/vscode-x/" 2>/dev/null || true
mkdir -p "$DEST/vscode-x/src"
cp "$ROOT"/vscode-x/src/*.js "$DEST/vscode-x/src/" 2>/dev/null || true

# ── 顶层文档 ──────────────────────────────────────────
cp "$ROOT"/README.md "$DEST/"

# ── VERSION：说明这个包是什么时候、从哪个状态打的 ──────
cat > "$DEST/VERSION" <<VEREOF
name     = $NAME
version  = $VER
built_at = $(date '+%Y-%m-%d %H:%M:%S %z')
built_on = $(uname -s) $(uname -m 2>/dev/null || echo '')
host     = $(uname -n 2>/dev/null || echo unknown)
VEREOF

# ── 发行说明 ──────────────────────────────────────────
cat > "$DEST/发行说明.md" <<'NOTEEOF'
# X 语言发行包

## 这是什么

一个**自带标准库与中文别名包、拷走就能用**的 X 语言工具链。
不需要装 mingw，不需要设环境变量，不需要 node。

## 先跑起来

```sh
bin/x.exe run tests/p0/01-hello.xle
```

`01-hello.xle` 会打印一行字。看到它说明编译器、字节码解释器、
标准库三条链路都正常。

## 中文编程（0.9.0 起）

`.xlc` 后缀即中文模式（关键字、内建类型、入口函数 `主` 全部中文），
内置的 `zh` 包再把标准库高频函数中文化。最小示例：

```x
// 我的程序.xlc
引入 @zh

函 主() -> 整数 {
    打印行(开方(144.0))      // 12.0
    打印行(长度("中文编程"))  // 4
    返回 0
}
```

配套的项目清单（x.toml）：

```toml
[package]
name = "我的项目"
version = "0.1.0"
entry = "src/我的程序.xlc"

[dependencies]
zh = "0.1"          # 解析到本包 bin/packages/zh/0.1.0
```

`examples/zh-demo/` 是可以直接跑的完整示例：

```sh
cd examples/zh-demo && ../../bin/x.exe run src/main.xlc
```

## 目录

| 目录 | 放什么 | 能删吗 |
|---|---|---|
| `bin/` | `x.exe` —— 编译器 + 解释器 + LSP 服务端 | **不能** |
| `bin/packages/` | 内置包注册表（`zh` 中文别名包在 `zh/0.1.0/`） | **不能**，删了 `引入 @zh` 全废 |
| `lib/stdlib/` | 标准库源码（10 个模块） | **不能**，删了 `import std.*` 全废 |
| `conf/` | 环境配置示例 | 可以 |
| `src/` | 编译器源码，用于重新构建 | 可以 |
| `docs/` | 语言规格（9 份）+ 打包说明 | 可以 |
| `tests/p0/` | P0 测试集 | 可以 |
| `examples/` | 示例项目（zh-demo 等，带 x.toml） | 可以 |
| `games/` | 扫雷与界面示例（源码） | 可以 |
| `vscode-x/` | 编辑器扩展（含 `x-lang-*.vsix`） | 可以 |
| `tools/` | 构建与测试脚本 | 可以 |

## 注意 `lib/` 这一层不能省

`x.exe` 按下面的顺序找标准库：

1. `<exe 目录>/stdlib`
2. `<exe 目录>/../lib/stdlib`  ← **本包用的是这个**
3. `<exe 目录>/lib/stdlib`
4. `<exe 目录>/../stdlib`
5. `<当前目录>/stdlib`

所以把 `bin/x.exe` 单独拷到别处会找不到标准库。要拷就整个目录一起拷。
`bin/packages/` 同理 —— 包注册表按 **exe 所在目录**的 `packages/` 解析，
`bin/` 与 `bin/packages/` 必须一起走。

标准库被挪到非常规位置时，设 `X_STDLIB_PATH` 指向**stdlib 目录本身**：

```sh
X_STDLIB_PATH=/opt/xlang/lib/stdlib bin/x.exe run a.xle
```

写错时会明确报「该目录不存在」，不会静默回退。

## 编辑器支持

```sh
code --install-extension vscode-x/x-lang-0.4.0.vsix
```

装完要让扩展找到 `bin/x.exe`。两种办法：

- 在设置里填 `xlang.executablePath` 指向本包的 `bin/x.exe`（最稳）
- 把 `bin/` 加进系统 `PATH`

扩展只提供**语法高亮**；补全、悬停、参数提示、大纲这四类全部来自
`bin/x.exe lsp`。所以找不到 `x.exe` 时表现是「高亮正常但没有任何
代码提示」—— 这最容易让人误以为是扩展装坏了。

## 重新构建（可选）

需要 mingw-w64 的 gcc 在 `C:/xgcc/mingw64/bin`，或改 `tools/build.sh`
里的 `XGCC`。

```sh
sh tools/build.sh
```

跑一遍测试：

```sh
sh tools/run_tests.sh
```

## 版本

见 `VERSION`。
NOTEEOF

# ── 自检：这一步不能省 ────────────────────────────────
#
# 打包最容易出的错不是漏文件，而是「文件都在但布局不对」——
# 比如 stdlib 放到了 lib/ 却没放 lib/stdlib/，从外面看目录很漂亮，
# 一跑就报「找不到标准库」。所以打完立刻用包里那个 x.exe 真跑一次。
echo "== 自检 =="
SELFTEST=$(cd "$DEST" && ./bin/x.exe run tests/p0/01-hello.xle 2>&1)
if [ $? -ne 0 ]; then
    echo "FAIL: 包里的 x.exe 跑不起来：" >&2
    echo "$SELFTEST" >&2
    exit 1
fi
echo "  x run           OK  ($SELFTEST)"

# 标准库也验一次 —— 01-hello 不用标准库，光它过不能说明 lib 摆对了。
#
# 这段刻意写得啰嗦：`set -eu` 下任何一条命令返回非 0 都会让脚本
# 中途退出，而「自检发现布局不对」应该打印明确原因再退出，不是
# 静无声息地停在半路。所以每步都显式判断，不用 && 串。
ST_FILE="$DEST/_x_stdlib_probe.xle"
# 用 math.xle 里真实存在的函数（不是 sqrt —— 那是内建函数，
# 不需要 import，用它测等于什么都没测）。
printf 'import std.math\nprintln(normalize(5.0, 0.0, 10.0))\n' > "$ST_FILE"
ST_OUT=$(cd "$DEST" && ./bin/x.exe run _x_stdlib_probe.xle 2>&1) || true
rm -f "$ST_FILE"
if echo "$ST_OUT" | grep -q "0.5"; then
    echo "  import std.math OK  ($ST_OUT)"
else
    echo "FAIL: 包里的标准库找不到或跑错：" >&2
    echo "$ST_OUT" >&2
    echo "" >&2
    echo "下一步排查：" >&2
    echo "  1. 确认 $DEST/lib/stdlib/math.xle 存在" >&2
    echo "  2. 确认 bin/x.exe 的 find_stdlib_root 已包含 <exe>/../lib/stdlib" >&2
    echo "     （src/main.c，第 2 个候选）" >&2
    exit 1
fi

# zh 包 + 注册表 + 重载 + Any 一起验 —— 这是 0.9.0 的两个新语言
# 特性（resolve_overload / Any 透传返回）与包注册表布局的合练。
# 任何一环断了（registry 摆错位置、zh 缺文件、重载解析回退），
# 这个探针都会当场红。
ZH_PROBE="$DEST/_x_zh_probe"
if [ -f "$DEST/bin/packages/zh/0.1.0/src/lib.xle" ]; then
    mkdir -p "$ZH_PROBE/src"
    cat > "$ZH_PROBE/x.toml" <<'ZHTOML'
[package]
name = "zh_probe"
version = "0.1.0"
entry = "src/main.xle"

[dependencies]
zh = "0.1"
ZHTOML
    cat > "$ZH_PROBE/src/main.xle" <<'ZHMAIN'
import @zh

fn main() -> Int {
    println(开方(144.0))
    println(绝对值(-7))
    println(绝对值(-2.5))
    println(查找("中文编程", "编程"))
    println(查找(建数组(3, 0), 0))
    println(长度(重复("ab", 2)))
    return 0
}
ZHMAIN
    # Windows 的 stdout 是文本模式，println 落到管道里是 CRLF ——
    # 先归一化行尾再比对，否则探针在 Windows 上永远假红。
    ZH_OUT=$(cd "$ZH_PROBE" && "$DEST/bin/x.exe" run src/main.xle 2>&1 | tr -d '\r') || true
    ZH_WANT='12.0
7
2.5
2
0
4'
    if [ "$ZH_OUT" = "$ZH_WANT" ]; then
        echo "  zh 包 + 重载 + Any  OK  (12.0 / 7 / 2.5 / 2 / 0 / 4)"
    else
        echo "FAIL: zh 包探针不符合预期：" >&2
        echo "$ZH_OUT" >&2
        echo "" >&2
        echo "期望：$ZH_WANT" >&2
        echo "排查：1) bin/packages/zh/0.1.0/src/lib.xle 在不在" >&2
        echo "      2) x.toml 的 zh = \"0.1\" 版本约束是否满足" >&2
        rm -rf "$ZH_PROBE" 2>/dev/null || true
        exit 1
    fi
    rm -rf "$ZH_PROBE" 2>/dev/null || true
else
    echo "  warn: 包内没有 zh 包，跳过 zh 探针" >&2
fi

# ── 可选：压成 zip ────────────────────────────────────
#
# 为什么不用 Compress-Archive：PowerShell 5.1 写出的 zip 里路径分隔符
# 是反斜杠（.NET 老实现），Windows 自己能解，Linux/macOS 的 unzip 会
# 把反斜杠当成**文件名的一部分**，解出来的目录树整个平铺 —— 一个
# 「本机验证过、发出去就坏」的格式比没有更糟（0.9.0 的包实测踩中）。
# 顺序：zip 命令 → Python zipfile（路径规范、全平台）→ 放弃并说明。
if [ "${1:-}" = "--zip" ]; then
    Z="$REL/$NAME.zip"
    rm -f "$Z" 2>/dev/null || true
    if command -v zip >/dev/null 2>&1; then
        ( cd "$REL" && zip -qr "$NAME.zip" "$NAME" )
        echo "  zip             OK  ($(du -h "$Z" | cut -f1))"
    elif command -v python >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1; then
        PY=$(command -v python || command -v python3)
        ( cd "$REL" && "$PY" -c "
import zipfile, os, sys
name = sys.argv[1]
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(name):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, '.').replace(os.sep, '/'))
" "$NAME" )
        if [ -f "$Z" ]; then
            echo "  zip(python)     OK  ($(du -h "$Z" | cut -f1))"
        else
            echo "  warn: python zipfile 失败，跳过 .zip" >&2
        fi
    else
        echo "  warn: 没有 zip 也没有 python，跳过 .zip 打包" >&2
        echo "        目录本身已就绪，可以手工压缩：$DEST" >&2
    fi
fi

echo ""
echo "完成：$DEST"
echo ""
echo "目录结构："
# 用 find 而不是 tree：Windows 的 Git Bash 通常没有 tree。
find "$DEST" -maxdepth 2 -type d | sed "s|$REL/||" | sort | sed 's|^|  |'
echo ""
echo "体积：$(du -sh "$DEST" | cut -f1)"
echo "文件数：$(find "$DEST" -type f | wc -l | tr -d ' ')"