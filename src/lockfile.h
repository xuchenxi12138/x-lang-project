// lockfile.h — x.lock 的读写与漂移检测
//
// ── lock 文件解决什么问题 ───────────────────────────────────
//
// x.toml 里写的是**范围**（`jsonx = "^1.2"`）。范围的作用是
// 表达「我能接受哪些版本」，它的答案随时间变化 —— 上游今天
// 发了 1.9.0，明天同一个 x.toml 就会解析出不同的版本。
//
// 这对日常开发是好事（自动拿补丁），对「可复现」是灾难：三个月
// 后同事 clone 下来编不过，谁也说不清为什么。
//
// x.lock 记的是**那次解析的结果**：每个包到底选了哪个版本。
// 之后每次构建都照 lock 走，不再重新挑。于是：
//
//   x.toml  → 表达意图（可读、会变）
//   x.lock  → 记录事实（机器写给机器，不手改）
//
// ── 格式 ────────────────────────────────────────────────────
//
// 故意用最简单的行式文本，不用 TOML：
//
//   # 由 x pkg install 生成，不要手工编辑
//   lockfile 1
//
//   package name = jsonx
//   package version = 1.2.3
//   package dir = jsonx/1.2.3
//
// 为什么不用 TOML：lock 是**生成物**，人不该读得很舒服、更不该
// 手改。用和 x.toml 一样的语法反而危险 —— 用户会以为可以照那种
// 格式手写，然后和生成器打架。一个一眼就不像手写文件的格式，
// 能少掉一整类「我改了 lock 怎么没生效」的问题。
//
// 但也保留可读性：git diff 时要能一眼看出「哪个包升级了」。
// 所以用 `key = value` 的平铺行，每个包三个键。

#ifndef X_LOCKFILE_H
#define X_LOCKFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "registry.h"

#define LOCK_MAX_ENTRIES 128

typedef struct {
    char name[64];
    char version[32];
    // 相对于注册表根的目录（"jsonx/1.2.3"）。
    //
    // 存相对而不是绝对：lock 要进版本库，绝对路径一进 git 就
    // 变成「在我机器上能编」。注册表位置是环境相关的东西，
    // 不该固化进可复现记录。
    char dir[160];
} LockEntry;

// 带 struct 标签而不是匿名 typedef。
//
// 理由：registry.h 需要在不包含本文件的前提下提及这个类型
// （否则两个头互相包含）。C 里能做前置声明的只有 `struct X`，
// 匿名 struct 的 typedef 无法前置声明 —— 写 `struct LockFile;`
// 会造出一个永不完整的**另一个**类型，于是所有传参都变成
// incompatible pointer type。
//
// 「加个标签」这件事在这里不是风格问题，是让两个头文件能
// 单向依赖的必要条件。
struct LockFile {
    int      format;                 // 记为 1
    LockEntry ent[LOCK_MAX_ENTRIES];
    int      count;
};
typedef struct LockFile LockFile;

// 读 x.lock。文件不存在时返回 true 且 out->count == 0 ——
// 「没有 lock」不是错误，是「还没 install 过」。
// 存在但格式坏了返回 false 并打印诊断。
bool lock_read(const char* path, LockFile* out);

// 写 x.lock（原子写：先写 .tmp 再 rename）。
bool lock_write(const char* path, const LockFile* lf);

// 从解析结果生成 lock 内容。
void lock_from_solution(LockFile* lf, const RegSolution* s);

// 把 lock 里的条目按注册表拼成完整目录（绝对路径）。
void lock_entry_dir(const char* root, const LockEntry* e, char* out,
                    size_t out_len);

// ============================================================
// 漂移检测
// ============================================================

typedef enum {
    DRIFT_NONE = 0,
    // x.toml 新加了依赖，lock 里还没有（下次构建选不出它）
    DRIFT_MISSING,
    // lock 里记着，但 x.toml 已经不要它了（lock 偏旧）
    DRIFT_EXTRA,
    // 同名包版本不一致：x.toml 的约束改了，或者上游发了新版而
    // 有人重算过约束
    DRIFT_VERSION,
    // lock 里的版本在注册表里找不到了（被删、换机器、注册表被清理）
    DRIFT_GONE,
    // 其它清单层面的不一致
    DRIFT_MANIFEST,
} DriftKind;

typedef struct {
    DriftKind kind;
    char      name[64];
    char      locked[32];    // lock 里的版本（可能为空）
    char      wanted[32];    // 按 x.toml 现在会选的版本（可能为空）
} Drift;

// 比较 lock 与当前解析结果，列出所有不一致。
//
// 返回不一致的条数；out 里按顺序写入（最多 max）。
//
// ── 为什么不用「lock 过期了，自动重新生成」 ────────────────
//
// 自动重生成会让 lock 失去全部意义：它存在的理由就是「不要自己
// 变」。一旦检测到不一致就悄悄改写，等于每次都重新挑版本，
// 只是多绕了一圈。必须显式 `x pkg install`。
int lock_drift(const LockFile* lf, const RegSolution* sol,
               Drift* out, int max);

// 打印漂移报告。
void lock_print_drift(const Drift* d, int n, FILE* out);

#endif // X_LOCKFILE_H