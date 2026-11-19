# 工具链安装记录（Windows / winlibs 路线）

`build.sh` 里的注释和 `BUILD.md` 都指向本文件说明「坑 2 已处理」，
但仓库里一直没有这个文件。这里补上，记录的是**一次真实安装的结果**，
不是通用教程 —— 换机器时按这里做能复现。

## 和 BUILD.md 的 MSYS2 路线有什么不同

`BUILD.md` 走的是「从 USTC 镜像抓 13 个 Arch 包 + `unpack_pkgs.py` +
`fix_symlinks.py`」。那条路能成，但要处理符号链接丢失、`gcc-libs` 空壳、
逐个包对版本。winlibs 的官方 zip 是**已经拼好的整体**，一个包解决全部依赖，
代价只是体积大（zip 276 MB）。

两条路产出的目录结构基本一致，唯一差别是 `cc1.exe` 的位置：

| 来源 | `cc1.exe` 路径 |
|---|---|
| MSYS2 / Arch 包 | `lib/gcc/x86_64-w64-mingw32/<ver>/` |
| **winlibs zip** | `libexec/gcc/x86_64-w64-mingw32/<ver>/` |

坑 2 的修复（把 DLL 复制到 `cc1.exe` 同目录）必须按后者来，否则复制到
`lib/gcc/...` 是个空目录，症状依旧是 `cc1.exe: error while loading shared
libraries: libmpfr-6.dll`。

## 本机实际安装（2026-11-13）

```
C:/xgcc/mingw64/          ← winlibs 解压结果，即 XGCC
├── bin/gcc.exe           MinGW-W64 x86_64-ucrt-posix-seh 16.2.0 (Brecht Sanders r3)
├── bin/*.dll             76 个（含 gmp/mpfr/mpc/isl/zstd/zlib）
└── libexec/gcc/x86_64-w64-mingw32/16.2.0/
    ├── cc1.exe
    └── *.dll             ← 从 bin/ 复制过来的 77 个（坑 2）
```

包：`winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip`
（github.com/brechtsanders/winlibs_mingw，release tag `16.2.0posix-23.1.3-14.0.0-ucrt-r3`，
SHA256 `3c69305081e4a647144339a936244fcf7d3d9e558319e1ad7be2cd2886d4b6a1`）

## 三条命令（换机器时照抄）

```sh
# 1. 解压（zip 内顶层就是 mingw64/ 目录，所以 -d 到 C:/xgcc）
unzip -q winlibs-*.zip -d /c/xgcc/

# 2. 坑 2：DLL 物理复制到 cc1.exe 同目录
D=/c/xgcc/mingw64/libexec/gcc/x86_64-w64-mingw32/16.2.0
cp /c/xgcc/mingw64/bin/*.dll "$D"/

# 3. 冒烟（不带 TMP 会撞坑 3a，中文用户名下 as 无法在 %TEMP% 建 .o）
TMP=C:/xtmp TEMP=C:/xtmp /c/xgcc/mingw64/bin/gcc.exe -O2 probe.c -o probe.exe
```

`build.sh` 自己会设 `TMP=C:/xtmp`，所以直接 `sh tools/build.sh` 即可。

## 验证到哪一步

| 命令 | 结果 |
|---|---|
| `sh tools/build.sh` | 零警告产出 `build/x.exe`（1.77 MB，`x 0.8.0-p0`） |
| `sh tools/run_tests.sh` | 83 项全过 |
| `sh tools/check_diagnostics.sh` | 13 项全完整 |
| `sh tools/check_compile_diagnostics.sh` | 18 场景，建议覆盖 100% |
| `sh tools/check_utf8.sh` | 11 项全过 |
| `sh tools/check_portable.sh` | 21 个编译单元零警告；**2 项 FAIL 见下** |

`run_tests.sh` 的「语法高亮 scope」一项要求 `vscode-textmate` /
`vscode-oniguruma`，它只在三个缓存目录里找，不会用扩展自带的
`vscode-x/node_modules`。没准备就 SKIP（跳过不算通过）：

```sh
mkdir -p ~/.cache/x-tm && cp -r vscode-x/node_modules ~/.cache/x-tm/
```

`check_portable.sh` 曾有的 2 项 FAIL（`src/lsp.c` 自带 `#ifdef _WIN32` +
`<io.h>`，LSP 的 stdout 二进制模式重设）已于 2026-11-14 修复：该逻辑
收进 `platform.c` 的 `plat_set_binary_stdio()`，`lsp.c` 改为一行调用，
现在 5 项检查全部通过。
