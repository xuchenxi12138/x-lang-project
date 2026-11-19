// XLanguage.java — X 语言的 IDEA 平台注册入口。
//
// 语言实例在整个平台里必须全局唯一：fileType / parserDefinition /
// highlighterFactory 等所有扩展点都是靠 language id（"X"）串起来的。
// INSTANCE 单例是 IDEA 平台插件的固定写法，不是过度设计。

package com.xlang.idea;

import com.intellij.lang.Language;

public final class XLanguage extends Language {
    public static final XLanguage INSTANCE = new XLanguage("X");

    private XLanguage(String id) {
        super(id);
    }
}
