#!/usr/bin/env python3
# gen_gui_embed.py — 把窗口运行时转成可内嵌的 C 字符串
#
# ── 为什么需要这一步 ────────────────────────────────────────
#
# AOT 产物（`x build --emit=exe`）必须是**单个自包含的 exe**。
# 因此窗口实现不能靠「编译时再去找 src/gui/gui_win32.c」——
# 拿到了别人机器上的 exe 时，那个文件根本不存在。
#
# 唯一的做法是把实现嵌进生成的 C 代码里，和 PRELUDE 一样。
#
# ── 为什么不手工复制一份到 codegen_c.c ──────────────────────
#
# 那会造成两份需要同步维护的 Win32 代码。改一处忘另一处，
# 症状是「解释器跑得好、AOT 产物行为不同」，而这类差异
# 只在 GUI 上体现，普通的 AOT 一致性测试抓不到全部。
#
# 所以这里让 gui.h + gui_win32.c 成为**唯一的源**，
# 每次 build 时转成 src/gui/gui_embed.h（生成的，不要手工编辑）。
#
# ── 产出的形状 ──────────────────────────────────────────────
#
#   static const char* GUI_IMPL_PRELUDE = "...gui.h 的内容...";
#   static const char* GUI_IMPL_BODY     = "...gui_win32.c 的内容...";
#
# 分成两段是因为 gui_win32.c 里有一行 `#include "gui.h"`，
# 在产物目录里那个文件不存在。分段之后 codegen 可以只输出
# PRELUDE（接口声明）+ BODY（实现，且已跳过 include 行）。

import re
import sys
import os


def escape_bytes(bs: bytes) -> str:
    """
    把一段 UTF-8 字节转成 C 字符串字面量的内容（不含两端引号）。

    ── 为什么按 bytes 而不是 str ──────────────────────────────
    最初写成逐字符处理（python 的 str），中文的 ord('中') 是
    0x4E2D，取 & 0xFF 得到 0x2D —— 也就是 '-'，于是所有中文
    注释都被转成了一串减号，而且**编译完全正常**，只是注释变成
    了乱码，不会有任何报错。这类「静默损坏」只能靠对生成的
    文件做一次回读校验来发现（见 main 里的自检）。
    """
    out = []
    for b in bs:
        if b == 0x5C:                      # '\\'
            out.append('\\\\')
        elif b == 0x22:                    # '"'
            out.append('\\"')
        elif b == 0x09:                    # tab
            out.append('\\t')
        elif 0x20 <= b < 0x7F:
            # `??` 在 C 里是三元组（trigraph）的引导序列。
            # gcc 默认关闭，但 -std=c11 的严格模式下会被解释，
            # `??=` 会变成 `#`。主动断开更安全。
            if b == 0x3F:
                out.append('\\077')        # 强制转义每个 '?'，彻底避开
            else:
                out.append(chr(b))
        else:
            # 非 ASCII（中文注释）与不可打印字符一律八进制转义。
            # 这样生成的文件是纯 ASCII，不依赖任何代码页 ——
            # 万一有人在 GBK 环境下重新构建，内容不会变。
            out.append('\\%03o' % b)
    return ''.join(out)


def to_c_string_lines(data: bytes, max_chunk: int = 400) -> str:
    """
    把整段文本转成若干行 C 字符串字面量，每行源文本一条。

    为什么要按源行切：生成的文件里能直接看出「第几行对应原文
    第几行」，AOT 产物编译失败时 gcc 报的行号还能大概定位回
    gui_win32.c。
    """
    lines = data.split(b'\n')
    # 文件以 \n 结尾时最后会多出一个空元素，不要给它造字面量
    trailing_nl = True
    if lines and lines[-1] == b'':
        lines.pop()
    else:
        # 源文件**没有**以换行结尾（gui.h 就是这种）。
        # 不加这个标记的话，下面给每行统一补一个 \n 会让结果
        # 比原文多出一个字节 —— 自检立刻报「长度不一致」。
        # 这种一字节差异肉眼绝对看不出来，但会让 head 与 body
        # 的拼接处多一个空行，生成的 C 代码里就多一行无意义注释。
        trailing_nl = False

    parts = []
    for idx, ln in enumerate(lines):
        # 长行再切碎，避免单个字面量过长（C99 要求实现至少支持
        # 4095 字节的字符串字面量，留足余量按 400 切）
        pieces = [ln[i:i + max_chunk] for i in range(0, len(ln), max_chunk)] or [b'']
        body = ''.join(escape_bytes(p) for p in pieces)
        # 只有原文真的带换行时才补 \n
        is_last = (idx == len(lines) - 1)
        if is_last and not trailing_nl:
            parts.append('    "%s"' % body)
        else:
            parts.append('    "%s\\n"' % body)
    return '\n'.join(parts)


def strip_include(data: bytes) -> bytes:
    """去掉 gui_win32.c 里对 gui.h 的 include（内容会单独给出）。"""
    return re.sub(rb'^#include\s+"gui\.h"\s*$',
                  b'/* gui.h \xe5\xb7\xb2\xe5\x86\x85\xe8\x81\x94\xe5\x9c\xa8\xe4\xb8\x8a\xe6\x96\xb9 */',
                  data, count=1, flags=re.M)


def verify(head_lit: str, head: bytes, body_lit: str, body: bytes) -> bool:
    """
    把生成的 C 字面量交给编译器解回来，逐字节比对原文。

    ── 为什么要分开校验两段 ────────────────────────────────────
    第一版把两段拼成一段校验，结果报「第 5164 字节不匹配」：
    期望 '/'、实际 '\\n'。原因是 to_c_string_lines 会把末尾的空
    行 pop 掉（那是文件结尾的 \\n 造成的，不该造字面量），
    于是 head 的最后一个换行没了，拼接处就少一个字节。

    这类「拼接处差一个字节」的错误如果不校验，后果是生成的
    C 代码里两条注释黏在一起——比如 `#endif  // X_GUI_H// gui_win32.c`。
    因为都在注释里，编译器不会报错，只是把后面整行当注释吃掉。
    如果那个位置恰好是一条预处理指令，就会变成极难查的编译错误。

    分两段校验之后，边界问题也一并暴露。
    """
    return (_verify_one(head_lit, head) and _verify_one(body_lit, body))


def _verify_one(lits: str, want: bytes) -> bool:
    import subprocess
    import tempfile

    cc = None
    # 先认环境变量，再试 PATH，最后试本项目工具链的默认位置。
    # 最后一项是必要的：这个项目的 gcc 刻意装在 C:/xgcc 而不在
    # PATH 里（见 tools/build.sh 的说明），只查 PATH 会一直跳过自检。
    cands = []
    if os.environ.get('X_CC'):
        cands.append(os.environ['X_CC'])
    cands += ['gcc', 'cc', 'clang']
    if os.name == 'nt':
        cands.append('C:/xgcc/mingw64/bin/gcc.exe')
    for cand in cands:
        try:
            subprocess.run([cand, '--version'], capture_output=True, check=True)
            cc = cand
            break
        except (OSError, subprocess.CalledProcessError):
            continue
    if cc is None:
        # 没有编译器就没法自检。不算失败，但要明确说出来 ——
        # 静默跳过是最坏的做法（「假通过」）。
        print('note: 没找到 C 编译器，跳过 gui_embed.h 的转义自检',
              file=sys.stderr)
        return True

    prog = (
        '#include <stdio.h>\n'
        'static const char* H =\n' + lits + ';\n'
        'int main(int argc, char** argv) {\n'
        '    FILE* f = fopen(argv[1], "wb");\n'
        '    if (!f) return 1;\n'
        '    fwrite(H, 1, __builtin_strlen(H), f);\n'
        '    fclose(f);\n'
        '    return 0;\n'
        '}\n'
    )

    with tempfile.TemporaryDirectory() as _td:
        # ── 关键：编译目录必须是纯 ASCII ────────────────────
        #
        # 这里踩的是两层坑，都只在用户名含中文时暴露：
        #
        #  1) as.exe 把 .o 写到 %TEMP%，中文路径下直接失败：
        #       Fatal error: can't create C:\Users\<乱码>\ccXXXX.o
        #  2) ld.exe 打不开含非 ASCII 字节的 **输出** 路径：
        #       cannot open output file C:\Users\<乱码>\tmpXXX\v.exe
        #
        # 所以光改 TMP/TEMP 不够 —— 临时工作目录本身也得搬。
        # 解法：TMP 指到 C:/xtmp，工作目录也建在它下面。
        # 这与 tools/build.sh 和 codegen_c 里处理的是同一个坑，
        # 三处必须用同一套办法，否则「某条路径能跑」会误导人。
        env = dict(os.environ)
        ascii_tmp = None
        for cand in ('C:/xtmp', 'C:/Temp', 'C:/Windows/Temp'):
            try:
                os.makedirs(cand, exist_ok=True)
                probe = os.path.join(cand, '.xprobe_embed')
                with open(probe, 'wb') as pf:
                    pf.write(b'x')
                os.remove(probe)
                ascii_tmp = cand
                break
            except OSError:
                continue

        if ascii_tmp:
            env['TMP'] = ascii_tmp
            env['TEMP'] = ascii_tmp
            workdir = os.path.join(ascii_tmp, 'xembed_%d' % os.getpid())
            os.makedirs(workdir, exist_ok=True)
        else:
            # 找不到 ASCII 目录就退回系统临时目录，自检可能被跳过
            workdir = _td

        try:
            src = os.path.join(workdir, 'v.c')
            exe = os.path.join(workdir, 'v.exe' if os.name == 'nt' else 'v')
            outp = os.path.join(workdir, 'out.txt')
            with open(src, 'w', encoding='ascii', newline='\n') as f:
                f.write(prog)
            r = subprocess.run([cc, '-std=c11', '-w', src, '-o', exe],
                               capture_output=True, env=env, cwd=workdir)
            if r.returncode != 0:
                print('note: 自检程序编译失败，跳过', file=sys.stderr)
                print(r.stderr.decode('utf-8', 'replace')[:800], file=sys.stderr)
                return True
            r = subprocess.run([exe, outp], capture_output=True, env=env, cwd=workdir)
            if r.returncode != 0:
                return False
            with open(outp, 'rb') as f:
                got = f.read()
        finally:
            if ascii_tmp:
                import shutil
                shutil.rmtree(workdir, ignore_errors=True)

    if got != want:
        n = min(len(got), len(want))
        for i in range(n):
            if got[i] != want[i]:
                print(f'error: 第 {i} 字节不匹配：期望 {want[i]:#04x}，'
                      f'实际 {got[i]:#04x}', file=sys.stderr)
                print(f'  上下文（期望）: {want[max(0,i-20):i+20]!r}',
                      file=sys.stderr)
                print(f'  上下文（实际）: {got[max(0,i-20):i+20]!r}',
                      file=sys.stderr)
                break
        else:
            print(f'error: 长度不一致：期望 {len(want)}，实际 {len(got)}',
                  file=sys.stderr)
        return False
    return True


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src_gui = os.path.join(root, 'src', 'gui')

    h_path = os.path.join(src_gui, 'gui.h')
    c_path = os.path.join(src_gui, 'gui_win32.c')
    out_path = os.path.join(src_gui, 'gui_embed.h')

    with open(h_path, 'rb') as f:
        h_data = f.read()
    with open(c_path, 'rb') as f:
        c_data = strip_include(f.read())

    head_lit = to_c_string_lines(h_data)
    body_lit = to_c_string_lines(c_data)

    # 头文件只保留声明部分：把注释压掉能省一半体积，
    # 但注释对排查问题有价值，权衡之后决定保留。
    body = (
        '// gui_embed.h — **由 tools/gen_gui_embed.py 生成，不要手工编辑**\n'
        '//\n'
        '// 源：src/gui/gui.h + src/gui/gui_win32.c\n'
        '//\n'
        '// 这个头文件把窗口运行时以 C 字符串形式提供给 AOT 代码生成器，\n'
        '// 使得 `x build --emit=exe` 的产物是自包含的单个可执行文件。\n'
        '//\n'
        '// 重新生成：./tools/build.sh（会自动调用脚本）\n'
        '// 或直接：python3 tools/gen_gui_embed.py\n'
        '\n'
        '#ifndef X_GUI_EMBED_H\n'
        '#define X_GUI_EMBED_H\n'
        '\n'
        '// 接口声明部分（gui.h 的内容）\n'
        'static const char* GUI_IMPL_HEAD =\n'
        f'{head_lit};\n'
        '\n'
        '// 实现部分（gui_win32.c 的内容，已去掉 #include "gui.h"）\n'
        'static const char* GUI_IMPL_BODY =\n'
        f'{body_lit};\n'
        '\n'
        '#endif  // X_GUI_EMBED_H\n'
    )

    # ── 自检 ────────────────────────────────────────────────
    #
    # 这个脚本最容易出错的地方是转义（把源码转成 C 字面量再让
    # 编译器转回来）。曾经写成按 python str 处理，中文字符被
    # 截断成 '-'，生成的文件仍然合法、编译仍然通过，只是所有
    # 中文注释变成了减号 —— 靠人眼根本发现不了。
    #
    # 所以这里把生成的字符串**真的编译回来**并逐字节比对。
    # 代价是一次进程启动，收益是这类静默损坏不可能逃过。
    if not verify(head_lit, h_data, body_lit, c_data):
        print('error: gui_embed.h 转义自检失败（见上方说明）', file=sys.stderr)
        return 1

    # 内容没变就不重写，避免无谓地触发整项目重编译
    old = None
    if os.path.exists(out_path):
        with open(out_path, 'r', encoding='utf-8') as f:
            old = f.read()
    if old == body:
        return 0

    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(body)
    return 0


if __name__ == '__main__':
    sys.exit(main())