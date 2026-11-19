// XCommenter.java — ctrl+/ 行注释开关。
//
// VSCode 侧由 language-configuration.json 提供同一行为（// 前缀）。
// 没有这个类，IDEA 里 ctrl+/ 会在行首插入 // 却无法再按一次取消。

package com.xlang.idea;

import com.intellij.lang.Commenter;
import org.jetbrains.annotations.Nullable;

public final class XCommenter implements Commenter {
    @Override
    public @Nullable String getLineCommentPrefix() {
        return "//";
    }

    @Override
    public @Nullable String getBlockCommentPrefix() {
        return "/*";
    }

    @Override
    public @Nullable String getBlockCommentSuffix() {
        return "*/";
    }

    @Override
    public @Nullable String getCommentedBlockCommentPrefix() {
        return null;
    }

    @Override
    public @Nullable String getCommentedBlockCommentSuffix() {
        return null;
    }
}
