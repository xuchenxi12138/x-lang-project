// XCompletionContributor.java — 代码补全。
//
// 词表三部分（关键字 / 内建类型 / 内建函数）来自 XCompletionData ——
// 由 gen-idea-data.js 从编译器源码生成，与 VSCode 提示同源。
// 第四部分是当前文档符号（XSymbols 扫描），对应 LSP 的
// 「本文档符号」来源。
//
// ── 什么时候给，什么时候闭嘴 ───────────────────────────────
//
// 只在标识符位置或空白位置给。字符串、注释、数字里闭嘴：用户
// 在注释里敲「prin」大概率是想写中文说明，弹出 println 是噪音。
// 与 lsp.c 的「少给比乱给好」同一条原则。

package com.xlang.idea;

import com.intellij.codeInsight.completion.CompletionContributor;
import com.intellij.codeInsight.completion.CompletionParameters;
import com.intellij.codeInsight.completion.CompletionResultSet;
import com.intellij.codeInsight.lookup.LookupElement;
import com.intellij.codeInsight.lookup.LookupElementBuilder;
import com.intellij.icons.AllIcons;
import com.intellij.psi.PsiElement;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

import java.util.ArrayList;
import java.util.List;

public final class XCompletionContributor extends CompletionContributor {

    @Override
    public void fillCompletionVariants(@NotNull CompletionParameters parameters,
                                       @NotNull CompletionResultSet result) {
        PsiElement position = parameters.getPosition();
        if (position.getContainingFile() == null) return;
        if (position.getLanguage() != XLanguage.INSTANCE) return;

        IElementType t = position.getNode() == null ? null : position.getNode().getElementType();
        // 只在标识符位置给。空白位置也弹的话，换行按 Enter 会被弹窗
        // 抢先插词 —— 0.3.2 修掉这个「没法写代码」的干扰源。
        if (t != XTokenTypes.X_IDENT) return;

        List<LookupElement> items = new ArrayList<>();
        for (String k : XCompletionData.KEYWORDS_EN) items.add(keyword(k));
        for (String k : XCompletionData.KEYWORDS_ZH) items.add(keyword(k));
        for (String ty : XCompletionData.TYPES_EN) items.add(type(ty));
        for (String ty : XCompletionData.TYPES_ZH) items.add(type(ty));
        for (int i = 0; i < XCompletionData.BUILTIN_NAME.length; i++) items.add(builtin(i));

        // 当前文档符号：函数 / 变量 / 类。名字可能与关键字候选重复
        //（用户把函数命名为 println 之类），平台会按名字去重展示。
        String text = position.getContainingFile().getText();
        for (XSymbols.Symbol s : XSymbols.scan(text)) {
            items.add(docSymbol(s));
        }

        result.addAllElements(items);
    }

    private static LookupElement keyword(String word) {
        return LookupElementBuilder.create(word)
            .withTypeText("关键字")
            .withBoldness(true);
    }

    private static LookupElement type(String name) {
        return LookupElementBuilder.create(name)
            .withIcon(AllIcons.Nodes.Class)
            .withTypeText("内建类型");
    }

    private static LookupElement builtin(int i) {
        String name = XCompletionData.BUILTIN_NAME[i];
        int argc = XCompletionData.BUILTIN_ARGC[i];
        String ret = XCompletionData.BUILTIN_RET[i];
        return LookupElementBuilder.create(name)
            .withIcon(AllIcons.Nodes.Method)
            .withTypeText(ret)
            .withTailText(argsSig(argc));
    }

    private static LookupElement docSymbol(XSymbols.Symbol s) {
        switch (s.kind) {
            case XSymbols.KIND_FN:
                return LookupElementBuilder.create(s.name)
                    .withIcon(AllIcons.Nodes.Method)
                    .withTypeText("文档函数");
            case XSymbols.KIND_TYPE:
                return LookupElementBuilder.create(s.name)
                    .withIcon(AllIcons.Nodes.Class)
                    .withTypeText("文档类型");
            case XSymbols.KIND_FIELD:
                return LookupElementBuilder.create(s.name)
                    .withIcon(AllIcons.Nodes.Field)
                    .withTypeText("文档字段");
            default:
                return LookupElementBuilder.create(s.name)
                    .withIcon(AllIcons.Nodes.Variable)
                    .withTypeText("文档变量");
        }
    }

    // 参数个数签名：0 个 → "()"，可变 → "(…)"，n 个 → "(…,…,…)"。
    // 没有参数名（编译器字节码里只存个数），占位符号已经足够表意。
    private static String argsSig(int argc) {
        if (argc == 0) return "()";
        if (argc < 0) return "(…)";
        StringBuilder sb = new StringBuilder("(");
        for (int i = 0; i < argc; i++) {
            if (i > 0) sb.append(", ");
            sb.append("…");
        }
        return sb.append(')').toString();
    }
}
