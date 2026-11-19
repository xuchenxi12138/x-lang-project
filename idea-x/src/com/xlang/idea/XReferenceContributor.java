// XReferenceContributor.java — ctrl+点击 / ctrl+B 跳转与溯源。
//
// ── 行为约定（对齐用户的溯源诉求） ─────────────────────────
//
//   点引用（如 self.vis 里的 vis）  → 直达声明（字段 vis: Bool）
//   点声明（如字段名 vis 本身）     → 弹出导航列表，列出全部引用位置
//   声明有重名（局部变量遮蔽字段）  → 点引用也弹列表，声明排最前
//
// 实现 PsiPolyVariantReference 的原因：IDEA 的 goto-declaration 对
// 多目标引用会自动弹导航列表，单目标则直接跳 —— 正好是上面两条
// 行为约定，不用自己造 UI。
//
// 范围：当前文件。跨文件的 import/包引用解析需要项目级符号表，
// 等 PSI 结构定型后再补 —— 与 LSP 当前实现一致，不画饼。

package com.xlang.idea;

import com.intellij.patterns.PlatformPatterns;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.PsiReference;
import com.intellij.psi.PsiReferenceBase;
import com.intellij.psi.PsiReferenceContributor;
import com.intellij.psi.PsiReferenceProvider;
import com.intellij.psi.PsiReferenceRegistrar;
import com.intellij.psi.PsiElementResolveResult;
import com.intellij.psi.ResolveResult;
import com.intellij.util.ProcessingContext;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.ArrayList;
import java.util.List;

public final class XReferenceContributor extends PsiReferenceContributor {

    @Override
    public void registerReferenceProviders(@NotNull PsiReferenceRegistrar registrar) {
        registrar.registerReferenceProvider(
            PlatformPatterns.psiElement(XTokenTypes.X_IDENT).withLanguage(XLanguage.INSTANCE),
            new PsiReferenceProvider() {
                @Override
                public PsiReference @NotNull [] getReferencesByElement(@NotNull PsiElement element,
                                                                       @NotNull ProcessingContext context) {
                    return new PsiReference[]{ new XDefinitionReference(element) };
                }
            });
    }

    private static final class XDefinitionReference extends PsiReferenceBase<PsiElement>
            implements com.intellij.psi.PsiPolyVariantReference {

        XDefinitionReference(PsiElement element) {
            super(element);
            setRangeInElement(new com.intellij.openapi.util.TextRange(0, element.getTextLength()));
        }

        // --------------------------------------------------------
        // 多目标：声明优先，引用随后。点击处本身从列表里剔除。
        // --------------------------------------------------------
        @Override
        public ResolveResult @NotNull [] multiResolve(boolean incompleteCode) {
            String name = getElement().getText();
            int myOffset = getElement().getTextOffset();
            PsiFile file = getElement().getContainingFile();
            if (file == null) return new ResolveResult[0];

            List<XSymbols.Symbol> all = XSymbols.scan(file.getText());

            boolean clickedIsDecl = false;
            List<XSymbols.Symbol> decls = new ArrayList<>();
            List<XSymbols.Symbol> uses = new ArrayList<>();
            for (XSymbols.Symbol s : all) {
                if (!s.name.equals(name)) continue;
                if (s.offset == myOffset) {
                    clickedIsDecl = s.decl;
                    continue;
                }
                if (s.decl) decls.add(s);
                else uses.add(s);
            }

            // 点在声明上 → 给全部引用；点在引用上 → 给全部声明
            List<XSymbols.Symbol> targets = clickedIsDecl ? uses : decls;

            List<ResolveResult> out = new ArrayList<>();
            for (XSymbols.Symbol s : targets) {
                PsiElement el = file.findElementAt(s.offset);
                if (el != null) out.add(new PsiElementResolveResult(el));
            }
            return out.toArray(new ResolveResult[0]);
        }

        // 非 poly 的调用方（如重命名）拿第一个声明；拿不到就是 null。
        @Override
        public @Nullable PsiElement resolve() {
            ResolveResult[] results = multiResolve(true);
            return results.length == 1 ? results[0].getElement() : null;
        }

        @Override
        public boolean isSoft() {
            // 软引用：解析不到定义时不标红。本实现只看当前文件，
            // 定义在别的文件里是常态，标红会造成大面积误报。
            return true;
        }
    }
}
