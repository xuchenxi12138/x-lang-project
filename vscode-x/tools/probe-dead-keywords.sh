#!/bin/sh
# tools/probe-dead-keywords.sh — 实测关键字可用性，并与补全黑名单对账
#
# ── 这个脚本解决什么问题 ──────────────────────────────────────
#
# lexer.c 的 kKeywords 是**词法**表：它只判断「这个拼写是不是关键字」，
# 不判断「这个关键字能不能被用」。P0 阶段大量 token 已登记进词法表
# （设计文档里有、TokKind 枚举里有、中文别名也配了），但 parser.c 从未
# 消费过 —— 写成代码会报「期望一个表达式，实际读到 XXX」。
#
# 补全和高亮如果照词法表全量输出，就会推荐这些词。用户照着写、编译
# 失败，比没有补全更糟。
#
# gen-completions.js 里有一份 DEAD_KEYWORDS 黑名单。它的判据必须是**实测**，
# 不能是「看着像没实现」。这个脚本就是那个判据的来源，同时反向校验：
# 黑名单里写死的和实测结果必须逐项一致，多一个少一个都报 FAIL。
#
# 用法：
#   sh tools/probe-dead-keywords.sh
#   sh tools/probe-dead-keywords.sh -v     # 附上每个用例的首行报错

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
X="$ROOT/../build/x.exe"
D="${TMPDIR:-/tmp}/xprobe.$$"
mkdir -p "$D"

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

if [ ! -x "$X" ]; then
    echo "FAIL: 找不到 $X —— 先跑 ./tools/build.sh" >&2
    exit 1
fi

ACTUAL_DEAD=""

# probe <关键字名> <期望: ok|dead> <用例首行> [其余行...]
#
# 期望值写死在调用处，而不是「跑一下看结果」。理由：这个脚本的用途是
# **发现编译器变化**。如果它自己判定期望，那么编译器把 `super` 实现了，
# 脚本会安静地把 super 划到「可用」——而 gen-completions.js 的黑名单
# 还留着它，补全于是永远不推荐 super。期望值必须是人写下的断言。
probe() {
    kw="$1"; want="$2"; shift 2
    f="$D/$kw.xle"
    : > "$f"
    for line in "$@"; do printf '%s\n' "$line" >> "$f"; done
    out=$("$X" check "$f" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then got=ok; else got=dead; fi

    if [ "$got" = "$want" ]; then
        printf "  %-12s %-4s ok\n" "$kw" "$got"
    else
        printf "  %-12s %-4s FAIL（期望 %s）\n" "$kw" "$got" "$want"
        printf '%s\n' "$out" | head -3 | sed 's/^/      /'
        MISMATCH=1
    fi
    [ "$got" = "dead" ] && ACTUAL_DEAD="$ACTUAL_DEAD $kw"
    if [ $VERBOSE -eq 1 ] && [ "$got" = "dead" ]; then
        printf '%s\n' "$out" | head -1 | cut -c1-92 | sed 's/^/      /'
    fi
}

MISMATCH=0

echo "实测关键字可用性"
echo "  $X"
echo ""

echo "── 类系统（v0.15 / v0.16 实现，必须可用）──"
probe class       ok   'class A { x: Int }' 'fn main() { }'
probe interface   ok   'interface Shape { fn area(self) -> Float }' 'fn main() { }'
probe abstract    ok   'abstract class A { }' 'fn main() { }'
probe final       ok   'final class A { }' 'fn main() { }'
probe extends     ok   'class A { }' 'class B extends A { }' 'fn main() { }'
probe implements  ok   'interface I { fn f(self) -> Int }' 'class A implements I { fn f(self) -> Int { return 1 } }' 'fn main() { }'
# override 曾经被我误判成死 token —— 原因是用例本身写错了：
# 父类没有同名方法，报的是「没有同名方法可覆写」（语义检查），
# 而不是「不认识 override」（语法检查）。区分这两者要看报错文本：
# 语法层的报错一律是「期望 X，实际读到 override」。
probe override    ok   'class A { fn f(self) -> Int { return 1 } }' 'class B extends A { override fn f(self) -> Int { return 2 } }' 'fn main() { }'
probe private     ok   'class A { private x: Int }' 'fn main() { }'
probe protected   ok   'class A { protected x: Int }' 'fn main() { }'
probe public      ok   'class A { public x: Int }' 'fn main() { }'
probe new         ok   'class A { }' 'fn main() { let a = new A() }'
probe self        ok   'class A { fn f(self) -> Int { return 1 } }' 'fn main() { }'
probe struct      ok   'struct P { x: Int }' 'fn main() { }'
probe fn          ok   'fn main() { }'
probe let         ok   'fn main() { let x = 1 }'
probe match       ok   'fn main() { let x = 1 let s = match x { 1 => "a", _ => "b" } }'

echo ""
echo "── 死 token（词法登记了，语法层不认）──"
probe enum        dead 'enum Color { Red, Green }' 'fn main() { }'
probe trait       dead 'trait Show { fn show(self) -> String; }' 'fn main() { }'
probe impl        dead 'struct P { x: Int }' 'impl P { fn f(self) -> Int { return 1 } }' 'fn main() { }'
probe type        dead 'type MyInt = Int' 'fn main() { }'
probe mod         dead 'mod foo { }' 'fn main() { }'
probe case        dead 'fn main() { let x = 1 match x { case 1 { } } }'
probe do          dead 'fn main() { do { } }'
probe catch       dead 'fn main() { do { } catch e { } }'
probe spawn       dead 'fn main() { spawn f() }'
probe dyn         dead 'fn f(x: dyn Show) { }' 'fn main() { }'
probe where       dead 'fn f(x: Int) where { }' 'fn main() { }'
probe unsafe      dead 'fn main() { unsafe { } }'
probe comptime    dead 'fn main() { comptime { } }'
probe extern      dead 'extern fn puts(s: String) -> Int' 'fn main() { }'
probe yield       dead 'fn gen() -> Int { yield 1 }' 'fn main() { }'
probe mut         dead 'fn main() { mut let x = 1 }'
probe ref         dead 'fn f(ref x: Int) { }' 'fn main() { }'
probe par         dead 'fn main() { par { } }'
# super —— 最容易误判的一个。
#
# 三处都「写着存在」：lexer 英文表有 super、中文表有 `父类` → TK_SUPER、
# x.h 有 TokKind。但 parser.c 里 TK_SUPER 出现 0 次，super() 报
# 「期望一个表达式，实际读到 super」。词法登记 ≠ 语法实现，而这两件事
# 在 lexer.c 里是挨着写的，扫一眼很容易看错。
probe super       dead 'class A { fn f(self) -> Int { return 1 } }' 'class B extends A { fn f(self) -> Int { return super() } }' 'fn main() { }'

# ── 与 gen-completions.js 的黑名单对账 ────────────────────────
echo ""
GEN="$ROOT/tools/gen-completions.js"
if [ -f "$GEN" ]; then
    # 从生成器里抽出 DEAD_KEYWORDS 的内容（两个 Set([...]) 之间的引号词）
    BLACK=$(sed -n '/const DEAD_KEYWORDS = new Set(\[/,/\]);/p' "$GEN" \
            | tr -d '\n' | sed 's/.*Set(\[//; s/\].*//' \
            | tr -d " '\"" | tr ',' '\n' | sed '/^$/d' | sort)
    MEASURED=$(printf '%s\n' "$ACTUAL_DEAD" | tr ' ' '\n' | sed '/^$/d' | sort)

    if [ "$BLACK" = "$MEASURED" ]; then
        n=$(printf '%s\n' "$BLACK" | sed '/^$/d' | wc -l)
        echo "黑名单对账：一致（$n 项）"
    else
        echo "黑名单对账：**不一致**"
        echo "  只在 gen-completions.js 里："
        printf '%s\n' "$BLACK" | comm -23 - "$MEASURED" 2>/dev/null | sed 's/^/    /'
        echo "  只在实测结果里："
        printf '%s\n' "$MEASURED" | comm -13 - "$BLACK" 2>/dev/null | sed 's/^/    /'
        MISMATCH=1
    fi
else
    echo "跳过对账：找不到 $GEN"
fi

echo ""
if [ $MISMATCH -ne 0 ]; then
    echo "结论：**有变化**。编译器能力变了，去同步 gen-completions.js 的"
    echo "      DEAD_KEYWORDS 和 syntaxes/x.tmLanguage.json，然后重跑生成。"
    rm -rf "$D" 2>/dev/null || true
    exit 1
fi
echo "结论：全部符合预期。"
rm -rf "$D" 2>/dev/null || true
exit 0