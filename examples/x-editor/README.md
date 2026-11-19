# Notepad X —— 用 X 语言中文模块写的 Notepad++ 风格编辑器

界面与操作参照 Notepad++：菜单栏、工具栏、多标签、行号、语法高亮、
查找/替换、状态栏、深色与浅色主题、缩放。

源码全部是中文模块（`.xlc`）：使用 `函 / 令 / 变 / 常 / 类 / 构造 /
新建 / 自身 / 如果 / 否则 / 当 / 返回 / 真 / 假 / 空` 等中文语法，类型使用
`整数 / 布尔 / 字符串 / 数组`。编辑器调用的文档、查找、高亮、按键、控件和
窗口 API 也全部由 `@zh` 提供中文名称。

## 启动入口

正常启动入口是 **`src/main.xlc`**，`x.toml` 的 `entry` 也指向它。
在 `x-lang` 仓库根目录执行以下命令即可打开编辑器：

```powershell
$env:X_STDLIB_PATH = "$PWD\stdlib"
.\build\x.exe run examples\x-editor\src\main.xlc
```

也可以编译正常编辑器，之后直接运行 exe：

```powershell
$env:X_STDLIB_PATH = "$PWD\stdlib"
$env:TMP="C:\xtmp"; $env:TEMP="C:\xtmp"
.\build\x.exe build examples\x-editor\src\main.xlc --emit=exe --profile=desktop `
    --cc=C:\xgcc\mingw64\bin\gcc.exe -o build\x-editor.exe
.\build\x-editor.exe
```

`src/shot.xlc` 只是自动截图入口，绘制 3 帧后会退出，不是正常启动入口。

## 目录

```
examples/x-editor/
  README.md              本说明与启动命令
  x.toml                 清单：入口、场景、zh 中文别名包依赖
  src/main.xlc           正常启动入口
  src/app.xlc            应用本体（编辑器类 + 运行编辑器）
  src/shot.xlc           截图入口（画 3 帧后写 BMP 并退出）
  tests/doc_test.xlc     无窗口回归测试（91 条断言）
  spec/spec.md           设计规格
  spec/tasks.md          任务与验证记录
  spec/checklist.md      验收清单
```

## 验证

在 `x-lang` 仓库根目录执行：

```powershell
$env:X_STDLIB_PATH = "$PWD\stdlib"

# 截图（生成 build/shot-x-editor.bmp）
.\build\x.exe run examples\x-editor\src\shot.xlc

# 回归测试（必须在 console 场景下才有输出）
.\build\x.exe run --profile=console examples\x-editor\tests\doc_test.xlc
```

`x.toml` 里 `profile = "desktop"`：双击 exe 不弹控制台黑框。
测试用 `--profile=console` 覆盖它，否则 `println` 的输出会被 Windows
子系统丢弃（程序本身不报错）。

## 功能对照 Notepad++

| 分类 | 已实现 |
| --- | --- |
| 文件 | 新建、打开、保存、另存为、全部保存、关闭、退出 |
| 多文档 | 标签栏、切换、关闭按钮、未保存标记（●）、未保存三选（保存/不保存/取消） |
| 编辑 | 撤销、重做、剪切、复制、粘贴、全选、复制行、删除行、行注释、缩进/反缩进、转大写/小写、排序行、去行尾空白 |
| 查找 | 查找、查找上一个/下一个（F3 / Shift+F3）、区分大小写、全词、循环、替换、全部替换 |
| 视图 | 行号开关、状态栏开关、放大/缩小/重置缩放、深色/浅色主题 |
| 语言 | 普通文本、X、C 系、JSON、HTML、Python、Markdown、INI，按后缀自动识别，也可手动切换 |
| 状态栏 | 行:列、选区字符数、换行符（CRLF/LF/CR）、编码、语言、插入模式 |
| 快捷键 | Ctrl+N/O/S/Shift+S/W、Ctrl+Z/Y/X/C/V/A/F/H、Ctrl+D/L/Q、Tab/Shift+Tab、Ctrl+=/-/0、F3/Shift+F3、Esc |

## 未实现（明确不做）

- 正则表达式查找替换（只做字面量 + 全词 + 大小写）。
- 列编辑、多光标、语法折叠、自动补全、宏录制。
- 编码转换（只按 UTF-8 读写；BOM 会去掉）、超大文件流式加载。
- 打印、插件系统、会话恢复。

## 语言能力缺口与封装位置

编辑器开发过程中，语言本体没有的能力都封进了标准库或运行时，
应用层不写平台细节：

| 缺口 | 封装位置 |
| --- | --- |
| 虚拟键码、修饰位、快捷键文本 | `stdlib/text/keys.xle` |
| 按行文档模型、码点光标、选区、快照撤销 | `stdlib/text/doc.xle` |
| 查找/替换（大小写、全词、循环、批量） | `stdlib/text/find.xle` |
| 7 种语言的词法着色与跨行状态 | `stdlib/text/highlight.xle` |
| 可滚动编辑区（行号、选区、着色） | `stdlib/text/codeview.xle` |
| 菜单/工具栏/标签/状态栏/输入框/主题 | `stdlib/ui/*` |
| 窗口、键鼠、剪贴板、文件对话框、截图 | `win*` 内建（`src/gui/gui_win32.c`），由 `@zh` 提供中文函数 |
| 中文标准库与编辑器 API 封装 | `pkg/zh`；应用只显式引入实际使用的中文名称 |

实现阶段顺带修了一个编译器缺陷：被 `import` 的 struct/class 的**方法体**
没有进入依赖闭包，导致方法里调用同文件自由函数时被裁掉并报
「未定义的函数」。修复在 `src/main.c` 的 `scan_type_methods`。

## 已知约束

- 解释器目前不向程序传 argv（`std.process` 尚未实现），所以截图用
  独立入口 `shot.xlc`，而不是 `main.xlc --shot`。
- 字符串里的 `{...}` 是插值语法；源码片段要用原始字符串 `r"..."`。
- `from` 是保留字，不能作参数名。

## 验证结果

- `x check examples/x-editor/src/main.xlc`：通过。
- `doc_test.xlc`：`PASS 91  FAIL 0`。
- 解释执行截图：`build/shot-x-editor.bmp`。
- AOT 正常入口：`build/x-editor.exe`；截图验证入口：`build/x-editor-shot.exe`。
- 语言回归 `tests/p0`：`PASS=65 FAIL=0`。
