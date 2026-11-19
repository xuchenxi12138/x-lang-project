#!/usr/bin/env python3
"""X 语言基准计时器 —— 交替测量多个候选。

为什么必须「交替」而不是「跑完 A 再跑完 B」：

    这台机器上常驻 ToDesk / aTrust / TQClient / 杀软，CPU 占用随机抖动，
    实测同一个二进制连续三轮能差 1.8 倍（596ms → 1076ms → 1129ms）。
    如果先测完 A 的 11 轮、再测 B 的 11 轮，那么「A 和 B 各自遇到的
    干扰强度」是不同的 —— 得出的比值可能是干扰的比值，不是性能的比值。

    交替测量让同一轮的 A 和 B 紧挨着跑，共享同一段系统状态；
    再各自取最小值，最小值天然过滤掉「被干扰的那几轮」。
    这是基准测试的标准做法，也是唯一能在噪声环境里得到可信比值的方式。

用法:
    python tools/bench_timer.py <轮数> <标签1>::<命令1> <标签2>::<命令2> ...

    命令里的参数用 `|` 分隔，不用引号 —— 引号会被 sh 保留成字面字符，
    再经 shlex 处理后变成路径的一部分，导致「系统找不到指定的文件」。

输出（每行一个）:
    <标签> <最小耗时ms>
"""
import os
import re
import subprocess
import sys
import time

# Git Bash 传过来的路径是 MSYS 形式：/c/xtmp/x.exe、/f/项目/...
# Windows 的 CreateProcess 不认这种写法，会报「系统找不到指定的文件」，
# 而且错误信息里不会提示是路径格式问题。这里统一转成 C:/xtmp/x.exe。
_MSYS = re.compile(r"^/([a-zA-Z])/")


def norm(p):
    m = _MSYS.match(p)
    if m:
        return m.group(1).upper() + ":/" + p[3:]
    return p


def main():
    rounds = int(sys.argv[1])
    specs = sys.argv[2:]

    cands = []
    for spec in specs:
        label, cmd = spec.split("::", 1)
        argv = [norm(a) for a in cmd.split("|")]
        cands.append((label, argv))

    best = {label: float("inf") for label, _ in cands}

    for _ in range(rounds):
        for label, cmd in cands:
            t0 = time.perf_counter()
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            dt = (time.perf_counter() - t0) * 1000.0
            if dt < best[label]:
                best[label] = dt

    for label, _ in cands:
        print(f"{label} {best[label]:.1f}")


if __name__ == "__main__":
    main()