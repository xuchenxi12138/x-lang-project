// lsp.h — Language Server Protocol 服务端（stdio 上的最小实现）
//
// ── 为什么 LSP 由 x.exe 自己当，不做成 Node 扩展 ──────────────
//
// 代码提示要「知道这个文件里有哪些函数、哪些变量、哪些内建」。
// 那个知识在编译器里。如果另起一个 Node 服务端，就得把词法、
// 作用域规则在 JS 里重写一遍 —— 两份实现，迟早分叉，而分叉的
// 表现是「编辑器提示的和编译器认的不一样」。
//
// 让 x.exe 直接讲 LSP，词法和符号信息来源就是它自己，不存在
// 分叉的可能。附带好处：
//
//   · 用户机器上不需要 Node（VSCode 自带 Node，但 Cursor、
//     VSCodium 的版本各不相同，而扩展宿主崩掉的样子很难查）
//   · 没有 node_modules，扩展包还是几百 KB
//   · 补全词表从 C 源码提取（tools/gen-completions.js），
//     跟着编译器走
//
// ── 为什么是 stdio，不是 TCP ────────────────────────────────
//
// LSP 规范把 stdio 定为默认传输。编辑器拉起子进程、通过管道说话，
// 进程退出就自然断开。TCP 要处理端口冲突、防火墙提示、残留进程 ——
// 对一个「跟着编辑器活」的进程来说，这些全是白得的复杂度。
//
// ── 实现范围 ────────────────────────────────────────────────
//
// 只做「编辑体验」部分，不做诊断（diagnostics）。诊断要跑完整
// 编译，而它有自己的 `x check` 命令 —— 在 LSP 里再实现一份，
// 等于把编译错误报告的两条路径分开维护。
//
//   initialize / initialized / shutdown / exit   必须
//   textDocument/didOpen / didChange / didClose  文档同步（全量）
//   textDocument/completion                      补全
//   textDocument/hover                           悬停
//   textDocument/signatureHelp                   参数提示
//   textDocument/documentSymbol                  大纲
//   completionItem/resolve                       补全项补文档
//
// 「全量同步」而不是增量：增量同步要处理 range 的 UTF-16 偏移与
// 编辑重叠，出错的症状是「编辑几次之后提示就错位了」—— 极难查。
// 全量同步在那个规模上（单文件几百 KB）开销可以忽略。

#ifndef X_LSP_H
#define X_LSP_H

// 在 stdio 上跑 LSP 服务端，阻塞直到客户端发 exit。
// 返回进程退出码（0 正常）。
int lsp_run(void);

#endif // X_LSP_H