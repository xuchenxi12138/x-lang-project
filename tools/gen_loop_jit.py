#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 src/loop_jit_gen.h —— `lf_run2()` 的函数体（双算子热循环展开）。

── 为什么需要展开，而不是继续用 reg[] ─────────────────────────

第四~六轮的 `lf_run_int` 已经做到了「寄存器化」：把槽位从 `Value`
数组解包到 `int64_t reg[8]`。但它漏了一件 C 层面无法回避的事实：

    reg[fo->dst] 里的 dst 是**运行期读出来的**。

只要下标是运行期值，gcc 就既不能展开、也不能证明下标不变，于是整个
`reg[]` 只能钉在栈上，每轮 4 次内存往返。实测（1e9 次
`c=c-1; i=i+1`，-O2，argv 防折叠，同进程交替取最小值）：

    D0  全具名、零分派                    326 ms   ← 天花板
    D1  (kind,dst) 合并分派 → 具名变量    888 ms
    D2  数组下标、零分派                 1473 ms
    D3  `lf_run_int` 现状（数组+运行期）  1547 ms

注意 D2 与 D1：**「数组下标」比「分派」更贵**。所以光合并分派没用，
必须让槽位落进具名变量。

── 为什么必须是独立函数，而不是原地展开 ──────────────────────

第一版是把展开体直接写进 `lf_run_int` 的函数体里（用
`#include` + `goto lf2_skip` 回到通用路径）。结果是**灾难性的**：

    展开体 2880 份 → x.exe 0.7MB → 5MB
    性能 1909ms → 37928ms（慢 20 倍）

原因不是 I-cache，而是 `goto` 把两条路径连成了**一个函数**：gcc 无法
证明那 2880 个 case 不会跳到通用路径，只能把它们全部当作 `lf_run_int`
的一部分来做寄存器分配，于是整个函数（包括真正要跑的通用循环）的
寄存器分配彻底退化。

把展开体做成**独立的 static 函数** `lf_run2()`，通用路径完全看不见它，
两个函数的寄存器分配互不干扰。这是唯一可行的结构。

── 返回值的契约 ───────────────────────────────────────────

  LF2_OK        循环正常跑完，reg[] 已写回 base[]
  LF2_OVERFLOW_* 发生了溢出，已把 reg[] 写回 base[] 并设好 errpos
  LF2_NOT_APPLY 形态不匹配（本函数不处理），调用方必须退回通用路径

注意 LF2_NOT_APPLY 时**不能**改动 base[]：调用方会从当前状态继续跑。

── 覆盖范围（以及为什么止步于此）──────────────────────────

  · 算子数 == 2                      真实热循环的绝大多数
  · 两个算子都属于 *_CONST 族         （ADD / SUB / MUL 各一种）
  · 槽位号 <= 3                      更大的槽位集合收益被稀释
  · 条件两侧都在已展开的具名变量里

组合数 = 4×4 槽位对 × 3×3 算子对 × 4×5 条件对 = 2880 份。

超出范围的循环原样退回通用路径，行为完全不变。

── 为什么不穷举到 LF_MAX_OPS(16) ────────────────────────

(10 形态 × 8 槽位)^16 是不可计算的。展开必须停在低算子数上，再往上
只能靠运行期生成机器码（真 JIT），那是另一个工程。
"""

import io
import os
import sys

# 算子种类。索引必须与 interp.c 里
# LF_ADD_CONST / LF_SUB_CONST / LF_MUL_CONST 的**枚举值**一致
# （0/1/2）。这三个是 LfOpKind 的头三个成员，改动枚举时要同步改这里。
KINDS = [
    ("add_ovf", "ADD", "+"),
    ("sub_ovf", "SUB", "-"),
    ("mul_ovf", "MUL", "*"),
]

NREG_MAX = 4

SYNC = "reg[0]=r0; reg[1]=r1; reg[2]=r2; reg[3]=r3;"


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, "src", "loop_jit_gen.h")
    nk = len(KINDS)
    buf = []

    def w(s=""):
        buf.append(s)

    n_pair = NREG_MAX * NREG_MAX            # 槽位对
    n_kindp = nk * nk                        # 算子对
    n_condp = NREG_MAX * (NREG_MAX + 1)      # 条件对（+1 档表示「无常量侧」）
    total = n_pair * n_kindp * n_condp

    w("/* 自动生成 —— 勿手改。见 tools/gen_loop_jit.py 顶部的原理说明。 */")
    w("/* lf_run2() 的函数体：%d 份展开循环，每份内部零分派、零数组访问。 */"
      % total)
    w("")
    w("int64_t r0 = reg[0], r1 = reg[1], r2 = reg[2], r3 = reg[3];")
    w("int64_t t;")
    w("(void)t;")
    w("")
    w("/* key = ((d0*4 + d1) * 9 + k0*3 + k1) * 20 + (c1*5 + c2)")
    w(" *   c2 == 4 表示「条件右侧是常量 c1」（*_CONST 形态），")
    w(" *   其余 0..3 表示右侧是具名变量 rN。")
    w(" * 用 5 而不是 4 作步长，是为了让这两种情形不撞车 —— 若把")
    w(" * cond_r2 = -1 直接当成 0，会与「cond_r2 真的是 r0」混淆，")
    w(" * 静默走错分支。 */")
    w("int lf2_c2 = (cond_r2 < 0) ? %d : cond_r2;" % NREG_MAX)
    w("int lf2_key = ((((fops[0].dst * %d + fops[1].dst) * %d + "
      "fops[0].kind * %d + fops[1].kind) * %d) + "
      "(cond_r1 * %d + lf2_c2));"
      % (NREG_MAX, n_kindp, nk, n_condp, NREG_MAX + 1))
    w("switch (lf2_key) {")

    for d0 in range(NREG_MAX):
        for d1 in range(NREG_MAX):
            for k0 in range(nk):
                for k1 in range(nk):
                    fn0, lbl0, _ = KINDS[k0]
                    fn1, lbl1, _ = KINDS[k1]
                    for c1 in range(NREG_MAX):
                        for c2 in range(NREG_MAX + 1):
                            # 同一个变量自己跟自己比恒为假 —— 真实代码
                            # 里那是死循环或无效条件，不该被展开。
                            if c2 < NREG_MAX and c1 == c2:
                                continue
                            key = (((d0 * NREG_MAX + d1) * n_kindp +
                                    k0 * nk + k1) * n_condp +
                                   c1 * (NREG_MAX + 1) + c2)
                            w("case %d: {" % key)
                            w("    for (;;) {")
                            w("        if (cond_kind == LF_CMP_NE_CONST) "
                              "{ if (!(r%d != c1)) break; }" % c1)
                            w("        else if (cond_kind == LF_CMP_LT_CONST) "
                              "{ if (!(r%d < c1)) break; }" % c1)
                            if c2 < NREG_MAX:
                                w("        else if (cond_kind == LF_CMP_NE_LOCAL) "
                                  "{ if (!(r%d != r%d)) break; }" % (c1, c2))
                                w("        else if (cond_kind == LF_CMP_LT_LOCAL) "
                                  "{ if (!(r%d < r%d)) break; }" % (c1, c2))
                            w("        else return LF2_NOT_APPLY;")
                            w("        if (X_UNLIKELY(%s(r%d, fops[0].k, &t))) "
                              "{ %s g_lf2_errpos = fops[0].pos; "
                              "  return LF2_OVERFLOW_%s; }"
                              % (fn0, d0, SYNC, lbl0))
                            w("        r%d = t;" % d0)
                            w("        if (X_UNLIKELY(%s(r%d, fops[1].k, &t))) "
                              "{ %s g_lf2_errpos = fops[1].pos; "
                              "  return LF2_OVERFLOW_%s; }"
                              % (fn1, d1, SYNC, lbl1))
                            w("        r%d = t;" % d1)
                            w("    }")
                            w("    %s return LF2_OK;" % SYNC)
                            w("}")
    w("default: return LF2_NOT_APPLY;")
    w("}")

    io.open(out_path, "w", encoding="utf-8", newline="\n").write(
        "\n".join(buf) + "\n")
    sys.stderr.write("loop_jit_gen.h: %d 行, %d 份循环体\n"
                     % (len(buf), total))


if __name__ == "__main__":
    main()