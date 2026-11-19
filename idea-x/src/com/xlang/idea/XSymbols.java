// XSymbols.java — 文档符号扫描（补全候选与跳转定义共用）。
//
// ── 声明的词法级判据（两条规则） ────────────────────────────
//
//   A. 定义关键字后的第一个标识符：fn foo / let x / class User
//   B. 标识符后紧跟 ':'（类型注解）：字段 vis: Bool、参数 a: Int、
//      带类型的 let x: Int
//
// 规则 B 是为「属性溯源」补的：类体里 `vis: Bool,` 这样的字段声明
// 没有任何定义关键字前缀，不扫冒号就永远点不进去 —— 这正是
// self.vis = true 里 vis 溯源失败的根因。括号深度用来区分参数
// （在参数表里）与字段（在类体里），只影响展示文案，不影响跳转。
//
// 用词法单元判断而不是字符串匹配，保证注释和字符串里的同名文本
// 不会混进来。与 src/lsp.c 的 scan_symbols 同源，代价同样是不区分
// 作用域：同名候选一起给，跳转声明优先。

package com.xlang.idea;

import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;

import java.util.ArrayList;
import java.util.List;

public final class XSymbols {

    public static final int KIND_FN = 0;      // 函数
    public static final int KIND_VAR = 1;     // 变量 / 参数
    public static final int KIND_TYPE = 2;    // 类 / 结构 / 接口 / 枚举
    public static final int KIND_FIELD = 3;   // 类字段 / 属性

    public static final class Symbol {
        public final String name;
        public final int kind;
        public final int offset;   // 名字 token 起始偏移
        public final boolean decl; // true 声明处；false 引用处

        Symbol(String name, int kind, int offset, boolean decl) {
            this.name = name;
            this.kind = kind;
            this.offset = offset;
            this.decl = decl;
        }
    }

    // 英文 + 中文两套定义关键字。中文与否不影响扫描 —— 只认英文会让
    // .xlc 文件的补全和跳转完全失效（与 LSP 的取舍一致）。
    private static final String[] DEF_FN   = {"fn", "函"};
    private static final String[] DEF_VAR  = {"let", "令", "var", "变", "const", "常"};
    private static final String[] DEF_TYPE = {"class", "类", "struct", "结构", "interface", "接口", "enum", "枚举"};

    private static final class Tok {
        final IElementType type;
        final String text;
        final int offset;
        Tok(IElementType type, String text, int offset) {
            this.type = type;
            this.text = text;
            this.offset = offset;
        }
    }

    /** 全部符号出现处：声明在前（按出现顺序），引用在后。 */
    public static List<Symbol> scan(CharSequence text) {
        List<Tok> toks = tokenize(text);
        List<Symbol> out = new ArrayList<>();

        int pendingKind = -1; // >=0：刚见过定义关键字，等名字
        int parenDepth = 0;   // 区分参数表（规则 B 在括号内）与类体

        for (int i = 0; i < toks.size(); i++) {
            Tok t = toks.get(i);
            if (t.type == TokenType.WHITE_SPACE || XTokenTypes.COMMENTS.contains(t.type)) {
                continue; // 空白与注释不打断：`fn /* 注 */ foo` 也要能扫到
            }

            if (t.type == XTokenTypes.X_KEYWORD) {
                pendingKind = defKind(t.text);
                continue;
            }

            if (t.type == XTokenTypes.X_IDENT) {
                if (pendingKind >= 0) {
                    // 规则 A：定义关键字后第一个标识符
                    out.add(new Symbol(t.text, pendingKind, t.offset, true));
                } else if (nextIsColon(toks, i)) {
                    // 规则 B：类型注解 → 字段（类体）或参数（参数表）
                    int kind = parenDepth > 0 ? KIND_VAR : KIND_FIELD;
                    out.add(new Symbol(t.text, kind, t.offset, true));
                } else {
                    // 引用处：self.vis 的 vis、调用点的 println
                    out.add(new Symbol(t.text, -1, t.offset, false));
                }
                pendingKind = -1;
                continue;
            }

            if (t.type == XTokenTypes.X_PUNCT) {
                if ("(".equals(t.text)) parenDepth++;
                else if (")".equals(t.text)) parenDepth = Math.max(0, parenDepth - 1);
            }
            pendingKind = -1;
        }
        return out;
    }

    private static boolean nextIsColon(List<Tok> toks, int i) {
        for (int j = i + 1; j < toks.size(); j++) {
            Tok n = toks.get(j);
            if (n.type == TokenType.WHITE_SPACE || XTokenTypes.COMMENTS.contains(n.type)) continue;
            return n.type == XTokenTypes.X_OPERATOR && ":".equals(n.text);
        }
        return false;
    }

    private static List<Tok> tokenize(CharSequence text) {
        List<Tok> out = new ArrayList<>();
        XLexer lx = new XLexer();
        lx.start(text, 0, text.length(), 0);
        while (lx.getTokenType() != null) {
            out.add(new Tok(lx.getTokenType(),
                            text.subSequence(lx.getTokenStart(), lx.getTokenEnd()).toString(),
                            lx.getTokenStart()));
            lx.advance();
        }
        return out;
    }

    private static int defKind(String word) {
        for (String w : DEF_FN) if (w.equals(word)) return KIND_FN;
        for (String w : DEF_VAR) if (w.equals(word)) return KIND_VAR;
        for (String w : DEF_TYPE) if (w.equals(word)) return KIND_TYPE;
        return -1;
    }

    private XSymbols() { }
}
