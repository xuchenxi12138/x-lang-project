# 构建环境搭建（Windows / MinGW-w64）

这份文档记录的是**踩过的坑**，不是通用教程。如果你在别的机器上搭，按这里的顺序做能省几个小时。

## 为什么不用 MSYS2 安装器

试过 `winget install msys2`，卡在 GitHub 直连：

```
InternetOpenUrl() failed. 0x80072efd
curl https://github.com  →  502
```

改用 USTC 镜像直接抓 Arch 包，全部成功。

## 需要哪些包

MinGW-w64 GCC 不是单个包，是**十三个包**。少一个就在某个环节崩，而且崩的方式极具误导性（见下面的坑）。

| 包 | 作用 | 缺了会怎样 |
|---|---|---|
| `gcc` | 编译器驱动 + `cc1.exe` | 无 |
| `gcc-libs` | **空壳元包**，只声明依赖 | — |
| `cc-libs` | **也是空壳** | — |
| `binutils` | `as` / `ld` / `objdump` | 无法汇编链接 |
| `crt` | C 运行时启动代码 | 链接失败 |
| `headers` | 系统头文件（`stdio.h` 等） | `no include path in which to search for stdio.h` |
| `libwinpthread` | `libwinpthread-1.dll` | gcc 启动失败 |
| `winpthreads` | `libpthread.a` / `libwinpthread.dll.a` | **`cannot find -lpthread`**，链接期才报 |
| `zlib` | `zlib1.dll` | `cc1.exe` 启动失败 |
| `zstd` | `libzstd.dll` | `cc1.exe` 启动失败（GCC 16 的 LTO 用 zstd 压缩） |
| `gmp` / `mpfr` / `mpc` / `isl` | 大数、浮点、多精度、循环优化 | `cc1.exe` 启动失败 |
| `windows-default-manifest` | 清单文件 | 通常无影响 |

**`gcc-libs` 是坑**：它只有 2 KB，里面是 `.BUILDINFO` / `.MTREE` / `.PKGINFO`，**一个文件都没有**。它真正的内容在 `cc-libs` 里，而 `cc-libs` 也是空壳。真要拿到 `libgcc_s_seh-1.dll`，得下**旧版本**的 `gcc-libs`：

```
mingw-w64-x86_64-gcc-libs-16.1.0-1-any.pkg.tar.zst   ← 这个有内容
mingw-w64-x86_64-gcc-libs-16.2.0-6-any.pkg.tar.zst   ← 空壳，别下
```

装好后的目录结构：

```
C:/xgcc/mingw64/
├── bin/            gcc.exe cc1 相关 DLL、as、ld、make 等
├── include/        系统头文件
├── lib/            静态库、libpthread.a
└── lib/gcc/x86_64-w64-mingw32/16.2.0/
    ├── cc1.exe     真正的编译器前端
    └── *.dll       ← 手动复制过来的，见坑 2
```

## 三个必踩的坑

### 坑 1：符号链接在 Windows 上丢失

Arch 包里的 `gcc.exe` 是指向 `cc.exe` 的**符号链接**。Windows 上解包（无论用 tar 还是 7z）如果没有管理员权限，这些链接会变成 0 字节文件。

症状：`gcc.exe --version` 无任何输出，退出码 0。

解决：`tools/fix_symlinks.py` 把所有符号链接按目标内容**复制成实体文件**。

```sh
python tools/unpack_pkgs.py C:/xgcc /tmp/xgcc/*.pkg.tar.zst
python tools/fix_symlinks.py C:/xgcc /tmp/xgcc/*.pkg.tar.zst
```

### 坑 2：gcc 驱动会丢掉 PATH

`gcc.exe` fork 出 `cc1.exe` 时，**新进程的环境不继承你 `export` 的 PATH**。所以即使 `C:/xgcc/mingw64/bin` 在 PATH 里、`gcc --version` 也能跑，一编译就失败。

症状（这个报错极具误导性）：

```
cc1.exe: error while loading shared libraries: libmpfr-6.dll:
         cannot open shared object file: No such file or directory
```

看起来像「没装 mpfr」，其实装了 —— 只是 `cc1.exe` 找不到。

解决：把所有 DLL 物理复制到 `cc1.exe` 同目录。

```sh
cp C:/xgcc/mingw64/bin/*.dll C:/xgcc/mingw64/lib/gcc/x86_64-w64-mingw32/16.2.0/
```

### 坑 3：中文用户名让汇编器和链接器双双失效

用户名含中文（如 `徐辰曦`）时：

**汇编器**无法在 `%TEMP%` 下创建 `.o`：

```
Assembler messages:
Fatal error: can't create C:\Users\<乱码>\AppData\Local\Temp\cceXEGTk.o:
                No such file or directory
```

**链接器**无法写含中文的输出路径：

```
ld.exe: cannot open output file F:/<乱码>/build/x.exe: No such file or directory
```

解决：`TMP` 指向 ASCII 路径，链接输出先写到 ASCII 路径再 `cp` 回来（`cp` 是文件操作，不经过 MinGW 的路径转换，中文没问题）。

**补充：这个坑对 `x build --emit=exe`（AOT）同样成立，而且更深一层。**

构建 `x.exe` 本身时，`build.sh` 是在 shell 层做规避（改 TMP + 输出走 ASCII + cp 回来）。但 AOT 是在 `x.exe` **运行时**调 gcc，shell 层的规避帮不上忙，必须在 C 代码里做同样的事。

逐层实测的结果：

| 尝试 | 结果 |
|---|---|
| 只把子进程 cwd 切到 ASCII 目录 | 仍失败，`as` 把 `.o` 放 cwd 了但 `cc1` 仍写 `%TEMP%` |
| 只改子进程的 `TMP`/`TEMP` | 成功（关键一步） |
| 加 `-pipe`（网上常见建议） | **无效**，这个 MinGW 版本仍落 `.o` 到 TEMP |
| 把源文件复制到 ASCII 目录 | 必须，`cc1` 读含中文的源路径也失败 |
| 产物在 ASCII 目录生成后再搬 | 必须，`ld` 打不开含中文的输出路径 |

四条全做才通。实现收在 `platform.c` 的 `plat_spawn_with_tmp()`（内部用 `CreateProcess`，因为 `_spawnvp` 不能指定 cwd）。

**另一个必须知道的实现细节**：`CreateProcess` 的环境块要求**一整块连续内存**（`"K=V\0K=V\0...\0\0"`）。把 `_environ` 这个指针数组强转成 `LPVOID` 传进去会得到 `ERROR_INVALID_PARAMETER (87)`——字符串在堆上是分散的，系统顺着第一段往后读，读到第二个指针就崩。必须自己摊平成一块。这个错误码 87 没有任何提示指向环境块，排查时很容易误判成路径问题。

`tools/build.sh` 已经把这两件事都处理了：

```sh
export TMP="C:/xtmp"
export TEMP="C:/xtmp"
...
-o "/c/xtmp/x.exe"      # 链接输出走 ASCII
cp "/c/xtmp/x.exe" "$OUT/x.exe"   # 再复制回项目目录
```

## 构建

```sh
./tools/build.sh          # 常规构建
./tools/build.sh clean    # 清理后重建
./tools/build.sh debug    # -O0 -g3，便于 gdb
```

产物是 `build/x.exe`，约 850 KB，无外部依赖（DLL 可静态链接，P0 用动态链接图快）。

## AOT 编译（`--emit=exe`）

需要本机有 C 编译器。`x` 会按 `--cc` > `CC` 环境变量 > 平台默认（Windows `gcc` / 类 Unix `cc`）的顺序找。

```sh
# 指定编译器（Windows 上通常需要，因为 gcc 常不在系统 PATH 里）
./build/x.exe build app.xle --emit=exe --cc=/c/xgcc/mingw64/bin/gcc.exe -o app.exe

# 只生成 C 代码，自己拿到别处编译
./build/x.exe build app.xle --emit=c -o app.c

# 交叉编译（需要对应的交叉工具链）
./build/x.exe build app.xle --emit=exe --target=x86_64-linux-gnu -o app

# 看有哪些目标
./build/x.exe target list
```

**三档产物各是什么**：

| 参数 | 产物 | 什么时候用 |
|---|---|---|
| 默认 | `.xbc` 字节码 | 只发字节码，目标机用 `x run` 执行 |
| `--emit=c` | C 源代码 | 目标机没有 `x` 但有 gcc；或要手工 review 生成的代码 |
| `--emit=exe` | 本机可执行文件 | 开箱运行、单文件分发 |

**格式不能单独指定**：Windows 只能 PE，Linux 只能 ELF，macOS 只能 Mach-O。所以是 `--target`（选平台）而不是 `--format`（选格式）。

编译成功后会**读一次产物文件头**确认格式正确——交叉编译配错工具链时，gcc 常常成功退出却产出宿主格式，只看退出码会全绿。

## 跨平台

平台相关代码全部在 `src/platform.c` 和 `src/platform.h` 两个文件里。**其他任何源文件不许 include `<windows.h>`/`<unistd.h>`，不许出现 `_WIN32`/`__APPLE__`/`__linux__`**。这条规则由 `tools/check_portable.sh` 机械验证：

```sh
./tools/check_portable.sh
```

5 项检查：平台宏不泄漏 / 系统头不泄漏 / 每个编译单元零警告 / 宿主三元组可解析 / 类 Unix 分支的符号与 include 齐全。

**为什么必须由脚本查**：在 Windows 上写 `#ifdef _WIN32` 是完全自然的动作，写完也能正常编译运行，错误只在 Linux 上第一次编译时才暴露——那时往往已经积累了十几处。这个检查在 Windows 上就能跑，提交前就能拦住。

**当前验证状态**：Windows 侧端到端验证完成（产物可运行、中文路径可编译、15/15 测试含 AOT 一致性全过）。Linux/macOS 侧只做了代码层验证，**尚未在真实机器上编译运行过**——这是目前最该补的一环。

## 测试

```sh
./tools/run_tests.sh              # 15 个语义测试（含中文零成本 / 超指令中性断言）
./tools/run_tests.sh -v           # 同时显示每个测试的输出
./tools/run_tests.sh 05           # 只跑文件名含 05 的
./tools/check_diagnostics.sh      # 13 项运行时诊断完整性
./tools/check_compile_diagnostics.sh  # 17 项编译期诊断完整性
./tools/check_utf8.sh             # 11 项 UTF-8 端到端检查
./tools/bench.sh                  # 性能基准（X vs C -O0 / -O2）
./tools/bench.sh --nosuper        # 关掉超指令，对比 L3 优化收益
```

`check_utf8.sh` 守的是「不会崩但会悄悄错」的那一类问题：BOM 是否剥离、列号是否按字符计、GBK 源文件是否被明确识别、中文路径能否读写、诊断标签是否按显示宽度对齐。漏掉任何一条程序都照常运行，只是行为错在暗处——所以必须由测试固定住。

`run_tests.sh` 对每个测试除跑三件事（预期输出、`.xbc` 往返一致、中英字节码一致）外，还会用 `X_NO_SUPER=1` 再跑一遍，输出必须逐字节相同。这是超指令的**中性断言**：L3 优化只允许改变速度，不允许改变结果。合并 pass 写错时的症状是静默算错（跳转跳过头、栈上少压一个操作数），光看「程序能跑通」发现不了。

## 性能基准

`bench.sh` 有四个设计决定，每一个都来自踩坑：

| 决定 | 原因 |
|---|---|
| 多轮**交替**测量取最小值 | 这台机器常驻 ToDesk / aTrust / 杀软，同一二进制连续三轮能差 1.8 倍。先测完 A 再测 B 会让两边遇到的干扰强度不同，比值就成了干扰的比值 |
| 计时用 Python 不用 shell `date` | Git Bash 的 `date` 每次 fork 一个 MSYS 进程，单次开销 100–300ms，比被测对象还大 |
| 扣掉进程启动基线（约 50ms） | 控制台初始化、UTF-8 切换、内存池都有固定成本，不扣会把它算进循环 |
| C 对照的递归参数从 `argv` 读 | `fib(33)` 字面量会被 gcc -O2 在**编译期**求值，运行时只花 0.1ms，测的是常量折叠能力而不是执行速度 |

当前结果（详见 `tests/p0/12-perf.xle` 头部）：整数循环 3000 万次 ≈ 7× C -O0，fib(33) ≈ 8.7×。

## 工具脚本一览

| 脚本 | 作用 |
|---|---|
| `unpack_pkgs.py` | 解 `.pkg.tar.zst`（Python 的 `tarfile` 不认 zstd，需 `pip install zstandard`） |
| `fix_symlinks.py` | 把符号链接具体化为实体文件（坑 1） |
| `build.sh` | 构建（处理坑 2 之外的坑 3） |
| `run_tests.sh` | 语义测试运行器 |
| `check_diagnostics.sh` | 运行时诊断完整性守门 |
| `check_compile_diagnostics.sh` | 编译期诊断完整性守门 |
| `check_utf8.sh` | UTF-8 端到端守门（编码、BOM、列号、路径、对齐） |
| `bench.sh` | 性能基准（交替测量 + 扣启动基线，X vs C -O0/-O2） |
| `bench_timer.py` | bench.sh 的计时内核，负责交替测量与 MSYS 路径转换 |