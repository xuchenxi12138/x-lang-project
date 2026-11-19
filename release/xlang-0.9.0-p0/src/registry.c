// registry.c — 本地包注册表与依赖解析

#include "registry.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"
#include "platform.h"
#include "lockfile.h"
#include "x.h"

// ============================================================
// 注册表定位
// ============================================================

bool reg_root(char* out, size_t out_len) {
    if (!out || out_len == 0) return false;

    const char* env = getenv("X_REGISTRY");
    if (env && *env) {
        snprintf(out, out_len, "%s", env);
        return x_is_dir(out);
    }

    char ed[1200];
    if (!plat_exe_dir(ed, sizeof(ed))) {
        // 连 exe 目录都拿不到时退回 cwd，至少有个确定的地方。
        // 只有在这种极端情况下才会走到这里。
        if (!plat_getcwd(ed, sizeof(ed))) snprintf(ed, sizeof(ed), ".");
    }
    // 去掉末尾分隔符，避免拼出 `//packages`
    size_t L = strlen(ed);
    while (L > 1 && (ed[L - 1] == '/' || ed[L - 1] == '\\')) ed[--L] = '\0';

    snprintf(out, out_len, "%s/packages", ed);
    return x_is_dir(out);
}

// ============================================================
// 列出某包的版本
// ============================================================

// 目录名是不是一个版本号。
//
// 不额外限制段数是故意的：注册表目录名是我们自己约定的完整形式，
// 但用户手工建目录时写成 "1.2" 很自然，直接拒绝会让人困惑。
// 能被 sem_parse 解析就算版本 —— "1.2" 与 "1.2.0" 解析后相等，
// 去重时留一个。
static bool looks_like_version(const char* s) {
    if (!s || !*s) return false;
    SemVer v;
    return sem_parse(s, &v);
}

typedef struct {
    char   ver[32];
    SemVer sv;
} VerEnt;

// 从新到旧排序。插入排序：版本数量通常个位数，不值得为 qsort
// 引一层比较上下文。
static void sort_desc(VerEnt* a, int n) {
    for (int i = 1; i < n; i++) {
        VerEnt cur = a[i];
        int j = i - 1;
        while (j >= 0 && sem_cmp(&a[j].sv, &cur.sv) < 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = cur;
    }
}

static int list_versions(const char* root, const char* name,
                         VerEnt* ent, int max) {
    char dir[1300];
    snprintf(dir, sizeof(dir), "%s/%s", root, name);

    PlatDir d;
    if (!plat_dir_open(&d, dir, NULL)) return 0;

    int n = 0;
    while (plat_dir_next(&d)) {
        if (!d.is_dir) continue;
        if (!looks_like_version(d.name)) continue;
        if (n >= max) break;
        if (!sem_parse(d.name, &ent[n].sv)) continue;
        snprintf(ent[n].ver, sizeof(ent[n].ver), "%s", d.name);
        n++;
    }
    plat_dir_close(&d);

    sort_desc(ent, n);

    // 去重："1.2" 与 "1.2.0" 语义相同，留一个。
    int w = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < w; k++) {
            if (sem_cmp(&ent[i].sv, &ent[k].sv) == 0) { dup = true; break; }
        }
        if (!dup) ent[w++] = ent[i];
    }
    return w;
}

int reg_list_versions(const char* root, const char* name,
                      char (*vers)[32], int max) {
    if (!root || !name || !vers || max <= 0) return 0;

    VerEnt ent[REG_MAX_VERSIONS];
    int n = list_versions(root, name, ent, REG_MAX_VERSIONS);
    for (int i = 0; i < n && i < max; i++) {
        // 截断到 31 字节并显式补 NUL。
        //
        // 用 memcpy 而不是 snprintf("%s")：编译器对后者要按「源可能
        // 有多长」做静态推断，而 vers 被衰减成指针后它就不知道目标
        // 只有 32 字节，于是报 -Wformat-truncation。长度上限由
        // VerEnt.ver 的类型（char[32]）保证，这里只是搬运。
        size_t L = strlen(ent[i].ver);
        if (L > 31) L = 31;
        memcpy(vers[i], ent[i].ver, L);
        vers[i][L] = '\0';
    }
    return n > max ? max : n;
}

bool reg_has_version(const char* root, const char* name, const char* ver) {
    if (!root || !name || !ver) return false;

    char p[1300];

    snprintf(p, sizeof(p), "%s/%s/%s/src/lib.xle", root, name, ver);
    if (x_file_exists(p)) return true;

    snprintf(p, sizeof(p), "%s/%s/%s/src/%s.xle", root, name, ver, name);
    if (x_file_exists(p)) return true;

    snprintf(p, sizeof(p), "%s/%s/%s/%s.xle", root, name, ver, name);
    if (x_file_exists(p)) return true;

    return false;
}

void reg_version_dir(const char* root, const char* name, const char* ver,
                     char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s/%s/%s", root ? root : "", name ? name : "",
             ver ? ver : "");
}

bool reg_entry_file(const char* root, const char* name, const char* ver,
                    char* out, size_t out_len) {
    if (!out || out_len == 0) return false;

    char p[1300];

    // 三处候选，顺序与 reg_has_version 完全一致。
    // 两处判据必须同源 —— 否则会出现「has_version 说有、entry 找不到」。
    snprintf(p, sizeof(p), "%s/%s/%s/src/lib.xle", root, name, ver);
    if (x_file_exists(p)) { snprintf(out, out_len, "%s", p); return true; }

    snprintf(p, sizeof(p), "%s/%s/%s/src/%s.xle", root, name, ver, name);
    if (x_file_exists(p)) { snprintf(out, out_len, "%s", p); return true; }

    snprintf(p, sizeof(p), "%s/%s/%s/%s.xle", root, name, ver, name);
    if (x_file_exists(p)) { snprintf(out, out_len, "%s", p); return true; }

    return false;
}

// ============================================================
// 选版本
// ============================================================

// 在已排好序的候选里找第一个同时满足**所有**约束的版本。
//
// 为什么是「所有」而不是「第一个」：同一个包可能被两条路径要求
// 不同范围（A 要 ^1.0，B 要 >=1.2）。只满足其中一个会选出一个
// 让另一条路径根本用不了的版本 —— 而错误会在编译到那一处时才
// 出现，且信息里不会提到是版本选错了。
//
// 返回 NULL 表示成功，否则返回原因。
static const char* pick_version(const char* root, const char* name,
                                const SemReq* reqs, int nreq,
                                char* out, size_t out_len) {
    if (!x_is_dir(root)) return "NO_ROOT";

    char dir[1300];
    snprintf(dir, sizeof(dir), "%s/%s", root, name);
    if (!x_is_dir(dir)) return "NO_PACKAGE";

    VerEnt ent[REG_MAX_VERSIONS];
    int n = list_versions(root, name, ent, REG_MAX_VERSIONS);
    if (n <= 0) return "NO_PACKAGE";

    // 列表从新到旧。第一个满足全部约束的就是最高可用版本。
    //
    // 顺序很重要：从旧到新找会得到「最旧的可用版本」，也就是永远
    // 不升级。这类 bug 在测试里很容易漏 —— 只要有一个版本能选上，
    // 程序就跑得通，只是永远用旧版。
    for (int i = 0; i < n; i++) {
        bool ok = true;
        for (int r = 0; r < nreq; r++) {
            if (!sem_satisfies(&ent[i].sv, &reqs[r])) { ok = false; break; }
        }
        if (!ok) continue;

        // 还要确认源码真在。目录在但源码缺（手工建了空目录、
        // 下载中断）时跳过它继续找 —— 比选上一个空壳、让编译在
        // 一个完全无关的地方报「找不到函数」好得多。
        if (!reg_has_version(root, name, ent[i].ver)) continue;

        snprintf(out, out_len, "%s", ent[i].ver);
        return NULL;
    }
    return "NO_MATCH";
}

const char* reg_pick(const char* root, const char* name, const SemReq* req,
                     char* out, size_t out_len) {
    if (!root || !name || !req || !out) return "NO_ROOT";
    return pick_version(root, name, req, 1, out, out_len);
}

// ============================================================
// 依赖解析
// ============================================================
//
// ── 数据结构 ────────────────────────────────────────────────
//
// 解析要同时维护两样东西：
//
//   · 每个包**已声明的约束**（可能好几条，来自不同上游）
//   · 每个包**已选定的版本**（每条约束都成立才定得下来）
//
// 两者不能合并成一个「版本」字段。合并就是「先来先定、后来忽略」——
// 那正是 npm 早期踩过的坑：同一个包要 1.x 和 2.x，它装 1.x 然后
// 静默让 2.x 的调用方在运行时炸。这里把冲突保留成两条约束，
// 定版时一次比对全部，定不下来就报错并列出**每一条约束的来源**。

#define MAX_CONS 256

typedef struct {
    char    name[64];
    SemReq  req;
    char    req_raw[64];
    // 这条约束是谁提的。空串 = 根清单。
    char    from[64];
    int     depth;          // 提出者的深度 + 1
} Con;

typedef struct {
    Con  c[MAX_CONS];
    int  n;
} ConSet;

static bool con_add(ConSet* cs, const char* name, const char* raw,
                    const char* from, int depth) {
    if (cs->n >= MAX_CONS) return false;
    // 同一条约束重复出现（菱形依赖：A→C、B→C 都写 ^1.0）。
    // 完全相同的约束加两遍没有意义，还会让冲突诊断里出现重复项。
    for (int i = 0; i < cs->n; i++) {
        if (strcmp(cs->c[i].name, name) == 0 &&
            strcmp(cs->c[i].req_raw, raw) == 0 &&
            strcmp(cs->c[i].from, from) == 0) {
            return true;
        }
    }
    Con* c = &cs->c[cs->n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->req_raw, sizeof(c->req_raw), "%s", raw ? raw : "*");
    snprintf(c->from, sizeof(c->from), "%s", from ? from : "");
    c->depth = depth;
    return sem_parse_req(raw, &c->req);
}

// 读某个已定版本自己的清单，把它的 registry 依赖加进约束集。
static bool absorb_deps(const char* root, const char* name, const char* ver,
                        int depth, ConSet* cs) {
    char vdir[1300];
    reg_version_dir(root, name, ver, vdir, sizeof(vdir));

    char mpath[1400];
    snprintf(mpath, sizeof(mpath), "%s/x.toml", vdir);
    if (!x_file_exists(mpath)) {
        // 版本目录里没有清单：当成无依赖。
        //
        // 这是有意的宽容 —— 手工往注册表扔一个 src/lib.xle 就该能
        // 用，不该强行要求同时写清单。传递依赖是额外信息，不是
        // 使用包的先决条件。
        return true;
    }

    PkgManifest m;
    if (!pkg_load(mpath, &m)) return false;

    bool ok = true;
    for (int i = 0; i < m.dep_count; i++) {
        const PkgDep* d = &m.deps[i];
        if (d->kind != DEP_REGISTRY) continue;   // path 依赖不参与选版本

        // 用真实包名，不是键名：键名只是**根项目源码里**的别名，
        // 传递依赖应该用上游自己声明的名字。
        const char* realname = d->package ? d->package : d->key;
        if (!con_add(cs, realname, d->version ? d->version : "*", name,
                     depth + 1)) {
            ok = false;
            break;
        }
    }

    pkg_free(&m);
    return ok;
}

bool reg_solve(const char* root, const PkgManifest* m,
               const struct LockFile* prefer, RegSolution* out) {
    if (!root || !m || !out) return false;
    memset(out, 0, sizeof(*out));

    ConSet cs;
    memset(&cs, 0, sizeof(cs));

    // ── 第一步：根清单的直接依赖变成约束 ─────────────────
    for (int i = 0; i < m->dep_count; i++) {
        const PkgDep* d = &m->deps[i];
        if (d->kind != DEP_REGISTRY) continue;
        const char* realname = d->package ? d->package : d->key;
        if (!con_add(&cs, realname, d->version ? d->version : "*", "", 0)) {
            diag_error((SrcPos){ m->manifest_path, d->line, 1 },
                       "依赖数量超过解析上限 %d", MAX_CONS);
            return false;
        }
    }

    // ── 第二步：逐个包定版，每定一个就把它的依赖并入约束集 ──
    //
    // 用下标 i 单向前进，不做递归：新出现的包追加在 cs 尾部，
    // 循环自然覆盖到。包依赖本来就可以有几十层，递归只会白吃栈。
    //
    // 注意 cs.n 在循环里会增长 —— 条件是动态的，这正是要的。
    for (int i = 0; i < cs.n; i++) {
        // 同一个包可能有多条约束，它们在 cs 里是分散的。对每个包
        // 只处理一次（第一次遇到时把该包的所有约束一起算上）。
        bool done = false;
        for (int k = 0; k < i; k++) {
            if (strcmp(cs.c[k].name, cs.c[i].name) == 0) { done = true; break; }
        }
        if (done) continue;

        const char* name = cs.c[i].name;

        // 收集这个包的全部约束
        SemReq reqs[MAX_CONS];
        int nreq = 0;
        for (int k = 0; k < cs.n && nreq < MAX_CONS; k++) {
            if (strcmp(cs.c[k].name, name) == 0) reqs[nreq++] = cs.c[k].req;
        }

        char picked[32];
        picked[0] = '\0';
        const char* why = NULL;

        // ── 先看 lock：已经锁定的版本仍然满足全部约束吗？ ────
        //
        // 满足就沿用它，一个字都不改。
        //
        // 这一步是 install 与 update 的分界线（见 registry.h 的
        // 论证）：install 传 prefer=x.lock，于是「上游发了新版」
        // 不会自动换版本；update 传 NULL，全部重新挑。
        //
        // 为什么不无条件沿用：约束可能改了（^1.2 → ^2.0），
        // 那时锁定的 1.2.3 已不合法，必须重挑。所以判据是
        // 「仍然满足」，不是「有就用」。
        if (prefer) {
            for (int k = 0; k < prefer->count; k++) {
                if (strcmp(prefer->ent[k].name, name) != 0) continue;

                SemVer lv;
                if (!sem_parse(prefer->ent[k].version, &lv)) break;

                bool ok = true;
                for (int r = 0; r < nreq; r++) {
                    if (!sem_satisfies(&lv, &reqs[r])) { ok = false; break; }
                }
                // 源码也得还在。lock 记着但注册表里被删了的情况，
                // 硬用会得到一个指向空目录的路径 —— 那时编译会报
                // 「找不到函数」，而真正的原因是包没了。
                if (ok && reg_has_version(root, name, prefer->ent[k].version)) {
                    snprintf(picked, sizeof(picked), "%s",
                             prefer->ent[k].version);
                    why = NULL;
                }
                break;
            }
        }

        if (picked[0] == '\0') {
            why = pick_version(root, name, reqs, nreq, picked, sizeof(picked));
        }

        if (why) {
            // ── 冲突 / 缺失：报错并列出每一条约束的来源 ──────
            //
            // 「A 要 1.x、B 要 2.x」这种信息如果只说「无解」，
            // 用户得一个个翻清单才知道该找谁改。所以每一条都点明。
            if (strcmp(why, "NO_MATCH") == 0 && nreq > 1) {
                diag_error((SrcPos){ m->manifest_path, 0, 0 },
                           "依赖 `%s` 版本冲突：没有任何一个版本同时满足 %d 条要求",
                           name, nreq);
            } else if (strcmp(why, "NO_MATCH") == 0) {
                diag_error((SrcPos){ m->manifest_path, 0, 0 },
                           "依赖 `%s` 无解：没有版本满足要求", name);
            } else if (strcmp(why, "NO_PACKAGE") == 0) {
                diag_error((SrcPos){ m->manifest_path, 0, 0 },
                           "注册表里没有 `%s` 这个包", name);
            } else {
                diag_error((SrcPos){ m->manifest_path, 0, 0 },
                           "注册表目录不存在：%s", root);
            }

            for (int k = 0; k < cs.n; k++) {
                if (strcmp(cs.c[k].name, name) != 0) continue;
                char rq[96];
                sem_format_req(&cs.c[k].req, rq, sizeof(rq));
                if (cs.c[k].from[0]) {
                    diag_note("  %s 要求 %s", cs.c[k].from, rq);
                } else {
                    diag_note("  x.toml 要求 %s", rq);
                }
            }

            if (strcmp(why, "NO_MATCH") == 0) {
                VerEnt ent[REG_MAX_VERSIONS];
                int nv = list_versions(root, name, ent, REG_MAX_VERSIONS);
                if (nv > 0) {
                    diag_note("注册表里 `%s` 现有版本：", name);
                    for (int k = 0; k < nv && k < 8; k++) {
                        diag_note("  %s", ent[k].ver);
                    }
                    if (nv > 8) diag_note("  ...（还有 %d 个）", nv - 8);
                }
                diag_help("放宽约束，或者把缺的版本放进 %s/%s/。", root, name);
            } else if (strcmp(why, "NO_PACKAGE") == 0) {
                diag_help("把包放进 %s/%s/1.0.0/（含 src/lib.xle），"
                          "或改用 path 依赖。", root, name);
            } else {
                diag_help("建这个目录，或用环境变量 X_REGISTRY 指到已有注册表。");
            }
            return false;
        }

        // 记进结果
        if (out->count < 128) {
            RegNode* nd = &out->node[out->count++];
            memset(nd, 0, sizeof(*nd));
            // 深度取该包所有约束里最小的那个 —— 同一包既是 A 的
            // 直接依赖又是 B 的传递依赖时，说它「深度 0」比「深度 3」
            // 更符合直觉。
            int best = 1 << 20;
            for (int k = 0; k < cs.n; k++) {
                if (strcmp(cs.c[k].name, name) != 0) continue;
                if (nd->req_raw[0] == '\0') {
                    snprintf(nd->req_raw, sizeof(nd->req_raw), "%s",
                             cs.c[k].req_raw);
                }
                if (cs.c[k].depth < best) best = cs.c[k].depth;
            }
            nd->depth = (best == (1 << 20)) ? 0 : best;
            snprintf(nd->name, sizeof(nd->name), "%s", name);
            snprintf(nd->version, sizeof(nd->version), "%s", picked);
        }

        if (!absorb_deps(root, name, picked, cs.c[i].depth, &cs)) return false;
    }

    // 根清单里的 key 名补回结果（诊断与 `pkg tree` 要显示它）
    for (int i = 0; i < out->count; i++) {
        for (int k = 0; k < m->dep_count; k++) {
            const PkgDep* d = &m->deps[k];
            if (d->kind != DEP_REGISTRY) continue;
            const char* realname = d->package ? d->package : d->key;
            if (strcmp(realname, out->node[i].name) == 0) {
                snprintf(out->node[i].key, sizeof(out->node[i].key), "%s", d->key);
                break;
            }
        }
    }

    return true;
}

// ============================================================
// 编译期解析一个 registry 依赖
// ============================================================

bool reg_resolve_entry(const char* manifest_root, const char* name,
                       const char* req_raw, char* out, size_t out_len,
                       bool* used_lock) {
    if (used_lock) *used_lock = false;
    if (!manifest_root || !name || !out || out_len == 0) return false;

    char root[1200];
    reg_root(root, sizeof(root));

    // ── 第一顺位：x.lock ──────────────────────────────────
    //
    // lock 里有就用它，完全不看约束。这不是「忽略用户意图」——
    // 约束在 install 的时候已经被满足过了，lock 记的就是那次
    // 满足的结果。构建时再比一遍，只会让同一个 lock + 同一份
    // 源码产出不同的版本。
    char lpath[1400];
    snprintf(lpath, sizeof(lpath), "%s/x.lock", manifest_root);

    LockFile lf;
    if (lock_read(lpath, &lf)) {
        for (int i = 0; i < lf.count; i++) {
            if (strcmp(lf.ent[i].name, name) != 0) continue;
            char vdir[1300];
            lock_entry_dir(root, &lf.ent[i], vdir, sizeof(vdir));

            char e[1300];
            if (reg_entry_file(root, name, lf.ent[i].version, e, sizeof(e))) {
                plat_abs_path(e, out, out_len);
                if (used_lock) *used_lock = true;
                return true;
            }
            // lock 里记的版本源码不见了 —— 继续往下试，让编译能过。
            // 这种情况通常是换机器/注册表被清了，`x pkg install`
            // 会把它报成 DRIFT_GONE。构建阶段不必因此失败：
            // 约束还有解就先用着，能编出东西比卡住有用。
        }
    }

    // ── 第二顺位：按约束从注册表挑 ────────────────────────
    SemReq req;
    if (!sem_parse_req(req_raw, &req)) return false;

    char picked[32];
    if (reg_pick(root, name, &req, picked, sizeof(picked)) != NULL) {
        return false;
    }

    char e[1300];
    if (!reg_entry_file(root, name, picked, e, sizeof(e))) return false;
    plat_abs_path(e, out, out_len);
    return true;
}

void reg_print_solution(const RegSolution* s, FILE* out) {
    if (!s || !out) return;
    if (s->count == 0) {
        fprintf(out, "  （没有 registry 依赖）\n");
        return;
    }
    for (int i = 0; i < s->count; i++) {
        const RegNode* n = &s->node[i];
        if (n->key[0]) {
            fprintf(out, "  %-16s %-10s   直接依赖（@%s，要求 %s）\n",
                    n->name, n->version, n->key, n->req_raw);
        } else {
            fprintf(out, "  %-16s %-10s   传递依赖\n", n->name, n->version);
        }
    }
}