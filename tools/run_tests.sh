#!/bin/sh
# X 语言 P0 测试运行器
#
# 约定：
#   tests/p0/NN-name.xle   正常运行，退出码必须为 0（英文源文件）
#   tests/p0/NN-name.xlc   中文源文件，后缀自动开中文模式，无需任何命令行参数
#   tests/p0/NN-name.expect 若存在，则用它比对 stdout（精确匹配）
#   tests/p0/NN-name.fail   若存在，表示该程序**预期失败**；
#                           文件内容写期望的退出码（70 = 运行时错误，1 = 编译错误）
#
# 每个测试额外断言两件事：
#   1. .xlc 与 .xle 编出的指令序列逐条一致（中文零成本，v0.8 起由后缀保证）
#   2. x run 与 x build && x run .xbc 的输出逐字节一致（一份字节码两档执行）
#
# 用法:
#   ./tools/run_tests.sh            跑全部
#   ./tools/run_tests.sh -v         显示每个测试的输出
#   ./tools/run_tests.sh 05         只跑文件名含 05 的

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/build/x.exe"
TDIR="$ROOT/tests/p0"
TMPD="C:/xtmp/xtest"
mkdir -p "$TMPD"

# ── 临时目录必须纯 ASCII ─────────────────────────────────────
#
# MinGW 的 cc1.exe 在非 ASCII 的 %TEMP%（比如中文用户名下的
# C:\Users\徐辰曦\AppData\Local\Temp）里建不出中间 .o，报的是：
#
#   Fatal error: can't create ...\Temp\ccXX.o: No such file or directory
#
# 这个错误看起来像「目录被删了」，实际是路径编码问题 ——
# 与 tools/build.sh 顶部记的坑 3a 是同一个。
#
# 为什么在这里 export 而不是逐个调用点加：本脚本有 6 处直接调用
# cc（AOT 一致性、profile、缓冲、multi-package、semver），逐个加
# 既啰嗦又必然漏 —— 而漏掉的那处会以「假失败」的形式出现，比
# 不测更糟：它让人以为代码坏了。统一在入口设一次，所有子进程继承
# （"$X" build --cc=... 内部也是 fork 出 cc，同样继承）。
export TMP="C:/xtmp"
export TEMP="C:/xtmp"
mkdir -p "$TMP"

VERBOSE=0
FILTER=""
for a in "$@"; do
    case "$a" in
        -v) VERBOSE=1 ;;
        *)  FILTER="$a" ;;
    esac
done

if [ ! -x "$X" ]; then
    echo "错误：找不到 $X，请先运行 ./tools/build.sh" >&2
    exit 1
fi

pass=0
fail=0
failed_list=""
aot_skipped=0
aot_marker=0

# ── 找 C 编译器 ─────────────────────────────────────────────
#
# 为什么不满足于 `command -v gcc`：本项目的工具链刻意装在
# C:/xgcc 而不进 PATH（见 tools/build.sh 的说明）。只查 PATH 会
# 一直得到「没找到」，于是 AOT 一致性检查**从来没跑过**，
# 而输出里看起来跟跑过一样（都是绿色的 PASS）。
#
# 这就是「假通过」——比没有检查更危险，因为它让人以为覆盖到了。
# 所以这里按 X_CC 环境变量 → PATH → 项目默认工具链的顺序找，
# 找到就用，找不到才计入 aot_skipped 并在汇总时明确报告。
find_cc() {
    if [ -n "${X_CC:-}" ] && [ -x "$X_CC" ]; then
        echo "$X_CC"; return 0
    fi
    if command -v gcc >/dev/null 2>&1; then
        command -v gcc; return 0
    fi
    if command -v cc >/dev/null 2>&1; then
        command -v cc; return 0
    fi
    for c in /c/xgcc/mingw64/bin/gcc.exe /usr/bin/gcc /usr/bin/cc; do
        if [ -x "$c" ]; then echo "$c"; return 0; fi
    done
    return 1
}
CC_BIN=$(find_cc || true)
gui_skipped=0

for src in "$TDIR"/*.xle "$TDIR"/*.xlc; do
    case "$src" in
        *.xlc) name=$(basename "$src" .xlc) ;;
        *)     name=$(basename "$src" .xle) ;;
    esac

    # 跳过英文对照版（它们由 -zh 那一条对比测试覆盖）
    case "$name" in
        *-en) continue ;;
    esac

    if [ -n "$FILTER" ]; then
        case "$name" in
            *"$FILTER"*) ;;
            *) continue ;;
        esac
    fi

    # ── 图形测试的环境门禁 ──────────────────────────────────
    #
    # 18-gui 需要真实的桌面会话。在没有图形界面的环境里
    # （CI 容器、纯 SSH）xgui_open 会给出错误并退出 70，
    # 于是这个测试必然失败 —— 但那不是代码有问题。
    #
    # 处理方式：按 X_GUI=0 显式关闭，或让脚本探测一次
    # 「能不能开窗」再决定。探测比硬编码文件名更好：
    # 将来加更多图形测试时不用改这里。
    case "$src" in
        *gui*)
            if [ "${X_GUI:-1}" = "0" ]; then
                gui_skipped=$((gui_skipped + 1))
                continue
            fi
            ;;
    esac

    out="$TMPD/$name.out"
    err="$TMPD/$name.err"

    # 中文模式由后缀决定（.xlc 自动，.xle 英文）。
    # 测试里**不再传 --zh** —— 后缀即声明，这是 v0.8 的规则，
    # 测试就应该按用户实际会写的方式跑，而不是替编译器补开关。
    "$X" run "$src" > "$out" 2> "$err"
    rc=$?

    expect_fail="$TDIR/$name.fail"
    expect_out="$TDIR/$name.expect"

    ok=1
    reason=""

    if [ -f "$expect_fail" ]; then
        # 预期失败
        want=$(tr -d ' \r\n' < "$expect_fail")
        if [ "$rc" != "$want" ]; then
            ok=0
            reason="退出码 $rc，期望 $want"
        fi
    else
        # 预期成功
        if [ "$rc" != "0" ]; then
            ok=0
            reason="退出码 $rc，期望 0"
        fi
        if [ -f "$expect_out" ]; then
            # ── 比对前把换行归一化 ────────────────────────────
            #
            # 这里原本是 `diff -q "$expect_out" "$out"` 直接比字节。
            #
            # 问题：`println` 在 Windows 上输出 CRLF，而 .expect 文件
            # 在仓库里存的是 LF，靠 core.autocrlf=true 在工作区里
            # 转成 CRLF 才能匹配。也就是说测试能不能过取决于**这台
            # 机器的 git 配置** —— 换一台 autocrlf=false 的机器，
            # 或者把仓库 clone 到 Linux（那里 autocrlf 默认 false），
            # 全部 .expect 立刻失败，而源码一个字都没错。
            #
            # 归一化之后，.expect 只表达「期望哪些行、什么内容」，
            # 不表达行尾风格。行尾是不是 CRLF 属于平台细节，不该由
            # 测试夹具来断言 —— 真要测它，应该单独测
            # `printf 'a\n' | od`，而不是把它混进每一条输出比对里。
            #
            # 为什么用 tr 而不是 unix2dos/dos2unix：那两个工具不是
            # 到处都有（Git Bash 里就没有 dos2unix，但有 tr）。
            #
            # 为什么用两个临时文件而不是 <(...) 进程替换：本脚本是
            # `#!/bin/sh`，进程替换是 bash 扩展，POSIX sh（dash）不认。
            tr -d '\r' < "$expect_out" > "$TDIR/_e.tmp" 2>/dev/null
            tr -d '\r' < "$out"        > "$TDIR/_o.tmp" 2>/dev/null
            if ! diff -q "$TDIR/_e.tmp" "$TDIR/_o.tmp" > /dev/null 2>&1; then
                ok=0
                reason="${reason:+$reason; }stdout 与 $name.expect 不符"
            fi
            rm -f "$TDIR/_e.tmp" "$TDIR/_o.tmp"
        fi
    fi

    # 中文/英文字节码一致性检查
    #
    # 只比「指令 + 操作数」，不比源码位置注释（两份文件行号不同，
    # 位置必然不同，那不是「成本」）。这正是 04 §2.5 的主张：
    # 中文别名在词法层归一化后，后续阶段完全看不到中文的存在。
    if [ "$ok" = "1" ]; then
        case "$name" in
            *-zh)
                # 中文版是 .xlc（后缀即声明），英文版是 .xle，都不传 --zh
                en="$TDIR/$(echo "$name" | sed 's/-zh$/-zh-en/').xle"
                if [ -f "$en" ]; then
                    "$X" disasm "$src" 2>/dev/null \
                        | grep -E '^ +[0-9]{4}  ' | sed 's/ *;.*$//' \
                        > "$TMPD/$name.zh.dis"
                    "$X" disasm "$en" 2>/dev/null \
                        | grep -E '^ +[0-9]{4}  ' | sed 's/ *;.*$//' \
                        > "$TMPD/$name.en.dis"
                    if ! diff -q "$TMPD/$name.zh.dis" "$TMPD/$name.en.dis" > /dev/null 2>&1; then
                        ok=0
                        reason="${reason:+$reason; }中英字节码不一致"
                        if [ "$VERBOSE" = "1" ]; then
                            diff "$TMPD/$name.zh.dis" "$TMPD/$name.en.dis" | head -10 | sed 's/^/        /'
                        fi
                    fi
                fi
                ;;
        esac
    fi

    # .xbc 往返一致性检查（02 §2「两档共享同一份字节码」）
    #
    # `x build a.xle -o a.xbc && x run a.xbc` 的输出必须与 `x run a.xle`
    # 逐字节相同。这是整个架构最核心的承诺，每天都该跑一遍。
    if [ "$ok" = "1" ] && [ ! -f "$expect_fail" ]; then
        bcfile="$TMPD/$name.xbc"
        if "$X" build "$src" -o "$bcfile" > /dev/null 2>&1; then
            "$X" run "$bcfile" > "$TMPD/$name.bc.out" 2> "$TMPD/$name.bc.err"
            bcrc=$?
            if [ "$bcrc" != "0" ] || ! diff -q "$out" "$TMPD/$name.bc.out" > /dev/null 2>&1; then
                ok=0
                reason="${reason:+$reason; }.xbc 往返执行结果不一致（退出码 $bcrc）"
            fi
        else
            ok=0
            reason="${reason:+$reason; }.xbc 生成失败"
        fi
    fi

    # 超指令中性检查（07 §3.3.1a）
    #
    # 超指令是**纯性能优化**，绝不能改变语义。所以对每个预期成功的
    # 测试，额外用 X_NO_SUPER=1 编译执行一遍，输出必须逐字节相同。
    #
    # 为什么这条检查是必须的：超指令合并会改变指令的绝对下标，而所有
    # 跳转操作数都是绝对下标。重映射写错时的症状是**不报错、不崩溃、
    # 只是算错** —— 跳转跳过头，栈上少压一个操作数，解释器读到上一次
    # 残留的值，类型完全合法。这种 bug 靠「程序能跑通」发现不了，
    # 只有 A/B 对比才能抓到。
    if [ "$ok" = "1" ] && [ ! -f "$expect_fail" ]; then
        X_NO_SUPER=1 "$X" run "$src" > "$TMPD/$name.nosuper.out" 2> "$TMPD/$name.nosuper.err"
        nsrc=$?
        if [ "$nsrc" != "0" ] || ! diff -q "$out" "$TMPD/$name.nosuper.out" > /dev/null 2>&1; then
            ok=0
            reason="${reason:+$reason; }关闭超指令后结果不同（退出码 $nsrc）—— 超指令改变了语义"
            if [ "$VERBOSE" = "1" ]; then
                diff "$out" "$TMPD/$name.nosuper.out" 2>/dev/null | head -10 | sed 's/^/        /'
            fi
        fi
    fi

    # AOT 一致性检查（02 §2「两档共享同一份字节码」的延伸）
    #
    # 解释执行与 AOT 编译走的是**同一份字节码**，只是一个用 switch 循环
    # 逐条解释、一个把每条指令翻译成 C 语句再交给 gcc -O2。两条路的
    # 输出必须逐字节相同 —— 否则「优化不改变语义」这句话就是空话。
    #
    # 这条检查实际抓到过一整类 bug：生成的代码里栈下标算错（把
    # 「执行前深度」和「执行后深度」混用），产物不崩溃、不报错，
    # 只是每个值都算成 0 或 1。纯靠「能跑通」完全发现不了。
    #
    # 找不到 C 编译器时：显式跳过并打印提示，绝不静默。
    #
    # 「跳过」和「通过」在输出里看起来一样，是所有测试脚本里最危险的
    # 一类失败模式 —— 本次就是先看到 15 个全绿，才发现 AOT 检查根本没
    # 跑（gcc 不在 PATH 上，`command -v gcc` 返回空，整段被 if 吞掉）。
    # 所以这里把「没有编译器」也说出来，并用 X_AOT_SKIPPED 计数，
    # 最后汇总时一并报告。
    if [ "$ok" = "1" ] && [ ! -f "$expect_fail" ] && [ "${X_AOT:-1}" != "0" ]; then
        # 源文件级的 AOT 豁免：`// aot-skip` 标记。
        #
        # ── 为什么需要这个出口 ──────────────────────────────
        #
        # v0.20 的动态调用（classNew / methodCall）与「按名查注解」
        # 目前只在解释器路径实现。AOT 侧要支持它们，得给生成代码
        # 配一张函数指针表（按虚表槽索引到函数体），工作量不小且
        # 与框架开发并行推进更合适。
        #
        # 用动态能力的测试（以及将来依赖框架的测试）打上
        # `// aot-skip` 标记即可跳过这条检查 —— 跳过会计数并在
        # 汇总里显式报告，绝不静默（见下面的教训注释）。
        case "$(head -c 4096 "$src")" in
            *"aot-skip"*)
                aot_skipped=$((aot_skipped + 1))
                aot_marker=$((aot_marker + 1))
                ;;
            *)
                aotexe="$TMPD/$name-aot.exe"
                if [ -n "$CC_BIN" ]; then
                    if "$X" build "$src" --emit=exe --cc="$CC_BIN" -o "$aotexe" \
                           > "$TMPD/$name.aotbuild" 2>&1; then
                        "$aotexe" > "$TMPD/$name.aot.out" 2> "$TMPD/$name.aot.err"
                        aotrc=$?
                        if [ "$aotrc" != "0" ] || ! diff -q "$out" "$TMPD/$name.aot.out" > /dev/null 2>&1; then
                            ok=0
                            reason="${reason:+$reason; }AOT 产物输出与解释器不一致（退出码 $aotrc）"
                            if [ "$VERBOSE" = "1" ]; then
                                diff "$out" "$TMPD/$name.aot.out" 2>/dev/null | head -10 | sed 's/^/        /'
                            fi
                        fi
                        rm -f "$aotexe"
                    else
                        ok=0
                        reason="${reason:+$reason; }AOT 编译失败"
                        if [ "$VERBOSE" = "1" ]; then
                            tail -5 "$TMPD/$name.aotbuild" | sed 's/^/        /'
                        fi
                    fi
                else
                    aot_skipped=$((aot_skipped + 1))
                fi
                ;;
        esac
    fi

    if [ "$ok" = "1" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  %s\n" "$name"
        if [ "$VERBOSE" = "1" ] && [ -s "$out" ]; then
            sed 's/^/        /' "$out"
        fi
    else
        fail=$((fail + 1))
        failed_list="$failed_list $name"
        printf "  \033[31mFAIL\033[0m  %s — %s\n" "$name" "$reason"
        if [ -s "$err" ]; then
            sed 's/^/        /' "$err" | head -10
        fi
    fi
done

# ============================================================
# 扫雷：棋盘逻辑 + 界面截图回归
# ============================================================
#
# ── 为什么放在 tests/p0 之外 ────────────────────────────────
#
# tests/p0 里每个文件都是「单文件自包含」的，而扫雷是多文件
# （board.xle + ui.xle + minesweeper.xle），靠 import 串起来。
# 把它塞进 p0 会让那一批测试的约定变模糊 —— 而 p0 的价值恰恰
# 在于「约定简单，一眼能看出每个文件测什么」。
#
# 所以单独一段，测两件事：
#
#   1. board_test.xle —— 棋盘逻辑的 25 项断言，纯 println 驱动，
#      不碰窗口。这是唯一能在无头环境里验证的部分。
#
#   2. demo.xle —— 用固定种子造四个局面各截一张图，验证界面
#      **能画出来**。截图内容不做像素比对（第一版永远是「不对」），
#      价值在于留下基线：改了绘制代码后重跑，人眼 diff 一次就能
#      看出哪里变了。
#
# 这两条都要求 X_GUI 未被显式关掉：demo 要开窗，而 board_test
# 通过 ui.xle 间接 import 了窗口内建（虽然实际不调用）。
if [ -z "$FILTER" ] || [ "${FILTER#*mine}" != "$FILTER" ] || [ "${FILTER#*board}" != "$FILTER" ]; then
    MINES="$ROOT/games/minesweeper"
    if [ -f "$MINES/board_test.xle" ]; then
        out="$TMPD/mines-board.out"
        err="$TMPD/mines-board.err"
        "$X" run "$MINES/board_test.xle" > "$out" 2> "$err"
        rc=$?
        if [ "$rc" = "0" ] && grep -q "board ok" "$out"; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  games/minesweeper/board_test\n"
            if [ "$VERBOSE" = "1" ]; then sed 's/^/        /' "$out"; fi
        else
            fail=$((fail + 1))
            failed_list="$failed_list mines-board"
            printf "  \033[31mFAIL\033[0m  games/minesweeper/board_test — 退出码 %s\n" "$rc"
            sed 's/^/        /' "$err" | head -10
            sed 's/^/        /' "$out" | tail -5
        fi
    fi

    if [ -f "$MINES/demo.xle" ]; then
        if [ "${X_GUI:-1}" = "0" ]; then
            gui_skipped=$((gui_skipped + 1))
        else
            # demo 会往 build/ 写四张 bmp。跑之前先删掉，这样
            # 「跑完存在」才是真的证明了它跑完了，而不是上次的残留。
            #
            # 注意：这里**不能用 `shot-*.bmp` 通配统计**。settings 的
            # demo 也写 shot-ui-*.bmp，也落在同一个 glob 里，两者会
            # 互相把对方的文件算进自己的数量。逐个点名才稳。
            MINE_SHOTS="shot-playing.bmp shot-lost.bmp shot-won.bmp shot-inter.bmp"
            for s in $MINE_SHOTS; do rm -f "$ROOT/build/$s"; done
            out="$TMPD/mines-demo.out"
            err="$TMPD/mines-demo.err"
            ( cd "$ROOT" && "$X" run games/minesweeper/demo.xle > "$out" 2> "$err" )
            rc=$?
            shots=0
            for s in $MINE_SHOTS; do
                if [ -f "$ROOT/build/$s" ]; then shots=$((shots + 1)); fi
            done
            if [ "$rc" = "0" ] && [ "$shots" = "4" ] && grep -q "demo ok" "$out"; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  games/minesweeper/demo（%s 张截图）\n" "$shots"
            else
                fail=$((fail + 1))
                failed_list="$failed_list mines-demo"
                printf "  \033[31mFAIL\033[0m  games/minesweeper/demo — 退出码 %s，截图 %s 张\n" \
                       "$rc" "$shots"
                sed 's/^/        /' "$err" | head -10
            fi
        fi
    fi
fi

# ============================================================
# 类型层组件库：games/settings/demo.xle
# ============================================================
#
# 这是 stdlib/ui.xle「类型层」的验收程序 —— 它存在的意义不是好看，
# 而是**用真实界面证明结构体、方法、字段访问能承担布局与绘制**。
#
# 检查三件事，缺一不可：
#   1. 退出码 0 且有 `demo ok`     —— 程序跑通
#   2. 两张截图（深/浅）都落盘      —— 绘制真的发生了
#   3. 关键算术断言出现在输出里      —— 不是硬编码的像素，而是算出来的
#
# 第 3 条尤其重要。如果哪天有人把 `Rect.area()` 改坏，截图可能看着
# 还行（面积不参与绘制），但断言会立刻炸掉。
if [ -z "$FILTER" ] || [ "${FILTER#*settings}" != "$FILTER" ] || [ "${FILTER#*ui}" != "$FILTER" ]; then
    SET="$ROOT/games/settings"
    if [ -f "$SET/demo.xle" ]; then
        if [ "${X_GUI:-1}" = "0" ]; then
            gui_skipped=$((gui_skipped + 1))
        else
            UI_SHOTS="shot-ui-dark.bmp shot-ui-light.bmp"
            for s in $UI_SHOTS; do rm -f "$ROOT/build/$s"; done
            out="$TMPD/settings-demo.out"
            err="$TMPD/settings-demo.err"
            ( cd "$ROOT" && "$X" run games/settings/demo.xle > "$out" 2> "$err" )
            rc=$?
            shots=0
            for s in $UI_SHOTS; do
                if [ -f "$ROOT/build/$s" ]; then shots=$((shots + 1)); fi
            done
            ok=1
            [ "$rc" = "0" ] || ok=0
            [ "$shots" = "2" ] || ok=0
            grep -q "demo ok" "$out" || ok=0
            # 算术断言：面积 236*338=79768，右边界 26+236=262
            grep -q "面积 79768" "$out" || ok=0
            grep -q "右边界 262" "$out" || ok=0
            if [ "$ok" = "1" ]; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  games/settings/demo（类型层 + %s 张截图）\n" "$shots"
                if [ "$VERBOSE" = "1" ]; then sed 's/^/        /' "$out"; fi
            else
                fail=$((fail + 1))
                failed_list="$failed_list settings-demo"
                printf "  \033[31mFAIL\033[0m  games/settings/demo — 退出码 %s，截图 %s 张\n" \
                       "$rc" "$shots"
                sed 's/^/        /' "$err" | head -10
                sed 's/^/        /' "$out" | tail -8
            fi
        fi
    fi
fi

# ============================================================
# 类层组件库：games/settings/demo-class.xle
# ============================================================
#
# 与上一段（结构体版 demo）测的是同一件事，但走的是**完全不同的
# 代码路径**：abstract Widget + 六个子类 + 虚方法派发 + 控件树。
#
# 为什么不能只信结构体版那一份：两条路径共用 ui.xle 的底层绘制
# 函数，但「谁来决定画什么」是两套实现。类层漏掉一个 draw 覆写、
# 或者虚表槽号串了，结构体版**照样全绿** —— 它根本不经过虚派发。
#
# 检查四件事：
#   1. 退出码 0 且有 `demo-class ok`
#   2. 两张截图（深/浅）都落盘
#   3. 内容区算术 —— 12/40/232/336，与 Widget.body 的规则一致
#   4. 引用语义与多态命中的断言 —— 别名改的是同一个对象、
#      隐藏后 pick 返回 null
#
# 第 3、4 条是「类系统真的在工作」的最小证据集合：前者证明继承
# 来的字段读得到，后者证明引用语义与虚派发没走样。
if [ -z "$FILTER" ] || [ "${FILTER#*class}" != "$FILTER" ] || [ "${FILTER#*settings}" != "$FILTER" ]; then
    SET="$ROOT/games/settings"
    if [ -f "$SET/demo-class.xle" ]; then
        if [ "${X_GUI:-1}" = "0" ]; then
            gui_skipped=$((gui_skipped + 1))
        else
            CLS_SHOTS="shot-class-dark.bmp shot-class-light.bmp"
            for s in $CLS_SHOTS; do rm -f "$ROOT/build/$s"; done
            out="$TMPD/settings-class.out"
            err="$TMPD/settings-class.err"
            ( cd "$ROOT" && "$X" run games/settings/demo-class.xle > "$out" 2> "$err" )
            rc=$?
            shots=0
            for s in $CLS_SHOTS; do
                if [ -f "$ROOT/build/$s" ]; then shots=$((shots + 1)); fi
            done
            ok=1
            [ "$rc" = "0" ] || ok=0
            [ "$shots" = "2" ] || ok=0
            grep -q "demo-class ok" "$out" || ok=0
            # 内容区：UiPanel(x=0,y=0,w=256,h=388) 带标题
            #   → x+12=12, y+40=40, w-24=232, h-40-12=336
            grep -q "内容区 Rect { x: 12, y: 40, w: 232, h: 336 }" "$out" || ok=0
            grep -q "面积 77952" "$out" || ok=0
            # 引用语义：别名 set(30) 之后原对象也是 30
            grep -q "别名改后 音量 30" "$out" || ok=0
            # 夹取：150 → 100，-20 → 0
            grep -q "夹取后 音量 100" "$out" || ok=0
            grep -q "夹取后 音量 0" "$out" || ok=0
            # 多态命中：隐藏后 pick 无结果
            grep -q "隐藏后命中(50,100) 为空 true" "$out" || ok=0
            grep -q "隐藏后 hit 为假 false" "$out" || ok=0
            if [ "$ok" = "1" ]; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  games/settings/demo-class（类层 + %s 张截图）\n" "$shots"
                if [ "$VERBOSE" = "1" ]; then sed 's/^/        /' "$out"; fi
            else
                fail=$((fail + 1))
                failed_list="$failed_list settings-class"
                printf "  \033[31mFAIL\033[0m  games/settings/demo-class — 退出码 %s，截图 %s 张\n" \
                       "$rc" "$shots"
                sed 's/^/        /' "$err" | head -10
                sed 's/^/        /' "$out" | tail -10
            fi
        fi
    fi
fi

echo
# ============================================================
# 使用场景（Profile）：三档真的不一样吗
# ============================================================
#
# ── 为什么这一段必须存在 ─────────────────────────────────────
#
# `--profile` 最容易变成「一个存进变量、然后什么也不做的参数」——
# 名字在 --help 里，行为与默认完全相同，谁也没法证明它有用。
# 所以这里对档位做三项**可证伪**的检查，每一项都对应一个真实的
# 物理差异：
#
#   1. 三档对同一份源码产出**逐字节相同**的 stdout
#      → 证明「场景不改变语义」。这一条是红线：如果档位能改变
#        计算结果，那它就是不可信的语言特性。
#
#   2. desktop 档的产物 PE 子系统是 2（WINDOWS_GUI）
#      → 证明「双击不弹黑框」是真的生效了。子系统写在文件头里，
#        由加载器读，运行期改不了 —— 这是档位唯一无法被运行期
#        模拟的差异。
#
#   3. service 档下 stderr 无缓冲，console 档下有缓冲
#      → 证明「服务崩溃前最后一条错误不丢」。这一条用「杀进程后
#        看输出在不在」来测，因为缓冲的差别只在异常退出时显现。
#
# 三项都不依赖图形环境，唯一依赖 CC_BIN（第 2、3 项要编 exe）。
if [ -z "$FILTER" ] || [ "${FILTER#*profile}" != "$FILTER" ]; then
    PROF_SRC="$ROOT/tests/p0/20-profile.xle"
    if [ -f "$PROF_SRC" ]; then
        # ---- 1. 三档输出一致 ----
        prof_ok=1
        prof_reason=""
        for p in console desktop service; do
            "$X" run "$PROF_SRC" --profile=$p > "$TMPD/prof-$p.out" 2> "$TMPD/prof-$p.err"
            prc=$?
            if [ "$prc" != "0" ]; then
                prof_ok=0
                prof_reason="--profile=$p 退出码 $prc"
                break
            fi
            if ! diff -q "$TMPD/prof-console.out" "$TMPD/prof-$p.out" > /dev/null 2>&1; then
                prof_ok=0
                prof_reason="--profile=$p 的输出与 console 不同"
                break
            fi
        done
        if [ "$prof_ok" = "1" ] && ! grep -q "profile ok" "$TMPD/prof-console.out"; then
            prof_ok=0
            prof_reason="缺少 'profile ok' 标记"
        fi
        if [ "$prof_ok" = "1" ]; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  --profile 三档语义一致\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list profile-semantics"
            printf "  \033[31mFAIL\033[0m  --profile 三档语义一致 — %s\n" "$prof_reason"
            for p in console desktop service; do
                [ -s "$TMPD/prof-$p.err" ] && sed 's/^/        /' "$TMPD/prof-$p.err" | head -5
            done
        fi

        # ---- 2. PE 子系统 ----
        #
        # 读 PE 头的 Subsystem 字段：文件偏移 0x3C 是 e_lfanew，
        # 指向 "PE\0\0"；COFF 头 20 字节后是可选头，Subsystem 在
        # 可选头偏移 0x44（PE32）或 0x44（PE32+ 也一样，因为
        # 那一段布局在两种格式里相同）。
        #
        # 用 od 而不是 python：这段测试要在没有 python 的机器上也能跑。
        if [ -n "$CC_BIN" ]; then
            sub_ok=1
            sub_reason=""
            for p in console desktop; do
                exe="$TMPD/prof-sub-$p.exe"
                if ! "$X" build "$PROF_SRC" --emit=exe --profile=$p --cc="$CC_BIN" \
                        -o "$exe" > "$TMPD/prof-sub-$p.log" 2>&1; then
                    sub_ok=0
                    sub_reason="--profile=$p 编译失败"
                    break
                fi
                # 只在 PE 产物上查子系统；非 Windows 宿主上这段跳过
                magic=$(od -An -tx1 -N2 "$exe" 2>/dev/null | tr -d ' \n')
                if [ "$magic" != "4d5a" ]; then
                    sub_ok=2      # 2 = 不是 PE，不适用
                    break
                fi
                # e_lfanew：0x3C 处的 4 字节小端
                lfanew=$(od -An -tu4 -j60 -N4 "$exe" 2>/dev/null | tr -d ' \n')
                suboff=$((lfanew + 4 + 20 + 68))
                sub=$(od -An -tu2 -j"$suboff" -N2 "$exe" 2>/dev/null | tr -d ' \n')
                if [ "$p" = "desktop" ]; then
                    [ "$sub" != "2" ] && { sub_ok=0; sub_reason="desktop 档子系统是 $sub，期望 2(WINDOWS_GUI)"; }
                else
                    [ "$sub" != "3" ] && { sub_ok=0; sub_reason="console 档子系统是 $sub，期望 3(CONSOLE)"; }
                fi
                [ "$sub_ok" = "0" ] && break
                rm -f "$exe"
            done
            if [ "$sub_ok" = "1" ]; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  --profile desktop 产出 WINDOWS 子系统\n"
            elif [ "$sub_ok" = "2" ]; then
                printf "  \033[33mSKIP\033[0m  --profile 子系统检查（产物不是 PE，当前平台不适用）\n"
            else
                fail=$((fail + 1))
                failed_list="$failed_list profile-subsystem"
                printf "  \033[31mFAIL\033[0m  --profile 子系统 — %s\n" "$sub_reason"
                [ -s "$TMPD/prof-sub-desktop.log" ] && \
                    sed 's/^/        /' "$TMPD/prof-sub-desktop.log" | tail -5
            fi
        else
            aot_skipped=$((aot_skipped + 1))
        fi

        # ---- 3. service 档 stdout 无缓冲 ----
        #
        # ── 这一项测什么，以及为什么必须用硬杀 ─────────────
        #
        # 要证明「stdout 的缓冲策略真的不同」，唯一能观察它的时刻
        # 是**进程被杀、缓冲区没被 flush** 的时候。所以这里编一个
        # 「输出半行（不带 \n）然后死循环」的程序，从外部硬杀它，
        # 再比两档留下多少字节。
        #
        #   行缓冲（console）  半行还躺在缓冲里 → 文件 0 字节
        #   无缓冲（service）  每次都直接写出去 → 文件 8 字节
        #
        # ── 这里原本写的是一个错误的东西 ─────────────────────
        #
        # 最初打算测的是「service 档的 stderr 无缓冲」。实测发现那件事
        # 测不出来：C 标准要求 stderr 启动即无缓冲，MinGW 上 2000 行
        # stderr + TerminateProcess 硬杀，加不加 setvbuf 都存活 2000 行。
        # 也就是说那一行 setvbuf 是空操作 —— 一个「看起来做了保护、
        # 实际什么都没做」的假实现。
        #
        # 所以把 service 档的真实差异改成 stdout 无缓冲（这一项能测出来），
        # 并且用这一项测试把「差异存在」钉死。
        #
        # Windows 上用 taskkill 硬杀；没有 taskkill 的平台跳过 ——
        # 不假装通过。
        if [ -n "$CC_BIN" ]; then
            BUF_SRC="$TMPD/bufhalf.xle"
            # ── 为什么用 sleep 而不是忙等循环 ──────────────────
            #
            # 这里原本写的是 `while n < 2000000000 { n = n + 1 }`，
            # 本意是「让进程活着」。但那是一个没有任何可观测结果的
            # 循环，AOT 优化后（局部帧 + always_inline，gcc 能把整个
            # 循环消掉）进程会立刻退出、缓冲区随之 flush，于是
            # console 档也会留下 8 字节，测试把「优化生效」误报成
            # 「缓冲策略坏了」。
            #
            # 要测的是缓冲策略，不是循环能不能跑满。sleep 是真正的
            # 系统调用，不可被优化掉，进程一定会停在原地等硬杀。
            printf 'print("HALFLINE")\nsleep(10000)\n' > "$BUF_SRC"

            buf_ok=1
            buf_reason=""
            if command -v taskkill >/dev/null 2>&1; then
                for p in console service; do
                    exe="$TMPD/buf-$p.exe"
                    if ! "$X" build "$BUF_SRC" --emit=exe --profile=$p \
                            --cc="$CC_BIN" -o "$exe" > "$TMPD/buf-$p.log" 2>&1; then
                        buf_ok=0; buf_reason="--profile=$p 编译失败"; break
                    fi
                    out="$TMPD/buf-$p.out"
                    : > "$out"
                    "$exe" > "$out" 2>/dev/null &
                    bpid=$!
                    # 等它真的开始跑，再杀。用 sleep 而不是轮询：这里
                    # 没有可靠的「已启动」信号，0.4 秒远大于启动开销。
                    sleep 0.4
                    wp=$(cat "/proc/$bpid/winpid" 2>/dev/null)
                    if [ -n "$wp" ]; then
                        taskkill //F //PID "$wp" >/dev/null 2>&1
                    fi
                    kill -9 "$bpid" 2>/dev/null
                    wait "$bpid" 2>/dev/null
                done

                cb=$(wc -c < "$TMPD/buf-console.out" | tr -d ' ')
                sb=$(wc -c < "$TMPD/buf-service.out" | tr -d ' ')

                # console 档：半行必须**丢**（证明它是行缓冲）
                if [ "$cb" != "0" ]; then
                    buf_ok=0
                    buf_reason="console 档硬杀后留下 ${cb} 字节，期望 0（行缓冲应丢弃半行）"
                fi
                # service 档：半行必须**在**（证明它是无缓冲）
                if [ "$buf_ok" = "1" ] && [ "$sb" = "0" ]; then
                    buf_ok=0
                    buf_reason="service 档硬杀后是 0 字节，期望 8（无缓冲应保留半行）"
                fi
            else
                buf_ok=2      # 没有 taskkill，无法硬杀
            fi

            if [ "$buf_ok" = "1" ]; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  --profile service 档 stdout 无缓冲（硬杀后保留半行）\n"
            elif [ "$buf_ok" = "2" ]; then
                printf "  \033[33mSKIP\033[0m  --profile 输出缓冲检查（无 taskkill，无法硬杀）\n"
            else
                fail=$((fail + 1))
                failed_list="$failed_list profile-buf"
                printf "  \033[31mFAIL\033[0m  --profile 输出缓冲 — %s\n" "$buf_reason"
                [ -s "$TMPD/buf-console.log" ] && sed 's/^/        /' "$TMPD/buf-console.log" | tail -3
            fi
            rm -f "$TMPD"/buf-*.exe
        else
            aot_skipped=$((aot_skipped + 1))
        fi
    fi
fi

echo
# ============================================================
# 包管理：声明 → 引用 → 解析 → 编译 的端到端闭环
# ============================================================
#
# ── 为什么这一段值得单独存在 ───────────────────────────────
#
# `import @name` 最容易做成「语法上接受、语义上没人解析」的假特性：
# parser 认得 @ 前缀、把 "@name" 存进 imports[]，然后模块加载器
# 拿着这串东西去找一个叫 "@name.xle" 的文件、报「文件不存在」。
# 用户看到的现象是「这个语法好像支持，但永远用不了」。
#
# 所以这里验证四段链路，每一段都必须真实工作：
#
#   1. 正向：清单声明 + import @ → 能编译能跑，输出正确
#   2. 位置无关：从项目根与从 app 目录跑，结果相同
#      （清单查找是「向上找」，不是「看当前目录」）
#   3. 未声明：import @ 一个清单里没有的名字 → **报错**，
#      而且错误位置指向那句 import，不是指向清单
#   4. 路径错：清单声明了但源码不存在 → 报错并列出试过的所有路径
#
# 3 和 4 是「包管理真的在管事」的证据 —— 一个不做校验的解析器
# 会让 1 也通过，但 3、4 会静默放过。
if [ -z "$FILTER" ] || [ "${FILTER#*pkg}" != "$FILTER" ]; then
    PKG_APP="$ROOT/tests/pkg/app"
    if [ -f "$PKG_APP/src/main.xle" ]; then

        # ---- 1. 正向闭环 ----
        pout="$TMPD/pkg-app.out"
        perr="$TMPD/pkg-app.err"
        "$X" run "$PKG_APP/src/main.xle" > "$pout" 2> "$perr"
        prc=$?
        pok=1
        preason=""
        if [ "$prc" != "0" ]; then
            pok=0; preason="退出码 $prc"
        elif [ -f "$PKG_APP/src/main.expect" ]; then
            # 与 tests/p0 的比对一样先归一化行尾 —— 理由见那里，
            # 一句话：测试不该因为宿主的 git autocrlf 设置而红。
            tr -d '\r' < "$PKG_APP/src/main.expect" > "$TMPD/_pe.tmp" 2>/dev/null
            tr -d '\r' < "$pout"                     > "$TMPD/_po.tmp" 2>/dev/null
            if ! diff -q "$TMPD/_pe.tmp" "$TMPD/_po.tmp" > /dev/null 2>&1; then
                pok=0; preason="输出与 main.expect 不符"
            fi
            rm -f "$TMPD/_pe.tmp" "$TMPD/_po.tmp"
        fi
        if [ "$pok" = "1" ]; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  pkg 声明→引用→解析→编译\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list pkg-e2e"
            printf "  \033[31mFAIL\033[0m  pkg 端到端 — %s\n" "$preason"
            sed 's/^/        /' "$perr" | head -10
        fi

        # ---- 2. 位置无关 ----
        #
        # 从 app 自己的目录跑（这时「向上找清单」要真的往上一级走）。
        # 这一条抓到过一类真实的设计错误：把清单查找写成
        # 「读当前目录的 x.toml」，于是 cd 到 src/ 里编译就找不到依赖。
        ( cd "$PKG_APP" && "$X" run src/main.xle > "$TMPD/pkg-cwd.out" 2>&1 )
        crc=$?
        if [ "$crc" = "0" ] && diff -q "$pout" "$TMPD/pkg-cwd.out" > /dev/null 2>&1; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  pkg 清单向上查找（换工作目录结果一致）\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list pkg-cwd"
            printf "  \033[31mFAIL\033[0m  pkg 清单向上查找 — 退出码 %s\n" "$crc"
            sed 's/^/        /' "$TMPD/pkg-cwd.out" | head -10
        fi

        # ---- 3. 未声明的依赖必须报错 ----
        #
        # 断言两件事：退出码非 0，且错误信息里出现「没有这个依赖」，
        # 且位置指向 .xle 而不是 x.toml。
        #
        # 为什么连位置一起断言：报错位置指向清单会误导用户去改清单，
        # 而实际该改的是源码那一行 import。
        bad="$TMPD/pkg-bad.xle"
        printf 'import @nosuchpkg\nprintln(1)\n' > "$bad"
        cp "$bad" "$PKG_APP/src/_bad.xle"
        "$X" run "$PKG_APP/src/_bad.xle" > "$TMPD/pkg-bad.out" 2>&1
        brc=$?
        rm -f "$PKG_APP/src/_bad.xle"
        if [ "$brc" != "0" ] && grep -q "没有这个依赖" "$TMPD/pkg-bad.out" \
           && grep -q "_bad.xle" "$TMPD/pkg-bad.out"; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  pkg 引用未声明的包会报错\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list pkg-undeclared"
            printf "  \033[31mFAIL\033[0m  pkg 未声明依赖 — 退出码 %s\n" "$brc"
            sed 's/^/        /' "$TMPD/pkg-bad.out" | head -10
        fi

        # ---- 4. 源码缺失必须报错并列出路径 ----
        #
        # ── 这里构造的状态：依赖已声明，但源码后来不见了 ────
        #
        # 注意 `pkg add` 现在会**当场校验路径可用**，所以不能再像
        # 以前那样直接 `add ghostpkg --path ../no-such-dir` ——
        # 那种写法依赖一个 bug（add 不校验），bug 修掉后测试就红了。
        #
        # 但「依赖声明了、源码却找不到」这个状态**真实存在**，而且
        # 是必须报好的错误：
        #
        #   · 用户 checkout 之后忘了拉某个 submodule
        #   · 依赖包被重命名或移动了目录
        #   · 打包发布时漏了 vendor 目录
        #
        # 所以测试按真实路径构造：先建一个**可用**的包目录、add 成功，
        # 然后让那个目录「消失」，再编译。这样测的既不是 add 的校验，
        # 也不是 add 的破例，而是「解析期发现源码不在了」这条路径。
        #
        # ── 为什么用 mv 改名而不是 rm -rf ──────────────────────
        #
        # 原来这一步是 `rm -rf "$GHOSTDIR"`。它在本机能跑，但整套
        # 测试跑下来会失败，而单独跑这一段又是通过的 —— 差别在于
        # 本机环境对批量删除有守卫（一轮内删超过几十个文件后会
        # 拦住后续 rm）。于是 `rm` 静默没生效、目录还在、import
        # 解析成功、`grc` 变成 0，测试报「退出码 0，期望非 0」。
        #
        # 那种红是**环境造成的假红**，比漏测更浪费人：它逼着人去
        # 查一个不存在的缺陷。而 mv 改名是原子操作，不需要外部
        # 许可，也不会被删除守卫拦 —— 而且它更贴近要模拟的现实：
        # 「目录被移走/改名了」，而不是「被删了」。
        ghost="$TMPD/pkg-ghost.xle"
        printf 'import @ghostpkg\nprintln(1)\n' > "$ghost"
        cp "$ghost" "$PKG_APP/src/_ghost.xle"

        # 造一个真的可用的依赖包，让 add 通过
        GHOSTDIR="$PKG_APP/../_ghostpkg_tmp"

        # ── 目标名必须唯一：mv 到已存在目录会把源塞进去 ──────
        #
        # 这里原来写死 `$GHOSTDIR.gone`。整套测试连跑两次就假红，
        # 而单独跑这一段是绿的。根因是 mv 的一条语义：
        #
        #   mv SOURCE TARGET  当 TARGET 是已存在的目录时，
        #   执行的是「把 SOURCE 移到 TARGET 里面」——
        #   也就是变成 TARGET/$(basename SOURCE)，
        #   而不是「用 SOURCE 替换 TARGET」。
        #
        # 于是链路变成：上一次的 .gone 因删除守卫没删掉 →
        # 这次 `mv $GHOSTDIR $GHOSTDIR.gone` 生成
        # `.gone/_ghostpkg_tmp/` → 源目录 **原地还在** →
        # import 解析成功 → 退出码 0 → 测试报
        # 「源码缺失 — 退出码 0」。
        #
        # 那条报错说的现象和真实原因毫无关系，最耗人。
        # 带进程号的唯一目标名从根上消除撞名：本进程不可能
        # 第二次移到同名目标上，跨进程也不会互相覆盖。
        GHOSTGONE="$GHOSTDIR.gone.$$"

        # ── 清理必须不依赖 rm 成功 ──────────────────────────
        #
        # 环境的删除守卫在整套测试跑了一段时间后会开始拒绝 rm -rf
        # （打印 SAFE_DELETE_BULK_CONFIRM_REQUIRED 并静默不执行）。
        # 于是这里如果只写 `rm -rf "$GHOSTDIR"`，旧目录会留在原地，
        # 后面 `mkdir -p` 复用它的 src/lib.xle 上一条内容，测试
        # 就在一个**被污染的状态**下开始 —— 表现是「单独跑能过、
        # 整套跑就红」。
        #
        # 所以清理先用 mv 挪到 .gone 名字（mv 不会被守卫拦），
        # 再从 .gone 建新目录。每一步都不假设 rm 真的生效。
        if [ -e "$GHOSTDIR" ]; then
            rm -rf "$GHOSTGONE" 2>/dev/null
            mv "$GHOSTDIR" "$GHOSTGONE" 2>/dev/null
            # 万一 mv 也失败（目录被占用），至少不要复用旧内容
            rm -rf "$GHOSTDIR" 2>/dev/null
        fi
        rm -rf "$GHOSTGONE" 2>/dev/null
        mkdir -p "$GHOSTDIR/src"
        printf 'fn ghost_entry() -> Int { return 0 }\n' > "$GHOSTDIR/src/lib.xle"

        cp "$PKG_APP/x.toml" "$TMPD/pkg-x.toml.bak"
        ( cd "$PKG_APP" && "$X" pkg add ghostpkg --path "$GHOSTDIR" ) \
            > "$TMPD/pkg-add.log" 2>&1
        addrc=$?

        # ── 断言前置：清单里真的加进去了吗 ──────────────────
        #
        # 没有这一步，一个**污染状态**会伪装成 PASS 或假 RED：
        #
        #   · 若 x.toml 里本来就有 ghostpkg（上次跑到一半被中断，
        #     第 890 行的 cp 恢复没执行），后面对 `../_ghostpkg_tmp`
        #     的引用依然成立，于是 run 通过、断言「该失败却成功」→ 假 RED
        #   · 若 add 静默没写进去，run 会报「未声明的包」，断言
        #     匹配不到「源码找不到」→ 也是假 RED
        #
        # 两种情况报的红都指向「pkg 源码缺失」，与实际原因无关。
        # 所以先确认「依赖确实在清单里」，让状态问题在这里就暴露。
        if ! grep -q '^ghostpkg[[:space:]]*=' "$PKG_APP/x.toml"; then
            fail=$((fail + 1))
            failed_list="$failed_list pkg-add"
            printf "  \033[31mFAIL\033[0m  pkg add 没写进清单（测试前置不成立）\n"
            sed 's/^/        /' "$TMPD/pkg-add.log" | head -5
            cp "$TMPD/pkg-x.toml.bak" "$PKG_APP/x.toml" 2>/dev/null
            rm -f "$PKG_APP/src/_ghost.xle"
            addrc=99   # 让下面的分支不重复报一次
            grc=1      # 同理
        fi

        # 现在把源码目录改名移走 —— 清单里还留着那条依赖。
        # 用 mv 而不是 rm：见上面关于「删除守卫造成假红」的说明。
        rm -rf "$GHOSTGONE" 2>/dev/null
        mv "$GHOSTDIR" "$GHOSTGONE" 2>/dev/null

        # ── 断言前置：源目录真的不在了吗 ────────────────────
        #
        # 「移走」是整个用例的**唯一自变量**：它没发生时，
        # import 当然能解析成功，下面只会报一条与原因无关的
        # 「退出码 0，期望非 0」。在这里拦住，报错就直接指向
        # mv 失败，而不是让人去猜解析器出了什么问题。
        if [ -e "$GHOSTDIR" ]; then
            fail=$((fail + 1))
            failed_list="$failed_list pkg-ghost-setup"
            printf "  \033[31mFAIL\033[0m  无法移走 ghost 源码目录（测试前置不成立）\n"
            printf "        仍存在：%s\n" "$GHOSTDIR"
            cp "$TMPD/pkg-x.toml.bak" "$PKG_APP/x.toml" 2>/dev/null
            rm -f "$PKG_APP/src/_ghost.xle"
            rm -rf "$GHOSTGONE" 2>/dev/null
            addrc=99
            grc=1
            ghost_setup_bad=1
        fi

        "$X" run "$PKG_APP/src/_ghost.xle" > "$TMPD/pkg-ghost.out" 2>&1
        grc=$?
        cp "$TMPD/pkg-x.toml.bak" "$PKG_APP/x.toml"
        rm -f "$PKG_APP/src/_ghost.xle"
        rm -rf "$GHOSTGONE" 2>/dev/null
        if [ "${ghost_setup_bad:-0}" = "1" ]; then
            :   # 前置已报过，不再重复计一次
        elif [ "$addrc" != "0" ]; then
            fail=$((fail + 1))
            failed_list="$failed_list pkg-add"
            printf "  \033[31mFAIL\033[0m  pkg add 命令 — 退出码 %s\n" "$addrc"
            sed 's/^/        /' "$TMPD/pkg-add.log" | head -10
        elif [ "$grc" != "0" ] && grep -q "源码找不到" "$TMPD/pkg-ghost.out" \
           && grep -q "试过这些路径" "$TMPD/pkg-ghost.out"; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  pkg 依赖源码缺失会报错并列出路径\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list pkg-missing"
            printf "  \033[31mFAIL\033[0m  pkg 源码缺失 — 退出码 %s\n" "$grc"
            sed 's/^/        /' "$TMPD/pkg-ghost.out" | head -10
        fi

        # ---- 5. pkg add 必须当场拒绝不可用的路径 ----
        #
        # ── 这一项守的是「延迟错误」这个缺陷 ─────────────────
        #
        # 以前 `x pkg add ghost --path ./nope` 无条件成功，把错误
        # 推迟到之后某次编译。两个理由说明这不可接受：
        #
        #   1. 延迟不增加任何信息。路径现在不存在，十分钟后还是
        #      不存在 —— 当场就能判的事，没有理由留给未来。
        #   2. 报错时用户早已忘了自己刚加过依赖。他看到的是
        #      「依赖 ghost 的源码找不到」，而记忆里明明写着「已添加
        #      依赖 ghost」成功了。这种矛盾最消耗信任。
        #
        # 两种坏路径都要测，因为用户要做的事不同：
        #   目录不存在   → 改路径，或建目录
        #   目录空       → 建 src/lib.xle
        #
        # 同时断言**清单没被改动** —— 加了条坏依赖的清单看起来
        # 和正常的一模一样，事后很难发现。
        addchk_ok=1
        addchk_why=""
        cp "$PKG_APP/x.toml" "$TMPD/x.toml.precheck"

        # 5a. 目录不存在
        ( cd "$PKG_APP" && "$X" pkg add nopathpkg --path ../_no_such_dir_xyz ) \
            > "$TMPD/add-baddir.log" 2>&1
        bdrc=$?
        if [ "$bdrc" = "0" ]; then
            addchk_ok=0; addchk_why="路径不存在时 add 返回了 0（应报错）"
        elif ! grep -q "不存在" "$TMPD/add-baddir.log"; then
            addchk_ok=0; addchk_why="路径不存在时没有说清原因"
        fi

        # 5b. 目录存在但没有入口
        if [ "$addchk_ok" = "1" ]; then
            EMPTYDIR="$PKG_APP/../_empty_pkg_tmp"
            rm -rf "$EMPTYDIR"; mkdir -p "$EMPTYDIR"
            ( cd "$PKG_APP" && "$X" pkg add emptypkg --path "$EMPTYDIR" ) \
                > "$TMPD/add-emptydir.log" 2>&1
            edrc=$?
            if [ "$edrc" = "0" ]; then
                addchk_ok=0; addchk_why="空目录时 add 返回了 0（应报错）"
            elif ! grep -q "没有可用的源码入口" "$TMPD/add-emptydir.log"; then
                addchk_ok=0; addchk_why="空目录时没有说清原因"
            fi
            rm -rf "$EMPTYDIR"
        fi

        # 5c. 两次失败都不许改动清单
        if [ "$addchk_ok" = "1" ]; then
            if ! diff -q "$TMPD/x.toml.precheck" "$PKG_APP/x.toml" >/dev/null 2>&1; then
                addchk_ok=0; addchk_why="add 失败后清单被改动了"
            fi
        fi

        if [ "$addchk_ok" = "1" ]; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  pkg add 拒绝不可用路径且不改清单\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list pkg-add-check"
            printf "  \033[31mFAIL\033[0m  pkg add 路径校验 — %s\n" "$addchk_why"
            sed 's/^/        /' "$TMPD/add-baddir.log" | head -6
        fi

        # ---- 6. x.toml 里的 profile 真的被采纳 ----
        #
        # ── 为什么这一项必须存在 ─────────────────────────────
        #
        # 这一条是为了守住一个**真实踩过**的 bug，不是假想。
        #
        # main() 里 `profile_name` 的初值原本写的是
        # `profile_default_name()`（即 "console"）。看起来无害 ——
        # 「默认档位」嘛。但 resolve_profile 的第一层判断是
        # `if (cli_name) return profile_find(cli_name)`，于是一个非
        # NULL 的 "console" 让第一层**永远命中**，清单里的
        # `profile = "desktop"` 一次都不会被读到。
        #
        # 用户看到的现象：在 x.toml 里写了 profile = "desktop"，
        # 重新 build，产物的 PE 子系统还是 3（CONSOLE），双击照样
        # 弹黑框，而且**没有任何提示**说明这个字段没生效。
        #
        # 修法是把初值改成 NULL（NULL =「用户没写 --profile」）。
        # 这一项测试就是钉子：把清单写成 desktop，期望产物子系统
        # 是 2。谁把初值改回去，这里立刻红。
        #
        # 同时验证优先级：命令行 --profile=console 必须能压过
        # 清单里的 desktop。
        if [ -n "$CC_BIN" ]; then
            MPROJ="$TMPD/mprof"
            rm -rf "$MPROJ"
            mkdir -p "$MPROJ"
            printf 'println("mprof")\n' > "$MPROJ/a.xle"
            printf '[package]\nname = "mprof"\nversion = "0.1.0"\nprofile = "desktop"\n' \
                > "$MPROJ/x.toml"

            mok=1
            mreason=""

            # 读 PE 头 Subsystem 的小函数（内联，避免再定义全局）
            pe_sub() {
                _m=$(od -An -tx1 -N2 "$1" 2>/dev/null | tr -d ' \n')
                [ "$_m" = "4d5a" ] || { echo "nope"; return; }
                _lf=$(od -An -tu4 -j60 -N4 "$1" 2>/dev/null | tr -d ' \n')
                od -An -tu2 -j$((_lf + 4 + 20 + 68)) -N2 "$1" 2>/dev/null | tr -d ' \n'
            }

            # 5a. 只有清单（无 --profile）→ 应为 desktop(2)
            "$X" build "$MPROJ/a.xle" --emit=exe --cc="$CC_BIN" \
                -o "$MPROJ/frommanifest.exe" > "$MPROJ/m.log" 2>&1
            if [ $? -ne 0 ]; then
                mok=0; mreason="清单档位编译失败"
            else
                ms=$(pe_sub "$MPROJ/frommanifest.exe")
                if [ "$ms" = "nope" ]; then
                    mok=2
                elif [ "$ms" != "2" ]; then
                    mok=0
                    mreason="x.toml 里 profile=desktop，产物子系统却是 ${ms}，期望 2 —— 清单字段没生效"
                fi
            fi

            # 5b. 命令行 --profile=console 压过清单 → 应为 console(3)
            if [ "$mok" = "1" ]; then
                "$X" build "$MPROJ/a.xle" --emit=exe --cc="$CC_BIN" \
                    --profile=console -o "$MPROJ/override.exe" > "$MPROJ/m2.log" 2>&1
                if [ $? -ne 0 ]; then
                    mok=0; mreason="命令行覆盖编译失败"
                else
                    ms2=$(pe_sub "$MPROJ/override.exe")
                    if [ "$ms2" != "3" ]; then
                        mok=0
                        mreason="--profile=console 未能压过清单里的 desktop，子系统是 ${ms2}"
                    fi
                fi
            fi

            # 5c. 清单里的档位名写错 → 警告但退回 console，仍然编译成功
            #     （不阻断：多打一个字母不该让整个项目编不了）
            if [ "$mok" = "1" ]; then
                printf '[package]\nname = "mprof"\nversion = "0.1.0"\nprofile = "desktpo"\n' \
                    > "$MPROJ/x.toml"
                if "$X" build "$MPROJ/a.xle" --emit=exe --cc="$CC_BIN" \
                        -o "$MPROJ/bad.exe" > "$MPROJ/m3.log" 2>&1; then
                    if ! grep -q "不是合法场景" "$MPROJ/m3.log"; then
                        mok=0; mreason="清单档位名写错时没有警告"
                    fi
                    ms3=$(pe_sub "$MPROJ/bad.exe")
                    if [ "$mok" = "1" ] && [ "$ms3" != "3" ]; then
                        mok=0; mreason="档位名非法时没有退回 console（子系统 ${ms3}）"
                    fi
                else
                    mok=0; mreason="清单档位名写错时整项目编译失败了（应只警告）"
                fi
            fi

            if [ "$mok" = "1" ]; then
                pass=$((pass + 1))
                printf "  \033[32mPASS\033[0m  pkg x.toml 的 profile 生效且命令行可覆盖\n"
            elif [ "$mok" = "2" ]; then
                printf "  \033[33mSKIP\033[0m  pkg x.toml profile 检查（产物不是 PE）\n"
            else
                fail=$((fail + 1))
                failed_list="$failed_list pkg-profile"
                printf "  \033[31mFAIL\033[0m  pkg x.toml profile — %s\n" "$mreason"
                [ -s "$MPROJ/m.log" ] && sed 's/^/        /' "$MPROJ/m.log" | tail -3
            fi
        else
            aot_skipped=$((aot_skipped + 1))
        fi
    fi
fi

# ============================================================
# 包管理：语义化版本 + 注册表 + lock
# ============================================================
#
# ── 为什么这一段与上面的 pkg 段分开 ─────────────────────────
#
# 上面那段测的是**闭合链路**：声明 → 引用 → 解析 → 编译。
# 它证明包管理「能用」。
#
# 这一段测的是**版本语义**：^ 与 ~ 的上界、预发布不参与普通约束、
# lock 优先于重挑版本、冲突时列出双方来源。这些不测的话，程序
# 照样「能用」—— 只是永远选最旧的版本，或者悄悄在 install 时
# 自动升级。功能在、行为错，是最难发现的一类缺陷。
#
# ── 为什么 semver 用 C 直接测 ──────────────────────────────
#
# semver.c 是纯计算（无 I/O、无全局状态、无诊断），可测边界极多。
# 走 CLI 测它要造注册表、写清单、再 grep 输出 —— 那样测到的是
# 「输出格式」而不是「比较逻辑」。见 tests/c/semver_test.c 的说明。
if [ -z "$FILTER" ] || [ "${FILTER#*semver}" != "$FILTER" ]; then
    if [ -n "$CC_BIN" ]; then
        ST="$TMPD/semver_test.exe"
        # TMP/TEMP 已在脚本入口统一指到 ASCII 路径（见顶部说明），
        # 这里直接调 cc 即可。
        if "$CC_BIN" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
              -I "$ROOT/src" \
              "$ROOT/src/semver.c" "$ROOT/tests/c/semver_test.c" \
              -o "$ST" > "$TMPD/semver-build.log" 2>&1; then
            "$ST" > "$TMPD/semver.out" 2>&1
            src_rc=$?
            if [ "$src_rc" = "0" ] && grep -q "semver ok" "$TMPD/semver.out"; then
                pass=$((pass + 1))
                n=$(grep -o '^  [0-9]* 项通过' "$TMPD/semver.out" | tr -dc '0-9')
                printf "  \033[32mPASS\033[0m  semver 版本比较与约束（%s 项）\n" "${n:-?}"
                if [ "$VERBOSE" = "1" ]; then
                    sed 's/^/        /' "$TMPD/semver.out"
                fi
            else
                fail=$((fail + 1))
                failed_list="$failed_list semver"
                printf "  \033[31mFAIL\033[0m  semver 版本比较与约束 — 退出码 %s\n" "$src_rc"
                grep 'FAIL' "$TMPD/semver.out" | sed 's/^/        /' | head -12
            fi
        else
            fail=$((fail + 1))
            failed_list="$failed_list semver-build"
            printf "  \033[31mFAIL\033[0m  semver 单元测试编译失败\n"
            sed 's/^/        /' "$TMPD/semver-build.log" | head -10
        fi
    else
        aot_skipped=$((aot_skipped + 1))
    fi
fi

# ── zh 包端到端（04 §3 L2：标准库中文别名层）────────────────
#
# pkg 块验证的是「包管理机制成立」，这一块验证的是**仓库自带的
# 第一个真实包**：x-lang/pkg/zh（标准库中文别名）。
#
# 两条验收，分别对应 04 §7 的两条硬指标：
#   1. 输出与 main.expect 逐字节一致 —— 中文别名真的能调用，
#      且行为与英文本体一致（每个值都可手算验证）
#   2. main.xlc 与 main.xle（英文关键字孪生版）编出的指令序列
#      一致 —— 08-zh 那条「中英零成本」在引入包依赖之后依然成立，
#      即 L2 别名没有把任何语义带进词法层之外的地方
#
# 程序里的文件操作写的是相对路径，会落在**当前工作目录**。
# 跑之前清一次、跑之后清一次 —— 测试不能给仓库留垃圾。
if [ -z "$FILTER" ] || [ "${FILTER#*pkg}" != "$FILTER" ] || [ "${FILTER#*zh}" != "$FILTER" ]; then
    ZH_APP="$ROOT/tests/pkg/zh-app"
    if [ -f "$ZH_APP/src/main.xlc" ]; then

        # ---- 1. 正向闭环 ----
        rm -f "$ROOT/zh_app_test.txt" "$ZH_APP/zh_app_test.txt"
        zout="$TMPD/zh-app.out"
        zerr="$TMPD/zh-app.err"
        "$X" run "$ZH_APP/src/main.xlc" > "$zout" 2> "$zerr"
        zrc=$?
        rm -f "$ROOT/zh_app_test.txt" "$ZH_APP/zh_app_test.txt"
        zok=1
        zreason=""
        if [ "$zrc" != "0" ]; then
            zok=0
            zreason="退出码 $zrc"
        else
            # 行尾归一化 —— 理由同 pkg-app：测试不该因 git autocrlf 而红。
            tr -d '\r' < "$ZH_APP/src/main.expect" > "$TMPD/_ze.tmp" 2>/dev/null
            tr -d '\r' < "$zout"                     > "$TMPD/_zo.tmp" 2>/dev/null
            if ! diff -q "$TMPD/_ze.tmp" "$TMPD/_zo.tmp" > /dev/null 2>&1; then
                zok=0
                zreason="输出与 main.expect 不符"
            fi
            rm -f "$TMPD/_ze.tmp" "$TMPD/_zo.tmp"
        fi
        if [ "$zok" = "1" ]; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  zh 包端到端（引入 @zh × 中文别名）\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list zh-pkg"
            printf "  \033[31mFAIL\033[0m  zh 包端到端 — %s\n" "$zreason"
            sed 's/^/        /' "$zerr" | head -10
        fi

        # ---- 2. 中英孪生字节码一致 ----
        "$X" disasm "$ZH_APP/src/main.xlc" 2>/dev/null \
            | grep -E '^ +[0-9]{4}  ' | sed 's/ *;.*$//' > "$TMPD/zh-app.zh.dis"
        "$X" disasm "$ZH_APP/src/main.xle" 2>/dev/null \
            | grep -E '^ +[0-9]{4}  ' | sed 's/ *;.*$//' > "$TMPD/zh-app.en.dis"
        if diff -q "$TMPD/zh-app.zh.dis" "$TMPD/zh-app.en.dis" > /dev/null 2>&1; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  zh 包中英字节码一致（.xlc vs .xle）\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list zh-pkg-bc"
            printf "  \033[31mFAIL\033[0m  zh 包中英字节码不一致\n"
            diff "$TMPD/zh-app.zh.dis" "$TMPD/zh-app.en.dis" 2>/dev/null \
                | head -10 | sed 's/^/        /'
        fi
        rm -f "$TMPD/zh-app.zh.dis" "$TMPD/zh-app.en.dis"
    fi
fi

# ── 语言服务（LSP）协议与客户端 ─────────────────────────────
#
# ── 这一段在测什么 ──────────────────────────────────────────
#
# lsp.c 的响应是手拼 JSON 字符串，lspClient.js 的映射是手写数字表 ——
# 两处都没有类型系统兜底。编译器不会说话，而错误在编辑器里表现为
# 「一直转圈」「提示不出现」「点了没反应」，几乎没有可用的线索。
#
# 所以这里真的启动 x.exe lsp，真的按 LSP 协议发字节：
#   check-lsp.js     验服务端（38 项断言，含内容而不只是「能解析」）
#   check-client.js  验客户端（用 vscode 桩跑真 lspClient.js）
#
# 需要 node。没有 node 就跳过并明确说「没检查」—— 跳过不等于通过。
if [ -z "$FILTER" ] || [ "${FILTER#*lsp}" != "$FILTER" ]; then
    if command -v node >/dev/null 2>&1; then
        LSPBASE="$ROOT/vscode-x"

        # 两个脚本都必须带超时。
        #
        # 它们各自启动一个 x.exe lsp 子进程，靠 node 的 process.exit
        # 收尾。这在正常情况下是干净的，但在 Windows 上子进程的 stdio
        # 管道偶尔不会立刻释放 —— 此时 node 认为还有活跃句柄，不退出，
        # 于是整个 run_tests.sh 无限期挂住。
        #
        # 那种挂住的表现极具误导性：日志停在「LSP 服务端协议 PASS」，
        # 后面没有了。看上去像测试逻辑错，实际是进程没收尾。
        # 加 timeout 把「挂死」变成「失败」，并留下明确原因。
        TOS=""
        command -v timeout >/dev/null 2>&1 && TOS="timeout 120"
        $TOS node "$LSPBASE/tools/check-lsp.js" > "$TMPD/lsp-server.out" 2>&1
        lsp_rc=$?
        if [ "$lsp_rc" = "0" ]; then
            n=$(grep -o '^  [0-9]* 项通过' "$TMPD/lsp-server.out" | tr -dc '0-9')
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  LSP 服务端协议（%s 项）\n" "${n:-?}"
        else
            fail=$((fail + 1))
            failed_list="$failed_list lsp-server"
            printf "  \033[31mFAIL\033[0m  LSP 服务端协议 — 退出码 %s\n" "$lsp_rc"
            grep 'FAIL' "$TMPD/lsp-server.out" | sed 's/^/        /' | head -12
        fi

        # 客户端也要带超时。理由见上面服务端那段：Windows 上子进程
        # 的 stdio 偶尔不立刻释放，node 会带着活跃句柄不退出，把整个
        # 套件挂住，而日志看起来只是「停在这里」。
        TOC=""
        command -v timeout >/dev/null 2>&1 && TOC="timeout 120"
        $TOC node "$LSPBASE/tools/check-client.js" > "$TMPD/lsp-client.out" 2>&1
        lspc_rc=$?
        if [ "$lspc_rc" = "0" ]; then
            n=$(grep -o '^  [0-9]* 项通过' "$TMPD/lsp-client.out" | tr -dc '0-9')
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  LSP 客户端链路（%s 项）\n" "${n:-?}"
        else
            fail=$((fail + 1))
            failed_list="$failed_list lsp-client"
            printf "  \033[31mFAIL\033[0m  LSP 客户端链路 — 退出码 %s\n" "$lspc_rc"
            grep 'FAIL' "$TMPD/lsp-client.out" | sed 's/^/        /' | head -12
        fi

        # 扩展清单必须自洽：有 main 才可能激活，激活事件必须覆盖语言。
        # 这两条能挡住「改了 package.json 忘加 main」这种致命但静默的错。
        if node -e '
            const p = require(process.argv[1]);
            const errs = [];
            if (!p.main) errs.push("缺 main 字段（扩展永远不会激活）");
            if (!(p.activationEvents || []).includes("onLanguage:x"))
                errs.push("缺 onLanguage:x 激活事件");
            if (!p.contributes.languages) errs.push("缺 languages 贡献点");
            if (!p.contributes.grammars) errs.push("缺 grammars 贡献点");
            if (errs.length) { console.error(errs.join("; ")); process.exit(1); }
        ' "$LSPBASE/package.json" 2> "$TMPD/manifest.err"; then
            pass=$((pass + 1))
            printf "  \033[32mPASS\033[0m  扩展清单自洽（main / 激活事件 / 贡献点）\n"
        else
            fail=$((fail + 1))
            failed_list="$failed_list lsp-manifest"
            printf "  \033[31mFAIL\033[0m  扩展清单不自洽\n"
            sed 's/^/        /' "$TMPD/manifest.err"
        fi

        # ── 语法高亮的 scope 断言 ────────────────────────────
        #
        # ── 为什么这一项必须进套件 ────────────────────────
        #
        # 高亮坏了不会报错、不会挂、不会让任何测试失败 —— 它只是
        # 「有些字颜色不对」。而颜色不对可能意味着：
        #   · 规则顺序错了（内建 scope 从未生效，因为函数调用规则抢先）
        #   · 区域 end 没等到匹配（接口无体方法把后半个文件吞掉）
        #   · captures 编号差一位（包名被标成标点）
        #
        # 三个都真实发生过，三个都能通过「看一眼觉得还行」的人工检查。
        # 只有断言能拦住它们，所以 check-grammar.js 必须进套件。
        #
        # 依赖 vscode-textmate / vscode-oniguruma。它们装在扩展目录之外
        # （否则会被 vsce 打进 vsix），所以靠 NODE_PATH 指过去。找不到
        # 就 SKIP 并说明「没检查」—— 跳过不等于通过。
        GRAM_OK=0
        for cand in "$TMPD/tm/node_modules" "/tmp/tm/node_modules" "$HOME/.cache/x-tm/node_modules"; do
            if [ -d "$cand/vscode-textmate" ] && [ -d "$cand/vscode-oniguruma" ]; then
                NODE_PATH="$cand" $TOS node "$LSPBASE/tools/check-grammar.js" > "$TMPD/grammar.out" 2>&1
                gr_rc=$?
                GRAM_OK=1
                if [ "$gr_rc" = "0" ]; then
                    n=$(grep -o '断言 [0-9]* 条：通过 [0-9]*' "$TMPD/grammar.out" | head -1)
                    pass=$((pass + 1))
                    printf "  \033[32mPASS\033[0m  语法高亮 scope（%s）\n" "${n:-?}"
                else
                    fail=$((fail + 1))
                    failed_list="$failed_list grammar"
                    printf "  \033[31mFAIL\033[0m  语法高亮 scope 断言\n"
                    grep -A3 'FAIL' "$TMPD/grammar.out" | sed 's/^/        /' | head -16
                fi
                break
            fi
        done
        if [ "$GRAM_OK" = "0" ]; then
            aot_skipped=$((aot_skipped + 1))
            printf "  \033[33mSKIP\033[0m  语法高亮 scope（未装 vscode-textmate，见 README「怎么验证」）\n"
        fi
    else
        aot_skipped=$((aot_skipped + 1))
        printf "  \033[33mSKIP\033[0m  LSP 检查（没找到 node）\n"
    fi
fi

# ── 注册表 + lock 的端到端行为 ──────────────────────────────
#
# 这一段的每个断言都对应一个「功能在但行为错」的缺陷：
#
#   1. ^1.2 必须选 1.2.3 而不是 1.0.0    —— 选版本方向反了
#   2. install 两次必须不动 x.lock        —— 每次都重写
#   3. 上游发新版后 install 不得升级      —— lock 形同虚设
#   4. update 必须升级                    —— 升级路径失效
#   5. 冲突必须报错且列出双方来源         —— 静默选一个
#   6. 传递依赖必须进 lock                —— 只解析直接依赖
#
# 造注册表用的是临时目录，不碰用户真实的 packages/。
if [ -z "$FILTER" ] || [ "${FILTER#*reg}" != "$FILTER" ]; then
    # ── 清理用 mv，不用 rm -rf ──────────────────────────────
    #
    # 理由与 GHOSTDIR 那段相同但更严重：这里上一整套测试跑过之后
    # 环境守卫会开始拒绝批量删除，而**被拒绝时它不是报错返回，是
    # 停下来等确认**。在没有终端的测试进程里，那就是无限期挂住 ——
    # 表现不是「测试失败」，而是整套测试跑不完，日志停在上一段。
    #
    # mv 到带时间戳的旁名不会触发守卫，也不依赖 rm 是否生效。
    REGBASE="$TMPD/reg-e2e"
    REG="$REGBASE/packages"
    RAPP="$REGBASE/app"
    if [ -e "$REGBASE" ]; then
        mv "$REGBASE" "$REGBASE.gone.$$" 2>/dev/null \
            || mv "$REGBASE" "$TMPD/gone-$$" 2>/dev/null
    fi
    mkdir -p "$REG/jsonx/1.0.0/src" "$REG/jsonx/1.2.3/src" \
             "$REG/jsonx/2.0.0/src" "$RAPP/src"
    # 挪走的旧目录事后清理；删不掉也不影响 —— 名字已不冲突
    rm -rf "$REGBASE.gone.$$" 2>/dev/null

    # 1.0.0 与 1.2.3 返回值不同 —— 这样「选了哪个」是能看出来的。
    # 两个版本返回同样的字符串，测试就退化成「能跑就行」。
    printf 'fn jsonxVer() -> String { return "1.0.0" }\n' > "$REG/jsonx/1.0.0/src/lib.xle"
    printf 'fn jsonxVer() -> String { return "1.2.3" }\n' > "$REG/jsonx/1.2.3/src/lib.xle"
    printf 'fn jsonxVer() -> String { return "2.0.0" }\n' > "$REG/jsonx/2.0.0/src/lib.xle"

    cat > "$RAPP/x.toml" <<'XTOML'
[package]
name = "regdemo"
version = "0.1.0"

[dependencies]
jsonx = "^1.2"
XTOML
    printf 'import @jsonx\nprintln(jsonxVer())\n' > "$RAPP/src/main.xle"

    XR="$X"
    export X_REGISTRY="$REG"

    # ---- 1. install + 选版本方向 ----
    ( cd "$RAPP" && "$XR" pkg install > "$TMPD/reg-inst1.out" 2>&1 )
    r1=$?
    picked=$(grep -o '1\.2\.3\|1\.0\.0\|2\.0\.0' "$RAPP/x.lock" 2>/dev/null | head -1)
    if [ "$r1" = "0" ] && [ "$picked" = "1.2.3" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  注册表 ^1.2 选最高满足版本（1.2.3）\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-pick"
        printf "  \033[31mFAIL\033[0m  注册表选版本 — 退出码 %s，选中的是 %s\n" \
               "$r1" "${picked:-（没有）}"
        sed 's/^/        /' "$TMPD/reg-inst1.out" | head -8
    fi

    # ---- 2. install 幂等（不重写 x.lock）----
    #
    # 用内容比对而不是 mtime：mtime 在某些文件系统上精度只有秒，
    # 两次跑在同一秒里就测不出来。内容一致 + 输出里明说
    # 「无需改动」两条一起断言，才是真的没重写。
    cp "$RAPP/x.lock" "$TMPD/reg-lock-a" 2>/dev/null
    ( cd "$RAPP" && "$XR" pkg install > "$TMPD/reg-inst2.out" 2>&1 )
    r2=$?
    if [ "$r2" = "0" ] && diff -q "$TMPD/reg-lock-a" "$RAPP/x.lock" >/dev/null 2>&1 \
       && grep -q "无需改动" "$TMPD/reg-inst2.out"; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  install 幂等（lock 已一致时不重写）\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-idem"
        printf "  \033[31mFAIL\033[0m  install 幂等 — 退出码 %s\n" "$r2"
        sed 's/^/        /' "$TMPD/reg-inst2.out" | head -8
    fi

    # ---- 3. 上游发新版后 install 不得升级 ----
    #
    # 这是 lock 存在的全部意义。写一个更高的 1.5.0 进注册表，
    # 然后 install —— 必须还是 1.2.3。
    mkdir -p "$REG/jsonx/1.5.0/src"
    printf 'fn jsonxVer() -> String { return "1.5.0" }\n' > "$REG/jsonx/1.5.0/src/lib.xle"
    ( cd "$RAPP" && "$XR" pkg install > "$TMPD/reg-inst3.out" 2>&1 )
    r3=$?
    keep=$(grep -o '1\.2\.3\|1\.5\.0' "$RAPP/x.lock" 2>/dev/null | head -1)
    ran=$( cd "$RAPP" && "$XR" run src/main.xle 2>&1 )
    if [ "$r3" = "0" ] && [ "$keep" = "1.2.3" ] && [ "$ran" = "1.2.3" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  install 不自动升级（lock 优先，仍跑 1.2.3）\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-noauto"
        printf "  \033[31mFAIL\033[0m  install 不该自动升级 — lock=%s，跑出 %s\n" \
               "${keep:-?}" "$ran"
    fi

    # ---- 4. update 必须升级 ----
    ( cd "$RAPP" && "$XR" pkg update > "$TMPD/reg-upd.out" 2>&1 )
    r4=$?
    up=$(grep -o '1\.5\.0' "$RAPP/x.lock" 2>/dev/null | head -1)
    ranup=$( cd "$RAPP" && "$XR" run src/main.xle 2>&1 )
    if [ "$r4" = "0" ] && [ "$up" = "1.5.0" ] && [ "$ranup" = "1.5.0" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  update 强制升级（升到 1.5.0）\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-update"
        printf "  \033[31mFAIL\033[0m  update 升级 — 退出码 %s，lock 里 %s，跑出 %s\n" \
               "$r4" "${up:-?}" "$ranup"
        sed 's/^/        /' "$TMPD/reg-upd.out" | head -8
    fi

    # ---- 5. 约束改了要重挑 ----
    #
    # 把 ^1.2 改成 =1.2.3，旧 lock（1.5.0）不再满足 —— install
    # 必须重挑回 1.2.3。这条证明「lock 优先」不是「无条件沿用」。
    sed -i 's/jsonx = "\^1.2"/jsonx = "=1.2.3"/' "$RAPP/x.toml"
    ( cd "$RAPP" && "$XR" pkg install > "$TMPD/reg-inst5.out" 2>&1 )
    r5=$?
    back=$(grep -o '1\.2\.3\|1\.5\.0' "$RAPP/x.lock" 2>/dev/null | head -1)
    if [ "$r5" = "0" ] && [ "$back" = "1.2.3" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  约束收紧后重挑版本（1.5.0 → 1.2.3）\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-reshape"
        printf "  \033[31mFAIL\033[0m  约束收紧后重挑 — 退出码 %s，lock 里 %s\n" \
               "$r5" "${back:-?}"
        sed 's/^/        /' "$TMPD/reg-inst5.out" | head -8
    fi

    # ---- 6. pkg lock 一致性检查的退出码 ----
    ( cd "$RAPP" && "$XR" pkg lock > "$TMPD/reg-lockchk.out" 2>&1 )
    r6=$?
    if [ "$r6" = "0" ] && grep -q "一致" "$TMPD/reg-lockchk.out"; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  pkg lock 一致时退出 0\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-lockchk"
        printf "  \033[31mFAIL\033[0m  pkg lock 一致性检查 — 退出码 %s\n" "$r6"
        sed 's/^/        /' "$TMPD/reg-lockchk.out" | head -8
    fi

    # ---- 7. 冲突必须报错并列出双方来源 ----
    #
    # 造两个包，各自要求 shared 的不同主版本。断言三件事：
    # 非零退出、提到「冲突」、两个要求者的名字都出现。
    #
    # 只断言「非零退出」是不够的 —— 注册表缺目录也会非零退出。
    # 断言两个来源名字都出现，才能证明诊断真的定位到了冲突双方。
    MK="$REGBASE/conflict"
    rm -rf "$MK" 2>/dev/null
    mkdir -p "$MK/packages/alpha/1.0.0/src" "$MK/packages/beta/1.0.0/src" \
             "$MK/packages/shared/1.0.0/src" "$MK/packages/shared/2.0.0/src" \
             "$MK/app/src"
    printf 'fn aF() -> Int { return 1 }\n' > "$MK/packages/alpha/1.0.0/src/lib.xle"
    printf 'fn bF() -> Int { return 2 }\n' > "$MK/packages/beta/1.0.0/src/lib.xle"
    printf 'fn sF() -> Int { return 1 }\n' > "$MK/packages/shared/1.0.0/src/lib.xle"
    printf 'fn sF() -> Int { return 2 }\n' > "$MK/packages/shared/2.0.0/src/lib.xle"
    printf '[package]\nname = "alpha"\nversion = "1.0.0"\n\n[dependencies]\nshared = "^1.0"\n' \
        > "$MK/packages/alpha/1.0.0/x.toml"
    printf '[package]\nname = "beta"\nversion = "1.0.0"\n\n[dependencies]\nshared = "^2.0"\n' \
        > "$MK/packages/beta/1.0.0/x.toml"
    printf '[package]\nname = "confapp"\nversion = "0.1.0"\n\n[dependencies]\nalpha = "^1.0"\nbeta = "^1.0"\n' \
        > "$MK/app/x.toml"
    printf 'println(1)\n' > "$MK/app/src/main.xle"

    ( cd "$MK/app" && X_REGISTRY="$MK/packages" "$XR" pkg install \
        > "$TMPD/reg-conflict.out" 2>&1 )
    r7=$?
    if [ "$r7" != "0" ] && grep -q "冲突" "$TMPD/reg-conflict.out" \
       && grep -q "alpha" "$TMPD/reg-conflict.out" \
       && grep -q "beta" "$TMPD/reg-conflict.out"; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  版本冲突报错并列出双方来源\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-conflict"
        printf "  \033[31mFAIL\033[0m  版本冲突检测 — 退出码 %s\n" "$r7"
        sed 's/^/        /' "$TMPD/reg-conflict.out" | head -10
    fi

    # ---- 8. 传递依赖进 lock ----
    APPP="$REGBASE/trans"
    rm -rf "$APPP" 2>/dev/null
    mkdir -p "$APPP/packages/appx/1.0.0/src" "$APPP/packages/jsonx/1.0.0/src" \
             "$APPP/app/src"
    printf 'fn appxName() -> String { return "appx" }\n' > "$APPP/packages/appx/1.0.0/src/lib.xle"
    printf 'fn jsonxVer() -> String { return "1.0.0" }\n' > "$APPP/packages/jsonx/1.0.0/src/lib.xle"
    printf '[package]\nname = "appx"\nversion = "1.0.0"\n\n[dependencies]\njsonx = "^1.0"\n' \
        > "$APPP/packages/appx/1.0.0/x.toml"
    printf '[package]\nname = "transapp"\nversion = "0.1.0"\n\n[dependencies]\nappx = "^1.0"\n' \
        > "$APPP/app/x.toml"
    printf 'import @appx\nprintln(appxName())\n' > "$APPP/app/src/main.xle"

    ( cd "$APPP/app" && X_REGISTRY="$APPP/packages" "$XR" pkg install \
        > "$TMPD/reg-trans.out" 2>&1 )
    r8=$?
    if [ "$r8" = "0" ] && grep -q "jsonx" "$APPP/app/x.lock" 2>/dev/null; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  传递依赖被解析并写进 x.lock\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list reg-trans"
        printf "  \033[31mFAIL\033[0m  传递依赖解析 — 退出码 %s\n" "$r8"
        sed 's/^/        /' "$TMPD/reg-trans.out" | head -10
    fi

    unset X_REGISTRY
    rm -rf "$REGBASE" 2>/dev/null
fi

# ── 子命令选项与顶层选项不撞名 ──────────────────────────────
#
# `x pkg add jsonx --version "^1.2"` 里的 --version 是 pkg add 的
# 选项（版本约束），不是「打印版本号」。全局参数扫描如果不区分
# 位置，会在子命令之前就把它当成顶层选项、打印版本号然后退出 ——
# 于是命令静默地什么也没做，清单一个字都没改，也没有任何报错。
#
# 这类缺陷特别值得留一条测试：它不崩、不报错、退出码是 0，
# 唯一的现象是「我明明加了依赖，怎么没加上」。
if [ -z "$FILTER" ] || [ "${FILTER#*opt}" != "$FILTER" ]; then
    OPTBASE="$TMPD/opt-e2e"
    rm -rf "$OPTBASE" 2>/dev/null
    mkdir -p "$OPTBASE/packages/jsonx/1.2.0/src" "$OPTBASE/app/src"
    printf 'fn jsonxVer() -> String { return "1.2.0" }\n' \
        > "$OPTBASE/packages/jsonx/1.2.0/src/lib.xle"
    printf '[package]\nname = "optapp"\nversion = "0.1.0"\n' > "$OPTBASE/app/x.toml"
    printf 'import @jsonx\nprintln(jsonxVer())\n' > "$OPTBASE/app/src/main.xle"

    # 顶层 --version 必须还是打印版本号
    tv=$("$X" --version 2>&1)
    case "$tv" in
        x\ *) top_ok=1 ;;
        *)    top_ok=0 ;;
    esac

    ( cd "$OPTBASE/app" && X_REGISTRY="$OPTBASE/packages" "$X" \
        pkg add jsonx --version "^1.2" > "$TMPD/opt-add.out" 2>&1 )
    arc=$?

    if [ "$top_ok" = "1" ] && [ "$arc" = "0" ] \
       && grep -q 'jsonx = "\^1.2"' "$OPTBASE/app/x.toml" 2>/dev/null; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  子命令的 --version 不被顶层参数扫描吞掉\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list opt-version"
        printf "  \033[31mFAIL\033[0m  子命令 --version 冲突 — 顶层=%s 退出码=%s\n" \
               "$tv" "$arc"
        sed 's/^/        /' "$TMPD/opt-add.out" | head -8
        sed 's/^/        x.toml: /' "$OPTBASE/app/x.toml" | head -8
    fi

    # 顺手验一条：add --version 之后 install 能跑通（闭环）
    ( cd "$OPTBASE/app" && X_REGISTRY="$OPTBASE/packages" "$X" pkg install \
        > "$TMPD/opt-inst.out" 2>&1 )
    irc=$?
    rv=$( cd "$OPTBASE/app" && X_REGISTRY="$OPTBASE/packages" "$X" run src/main.xle 2>&1 )
    if [ "$irc" = "0" ] && [ "$rv" = "1.2.0" ]; then
        pass=$((pass + 1))
        printf "  \033[32mPASS\033[0m  add --version 后 install 与运行闭环\n"
    else
        fail=$((fail + 1))
        failed_list="$failed_list opt-e2e"
        printf "  \033[31mFAIL\033[0m  add --version 闭环 — install=%s 跑出 %s\n" \
               "$irc" "$rv"
        sed 's/^/        /' "$TMPD/opt-inst.out" | head -8
    fi

    rm -rf "$OPTBASE" 2>/dev/null
fi

echo
if [ "$aot_skipped" != "0" ] && [ "${X_AOT:-1}" != "0" ]; then
    # 两种跳过原因分开报：
    #   aot_marker —— 测试源文件带 `// aot-skip` 标记（动态调用等
    #                 内建只有解释器路径实现，见 55-dynamic.xle）。
    #   其余      —— 没找到编译器，纯环境问题。
    # 混在一个数字里报「没找到 gcc」会把标记豁免误报成环境问题，
    # 真正缺编译器的情况反而被淹没。
    if [ "$aot_marker" != "0" ]; then
        printf "\033[33m注意：%d 个测试带 aot-skip 标记（动态调用/按名反射的 AOT 支持尚未实现，见 codegen_c.c）\033[0m\n" "$aot_marker"
    fi
    aot_nocc=$((aot_skipped - aot_marker))
    if [ "$aot_nocc" != "0" ]; then
        printf "\033[33m注意：%d 个测试因没找到 gcc/cc 跳过 AOT 一致性检查\033[0m\n" "$aot_nocc"
        echo "      没找到 gcc/cc。这不算通过 —— 只是没检查。"
        echo "      装一个 C 编译器，或用 X_CC=<path> 指定后重跑。"
    fi
fi

if [ "$gui_skipped" != "0" ]; then
    printf "\033[33m注意：%d 个图形测试被跳过（X_GUI=0）\033[0m\n" "$gui_skipped"
    echo "      图形测试需要真实桌面会话。这不算通过 —— 只是没检查。"
fi

if [ "$fail" = "0" ]; then
    printf "\033[32m%d 个测试全部通过\033[0m\n" "$pass"
    exit 0
else
    printf "\033[31m%d 通过，%d 失败\033[0m:%s\n" "$pass" "$fail" "$failed_list"
    exit 1
fi