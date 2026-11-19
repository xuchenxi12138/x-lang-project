// main.c — x 命令行入口（P0）
//
// 确定源文件后缀（v0.13）：
//   .xle  X Language English  —— 本体（英文关键字）
//   .xlc  X Language Chinese  —— 中文编程，**后缀即声明**，自动开启中文模式
//   .xbc                       —— 字节码
//
// ── 为什么 v0.13 换了后缀 ────────────────────────────────────
//
// 旧后缀 `xla`（前缀 x + 后缀 la）与 Excel 的加载项格式（Excel
// Add-in）完全撞名。Windows 上双击它会被 Excel 抢走；邮件网关、
// 网盘、杀毒软件也按 Excel 加载项处理 —— 有些工厂/金融环境里
// 加载项类文件会被直接拦截（因为它是 VBA 宏的常见载体）。
//
// 用户在自己的项目里看到「X 源文件」挂着 Excel 图标，或者源文件
// 根本发不出去，这都不是「小瑕疵」。后缀是给操作系统和第三方
// 工具看的契约，撞名就得让位。
//
// 新后缀 `.xle` 顺带把对称性补齐了：e = English、c = Chinese，
// 两个字面直接对应，不用记。而且 .xle/.xlc 在任何平台的已知
// 格式表里都不冲突。
//
// 为什么用两个后缀而不是「一个后缀 + --zh 开关」：
//   铁律 14「每个名字自带它是什么」要求一个文件被单独摘出来
//   （日志片段、grep 结果、issue 里的代码块）时还能看出它是什么。
//   如果中文代码的文件也叫 .x，编辑器、LSP、构建脚本、
//   代码审查者都必须靠读内容才知道该用哪种词法 —— 那就退回了
//   「靠位置信息区分」，正是 01 §11.1 明确否决的做法。
//
//   注意 .xle / .xlc 不是同一种语言的两个方言，而是**同一个词法器的
//   两个入口**：04 §2.5 已证明中文别名在词法层归一化后，
//   后续所有阶段看不到差别，字节码逐字节一致。
//
// 命令集（与 03-标准库与包管理.md §6.8 命名一致）：
//
//   x run <file.xle>            编译并立即执行（解释档）
//   x build <file.xle> [--emit] 只编译，产出 .xbc 字节码
//   x disasm <file.xle>         打印字节码反汇编，用于调试与差分测试
//   x check <file.xle>          只做语法与语义检查，不执行
//   x pkg <init|add|remove|list|tree|install|update|lock|registry>
//                               包管理（v0.13 起，v0.14 加注册表与 lock）
//   x --version                 版本
//   x --help                    帮助
//
// 后续阶段会加：x fmt / x test / x doc / x repl（repl 已裁决不做）

#include "x.h"
#include "registry.h"
#include "lockfile.h"
#include "lsp.h"
#include <stdarg.h>
#include <errno.h>

#define X_VERSION "0.9.0-p0"

// 后缀常量：集中定义，避免各处硬编码拼错
#define SUFFIX_XLE  ".xle"
#define SUFFIX_XLC  ".xlc"
#define SUFFIX_XBC  ".xbc"

static bool ends_with(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

// 是不是 X 的源文件（不含字节码）
static bool is_source_file(const char* path) {
    return ends_with(path, SUFFIX_XLE) || ends_with(path, SUFFIX_XLC);
}

// .xlc 隐含中文模式。显式 --zh 与此不冲突（都是开，没有「关」的情况）。
static bool suffix_implies_zh(const char* path) {
    return ends_with(path, SUFFIX_XLC);
}

static void usage(FILE* out) {
    fprintf(out,
        "x %s — X 语言编译与运行时\n"
        "\n"
        "用法:\n"
        "  x run    <file>               编译并解释执行\n"
        "  x build  <file> [-o out]      编译（默认产字节码，--emit=exe 产原生可执行文件）\n"
        "  x disasm <file>               打印字节码反汇编\n"
        "  x check  <file>               只检查，不执行\n"
        "  x target list                 列出可编译的目标平台与产物格式\n"
        "  x profile list                列出使用场景（console / desktop / service）\n"
        "\n"
        "包管理:\n"
        "  x pkg init                    在当前目录生成 x.toml\n"
        "  x pkg add <name> --path <dir> 声明一个本地路径依赖\n"
        "  x pkg remove <name>           删掉一条依赖（其它内容逐字节保留）\n"
        "  x pkg list                    列出本包的声明与全部依赖\n"
        "  x pkg tree                    打印依赖树\n"
        "  x pkg install                 按约束解析版本，写 x.lock（幂等）\n"
        "  x pkg update                  强制重新选版本（升级）\n"
        "  x pkg lock                    检查 x.lock 与当前依赖是否一致\n"
        "  x pkg registry                看注册表里有哪些包和版本\n"
        "\n"
        "  用起来是这样的五步：\n"
        "    1. x pkg init                      建清单\n"
        "    2. x pkg add mathutil --path ../mathutil   声明依赖\n"
        "    3. 在代码里写    import @mathutil            引用\n"
        "    4. x build src/main.xle            编译时自动解析到\n"
        "                                       ../mathutil/src/lib.xle\n"
        "    5. x pkg tree                      看依赖树确认\n"
        "\n"
        "  清单查找是「从源文件所在目录向上找 x.toml」，与你在哪个目录\n"
        "  敲命令无关 —— 所以 cd src && x build main.xle 和\n"
        "  x build src/main.xle 结果一样。\n"
        "\n"
        "依赖有两种写法:\n"
        "  util  = { path = \"../util\" }   本地路径，版本由文件系统决定\n"
        "  jsonx = \"^1.2\"                  注册表，按版本约束选最高满足版本\n"
        "\n"
        "  版本约束: 1.2.3（等价 ^1.2.3）、^1.2.3、~1.2.3、=1.2.3、\n"
        "            >=1.2.0、<2.0.0、*\n"
        "\n"
        "  注册表是**本地目录**，默认在 x.exe 旁边的 packages/，可用\n"
        "  环境变量 X_REGISTRY 改。布局就是个约定，可以手工往里放:\n"
        "      packages/包名/版本/src/lib.xle\n"
        "      packages/包名/版本/x.toml       （声明它自己依赖什么）\n"
        "  于是「x.exe + stdlib + packages 拷到哪都能用」—— 与标准库\n"
        "  同一条思路。\n"
        "\n"
        "  x.lock 记录**上次解析选定的版本**，是可复现构建的依据，\n"
        "  应该进版本库。install 不会自动升级已有依赖（有 lock 就按\n"
        "  lock 走）；要升级得显式 `x pkg update`。\n"
        "\n"
        "  未在清单里声明的 import @name 会**报错**（位置指向那句 import，\n"
        "  不是指向清单），依赖源码缺失会报错并列出试过的所有路径。\n"
        "  远程下载、签名、发布（fetch / publish）尚未实现，相关命令\n"
        "  会明确说「尚未实现」而不是假装成功。\n"
        "\n"
        "文件后缀:\n"
        "  .xle    X 源文件（英文关键字，e = English）\n"
        "  .xlc    X 源文件（中文关键字，c = Chinese，自动开中文模式）\n"
        "  .xbc    字节码，可直接 run / disasm\n"
        "\n"
        "  注意：v0.13 起英文源文件后缀是 .xle，旧后缀 .xla 已废弃 ——\n"
        "  它与 Excel 加载项格式撞名，会被 Windows/邮件网关按 Excel\n"
        "  文件处理。旧项目需要批量改名。\n"
        "\n"
        "选项:\n"
        "  -o <path>            指定输出路径\n"
        "  --emit=<kind>        产物类型：bc（字节码，默认）或 exe（原生可执行文件）\n"
        "  --target=<triple>    目标平台，见 `x target list`。默认 host\n"
        "  --profile=<name>     使用场景，见 `x profile list`。默认 console\n"
        "  --cc=<path>          指定 C 编译器（--emit=exe 时用）\n"
        "  --no-color           关闭 ANSI 颜色与可点击链接（06 §4 的兜底模式）\n"
        "  --version            打印版本\n"
        "  --help               打印本帮助\n"
        "\n"
        "关于产物格式（这是个常见困惑）：\n"
        "  格式不由你选，由目标操作系统决定 —— Windows 只能是 PE(.exe)，\n"
        "  Linux 只能是 ELF，macOS 只能是 Mach-O。所以参数是 --target\n"
        "  而不是 --format：提供一个在目标机器上跑不起来的格式没有意义。\n"
        "\n"
        "关于使用场景（--profile）：\n"
        "  场景决定链路，在执行之前定死，运行期不做派发。三档的真实差别：\n"
        "    console  控制台。不链窗口运行时，PE 子系统 3，stdout 行缓冲\n"
        "    desktop  桌面。内嵌窗口运行时 + 链 gdi32/user32，PE 子系统 2\n"
        "             （双击运行不弹控制台黑框），栈深加倍\n"
        "    service  服务。stdout 无缓冲 —— 进程被杀时已输出的不会丢\n"
        "\n"
        "  子系统这个字段写在文件头里，由系统加载器读，运行期没有任何办法\n"
        "  改，所以它只能是编译期选项。这就是「场景必须在执行之前定死」的\n"
        "  物理来源，不是一条设计偏好。\n"
        "\n"
        "  也可以写在 x.toml 里，让 clone 下来的人直接 build 就对：\n"
        "      [package]\n"
        "      profile = \"desktop\"\n"
        "  优先级：命令行 --profile > x.toml > 默认 console。\n"
        "\n"
        "退出码:\n"
        "  0    成功\n"
        "  1    编译错误（语法/语义）\n"
        "  70   运行时错误（如整数除零、未捕获异常）\n"
        "\n",
        X_VERSION);
}

// ============================================================
// 编译流水线（run / build / disasm / check 共用）
// ============================================================
// 顺序不能变（05 文档 §6 的硬性要求）：
//   词法 → 语法 → AST → 降级 → 字节码
// 任何一步出错就停下，不把错误往下传。

typedef struct {
    char*     src;        // 源码全文
    size_t    src_len;
    Token*    toks;
    size_t    tok_count;
    Module*   module;
    Bytecode  bc;
    Arena     arena;
    bool      ok;
    // 包清单。放在这里而不是 ModGraph 里：它的生命周期比加载过程长 ——
    // 后面 --profile 的默认值、`x pkg tree` 之类都要用。由
    // compilation_free 统一释放在最省心的位置。
    PkgManifest manifest;
    bool        has_manifest;
} Compilation;

// ============================================================
// 模块加载（P0：文本级合并）
// ============================================================
//
// ── 语义 ──────────────────────────────────────────────────
//
// `import "board.xle"` 的行为是：解析那个文件，把它的 fn 声明
// 并入当前编译单元。之后调用 board.xle 里的函数就像它们写在
// 本文件里一样 —— 没有前缀、没有命名空间。
//
// ── 三条硬规则 ────────────────────────────────────────────
//
// 1. **被 import 的文件只能有 fn 声明**。顶层语句会被合成为 main，
//    多个文件都有顶层语句就会撞车。与其在合成阶段报一个指向
//    别处的错，不如在加载时就说清楚。
//
// 2. **路径相对当前文件解析**，不是相对工作目录。这样 `x run
//    games/minesweeper/main.xle` 和 `cd games/minesweeper && x run
//    main.xle` 行为一致 —— 后者是用户最容易做的事。
//
// 3. **循环 import 直接报错**。文本合并下 A import B、B import A
//    会让函数重复定义；提前拦住能给出准确的环路径而不是一堆
//    重定义错误。
//
// ── 为什么这不算「真正的模块系统」 ─────────────────────────
//
// 没有导出表、没有可见性、没有独立编译、没有名称修饰。真正做到
// 那些需要一套中间表示与链接流程，01 §7 把它排在 P1。
// P0 只需要「把几个文件拼起来」，文本合并能满足且只有几十行。

// 一次编译最多合并多少个源文件。
//
// ── 为什么从 32 提到 256 ────────────────────────
//
// 原值 32 是按「一个程序 import 十来个别文件」的想象定的。但
// 「一个类一个文件、按职责分目录」这种工程化组织，很快就撞上
// 它 —— 一个 UI 组件库光 core/ 就要 6 个文件，加上 control/、
// layout/、display/ 轻松过 30。
//
// 而这个上限本身并不防什么：文本合并的开销是 O(总字节数)，
// 不是 O(文件数)。32 个文件如果各 10KB，开销和 320 个 1KB
// 文件差不多。真正该防的是「总字节数爆炸」，那由另外的检查
// （单文件大小、token 数）负责。
//
// 所以提到 256：留足工程化空间，同时仍能挡住「误把整个
// node_modules 拖进来」这种事故。每个 ModFile 只有 32 字节
// （指针 + 指针 + 2 个 bool + 对齐），256 个是 8KB 栈上空间，
// 完全不是问题。
#define MOD_MAX_FILES 256

typedef struct {
    char*   path;           // 已解析的绝对路径（用于去重与报错）
    Module* mod;
    bool    loaded;         // 是否已完全处理（防重入）
    bool    visiting;       // 正在处理中（检测环）
} ModFile;

typedef struct {
    ModFile files[MOD_MAX_FILES];
    int     count;
    // 合并结果：所有文件的函数放到一个 Module 里，顶层语句保持
    // 只有主文件有（规则 1 保证）
    Module* merged;
    Arena*  arena;
    // 包清单。为 NULL 表示「不是包项目」（没有 x.toml）——
    // 单文件程序与 games/ 里的例子都属于这种，它们照常工作，
    // 只是不能用 `import @name`。
    const PkgManifest* manifest;
} ModGraph;

// 把 base 的目录部分与 rel 拼起来，规范化成绝对路径。
//
// 为什么不直接用拼接结果：`a/../b.xle` 与 `b.xle` 是同一个文件，
// 但字节不同 —— 去重就失效了，同一个文件被加载两次，于是函数
// 重复定义。所以走一次 plat_abs_path（它内部会处理 . 和 ..）。
static void resolve_path(const char* base_file, const char* rel,
                         char* out, size_t out_len) {
    char dir[900];
    snprintf(dir, sizeof(dir), "%s", base_file);

    // 找最后一个路径分隔符。/ 和 \ 都要认 —— Windows 上两种都常见。
    char* slash = strrchr(dir, '/');
    char* bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) {
        *(slash + 1) = '\0';       // 保留分隔符，方便直接拼
    } else {
        dir[0] = '\0';             // 没有目录部分：相对当前工作目录
    }

    char joined[1400];
    snprintf(joined, sizeof(joined), "%s%s", dir, rel);
    plat_abs_path(joined, out, out_len);
}

// ============================================================
// 标准库定位
// ============================================================
//
// `import std.json` 要落到一个**确定的**文件上。候选顺序：
//
//   1. $X_STDLIB_PATH           —— 显式覆盖，开发和测试用
//   2. <exe 所在目录>/stdlib/    —— 正式发布形态：x.exe 与 stdlib 同放
//   3. <exe 所在目录>/../stdlib/ —— 开发形态：build/x.exe 与 ../stdlib
//   4. <cwd>/stdlib/            —— 在项目根跑命令时
//
// ── 为什么不只用 cwd ────────────────────────────────────────
//
// 用户的核心诉求是「x.exe 和标准库放一个文件夹，import 就能用」。
// 按 cwd 找会在两种最常见的场景下失败：
//   · cd 到子目录跑（cd games && x run main.xle）
//   · 用绝对路径从别处调用
// 而按 exe 位置找，拷到哪都能用 —— 这才是「标准库」该有的性质。
//
// ── 为什么保留 cwd 作为最后一项 ─────────────────────────────
//
// 源码树里 build/x.exe 与 stdlib/ 是兄弟目录，第 3 项已经覆盖。
// 但有人会把 x.exe 单独拷到项目里试用，此时 exe 旁边没有 stdlib，
// cwd 兜底能让那种临时用法跑起来。顺序在最后，不影响正式形态。
//
// 返回值：找到返回 true 并填 out（不含结尾分隔符）。
static bool find_stdlib_root(char* out, size_t out_len) {
    const char* env = getenv("X_STDLIB_PATH");
    if (env && env[0]) {
        // 环境变量直接当根目录用，不再拼 stdlib —— 变量的语义就是
        // 「标准库在哪」，再拼一层会让用户写 X_STDLIB_PATH=<root>/stdlib
        // 还是 <root> 变成一个需要试错的问题。
        plat_abs_path(env, out, out_len);
        if (x_is_dir(out)) return true;
        // 显式指定了却不存在：这是错误，不该静默回退。
        // 回退的后果是用户在错误的目录里调了半天，而真相是他的
        // 环境变量写错了 —— 一条明确的诊断能省掉这个过程。
        fprintf(stderr, "error: X_STDLIB_PATH 指向的目录不存在：%s\n", env);
        fprintf(stderr, "help: 该变量应指向包含 math.xle / json.xle 的目录\n");
        return false;
    }

    char exe_dir[1200];
    if (plat_exe_dir(exe_dir, sizeof(exe_dir)) && exe_dir[0]) {
        char cand[1600];

        // ── 第 1 项：源码树布局 `<exe 目录>/stdlib` ─────────────
        //
        // 开发时的形态：build/x.exe 与 stdlib/ 是兄弟目录。
        snprintf(cand, sizeof(cand), "%s/stdlib", exe_dir);
        if (x_is_dir(cand)) {
            if (strlen(cand) >= out_len) return false;
            memcpy(out, cand, strlen(cand) + 1);
            return true;
        }

        // ── 第 2 项：发行版布局 `<exe 目录>/../lib` ────────────
        //
        // 安装后的形态（bin/ 与 lib/ 并列，见 release/ 的目录说明）：
        //
        //     xlang/
        //     ├── bin/x.exe
        //     └── lib/stdlib/*.xle
        //
        // 这一项必须排在 `../stdlib` **之前**。两者都指向「上一层」，
        // 但 `../stdlib` 是源码树的直觉，而 `lib/` 是安装式的直觉 ——
        // 一个发行包里同时有 lib/stdlib 和 stdlib 是不可能的（那就
        // 重复了），所以顺序其实不冲突，但明确写出来省得以后有人
        // 把 stdlib 直接放在 xlang/ 下时踩到。
        snprintf(cand, sizeof(cand), "%s%c..%clib%cstdlib",
                 exe_dir, PLAT_SEP, PLAT_SEP, PLAT_SEP);
        plat_abs_path(cand, out, out_len);
        if (x_is_dir(out)) return true;

        // ── 第 3 项：扁平式 `<exe 目录>/lib` ────────────────
        //
        // 有人会把可执行文件和库都放在同一层（bin 省略掉），
        // 即 xlang/x.exe + xlang/lib/stdlib/。这一项兜住它。
        snprintf(cand, sizeof(cand), "%s%clib%cstdlib",
                 exe_dir, PLAT_SEP, PLAT_SEP);
        plat_abs_path(cand, out, out_len);
        if (x_is_dir(out)) return true;

        // ── 第 4 项：`<exe 目录>/../stdlib`（源码树的另一种摆法）──
        //
        // Windows 上路径分隔符两种都能用，但拼出来要跟平台习惯一致，
        // 否则某些老工具会把混合分隔符的路径当成相对路径。
        snprintf(cand, sizeof(cand), "%s%c..%cstdlib", exe_dir, PLAT_SEP, PLAT_SEP);
        plat_abs_path(cand, out, out_len);
        if (x_is_dir(out)) return true;
    }

    if (plat_getcwd(exe_dir, sizeof(exe_dir))) {
        char cand[1600];
        snprintf(cand, sizeof(cand), "%s/stdlib", exe_dir);
        if (x_is_dir(cand)) {
            if (strlen(cand) >= out_len) return false;
            memcpy(out, cand, strlen(cand) + 1);
            return true;
        }
    }

    out[0] = '\0';
    return false;
}

// 把一个点路径模块名映射到标准库文件。
//   "std.json"    → <root>/json.xle
//   "std.db.sql"  → <root>/db/sql.xle
// 点换成路径分隔符，最后补 .xle。
static bool resolve_std_module(const char* mod_name, char* out, size_t out_len) {
    char root[1200];
    if (!find_stdlib_root(root, sizeof(root))) return false;

    // 跳过 "std" 前缀
    const char* rest = mod_name;
    if (strncmp(rest, "std.", 4) == 0) rest += 4;
    else if (strcmp(rest, "std") == 0) rest = "index";   // import std 等价于 std.index
    else return false;

    // 逐字符拷贝，点转分隔符。同时挡掉 .. 这种穿越 —— 标准库路径
    // 不该能跳出 stdlib 目录，否则 `import std...etc.passwd` 这种写法
    // 会变成一个路径穿越入口。
    char rel[600];
    size_t w = 0;
    for (const char* p = rest; *p && w + 2 < sizeof(rel); p++) {
        if (*p == '.') {
            // 连续的 .. 直接拒绝
            if (p[1] == '.' || (w > 0 && rel[w - 1] == '.')) return false;
            rel[w++] = PLAT_SEP;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
            rel[w++] = *p;
        } else {
            return false;   // 非法字符一律拒绝，不做「尽量解析」
        }
    }
    rel[w] = '\0';
    if (w == 0) return false;

    // 先算长度再拷：snprintf 截断会**静默成功**，得到一个少了一截的
    // 路径，然后报「找不到标准库」—— 而用户的标准库其实是好的，
    // 只是路径太长。显式拒绝比猜一个好。
    if (strlen(root) + 1 + w + 5 >= out_len) return false;
    {
        size_t pos = 0;
        memcpy(out + pos, root, strlen(root));
        pos += strlen(root);
        out[pos++] = PLAT_SEP;
        memcpy(out + pos, rel, w);
        pos += w;
        memcpy(out + pos, ".xle", 5);   // 含 NUL
    }

    // ── 必须实际存在才算解析成功 ────────────────────────────
    //
    // 只拼出路径不算数。若不在这里查存在性，失败会推迟到下一层
    // 的「无法打开文件：...\stdlib\nonexistent.xle」—— 那条错误
    // 只说了事实，没说「标准库在哪找、怎么查有哪些模块、能不能
    // 用环境变量改」，用户拿不到任何下一步动作。
    //
    // 在这里挡掉，调用方就能给出完整诊断。**能用一次检查消除的
    // 延迟错误，就不该让它延迟** —— 延迟只增加定位成本，不增加信息。
    if (x_file_exists(out)) return true;

    // ── 目录形式的回退：`std.ui` → <root>/ui/index.xle ─────────
    //
    // ── 为什么需要这条路 ──────────────────────────────────
    //
    // 「一个类一个文件、按职责分目录」是工程化的基本要求，而它
    // 直接导出一个需求：模块可以是一个**目录**，入口固定为目录
    // 下的 index.xle。
    //
    // 没有这条回退时，`import std.ui` 只能指向单个 ui.xle ——
    // 于是要么把 30 个组件塞进一个文件（工程化灾难），要么让
    // 调用方写 `import std.ui.index`（把内部结构暴露给使用者，
    // 而且改名就没法平滑迁移）。
    //
    // ── 解析顺序：先文件、后目录 ──────────────────────────
    //
    // 先试 ui.xle，存在就用它 —— 这保证了向后兼容：已经把
    // 多个模块写成扁平文件的库不会被意外改道。只有扁平文件
    // 不存在时才找目录入口。
    //
    // 这个顺序也符合「显式的优先」：一个具体文件比一个目录
    // 里的约定入口更明确。
    {
        // 把 ".xle" 换成 dir + "/index.xle"。
        // out 当前是 ".../ui.xle"，w 是相对路径长度（不含 .xle）。
        size_t base_len = strlen(root) + 1 + w;
        static const char k_idx[] = "/index.xle";
        size_t need = base_len + sizeof(k_idx);   // 含 NUL
        if (need < out_len) {
            char trial[1200];
            memcpy(trial, out, base_len);
            trial[base_len] = '\0';
            // 用平台分隔符而不是硬编码 '/'，Windows 上两者都能用，
            // 但拼出来的路径会进入错误信息，一致更易读。
            snprintf(trial + base_len, sizeof(trial) - base_len,
                     "%cindex.xle", PLAT_SEP);
            if (x_file_exists(trial)) {
                memcpy(out, trial, strlen(trial) + 1);
                return true;
            }
        }
    }

    // 都不存在：失败。调用方会给「有哪些模块」的提示。
    //
    // 大小写修正：Windows 文件系统不区分大小写，但 Linux 区分。
    // `import std.JSON` 在 Windows 上会「能用」，到 Linux 就找不到 ——
    // 这种平台差异最难查，因为它只在换机器时出现。这里不猜，
    // 让它在哪个平台都一致地失败，用户当场改对。
    return false;
}

// 列出标准库模块（x stdlib）。
//
// 为什么要这个命令：标准库的价值一半在「有」，一半在「用户知道有什么」。
// 没有列表，用户只能靠文档或者试错 —— 而试错的成本是编译一次才知道
// 模块名写错没有，且错误信息只能告诉他「找不到」。
//
// 输出同时给出 import 写法，用户可以直接抄。路径也印出来，
// 排查「为什么找不到」时能立刻看出是不是指到了别处的 stdlib。
static int stdlib_list(const char* root) {
    // 收集名字后排序输出。目录遍历顺序由文件系统决定，
    // 不排序的话每次跑出来顺序可能不同 —— 用户会以为是模块变多了。
    char names[64][128];
    int n = 0;

    PlatDir d;
    if (!plat_dir_open(&d, root, "*.xle")) {
        printf("标准库目录为空或无法读取：%s\n", root);
        return 0;
    }
    while (plat_dir_next(&d)) {
        if (d.is_dir) continue;
        const char* base = d.name;
        const char* dot = strrchr(base, '.');
        if (!dot || strcmp(dot, ".xle") != 0) continue;
        size_t len = (size_t)(dot - base);
        if (len == 0 || len >= sizeof(names[0])) continue;
        if (n >= (int)(sizeof(names) / sizeof(names[0]))) break;
        memcpy(names[n], base, len);
        names[n][len] = '\0';
        n++;
    }
    plat_dir_close(&d);

    // 简单插入排序：n 最多几十，不值得引入 qsort 比较函数
    for (int i = 1; i < n; i++) {
        char key[128];
        memcpy(key, names[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            memcpy(names[j + 1], names[j], sizeof(names[0]));
            j--;
        }
        memcpy(names[j + 1], key, sizeof(key));
    }

    printf("标准库：%s\n\n", root);
    if (n == 0) {
        printf("  （空）\n");
        printf("\n提示：标准库应与 x.exe 放在同一目录下的 stdlib/ 里。\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        printf("  import std.%s\n", names[i]);
    }
    printf("\n共 %d 个模块。\n", n);
    return 0;
}

// 加载并解析一个源文件（不含 import 展开）。
//
// 这是 compile_file 原来那段「读文件 → 校验编码 → 词法 → 语法」的
// 原样提取，抽出来是为了能被模块加载器对每个被 import 的文件复用。
//
// out_src 与 out_toks 由调用方负责 free —— 它们不能放进 arena，
// 因为 read_file 用的是 malloc。
static bool load_one_file(const char* path, Arena* arena,
                          char** out_src, size_t* out_src_len,
                          Token** out_toks, size_t* out_tok_count,
                          Module** out_mod) {
    strace_enter("load_one_file");
    *out_src = NULL;
    *out_toks = NULL;
    *out_mod = NULL;

    // ---- 后缀检查与中文模式判定 ----
    // 后缀既是「这是什么文件」的声明，也是中文模式的开关。
    // 不认识的后缀直接拒绝，而不是猜——猜错会让用户拿到一堆
    // 莫名其妙的语法错误（比如把 .txt 当源码喂进来）。
    if (!is_source_file(path)) {
        fprintf(stderr,
                "error: `%s` 不是 X 的源文件\n"
                "  note: 源文件后缀必须是 .xle（英文）或 .xlc（中文）；"
                "字节码是 .xbc\n"
                "  help: 把文件改名为 .xle 或 .xlc 即可。"
                "从 v0.12 及以前升级的项目需要批量改名："
                ".xle → .xle（旧后缀与 Excel 加载项格式冲突），"
                ".x → .xle/.xlc（v0.7 及以前）\n",
                path);
        return false;
    }

    // .xlc 隐含中文模式。这里是全局词法器开关，所以必须在 lex_all 之前设。
    //
    // 关键细节：**每次都显式设置，不依赖默认值**。
    // 因为这是全局状态，一个进程里若先后编译 .xlc 和 .xle（本项目
    // 的扫雷就有 .xle 和 .xlc 混用的情况），残留的 true 会让英文文件
    // 被按中文词法解析 —— 那种 bug 表现为「英文文件里 `如果` 报错」
    // 或「`Int` 变成未定义类型」，极难定位。
    // 所以这里写死「根据当前文件的后缀决定」，而不是「只在 .xlc 时开」。
    lexer_set_zh_mode(suffix_implies_zh(path));

    // ---- 1. 读文件 ----
    char* src = read_file(path, out_src_len);
    if (!src) return false;
    *out_src = src;

    // ---- 1.5 校验编码：源文件必须是 UTF-8（01 §2.1）----
    //
    // 不做这一步的后果实测过：一个 GBK 编码的 .xlc 文件里「令」是 c1 ee、
    // 「姓名」是 d0 d5 c3 fb，这些字节凑巧大都能构成合法 UTF-8 序列，于是
    // 词法器不报编码错，而是把它们当乱码标识符，最终输出：
    //   error: 未定义的变量 ``
    //   error: 给未定义的变量 `` 赋值
    // 用户看到的是两个空白名字和一个「未定义」——真实原因（文件存成了 GBK）
    // 完全没被提及。所以在词法分析之前就把编码问题拦下来，直接说清楚。
    size_t bad_off = 0;
    int bad_line = 0, bad_col = 0;
    if (!utf8_validate(src, *out_src_len, &bad_off, &bad_line, &bad_col)) {
        diag_error((SrcPos){ path, bad_line, bad_col },
                   "源文件不是合法的 UTF-8：第 %zu 个字节处（0x%02X）无法解码",
                   bad_off, (unsigned char)src[bad_off]);
        diag_note("01 §2.1 规定 X 的源文件必须是 UTF-8。"
                  "这个文件很可能是用 GBK/GB2312 保存的——"
                  "简体中文 Windows 上记事本与部分编辑器默认就是 GBK。");
        diag_help("用编辑器的「另存为」把编码改成 UTF-8（带不带 BOM 都可以），"
                  "或在 VS Code 右下角点编码名 → Save with Encoding → UTF-8");
        return false;
    }

    // ---- 2. 词法 ----
    //
    // 注意 alloca 风格的问题：Lexer 拿到的 path 指针会存进每个 token
    // 的 SrcPos 里。所以 path 必须活得够久 —— 这里要求调用方传的
    // path 生命周期长于整个编译过程（main.c 里用的是 arena 或 argv，
    // 都满足）。
    Lexer lx;
    lexer_init(&lx, path, src, *out_src_len, arena);
    *out_toks = lex_all(&lx, out_tok_count);
    if (g_error_count > 0) {
        fprintf(stderr, "\n%d 个词法错误，编译中止。\n", g_error_count);
        return false;
    }

    // ---- 3. 语法 ----
    strace_enter("parse");
    *out_mod = parse(*out_toks, *out_tok_count, arena, path);
    strace_leave();
    if (g_error_count > 0) {
        fprintf(stderr, "\n%d 个语法错误，编译中止。\n", g_error_count);
        return false;
    }

    strace_leave();
    return true;
}

static bool is_pure_decl(Stmt* s) {
    if (s->kind != ST_LET) return false;
    // is_const 或 let 都算（ST_LET 已排除 var）。
    // 要求有初始化表达式：`const X` 没初值在 P0 本身就是错的，
    // 但这里多一道防线，避免把空声明合进 main 造成奇怪行为。
    return s->var.init != NULL;
}

// 递归加载：把 path 及其 import 的所有文件的 fn 声明合并进 g->merged。
//
// 合并顺序是**后序**（先加载依赖，再加载自己）。这样即使不依赖
// 「函数可以前向引用」这条性质也能工作 —— 而 compile.c 确实做了
// 两遍扫描支持前向引用，所以顺序其实无关紧要。选后序是因为它
// 天然把错误报到「最深的依赖」上，定位更快。
//
// is_entry 区分入口文件：只有它能带顶层语句（顶层语句会合成为
// main）。不为入口单独写一条加载路径，是因为那条路要走一遍
// 「读文件 → 校验编码 → 词法 → 语法」，两份实现必然漂移。
static bool mod_load(ModGraph* g, const char* path, const char* from_file,
                     bool is_entry) {
    strace_enter(path);
    // 已加载过：直接返回。重复 import 同一个文件是允许的（比如两个
    // 模块都 import 了 utils），去重靠绝对路径。
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->files[i].path, path) == 0) {
            if (g->files[i].visiting) {
                // 环：A → B → A。文本合并下这会让 fn 重复定义，
                // 报错时把环上所有文件列出来，比一屏「重定义」有用。
                diag_error((SrcPos){ from_file, 0, 0 },
                           "循环 import：`%s` 已经被引入过，但还没加载完", path);
                diag_note("P0 的模块是文本级合并，没有先后依赖的解析能力。"
                          "环会让同一个函数被定义两次。");
                diag_help("把两个文件共同依赖的部分抽到第三个文件里，"
                          "两边都 import 它。");
                return false;
            }
            return true;
        }
    }

    if (g->count >= MOD_MAX_FILES) {
        diag_error((SrcPos){ from_file, 0, 0 },
                   "import 的文件超过上限 %d 个", MOD_MAX_FILES);
        diag_help("P0 的模块上限是有意设的：文本合并的开销随文件数线性增长，"
                  "超过这个数量说明该拆分编译单元了（P1 的能力）");
        return false;
    }

    char* src = NULL;
    size_t src_len = 0;
    Token* toks = NULL;
    size_t tok_count = 0;
    Module* m = NULL;

    if (!load_one_file(path, g->arena, &src, &src_len, &toks, &tok_count, &m)) {
        free(src);
        free(toks);
        return false;
    }

    int slot = g->count++;
    g->files[slot].path     = (char*)path;   // path 来自 arena，无需复制
    g->files[slot].mod      = m;
    g->files[slot].loaded   = false;
    g->files[slot].visiting = true;

    // 被 import 的文件允许哪些顶层语句？
    //
    // ── 只允许「不可变声明」 ────────────────────────────────
    //
    // `const X = 1` 和 `let Y = 2` 这类声明没有副作用，合进 main
    // 里按顺序初始化就行，不会和入口的代码抢执行权。
    //
    // 而 `println(...)`、`var z = f()`、`if ... {}` 这些是**可执行
    // 语句**，它们合进 main 之后会插在入口代码中间执行，顺序完全
    // 取决于 import 的书写位置 —— 那种「程序行为由文件排列决定」
    // 的语义没有任何人能推理清楚，所以直接禁止。
    //
    // `var` 也不允许：可变的顶层状态在两个文件之间是隐式共享的，
    // 那正是全局变量的所有问题，而语言故意没有全局变量。

    // 检查：被 import 的文件里，非纯声明的顶层语句一律报错。
    if (!is_entry) {
        for (int i = 0; i < m->top_count; i++) {
            if (!is_pure_decl(m->top_stmts[i])) {
                Stmt* bad = m->top_stmts[i];
                diag_error(bad->pos,
                           "被 import 的文件里不能有可执行的顶层语句");
                diag_note("顶层语句会被合进 main，位置取决于 import 写在哪，"
                          "程序的执行顺序会变得无法推理。");
                diag_note("`const` / `let` 声明是允许的 —— 它们只是初始化，"
                          "没有副作用。");
                diag_help("把这段逻辑包进一个 `fn 名字() -> Int { ... }` 里，"
                          "在入口文件里显式调用它。");
                free(src);
                free(toks);
                return false;
            }
        }
    }

    // 先递归加载依赖
    for (int i = 0; i < m->import_count; i++) {
        char dep[1200];

        // `@name` 是**包依赖**，走清单解析；其余走相对路径。
        //
        // 这两条路必须在这里分开，而不是在 parser 里：
        // parser 只看语法，它不知道 x.toml 在哪、也不知道有哪些依赖。
        // 文件系统的知识全在这一层（这也是为什么 parser 产出的
        // imports[] 里，包依赖被保留成 "@name" 原样 —— 前缀就是标记）。
        if (m->imports[i][0] == '@') {
            const char* name = m->imports[i] + 1;
            if (!g->manifest) {
                // 主位置指向**那句 import** —— 用户能改的是它。
                // 缺清单这个事实本身没有行号，所以在 note 里说明。
                int iline = (m->import_lines && i < m->import_count)
                                ? m->import_lines[i] : 1;
                diag_error((SrcPos){ path, iline, 1 },
                           "`import @%s` 需要 x.toml，但没找到", name);
                diag_note("从当前目录向上找不到清单文件。`@` 前缀表示"
                          "「声明在清单里的包依赖」，没有清单就无从解析。");
                diag_help("在项目根建一个 x.toml（`x pkg init`），"
                          "或在 [dependencies] 里声明 `%s`。", name);
                diag_help("如果只是想引入同目录的另一个文件，去掉 @："
                          "`import %s`。", name);
                free(src);
                free(toks);
                return false;
            }
            int iline = m->import_lines ? m->import_lines[i] : 1;
            if (!pkg_resolve_import(g->manifest, name, path, iline,
                                    dep, sizeof(dep))) {
                free(src);
                free(toks);
                return false;
            }
        } else if (strncmp(m->imports[i], "std.", 4) == 0 ||
                   strcmp(m->imports[i], "std") == 0) {
            // 标准库：走 stdlib 目录查找，与 cwd 无关。
            if (!resolve_std_module(m->imports[i], dep, sizeof(dep))) {
                int iline = (m->import_lines && i < m->import_count)
                                ? m->import_lines[i] : 1;
                diag_error((SrcPos){ path, iline, 1 },
                           "找不到标准库模块 `%s`", m->imports[i]);
                diag_note("标准库按**可执行文件所在目录**查找，与当前工作目录无关。");
                diag_help("确认 x.exe 旁边有 stdlib 目录（`x stdlib list` 可查看）。");
                diag_help("也可以用 X_STDLIB_PATH 环境变量指定标准库位置。");
                free(src);
                free(toks);
                return false;
            }
        } else {
            resolve_path(path, m->imports[i], dep, sizeof(dep));
        }

        // dep 是栈上的，但 mod_load 会把它存进 g->files[].path。
        // 所以复制一份到 arena 里 —— 这个坑很隐蔽：用栈地址的话
        // 去重比较会读到已经失效的栈内容，表现为「同一个文件被加载
        // 两次」或者更糟：随机匹配到别的路径。
        char* dep_persist = arena_strdup(g->arena, dep);

        if (!mod_load(g, dep_persist, path, false)) {
            free(src);
            free(toks);
            return false;
        }
    }

    g->files[slot].visiting = false;
    g->files[slot].loaded   = true;

    // 把本文件的 fn 追加到合并结果。
    // 顶层语句只有入口文件有，这里不处理。
    for (int i = 0; i < m->func_count; i++) {
        Module* mg = g->merged;
        int cap = mg->func_count;
        // 简单起见每次重新分配：函数数量在几十个量级，
        // 复制总开销可以忽略。换成倍增数组要维护额外状态，
        // 而这段代码不是热路径。
        FuncDecl** nf = arena_alloc(g->arena, (size_t)(cap + 1) * sizeof(FuncDecl*));
        if (mg->funcs) {
            memcpy(nf, mg->funcs, (size_t)cap * sizeof(FuncDecl*));
        }
        nf[cap] = m->funcs[i];
        mg->funcs = nf;
        mg->func_count = cap + 1;
    }

    // 结构体声明也要合并 —— 否则 `import std.ui` 之后 ui.xle 里
    // 定义的 Rect 在入口文件里就不可见了。合并顺序不影响正确性
    // （类型表在编译期先整体建好再编译函数体，支持前向引用），
    // 但保持「依赖先、入口后」能让诊断里的重名报错指向更合理的位置。
    for (int i = 0; i < m->struct_count; i++) {
        Module* mg = g->merged;
        int cap = mg->struct_count;
        StructDecl** nd = arena_alloc(g->arena, (size_t)(cap + 1) * sizeof(StructDecl*));
        if (mg->structs) {
            memcpy(nd, mg->structs, (size_t)cap * sizeof(StructDecl*));
        }
        nd[cap] = m->structs[i];
        mg->structs = nd;
        mg->struct_count = cap + 1;
    }
    // 类声明同样要合并 —— 与结构体完全相同的理由，而且更严重：
    // 类的方法（含构造函数）虽然会以 `Class.method` 的形式出现在
    // m->funcs 里被合并进去，但**类表**（字段布局、虚表）是从
    // m->classes 建的。只合并 funcs 而不合并 classes，结果就是：
    // 函数都在，类却不存在 —— 报「未知的类 A」，而用户明明写了。
    for (int i = 0; i < m->class_count; i++) {
        Module* mg = g->merged;
        int cap = mg->class_count;
        ClassDecl** nc = arena_alloc(g->arena, (size_t)(cap + 1) * sizeof(ClassDecl*));
        if (mg->classes) {
            memcpy(nc, mg->classes, (size_t)cap * sizeof(ClassDecl*));
        }
        nc[cap] = m->classes[i];
        mg->classes = nc;
        mg->class_count = cap + 1;
    }
    // import 列表不用合并：依赖已经被递归处理完了。

    // 关键：**先**补一次重计数。
    //
    // 为什么要单独一步而不是在 parse 阶段就算好：被 import 文件里的
    // 顶层 `const` / `let` 声明要合进 main，而 main 的执行顺序是
    // 「依赖文件的常量先初始化，然后才是入口自己的语句」。
    // 后序遍历天然给出这个顺序（依赖先处理），所以这里按入槽顺序
    // 依次拼接即可 —— 而不需要在这里再排一次序。

    free(src);
    free(toks);
    strace_leave();
    return true;
}

static bool compile_file(const char* path, Compilation* out) {
    memset(out, 0, sizeof(*out));
    arena_init(&out->arena);

    // ---- 0. 后缀检查 ----
    // 入口文件的后缀在这里先查一遍，让「文件名打错」这类错误
    // 在开始递归之前就报出来，而不是藏在某个 import 里。
    if (!is_source_file(path)) {
        fprintf(stderr,
                "error: `%s` 不是 X 的源文件\n"
                "  note: 源文件后缀必须是 .xle（英文）或 .xlc（中文）；"
                "字节码是 .xbc\n"
                "  help: 把文件改名为 .xle 或 .xlc 即可。"
                "从 v0.12 及以前升级的项目需要批量改名："
                ".xle → .xle（旧后缀与 Excel 加载项格式冲突），"
                ".x → .xle/.xlc（v0.7 及以前）\n",
                path);
        return false;
    }

    // ---- 1. 加载入口 + 递归加载所有 import ----
    ModGraph g;
    memset(&g, 0, sizeof(g));
    g.arena = &out->arena;

    // ---- 找包清单 ----
    //
    // 从入口文件所在目录向上找 x.toml。找不到不算错误 ——
    // 单文件程序（tests/p0/*.xle、games/ 里的例子）没有清单，
    // 它们照常编译，只是不能写 `import @name`。
    //
    // 为什么用「入口文件所在目录」而不是当前工作目录：
    // 项目根由清单定义，与你在哪个目录敲命令无关。
    // `cd src && x build main.xle` 和 `x build src/main.xle`
    // 必须得到同一个结果。
    bool have_manifest = false;
    {
        char entry_abs_probe[1200];
        plat_abs_path(path, entry_abs_probe, sizeof(entry_abs_probe));
        char dir[1200];
        snprintf(dir, sizeof(dir), "%s", entry_abs_probe);
        char* slash = strrchr(dir, '/');
        char* bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) *slash = '\0';
        else snprintf(dir, sizeof(dir), ".");

        char mpath[1300];
        if (pkg_find_manifest(dir, mpath, sizeof(mpath))) {
            if (pkg_load(mpath, &out->manifest)) {
                have_manifest = true;
                out->has_manifest = true;
                g.manifest = &out->manifest;
            } else {
                // 清单存在但坏了：这是硬错误，不能当「没有清单」放过。
                // 静默忽略会让用户面对一堆「函数未定义」，而不是
                // 「你的 x.toml 第 7 行写错了」这个真正的原因。
                return false;
            }
        }
    }
    (void)have_manifest;

    char* entry_abs_buf[1];
    (void)entry_abs_buf;
    char entry_abs[1200];
    plat_abs_path(path, entry_abs, sizeof(entry_abs));
    char* entry_persist = arena_strdup(&out->arena, entry_abs);

    // 合并结果的骨架。顶层语句稍后从入口文件搬过来。
    g.merged = arena_alloc(&out->arena, sizeof(Module));
    memset(g.merged, 0, sizeof(*g.merged));
    g.merged->file = entry_persist;

    // 入口文件自己也要走 mod_load，这样「读文件 / 校验 / 解析」
    // 只有一份实现，不会出现「入口允许顶层语句、依赖不允许」之外
    // 的任何行为差异。
    if (!mod_load(&g, entry_persist, entry_persist, true)) {
        return false;
    }

    // ── 顶层语句的最终顺序 ──────────────────────────────────
    //
    // 需要的是：依赖文件的 const/let 声明先执行（它们的值可能被
    // 入口语句用到），然后才是入口自己的顶层语句。
    //
    // 达成方式：按 g.files 的入槽顺序遍历。mod_load 是后序遍历，
    // 所以「被依赖的」总是先入槽 —— 直接顺着数组拼就得到正确顺序，
    // 不需要在这里做拓扑排序。
    //
    // 入口自己也在数组里，它的顶层语句自然也按顺序接在最后。
    {
        int cap = 0;
        Stmt** all = NULL;
        int total = 0;
        for (int i = 0; i < g.count; i++) {
            total += g.files[i].mod->top_count;
        }
        if (total > 0) {
            all = arena_alloc(&out->arena, (size_t)total * sizeof(Stmt*));
            for (int i = 0; i < g.count; i++) {
                Module* fm = g.files[i].mod;
                for (int j = 0; j < fm->top_count; j++) {
                    all[cap++] = fm->top_stmts[j];
                }
            }
        }
        g.merged->top_stmts = all;
        g.merged->top_count = cap;
    }

    out->module = g.merged;

    // ---- 2. 降级到字节码 ----
    bc_init(&out->bc);
    // 记下源文件名。代码生成要把它写进产物：运行时错误必须报出
    // 「哪个文件的第几行」，否则 AOT 产物的诊断会比解释档差一截，
    // 而两档的输出一致是我们承诺过的事。
    out->bc.source_file = x_strdup(path);
    strace_enter("compile_to_bytecode");
    compile_to_bytecode(out->module, &out->bc, &out->arena);
    strace_leave();
    if (g_error_count > 0) {
        fprintf(stderr, "\n%d 个编译错误，编译中止。\n", g_error_count);
        return false;
    }

    out->ok = true;
    return true;
}

static void compilation_free(Compilation* c) {
    free(c->src);
    free(c->toks);
    free(c->bc.code);

    // ── 顺序在这里是正确性的一部分，不是风格 ─────────────────
    //
    // 常量池里的字符串必须先逐个释放，**最后**才能 free 掉 consts
    // 数组本身。反过来写（先 free 数组、再遍历数组取每个元素的
    // kind）是一次 use-after-free：读的是已经还给堆管理器的内存。
    //
    // ── 为什么这个 bug 特别难抓 ──────────────────────────
    //
    // free 之后那块内存通常还在，所以「读取返回值」这一步往往
    // 能成功 —— 崩不崩取决于堆管理器有没有把那几个字节改写成
    // free-list 的元数据（前后块指针、size）。于是症状完全不由
    // 这段代码决定，而由**整个程序此前分配过什么**决定：
    //
    //   x run tests/p0/30-stdlib-fs.xle       → 段错误（退出码 139）
    //   x run <同一个文件的绝对路径>            → 正常退出
    //
    // 同一个文件、同一份字节码，只因为 source_file 字符串长度不同
    // （路径长一截），堆布局就变了，一个崩一个不崩。更误导人的是
    // 「加一行 println 就好了」—— 那只是又一次挪动了堆布局。
    //
    // 实测定位路径：崩溃落在本文件第 900 行（`consts[i].kind`），
    // 而 addr2line 报的是 compilation_free —— 从「退出流程崩了」
    // 一路反查到这里的顺序错误，比按输入格式（LF/CRLF）二分快
    // 得多。**凡是「换个输入长度就崩」的崩溃，先看释放顺序。**
    for (int i = 0; i < c->bc.const_count; i++) {
        if (c->bc.consts[i].kind == CONST_STR) free(c->bc.consts[i].str.s);
    }
    free(c->bc.consts);

    for (int i = 0; i < c->bc.func_count; i++) {
        free(c->bc.funcs[i].name);
    }
    free(c->bc.funcs);
    free(c->bc.source_file);
    if (c->has_manifest) pkg_free(&c->manifest);
    arena_free(&c->arena);
}

// ============================================================
// 子命令
// ============================================================

// run 的两种情况：
//   1. 源文件（.xle / .xlc）→ 编译后执行
//   2. .xbc 字节码          → 直接加载执行
//      这是 02 §2「同一份字节码」的验证入口：
//      `x build a.xle && x run a.xbc` 的输出必须与 `x run a.xle` 一致
// ============================================================
// ============================================================
// 档位的最终裁决：命令行 > 清单 > 默认
// ============================================================
//
// 三层优先级，理由各不同：
//
//   命令行  —— 本次构建的临时意图（「我就想试一下 desktop」），
//              必须能覆盖清单，否则改个档位要先改文件。
//   清单    —— 项目的**长期声明**（这是个桌面应用）。放在清单里的
//              意义是：别人 clone 下来直接 x build 就能得到正确的
//              产物形态，不需要知道该加什么参数。
//   默认    —— console。见 profile.c 里对「默认值要能跑」的说明。
//
// 为什么这个裁决放在编译之后：清单是 compile_file 才读的（它要
// 从入口文件所在目录向上找）。命令行解析阶段还不知道项目根在哪。
//
// cli_name 为 NULL 表示「用户没写 --profile」。
static const XProfileDef* resolve_profile(const char* cli_name,
                                          const Compilation* c) {
    if (cli_name) {
        // 已经在参数解析时校验过名字，这里查不到只可能是内部错误
        const XProfileDef* p = profile_find(cli_name);
        if (p) return p;
    }

    if (c->has_manifest && c->manifest.profile) {
        const XProfileDef* p = profile_find(c->manifest.profile);
        if (p) {
            // ── 这条提示必须走 stderr ────────────────────────────
            //
            // 一开始写的是 printf，结果 `x run a.xle > out.txt` 得到的是
            //
            //     （使用 x.toml 里声明的场景：desktop）
            //     hi
            //
            // 程序的输出被一行编译器提示污染了。两个后果：
            //   1. `x run a.xle | grep foo` 之类的管道全部带上噪音
            //   2. 「AOT 与解释两档 stdout 逐字节一致」这条红线测试
            //      会因为这个提示而失败 —— 而它本该只在有差异时失败
            //
            // 判据是：任何不是**程序自己输出**的东西，都不能进 stdout。
            // 这条提示描述的是编译器的决定，不是程序的运行结果。
            diag_note("使用 x.toml 里声明的场景：%s（命令行 --profile 可覆盖）",
                      c->manifest.profile);
            return p;
        }
        // 清单里的档位名非法：警告但不阻断 —— 这不是代码错误，
        // 退回默认档位仍能编出东西来。硬报错会让「清单里多打了一个
        // 字母」变成「整个项目编译不了」。
        diag_warn((SrcPos){ c->manifest.manifest_path, 1, 1 },
                  "x.toml 里的 profile `%s` 不是合法场景", c->manifest.profile);
        diag_help("可用的是 console / desktop / service，见 `x profile list`。");
        diag_note("已退回默认档位 console。命令行加 --profile 可显式指定。");
    }

    return profile_get(PROF_CONSOLE);
}

// --profile 的语义检查（build / run 共用）
// ============================================================
//
// 两个方向的错配都要提示，因为它们的后果完全不同：
//
//   A) 用窗口、但档位不内嵌窗口  → 链接期报 undefined reference，
//      或者运行时空实现什么都不画。**用户会立刻发现**。
//
//   B) 不内嵌窗口（desktop 档）、但程序只 println
//      → 双击运行**输出静默丢失，退出码还是 0**。这是最危险的一类：
//        没有任何错误，程序看起来「跑了」，只是什么都没显示出来。
//
// B 的机理（实测确认，不是推测）：WINDOWS 子系统的进程在无控制台
// 启动时，GetStdHandle(STD_OUTPUT_HANDLE) 返回 NULL，printf 不失败
// （返回值照常计数），但第一次 fflush 后 ferror(stdout) 就被置位。
// 于是所有输出进了一个没有出口的缓冲。
//
// 所以这两条检查的价值不对称：A 是提醒，B 是救命。
//
// 两者都**不报错**：bc 里出现窗口内建只说明代码里写了，不说明它会
// 执行 —— 一个测试文件完全可能 import 了 ui.xle（因而带上窗口内建）
// 而实际只跑 board_test 的逻辑，这种情况下阻止编译是错的。
static void check_profile_fit(Bytecode* bc, const XProfileDef* prof,
                              const char* src_path) {
    if (!bc || !prof) return;
    SrcPos pos = { src_path, 1, 1 };
    bool uses_gui = bc_uses_gui(bc);

    // ---- A) 用窗口，但档位不内嵌 ----
    if (!prof->want_gui && uses_gui) {
        diag_warn(pos, "程序用到了窗口能力，但场景 `%s` 不内嵌窗口运行时",
                  prof->name);

        // 警告之后必须说清两件事：怎么消除警告、以及不改会怎样。
        // 「不改会怎样」是最容易被省略的部分，而它恰恰是用户判断
        // 「这个警告要不要处理」的唯一依据。
        if (strcmp(prof->name, "service") == 0) {
            diag_note("服务进程通常没有可用的图形会话（无桌面、无显示器），"
                      "开窗会在运行时失败。");
        } else {
            diag_note("当前产物的子系统是 CONSOLE，双击运行会先弹一个控制台黑框。");
        }
        diag_help("如果这就是个图形程序，加 `--profile=desktop` 重编。");
        diag_help("如果窗口能力只是被 import 带进来、实际不调用，加 "
                  "`--profile=desktop` 也无副作用（不会主动开窗）。");
        return;
    }

    // ---- B) desktop 档，但程序根本不开窗 ----
    //
    // 判据是「有没有窗口内建 + 有没有打印」两条同时成立。
    //
    // 为什么要求「有打印」才警告：一个 desktop 档的空程序不会丢任何
    // 东西。真正会出事的是「以为自己写了命令行工具、却选了 desktop
    // 档」的人 —— 他一定写了 println。
    if (prof->want_gui && !uses_gui && bc_has_print(bc)) {
        diag_warn(pos, "场景 `%s` 不挂控制台，但程序用 println 输出",
                  prof->name);
        diag_note("WINDOWS 子系统的进程双击启动时没有控制台，stdout 句柄为 NULL。"
                  "println 不会报错，但输出会全部丢失 —— 退出码仍然是 0。");
        diag_note("这一条是实测确认的：GetStdHandle 返回 NULL，"
                  "printf 照常返回字节数，fflush 后 ferror 才被置位。");
        diag_help("如果这是个命令行工具，去掉 `--profile=%s`（默认就是 console）。",
                  prof->name);
        diag_help("如果它确实要开窗，那 println 里的内容应该改成画到窗口上，"
                  "或输出到日志文件。");
    }
}

// ============================================================
// 子命令
// ============================================================

// profile_name 是**命令行传进来的名字，可为 NULL**（表示没写 --profile）。
// 真正的档位在编译完之后由 resolve_profile 裁决（命令行 > 清单 > 默认），
// 因为清单是 compile_file 才读到的。传名字而不是传已解析的档位，
// 就是为了让这个优先级能成立。
static int cmd_run(const char* path, const char* profile_name) {
    if (ends_with(path, SUFFIX_XBC)) {
        Bytecode bc;
        if (!xbc_read(path, &bc)) return 1;
        // 字节码没有清单信息（.xbc 里不存项目配置），只能用命令行/默认。
        const XProfileDef* prof = profile_find(profile_name);
        if (!prof) prof = profile_get(PROF_CONSOLE);
        check_profile_fit(&bc, prof, path);
        int rc = run_bytecode(&bc, prof);
        return rc;
    }

    Compilation c;
    if (!compile_file(path, &c)) {
        compilation_free(&c);
        return 1;
    }
    const XProfileDef* prof = resolve_profile(profile_name, &c);
    check_profile_fit(&c.bc, prof, path);
    int rc = run_bytecode(&c.bc, prof);
    compilation_free(&c);
    return rc;
}

static int cmd_check(const char* path, const char* profile_name) {
    Compilation c;
    bool ok = compile_file(path, &c);
    if (ok) {
        // 档位检查也归「检查」的职责。用户写 `x check foo.xle
        // --profile=desktop` 就是想在做产物之前确认这个组合有没有问题 ——
        // 如果这里不查、只有 build/run 才查，那 check 的结论就是
        // 不完整的（说 ok，实际编出来双击不输出）。
        const XProfileDef* prof = resolve_profile(profile_name, &c);
        check_profile_fit(&c.bc, prof, path);
        printf("%s: ok — %d 条指令, %d 个常量, %d 个函数\n",
               path, c.bc.code_count, c.bc.const_count, c.bc.func_count);
    }
    compilation_free(&c);
    return ok ? 0 : 1;
}

static int cmd_disasm(const char* path) {
    if (ends_with(path, SUFFIX_XBC)) {
        Bytecode bc;
        if (!xbc_read(path, &bc)) return 1;
        bc_disasm(&bc, stdout);
        return 0;
    }

    Compilation c;
    if (!compile_file(path, &c)) {
        compilation_free(&c);
        return 1;
    }
    bc_disasm(&c.bc, stdout);
    compilation_free(&c);
    return 0;
}

static int cmd_build(const char* path, const char* out_path,
                     const char* emit_kind, const XTarget* tgt,
                     const char* cc_override, const char* profile_name) {
    Compilation c;
    if (!compile_file(path, &c)) {
        compilation_free(&c);
        return 1;
    }

    // 与 cmd_run / cmd_check 一致：编完才知道清单在哪，所以档位在此裁决。
    const XProfileDef* prof = resolve_profile(profile_name, &c);

    // 档位与程序是否匹配。放在这里（而不是只有 run 里）是关键：
    // 用户真正会踩坑的时刻是 `x build --emit=exe --profile=desktop`
    // 产出一个双击静默不输出的程序 —— 而 `x run` 在终端里总是有
    // 控制台，永远看不出这个问题。
    check_profile_fit(&c.bc, prof, path);

    // 默认输出名。三类产物三种命名，都从源文件主干名推出来。
    char default_out[1024];
    if (!out_path) {
        if (strcmp(emit_kind, "bc") == 0) {
            // 字节码：demo.xle → demo.xbc
            snprintf(default_out, sizeof(default_out), "%s", path);
            size_t len = strlen(default_out);
            if (ends_with(default_out, SUFFIX_XLE)) {
                len -= strlen(SUFFIX_XLE);
            } else if (ends_with(default_out, SUFFIX_XLC)) {
                len -= strlen(SUFFIX_XLC);
            } else {
                char* dot = strrchr(default_out, '.');
                if (dot) len = (size_t)(dot - default_out);
            }
            snprintf(default_out + len, sizeof(default_out) - len, "%s", SUFFIX_XBC);
        } else if (strcmp(emit_kind, "c") == 0) {
            // 中间 C 代码：demo.xle → demo.c
            target_output_name(path, target_host(), default_out, sizeof(default_out));
            // target_output_name 给的是可执行名，这里换后缀
            char* dot = strrchr(default_out, '.');
            if (dot) *dot = '\0';
            size_t l = strlen(default_out);
            snprintf(default_out + l, sizeof(default_out) - l, ".c");
        } else {
            // 原生可执行文件：后缀与平台标识由 target 决定
            target_output_name(path, tgt, default_out, sizeof(default_out));
        }
        out_path = default_out;
    }

    int rc = 0;
    if (strcmp(emit_kind, "bc") == 0) {
        bool ok = xbc_write(out_path, &c.bc, path);
        if (ok) {
            printf("已生成 %s  (%d 条指令, %d 个常量)\n",
                   out_path, c.bc.code_count, c.bc.const_count);
        }
        rc = ok ? 0 : 1;
    } else if (strcmp(emit_kind, "c") == 0) {
        rc = codegen_c_emit(&c.bc, out_path, prof);
    } else {
        rc = codegen_c_build(&c.bc, path, out_path, tgt, cc_override, prof);
    }

    compilation_free(&c);
    return rc;
}

// ============================================================
// x pkg —— 包管理
// ============================================================
//
// ── P0 的包管理做到哪一步 ──────────────────────────────────
//
// 完整形态（spec/03 §6）包含注册中心、下载、签名、MVS 解析、
// 内容寻址缓存。那是 P1~P2 的工作量。
//
// P0 做的是**端到端可用的最小闭环**：
//
//     x pkg init          生成清单
//     x pkg add name      声明依赖（path 依赖直接可用）
//     x pkg list          看有哪些依赖
//     x pkg tree          看依赖树
//     import @name        在代码里引用，编译时解析到具体文件
//
// 这五步串起来，用户就能真实地把代码拆成包、声明依赖、引用依赖。
// 「能看出来」这件事靠的是闭环，不是靠命令数量 —— 一个能跑通的
// 五个命令，胜过二十个半成品的命令。
//
// ── 刻意不做的 ────────────────────────────────────────────
//
//   x pkg add name（registry 版）  需要网络与注册中心，P0 直接拒绝
//   x pkg fetch / vendor           同上
//   x pkg publish                  需要签名与注册中心
//
// 这些命令**在帮助里显式标注「尚未实现」**，而不是藏起来或者假成功。
// 静默失败是最坏的选择：用户会以为下载好了，然后在编译时得到
// 一堆「函数未定义」。

// 生成一个默认清单。
//
// 为什么生成的内容这么短：清单的每一个字段都该有真实用途。
// 生成一堆空的注释模板（`# authors = []` 之类）只会让人一眼略过，
// 不如只给必填的三项 + 一条说明怎么加依赖。
static int pkg_cmd_init(const char* dir) {
    char target[1300];
    snprintf(target, sizeof(target), "%s/x.toml", dir);

    // 已存在就拒绝覆盖 —— 清单里可能有用户手工加的依赖，
    // 覆盖是不可逆的数据丢失。
    FILE* probe = x_fopen(target, "rb");
    if (probe) {
        fclose(probe);
        // 位置传 NULL：路径已经写在错误消息里了，再印一遍是纯重复：
        //     error: `C:\p\x.toml` 已经存在
        //     位置   C:\p\x.toml
        // 而且「位置」这个词在这里有误导性 —— 它暗示文件里有问题，
        // 实际是「这个文件的存在本身」挡住了操作。
        diag_error((SrcPos){ NULL, 0, 0 }, "`%s` 已经存在", target);
        diag_help("要重新初始化，先手工删掉它 —— "
                  "编译器不替你覆盖清单，里面可能有你的依赖声明。");
        return 1;
    }

    // 包名默认取目录名。目录名可能含空格/中文/大写，
    // 那些都不合法，所以做一次清洗并说明改了什么。
    char name[128];
    {
        const char* base = dir;
        for (const char* p = dir; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        size_t k = 0;
        for (const char* p = base; *p && k < sizeof(name) - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                name[k++] = (char)c;
            } else if (c == '-' || c == ' ') {
                name[k++] = '_';
            }
            // 其它字符（中文等）直接丢掉
        }
        name[k] = '\0';
        // 首位必须是字母
        if (k == 0 || !(name[0] >= 'a' && name[0] <= 'z')) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "app_%s", name);
            snprintf(name, sizeof(name), "%s", tmp);
        }
    }

    FILE* f = x_fopen(target, "wb");
    if (!f) {
        // 同上：路径已在消息里，不重复，也不用「位置」这个词。
        diag_error((SrcPos){ NULL, 0, 0 }, "无法写入 `%s`", target);
        diag_help("检查目录是否可写。");
        return 1;
    }

    // 用 \n 而不是 \r\n：清单是跨平台共享的，
    // CRLF 会让 diff 在 Linux/macOS 上显示成「整文件都改了」。
    fprintf(f,
        "[package]\n"
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "entry = \"src/main.xle\"\n"
        "\n"
        "[dependencies]\n"
        "# 本地依赖（P0 只支持这种）：\n"
        "# util = { path = \"../util\" }\n"
        "#\n"
        "# 注册中心依赖需要 P1 的网络支持：\n"
        "# http_client = \"0.9\"\n",
        name);
    fclose(f);

    printf("已生成 %s\n", target);
    printf("  包名    %s\n", name);
    printf("  入口    src/main.xle\n");
    printf("\n");
    printf("下一步：\n");
    printf("  1. 先确认清单里的包名与入口是否正确\n");
    printf("  2. 加依赖：x pkg add <名字> --path <目录>\n");
    printf("  3. 在代码里引用：import @<名字>\n");
    return 0;
}

// 从当前目录向上找清单；找不到就报错（init 之外的命令都需要它）
static bool pkg_require_manifest(char* out, size_t out_len) {
    char cwd[1024];
    if (!plat_getcwd(cwd, sizeof(cwd))) {
        snprintf(cwd, sizeof(cwd), ".");
    }
    if (pkg_find_manifest(cwd, out, out_len)) return true;

    diag_error((SrcPos){ ".", 1, 1 }, "找不到 x.toml");
    diag_note("从 `%s` 向上逐级找，都没有清单文件。", cwd);
    diag_note("包管理命令需要知道「这是哪个项目」，而项目的边界由 x.toml 定义。");
    diag_help("先在项目根目录运行 `x pkg init`。");
    return false;
}

// ============================================================
// x pkg install / update / lock / registry
// ============================================================
//
// ── 三个命令的分工 ──────────────────────────────────────────
//
//   install   按 x.toml 的约束解析一遍，把结果写进 x.lock。
//             已有 lock 且没有真正变化时，什么也不改（幂等）。
//
//   update    同上，但**强制重新选版本**（忽略现有 lock）。
//             这是「我要升级」的唯一表达方式。
//
//   lock      只显示 lock 与当前状态是否一致，什么都不改。
//             用于 CI：不一致就退出非零。
//
// ── 为什么 install 与 update 要分开 ────────────────────────
//
// 合成一个的话，「装了但没更新」和「非要更新」就没法区分。
// npm 的 install 在 lock 存在时按 lock 装（不更新），加了参数
// 才更新 —— 这个区分是必要的，否则「我明明没动清单，怎么依赖
// 变了」会反复出现。

// 把解析结果与 lock 比对并打印。返回不一致条数（0 = 一致）。
static int pkg_report_drift(const LockFile* lf, const RegSolution* sol) {
    Drift d[LOCK_MAX_ENTRIES];
    int n = lock_drift(lf, sol, d, LOCK_MAX_ENTRIES);
    if (n > 0) lock_print_drift(d, n, stderr);
    return n;
}

// 共用的解析 + 写 lock。
//
// force_update 为 true 时忽略现有 lock（update 命令）。
static int pkg_do_resolve(bool force_update, bool quiet) {
    char mpath[1300];
    if (!pkg_require_manifest(mpath, sizeof(mpath))) return 1;

    PkgManifest m;
    if (!pkg_load(mpath, &m)) return 1;

    // 注册表位置。即使没有任何 registry 依赖也先探一次 ——
    // 这样报「注册表不存在」时是有依据的，而不是猜。
    char root[1200];
    bool has_reg = reg_root(root, sizeof(root));

    bool has_registry_dep = false;
    for (int i = 0; i < m.dep_count; i++) {
        if (m.deps[i].kind == DEP_REGISTRY) { has_registry_dep = true; break; }
    }

    char lpath[1400];
    snprintf(lpath, sizeof(lpath), "%s/x.lock", m.root_dir);

    // 没有任何 registry 依赖：lock 应该是空的。
    //
    // 但仍然要写 —— 一个空 lock 和「没有 lock」在语义上不同：
    // 前者是「解析过了，确认不需要外部包」，后者是「从没解析过」。
    // CI 靠这个区分「项目还没初始化」和「项目确定不依赖外部包」。
    if (!has_registry_dep) {
        LockFile lf;
        memset(&lf, 0, sizeof(lf));
        lf.format = 1;

        // lock 已经是空的就不动它 —— 理由同上（mtime 也是状态）。
        LockFile cur;
        bool have_cur = lock_read(lpath, &cur) && cur.count == 0 &&
                        x_file_exists(lpath);
        if (!have_cur) {
            if (!lock_write(lpath, &lf)) { pkg_free(&m); return 1; }
        }

        if (!quiet) {
            printf("%s %s\n", m.name ? m.name : "", m.version ? m.version : "");
            if (have_cur) {
                printf("（x.toml 里没有 registry 依赖，x.lock 已是空的）\n");
            } else {
                printf("（x.toml 里没有 registry 依赖，x.lock 已清空）\n");
            }
            printf("\n清单: %s\n", mpath);
        }
        pkg_free(&m);
        return 0;
    }

    if (!has_reg) {
        diag_error((SrcPos){ mpath, 0, 0 },
                   "x.toml 里有 registry 依赖，但注册表目录不存在");
        diag_note("找的位置：%s", root);
        diag_note("注册表默认在 x.exe 旁边（<exe目录>/packages），"
                  "可用环境变量 X_REGISTRY 改。");
        diag_help("建了这个目录，把包按 `包名/版本/src/lib.xle` 放进去。");
        pkg_free(&m);
        return 1;
    }

    // 解析
    //
    // ── update 与 install 的唯一差别就在这里 ────────────────
    //
    //   install 把现有 lock 传进去 → 已锁定的版本继续沿用
    //   update  传 NULL            → 全部按约束重新挑，能升就升
    //
    // 两者走同一个解析器，只是「要不要尊重既有结论」这一个输入
    // 不同。分成两个实现的话，冲突检测、约束求交、诊断文案
    // 都会各写一份，然后慢慢走偏。
    LockFile old;
    lock_read(lpath, &old);

    RegSolution sol;
    if (!reg_solve(root, &m, force_update ? NULL : &old, &sol)) {
        pkg_free(&m);
        return 1;
    }

    // 与现有 lock 比对（update 模式下不比对，因为它本来就是要改）
    if (!force_update && old.count > 0) {
        int drift = pkg_report_drift(&old, &sol);
        if (drift == 0) {
            // 完全一致：不重写文件。
            //
            // 「内容一样就不碰」是有意义的 —— 重写会改 mtime，
            // 构建系统（以及人）会把 mtime 变化当成「变了」。
            // 幂等不只是输出的幂等，也包括不该动的东西不动。
            //
            // 注意这个提前返回**不能写在 `!quiet` 里面**：那样
            // quiet 模式下会掉到下面去重写文件，于是同一个命令
            // 加不加 quiet 会得到不同的副作用。输出可以不同，
            // 行为不能不同。
            if (!quiet) {
                printf("%s %s\n", m.name ? m.name : "",
                       m.version ? m.version : "");
                reg_print_solution(&sol, stdout);
                printf("\n已是最新，x.lock 无需改动。\n");
            }
            pkg_free(&m);
            return 0;
        }
        if (!quiet) {
            printf("x.lock 有 %d 处变化，正在更新…\n\n", drift);
        }
    }

    LockFile lf;
    lock_from_solution(&lf, &sol);
    if (!lock_write(lpath, &lf)) { pkg_free(&m); return 1; }

    if (!quiet) {
        printf("%s %s\n", m.name ? m.name : "", m.version ? m.version : "");
        reg_print_solution(&sol, stdout);
        printf("\n注册表: %s\n", root);
        printf("已写入: %s\n", lpath);
        printf("\n提示：x.lock 应该进版本库 —— 它是「可复现」的依据。\n");
    }

    pkg_free(&m);
    return 0;
}

// x pkg registry —— 看看注册表里有什么
//
// 这个命令不做下载，只做**观察**。它的价值在于：注册表是一个
// 纯目录约定，用户可以手工往里面放东西 —— 那就必须有一个
// 「我放对了没有」的观察手段，否则只能靠猜。
static int pkg_cmd_registry(void) {
    char root[1200];
    bool ok = reg_root(root, sizeof(root));

    printf("注册表: %s%s\n", root, ok ? "" : "  （不存在）");

    if (!ok) {
        const char* env = getenv("X_REGISTRY");
        if (env && *env) {
            printf("\n路径来自环境变量 X_REGISTRY。\n");
        } else {
            printf("\n默认位置是 x.exe 旁边的 packages 目录。\n");
            printf("要换位置就设环境变量 X_REGISTRY。\n");
        }
        printf("\n建立方式：\n");
        printf("  mkdir -p \"%s/包名/1.0.0/src\"\n", root);
        printf("  写 %s/包名/1.0.0/src/lib.xle\n", root);
        printf("  可选：写 %s/包名/1.0.0/x.toml（声明它自己依赖什么）\n", root);
        return 0;
    }

    PlatDir d;
    if (!plat_dir_open(&d, root, NULL)) {
        printf("\n（读不出内容）\n");
        return 1;
    }

    int npkg = 0;
    while (plat_dir_next(&d)) {
        if (!d.is_dir) continue;
        char nm[600];
        snprintf(nm, sizeof(nm), "%s", d.name);

        char vers[REG_MAX_VERSIONS][32];
        int nv = reg_list_versions(root, nm, vers, REG_MAX_VERSIONS);
        if (nv <= 0) continue;   // 不是包目录（可能是别的杂项）

        npkg++;
        printf("\n  %s\n", nm);
        for (int i = 0; i < nv; i++) {
            bool usable = reg_has_version(root, nm, vers[i]);
            printf("    %-10s %s\n", vers[i],
                   usable ? "" : "（缺 src/lib.xle，不可用）");
        }
    }
    plat_dir_close(&d);

    if (npkg == 0) {
        printf("\n（注册表是空的）\n");
        printf("放一个包进去：%s/名字/1.0.0/src/lib.xle\n", root);
    } else {
        printf("\n共 %d 个包。\n", npkg);
    }
    return 0;
}

static int cmd_pkg(int nargs, const char** args) {
    if (nargs < 2) {
        fprintf(stderr,
            "用法: x pkg <命令>\n"
            "\n"
            "  init              在当前目录生成 x.toml\n"
            "  list              列出清单里声明的依赖\n"
            "  tree              打印依赖树（显示 @引入名，可与源码对照）\n"
            "  add <名> --path <目录>   添加本地依赖\n"
            "  remove <名>       移除依赖\n"
            "  install           按 x.toml 解析版本，写 x.lock\n"
            "  update            强制重新选版本（忽略现有 x.lock）\n"
            "  lock              检查 x.lock 与当前依赖是否一致\n"
            "  registry          看注册表里有哪些包和版本\n"
            "\n"
            "依赖有两种：\n"
            "  util = { path = \"../util\" }    本地路径，版本由文件系统决定\n"
            "  jsonx = \"^1.2\"                  注册表，按版本约束选最高满足版本\n"
            "\n"
            "注册表现在是**本地目录**（默认 x.exe 旁边的 packages/，\n"
            "可用 X_REGISTRY 改）。格式是 packages/包名/版本/src/lib.xle，\n"
            "用户可以直接手工往里放，不需要任何命令参与。\n"
            "\n"
            "远程下载与发布（fetch / publish）属于 P1 —— 这里不提供，\n"
            "也不做「假装成功」。\n"
            "\n"
            "  import @名字 的解析规则见 spec/01 §11.1。\n");
        return 2;
    }

    const char* sub = args[1];

    if (strcmp(sub, "init") == 0) {
        char cwd[1024];
        if (!plat_getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        return pkg_cmd_init(cwd);
    }

    if (strcmp(sub, "list") == 0 || strcmp(sub, "tree") == 0) {
        char mpath[1300];
        if (!pkg_require_manifest(mpath, sizeof(mpath))) return 1;

        PkgManifest m;
        if (!pkg_load(mpath, &m)) return 1;

        if (strcmp(sub, "tree") == 0) {
            pkg_print_tree(&m, stdout);
        } else {
            // list：更扁平的形式，一行一个，适合脚本处理
            //
            // 这里只显示**清单里写的**约束，不去解析注册表。
            // 有意的分工：list 是「我声明了什么」，install 是
            // 「实际会用什么」。混在一起会让 list 因为注册表缺包
            // 而失败 —— 而用户想确认「我清单里到底写了什么」时，
            // 注册表的状态根本不该是障碍。
            printf("%s %s\n", m.name, m.version);
            if (m.dep_count == 0) {
                printf("（没有依赖）\n");
            } else {
                bool has_reg_dep = false;
                for (int i = 0; i < m.dep_count; i++) {
                    const PkgDep* d = &m.deps[i];
                    if (d->kind == DEP_PATH) {
                        printf("  @%-16s path: %s\n", d->key, d->path);
                    } else {
                        printf("  @%-16s %s\n", d->key, d->version ? d->version : "");
                        has_reg_dep = true;
                    }
                }
                if (has_reg_dep) {
                    printf("\n（上面是清单里的**约束**。实际选定的版本看\n"
                           "  `x pkg install` 的输出或 x.lock。）\n");
                }
            }
            printf("\n清单: %s\n", mpath);
        }
        pkg_free(&m);
        return 0;
    }

    if (strcmp(sub, "add") == 0) {
        if (nargs < 3) {
            fprintf(stderr, "error: `x pkg add` 需要一个包名\n");
            fprintf(stderr, "  help: x pkg add util --path ../util\n");
            return 2;
        }
        const char* name = args[2];
        const char* dep_path = NULL;
        const char* dep_ver = NULL;
        for (int i = 3; i < nargs; i++) {
            if (strcmp(args[i], "--path") == 0 && i + 1 < nargs) {
                dep_path = args[i + 1];
                i++;
            } else if (strcmp(args[i], "--version") == 0 && i + 1 < nargs) {
                dep_ver = args[i + 1];
                i++;
            }
        }

        // 包名必须是合法标识符 —— 它要出现在 `import @name` 里。
        // 在这里就拦住，比等到用户写完 import 才报错有用。
        //
        // ── 为什么位置是 {NULL} 而不是 {name, 1, 1} ──────────
        //
        // 一开始写成 {name, 1, 1}，于是报错显示：
        //
        //     位置   Bad-Name:1:1
        //
        // 这不是一个存在的文件，是用户刚敲的**命令行参数**。把参数
        // 当文件名印出来，用户会以为有个叫 Bad-Name 的文件出问题了，
        // 或者以为要去建一个。而真相是「这个参数本身不合法」。
        //
        // 命令行参数错误没有源位置。传 NULL 让它不打印位置行，
        // 比伪造一个文件路径诚实。
        //
        // 顺序与 pkg.c 里的一致：先判首字母，再逐字符判。
        // 反过来的话 "T-2" 会报「含有非法字符 T」，而 T 的问题
        // 在于位置不在字符本身。
        if (!(name[0] >= 'a' && name[0] <= 'z')) {
            diag_error((SrcPos){ NULL, 0, 0 },
                       "包名 `%s` 必须以小写字母开头", name);
            diag_note("包名规则是 `[a-z][a-z0-9_]*`。");
            if (name[0] >= 'A' && name[0] <= 'Z') {
                char fixed[128];
                size_t k = 0;
                for (const char* q = name; *q && k + 1 < sizeof(fixed); q++) {
                    char c = *q;
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
                    fixed[k++] = c;
                }
                fixed[k] = '\0';
                diag_help("改成 `%s`。", fixed);
            } else {
                diag_help("在开头加一个字母，例如 `p%s`。", name);
            }
            return 1;
        }
        for (const char* p = name; *p; p++) {
            bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
            if (ok) continue;
            if (*p >= 'A' && *p <= 'Z') {
                diag_error((SrcPos){ NULL, 0, 0 },
                           "包名 `%s` 不能含大写字母 `%c`", name, *p);
                diag_note("包名只能用 `[a-z0-9_]`。");
                diag_help("改成小写：`%c` → `%c`。", *p, (char)(*p - 'A' + 'a'));
            } else {
                diag_error((SrcPos){ NULL, 0, 0 }, "包名 `%s` 不能作为引入名", name);
                diag_note("`import @名字` 要求名字是合法标识符，"
                          "所以包名只能用 [a-z0-9_]。");
                diag_help("把 `%s` 里的非法字符换成下划线。", name);
            }
            return 1;
        }

        // ── 两种添加方式：路径依赖与版本依赖 ────────────────────
        //
        // 早先这里只接受 --path，因为那时没有注册表 —— 写一个
        // `name = "1.0"` 的依赖是**不可能解析成功**的，允许写进去
        // 等于让用户埋一个必定失败的配置。现在注册表有了，
        // `--version` 才真正有意义。

        if (!dep_path && !dep_ver) {
            diag_error((SrcPos){ NULL, 0, 0 },
                       "需要 `--path <目录>` 或 `--version <约束>`");
            diag_note("--path     本地路径依赖，版本由文件系统决定");
            diag_note("--version  注册表依赖，按版本约束选最高满足版本");
            diag_help("例如：x pkg add %s --version \"^1.2\"", name);
            diag_help("或者：x pkg add %s --path ../%s", name, name);
            return 1;
        }
        if (dep_path && dep_ver) {
            diag_error((SrcPos){ NULL, 0, 0 },
                       "`--path` 与 `--version` 不能同时给");
            diag_note("路径依赖的版本由文件系统决定，不需要也不能指定版本约束。");
            diag_help("二选一：要么 --path，要么 --version。");
            return 1;
        }

        // 版本约束在这里就校验格式 —— 写进清单之后再发现写错，
        // 要等到 install 才会报错，而那时清单已经改了。
        if (dep_ver) {
            SemReq probe;
            if (!sem_parse_req(dep_ver, &probe)) {
                diag_error((SrcPos){ NULL, 0, 0 },
                           "版本约束 `%s` 无法解析", dep_ver);
                diag_note("支持：`1.2.3`（等价 ^）、`^1.2.3`、`~1.2.3`、");
                diag_note("`=1.2.3`、`>=1.2.0`、`<2.0.0`、`*`。");
                diag_help("例如：x pkg add %s --version \"^1.2\"", name);
                return 1;
            }
        }

        char mpath[1300];
        if (!pkg_require_manifest(mpath, sizeof(mpath))) return 1;

        // ── 当场校验路径能用 ────────────────────────────────
        //
        // 以前这里什么都不查，`x pkg add ghost --path ./nope` 会
        // 打印「已添加依赖 ghost → ./nope」然后返回 0。错误的后果
        // 被完整地推迟到了之后某次编译，而那时用户早已忘了这件事。
        //
        // 一个当场能查出来的错误，没有理由留给未来。
        //
        // 位置在写文件之前：清单一旦被改动，再报错就得让用户自己
        // 回滚，而「加了条坏依赖」的清单看起来和正常的一模一样。
        if (dep_path) {
            char root_abs[1200];
            char mdir[1300];
            snprintf(mdir, sizeof(mdir), "%s", mpath);
            char* sl = strrchr(mdir, '/');
            char* bs = strrchr(mdir, '\\');
            if (bs && (!sl || bs > sl)) sl = bs;
            if (sl) *sl = '\0'; else snprintf(mdir, sizeof(mdir), ".");
            plat_abs_path(mdir, root_abs, sizeof(root_abs));

            const char* why = pkg_check_path_dep(root_abs, dep_path, name);
            if (why) {
                diag_error((SrcPos){ NULL, 0, 0 },
                           "路径 `%s` 不能作为依赖 `%s` 的源码位置", dep_path, name);
                // 「相对于 X」这句话只在相对路径时成立。绝对路径还
                // 这么说，用户会去那个目录下找一个同名子目录 ——
                // 一个从来不存在的东西，怎么找都找不着。
                bool is_abs = pkg_path_is_absolute(dep_path);
                if (strcmp(why, "DIR_MISSING") == 0) {
                    if (is_abs) {
                        diag_note("这个目录不存在。");
                    } else {
                        diag_note("这个目录不存在（相对于 %s）。", root_abs);
                    }
                    diag_help("检查路径是否写错，或先建好这个包目录。");
                } else {
                    diag_note("目录存在，但里面没有可用的源码入口。");
                    if (!is_abs) diag_note("查找基准目录是 %s。", root_abs);
                    diag_note("找过这些位置：");
                    char cand[4][1300];
                    int n = pkg_candidate_paths(root_abs, dep_path, name,
                                                true, cand, 4);
                    for (int i = 0; i < n; i++) diag_note("  %s", cand[i]);
                    diag_help("库包至少要有 src/lib.xle；"
                              "独立的可执行包要有 src/main.xle。");
                }
                diag_note("清单没有被改动。");
                return 1;
            }
        }

        // 追加到清单。这里做的是**文本追加**，不是「解析 → 修改 → 重写」。
        //
        // 为什么不用重写：重写会丢掉用户的注释与字段顺序，而清单是
        // 人维护的文件 —— 一个「加个依赖就删掉我所有注释」的工具是
        // 不可接受的。文本追加只加一行 `[dependencies]` 下的键值，
        // 对文件其余部分零影响。
        //
        // 代价是：如果清单里没有 [dependencies] 段，追加位置就是
        // 文件末尾，得自己补一个段头。这在实际使用里很好处理。
        size_t len = 0;
        char* src = read_file(mpath, &len);
        if (!src) {
            diag_error((SrcPos){ mpath, 0, 0 }, "无法读取 `%s`", mpath);
            return 1;
        }

        // 已经存在同名依赖？直接拒绝，不静默覆盖。
        {
            PkgManifest m;
            if (pkg_load(mpath, &m)) {
                if (pkg_find_dep(&m, name)) {
                    diag_error((SrcPos){ mpath, 0, 0 },
                               "依赖 `%s` 已经在清单里了", name);
                    diag_help("要改路径，先 `x pkg remove %s`。", name);
                    pkg_free(&m);
                    free(src);
                    return 1;
                }
                pkg_free(&m);
            }
        }

        bool has_dep_section = (strstr(src, "[dependencies]") != NULL);

        // 追加前必须确认文件以换行结尾。
        //
        // ── 这个检查不是洁癖，是防数据损坏 ──────────────────────
        //
        // 手工维护的清单经常最后一行没有换行（编辑器保存时不留）。
        // 直接追写会得到：
        //
        //     mathutil = { path = "../mathutil" }ghostpkg = { path = "..." }
        //
        // 结果是**两条依赖都坏掉**：原来的 mathutil 键后面多了垃圾，
        // 新加的 ghostpkg 被当成前一个值的延续。用户看到的是
        // 「加了新依赖之后原来的依赖也没了」，而清单表面上「看起来
        // 加了一条」，极难联想到是换行的问题。
        //
        // 实测踩到：tests/pkg 的那条用例就是这么失败的。
        bool need_nl = false;
        if (len > 0 && src[len - 1] != '\n') need_nl = true;

        FILE* f = x_fopen(mpath, "ab");
        if (!f) {
            diag_error((SrcPos){ mpath, 0, 0 }, "无法写入 `%s`", mpath);
            free(src);
            return 1;
        }
        if (need_nl) fputc('\n', f);
        if (!has_dep_section) {
            fputs("\n[dependencies]\n", f);
        }

        if (dep_path) {
            fprintf(f, "%s = { path = \"%s\" }\n", name, dep_path);
        } else {
            // 版本号写成字符串，不是数字。
            //
            // 数字 `1.2` 在清单解析里会被当成浮点而报错（清单只认
            // 字符串、整数、布尔）。加引号是唯一的正确写法，
            // 所以这里始终加。
            fprintf(f, "%s = \"%s\"\n", name, dep_ver);
        }
        fclose(f);
        free(src);

        if (dep_path) {
            // 说「已校验」是有意义的：用户刚被校验收下过（前面的
            // 检查可能失败过），这里明确告诉他这一条已经验证过接口
            // 存在，不必再自己确认一遍。
            printf("已添加依赖 %s → %s（源码入口已校验）\n", name, dep_path);
        } else {
            printf("已添加依赖 %s = \"%s\"\n", name, dep_ver);

            // 顺手看一眼注册表现在能不能满足它。
            //
            // 这不是必须的 —— 用户完全可以先写清单、之后再往注册表
            // 放包。但「加了个依赖，注册表里根本没有这个包」是最常见
            // 的下一步卡点，能当场说清就当场说清。
            char root[1200];
            if (reg_root(root, sizeof(root))) {
                SemReq req;
                sem_parse_req(dep_ver, &req);
                char picked[32];
                const char* why = reg_pick(root, name, &req, picked, sizeof(picked));
                if (!why) {
                    printf("注册表里现在能选中 %s（下一步：x pkg install）\n", picked);
                } else if (strcmp(why, "NO_PACKAGE") == 0) {
                    printf("note: 注册表 %s 里还没有 `%s` 这个包。\n", root, name);
                    printf("      放一个进去：%s/%s/1.0.0/src/lib.xle\n", root, name);
                } else {
                    printf("note: 注册表里 `%s` 没有满足 %s 的版本"
                           "（`x pkg registry` 可查看）。\n", name, dep_ver);
                }
            } else {
                printf("note: 注册表目录不存在（默认在 x.exe 旁边的 packages/）。\n");
            }
        }
        printf("在代码里这样引用：import @%s\n", name);
        return 0;
    }

    if (strcmp(sub, "remove") == 0) {
        if (nargs < 3) {
            fprintf(stderr, "error: `x pkg remove` 需要一个包名\n");
            return 2;
        }
        const char* name = args[2];
        char mpath[1300];
        if (!pkg_require_manifest(mpath, sizeof(mpath))) return 1;

        size_t len = 0;
        char* src = read_file(mpath, &len);
        if (!src) return 1;

        // 逐行重写，跳过目标依赖那几行。
        //
        // 除了删除，其它内容**逐字节保留** —— 注释、空行、字段顺序
        // 全部原样。用「解析成结构再序列化」的做法会丢掉这些，
        // 而清单是人维护的文件。
        FILE* f = x_fopen(mpath, "wb");
        if (!f) { free(src); return 1; }

        bool removed = false;
        char* line = src;
        while (*line) {
            char* eol = line;
            while (*eol && *eol != '\n') eol++;
            size_t linelen = (size_t)(eol - line);

            // 判断这一行是不是 `name = ...`
            char probe[1600];
            size_t n = linelen < sizeof(probe) - 1 ? linelen : sizeof(probe) - 1;
            memcpy(probe, line, n);
            probe[n] = '\0';
            char* t = probe;
            while (*t == ' ' || *t == '\t') t++;
            bool is_target = false;
            size_t nl = strlen(name);
            if (strncmp(t, name, nl) == 0) {
                char* after = t + nl;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == '=') is_target = true;
            }

            if (is_target) {
                removed = true;
                // 跳过这一行（含换行）
            } else {
                fwrite(line, 1, linelen, f);
                if (*eol == '\n') fputc('\n', f);
            }

            if (!*eol) break;
            line = eol + 1;
        }
        fclose(f);
        free(src);

        if (!removed) {
            // 没有行号可言 —— 要删的东西不存在，它不占任何一行。
            // 指向 x.toml:1:1 会让用户跳到 [package] 那一行，
            // 与错误毫无关系。
            diag_error((SrcPos){ mpath, 0, 0 },
                       "清单里没有依赖 `%s`", name);
            diag_help("运行 `x pkg list` 看看声明了哪些依赖。");
            return 1;
        }
        printf("已移除依赖 %s\n", name);
        printf("  note: 代码里残留的 `import @%s` 会让编译失败，记得一起删掉。\n", name);
        return 0;
    }

    // 明确列出「知道但没做」的命令，而不是笼统说「未知命令」。
    //
    // 用户看到「未知命令 fetch」会怀疑是不是拼错了；
    // 看到「fetch 尚未实现」才知道是能力还没做。
    if (strcmp(sub, "install") == 0) {
        return pkg_do_resolve(false, false);
    }
    if (strcmp(sub, "update") == 0) {
        return pkg_do_resolve(true, false);
    }
    if (strcmp(sub, "lock") == 0) {
        // 只检查、不改。CI 用：不一致就退出非零。
        char mpath[1300];
        if (!pkg_require_manifest(mpath, sizeof(mpath))) return 1;
        PkgManifest m;
        if (!pkg_load(mpath, &m)) return 1;

        char lpath[1400];
        snprintf(lpath, sizeof(lpath), "%s/x.lock", m.root_dir);

        char root[1200];
        reg_root(root, sizeof(root));

        LockFile lf;
        if (!lock_read(lpath, &lf)) { pkg_free(&m); return 1; }

        // 按 lock 优先解析：这样比出来的差异才是「x.toml 变了」，
        // 不是「上游发了新版」。后者不是漂移，是正常工作。
        RegSolution sol;
        bool solved = reg_solve(root, &m, &lf, &sol);
        pkg_free(&m);
        if (!solved) return 1;

        if (lf.count == 0 && sol.count == 0) {
            printf("x.lock 与当前依赖一致（都没有外部包）。\n");
            return 0;
        }
        if (lf.count == 0) {
            printf("还没有 x.lock（或它是空的），而 x.toml 需要 %d 个包。\n",
                   sol.count);
            printf("运行 `x pkg install` 生成它。\n");
            return 1;
        }

        int drift = pkg_report_drift(&lf, &sol);
        if (drift == 0) {
            printf("x.lock 与当前依赖一致。\n");
            reg_print_solution(&sol, stdout);
            return 0;
        }
        printf("\n运行 `x pkg install` 把它更新到一致。\n");
        printf("要强制重新选版本（升级），用 `x pkg update`。\n");
        return 1;
    }
    if (strcmp(sub, "registry") == 0) {
        return pkg_cmd_registry();
    }

    static const char* planned[] = {
        "fetch", "vendor", "publish", "search", "info",
        "bundle", "audit", "outdated", "clean", "cache", NULL
    };
    for (int i = 0; planned[i]; i++) {
        if (strcmp(sub, planned[i]) == 0) {
            diag_error((SrcPos){ "x pkg", 1, 1 },
                       "`x pkg %s` 尚未实现", sub);
            diag_note("它需要注册中心、网络、签名验证或内容寻址缓存，"
                      "这些都在 spec/03 §6，但属于 P1~P2。");
            diag_note("P0 做好的是最小闭环：init / add / remove / list / "
                      "tree + import @name 解析。");
            diag_help("本地依赖用 `x pkg add 名字 --path 目录`，"
                      "这条路不需要网络，现在就能用。");
            return 2;
        }
    }

    fprintf(stderr, "error: 未知的包管理命令 `%s`\n", sub);
    fprintf(stderr, "  help: 运行 `x pkg` 看支持哪些命令\n");
    return 2;
}

// ============================================================
// 入口
// ============================================================

int main(int argc, char** argv) {
    // 第零件事：装上崩溃处理器（只有 debug 构建有实质内容）。
    // 必须最先调用 —— 越早装，能覆盖的代码越多。
    crash_handler_install();

    // 第一件事：把终端切到 UTF-8。必须早于任何输出——包括下面的 usage。
    // 详见 utf8.c 顶部注释：程序输出的一直是合法 UTF-8，乱码是控制台
    // 按 GBK 解码造成的，这一步就是告诉控制台「我说的是 UTF-8」。
    utf8_init_console();

    // 第二件事：把 stdout 切成行缓冲。
    //
    // ── 为什么这不算「小事」──────────────────────────────────
    //
    // C 运行库的默认规则是：stdout 连到终端时行缓冲，连到管道/文件时
    // **全缓冲**。也就是说，只要用户写成
    //
    //     x run foo.xle > out.txt
    //     x run foo.xle | findstr xxx
    //
    // println 的输出就一直攒在 4 KB 缓冲里。程序正常退出会 flush，
    // 所以平时看不出来；但**一旦崩溃或超时被杀，缓冲区里的东西全部
    // 丢失**，用户看到的是「一行输出都没有，直接段错误」。
    //
    // 这个后果比听起来严重得多：它把「程序跑到第几行崩的」这条最有用
    // 的线索抹掉了。本项目真的踩过 —— 定位扫雷 demo 的段错误时，
    // 十几个 println 打点全部没打印，第一反应是「难道连第一句都没执行」，
    // 白白多花了两轮二分排查。
    //
    // 代价是每行一次 write 系统调用。脚本语言的输出量（几百行）下
    // 这个开销完全可以忽略；而且现在用户没法通过重定向拿回全缓冲
    // 的性能了 —— 但换个角度，「崩溃时能看到崩溃前的输出」比省几次
    // 系统调用重要得多。真要极致性能，那是 P1 的 buffered writer。
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    // 全局选项先扫一遍
    bool zh_mode = false;
    const char* out_path = NULL;
    const char* emit_kind = "bc";       // "bc"（字节码）或 "exe"（原生）
    const char* target_name = "host";   // 目标平台，默认宿主
    const char* cc_override = NULL;     // --cc 指定的 C 编译器
    // 初值必须是 NULL，不能是 profile_default_name()。
    //
    // ── 这一行踩过坑 ──────────────────────────────────────
    //
    // `const char* profile_name = profile_default_name();` 看起来无害 ——
    // 「默认档位」嘛。但它让 resolve_profile 的第一层（`if (cli_name)`）
    // **永远命中**，于是 x.toml 里的 `profile = "desktop"` 一次都不会
    // 被读到。用户改了清单、什么反应都没有，也看不到任何提示。
    //
    // NULL 的语义是「用户没写 --profile」，这是唯一能让三层优先级
    // （命令行 > 清单 > 默认）真正成立的办法。默认值由 resolve_profile
    // 在最后一层兜底。
    const char* profile_name = NULL;    // 使用场景（NULL = 没写 --profile）
    const char* args[16];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        // argv 在 Windows 上是 ANSI 代码页编码的（中文系统即 GBK）。
        // 直接拿去打印会与 UTF-8 的其它字符串混成「半 GBK 半 UTF-8」；
        // 转成 UTF-8 后再统一处理。转换失败则退回原指针（至少能用）。
        char* a = utf8_from_locale(argv[i]);
        const char* arg = a ? a : argv[i];

        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            // ── 只在「还没遇到位置参数」时才当成顶层选项 ──────
            //
            // 这里踩过一个真实的坑：`x pkg add jsonx --version "^1.2"`
            // 里的 --version 是 **pkg add 自己的选项**，指「版本约束」。
            // 而全局扫描在子命令参数之前就先把它拦下了，于是：
            //
            //     $ x pkg add jsonx --version "^1.2"
            //     x 0.9.0-p0                    ← 打印版本号就退出了
            //
            // 用户看到的是「命令静默地什么也没做，还莫名打印了版本号」。
            // 清单一个字都没改，也没有任何报错 —— 最坏的一种失败。
            //
            // 判据用 nargs：它是「已经收集到的非选项参数」个数。
            // 为 0 说明还没碰到 `pkg` / `run` / `build` 这类子命令，
            // 此时的 --version 确实是顶层的。一旦有了子命令，选项
            // 归属就归子命令自己解释。
            if (nargs == 0) {
                printf("x %s\n", X_VERSION);
                return 0;
            }
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            if (nargs == 0) {
                usage(stdout);
                return 0;
            }
        }
        if (strcmp(arg, "--zh") == 0) {
            zh_mode = true;
            continue;
        }
        if (strcmp(arg, "--no-color") == 0) {
            // 直接调平台层关掉，不改环境变量。
            //
            // 改环境变量的做法（_putenv("WT_SESSION=") / unsetenv("TERM")）
            // 有三个问题：会污染子进程（`x build` 拉起 gcc 时 gcc 也受影响）、
            // 在部分平台上不可逆、以及它关掉的是「看起来像终端」这个判据
            // 而不是「要不要上色」这件事本身——两件事不该混。
            plat_disable_color();
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: `-o` 需要一个参数\n");
                return 2;
            }
            i++;
            char* o = utf8_from_locale(argv[i]);
            out_path = o ? o : argv[i];
            continue;
        }

        // --emit=bc|exe —— 产物类型
        //
        // 用 --emit 而不是让 -o 后缀自动推断：后缀推断在
        // `-o build/out` 这种没有后缀的写法上失效，而用户没法从
        // 错误信息里看出到底哪一步猜错了。显式声明，猜错就报错。
        if (strncmp(arg, "--emit=", 7) == 0) {
            emit_kind = arg + 7;
            if (strcmp(emit_kind, "bc") != 0 && strcmp(emit_kind, "exe") != 0 &&
                strcmp(emit_kind, "c")  != 0) {
                fprintf(stderr,
                        "error: `--emit=%s` 不认识\n"
                        "  note: 支持三种产物\n"
                        "        --emit=bc    字节码 .xbc（默认，可 x run 直接执行）\n"
                        "        --emit=c     生成的 C 代码（调试代码生成器用）\n"
                        "        --emit=exe   原生可执行文件（PE/ELF/Mach-O，由 --target 决定）\n",
                        emit_kind);
                return 2;
            }
            continue;
        }

        // --target=<三元组> —— 目标平台
        if (strncmp(arg, "--target=", 9) == 0) {
            target_name = arg + 9;
            continue;
        }

        // --cc=<path> —— 指定 C 编译器（--emit=exe 时使用）
        if (strncmp(arg, "--cc=", 5) == 0) {
            cc_override = arg + 5;
            continue;
        }

        // --profile=<name> —— 使用场景
        //
        // 这里就校验名字，而不是等到 build 里再查。好处是拼错能立刻
        // 报错，而且报错时能列出全部合法值 —— 用户打 `--profile=destkop`
        // 时最想知道的就是「正确的写法是什么」。
        //
        // 同样不做模糊匹配：猜错档位的代价是编出一个能跑但不对的产物
        // （比如桌面程序带着黑框），比直接失败难发现得多。
        if (strncmp(arg, "--profile=", 10) == 0) {
            profile_name = arg + 10;
            if (!profile_name_ok(profile_name)) {
                fprintf(stderr, "error: 未知使用场景 `%s`\n", profile_name);
                fprintf(stderr,
                        "  note: 场景名必须精确匹配，不做模糊匹配\n"
                        "  help: 运行 `x profile list` 查看全部可用场景\n"
                        "        常用的是 console（命令行）与 desktop（图形界面）\n");
                return 2;
            }
            continue;
        }
        if (nargs < 16) args[nargs++] = arg;
    }

    if (nargs == 0) {
        usage(stderr);
        return 2;
    }

    // --zh 只是「提前声明」，真正生效的是 compile_file 里的后缀判定：
    //   .xlc → 强制开（后缀即声明，不可被关掉）
    //   .xle → 跟随 --zh（显式开关优先于默认值）
    // 也就是说 --zh 对 .xle 有效、对 .xlc 冗余、对字节码无效（字节码里已归一化）。
    lexer_set_zh_mode(zh_mode);

    const char* cmd = args[0];

    if (strcmp(cmd, "run") == 0) {
        if (nargs < 2) {
            fprintf(stderr, "error: `x run` 需要一个源文件（.xle / .xlc）或字节码（.xbc）\n");
            return 2;
        }
        return cmd_run(args[1], profile_name);
    }
    if (strcmp(cmd, "check") == 0) {
        if (nargs < 2) {
            fprintf(stderr, "error: `x check` 需要一个源文件（.xle / .xlc）\n");
            return 2;
        }
        return cmd_check(args[1], profile_name);
    }
    if (strcmp(cmd, "disasm") == 0) {
        if (nargs < 2) {
            fprintf(stderr, "error: `x disasm` 需要一个源文件（.xle / .xlc）或字节码（.xbc）\n");
            return 2;
        }
        return cmd_disasm(args[1]);
    }
    if (strcmp(cmd, "build") == 0) {
        if (nargs < 2) {
            fprintf(stderr, "error: `x build` 需要一个源文件（.xle / .xlc）\n");
            return 2;
        }
        // --target 的解析放在这里而不是参数扫描时：那时还不知道
        // 是哪条子命令，`x target list` 里的 "target" 会被误当平台名。
        const XTarget* tgt = target_find(target_name);
        if (!tgt) {
            fprintf(stderr,
                    "error: 未知目标平台 `%s`\n",
                    target_name);
            fprintf(stderr,
                    "  note: 目标名按「架构-系统-ABI」三段写，也可用简写\n"
                    "        windows / linux / macos / host\n"
                    "  help: 运行 `x target list` 查看全部可用目标\n"
                    "        拼错一个字母就会报错，不做模糊匹配 —— 猜错会产出\n"
                    "        在目标机器上跑不起来的文件，比直接报错更糟\n");
            return 2;
        }
        return cmd_build(args[1], out_path, emit_kind, tgt, cc_override,
                         profile_name);
    }

    if (strcmp(cmd, "profile") == 0) {
        // x profile list —— 列出使用场景。
        //
        // 与 `x target list` 并列而分开，因为它们是两个正交的轴：
        // target 回答「给哪个平台编」，profile 回答「给哪种场景编」。
        // 合成一张表会让「Windows 上的桌面程序」这种组合无从表达。
        if (nargs < 2 || strcmp(args[1], "list") == 0) {
            profile_print_table(stdout);
            return 0;
        }
        fprintf(stderr, "error: `x profile` 只支持 `list`\n");
        return 2;
    }

    if (strcmp(cmd, "pkg") == 0) {
        return cmd_pkg(nargs, args);
    }

    // x lsp —— 在 stdio 上讲 LSP，给编辑器用
    //
    // 为什么不叫 `x lsp serve` 之类：这个命令除了当服务端没有别的
    // 用途，多一层子命令只是让人多敲几个字。编辑器配置里那行
    // `{"command": "x", "args": ["lsp"]}` 越短越好。
    if (strcmp(cmd, "lsp") == 0) {
        return lsp_run();
    }

    if (strcmp(cmd, "stdlib") == 0) {
        char root[1200];
        if (!find_stdlib_root(root, sizeof(root))) {
            fprintf(stderr, "error: 找不到标准库目录\n");
            fprintf(stderr, "help: 标准库应与 x.exe 放在同一目录下的 stdlib/ 里\n");
            fprintf(stderr, "help: 或用 X_STDLIB_PATH 环境变量指定\n");
            return 2;
        }
        // 列目录。子命令目前只有 list；`x stdlib path` 打印根目录，
        // 排查「到底找的是哪个 stdlib」时很有用 —— 多个 x.exe 拷贝
        // 在不同目录时，这个问题一定会出现。
        if (nargs >= 2 && strcmp(args[1], "path") == 0) {
            printf("%s\n", root);
            return 0;
        }
        return stdlib_list(root);
    }

    if (strcmp(cmd, "target") == 0) {
        // x target list —— 列出目标与产物格式。
        //
        // 之所以把「格式」印在表里：这是用户最常问的那个问题
        // （「应该编译成什么格式」），而答案总是「看你的目标系统」。
        // 放在一张表里比写在文档里更容易被看到。
        if (nargs < 2 || strcmp(args[1], "list") == 0) {
            target_print_table(stdout);
            return 0;
        }
        fprintf(stderr, "error: `x target` 只支持 `list`\n");
        return 2;
    }

    fprintf(stderr, "error: 未知子命令 `%s`\n\n", cmd);
    usage(stderr);
    return 2;
}