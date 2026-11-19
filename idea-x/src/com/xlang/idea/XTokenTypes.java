// XTokenTypes.java — 词法单元类型。
//
// ── 为什么只有十几个 token 而没有语法树节点 ────────────────
//
// 完整的解析树要 Grammar-Kit 或手写 PSI 结构，构建链会重一大截。
// 而 VSCode 插件的经验（check-lsp 101 项）已经证明：高亮 / 补全 /
// 跳转定义这三件事，用「词法单元 + 文本扫描」就够到同等体验 ——
// LSP 服务端自己也是词法级扫描符号的（见 src/lsp.c 的取舍说明）。
//
// IDEA 这边沿用同一思路：平坦 token 流，三类功能共用一次词法分析。
// 代价（不区分作用域、无类型推断）与 LSP 完全一致，不新增短板。

package com.xlang.idea;

import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;

public interface XTokenTypes {
    IElementType X_IDENT       = new IElementType("X_IDENT", XLanguage.INSTANCE);
    IElementType X_KEYWORD     = new IElementType("X_KEYWORD", XLanguage.INSTANCE);
    IElementType X_TYPE        = new IElementType("X_TYPE", XLanguage.INSTANCE);
    IElementType X_ANNOTATION  = new IElementType("X_ANNOTATION", XLanguage.INSTANCE);
    IElementType X_STRING      = new IElementType("X_STRING", XLanguage.INSTANCE);
    IElementType X_CHAR        = new IElementType("X_CHAR", XLanguage.INSTANCE);
    IElementType X_NUMBER      = new IElementType("X_NUMBER", XLanguage.INSTANCE);
    IElementType X_LINE_COMMENT  = new IElementType("X_LINE_COMMENT", XLanguage.INSTANCE);
    IElementType X_DOC_COMMENT   = new IElementType("X_DOC_COMMENT", XLanguage.INSTANCE);
    IElementType X_BLOCK_COMMENT = new IElementType("X_BLOCK_COMMENT", XLanguage.INSTANCE);
    IElementType X_OPERATOR    = new IElementType("X_OPERATOR", XLanguage.INSTANCE);
    IElementType X_PUNCT       = new IElementType("X_PUNCT", XLanguage.INSTANCE);

    // 空白用平台的通用类型，编辑器才能正确处理格式化与折叠。
    TokenSet WHITE_SPACES = TokenSet.create(com.intellij.psi.TokenType.WHITE_SPACE);
    TokenSet COMMENTS     = TokenSet.create(X_LINE_COMMENT, X_DOC_COMMENT, X_BLOCK_COMMENT);
    TokenSet STRINGS      = TokenSet.create(X_STRING, X_CHAR);

    // ── 注意：这里不建 KEYWORDS TokenSet ────────────────────
    //
    // 高亮映射在 XSyntaxHighlighter 里按 token 类型查表，而补全/跳转
    // 需要的是「具体词文本」。token 类型上 X_KEYWORD 已经够用，
    // 词表判断走 XCompletionData / XSymbols 的字符串集合。
}
