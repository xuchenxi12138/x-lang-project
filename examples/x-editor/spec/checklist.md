# 验收清单

- [x] 标准库不依赖绝对路径：应用用 `std.*` 与 `@zh`（相对 `x.toml` 的 `path` 依赖）引用。
- [x] 每个 `import` 都显式声明引入的名字；未使用的名字不参与编译（编译器依赖闭包）。
- [x] `stdlib/text/keys.xle` 提供键码、修饰位、快捷键文本。
- [x] `stdlib/text/doc.xle` 提供按行文档模型、码点光标、选区、快照撤销。
- [x] `stdlib/text/find.xle` 提供大小写/全词/循环查找与替换。
- [x] `stdlib/text/highlight.xle` 支持 X/C/JSON/HTML/Python/Markdown/INI 与跨行状态。
- [x] `examples/x-editor/src/app.xlc` 提供菜单、工具栏、标签、查找面板、状态栏。
- [x] 工具栏包含 新建/打开/保存/全部保存/剪切/复制/粘贴/撤销/重做/查找/替换/注释/大写/小写/深色。
- [x] 查找面板包含 查找框、替换框、区分大小写、全词、循环、上一个、下一个、替换、全部替换、关闭。
- [x] `x check examples/x-editor/src/main.xlc` 通过。
- [x] `x run --profile=console examples/x-editor/tests/doc_test.xlc` 输出 `PASS 91  FAIL 0`。
- [x] 解释档 `shot.xlc` 生成 `build/shot-x-editor.bmp`，布局与着色正常。
- [x] AOT 从 `src/main.xlc` 生成正常编辑器 `build/x-editor.exe`，从 `src/shot.xlc` 生成截图验证程序。
- [x] `tests/p0` 回归 `PASS=65 FAIL=0`。
- [x] 编译器缺陷已修：被 import 的 struct/class 方法体计入依赖闭包（`src/main.c` 的 `scan_type_methods`）。
- [x] `examples/x-editor/README.md` 的运行方式、功能对照、未实现项与语言缺口与代码一致。
