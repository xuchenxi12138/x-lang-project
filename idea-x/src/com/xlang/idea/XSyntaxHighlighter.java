// XSyntaxHighlighter.java — token 类型 → 颜色属性映射。
//
// 颜色锚点全部挂在 DefaultLanguageHighlighterColors 上而不是写死
// RGB：这样插件跟随用户主题（浅色/深色/自定义）自动适配 —— 与
// VSCode 里 tmLanguage 用语义 scope 的做法等价。
//
// 自定义 TextAttributesKey 的名字（X_KEYWORD 等）会出现在
// XColorSettingsPage 的设置页里，用户可以逐项改色。

package com.xlang.idea;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

import java.util.HashMap;
import java.util.Map;

public final class XSyntaxHighlighter extends SyntaxHighlighterBase {
    private static final TextAttributesKey[] EMPTY = new TextAttributesKey[0];

    public static final TextAttributesKey X_KEYWORD_KEY =
        TextAttributesKey.createTextAttributesKey("X_KEYWORD", DefaultColors.KEYWORD);
    public static final TextAttributesKey X_TYPE_KEY =
        TextAttributesKey.createTextAttributesKey("X_TYPE", DefaultColors.TYPE);
    public static final TextAttributesKey X_ANNOTATION_KEY =
        TextAttributesKey.createTextAttributesKey("X_ANNOTATION", DefaultColors.ANNOTATION);
    public static final TextAttributesKey X_STRING_KEY =
        TextAttributesKey.createTextAttributesKey("X_STRING", DefaultColors.STRING);
    public static final TextAttributesKey X_CHAR_KEY =
        TextAttributesKey.createTextAttributesKey("X_CHAR", DefaultColors.STRING);
    public static final TextAttributesKey X_NUMBER_KEY =
        TextAttributesKey.createTextAttributesKey("X_NUMBER", DefaultColors.NUMBER);
    public static final TextAttributesKey X_LINE_COMMENT_KEY =
        TextAttributesKey.createTextAttributesKey("X_LINE_COMMENT", DefaultColors.LINE_COMMENT);
    public static final TextAttributesKey X_DOC_COMMENT_KEY =
        TextAttributesKey.createTextAttributesKey("X_DOC_COMMENT", DefaultColors.DOC_COMMENT);
    public static final TextAttributesKey X_BLOCK_COMMENT_KEY =
        TextAttributesKey.createTextAttributesKey("X_BLOCK_COMMENT", DefaultColors.BLOCK_COMMENT);
    public static final TextAttributesKey X_OPERATOR_KEY =
        TextAttributesKey.createTextAttributesKey("X_OPERATOR", DefaultColors.OPERATION_SIGN);

    // DefaultLanguageHighlighterColors 的 import 名很长，收拢到这里。
    private interface DefaultColors {
        TextAttributesKey KEYWORD = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.KEYWORD;
        TextAttributesKey TYPE = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.CLASS_REFERENCE;
        TextAttributesKey ANNOTATION = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.METADATA;
        TextAttributesKey STRING = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.STRING;
        TextAttributesKey NUMBER = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.NUMBER;
        TextAttributesKey LINE_COMMENT = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.LINE_COMMENT;
        TextAttributesKey DOC_COMMENT = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.DOC_COMMENT;
        TextAttributesKey BLOCK_COMMENT = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.BLOCK_COMMENT;
        TextAttributesKey OPERATION_SIGN = com.intellij.openapi.editor.DefaultLanguageHighlighterColors.OPERATION_SIGN;
    }

    private static final Map<IElementType, TextAttributesKey> KEYS = new HashMap<>();

    static {
        KEYS.put(XTokenTypes.X_KEYWORD, X_KEYWORD_KEY);
        KEYS.put(XTokenTypes.X_TYPE, X_TYPE_KEY);
        KEYS.put(XTokenTypes.X_ANNOTATION, X_ANNOTATION_KEY);
        KEYS.put(XTokenTypes.X_STRING, X_STRING_KEY);
        KEYS.put(XTokenTypes.X_CHAR, X_CHAR_KEY);
        KEYS.put(XTokenTypes.X_NUMBER, X_NUMBER_KEY);
        KEYS.put(XTokenTypes.X_LINE_COMMENT, X_LINE_COMMENT_KEY);
        KEYS.put(XTokenTypes.X_DOC_COMMENT, X_DOC_COMMENT_KEY);
        KEYS.put(XTokenTypes.X_BLOCK_COMMENT, X_BLOCK_COMMENT_KEY);
        KEYS.put(XTokenTypes.X_OPERATOR, X_OPERATOR_KEY);
        // 标识符与标点不映射：默认正文色。函数名/类名的变色需要解析
        // 级语义，平坦 token 流给不了 —— 宁缺毋滥，不给错的颜色。
    }

    @Override
    public @NotNull Lexer getHighlightingLexer() {
        return new XLexer();
    }

    @Override
    public TextAttributesKey @NotNull [] getTokenHighlights(IElementType type) {
        TextAttributesKey key = KEYS.get(type);
        return key == null ? EMPTY : pack(key);
    }
}
