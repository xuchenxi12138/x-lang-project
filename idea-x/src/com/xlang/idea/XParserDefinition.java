// XParserDefinition.java — 最小解析定义：平坦 PSI。
//
// 解析器不建任何组合节点，只把 token 逐个放进根文件里。编辑器的
// 高亮、补全、引用、注释、折叠全部在 token 流上工作，不需要真实
// 语法树。这是「与编译器能力对齐」的取舍：语言本身还在演进期，
// 稳定的语法树等 parser 定型后再补。

package com.xlang.idea;

import com.intellij.lang.ASTNode;
import com.intellij.lang.ParserDefinition;
import com.intellij.lang.PsiBuilder;
import com.intellij.lang.PsiParser;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.project.Project;
import com.intellij.psi.FileViewProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.impl.source.tree.LeafPsiElement;
import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;

public final class XParserDefinition implements ParserDefinition {
    private static final IFileElementType FILE = new IFileElementType(XLanguage.INSTANCE);

    @Override
    public @NotNull Lexer createLexer(Project project) {
        return new XLexer();
    }

    @Override
    public @NotNull PsiParser createParser(Project project) {
        // 平坦解析：把所有 token 收进根节点，不建结构。
        //
        // 根 marker 不能省。2022.3 之后的 PsiBuilderImpl 走 light tree
        // 构建路径（MarkerProduction），getTreeBuilt() 会无条件取第 0 个
        // start marker；一个 marker 都不建时抛
        // IndexOutOfBoundsException: Index (0) >= list size (0)，
        // 解析必崩、PSI 建不出来，编辑器每次输入都会回滚 —— 表现为
        // 「敲一个字被下一个字替换后消失」。官方 Simple 语言教程同样
        // 先 mark() 再 done(root)。done() 的根节点由平台拆开展平，
        // 树的形状仍是平坦的 token 流。
        return (rootType, builder) -> {
            PsiBuilder.Marker root = builder.mark();
            while (!builder.eof()) {
                builder.advanceLexer();
            }
            root.done(rootType);
            return builder.getTreeBuilt();
        };
    }

    @Override
    public @NotNull IFileElementType getFileNodeType() {
        return FILE;
    }

    @Override
    public @NotNull PsiFile createFile(@NotNull FileViewProvider viewProvider) {
        return new XFile(viewProvider, XLanguage.INSTANCE);
    }

    // PsiFileBase 在 2024.1 里是抽象类，不能直接 new。包一层唯一的
    // 具体子类，构造语义不变。
    private static final class XFile extends PsiFileBase {
        XFile(@NotNull FileViewProvider viewProvider, @NotNull com.intellij.lang.Language language) {
            super(viewProvider, language);
        }

        @Override
        public @NotNull com.intellij.openapi.fileTypes.FileType getFileType() {
            return XFileType.INSTANCE;
        }
    }

    @Override
    public @NotNull SpaceRequirements spaceExistanceTypeBetweenTokens(ASTNode left, ASTNode right) {
        return SpaceRequirements.MUST_NOT;
    }

    // 注释 token 集合：编辑器的「到行尾/到块尾」智能选择、TODO 扫描、
    // 注释开关都靠它区分注释与代码。
    @Override
    public @NotNull TokenSet getCommentTokens() {
        return XTokenTypes.COMMENTS;
    }

    @Override
    public @NotNull TokenSet getStringLiteralElements() {
        return TokenSet.EMPTY;
    }

    @Override
    public @NotNull PsiElement createElement(@NotNull ASTNode node) {
        // 平坦树里编辑器只会走到叶子节点（组合节点只有根，根走
        // createFile）。这里兜底处理，防止平台对空节点类型另作解释。
        return new LeafPsiElement(node.getElementType(), node.getText());
    }
}
