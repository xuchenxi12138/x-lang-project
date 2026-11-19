// target.h — 编译目标：产物格式、文件后缀、工具链
//
// ── 先回答「应该编译成什么格式」 ────────────────────────────
//
// 这个问题没有选择余地：**每个操作系统的可执行文件格式是它定的，
// 不是我们定的**。三个平台各只有一种原生格式：
//
//   Windows  PE / COFF   （.exe，文件头 "MZ" 开头，e_lfanew 指向 "PE\0\0"）
//   Linux    ELF         （无后缀，文件头 0x7F 'E' 'L' 'F'）
//   macOS    Mach-O      （无后缀，文件头 0xFEEDFACF 小端 64 位）
//
// 所以「指定编译格式」实际是指定**目标平台**，格式是它的推论。
// 这就是为什么参数叫 --target 而不是 --format：用户可以要求
// 「给 Linux 编一个」，但不应该被允许要求「给 Windows 编一个 ELF」
// —— 那个文件在 Windows 上根本跑不起来，允许指定只会制造废品。
//
//   ★ 格式由平台决定，不单独暴露。
//
// ── 三元组为什么是三段 ──────────────────────────────────────
//
// 形如 x86_64-windows-msvc：
//   第 1 段 = CPU 架构（决定指令集，也决定产物能不能在目标机器上跑）
//   第 2 段 = 操作系统（决定可执行文件格式与系统调用约定）
//   第 3 段 = ABI / libc（决定链接哪个 C 运行库、符号命名规则）
//
// 第三段在 Windows 上是 msvc/mingw（决定 _main vs main 的入口约定），
// 在 Linux 上是 gnu/musl（决定链接 glibc 还是静态 musl），
// macOS 上没有第三段（只有一套 ABI）。
//
// 这不是学 Rust 的格式，而是 ELF/PE 本身的现实约束：
// 同一台 x86-64 机器上，链接 glibc 与链接 musl 的产物不能混用。

#ifndef X_TARGET_H
#define X_TARGET_H

#include <stdbool.h>
#include <stdio.h>

// ============================================================
// 产物格式
// ============================================================

typedef enum {
    FMT_UNKNOWN = 0,
    FMT_PE,         // Windows 可执行文件
    FMT_ELF,        // Linux / BSD 可执行文件
    FMT_MACHO,      // macOS 可执行文件
} XObjFormat;

// 格式的可读名（"PE" / "ELF" / "Mach-O"），用于诊断与 --target list 表格
const char* fmt_name(XObjFormat f);

// 从文件头判断一个文件是什么格式。用于**产物的自检**：
// 编完读回来看 magic，确认工具链真的产出了声称的格式。
//
// 为什么必须自检：交叉编译配错工具链时，gcc 常常「成功」退出，
// 产出的却是宿主格式。这时 if 只看退出码就全绿，把产物拿到目标
// 机器上才发现跑不了。读一次文件头是几行代码的事，能挡住整类事故。
//
// 返回 FMT_UNKNOWN 表示读不出或不是这三种之一（脚本、文本等）。
XObjFormat fmt_detect_file(const char* path);

// ============================================================
// 目标描述
// ============================================================

typedef struct {
    const char* triple;     // 完整三元组，如 "x86_64-linux-gnu"
    const char* arch;       // "x86_64" / "aarch64"
    const char* os;         // "windows" / "linux" / "macos"
    const char* abi;        // "msvc" / "mingw" / "gnu" / "musl" / ""（macOS 为空）
    XObjFormat  format;     // 由 os 决定，不单独指定
    const char* exe_suffix; // ".exe" / ""
    const char* cc_prefix;  // 交叉编译器前缀，如 "x86_64-linux-gnu-"，宿主为空串
    const char* note;       // 一行说明（给人看，也进 --target list）
} XTarget;

// 全部已知目标。返回数组与长度（不返回 NULL 结尾，长度显式传）。
const XTarget* target_table(int* count);

// 按名字查目标。接受三种写法：
//   完整三元组   "x86_64-linux-gnu"
//   简写         "linux" / "windows" / "macos"（取该平台的默认架构）
//   "host"       宿主平台（由 plat_host_triple() 决定）
//
// 查不到返回 NULL，**不做模糊匹配**——拼错一个字母就该报错，
// 而不是猜用户想要哪个。猜错的代价是编出跑不起来的产物。
const XTarget* target_find(const char* name);

// 宿主平台对应的目标。若宿主不在表里（罕见架构），返回 NULL。
const XTarget* target_host(void);

// 该目标是否为当前宿主（决定能否直接运行产物做自测）
bool target_is_host(const XTarget* t);

// 打印目标列表（x target list）
void target_print_table(FILE* out);

// ============================================================
// 产物路径
// ============================================================

// 由源文件路径推出默认产物路径。
//
//   Windows 目标：foo.xle → foo.exe
//   Linux/macOS：  foo.xle → foo
//
// 但交叉编译时必须加平台后缀，否则同一目录下为三个平台各编一次的
// 产物会互相覆盖，而且**看起来都叫 foo**——把 ELF 当 exe 双击
// 得到的是「不是有效的 Win32 应用程序」，这个错误信息毫无指向性。
//
// 规则：宿主目标用裸名，非宿主目标追加 "-<os>[-<arch>]"。
//   x build foo.xle --target=host            → foo.exe
//   x build foo.xle --target=x86_64-linux-gnu → foo-linux
//   x build foo.xle --target=aarch64-macos    → foo-macos-arm64
//
// out 由调用方提供，建议至少 1024 字节。
void target_output_name(const char* src_path, const XTarget* t,
                        char* out, size_t out_len);

#endif // X_TARGET_H