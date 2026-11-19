// json_read.h — 极简 JSON 读取（只为 LSP 服务）
//
// ── 为什么自己写，不用现成的 ────────────────────────────────
//
// 1. 依赖。x.exe 是单文件分发，加一个 C JSON 库要么 vendor 进来
//    （几千行，风格与本项目不一致），要么要求系统装（破坏可移植性）。
//
// 2. 需求极小。LSP 消息里我们只读几个位置：
//      method / id / params.textDocument.uri / params.text
//      params.position.line / character
//    需要的语义就「按名字取子对象、取字符串、取整数」。完整 JSON
//    解析器要处理的东西（浮点精度、代理对、深度限制、重复键）在
//    这里反而是噪音。
//
// 3. 失败要说清楚。这个解析器的每个失败都带位置，因为 LSP 是
//    「跑起来之后才出问题」的东西 —— 编辑器那边只显示「语言服务器
//    已停止」，没有任何细节。能在 stderr 留一行「第 3 个字节处
//    期望 ':'」，排查成本差一个数量级。
//
// ── 内存模型 ────────────────────────────────────────────────
//
// 一次解析吃掉一整块 arena，对象树全在里面。不要逐节点 free ——
// LSP 每收到一条消息就解析一次、处理完就整块丢弃。逐节点释放
// 在「错误路径提前返回」时必然漏，而这类程序的寿命是「跟着编辑器
// 一直开着」，漏一点就是漏一天。
//
// 字符串不转义解码到独立内存，而是**原地**改写：JSON 的 \u XXXX
// 与 \" 等转义在解析时直接替换成真实字符，指针指向原缓冲。这样
// 零额外分配，代价是解析会修改输入缓冲（调用方不该再用原内容）。

#ifndef X_JSON_READ_H
#define X_JSON_READ_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JV_NULL,
    JV_BOOL,
    JV_NUM,
    JV_STR,
    JV_ARR,
    JV_OBJ,
} JvKind;

typedef struct Jv Jv;

struct Jv {
    JvKind kind;
    // JV_BOOL
    bool b;
    // JV_NUM（只用整数：LSP 的行列、id 都是整数）
    long long i;
    // JV_STR：指向输入缓冲里的 NUL 结尾串（已去转义）
    char* s;
    // JV_ARR / JV_OBJ
    int    n;
    Jv**   items;      // 数组元素 / 对象的值
    char** keys;       // 对象键（数组时为 NULL）
    // 解析位置（行号，仅用于错误信息）
    int    line;
};

// 解析整个缓冲。失败返回 NULL，并把错误写进 err（可为 NULL）。
// 缓冲会被原地改写（转义解码），调用方不应再使用原内容。
Jv* jv_parse(char* buf, size_t len, char* err, size_t err_len);

// 按名字取对象成员。不是对象、或没有这个键，返回 NULL。
Jv* jv_get(const Jv* obj, const char* key);

// 取字符串。不是字符串返回 NULL。
const char* jv_str(const Jv* v);

// 取整数。数字或布尔都接受（布尔给 0/1）。其它返回 def。
long long jv_int(const Jv* v, long long def);

// 取数组长度（不是数组返回 0）
int jv_len(const Jv* v);

// 取数组第 i 项（越界返回 NULL）
Jv* jv_at(const Jv* v, int i);

#endif // X_JSON_READ_H