// XFileType.java — 文件类型识别：.xle（英文）与 .xlc（中文）。
//
// extensions 列表本身注册在 plugin.xml 的 fileType 扩展点里，
// 这里只负责「识别之后是什么类型」。

package com.xlang.idea;

import com.intellij.openapi.fileTypes.LanguageFileType;
import org.jetbrains.annotations.NotNull;

import javax.swing.Icon;

public final class XFileType extends LanguageFileType {
    public static final XFileType INSTANCE = new XFileType();

    private XFileType() {
        super(XLanguage.INSTANCE);
    }

    @Override
    public @NotNull String getName() {
        return "X source";
    }

    @Override
    public @NotNull String getDescription() {
        return "X 语言源码（.xle / .xlc）";
    }

    @Override
    public @NotNull String getDefaultExtension() {
        return "xle";
    }

    @Override
    public Icon getIcon() {
        return XIcons.FILE;
    }
}
