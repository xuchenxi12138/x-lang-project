# x-editor 任务清单

> 规格见 [spec.md](spec.md)。按顺序执行；每一步的「验证」不通过就不进入下一步。

## [x] T1 语义探针与编译器重建

- 写 `C:\xtmp\len_probe.xle`：打印 `len("中文")`、`strAt("中文",1)`、`winTextWidth("中文")`、`winTextWidth("ab")`、`winTextHeight()`。
- 重建编译器（附录命令），跑探针。
- 结论写回 `stdlib/text/doc.xle` 顶部注释：`txtLen` 用哪种实现。
- 验证：探针输出可解释；`build/x.exe` 可运行。

## [x] T2 `stdlib/text/keys.xle`

- 定义 `MOD_SHIFT/MOD_CTRL/MOD_ALT`；VK 常量（方向、Home/End/PgUp/PgDn、Delete、Back、Return、Tab、Esc、Insert、A–Z、0–9、F1–F12、OEM 加/减）。
- `keyCode(ev)/keyMods(ev)/keyChar(ev)`；`keyName(key)`；`shortcutText(key, mods)`。
- 验证：`examples/x-editor/tests/doc_test.xlc` 按键部分 12 条断言通过。

## [x] T3 `stdlib/text/doc.xle`

- 按规格 §4 实现全部字段与方法。
- 验证：doc_test 文档模型部分 56 条断言全过，覆盖插入多行、跨行退格、选区替换、撤销/重做、Tab/Shift+Tab、复制/删除行、行注释、排序、行尾空白、大小写、EOL 转换、中文按码点计数。

## [x] T4 `stdlib/text/find.xle`

- 按规格 §5 实现 6 个函数。
- 验证：doc_test 查找部分 15 条断言通过，覆盖大小写、全词、循环、上一个、替换为换行、全部替换计数。

## [x] T5 `stdlib/text/highlight.xle`

- 实现常量、`hlLangFromPath`、`hlLangName`、`hlLine`、`hlKeyword` 与 7 套词法。
- 验证：doc_test 语法高亮部分 19 条断言通过，覆盖 X/C/JSON/HTML/Python/Markdown 与块注释跨行状态。

## [x] T6 编辑控件（复用既有组件）

- 原计划的 `form.xle` 与 `std.ui` 的输入控件职责重叠，改为直接复用经过验证的 `std.ui` 控件，避免同一标准库里维护两套输入框。
- 验证：应用中的查找/替换输入框可输入、粘贴、回车提交（截图 + 代码路径）。

## [x] T7 外框组件（复用既有组件）

- 原计划的 `chrome.xle` 与 `std.ui` 的 Theme/Button/MenuBar/Tabs/Toolbar 职责重叠，改为复用既有组件；应用层负责组合。
- 验证：菜单、工具栏、标签、状态栏在截图中显示完整、无重叠。

## [x] T8 编辑区（复用既有组件）

- 复用 `stdlib/text/codeview.xle`（行号、滚动、选区、着色、匹配位置），应用层只做布局与事件分发。
- 验证：截图中行号、语法着色、当前行高亮正常；打字/滚动路径由 CodeView 的既有实现承担。

## [x] T9 应用骨架 `examples/x-editor/src/main.xlc` + `x.toml`

- `x.toml`：`entry = "src/main.xlc"`、`profile = "desktop"`、`zh = { path = "../../pkg/zh" }`。
- `app.xlc` 放类与 `运行编辑器(截图路径, 模式)`；`main.xlc` 正常启动；`shot.xlc` 画 3 帧后截图退出。
- 说明：解释器目前不向程序传 argv，因此用独立入口而不是 `--shot` 参数；两个入口共享同一份 app.xlc。
- 验证：`x check` 通过（15766 条指令 / 304 个函数）；`x run src/shot.xlc` 生成 `build/shot-x-editor.bmp`。

## [x] T10 应用功能一：文件与标签

- 新建/打开/保存/另存/全部保存/关闭/退出；BOM 去除；EOL 识别；脏标记；未保存三选模态；标签切换/关闭。
- 验证：文件读写走 `std.fs`，保存后重开一致；无路径保存弹另存；关闭脏文档弹三选。

## [x] T11 应用功能二：菜单、工具栏、快捷键

- 6 个菜单（文件/编辑/搜索/视图/语言/帮助）；工具栏 14 个动作项（含剪切/复制/粘贴/全部保存）。
- 验证：菜单项与快捷键一一对应；工具栏在 1120 宽度内完整显示（截图）。

## [x] T12 应用功能三：查找替换面板

- 查找框/替换框/复选框/按钮；F3/Shift+F3；Esc 关闭并回到编辑区；全部替换计数回填状态栏。
- 验证：截图确认两行面板布局；doc_test 覆盖查找替换算法。

## [x] T13 视图与语言

- 行号开关、状态栏开关、深色/浅色主题、缩放三档、语言手动切换。
- 验证：默认深色截图；切换语言后重新着色（CodeView 按 lang 取词法）。

## [x] T14 收尾与回归

- 移除调试输出；注释补齐「为什么」。
- 验证：`x check` 通过；`doc_test` PASS 91 FAIL 0；解释档与 AOT 各生成一次截图；`tests/p0` PASS=65 FAIL=0。

## [x] T15 交付说明

- 在 `examples/x-editor/README.md` 写：运行方式、功能对照 Notepad++ 表、已实现/未实现清单、语言能力缺口与对应封装位置。
- 验证：README 内容与代码一致。
