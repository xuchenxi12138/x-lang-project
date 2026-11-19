// lockfile.c — x.lock 的读写与漂移检测

#include "lockfile.h"

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "x.h"

// 读一个小文本文件到堆上。返回 NULL 表示读不到。
static char* slurp(const char* path) {
    FILE* f = x_fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)x_alloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

bool lock_read(const char* path, LockFile* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->format = 1;

    if (!x_file_exists(path)) return true;   // 没有 lock 不算错误

    char* src = slurp(path);
    if (!src) {
        diag_error((SrcPos){ path, 0, 0 }, "x.lock 打不开");
        diag_help("删掉它并重新运行 `x pkg install`。");
        return false;
    }

    // 逐行解析。用的键只有 format / name / version / dir 四个。
    char cur_name[64] = "";
    char cur_ver[32] = "";
    char cur_dir[160] = "";
    bool in_pkg = false;

    int line_no = 0;
    char* p = src;
    int bad = 0;

    while (*p) {
        line_no++;
        char* eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        char* l = p;
        // 去行尾 \r（lock 会在不同平台上被读写，行尾不该影响解析）
        size_t ll = strlen(l);
        while (ll > 0 && (l[ll - 1] == '\r' || l[ll - 1] == ' ')) l[--ll] = '\0';

        // 去掉 UTF-8 BOM
        if (line_no == 1 && (unsigned char)l[0] == 0xEF &&
            (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) {
            l += 3;
        }

        // 跳过前导空白
        while (*l == ' ' || *l == '\t') l++;

        if (*l == '\0' || *l == '#') {
            // 空行 / 注释
        } else {
            char* eq = strchr(l, '=');
            if (!eq) {
                diag_error((SrcPos){ path, line_no, 1 },
                           "x.lock 里这一行不是 `键 = 值`");
                diag_note("lock 是生成物，不该手工编辑。");
                diag_help("删掉 x.lock 再运行 `x pkg install` 重新生成。");
                bad++;
            } else {
                *eq = '\0';
                char* key = l;
                size_t kl = strlen(key);
                while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) {
                    key[--kl] = '\0';
                }
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;

                if (strcmp(key, "lockfile") == 0) {
                    out->format = atoi(val);
                } else if (strncmp(key, "package ", 8) == 0) {
                    // 每条记录是 `package <子键> = 值` 三行一组。
                    //
                    // ── 这里踩过一个坑，值得留个记录 ──────────
                    //
                    // 第一版写成 `strcmp(key, "package") == 0` 再在
                    // 里面 strchr(key, ' ') 找子键。但 key 已经在上面
                    // 按第一个 '=' 切过了，它拿到的是 "package name"
                    // 而不是 "package" —— 于是那个 strcmp 永远不成立，
                    // 整段解析成了**死代码**。
                    //
                    // 症状特别隐蔽：lock 写得出来、长得完全正确，
                    // 读回来却永远是空表。于是 install 每次都认为
                    // 「lock 里什么都没有」，每次重算、每次重写，
                    // 而 lock 检查永远报「还没有 x.lock」。
                    // 文件明明在那里，内容明明对 —— 就是读不见。
                    //
                    // 教训：解析器里「切分之后再用切分前的形状做判断」
                    // 是必须当场核对的事，因为编译器不会提醒你。
                    const char* sub = key + 8;
                    while (*sub == ' ' || *sub == '\t') sub++;

                    if (strcmp(sub, "name") == 0) {
                        // 新的一条开始 → 先把上一条收进结果
                        if (in_pkg && cur_name[0] && out->count < LOCK_MAX_ENTRIES) {
                            LockEntry* e = &out->ent[out->count++];
                            snprintf(e->name, sizeof(e->name), "%s", cur_name);
                            snprintf(e->version, sizeof(e->version), "%s", cur_ver);
                            snprintf(e->dir, sizeof(e->dir), "%s", cur_dir);
                        }
                        snprintf(cur_name, sizeof(cur_name), "%s", val);
                        cur_ver[0] = '\0';
                        cur_dir[0] = '\0';
                        in_pkg = true;
                    } else if (strcmp(sub, "version") == 0) {
                        snprintf(cur_ver, sizeof(cur_ver), "%s", val);
                    } else if (strcmp(sub, "dir") == 0) {
                        snprintf(cur_dir, sizeof(cur_dir), "%s", val);
                    } else {
                        // 不认识的子键：忽略（前向兼容）
                    }
                } else {
                    // 不认识的顶层键：忽略，但记一笔。
                    // 前向兼容 —— 未来的 lock 格式可能加字段。
                }
            }
        }

        if (!saved) break;
        p = eol + 1;
    }

    // 收尾最后一条
    if (in_pkg && cur_name[0] && out->count < LOCK_MAX_ENTRIES) {
        LockEntry* e = &out->ent[out->count++];
        snprintf(e->name, sizeof(e->name), "%s", cur_name);
        snprintf(e->version, sizeof(e->version), "%s", cur_ver);
        snprintf(e->dir, sizeof(e->dir), "%s", cur_dir);
    }

    free(src);
    return bad == 0;
}

// 一条记录写成四行。dir 为空时按 name/version 兜出来 ——
// 手写的 lock 少写 dir 也能用。
static void write_entry(FILE* f, const LockEntry* e) {
    char dir[160];
    if (e->dir[0]) {
        snprintf(dir, sizeof(dir), "%s", e->dir);
    } else {
        snprintf(dir, sizeof(dir), "%s/%s", e->name, e->version);
    }
    fprintf(f, "package name = %s\n", e->name);
    fprintf(f, "package version = %s\n", e->version);
    fprintf(f, "package dir = %s\n", dir);
    fprintf(f, "\n");
}

bool lock_write(const char* path, const LockFile* lf) {
    if (!path || !lf) return false;

    // 原子写：先写 .tmp，成功后 rename。
    //
    // 为什么不用 fopen("w") 直接覆盖：写到一半断电/磁盘满，会留下
    // 一个被截断的 lock。而 lock 是「可复现」的唯一依据，坏了比
    // 没有更糟 —— 下一次构建会按半份 lock 走，选出错的版本。
    char tmp[1400];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = x_fopen(tmp, "wb");
    if (!f) {
        diag_error((SrcPos){ path, 0, 0 }, "写不了 %s", tmp);
        return false;
    }

    fprintf(f, "# 由 `x pkg install` 生成 —— 不要手工编辑。\n");
    fprintf(f, "# 它记录的是**上次解析选定的版本**，用于可复现构建。\n");
    fprintf(f, "# 要升级依赖，改 x.toml 里的约束再跑一次 install。\n");
    fprintf(f, "\n");
    fprintf(f, "lockfile = %d\n", lf->format > 0 ? lf->format : 1);
    fprintf(f, "\n");

    for (int i = 0; i < lf->count; i++) {
        write_entry(f, &lf->ent[i]);
    }

    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0) ok = false;

    if (!ok) {
        x_remove(tmp);
        diag_error((SrcPos){ path, 0, 0 }, "写 x.lock 失败（磁盘满？）");
        return false;
    }

    x_remove(path);
    if (!x_rename(tmp, path)) {
        x_remove(tmp);
        diag_error((SrcPos){ path, 0, 0 }, "无法把 %s 重命名为 x.lock", tmp);
        return false;
    }
    return true;
}

void lock_from_solution(LockFile* lf, const RegSolution* s) {
    if (!lf) return;
    memset(lf, 0, sizeof(*lf));
    lf->format = 1;
    if (!s) return;

    for (int i = 0; i < s->count && lf->count < LOCK_MAX_ENTRIES; i++) {
        const RegNode* n = &s->node[i];
        if (!n->name[0] || !n->version[0]) continue;
        LockEntry* e = &lf->ent[lf->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", n->name);
        snprintf(e->version, sizeof(e->version), "%s", n->version);
        snprintf(e->dir, sizeof(e->dir), "%s/%s", n->name, n->version);
    }
}

void lock_entry_dir(const char* root, const LockEntry* e, char* out,
                    size_t out_len) {
    if (!out || out_len == 0) return;
    if (!e) { out[0] = '\0'; return; }
    if (e->dir[0]) {
        snprintf(out, out_len, "%s/%s", root ? root : "", e->dir);
    } else {
        snprintf(out, out_len, "%s/%s/%s", root ? root : "", e->name, e->version);
    }
}

// ============================================================
// 漂移检测
// ============================================================

static const LockEntry* lock_find(const LockFile* lf, const char* name) {
    for (int i = 0; i < lf->count; i++) {
        if (strcmp(lf->ent[i].name, name) == 0) return &lf->ent[i];
    }
    return NULL;
}

static const RegNode* sol_find(const RegSolution* s, const char* name) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->node[i].name, name) == 0) return &s->node[i];
    }
    return NULL;
}

int lock_drift(const LockFile* lf, const RegSolution* sol,
               Drift* out, int max) {
    if (!lf || !sol || !out || max <= 0) return 0;
    int n = 0;

    // 1. lock 里有的：查版本是否还一致、源码是否还在
    for (int i = 0; i < lf->count && n < max; i++) {
        const LockEntry* e = &lf->ent[i];
        const RegNode* w = sol_find(sol, e->name);
        if (!w) {
            // x.toml 里已经删掉这个依赖了，但 lock 还记着
            Drift* d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->kind = DRIFT_EXTRA;
            snprintf(d->name, sizeof(d->name), "%s", e->name);
            snprintf(d->locked, sizeof(d->locked), "%s", e->version);
            continue;
        }
        if (strcmp(e->version, w->version) != 0) {
            Drift* d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->kind = DRIFT_VERSION;
            snprintf(d->name, sizeof(d->name), "%s", e->name);
            snprintf(d->locked, sizeof(d->locked), "%s", e->version);
            snprintf(d->wanted, sizeof(d->wanted), "%s", w->version);
        }
    }

    // 2. 解析出来但 lock 里没有的
    for (int i = 0; i < sol->count && n < max; i++) {
        const RegNode* w = &sol->node[i];
        if (!w->name[0]) continue;
        if (lock_find(lf, w->name)) continue;
        Drift* d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->kind = DRIFT_MISSING;
        snprintf(d->name, sizeof(d->name), "%s", w->name);
        snprintf(d->wanted, sizeof(d->wanted), "%s", w->version);
    }

    return n;
}

void lock_print_drift(const Drift* d, int n, FILE* out) {
    if (!d || !out) return;
    if (n <= 0) return;
    fprintf(out, "x.lock 与当前依赖不一致（%d 处）：\n", n);
    for (int i = 0; i < n; i++) {
        switch (d[i].kind) {
        case DRIFT_VERSION:
            fprintf(out, "  %-16s lock 里是 %s，按 x.toml 现在会选 %s\n",
                    d[i].name, d[i].locked, d[i].wanted);
            break;
        case DRIFT_EXTRA:
            fprintf(out, "  %-16s lock 里记着 %s，但 x.toml 已经不要它了\n",
                    d[i].name, d[i].locked);
            break;
        case DRIFT_MISSING:
            fprintf(out, "  %-16s x.toml 新加的依赖，lock 里没有（会选 %s）\n",
                    d[i].name, d[i].wanted);
            break;
        case DRIFT_GONE:
            fprintf(out, "  %-16s lock 里的 %s 在注册表里找不到了\n",
                    d[i].name, d[i].locked);
            break;
        case DRIFT_MANIFEST:
            fprintf(out, "  %-16s 清单层面不一致\n", d[i].name);
            break;
        case DRIFT_NONE:
            break;
        }
    }
}