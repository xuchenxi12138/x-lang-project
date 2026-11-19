// registry.h — 本地包注册表与依赖解析
//
// ── 本地注册表长什么样 ──────────────────────────────────────
//
//   <registry>/                    默认是 <exe目录>/packages
//     jsonx/
//       0.1.0/
//         x.toml                   该版本的清单
//         src/lib.xle              该版本的源码
//       0.2.0/
//         ...
//     httpclient/
//       1.0.0/
//
// 也就是说：**一个包里每个版本各占一个目录**。这个形状不是随便
// 定的，它带来两个直接好处：
//
//   1. 同一个机器上多个项目可以各用各的版本，互不干扰。扁平结构
//      （只有 <registry>/<name>/）做不到 —— 那是 node_modules 的
//      困境，升级一个项目会连带升级另一个。
//
//   2. 「选定版本」这一步变成纯粹的目录存在性检查，不需要解析
//      任何元数据文件来判断某版本是否存在。少一个解析步骤，就
//      少一类「元数据写着有、实际没有」的扯皮。
//
// ── 为什么叫「本地」注册表 ──────────────────────────────────
//
// 完整的包注册中心是：一台服务器、一个上传协议、一套签名、
// 一个索引。那是另一个项目。这里做的是**同一套语义的下半截**：
//
//   声明版本范围 → 从注册表里选出满足条件的最高版本 → 写进 lock
//
// 上层（远程下载）以后接进来时，只需要把「列出某包有哪些版本」
// 换成一次 HTTP 请求，其余逻辑（选版本、算交集、写 lock、检测漂移）
// 一行不用改。这就是先把本地做完整、而不是先做半个网络的价值。
//
// 用户也可以往这个目录里手工放包 —— 目录结构就是全部约定，
// 不需要任何命令行工具参与。这是有意的：注册表格式是人能读懂、
// 能手改的，出错时能直接看、直接改。

#ifndef X_REGISTRY_H
#define X_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "semver.h"

// 直接包含而不是前置声明 `struct PkgManifest`：
// pkg.h 里那个类型是**匿名结构体的 typedef**，它不是 `struct
// PkgManifest` 这个标签。前置声明会造出一个同名但永不完整的
// 新类型，于是 reg_solve 的形参类型和调用方传来的 PkgManifest*
// 不兼容 —— 编译器会警告、行为未定义。
#include "pkg.h"

// LockFile 只在 reg_solve 的参数里以指针形式出现，所以这里用
// 前置声明而不是 #include "lockfile.h"。
//
// 为什么不能直接包含：lockfile.h 需要 RegSolution（lock_from_solution
// 与 lock_drift 的形参），所以它 #include "registry.h"。如果这边
// 反过来也 include lockfile.h，就成了循环包含 —— 头文件保护会
// 让先被展开的那个看到对方未完成的类型，于是 RegSolution 变成
// 未知标识符，报一堆「expected const int *」这种完全指向错误
// 方向的编译错误。
//
// 依赖方向定死：lockfile 依赖 registry 的数据结构，registry
// 只通过指针提及 LockFile。
struct LockFile;

// 一个包名的版本上限。超过这个数的包基本可以断定是注册表被
// 生成的垃圾目录污染了，早报错比慢慢扫有用。
#define REG_MAX_VERSIONS 256

// ============================================================
// 注册表定位
// ============================================================

// 找到注册表根目录。顺序：
//   1. 环境变量 X_REGISTRY（允许用户指到共享目录）
//   2. <exe目录>/packages
//
// 关于 2：与标准库同一条思路 —— 「x.exe 旁边放什么，就能用什么」。
// 拷到哪都能工作的程序，不该因为注册表在别处就失效。
//
// 找不到（目录不存在）时返回 false，out 仍是可用的路径
// （后面要报「注册表是空的」而不是「注册表路径未知」）。
bool reg_root(char* out, size_t out_len);

// 列出某包在注册表里有哪些版本，按从新到旧排好序。
//
// 返回版本数量（0 表示没有这个包）。版本字符串写进 vers[i]
// （每项至少 32 字节）。
int reg_list_versions(const char* root, const char* name,
                      char (*vers)[32], int max);

// 某包某版本的源码入口是否可用。
//
// 判据与 pkg_candidate_paths 的 vendor 分支一致：src/lib.xle /
// src/<name>.xle / <name>.xle。用同一套，否则会出现「解析说
// 有 0.2.0、编译说找不到 0.2.0」。
bool reg_has_version(const char* root, const char* name, const char* ver);

// 某包某版本的目录（绝对路径）。
void reg_version_dir(const char* root, const char* name, const char* ver,
                     char* out, size_t out_len);

// 某包某版本里实际的入口源文件路径。
//
// 三处候选与 reg_has_version 完全一致，这里直接给出找到的那一个。
// 找不到返回 false。
bool reg_entry_file(const char* root, const char* name, const char* ver,
                    char* out, size_t out_len);

// 编译期解析一个 registry 依赖：给出它的入口源文件绝对路径。
//
// 顺序：
//   1. x.lock 里有这个包 → 用它记的版本（**这是可复现的保证**）
//   2. 没有 → 按 req_raw 从注册表选出最高满足版本
//   3. 注册表里找不到 → false（调用方再退回 vendor/ 兼容旧布局）
//
// manifest_root 是根项目目录（x.toml 所在处），用它找 x.lock。
//
// ── 为什么 lock 优先于重新挑版本 ────────────────────────────
//
// 如果每次都按约束重挑，那么上游一发新版、同事的构建就会换版本，
// 于是「我这能跑、他那不行」重新出现。lock 的存在就是为了让
// 「已经确定下来的事不要再变」。
//
// 只有显式 `x pkg install` 才会重算 lock —— 那是用户表达
// 「我要更新依赖」的唯一方式。
bool reg_resolve_entry(const char* manifest_root, const char* name,
                       const char* req_raw, char* out, size_t out_len,
                       bool* used_lock);

// ============================================================
// 选版本
// ============================================================

// 从注册表里选出满足 req 的最高版本。
//
// 成功返回 true 并写入选中的版本字符串；失败返回 false 并
// 在 why 里写原因（指向静态字符串）：
//   "NO_PACKAGE"   注册表里没有这个包
//   "NO_MATCH"     有这个包，但没有版本满足约束
//   "NO_ROOT"      注册表目录本身不存在
const char* reg_pick(const char* root, const char* name, const SemReq* req,
                     char* out, size_t out_len);

// ============================================================
// 依赖解析
// ============================================================

// 解析结果里的一项：某个包最终选定的版本。
typedef struct {
    char    key[64];        // 源码里的 import @key（根依赖才有意义）
    char    name[64];       // 注册表里的包名
    char    version[32];    // 选定版本
    char    req_raw[64];    // 根清单里写的原始约束（诊断用）
    int     depth;          // 传递依赖的深度（0 = 直接依赖）
    // 该依赖是由谁引入的。空串表示根清单。
    // 「A 要 1.x，B 要 2.x，冲突了」这种错误里，用户需要知道
    // 这两个要求分别从哪儿来，否则只能一个个包翻清单。
    char    from[64];
} RegNode;

typedef struct {
    RegNode node[128];
    int     count;
} RegSolution;

// 解出整棵依赖树的版本。
//
// ── prefer：锁定优先 ───────────────────────────────────────
//
// prefer 不为 NULL 时，注册表里已有、且这条锁定版本**仍然满足
// 所有约束**的包，就沿用锁定版本，不重新挑最高版本。
//
// 这是 install 与 update 的分界线：
//
//   install 传现有 x.lock  → 已有的不动，只解新加的
//   update  传 NULL        → 全部重新挑，能升就升
//
// ── 为什么 install 必须这么做 ──────────────────────────────
//
// 如果 install 每次都挑最高版本，那么「上游今天发了个补丁」
// 会让所有人的下一次 install 悄悄换版本。lock 记录了版本却
// 每次都被覆盖，等于没有 lock。可复现的前提是「已经定下来的
// 事不要自己变」。
//
// 但也要允许一个例外：约束改了（`^1.2` → `^2.0`），原来锁的
// 1.2.3 已经不满足，那就必须重挑。所以判据是「无条件沿用」
// 不成立，而是「仍然满足才沿用」。
//
// ── 冲突时怎么办：报错，不猜 ────────────────────────────
//
// 同一个包被两条路径要求两个不兼容的版本时，这里**直接失败**
// 并同时列出两个要求和它们的来源。
//
// 另一个选择是「选最高的，让旧的将就」—— npm 早期就是这么干的，
// 代价是运行时才炸。X 没有动态链接、没有运行时模块解析，选错
// 版本就是一个编译期就能发现的错误，那就该在编译期发现。
//
// 成功返回 true。失败时打印诊断并返回 false。
//
// 只有 registry 依赖参与解析；path 依赖按原样跳过（它们的版本由
// 文件系统说了算，没有「选」这回事）。
bool reg_solve(const char* root, const PkgManifest* m,
               const struct LockFile* prefer, RegSolution* out);

// 打印解出来的版本清单（x pkg install 的输出）。
void reg_print_solution(const RegSolution* s, FILE* out);

#endif // X_REGISTRY_H