// profile.c — 档位表与查询
//
// 设计理由见 profile.h。这个文件只做一件事：把「场景」变成一组
// 编译期常量。没有运行期逻辑，没有配置合并 —— 档位是**选一个**，
// 不是叠加。

#include "profile.h"

#include <string.h>

// ============================================================
// 档位表
// ============================================================
//
// 三档的差别必须是真的差别，不能只是「名字不同、行为一样」。
// 每一行下面都写了它凭什么与其他两行不同；写不出来的档位就不该存在。
//
//   console vs desktop
//     want_gui        false/true      —— 决定内嵌不内嵌窗口运行时
//     gui_subsystem   false/true      —— 决定 PE 头里那个字段
//     call_depth      4096 / 8192     —— 决定栈开多大
//     体积差          实测 47KB vs 503KB（hello vs 扫雷）
//
//   console vs service
//     stdout_buf      LINE / NONE     —— 进程被杀时最后半行留不留
//     其余完全一致
//
// ── service 只剩一条差异，这是如实记录 ──────────────────────
//
// 这里原本还给 service 写了 allow_bin_depend=false（「服务端进程不该
// fork 外部程序」），并在表注释里声明为「契约，等 std.process 进来
// 后生效」。问题是没有任何代码读它 —— 那个字段在当时是纯粹的文档，
// 伪装成了机器可检查的约束。
//
// 现在删掉了。service 与 console 的真实差异就是 stdout 无缓冲，
// 这个差异可实测（见 tools/run_tests.sh 的 profile 段第 3 项），
// 值得进表。「服务不能起子进程」这条等真有编码能力时再进来。

static const XProfileDef g_profiles[PROF_COUNT] = {
    {
        "console", "控制台", "命令行工具、脚本、测试",
        /* want_gui        */ false,
        /* gui_subsystem   */ false,
        /* call_depth      */ 4096,
        /* stdout_buf      */ XBUF_LINE,
    },
    {
        "desktop", "桌面", "图形界面程序（即时模式窗口）",
        /* want_gui        */ true,
        /* gui_subsystem   */ true,
        /* call_depth      */ 8192,
        /* stdout_buf      */ XBUF_LINE,
    },
    {
        "service", "服务", "长驻后台服务（输出无缓冲）",
        /* want_gui        */ false,
        /* gui_subsystem   */ false,
        /* call_depth      */ 4096,
        /* stdout_buf      */ XBUF_NONE,
    },
};

const XProfileDef* profile_get(XProfile p) {
    if (p < 0 || p >= PROF_COUNT) return NULL;
    return &g_profiles[p];
}

const XProfileDef* profile_find(const char* name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < PROF_COUNT; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) return &g_profiles[i];
    }
    return NULL;
}

bool profile_name_ok(const char* name) {
    return profile_find(name) != NULL;
}

const char* profile_default_name(void) {
    // 默认是 console 而不是 desktop：绝大多数程序是命令行工具
    // （测试、构建脚本、数据处理），而且 console 是**没有额外假设**
    // 的那一档 —— 它不需要图形环境，在任何机器上都能跑。
    //
    // 让默认档位是「最不挑环境」的那个，符合「默认值要能跑」这条。
    return g_profiles[PROF_CONSOLE].name;
}

void profile_print_table(FILE* out) {
    fprintf(out, "使用场景（Profile）—— 一个场景一条固定链路，运行期无派发\n\n");

    // ── 为什么这里不用 %-10s 排版 ───────────────────────────
    //
    // printf 的宽度按**字节**算，而中日韩字符在 UTF-8 里占 3 字节、
    // 显示时占 2 列。用 "%-10s" 排中英混排的表头，得到的是一张
    // 斜着的表 —— 而且斜多少取决于每行的中文字数，逐行都不一样。
    //
    // 所以这张表的每一列都由**固定字面量**拼成：列宽按显示列手算，
    // 每个值域都预先确认过宽度相同（三档名字都是 7 字符的 ASCII，
    // 「内嵌/不链」都是 2 汉字，「CONSOLE/WINDOWS」都是 7 字符，
    // 「行/无」都是 1 汉字），于是不需要任何运行时补空格，
    // 对齐是结构性的而非算出来的。
    fprintf(out, "   名字      窗口运行时  子系统     栈深    输出缓冲  用途\n");
    fprintf(out, "  ----------------------------------------------------------------------\n");

    for (int i = 0; i < PROF_COUNT; i++) {
        const XProfileDef* p = &g_profiles[i];
        bool is_default = (i == PROF_CONSOLE);
        // 窗口运行时列：内嵌 / 不链（都是 2 汉字 = 4 显示列，补 8 空格）
        // 子系统列：CONSOLE / WINDOWS（都是 7 字符，无需补）
        // 输出缓冲列：行 / 无（都是 1 汉字 = 2 显示列，补 8 空格）
        fprintf(out, "  %s %-7s  %s        %-7s   %-4d    %s         %s\n",
                is_default ? "*" : " ",
                p->name,
                p->want_gui ? "内嵌" : "不链",
                p->gui_subsystem ? "WINDOWS" : "CONSOLE",
                p->call_depth,
                p->stdout_buf == XBUF_NONE ? "无" : "行",
                p->purpose);
    }

    fprintf(out, "\n");
    fprintf(out, "  * = 默认档位\n");
    fprintf(out, "\n");
    fprintf(out, "  窗口运行时：内嵌 = 把 src/gui/gui_win32.c 编进产物并链 gdi32/user32\n");
    fprintf(out, "              不链 = 产物里根本不存在窗口相关符号与代码\n");
    fprintf(out, "  子系统：    WINDOWS = PE 头 subsystem=2，双击运行不弹控制台黑框\n");
    fprintf(out, "              CONSOLE = subsystem=3，可被管道与重定向接走输出\n");
    fprintf(out, "  输出缓冲：  行 = 见到 \\n 才写出去，进程被杀时最后半行会丢\n");
    fprintf(out, "              无 = 每次都直接写出去，进程被杀时已输出的都留着\n");
    fprintf(out, "\n");
    fprintf(out, "  前四项都是**编译期**决定的：子系统写在文件头里，由系统加载器\n");
    fprintf(out, "  读取，运行期没有任何办法修改。这就是「场景必须在执行之前定死」\n");
    fprintf(out, "  的物理来源，不是一条设计偏好。输出缓冲是唯一在运行期生效的一项，\n");
    fprintf(out, "  因为它表达的是「进程死亡时能看到多少」，没有编译期等价物。\n");
    fprintf(out, "\n");
    fprintf(out, "  也可以写在 x.toml 里，让 clone 下来的人直接 build 就对：\n");
    fprintf(out, "      [package]\n");
    fprintf(out, "      profile = \"desktop\"\n");
    fprintf(out, "  优先级是 命令行 --profile > x.toml > 默认 console。\n");
    fprintf(out, "\n");
    fprintf(out, "  用法：x build app.xle --emit=exe --profile=desktop\n");
    fprintf(out, "        x run app.xle --profile=desktop      （解释档，同样生效）\n");
}