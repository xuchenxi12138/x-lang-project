// XLexer.java — X 语言的词法器（手写，非 Flex 生成）。
//
// ── 为什么手写而不上 Grammar-Kit/Flex ───────────────────────
//
// Flex 生成要额外的构建工具（JFlex 插件）。本词法器的规则面很小：
// 关键字查表、字符串/字符/数字/注释/注解，全部能在一页里写完 ——
// 手写比引入生成链更短。规则必须与 src/lexer.c 保持镜像，见
// check-plugin.js 里对关键字数量的断言。
//
// ── 与 lexer.c 对齐的三个要点 ──────────────────────────────
//
//   1. 英文 45 关键字 + 中文 45 别名。中文模式与否不影响识别：
//      两套都认。`如果` 在 .xle 里是编译不过的词，但把它标成关键字
//      并无误导 —— 与 LSP 的取舍一致（少给比乱给好的反面：词表
//      对中文文件全开，否则 .xlc 提示会全空）。
//   2. 嵌套块注释。lexer.c 的块注释按计数器实现可嵌套，这里同样
//      用深度计数（存进 state，跨 incremental re-lex 正确恢复）。
//   3. 标识符允许中文。用 Character.isJavaIdentifier* 判定，它们
//      对 CJK 汉字返回 true、对全角标点（（），。）返回 false ——
//      这正是 lexer.c 的行为边界。
//
// ── state 编码 ─────────────────────────────────────────────
//
// state = 0        正常
// state > 0        在块注释里，值为当前嵌套深度
//
// getState() 返回「当前 token 开始前」的状态：IDEA 增量重排（局部
// 重刷）时会从某个 token 起点带着 state 重启，token 内部状态必须
// 编码进去，否则嵌套注释的深度会在编辑后丢掉。

package com.xlang.idea;

import com.intellij.lexer.Lexer;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;

import java.util.HashMap;
import java.util.Map;

public final class XLexer extends Lexer {
    private CharSequence buf;
    private int endPos;
    private int pos;
    private int stateBefore;   // 当前 token 开始前的状态
    private int stateAfter;    // 当前 token 结束后的状态
    private IElementType tokType;
    private int tokStart;
    private int tokEnd;

    // 英文关键字表（镜像 src/lexer.c kKeywords[]）
    private static final String[] EN_KEYWORDS = {
        "as", "break", "case", "catch", "comptime", "const", "continue", "defer", "do", "dyn",
        "else", "enum", "extern", "false", "fn", "for", "from", "if", "impl", "import", "in",
        "let", "loop", "match", "mod", "mut", "not", "null", "or", "par", "pub", "public",
        "ref", "return", "self", "spawn", "struct", "trait", "true", "type", "unsafe", "var",
        "void", "where", "while", "yield",
        // 类系统（v0.15）
        "class", "extends", "implements", "abstract", "interface", "override",
        "private", "protected", "new", "super", "final",
    };

    // 中文关键字别名（镜像 kZhAliases[]）
    private static final String[] ZH_KEYWORDS = {
        "函", "令", "变", "常", "结构", "枚举", "特征", "实现", "模组", "引入",
        "从", "公开", "自身", "真", "假", "空", "可变", "引用", "类型", "作为",
        "非", "编译期", "外部", "做", "捕获", "如果", "否则", "当", "遍历", "于",
        "循环", "跳出", "继续", "匹配", "情形", "返回", "产出", "延后", "派生",
        "并行", "动态", "约束", "或", "不安全", "无",
        // 类系统（v0.15）
        "类", "继承", "实现接口", "抽象", "接口", "覆写", "私有", "保护",
        "新建", "父类", "终态",
    };

    // 内建类型名。中文别名按 lexer.c 的处理标成类型而不是关键字：
    // 类型名走标识符通道（文本归一化），`无` 例外 —— 它同时是 void
    // 关键字，关键字优先，所以不在下表里。
    private static final String[] TYPE_NAMES = {
        "Int", "Float", "Bool", "String", "Char", "Complex", "Array", "Void",
        "整数", "浮点", "布尔", "字符串", "字符", "复数", "数组",
    };

    private static final Map<String, Boolean> KEYWORD_SET = new HashMap<>();
    private static final Map<String, Boolean> TYPE_SET = new HashMap<>();

    static {
        for (String k : EN_KEYWORDS) KEYWORD_SET.put(k, Boolean.TRUE);
        for (String k : ZH_KEYWORDS) KEYWORD_SET.put(k, Boolean.TRUE);
        for (String t : TYPE_NAMES) TYPE_SET.put(t, Boolean.TRUE);
    }

    public XLexer() { }

    @Override
    public void start(CharSequence buffer, int startOffset, int endOffset, int initialState) {
        this.buf = buffer;
        this.endPos = endOffset;
        this.pos = startOffset;
        this.stateBefore = initialState;
        this.stateAfter = initialState;
        this.tokType = null;
        this.tokStart = startOffset;
        this.tokEnd = startOffset;
        advance();
    }

    @Override
    public int getState() {
        return stateBefore;
    }

    @Override
    public IElementType getTokenType() {
        return tokType;
    }

    @Override
    public CharSequence getBufferSequence() {
        return buf;
    }

    @Override
    public int getBufferEnd() {
        return endPos;
    }

    @Override
    public int getTokenStart() {
        return tokStart;
    }

    @Override
    public int getTokenEnd() {
        return tokEnd;
    }

    @Override
    public void advance() {
        if (tokEnd >= endPos) {
            tokType = null;
            tokStart = tokEnd = endPos;
            stateBefore = stateAfter;
            return;
        }
        pos = tokEnd;
        tokStart = pos;
        stateBefore = stateAfter;
        scan();
        pos = tokEnd;
    }

    // 增量重排后从指定位置恢复。offset 处的状态决定接下来 scan()
    // 的入口（state>0 表示在块注释内部），与 start() 语义一致。
    @Override
    public void restore(com.intellij.lexer.LexerPosition position) {
        int offset = position.getOffset();
        pos = offset;
        tokEnd = offset;
        tokStart = offset;
        tokType = null;
        stateBefore = position.getState();
        stateAfter = stateBefore;
        advance();
    }

    // 平台在增量重排前会取当前位置快照，再在合适时机 restore 回去。
    // 注意给的是「token 结束处 + 之后的状态」——与 restore 的入口对齐。
    @Override
    public com.intellij.lexer.LexerPosition getCurrentPosition() {
        int offset = tokEnd;
        int state = stateAfter;
        return new com.intellij.lexer.LexerPosition() {
            @Override public int getOffset() { return offset; }
            @Override public int getState() { return state; }
        };
    }

    // ------------------------------------------------------------
    // 主扫描：按「注释 → 字符串 → 数字 → 注解 → 标识符 → 其余」的
    // 顺序判定，与 tmLanguage 的规则顺序一致（顺序有语义，见
    // x.tmLanguage.json 顶部注释）。
    // ------------------------------------------------------------
    private void scan() {
        // 增量重启：从块注释内部恢复时，stateBefore 携带深度。
        if (stateBefore > 0) {
            scanBlockComment(stateBefore, false);
            return;
        }

        char c = buf.charAt(pos);

        if (c == '/' && pos + 1 < endPos) {
            char n = buf.charAt(pos + 1);
            if (n == '/') {
                // /// 与 //! 是文档注释。必须先于普通 // 判定，否则
                // 前缀被普通注释吃掉（tmLanguage 同样的顺序坑）。
                boolean doc = pos + 2 < endPos && (buf.charAt(pos + 2) == '/' || buf.charAt(pos + 2) == '!');
                while (pos < endPos && buf.charAt(pos) != '\n' && buf.charAt(pos) != '\r') pos++;
                tokEnd = pos;
                tokType = doc ? XTokenTypes.X_DOC_COMMENT : XTokenTypes.X_LINE_COMMENT;
                stateAfter = 0;
                return;
            }
            if (n == '*') {
                scanBlockComment(1, true);
                return;
            }
        }

        if (c == '"') {
            scanString();
            return;
        }
        if (c == '\'') {
            scanChar();
            return;
        }
        if (c >= '0' && c <= '9') {
            scanNumber();
            return;
        }
        if (c == '@' && pos + 1 < endPos && isIdentPart(buf.charAt(pos + 1))) {
            // 注解 @Name 整体一个 token（@ 与名字在 IDEA 里不需要分色）。
            pos++;
            while (pos < endPos && isIdentPart(buf.charAt(pos))) pos++;
            tokEnd = pos;
            tokType = XTokenTypes.X_ANNOTATION;
            stateAfter = 0;
            return;
        }
        if (isIdentStart(c)) {
            pos++;
            while (pos < endPos && isIdentPart(buf.charAt(pos))) pos++;
            String word = buf.subSequence(tokStart, pos).toString();
            tokEnd = pos;
            // 关键字优先于类型名：`无` 在 kZhTypeAliases 里映射 void，
            // 但它是关键字（lexer.c lex_ident 的取舍，两处必须一致）。
            if (KEYWORD_SET.containsKey(word)) tokType = XTokenTypes.X_KEYWORD;
            else if (TYPE_SET.containsKey(word)) tokType = XTokenTypes.X_TYPE;
            else tokType = XTokenTypes.X_IDENT;
            stateAfter = 0;
            return;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\u000C') {
            while (pos < endPos && " \t\n\r\u000C".indexOf(buf.charAt(pos)) >= 0) pos++;
            tokEnd = pos;
            tokType = TokenType.WHITE_SPACE;
            stateAfter = 0;
            return;
        }
        if (isOperatorChar(c)) {
            pos++;
            tokEnd = pos;
            tokType = XTokenTypes.X_OPERATOR;
            stateAfter = 0;
            return;
        }
        // 其余标点（{}()[];, 等）
        pos++;
        tokEnd = pos;
        tokType = XTokenTypes.X_PUNCT;
        stateAfter = 0;
    }

    private void scanString() {
        pos++; // 开引号
        while (pos < endPos) {
            char c = buf.charAt(pos);
            if (c == '\\') {
                // 转义跳两格：\" 里的引号不能当字符串终点。
                pos += 2;
                continue;
            }
            if (c == '"') {
                pos++;
                break;
            }
            if (c == '\n') break; // 未闭合：到行尾为止，不吞整个文件
            pos++;
        }
        if (pos > endPos) pos = endPos;
        tokEnd = pos;
        tokType = XTokenTypes.X_STRING;
        stateAfter = 0;
    }

    private void scanChar() {
        pos++; // 开引号
        if (pos < endPos && buf.charAt(pos) == '\\') pos += 2;
        else if (pos < endPos) pos++;
        // 只吃一个字符：与 lexer.c 的 TK_CHAR 一致（单字符 + 可选转义）。
        if (pos < endPos && buf.charAt(pos) == '\'') pos++;
        if (pos > endPos) pos = endPos;
        tokEnd = pos;
        tokType = XTokenTypes.X_CHAR;
        stateAfter = 0;
    }

    private void scanNumber() {
        if (buf.charAt(pos) == '0' && pos + 1 < endPos) {
            char b = buf.charAt(pos + 1);
            if (b == 'x' || b == 'X' || b == 'b' || b == 'B' || b == 'o' || b == 'O') {
                pos += 2;
                while (pos < endPos && (isHexOrBaseDigit(buf.charAt(pos)) || buf.charAt(pos) == '_')) pos++;
                tokEnd = pos;
                tokType = XTokenTypes.X_NUMBER;
                stateAfter = 0;
                return;
            }
        }
        while (pos < endPos && ((buf.charAt(pos) >= '0' && buf.charAt(pos) <= '9') || buf.charAt(pos) == '_')) pos++;
        // 浮点：小数点后必须跟数字，避免把区间 0..5 的两个点误吃。
        if (pos < endPos && buf.charAt(pos) == '.' && pos + 1 < endPos
                && buf.charAt(pos + 1) >= '0' && buf.charAt(pos + 1) <= '9') {
            pos++;
            while (pos < endPos && ((buf.charAt(pos) >= '0' && buf.charAt(pos) <= '9') || buf.charAt(pos) == '_')) pos++;
        }
        if (pos < endPos && (buf.charAt(pos) == 'e' || buf.charAt(pos) == 'E')) {
            int save = pos;
            pos++;
            if (pos < endPos && (buf.charAt(pos) == '+' || buf.charAt(pos) == '-')) pos++;
            if (pos < endPos && buf.charAt(pos) >= '0' && buf.charAt(pos) <= '9') {
                while (pos < endPos && ((buf.charAt(pos) >= '0' && buf.charAt(pos) <= '9') || buf.charAt(pos) == '_')) pos++;
            } else {
                pos = save; // e 后面没数字，回退：e 是别的词的一部分
            }
        }
        tokEnd = pos;
        tokType = XTokenTypes.X_NUMBER;
        stateAfter = 0;
    }

    // depth：进入时的嵌套深度。fresh：是否刚匹配到开头的 "/*"。
    private void scanBlockComment(int depth, boolean fresh) {
        int i = pos + (fresh ? 2 : 0);
        while (i < endPos) {
            char c = buf.charAt(i);
            if (c == '*' && i + 1 < endPos && buf.charAt(i + 1) == '/') {
                depth--;
                i += 2;
                if (depth == 0) {
                    tokEnd = i;
                    tokType = XTokenTypes.X_BLOCK_COMMENT;
                    stateAfter = 0;
                    return;
                }
            } else if (c == '/' && i + 1 < endPos && buf.charAt(i + 1) == '*') {
                depth++;
                i += 2;
            } else {
                i++;
            }
        }
        tokEnd = endPos;
        tokType = XTokenTypes.X_BLOCK_COMMENT;
        stateAfter = depth; // 文件截断在注释里：把深度存进 state
    }

    private static boolean isIdentStart(char c) {
        return Character.isJavaIdentifierStart(c);
    }

    private static boolean isIdentPart(char c) {
        return Character.isJavaIdentifierPart(c);
    }

    private static boolean isOperatorChar(char c) {
        switch (c) {
            case '+': case '-': case '*': case '/': case '%': case '=':
            case '<': case '>': case '!': case '&': case '|': case '^':
            case '~': case '?': case '.': case ':':
                return true;
            default:
                return false;
        }
    }

    private static boolean isHexOrBaseDigit(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
}
