// XColorSettingsPage.java — 「设置 → 编辑器 → 配色方案 → X 语言」页面。
//
// 没有这一页，TextAttributesKey 仍然生效（走默认锚点颜色），但用户
// 改不了。一小段实现换来可配置性，值。

package com.xlang.idea;

import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.options.colors.AttributesDescriptor;
import com.intellij.openapi.options.colors.ColorDescriptor;
import com.intellij.openapi.options.colors.ColorSettingsPage;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.Icon;
import java.util.HashMap;
import java.util.Map;

public final class XColorSettingsPage implements ColorSettingsPage {

    private static final AttributesDescriptor[] DESCRIPTORS = {
        new AttributesDescriptor("关键字", XSyntaxHighlighter.X_KEYWORD_KEY),
        new AttributesDescriptor("内建类型", XSyntaxHighlighter.X_TYPE_KEY),
        new AttributesDescriptor("注解", XSyntaxHighlighter.X_ANNOTATION_KEY),
        new AttributesDescriptor("字符串", XSyntaxHighlighter.X_STRING_KEY),
        new AttributesDescriptor("字符", XSyntaxHighlighter.X_CHAR_KEY),
        new AttributesDescriptor("数字", XSyntaxHighlighter.X_NUMBER_KEY),
        new AttributesDescriptor("注释//行注释", XSyntaxHighlighter.X_LINE_COMMENT_KEY),
        new AttributesDescriptor("注释//文档注释", XSyntaxHighlighter.X_DOC_COMMENT_KEY),
        new AttributesDescriptor("注释//块注释", XSyntaxHighlighter.X_BLOCK_COMMENT_KEY),
        new AttributesDescriptor("运算符", XSyntaxHighlighter.X_OPERATOR_KEY),
    };

    // 演示文本里的标签 → 颜色项。标签必须与 getDemonstrationText 一致。
    private static final Map<String, TextAttributesKey> TAGS = new HashMap<>();

    static {
        TAGS.put("kw", XSyntaxHighlighter.X_KEYWORD_KEY);
        TAGS.put("ty", XSyntaxHighlighter.X_TYPE_KEY);
        TAGS.put("ann", XSyntaxHighlighter.X_ANNOTATION_KEY);
        TAGS.put("str", XSyntaxHighlighter.X_STRING_KEY);
        TAGS.put("chr", XSyntaxHighlighter.X_CHAR_KEY);
        TAGS.put("num", XSyntaxHighlighter.X_NUMBER_KEY);
        TAGS.put("com", XSyntaxHighlighter.X_LINE_COMMENT_KEY);
        TAGS.put("doc", XSyntaxHighlighter.X_DOC_COMMENT_KEY);
        TAGS.put("blk", XSyntaxHighlighter.X_BLOCK_COMMENT_KEY);
        TAGS.put("op", XSyntaxHighlighter.X_OPERATOR_KEY);
    }

    @Override
    public @Nullable Icon getIcon() {
        return XIcons.FILE;
    }

    // 设置页预览区需要高亮器实例；与编辑器用的是同一个映射。
    @Override
    public @NotNull com.intellij.openapi.fileTypes.SyntaxHighlighter getHighlighter() {
        return new XSyntaxHighlighter();
    }

    @Override
    public @NotNull String getDisplayName() {
        return "X 语言";
    }

    @Override
    public @NotNull AttributesDescriptor @NotNull [] getAttributeDescriptors() {
        return DESCRIPTORS;
    }

    @Override
    public @NotNull ColorDescriptor @NotNull [] getColorDescriptors() {
        return ColorDescriptor.EMPTY_ARRAY;
    }

    @Override
    public @Nullable Map<String, TextAttributesKey> getAdditionalHighlightingTagToDescriptorMap() {
        return TAGS;
    }

    @Override
    public @NotNull String getDemoText() {
        return """
            /// 文档注释：打招呼函数
            //! 模块级文档
            // 普通行注释
            /* 块注释可以 /* 嵌套一层 */ 继续写 */

            @Deprecated
            @Table("users")
            <kw>class</kw> User <op>{</op>
                name: <ty>String</ty>,
                <kw>fn</kw> speak() -> <ty>String</ty> <op>{</op> <kw>return</kw> <str>"你好"</str> <op>}</op>
            <op>}</op>

            <kw>fn</kw> main() <op>{</op>
                <kw>let</kw> n: <ty>Int</ty> = <num>0xFF</num>;
                <kw>let</kw> c = <chr>'好'</chr>;
                <kw>let</kw> f = <num>1.5e3</num>;
                <kw>if</kw> n <op>></op> <num>0</num> <op>{</op> println(<str>"ok"</str>) <op>}</op> <kw>else</kw> <op>{</op> <kw>return</kw> <op>}</op>
            <op>}</op>
            """;
    }
}
