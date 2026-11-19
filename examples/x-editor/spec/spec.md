# Notepad++ 风格编辑器（x-editor）规格

> 目的：用 X 语言实现一个类 Notepad++ 的文本编辑器，作为**语言完整性验收程序**。
> 语言缺什么能力，就把该能力封装进 `stdlib/text/`（依赖库），而不是在应用里绕过去。

状态：待评审（评审通过后进入实现）
语言：中文（对话、注释、文档）；源码 `.xlc` 自动中文关键字

---

## 1. 目标与验收判定

### 1.1 目标

一个可日常使用的单窗口多标签文本编辑器，交互与功能对齐 Notepad++ 的常用子集：

- 多标签、新建/打开/保存/另存/全部保存/关闭，未保存修改确认
- 按行文档模型，UTF-8 安全（光标/选区按码点，不会切半个汉字）
- 完整键盘编辑：方向、Home/End、PgUp/PgDn、退格/删除、Tab/Shift+Tab、回车
- 撤销/重做（含多步、快照式）
- 剪切/复制/粘贴/全选/复制当前行/删除当前行/行注释/排序行/去尾空格/大小写
- 查找、替换、全部替换、查找上一个、区分大小写、全词匹配、循环查找、匹配高亮
- 语法高亮：X / C 系 / JSON / HTML / Python / Markdown / INI；按扩展名自动识别，可手动切换
- 菜单栏（文件/编辑/搜索/视图/语言/帮助）、工具栏、标签栏、状态栏
- 行号、当前行高亮、选区、光标跟随滚动、水平滚动、滚轮、滚动条拖拽
- 缩放（Ctrl+加号/减号/0、Ctrl+滚轮），深色/浅色两套配色
- 换行符转换（CRLF / LF / CR），状态栏显示
- 跳转行、关于对话框、退出确认

### 1.2 验收标准（全部必须满足）

1. `x check examples/x-editor/src/main.xlc` 零错误零警告。
2. `x run examples/x-editor/src/shot.xlc` 能开窗、绘制、截图、退出，进程退出码 0。
3. 截图转 PNG 后人工确认：菜单栏/工具栏/标签栏/编辑区/状态栏齐全，中文与代码着色正常，光标可见。
4. AOT：从 `main.xlc` 构建正常编辑器 `build/x-editor.exe`；从 `shot.xlc` 构建的验证程序运行结果与解释档一致。
5. 功能自测脚本（无窗口路径）：`examples/x-editor/tests/doc_test.xlc` 覆盖文档模型与查找替换，输出 `PASS 91  FAIL 0`。
6. `tests/p0` 回归基线保持 PASS=65 FAIL=0。

### 1.3 明确不做（写进 README 注释，避免误期）

- 正则查找、跨文件查找、列编辑/多光标、代码折叠、自动换行
- 编码转换（只支持 UTF-8；状态栏固定显示 UTF-8）
- 打印、宏录制、插件系统、会话保存、最近文件、拖放打开
- 语法高亮的语义级准确（只做词法级；块注释/三引号跨行用行状态位）

---

## 2. 已确认的语言与运行时能力（不得重复实现）

### 2.1 GUI 内建（v0.21 已接入，x.h / interp.c / codegen_c.c / bytecode.c 皆已验证）

| 内建 | 签名 | 说明 |
|---|---|---|
| `winPollKey()` | `-> Array?` | `[key: Int, ch: String, mods: Int]`；mods 1=Shift 2=Ctrl 4=Alt |
| `winPollWheel()` | `-> Int` | 正数向上滚，单位「行」 |
| `winKeyDown(key)` | `-> Bool` | 查询按键当前是否按下 |
| `winWidth()/winHeight()` | `-> Int` | 客户区尺寸 |
| `winSetTitle(s)` | `-> Null` | 改标题 |
| `winSetFont(family,size,bold)` | `-> Bool` | 全局字体 |
| `winSetCursor(kind)` | `-> Null` | 0 箭头 / 1 I 型 / 2 手型 |
| `winClipGet()/winClipSet(s)` | `-> String` / `-> Bool` | 剪贴板 |
| `winOpenFile(title,filter,initial)` | `-> String` | 取消返回 `""`；filter 用 `\|` 分隔 |
| `winSaveFile(...)` | `-> String` | 同上 |
| `winMessage(title,text,kind)` | `-> Int` | 0 信息 / 1 是否 / 2 是否取消，返回 1/0/-1 |
| `winSetResizable(on)` | `-> Null` | 布尔传 0/1 |

绘制原语：`winBeginFrame/winEndFrame/winFillRect/winBorderRect/winText/winTextWidth/winTextHeight/winClip/winUnclip/winFillRound/winBorderRound/winEllipse/winGradient/winScreenshot/winMouseX/winMouseY/winMouseDown/winPollClick`。

键盘语义（已实测）：
- 可打印字符只从 `WM_CHAR` 入队，`key=0`，`ch` 是 UTF-8 字符串（中文输入法可用）。
- 功能键（方向/Home/End/PgUp/PgDn/Delete/Insert/F1..F24/回车/退格/Tab/Esc）与**带 Ctrl/Alt 的键**从 `WM_KEYDOWN` 入队，`key` 是 Win32 虚拟键码。
- Ctrl+A 得到 `key=65, mods=2`；回车得到 `key=13, ch=""`。

### 2.2 标准库既有能力

- `std.fs`：`fsReadText/fsWriteDeep/fsBaseName/fsDirName/fsExtName/fsIsAbsolute/fsNormalize/fsReadLines/fsSplitLines/fsWriteLines/fsEnsureDir/fsTouch/fsCopy/fsTempPath/fsListFiles/fsListDirs`
- `std.str`：`strTrimStart/strTrimEnd/strSplitN/strLines/strLineCount/strReplaceN/strFindNth/strCount/strReverse/strEllipsis/strIsInt/strBasename/strDirname/strExtname/strJoinPath/strPadLeft/strDisplayWidth`
- `std.array`：`arrCopy/arrInsert/arrRemove/arrSlice/arrConcat/arrIndexOf/arrContains/arrSort`
- `@zh`（`pkg/zh`）：中文别名包，`x.toml` 用 path 依赖接入

### 2.3 必须现场确认的两个语义（T1 里程碑）

1. `len(s)` 对非 ASCII 是**码点**还是**字节**。
   - 若是码点，`txtLen(s)` 直接返回 `len(s)`。
   - 若是字节，`txtLen(s)` 用 `while i < len(s) { if len(strAt(s,i)) == 0 { return i }; i = i + 1 }` 计数。
2. `winTextWidth` 对中文串返回的是实际像素宽度（用于光标定位与选区绘制）。
   - 若不是，退化为「ASCII 按单字符宽、CJK 按 2 倍宽」估算，并在代码注释说明。

---

## 3. 文件与模块划分

```
examples/x-editor/
  x.toml                       # name/version/entry/profile + zh 依赖
  spec/spec.md                 # 本文件
  spec/tasks.md                # 任务清单
  src/app.xlc                  # 应用本体（类 + 运行编辑器，中文关键字）
  src/main.xlc                 # 正常启动入口
  src/shot.xlc                 # 截图入口（画 3 帧后写 BMP 并退出）
  tests/doc_test.xlc           # 无窗口自测（文档模型 + 查找 + 高亮 + 按键）
stdlib/text/
  keys.xle                     # 虚拟键码、修饰位、快捷键文本
  doc.xle                      # 文档模型（按行、码点光标、选区、撤销）
  find.xle                     # 查找/替换
  highlight.xle                # 词法着色（7 种语言）
  codeview.xle                 # 编辑区：行号、滚动、选区、着色
```

依赖方向（单向）：`keys ← doc ← find`、`doc ← codeview`、`highlight ← codeview`、`std.ui ← app`。

> 实现说明：外框（菜单/工具栏/标签/状态栏）与单行输入框直接复用已有的
> `std.ui` 组件，编辑区复用 `codeview.xle`。规格早期设想的 `chrome.xle`、
> `form.xle`、`editorview.xle` 与这些组件职责重叠，已在实现阶段合并掉，
> 避免同一标准库维护两套同名能力。

---

## 4. 数据模型：`stdlib/text/doc.xle`

### 4.1 结构

```x
class Doc {
    lines: Array      // 每行文本，不含行尾；至少有 1 个元素
    ln: Int           // 光标行（0 基）
    col: Int          // 光标列（码点，0 基）
    anchorLn: Int     // 选区锚点；与光标相同表示无选区
    anchorCol: Int
    top: Int          // 视口首行
    left: Int         // 水平滚动像素
    undo: Array       // 快照数组，上限 200
    redo: Array
    dirty: Bool
    path: String
    eol: Int          // 0=CRLF 1=LF 2=CR
    lang: Int         // LANG_*，见 highlight.xle
    prefCol: Int      // 纵向移动的目标列，-1 表示无
}
```

快照 = `[arrCopy(lines), ln, col]`。`beforeEdit()` 推入 undo、清空 redo、置 `dirty=true`；`undo()` 把当前状态推入 redo 再恢复；上限 200 条（`arrRemove(undo,0)`）。

**关键约束**：所有修改行数组的操作必须走 `self.lines[i] = 新串` / `arrInsert` / `arrRemove`，不得原地改字符串（语言字符串不可变，天然满足）。恢复快照时必须 `arrCopy` 后再赋值，避免快照被后续原地修改污染。

### 4.2 函数清单（全部按码点）

工具：
- `txtLen(s: String) -> Int`
- `txtIsSpaceChar(c: String) -> Bool`
- `txtLeadingWidth(line: String) -> Int`（前导空格/制表符列数，Tab=4）
- `txtEolText(eol: Int) -> String`

文档：
- `Doc.text() -> String`（按 `eol` 拼接所有行，末尾带换行）
- `Doc.lineCount() -> Int`、`Doc.lineAt(i) -> String`、`Doc.curLine() -> String`
- `Doc.title() -> String`（无路径→`"新建 N"` 由 App 传入名字；有路径→文件名）
- `Doc.ensureCursor()`（钳制 ln/col/anchor）
- `Doc.hasSel() -> Bool`、`Doc.selStart() -> Array[ln,col]`、`Doc.selEnd() -> Array[ln,col]`、`Doc.selText() -> String`、`Doc.selCharCount() -> Int`
- `Doc.setCursor(ln,col,extend: Int)`
- `Doc.clearSel()`

编辑（都先 `beforeEdit()`，除 `deleteSelSilent`）：
- `Doc.insertText(s: String)`：先规范化 `\r\n`/`\r`→`\n`，删除选区，再按行拆分插入
- `Doc.newline()` = `insertText("\n")`
- `Doc.backspace() -> Bool`
- `Doc.deleteForward() -> Bool`
- `Doc.tabIndent()` / `Doc.tabUnindent()`：无选区替换 4 空格；有选区对整行范围增删前导 4 空格
- `Doc.duplicateLine()`、`Doc.deleteLine()`、`Doc.toggleComment()`
- `Doc.moveLineUp()` / `Doc.moveLineDown()`
- `Doc.sortLines()`、`Doc.trimTrailing()`
- `Doc.toUpperSel()` / `Doc.toLowerSel()`
- `Doc.convertEol(eol: Int)`

导航：
- `Doc.moveLeft(extend)`、`moveRight(extend)`、`moveUp(extend)`、`moveDown(extend)`
- `Doc.moveHome(extend)`（首非空↔0 切换）、`Doc.moveEnd(extend)`
- `Doc.moveDocStart(extend)`、`Doc.moveDocEnd(extend)`
- `Doc.pageUp(pageLines)`、`Doc.pageDown(pageLines)`
- `Doc.selectAll()`
- `Doc.gotoLine(n: Int)`（1 基）
- 纵向移动使用 `prefCol`；任何水平移动或编辑后置 `prefCol = -1`

统计：
- `Doc.charCount() -> Int`（不含换行）、`Doc.wordCount() -> Int`（空白分隔）

### 4.3 关键行为约定

- 删除跨行：`backspace` 在列首时把当前行并入上一行，光标落到接缝处。
- 插入多行文本：`head + parts[0]`、中间行、`parts[last] + tail`，光标落在最后一段末尾。
- 撤销期间不产生新的 undo（`undo()` 内部直接改状态，不调 `beforeEdit`）。
- 空文档始终保留 1 个空行，不允许 `lines` 为空数组。

---

## 5. 查找替换：`stdlib/text/find.xle`

```
findNextIn(doc, q, matchCase: Int, wholeWord: Int, wrap: Int) -> Bool
findPrevIn(doc, q, matchCase, wholeWord, wrap) -> Bool
findAllIn(doc, q, matchCase, wholeWord) -> Array   // 每项 [ln, col, len]
replaceOne(doc, q, repl, matchCase, wholeWord) -> Bool
replaceAll(doc, q, repl, matchCase, wholeWord) -> Int
countIn(doc, q, matchCase, wholeWord) -> Int
```

规则：
- 空查询直接返回 False/0。
- 大小写不敏感只做 ASCII（`strLower` 的既有边界），注释里写明。
- 全词：匹配位置左右字符都不是「词字符」（ASCII 字母数字下划线，或码点 ≥ 128 的 CJK）。
- `findNextIn` 从当前选区末尾之后找；到文末且 `wrap=1` 时从头找；命中后设置选区并滚动由 EditorView 负责。
- `replaceOne`：当前选区恰好等于目标时替换，否则先 `findNextIn` 再替换。
- `replaceAll`：先收集全部匹配（自后向前替换，避免位移），返回替换次数；替换后光标置文首。

---

## 6. 语法高亮：`stdlib/text/highlight.xle`

### 6.1 常量

```
LANG_NONE=0 LANG_X=1 LANG_C=2 LANG_JSON=3 LANG_HTML=4 LANG_PY=5 LANG_MD=6 LANG_INI=7
TK_KW=1 TK_STR=2 TK_CMT=3 TK_NUM=4 TK_FUNC=5 TK_TYPE=6 TK_PRE=7 TK_TAG=8 TK_ATTR=9 TK_KEY=10 TK_OP=11 TK_LINK=12 TK_BOLD=13 TK_ANNO=14
```

### 6.2 接口

- `hlLangFromPath(path) -> Int`：按扩展名映射（xle/xlc→X；c/h/cpp/hpp/cc/java/js/ts/go/rs/cs/kt→C；json→JSON；html/htm/xml/svg→HTML；py→PY；md→MD；ini/conf/toml/cfg→INI；其余→NONE）
- `hlLangName(lang) -> String`：状态栏显示名
- `hlLine(line: String, lang: Int, state: Int) -> Array`：返回 `[tokens, newState]`；tokens 每项 `[start, len, kind]`，只含**需要着色**的片段，未覆盖部分按普通文字绘制
- `hlKeyword(kwstr, w) -> Bool`：`strFind(kwstr, "|"+w+"|") >= 0`

状态位：1=`/* */` 块注释；2=`<!-- -->`；3=Python 三引号；4=Markdown 围栏代码块。

### 6.3 各语言规则（词法级）

- **X/C 系**：`//`、`/* */`、字符串 `"`/`'`（含转义，未闭合到行尾）、数字（含 0x/小数/指数）、标识符（关键字→TK_KW，类型名→TK_TYPE，后随 `(`→TK_FUNC）；行首 `#`→TK_PRE 整行；运算符字符→TK_OP。
- **JSON**：字符串 TK_STR（后随 `:` 的字符串改判 TK_KEY）、数字 TK_NUM、`true/false/null` TK_KW；支持 `//` 注释（状态位 1）。
- **HTML/XML**：`<!-- -->`（状态位 2）、标签 `<...>` 内：`<`、`>`、标签名 TK_TAG，属性名 TK_ATTR，引号值 TK_STR；标签外文字 TK_NORM。
- **Python**：`#` 注释、`'''`/`"""` 三引号（状态位 3）、`'`/`"` 字符串、数字、关键字、`def`/`class` 后的名字 TK_FUNC、`@装饰器` TK_ANNO。
- **Markdown**：`#` 标题整行 TK_KW、` ``` ` 围栏（状态位 4，围栏内整行 TK_CMT）、行内 `` `code` `` TK_STR、`**粗体**` TK_BOLD、`[文本](链接)` TK_LINK、行首 `-/*/+` 与 `1.` TK_KW、`>` 引用 TK_CMT。
- **INI**：`;`/`#` 行注释、`[节]` TK_KEY、`key=` 的 key TK_KEY、值中引号串 TK_STR。

性能：只对**可见行**调用 `hlLine`；EditorView 按 `(lineText, lang, state)` 缓存结果，缓存上限 512 条，文档修改时清空。

---

## 7. UI 组件库（复用 `std.ui`）

> 本节的 `chrome.xle` / `form.xle` 设计已不再单独实现：配色走 `std.ui.core.theme`，
> 按钮/菜单/标签/工具栏/输入框走 `std.ui` 既有组件，应用层只负责布局与事件分发。

### 7.1 配色（浅色，对齐 Notepad++ 默认观感）

`CH_BG=0xF5F6F8`、`CH_SURFACE=0xFFFFFF`、`CH_BORDER=0xC9CED6`、`CH_TEXT=0x1F2328`、`CH_DIM=0x6E7781`、`CH_ACCENT=0x1F6FEB`、`CH_ACCENT_D=0x0B4FCB`、`CH_SEL=0xCCE4FF`、`CH_CURLINE=0xF2F6FC`、`CH_GUTTER=0xF0F2F5`、`CH_GUTTER_TEXT=0x9AA4B2`、`CH_TOOLBAR=0xEDEFF3`、`CH_TAB_ACTIVE=0xFFFFFF`、`CH_TAB=0xE6E9EF`、`CH_STATUS=0xE8EBF0`、`CH_OK=0x1A7F37`、`CH_ERR=0xCF222E`。

深色模式：`CH_BG=0x1E1E1E`、`CH_SURFACE=0x252526`、`CH_BORDER=0x3C3C3C`、`CH_TEXT=0xD4D4D4`、`CH_DIM=0x9A9A9A`、`CH_ACCENT=0x4A9EFF`、`CH_SEL=0x264F78`、`CH_CURLINE=0x2A2D2E`、`CH_GUTTER=0x1E1E1E`、`CH_GUTTER_TEXT=0x858585`、`CH_TAB_ACTIVE=0x1E1E1E`、`CH_TAB=0x2D2D2D`、`CH_STATUS=0x007ACC`（文字白）。
语法色（浅/深各一套）：关键字 `0x0033B3`/`0x569CD6`、字符串 `0x067D17`/`0xCE9178`、注释 `0x8C8C8C`/`0x6A9955`、数字 `0x1750EB`/`0xB5CEA8`、函数 `0x00627A`/`0xDCDCAA`、类型 `0x0E7C7B`/`0x4EC9B0`、预处理 `0x9E880D`/`0xC586C0`、标签 `0x800000`/`0x569CD6`、属性 `0xE50000`/`0x9CDCFE`、键 `0x0451A5`/`0x9CDCFE`、运算符 `0x333333`/`0xD4D4D4`、链接 `0x1F6FEB`/`0x4A9EFF`。

### 7.2 组件

`chrome.xle`：
- `chButton(r: Rect, label: String, hovered: Int, primary: Int)`：圆角按钮
- `chToolWidth(label) -> Int`、`chToolItem(r, label, hovered)`：工具栏项
- `chTabWidth(title) -> Int`、`chTabHeight() -> Int`、`chDrawTab(r, title, active, hovered, dirty, hoverClose)`、`chTabCloseRect(r) -> Rect`
- `class PopupMenu { x,y,w,items,hover }`，`items` 每项 `[id, label, shortcut]`，`id=-1` 分隔线；方法 `draw()`、`itemAt(mx,my) -> Int`、`height() -> Int`、`widthFor(items) -> Int`
- `chMenuBarHeight() -> Int`、`chMenuTitleWidth(s) -> Int`、`chDrawMenuTitle(r, title, open, hover)`
- `chStatus(r, sections: Array)`：左中右三段绘制

`form.xle`：
- `class EditField { value: String, caret: Int, focused: Int, scroll: Int }`
- `draw(r: Rect, placeholder: String)`（带边框、焦点色、光标闪烁由时间驱动）
- `key(ev: Array) -> Int`（0=未消费，1=回车提交；处理退格/删除/左右/Home/End/字符插入/粘贴/全选）
- `click(mx, my)`
- `setValue(s)`

---

## 8. 编辑区：复用 `stdlib/text/codeview.xle`

> 本节的 `editorview.xle` 几何与绘制设计已由既有 `codeview.xle` 承担，
> 应用层通过 `CodeView` 的公开方法（`draw/wheel/key/click` 等）驱动。

### 8.1 几何

```
lineH    = winTextHeight() + 4        // 行高
gutterW  = 8 + 数字位数*winTextWidth("0") + 10
textX    = rect.x + gutterW + 6 - doc.left
textY(i) = rect.y + 3 + (i - doc.top) * lineH
```

- 垂直滚动条宽 12，水平滚动条高 12，位于 `rect` 右下；两者都支持点击翻页与拖拽。
- `colAt(line, px) -> Int`：累加 `winTextWidth(前缀)` 找最接近的码点边界。
- `caretX()/caretY()` 由当前 ln/col 计算。

### 8.2 绘制顺序

1. 背景 + 当前行高亮
2. 选区（跨行分段）
3. 行号（右对齐，当前行加粗/高亮色）
4. 文字（按 `hlLine` 的 tokens 分段着色；`winClip` 裁剪到编辑区）
5. 查找匹配高亮（全部匹配描边，当前匹配填充）
6. 光标（1px 竖线，高 lineH-4）
7. 滚动条与角落

### 8.3 输入

`EditorView.key(ev) -> Int` 返回动作码：

```
ACT_NONE=0 ACT_NEW=1 ACT_OPEN=2 ACT_SAVE=3 ACT_SAVEAS=4 ACT_CLOSE=5 ACT_EXIT=6
ACT_FIND=7 ACT_REPLACE=8 ACT_GOTO=9 ACT_ZOOMIN=10 ACT_ZOOMOUT=11 ACT_ZOOMRESET=12
ACT_NEXTTAB=13 ACT_PREVTAB=14 ACT_SAVEALL=15 ACT_TOGGLEDARK=16 ACT_ABOUT=17
```

视图自行处理：字符插入（`ch != ""` 且 mods 无 Ctrl/Alt）、回车、退格、删除、方向、Home/End/PgUp/PgDn、Tab/Shift+Tab、Ctrl+A/C/X/V/Z/Y/D/L/Q、Ctrl+Shift+Up/Down、Ctrl+Home/End、Ctrl+F/H/G/S/N/O/W、Ctrl+=/-/0、Ctrl+Tab/Shift+Tab、Ctrl+Shift+S。其余未识别键返回 ACT_NONE。

`click(mx,my,shift)`：编辑区内设定光标（shift 扩展）；行号区单击整行选中；滚动条区进入拖拽。
`dragTo(mx,my)`：拖动扩展选区。
`wheel(n)`：滚动 3n 行（Ctrl 按下时改为缩放）。

---

## 9. 应用装配：`examples/x-editor/src/app.xlc`（入口 `main.xlc` / `shot.xlc`）

### 9.1 布局（窗口默认 1120x720，可缩放）

```
y=0    菜单栏      高 30（文件 编辑 搜索 视图 语言 帮助）
y=30   工具栏      高 38（新建 打开 保存 全部保存 | 剪切 复制 粘贴 | 撤销 重做 | 查找 替换 | 注释 大写 小写 | 深色）
y=68   标签栏      高 30（文件名 + 修改点 + 关闭 x）
y=98   编辑区      剩余高度 - 查找面板 - 状态栏
...    查找面板    高 0 或 76（搜索框、替换框、区分大小写、全词、循环、查找上一个/下一个、替换、全部替换、关闭）
底     状态栏      高 26（行:列 | 选区/长度 | 换行符 | UTF-8 | 语言 | INS）
```

### 9.2 多标签

- `App` 持有 `docs: Array`、`active: Int`、`tabNames: Array`。
- 切换/关闭/新建都更新标题（`winSetTitle("文件名 - notepad-x")`）。
- 关闭脏文档弹三选模态（保存/不保存/取消）；退出时遍历所有脏文档。
- 标签宽度 = `chTabWidth(title)`，超出可视宽度从左侧截断（保证当前标签可见）。

### 9.3 菜单项

- 文件：新建(Ctrl+N)、打开(Ctrl+O)、保存(Ctrl+S)、另存为(Ctrl+Shift+S)、全部保存、关闭(Ctrl+W)、退出(Alt+F4)
- 编辑：撤销(Ctrl+Z)、重做(Ctrl+Y)、剪切(Ctrl+X)、复制(Ctrl+C)、粘贴(Ctrl+V)、全选(Ctrl+A)、复制当前行(Ctrl+D)、删除当前行(Ctrl+L)、行注释(Ctrl+Q)、缩进(Tab)、反缩进(Shift+Tab)、转大写、转小写、排序行、去行尾空格、转为 CRLF、转为 LF、转为 CR
- 搜索：查找(Ctrl+F)、替换(Ctrl+H)、查找下一个(F3)、查找上一个(Shift+F3)、全部替换
- 视图：显示行号、状态栏、放大(Ctrl+=)、缩小(Ctrl+-)、恢复缩放(Ctrl+0)、深色模式
- 语言：普通文本、X、C 系、JSON、HTML、Python、Markdown、INI（当前项打勾）
- 帮助：关于

菜单开合：点击顶级标题展开；展开时悬停切换；点击项执行并收起；Esc/点空白收起；`PopupMenu` 在**所有控件之后**绘制。

### 9.4 查找面板

- 打开时聚焦查找框；回车=查找下一个；Esc=关闭并回编辑区。
- 面板文本变化时重算全部匹配并高亮；状态 `第 x/y 个匹配（共 n）`。
- 替换时保持面板不关闭。

### 9.5 模态

`class ModalState { kind: Int, title: String, text: String, choice: Int }`
- 关于：标题/版本/语言能力说明，确定关闭。
- 未保存：保存/不保存/取消（返回 1/0/-1，复用 `winMessage` 的返回语义）。
- 跳转行：一个 `EditField` + 确定/取消；非法输入不关闭。

### 9.6 文件读写

- 打开：`winOpenFile("打开文件", "所有文件 (*.*)|*.*|文本文件 (*.txt)|*.txt|X 源文件 (*.xle;*.xlc)|*.xle;*.xlc", 上次目录)`；取消返回 `""`。
- 保存：有路径→`fsWriteDeep`；无路径→`winSaveFile`。
- 打开时识别并去掉 UTF-8 BOM（字节 `EF BB BF`）；换行符按首个 `\n` 前是否有 `\r` 判定，其次 `\r` 单独。
- 保存失败用 `winMessage(..., 0)` 提示。

### 9.7 主循环

```
winOpen("notepad-x", 1180, 760); winSetResizable(1); winSetFont("Microsoft YaHei UI", 14, 0)
while 运行中:
    winBeginFrame(); 按 winWidth/winHeight 重排; 绘制; winEndFrame()
    处理 winPollKey 队列（全消费）→ EditorView.key / 全局快捷键 / 模态优先
    处理 winPollWheel
    处理 winPollClick 队列
    if winMouseDown(1): 拖拽选择 / 滚动条拖拽
    若 winShouldClose() 且无脏文档 → 退出；有脏 → 弹未保存模态
    winSleep(16)
winClose()
```

`运行编辑器(截图路径, 模式)`：截图路径非空时绘制 3 帧后 `winScreenshot(path)`、`winClose()`、退出；用于自动验收。解释器目前不向程序传 argv，所以用 `shot.xlc` 入口而不是命令行参数。

---

## 10. 验证步骤（实现完成后必须逐条执行）

1. `x check examples/x-editor/src/main.xlc`。
2. `x run examples/x-editor/src/shot.xlc` 生成 `build/shot-x-editor.bmp`；PowerShell 用 `System.Drawing` 转 PNG，检查布局与配色。
3. `x run --profile=console examples/x-editor/tests/doc_test.xlc`，输出 `PASS 91  FAIL 0`。
4. AOT：从 `main.xlc` 构建 `build/x-editor.exe` 作为正常编辑器；从 `shot.xlc` 构建 `build/x-editor-shot.exe`，运行后者复验截图。
5. `tests/p0` 回归 PASS=65 FAIL=0。
6. 手工验证文件对话框（打开/保存/另存）、未保存三选、标签切换与关闭。

---

## 11. 附录：构建命令（Windows）

```
cd e:\\项目\\x-project\\x-lang
python tools\\gen_gui_embed.py
C:\\xgcc\\mingw64\\bin\\gcc.exe -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -D_XOPEN_SOURCE=700 src\\platform.c src\\crash.c src\\trace.c src\\target.c src\\profile.c src\\pkg.c src\\semver.c src\\registry.c src\\lockfile.c src\\json_read.c src\\lsp.c src\\utf8.c src\\util.c src\\lexer.c src\\parser.c src\\bytecode.c src\\xbc.c src\\compile.c src\\interp.c src\\gui\\gui_win32.c src\\codegen_c.c src\\main.c -lgdi32 -luser32 -lws2_32 -lmsimg32 -lcomdlg32 -o C:\\xtmp\\x.exe
Copy-Item C:\\xtmp\\x.exe build\\x.exe -Force
$env:X_STDLIB_PATH="e:\\项目\\x-project\\x-lang\\stdlib"
.\\build\\x.exe run examples\\x-editor\\src\\shot.xlc
.\\build\\x.exe run --profile=console examples\\x-editor\\tests\\doc_test.xlc
```
