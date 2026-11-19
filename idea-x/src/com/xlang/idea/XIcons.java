// XIcons.java — 插件图标。
//
// IDEA 平台原生支持 SVG 图标（16x16，按 IDE 主题自动缩放），所以这里
// 直接用 svg 而不是 png —— 一份资源在浅色/深色主题下都清晰。

package com.xlang.idea;

import com.intellij.openapi.util.IconLoader;

import javax.swing.Icon;

public final class XIcons {
    public static final Icon FILE = IconLoader.getIcon("/icons/x.svg", XIcons.class);

    private XIcons() { }
}
