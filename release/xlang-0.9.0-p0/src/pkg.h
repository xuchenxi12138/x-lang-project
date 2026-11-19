// pkg.h — 包清单（x.toml）与包查找路径
//
// ── 这个文件解决「包管理看不出来」的问题 ────────────────────
//
// 在它之前，X 的模块系统只有 `import "board.xle"` —— 文本级合并。
// 那是最小可用的东西，但它不是**包管理**：没有清单、没有版本、
// 没有「这个项目依赖哪些外部代码」这个问题的答案。
//
// 包管理要看得出存在，得有三样东西同时成立：
//
//   1. 一个声明依赖的地方（x.toml）
//   2. 一个引用依赖的语法（import @name）
//   3. 一个能把名字解析到具体文件的查找器（本文件）
//
// 少了任何一个，用户就只看到「又一个 import 写法」，看不出这套机制。
//
// ── 为什么是 TOML 的子集，不是完整 TOML ────────────────────
//
// 完整 TOML 要处理：多行字符串、内联表、数组表、日期时间、
// 点号键、引号变体、转义规则。那是一个几百行的解析器，
// 而清单文件实际只用到「表 + 键值 + 字符串」三样。
//
// 所以这里只实现清单需要的子集，并且**遇到不认识的语法显式报错**，
// 不静默跳过 —— 静默跳过的后果是用户写了 `dependencies = { a = "1" }`
// （内联表）却得到「没有依赖」，比报错难查得多。
//
// 子集覆盖：
//
//   [section]                 表
//   [section.sub]             嵌套表（只用点号拼名字，不做嵌套结构）
//   key = "string"            字符串
//   key = 123                 整数
//   key = true / false        布尔
//   # 注释                    行内与整行
//
// 不支持的（遇到就报错并指出行号）：
//
//   [table] 里的 [[array-of-tables]]、内联表 { }、多行 """ """、
//   数组 [ ]、日期。

#ifndef X_PKG_H
#define X_PKG_H

#include <stdbool.h>
#include <stdio.h>

// ============================================================
// 清单
// ============================================================

#define PKG_MAX_DEPS 64

typedef enum {
    DEP_REGISTRY = 0,   // 版本号（"1.2"），从注册中心取
    DEP_PATH,           // 本地路径（{ path = "../util" }）
} DepKind;

typedef struct {
    char*   key;        // 清单键名 = 源码里的 import @key
    char*   package;    // 真实包名（键名不同时才有意义），可为 NULL
    char*   version;    // 版本要求（registry 依赖必须有）
    char*   path;       // 本地路径（path 依赖必须有）
    DepKind kind;
    // 声明在清单的第几行。诊断用 —— 「依赖 ghost 找不到源码」这条
    // 错误如果只说文件名不说行号，用户得自己在一屏清单里找那一行。
    int     line;
} PkgDep;

typedef struct {
    // [package]
    char*   name;
    char*   version;
    char*   entry;          // 入口文件，默认 src/main.xle
    char*   description;
    char*   profile;        // 默认场景（可被 --profile 覆盖）

    // [dependencies]
    PkgDep  deps[PKG_MAX_DEPS];
    int     dep_count;

    // 清单文件自身的路径（绝对），用于算相对路径与诊断位置
    char*   manifest_path;
    char*   root_dir;       // 清单所在目录（绝对）
} PkgManifest;

// 从目录向上找 x.toml。找到返回 true 并填 out；
// 找不到返回 false（不是错误 —— 单文件程序不需要清单）。
bool pkg_find_manifest(const char* start_dir, char* out, size_t out_len);

// 读并解析清单。失败时打印诊断并返回 false。
bool pkg_load(const char* manifest_path, PkgManifest* out);

void pkg_free(PkgManifest* m);

// 按名字找依赖。查不到返回 NULL。
const PkgDep* pkg_find_dep(const PkgManifest* m, const char* key);

// ============================================================
// 包解析：把 import @name 落到具体文件
// ============================================================

// 解析 `@name` 对应的源文件路径。
//
// 规则（按顺序尝试，第一个存在者胜出）：
//
//   1. 若 name 在 [dependencies] 里且是 path 依赖
//        → <root>/<dep.path>/src/lib.xle
//        → <root>/<dep.path>/src/<name>.xle
//        → <root>/<dep.path>/<name>.xle
//
//   2. 若在 deps 里但是 registry 依赖
//        → <root>/vendor/<name>/src/lib.xle（vendor 过的源码）
//        找不到则报错并提示 `x pkg vendor`（P0 没有网络抓取）
//
//   3. 若不在 deps 里
//        → 报错：「import @name 未在 x.toml 声明」
//
// from_pos 是**源码里那句 import 的位置**。诊断要以它为主位置，
// 因为用户改的是源码那一行；清单的行号放在 note 里。
// 这一点很重要：错误指向 x.toml 会让人以为要改清单，而实际
// 常见情况是「包里确实没有这个依赖，应该删掉这句 import」。
//
// cause_file 是发起 import 的源文件（用于 from_pos）。
//
// 返回 true 表示解析成功，out 里是绝对路径。
bool pkg_resolve_import(const PkgManifest* m, const char* name,
                        const char* cause_file, int cause_line,
                        char* out, size_t out_len);

// 打印依赖树（x pkg tree）
void pkg_print_tree(const PkgManifest* m, FILE* out);

// ============================================================
// 候选路径与路径校验
// ============================================================

// 枚举一个依赖的源码可能所在的路径，填入 cand（每项 1300 字节），
// 返回写入的条数。is_path_dep 区分 path 依赖与 vendor 依赖。
//
// 这个函数存在的唯一理由是**让两个调用方用同一套判据**：
//   · pkg_resolve_import —— 编译时解析
//   · pkg_check_path_dep —— `x pkg add` 时立即校验
// 两处各写一份的话，迟早会出现「add 说行、编译说找不到」。
int pkg_candidate_paths(const char* root_dir, const char* dep_path,
                        const char* name, bool is_path_dep,
                        char (*cand)[1300], int max);

// 路径是否绝对（盘符 / 根 / UNC）。
//
// 存在的理由：path 依赖默认相对清单目录，拼接时要加前缀。绝对路径
// 再拼前缀会得到一个不存在的路径，然后报「目录不存在」—— 用户去
// 查那个明明存在的目录，怎么查都是对的。
bool pkg_path_is_absolute(const char* p);

// 校验 path 依赖指向的目录**当场**可用（`x pkg add` 用）。
//
// 返回 NULL 表示可用；否则返回一个静态字符串作为原因：
//   "DIR_MISSING"  目录不存在（路径写错，或忘了建）
//   "NO_ENTRY"     目录在，但没有 src/lib.xle / src/<name>.xle / main.xle
//   "PARAM"        参数不完整（内部错误）
//
// ── 为什么要做这个校验 ──────────────────────────────────────
//
// 以前 `x pkg add ghost --path ./nope` 会无条件成功，把错误推迟到
// 之后某次编译。那时用户早已忘了刚加过依赖，看到「依赖 ghost 的
// 源码找不到」只会困惑：明明加成功了。
//
// 能用一次检查消除的延迟错误，就不该让它延迟 —— 延迟只增加
// 定位成本，不增加任何信息。
const char* pkg_check_path_dep(const char* root_dir, const char* dep_path,
                               const char* name);

#endif // X_PKG_H