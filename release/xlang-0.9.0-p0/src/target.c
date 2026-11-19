// target.c — 目标平台表与产物格式识别
//
// 设计理由见 target.h。这个文件的核心主张只有一句：
// **可执行文件格式由操作系统决定，不是可选参数。**

#include "target.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
// 格式名
// ============================================================

const char* fmt_name(XObjFormat f) {
    switch (f) {
        case FMT_PE:    return "PE";
        case FMT_ELF:   return "ELF";
        case FMT_MACHO: return "Mach-O";
        default:        return "unknown";
    }
}

// ============================================================
// 文件头识别
// ============================================================

XObjFormat fmt_detect_file(const char* path) {
    // 用 x_fopen 而不是 fopen：路径可能是中文，直连 fopen 会 ENOENT
    FILE* f = x_fopen(path, "rb");
    if (!f) return FMT_UNKNOWN;

    unsigned char h[4] = {0};
    size_t n = fread(h, 1, 4, f);
    fclose(f);

    if (n < 4) return FMT_UNKNOWN;

    // ELF：0x7F 'E' 'L' 'F'
    if (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        return FMT_ELF;
    }

    // Mach-O：四种可能形态
    //   0xFEEDFACF 小端 64 位（当前 x86-64 / arm64 macOS 的绝大多数）
    //   0xCFFAEDFE 大端 64 位（同上，字节序换位，理论上不会遇到）
    //   0xFEEDFACE / 0xCEFAEDFE 32 位（老系统，保留判断）
    // 另外 0xCAFEBABE 是 universal binary（fat）——那是多个 Mach-O
    // 打包在一起，本质仍是 Mach-O 家族。
    if ((h[0] == 0xFE && h[1] == 0xED && h[2] == 0xFA && h[3] == 0xCF) ||
        (h[0] == 0xCF && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE) ||
        (h[0] == 0xFE && h[1] == 0xED && h[2] == 0xFA && h[3] == 0xCE) ||
        (h[0] == 0xCE && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE) ||
        (h[0] == 0xCA && h[1] == 0xFE && h[2] == 0xBA && h[3] == 0xBE)) {
        return FMT_MACHO;
    }

    // PE：前两字节 "MZ"，真正的 PE 签名在 e_lfanew 处。
    // 只查 "MZ" 会把 DOS 时代的 .exe 也算成 PE，但这个项目里不会遇到；
    // 完整校验要多读两次文件，收益为零。这里做「足够区分 ELF/Mach-O」即可。
    if (h[0] == 'M' && h[1] == 'Z') {
        return FMT_PE;
    }

    return FMT_UNKNOWN;
}

// ============================================================
// 目标表
// ============================================================
//
// 为什么只列这几个：这是**当前能真实产出并验证**的集合。
// 表里每多一行就多一个「声称支持但没测过」的条目，那比不列更糟——
// 用户按表操作、得到跑不起来的产物，从此不信任整个文档。
//
// Android / iOS / WASM / 裸机在 02 §9 有完整设计，但它们需要
// profile 系统与对应的运行时（无 libc、无展开表），不是改个
// 编译器前缀就能成立的，所以暂不入表。

static const XTarget g_targets[] = {
    // triple                       arch       os         abi      format      suffix  cc_prefix                  note
    { "x86_64-windows-msvc",   "x86_64",  "windows", "msvc",  FMT_PE,    ".exe", "",
      "Windows 64 位，原生（MSVC 运行时）" },
    { "x86_64-windows-mingw",  "x86_64",  "windows", "mingw", FMT_PE,    ".exe", "",
      "Windows 64 位，MinGW（本机 GCC 默认走这条）" },
    { "aarch64-windows-msvc",  "aarch64", "windows", "msvc",  FMT_PE,    ".exe", "",
      "Windows on ARM64" },

    { "x86_64-linux-gnu",      "x86_64",  "linux",   "gnu",   FMT_ELF,   "", "x86_64-linux-gnu-",
      "Linux 64 位，动态链接 glibc" },
    { "x86_64-linux-musl",     "x86_64",  "linux",   "musl",  FMT_ELF,   "", "x86_64-linux-musl-",
      "Linux 64 位，静态链接 musl（单文件可分发）" },
    { "aarch64-linux-gnu",     "aarch64", "linux",   "gnu",   FMT_ELF,   "", "aarch64-linux-gnu-",
      "Linux ARM64（服务器 / 树莓派）" },
    { "aarch64-linux-musl",    "aarch64", "linux",   "musl",  FMT_ELF,   "", "aarch64-linux-musl-",
      "Linux ARM64，静态链接" },

    { "x86_64-macos",          "x86_64",  "macos",   "",      FMT_MACHO, "", "x86_64-apple-darwin-",
      "macOS Intel" },
    { "aarch64-macos",         "aarch64", "macos",   "",      FMT_MACHO, "", "aarch64-apple-darwin-",
      "macOS Apple Silicon（M 系列）" },
};

const XTarget* target_table(int* count) {
    if (count) *count = (int)(sizeof(g_targets) / sizeof(g_targets[0]));
    return g_targets;
}

// ============================================================
// 查询
// ============================================================

const XTarget* target_find(const char* name) {
    if (!name) return NULL;

    // "host" 是一个特殊名字：指宿主平台。
    // 它的价值在于让构建脚本可以写 `--target=host` 而不必知道
    // 自己在什么机器上跑——这正是「跨平台」最实用的形态。
    if (strcmp(name, "host") == 0) {
        return target_host();
    }

    // 完整三元组优先
    int n = 0;
    const XTarget* t = target_table(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].triple, name) == 0) return &t[i];
    }

    // 平台简写：取该平台的默认架构（x86_64 优先，没有则第一个）
    // 这不是模糊匹配 —— "linux" 是个明确的、有文档的缩写，
    // 而 "linu" 之类仍然会报错。
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].os, name) != 0) continue;
        if (strcmp(t[i].arch, "x86_64") == 0) return &t[i];
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].os, name) == 0) return &t[i];
    }

    // 架构简写："arm64" / "x64" 这类习惯写法映射到标准名
    // （只做无歧义的映射：arm64 → aarch64 是全行业同义）
    const char* norm = NULL;
    if (strcmp(name, "arm64") == 0) norm = "aarch64";
    if (norm) {
        for (int i = 0; i < n; i++) {
            if (strcmp(t[i].arch, norm) == 0 && target_is_host(&t[i])) return &t[i];
        }
    }

    return NULL;
}

const XTarget* target_host(void) {
    const char* host = plat_host_triple();
    int n = 0;
    const XTarget* t = target_table(&n);

    // 精确匹配
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].triple, host) == 0) return &t[i];
    }

    // Windows 上宿主三元组是 "-windows-msvc"，但本机实际用的是
    // MinGW 工具链。这两者在**产物格式上完全一样**（都是 PE），
    // 差别只在 ABI 细节，而我们生成的是 C 代码再交给工具链，
    // 不直接处理 ABI。所以按操作系统 + 架构匹配即可。
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].arch, host) == 0) continue;   // 不可能，防呆
    }
    for (int i = 0; i < n; i++) {
        if (strstr(host, t[i].os) == NULL) continue;
        if (strncmp(host, t[i].arch, strlen(t[i].arch)) != 0) continue;
        return &t[i];
    }

    return NULL;
}

bool target_is_host(const XTarget* t) {
    if (!t) return false;
    const XTarget* h = target_host();
    if (!h) return false;
    // 同一平台 + 同一架构 = 能在本机运行，就可以直接跑产物做自测。
    // ABI 不同（msvc vs mingw）不影响「能不能跑」——PE 就是 PE。
    return strcmp(t->os, h->os) == 0 && strcmp(t->arch, h->arch) == 0;
}

// ============================================================
// 打印
// ============================================================

void target_print_table(FILE* out) {
    int n = 0;
    const XTarget* t = target_table(&n);
    const XTarget* h = target_host();

    fprintf(out, "已知目标（* = 当前宿主，可直接运行产物）\n\n");
    fprintf(out, "  %-24s %-9s %-8s %-7s %s\n",
            "三元组", "格式", "产物后缀", "", "说明");
    fprintf(out, "  ------------------------------------------------------------------------\n");

    for (int i = 0; i < n; i++) {
        bool is_host = h && strcmp(t[i].triple, h->triple) == 0;
        fprintf(out, "%s %-24s %-9s %-8s %s\n",
                is_host ? " *" : "  ",
                t[i].triple,
                fmt_name(t[i].format),
                t[i].exe_suffix[0] ? t[i].exe_suffix : "（无）",
                t[i].note);
    }
    fprintf(out, "\n");
    fprintf(out, "  简写：--target=windows / linux / macos 取该平台 x86_64\n");
    fprintf(out, "        --target=host 取当前宿主平台\n");
    fprintf(out, "  格式由操作系统决定，不可单独指定：\n");
    fprintf(out, "        Windows → PE    Linux → ELF    macOS → Mach-O\n");
}

// ============================================================
// 产物路径
// ============================================================

void target_output_name(const char* src_path, const XTarget* t,
                        char* out, size_t out_len) {
    // 取源文件的「主干名」：去掉目录与后缀
    const char* base = src_path;
    for (const char* p = src_path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    const char* dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);

    if (target_is_host(t)) {
        // 宿主目标：裸名 + 后缀，与直接 gcc -o foo 的直觉一致
        snprintf(out, out_len, "%.*s%s", (int)stem_len, base, t->exe_suffix);
        return;
    }

    // 交叉目标：必须带平台标识，否则三个平台的产物会互相覆盖，
    // 而且名字完全一样 —— 把 ELF 当 .exe 双击得到的错误信息
    // 是「不是有效的 Win32 应用程序」，完全不指向真正的原因。
    //
    // 命名取 os 的短形式，架构只在非 x86_64 时附加（x86_64 是绝大多数
    // 平台的历史默认，附加反而让名字变长且没有信息量）。
    const char* os_short = t->os;
    if (strcmp(t->os, "windows") == 0) os_short = "win";
    if (strcmp(t->os, "macos") == 0)   os_short = "mac";

    if (strcmp(t->arch, "x86_64") == 0) {
        snprintf(out, out_len, "%.*s-%s%s", (int)stem_len, base, os_short,
                 t->exe_suffix);
    } else {
        snprintf(out, out_len, "%.*s-%s-%s%s", (int)stem_len, base, os_short,
                 t->arch, t->exe_suffix);
    }
}