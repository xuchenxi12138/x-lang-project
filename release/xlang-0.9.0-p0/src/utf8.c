// utf8.c — UTF-8 与终端编码边界
//
// 为什么单独一个文件：X 的**语言内部**全是 UTF-8，无需任何 locale 支持
// ——词法器按字节 >= 0x80 认标识符字符，中文标识符天然合法（铁律 6）。
// 真正需要操心 UTF-8 的只有「与外部环境的交界处」，全部集中在这里：
//
//   1. 输出到 Windows 控制台（默认 GBK 代码页 → 合法 UTF-8 被当成乱码）
//   2. 从 argv 读路径（Windows 的 argv 是 ANSI 代码页，中文路径会坏）
//   3. 读文件时剥 BOM（01 §2.1 的承诺）
//   4. 列号按字符而非字节计（否则中文前缀后所有位置都错）
//   5. 诊断里按显示宽度对齐/截断源码片段
//
// 1、2、3、4 是正确性问题，5 是排版问题。正确性问题全部在 P0 修掉。

#include "x.h"

// ============================================================
// 终端初始化 —— 实现已移到 platform.c（平台边界集中处）
// ============================================================
//
// 这里只保留一层薄转发，让原有调用点（main.c 的 utf8_init_console）
// 不用全部改名。真正的 Windows 代码页处理在 platform.c。

void utf8_init_console(void) {
    plat_init_console();
}

// ============================================================
// 编码转换 —— 实现已移到 platform.c
// ============================================================
//
// 这两件事本质上是「与操作系统交界」，属于平台边界而不是 UTF-8 逻辑。
// 它们留在 utf8.c 会让人误以为编码转换是 UTF-8 的事，其实相反：
// 正是因为系统不按 UTF-8 来，才需要转换。

char* utf8_from_locale(const char* s) {
    return plat_from_locale(s);
}

char* utf8_to_locale(const char* s) {
    return plat_to_locale(s);
}

// ============================================================
// BOM
// ============================================================

const char* utf8_strip_bom(const char* s, size_t* len) {
    if (s && len && *len >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        *len -= 3;
        return s + 3;
    }
    return s;
}

// ============================================================
// 解码
// ============================================================

int utf8_decode(const char* s, size_t len, int* adv) {
    if (adv) *adv = 1;
    if (!s || len == 0) return -1;

    unsigned char c0 = (unsigned char)s[0];

    // 单字节
    if (c0 < 0x80) return (int)c0;

    // 多字节：先看首字节定长度，再逐字节校验续字节前缀 10xxxxxx。
    // 严格拒绝超长编码（overlong）与代理区（U+D800..DFFF）：
    // 这两类不是合法 UTF-8，放过去会让「两个不同的字节串解码出同一个
    // 字符串」，而标识符比较是按码点的，那会造成符号表串味。
    int need;
    int cp;
    int min;
    if ((c0 & 0xE0) == 0xC0)      { need = 2; cp = c0 & 0x1F; min = 0x80; }
    else if ((c0 & 0xF0) == 0xE0) { need = 3; cp = c0 & 0x0F; min = 0x800; }
    else if ((c0 & 0xF8) == 0xF0) { need = 4; cp = c0 & 0x07; min = 0x10000; }
    else return -1;   // 0x80..0xBF 开头（孤立续字节）或 0xF8+（非法）

    if (len < (size_t)need) return -1;

    for (int i = 1; i < need; i++) {
        unsigned char ci = (unsigned char)s[i];
        if ((ci & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (ci & 0x3F);
    }

    if (cp < min) return -1;                          // 超长编码
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;      // 代理区
    if (cp > 0x10FFFF) return -1;                     // 超出 Unicode

    if (adv) *adv = need;
    return cp;
}

// 码点个数。按首字节数（非 10xxxxxx 的都是首字节）。
//
// 这个函数与 len(String) 直接相关：字符串的 len 必须给码点数而不是
// 字节数，否则它与 strAt / strSub / strFind 的码点索引是两套坐标系。
// 后果是「for i in 0..len(s) { strAt(s,i) }」在遇到中文时越界取空串，
// 而 ASCII 输入完全正常 —— 测试里看不出来，用户一用中文就错。
//
// 不逐码点校验合法性（那要用 utf8_decode 走一遍），只数首字节：
// 非法字节串也应该有确定的长度，而不是在算长度时报告错误。合法性
// 由需要解码的地方（strAt / strCode 等）各自判断。
size_t utf8_cplen(const char* s, size_t len) {
    if (!s) return 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    }
    return n;
}

// ============================================================
// 显示宽度（04 §6.1）
// ============================================================

int utf8_width(int cp) {
    if (cp < 0) return 1;          // 非法字节按 1 列占位，保证不出现负宽度

    // 组合符号（Cambria 等）：0 列。
    // P0 只覆盖最常见的区间，完整表在 P1 用 Unicode 数据库生成。
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) return 0;

    if (cp == 0x200B || cp == 0xFEFF) return 0;   // 零宽空格 / BOM

    // 控制字符：0 列（不会出现在标识符里，但可能出现在诊断的源码片段中）
    if (cp < 0x20 || cp == 0x7F) return 0;

    // 中日韩宽字符。这是 04 §6.1 要求的核心区间：
    // 决定中文标识符在 fmt 对齐时算 2 列还是 1 列。
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // 谚文字母
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK 部首、标点
        (cp >= 0x3041 && cp <= 0x33FF) ||   // 平假名/片假名/注音/CJK 兼容
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK 扩展 A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK 统一表意文字（主区）
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // 彝文
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   // 谚文音节
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK 兼容表意文字
        (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK 兼容形式
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // 全角形式
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // 全角符号
        (cp >= 0x20000 && cp <= 0x3FFFD)) { // CJK 扩展 B 及以后
        return 2;
    }
    return 1;
}

// ============================================================
// 全缓冲区校验
// ============================================================

bool utf8_validate(const char* s, size_t len,
                   size_t* bad_off, int* bad_line, int* bad_col) {
    if (!s) return true;

    size_t i = 0;
    int line = 1;
    int col  = 1;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];

        // 快速路径：ASCII 占绝大多数，逐字节走不进去解码函数
        if (c < 0x80) {
            if (c == '\n') { line++; col = 1; }
            else if (c != '\r') { col++; }
            i++;
            continue;
        }

        int adv = 1;
        int cp = utf8_decode(s + i, len - i, &adv);
        if (cp < 0) {
            if (bad_off)  *bad_off  = i;
            if (bad_line) *bad_line = line;
            if (bad_col)  *bad_col  = col;
            return false;
        }
        col++;      // 一个码点算 1 列（与词法器的列号口径一致）
        i += (size_t)adv;
    }
    return true;
}

// ============================================================
// 按显示宽度截断
// ============================================================

size_t utf8_clip_cols(const char* s, size_t len, int max_cols) {
    if (!s || max_cols <= 0) return 0;
    size_t i = 0;
    int cols = 0;
    while (i < len) {
        int adv = 1;
        int cp = utf8_decode(s + i, len - i, &adv);
        int w = utf8_width(cp);
        if (cols + w > max_cols) break;
        cols += w;
        i += (size_t)adv;
    }
    return i;
}