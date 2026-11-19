// pkg.c — 清单解析与包查找
//
// 设计理由见 pkg.h。这个文件的核心主张：
// **依赖清单是唯一的真相来源**，`import @name` 只能引用已声明的键。

#include "x.h"
#include "pkg.h"
#include "platform.h"
#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ============================================================
// 小工具
// ============================================================

// 去掉首尾空白，原地修改，返回新起点。
static char* trim(char* s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s[--n] = '\0';
        else break;
    }
    return s;
}

// 剥掉行内注释。**必须跳过引号里的 #**，否则
//     version = "1.0 # not a comment"
// 会被截断成一个坏字符串。TOML 里 # 在字符串中是普通字符。
static void strip_comment(char* s) {
    bool in_str = false;
    char quote = 0;
    for (char* p = s; *p; p++) {
        if (in_str) {
            if (*p == '\\' && *(p + 1)) { p++; continue; }
            if (*p == quote) in_str = false;
        } else {
            if (*p == '"' || *p == '\'') { in_str = true; quote = *p; }
            else if (*p == '#') { *p = '\0'; return; }
        }
    }
}

// 读一个带引号的字符串。返回 malloc 的副本，失败返回 NULL。
//
// 只支持 " 和 ' 两种引号，不做多行字符串 —— 清单里没有那种需求，
// 而多行字符串的转义规则（行末反斜杠续行、三个引号的边界）是
// TOML 里最容易写错的部分，没有需求就不实现。
static char* parse_string(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '"' && *s != '\'') return NULL;
    char q = *s++;
    const char* start = s;
    char* out = x_alloc(strlen(s) + 1);
    size_t k = 0;
    while (*s && *s != q) {
        if (*s == '\\' && *(s + 1)) {
            s++;
            switch (*s) {
                case 'n': out[k++] = '\n'; break;
                case 't': out[k++] = '\t'; break;
                case '\\': out[k++] = '\\'; break;
                case '"': out[k++] = '"'; break;
                default:  out[k++] = *s; break;
            }
            s++;
        } else {
            out[k++] = *s++;
        }
    }
    if (*s != q) { free(out); return NULL; }   // 引号没闭合
    out[k] = '\0';
    (void)start;
    return out;
}

// ============================================================
// 清单查找
// ============================================================

// 从 start_dir 向上逐级找 x.toml。
//
// 为什么要向上找：跟 Cargo/npm 一样，项目根目录由清单文件定义，
// 而不是由「你在哪个子目录」定义。这样 `cd src && x build main.xle`
// 也能正确找到项目根，不必要求用户在根目录执行。
bool pkg_find_manifest(const char* start_dir, char* out, size_t out_len) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", start_dir);

    for (int guard = 0; guard < 64; guard++) {   // 防死循环的硬上限
        char probe[1300];
        // Windows 与类 Unix 都接受 /，平台层不需要参与
        snprintf(probe, sizeof(probe), "%s/x.toml", dir);

        FILE* f = x_fopen(probe, "rb");
        if (f) {
            fclose(f);
            snprintf(out, out_len, "%s", probe);
            return true;
        }

        // 上一级
        char* slash = strrchr(dir, '/');
        char* bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (!slash) break;
        if (slash == dir) break;          // 已经到 "/"
        if (slash == dir + 2 && dir[1] == ':') {
            // "C:/" 这一级：再往上没有了
            dir[2] = '\0';
        } else {
            *slash = '\0';
        }
    }
    return false;
}

// ============================================================
// 解析
// ============================================================

// 判断 key 是否属于 [package] 段
static int parse_lines(const char* path, char* src, PkgManifest* m);

bool pkg_load(const char* manifest_path, PkgManifest* out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char* src = read_file(manifest_path, &len);
    if (!src) {
        diag_error((SrcPos){ manifest_path, 0, 0 },
                   "无法读取清单文件 `%s`", manifest_path);
        return false;
    }

    out->manifest_path = x_strdup(manifest_path);

    // 根目录 = 清单所在目录
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", manifest_path);
        char* slash = strrchr(dir, '/');
        char* bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) *slash = '\0';
        else snprintf(dir, sizeof(dir), ".");
        out->root_dir = x_strdup(dir);
    }

    bool ok = parse_lines(manifest_path, src, out) > 0;
    free(src);

    if (!ok) {
        pkg_free(out);
        return false;
    }

    // 默认入口
    if (!out->entry) out->entry = x_strdup("src/main.xle");

    return true;
}

// 当前的段名。用一个字符串就够 —— 清单只有一层半结构
// （[package]、[dependencies]），不需要真正的树。
static int parse_lines(const char* path, char* src, PkgManifest* m) {
    char section[128] = "";

    // 按行切。不用 strtok（它会吞掉空行，而行号对诊断很重要）。
    int line_no = 0;
    char* line = src;
    int errors = 0;

    while (*line) {
        line_no++;

        // 找行尾
        char* eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        // 处理这一行（line 到 eol）
        char* l = line;
        // 去掉 UTF-8 BOM（Windows 记事本会加）
        if (line_no == 1 && (unsigned char)l[0] == 0xEF &&
            (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) {
            l += 3;
        }
        l = trim(l);
        strip_comment(l);
        l = trim(l);

        if (*l == '\0') {
            // 空行 / 纯注释
        } else if (*l == '[') {
            // ---- 段 ----
            char* end = strchr(l, ']');
            if (!end) {
                diag_error((SrcPos){ path, line_no, 1 },
                           "表名没有闭合的 `]`");
                diag_help("写法是 `[package]` 或 `[dependencies]`");
                errors++;
            } else {
                *end = '\0';
                char* name = trim(l + 1);
                // [[array-of-tables]] 不支持
                if (*name == '[') {
                    diag_error((SrcPos){ path, line_no, 1 },
                               "不支持 `[[...]]`（数组表）");
                    diag_note("清单只需要 [package] 与 [dependencies] 两个表。");
                    errors++;
                } else {
                    snprintf(section, sizeof(section), "%s", name);
                }
            }
        } else {
            // ---- 键值 ----
            char* eq = strchr(l, '=');
            if (!eq) {
                diag_error((SrcPos){ path, line_no, 1 },
                           "这一行不是 `键 = 值` 的形式");
                diag_note("清单支持：`[表]`、`键 = \"字符串\"`、`键 = 123`、"
                          "`键 = true/false`、`# 注释`");
                diag_help("如果这是某个多行值的一部分，P0 不支持多行字符串。");
                errors++;
            } else {
                *eq = '\0';
                char* key = trim(l);
                char* val = trim(eq + 1);

                // 去掉键的引号（TOML 允许 "key" = ...）
                char* kq = parse_string(key);
                if (kq) { free(kq); }   // 键名带引号的情况只做兼容，不特殊处理

                // ---- 内联表 / 数组：显式报错 ----
                if (*val == '{') {
                    // 内联表：只支持一种形式 `{ path = "..." }`
                    // 因为 path 依赖是这个子集里唯一会用到内联表的场景。
                    //
                    // 不做成通用内联表解析：一旦支持了 { }，用户就会
                    // 期待 { version = "1", features = [...] }，
                    // 而 features 又要数组…每个都引出一个新语法。
                    // 与其做半个 TOML、让用户以为支持了，不如只支持
                    // 真实需要的那一种，其它明确报错。
                    if (strcmp(section, "dependencies") != 0) {
                        diag_error((SrcPos){ path, line_no, 1 },
                                   "内联表 `{ }` 只支持用在 [dependencies] 里");
                        errors++;
                    } else {
                        char* pathval = strstr(val, "path");
                        if (!pathval) {
                            diag_error((SrcPos){ path, line_no, 1 },
                                       "依赖 `%s` 的内联表里没有 `path`", key);
                            diag_note("P0 支持两种依赖写法：");
                            diag_note("  util = \"1.0\"                  版本号（注册中心）");
                            diag_note("  util = { path = \"../util\" }   本地路径");
                            errors++;
                        } else {
                            char* eq2 = strchr(pathval, '=');
                            char* pv = eq2 ? parse_string(eq2 + 1) : NULL;
                            if (!pv) {
                                diag_error((SrcPos){ path, line_no, 1 },
                                           "`path` 的值必须是字符串");
                                errors++;
                            } else if (m->dep_count < PKG_MAX_DEPS) {
                                PkgDep* d = &m->deps[m->dep_count++];
                                d->key = x_strdup(key);
                                d->line = line_no;
                                d->kind = DEP_PATH;
                                d->path = pv;
                            } else {
                                diag_error((SrcPos){ path, line_no, 1 },
                                           "依赖数量超过上限 %d", PKG_MAX_DEPS);
                                free(pv);
                                errors++;
                            }
                        }
                    }
                } else if (*val == '[') {
                    diag_error((SrcPos){ path, line_no, 1 },
                               "数组 `[ ]` 在 P0 的清单里不支持");
                    diag_note("还没实现需要数组的场景（features、多个作者等）。");
                    errors++;
                } else if (*val == '"' || *val == '\'') {
                    // ---- 字符串值 ----
                    char* sv = parse_string(val);
                    if (!sv) {
                        diag_error((SrcPos){ path, line_no, 1 },
                                   "字符串的引号没有闭合");
                        errors++;
                    } else if (strcmp(section, "package") == 0) {
                        if (strcmp(key, "name") == 0)          { free(m->name);        m->name = sv; }
                        else if (strcmp(key, "version") == 0)  { free(m->version);     m->version = sv; }
                        else if (strcmp(key, "entry") == 0)    { free(m->entry);       m->entry = sv; }
                        else if (strcmp(key, "description") == 0) { free(m->description); m->description = sv; }
                        else if (strcmp(key, "profile") == 0)  { free(m->profile);     m->profile = sv; }
                        else {
                            // 不认识的键**不报错**：清单会随版本增加键，
                            // 旧编译器读到新键应该能继续工作（前向兼容）。
                            // 但要说一声，因为拼错键名是最常见的错误。
                            diag_warn((SrcPos){ path, line_no, 1 },
                                      "[package] 里不认识的键 `%s`，已忽略", key);
                            free(sv);
                        }
                    } else if (strcmp(section, "dependencies") == 0) {
                        if (m->dep_count < PKG_MAX_DEPS) {
                            PkgDep* d = &m->deps[m->dep_count++];
                            d->key = x_strdup(key);
                            d->line = line_no;
                            d->kind = DEP_REGISTRY;
                            d->version = sv;
                        } else {
                            diag_error((SrcPos){ path, line_no, 1 },
                                       "依赖数量超过上限 %d", PKG_MAX_DEPS);
                            free(sv);
                            errors++;
                        }
                    } else {
                        // 其它段（[scripts] 等）在 P0 尚未实现，
                        // 静默忽略值 —— 但段名本身要认，否则
                        // `[profile.release]` 这种会很吵。
                        free(sv);
                    }
                } else if (strcmp(val, "true") == 0 || strcmp(val, "false") == 0) {
                    // 布尔：目前没有键需要它，忽略值即可
                } else {
                    // ---- 数字 ----
                    char* endp = NULL;
                    long long v = strtoll(val, &endp, 10);
                    (void)v;
                    if (endp && *trim(endp) != '\0') {
                        diag_error((SrcPos){ path, line_no, 1 },
                                   "值的类型无法识别：`%s`", val);
                        diag_note("清单里的值只能是字符串、整数、布尔。");
                        diag_help("字符串要加引号，例如 `version = \"1.0\"`。"
                                  "版本号是字符串，不是数字 —— 写成 1.0 会变成浮点，"
                                  "而 P0 的清单不认浮点。");
                        errors++;
                    }
                }
            }
        }

        if (!saved) break;      // 文件末尾没有换行
        line = eol + 1;
    }

    if (errors > 0) return 0;

    // 必填项检查
    if (!m->name) {
        diag_error((SrcPos){ path, 0, 0 }, "清单缺少 `[package] name`");
        diag_note("包名是标识依赖的唯一名字，不能省。");
        diag_help("在 `[package]` 下加一行 `name = \"你的包名\"`。");
        return 0;
    }
    if (!m->version) {
        diag_error((SrcPos){ path, 0, 0 }, "清单缺少 `[package] version`");
        diag_help("在 `[package]` 下加一行 `version = \"0.1.0\"`。");
        return 0;
    }

    // 包名必须是合法标识符 —— 因为它会出现在 `import @name` 里。
    //
    // 这一条在 spec/03 §6.4.1 有完整论证：允许连字符就必须引入
    // 「自动转下划线」的隐式规则，那样 grep 就断链了。
    //
    // ── 为什么先判首字母，再逐个字符判 ──────────────────────
    //
    // 反过来写会出现一句误导人的错误：包名是 "T-2" 时，逐个字符
    // 检查会先撞上 'T'，于是报
    //
    //     包名 `T-2` 含有非法字符 `T`
    //
    // 但 'T' 本身不是非法字符 —— 大写字母的问题在于**不能位于
    // 开头**，而不是不能出现（虽然 P0 也确实只允许小写，但那是
    // 另一条规则）。而且这样写会让下面那段「必须以小写字母开头」
    // 变成**永远走不到的代码**：首字母大写一定已在循环里被拦下。
    //
    // 死代码比没有代码更糟 —— 它让人以为这条规则被单独检查过。
    if (!islower((unsigned char)m->name[0])) {
        // 首字符有两种不合格方式，处理方式完全不同：
        //   大写字母  → 转小写就行，报错时直接给出结果
        //   数字/符号 → 转小写没用，必须让用户自己起个名字
        //
        // 混在一起报会出现这种打转的建议：包名是 `9abc`，报
        // 「必须以小写字母开头」，然后 help 说「改成 `9abc`」——
        // 建议和现状一模一样，用户照做一遍还是同样的错。
        if (isupper((unsigned char)m->name[0])) {
            char fixed[128];
            size_t k = 0;
            for (const char* q = m->name; *q && k + 1 < sizeof(fixed); q++) {
                char c = *q;
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
                fixed[k++] = c;
            }
            fixed[k] = '\0';
            diag_error((SrcPos){ path, 0, 0 },
                       "包名 `%s` 必须以小写字母开头", m->name);
            diag_note("包名规则是 `[a-z][a-z0-9_]*`。");
            diag_help("改成 `%s`。", fixed);
        } else {
            diag_error((SrcPos){ path, 0, 0 },
                       "包名 `%s` 的第一个字符 `%c` 不是字母", m->name, m->name[0]);
            diag_note("包名规则是 `[a-z][a-z0-9_]*` —— "
                      "第一个字符必须是字母，因为 `import @名字` 里它要能当标识符。");
            diag_help("在开头加一个字母，例如 `p%s`。", m->name);
        }
        return 0;
    }

    for (const char* p = m->name; *p; p++) {
        if (islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_') {
            continue;
        }
        // 到这里才是真的「这个字符不允许出现」。
        // 分开报大写与其它字符，因为用户要做的事不同。
        if (isupper((unsigned char)*p)) {
            diag_error((SrcPos){ path, 0, 0 },
                       "包名 `%s` 不能含大写字母 `%c`", m->name, *p);
            diag_note("包名只能用 `[a-z0-9_]`。");
            diag_help("改成小写：`%c` → `%c`。", *p,
                      (char)(*p - 'A' + 'a'));
        } else {
            diag_error((SrcPos){ path, 0, 0 },
                       "包名 `%s` 含有非法字符 `%c`", m->name, *p);
            diag_note("包名只能用 `[a-z0-9_]` —— 因为它要能作为 "
                      "`import @名字` 的标识符使用。");
            diag_help("把 `-` 换成 `_`，例如 `http-client` → `http_client`。");
        }
        return 0;
    }

    return 1;
}

void pkg_free(PkgManifest* m) {
    if (!m) return;
    free(m->name);
    free(m->version);
    free(m->entry);
    free(m->description);
    free(m->profile);
    free(m->manifest_path);
    free(m->root_dir);
    for (int i = 0; i < m->dep_count; i++) {
        free(m->deps[i].key);
        free(m->deps[i].package);
        free(m->deps[i].version);
        free(m->deps[i].path);
    }
    memset(m, 0, sizeof(*m));
}

const PkgDep* pkg_find_dep(const PkgManifest* m, const char* key) {
    if (!m || !key) return NULL;
    for (int i = 0; i < m->dep_count; i++) {
        if (strcmp(m->deps[i].key, key) == 0) return &m->deps[i];
    }
    return NULL;
}

// ============================================================
// 解析 import @name
// ============================================================

static bool file_exists(const char* p) {
    FILE* f = x_fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// ============================================================
// 一个依赖的源码可能在哪几个位置
// ============================================================
//
// 抽成独立函数，因为有**两个**调用方需要它，而它们必须用同一套
// 判据 —— 否则会出现「x pkg add 说没问题、编译说找不到」这种
// 自相矛盾的体验：
//
//   1. pkg_resolve_import  编译时解析 import
//   2. pkg_check_path_dep   `x pkg add` 时立即验证路径可用
//
// 第 2 个是本轮补上的。以前 `x pkg add --path ./nope` 会无条件
// 成功，于是错误被推迟到几小时后某次编译 —— 那时用户早已忘了
// 自己刚加过依赖，看到「源码找不到」只会困惑。
int pkg_candidate_paths(const char* root_dir, const char* dep_path,
                        const char* name, bool is_path_dep,
                        char (*cand)[1300], int max) {
    int n = 0;
    if (max <= 0) return 0;

    if (is_path_dep) {
        // 本地路径依赖。默认相对清单目录，但绝对路径不拼前缀 ——
        // 与 pkg_check_path_dep 用同一个判据，否则会出现
        // 「add 时校验过了、编译时却找不到」。
        const char* base = pkg_path_is_absolute(dep_path) ? "" : root_dir;
        const char* sep  = pkg_path_is_absolute(dep_path) ? "" : "/";
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s%s%s/src/lib.xle", base, sep, dep_path);
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s%s%s/src/%s.xle", base, sep, dep_path, name);
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s%s%s/%s.xle", base, sep, dep_path, name);
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s%s%s/main.xle", base, sep, dep_path);
    } else {
        // 注册中心依赖：P0 没有网络，只认 vendor 目录。
        //
        // 为什么先做 vendor 而不是先做下载：下载需要网络、HTTP 客户端、
        // TLS、哈希校验、注册中心本身 —— 那是一整条链。而 vendor
        // 只需要一个目录约定。先让「包管理」这件事端到端能跑通
        // （声明 → 引用 → 解析 → 编译），比先让「能联网下载」更有价值。
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s/vendor/%s/src/lib.xle", root_dir, name);
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s/vendor/%s/src/%s.xle", root_dir, name, name);
        if (n < max) snprintf(cand[n++], sizeof(cand[0]), "%s/vendor/%s/%s.xle", root_dir, name, name);
    }
    return n;
}

// 一个 path 依赖的路径是绝对的还是相对的。
//
// 判据覆盖三种绝对形式：
//   C:/x  C:\x     Windows 盘符
//   /x             POSIX 根
//   \\server\x     Windows UNC（本工具链不涉及，但判了不亏）
//
// 为什么必须区分：path 依赖默认是**相对于清单所在目录**的，拼接时
// 要加 root_dir 前缀。但用户完全可能写绝对路径（尤其是跨盘的包），
// 那时再拼前缀就得到 `C:\proj/C:/other/pkg` 这种不存在的路径 ——
// 而错误信息会说「这个目录不存在」，让用户去检查一个明明存在的目录。
bool pkg_path_is_absolute(const char* p) {
    if (!p || !*p) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    // 盘符形式：字母 + 冒号
    if (p[1] == ':' &&
        ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) {
        return true;
    }
    return false;
}

// 校验一个 path 依赖指向的目录**现在**就能用。
//
// 用来在 `x pkg add` 当场拦住「路径写错了 / 忘了建目录 / 目录是空的」。
// 返回 NULL 表示可用，否则返回一句人类可读的原因（指向静态字符串，
// 不需要释放）。
const char* pkg_check_path_dep(const char* root_dir, const char* dep_path,
                               const char* name) {
    if (!root_dir || !dep_path || !name) return "PARAM";

    // ── 绝对路径不拼前缀 ────────────────────────────────
    //
    // 这里踩过一个自造的 bug：无条件写成 `root_dir + "/" + dep_path`。
    // 用户传 `C:/xtmp/dep` 这种绝对路径时，拼出来是
    // `C:\proj/C:/xtmp/dep` —— 一个不存在的路径，于是校验失败并说
    // 「这个目录不存在」，而用户盯着那个确实存在的目录一头雾水。
    //
    // 判断同一个路径该不该加前缀，是发现路径问题的第一道关。
    char dir_probe[1400];
    if (pkg_path_is_absolute(dep_path)) {
        snprintf(dir_probe, sizeof(dir_probe), "%s", dep_path);
    } else {
        snprintf(dir_probe, sizeof(dir_probe), "%s/%s", root_dir, dep_path);
    }
    if (!x_is_dir(dir_probe)) return "DIR_MISSING";

    char cand[4][1300];
    int n = pkg_candidate_paths(root_dir, dep_path, name, true, cand, 4);
    for (int i = 0; i < n; i++) {
        if (file_exists(cand[i])) return NULL;
    }
    return "NO_ENTRY";
}

bool pkg_resolve_import(const PkgManifest* m, const char* name,
                        const char* cause_file, int cause_line,
                        char* out, size_t out_len) {
    const PkgDep* d = pkg_find_dep(m, name);
    if (!d) {
        // 主位置指向**源码里的 import**，不是清单。
        //
        // 为什么：用户改的是源码那一行。指向 x.toml 会让人以为
        // 要改清单，而最常见的情况恰恰相反 —— 这个包确实不该被依赖，
        // 应该删掉那句 import。
        diag_error((SrcPos){ cause_file ? cause_file : m->manifest_path,
                             cause_line > 0 ? cause_line : 1, 1 },
                   "`import @%s`：清单里没有这个依赖", name);
        diag_note("`@name` 只能引用 x.toml 里 [dependencies] 已声明的键。");
        diag_note("这条规则的意义：依赖清单是唯一的真相来源，"
                  "否则 `x pkg fetch --locked` 无法保证可复现。");
        diag_help("要引入它，在 %s 的 [dependencies] 下加一行：%s = \"0.1\"",
                  m->manifest_path, name);
        diag_help("不要引入，就删掉这句 import。");
        return false;
    }

    // ── registry 依赖：先查注册表（含 lock），再退回 vendor ──
    //
    // 顺序不能反。vendor/ 是 v0.13 的旧布局（手工把源码拷进项目），
    // 注册表是新的：源码放机器上共享，项目只记版本。新布局优先，
    // 老项目照样能编 —— 但注册表里有的包，行为以注册表为准，
    // 否则「装好了新版本却还在用 vendor 里的旧副本」会非常难查。
    if (d->kind == DEP_REGISTRY) {
        char ent[1300];
        bool via_lock = false;
        if (reg_resolve_entry(m->root_dir, name, d->version, ent, sizeof(ent),
                              &via_lock)) {
            snprintf(out, out_len, "%s", ent);
            return true;
        }
        // 注册表里没有 → 落到下面 vendor 的候选路径。
        // 不在这里报错：老项目只用 vendor 是完全合法的用法。
    }

    // 候选路径列表。按顺序试，第一个存在的胜出。
    char cand[4][1300];
    int ncand = pkg_candidate_paths(m->root_dir, d->path, name,
                                    d->kind == DEP_PATH, cand, 4);

    for (int i = 0; i < ncand; i++) {
        if (file_exists(cand[i])) {
            // 规范化成绝对路径 —— 模块去重靠绝对路径比较
            plat_abs_path(cand[i], out, out_len);
            return true;
        }
    }

    // 找不到：把试过的路径全列出来。
    //
    // 这是「解析失败」类错误里最有用的一条信息 —— 用户不需要猜
    // 编译器在哪个目录下找，直接看到路径就能判断是自己放错地方了，
    // 还是路径写错了。
    diag_error((SrcPos){ m->manifest_path, d->line, 1 },
               "依赖 `%s` 的源码找不到", name);
    if (d->kind == DEP_PATH) {
        // 绝对路径不能说「相对于 X」—— 见 pkg_path_is_absolute 的说明。
        if (pkg_path_is_absolute(d->path)) {
            diag_note("这是个 path 依赖，路径 `%s` 下没有可用入口。", d->path);
        } else {
            diag_note("这是个 path 依赖，路径 `%s`（相对于 %s）下没有可用入口。",
                      d->path, m->root_dir);
        }
    } else {
        char rroot[1200];
        bool has_reg = reg_root(rroot, sizeof(rroot));
        diag_note("这是个注册中心依赖（版本要求 %s）。",
                  d->version ? d->version : "*");
        if (has_reg) {
            diag_note("注册表位置：%s", rroot);
            diag_note("那里没有能满足这个要求的 `%s`。", name);
            diag_help("看看有哪些版本：`x pkg list`；放一个进去："
                      "%s/%s/<版本>/src/lib.xle。", rroot, name);
            diag_help("或者改成 path 依赖：`x pkg add %s --path ../%s`。",
                      name, name);
        } else {
            diag_note("注册表目录 %s 不存在（可用 X_REGISTRY 指定）。", rroot);
            diag_help("用 `x pkg install` 之前先把包放进 %s/%s/1.0.0/。",
                      rroot, name);
            diag_help("或者改成 path 依赖：`x pkg add %s --path ../%s`。",
                      name, name);
        }
        diag_note("也会退回项目内的 vendor/%s/ 找源码（旧布局）。", name);
    }
    diag_note("试过这些路径：");
    for (int i = 0; i < ncand; i++) {
        diag_note("  %s", cand[i]);
    }
    diag_help("path 依赖的目录里需要有 src/lib.xle（库入口），"
              "或者 src/%s.xle。", name);
    return false;
}

// ============================================================
// 依赖树
// ============================================================

void pkg_print_tree(const PkgManifest* m, FILE* out) {
    if (!m) return;
    fprintf(out, "%s %s\n", m->name ? m->name : "?",
            m->version ? m->version : "?");

    if (m->dep_count == 0) {
        fprintf(out, "└── （没有依赖）\n");
        return;
    }

    for (int i = 0; i < m->dep_count; i++) {
        const PkgDep* d = &m->deps[i];
        bool last = (i == m->dep_count - 1);
        const char* branch = last ? "└── " : "├── ";

        // 显示 @key（源码里的写法），不是包名 ——
        // 这样输出能和代码里的 `import @xxx` 直接对照，
        // 也可以直接 grep。
        fprintf(out, "%s@%s", branch, d->key);

        if (d->kind == DEP_PATH) {
            fprintf(out, "  (path: %s)", d->path);
        } else {
            fprintf(out, " %s", d->version ? d->version : "");
            if (d->package) {
                fprintf(out, "  (package: %s)", d->package);
            }
        }
        fprintf(out, "\n");
    }
}