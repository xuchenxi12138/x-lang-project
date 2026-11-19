# X Language for VS Code

X 语言（`.xle` / `.xlc`）的语法高亮与代码提示扩展。

## 两套能力，两个来源

这个扩展提供两类功能，它们的实现机制完全不同：

| 能力 | 来源 | 依赖 |
|---|---|---|
| 语法高亮、括号配对、注释、自动缩进 | 扩展自带的 declarative 配置 | 无 |
| 补全、悬停、参数提示、文档大纲 | `x.exe lsp`（语言服务） | **必须能找到 x.exe** |

**前者不需要任何东西，装上就有；后者找不到 `x.exe` 就一个都没有。**

这个区别极其重要 —— 高亮正常工作会让人以为扩展装好了，而代码提示的缺失
完全静默。排查顺序见仓库里的 `docs/打包与异地使用.md` 第四节。

## 代码提示（0.2.0 起）

| 能力 | 触发 | 效果 |
|---|---|---|
| 补全 | 输入时自动，或 `Ctrl+Space` | 本文档符号 + 37 个关键字 / 36 个中文别名 + 96 个内建 + 类型 |
| 悬停 | 鼠标停在标识符上 | 内建函数的参数个数与返回类型 / 本文档函数的定义行 |
| 参数提示 | 输入 `(` 或 `,` | `arrPush(#1, #2)` / 自己写的 `greet(name)` |
| 文档大纲 | `Ctrl+Shift+O` | 文件里所有函数与变量，可跳转 |

补全词表不是手写的，由 `tools/gen-completions.js` 从 C 源码提取：

```
src/lexer.c     kKeywords / kZhAliases / kZhTypeAliases  → 关键字与中文别名
src/bytecode.c  g_builtins[]                            → 内建函数与参数个数
src/compile.c   ty_from_name / 特殊形式                 → 类型名、print/println
```

**词法表比语法能力宽**：`lexer.c` 里有 57 个英文关键字，`parser.c` 只消费了 37 个。生成器里有一份
`DEAD_KEYWORDS` 黑名单把剩下 20 个剔掉，理由和判据见下面「哪些语法被故意不高亮」。

改编译器后重新生成，提示自动跟上：

```bash
node tools/gen-completions.js --write    # 写入 src/lsp_data.h
sh ../tools/build.sh                     # 重新编译 x.exe
```

## 这个扩展解决什么

VSCode 默认把 `.xle` / `.xlc` 当纯文本，于是函数名、变量名、类型名、关键字全是同一个颜色，读代码只能靠脑子分类。装上之后：

| 代码元素 | scope | 典型配色 |
| --- | --- | --- |
| 函数定义 `fn add` / `函 square` 里的名字 | `entity.name.function.x` | 黄 / 橙 |
| 函数调用 `add(1, 2)` | `support.function.call.x` | 与定义点区分 |
| 内建函数 `print` `len` `array` … | `support.function.builtin*.x` | 蓝 |
| 反射内建 `className` `fieldGet` `annoHas` … | `support.function.builtin.reflect.x` / `.annotation.x` | 蓝（可单独配色）|
| 类 / 接口 / 结构体名 `class User` / `类 甲` | `entity.name.type.class.x` | 类型色 |
| 构造函数 `constructor(...)` | `entity.name.function.constructor.x` | 函数色 |
| 继承目标 `class B extends A` 里的 `A` | `entity.name.type.x` | 类型色 |
| `new` / `新建` | `keyword.operator.new.x` | 运算符色 |
| `extends` `implements` / `继承` `实现接口` | `keyword.other.inheritance.x` | 关键字色 |
| 修饰符 `public` `private` `protected` `abstract` `override` `static` | `storage.modifier.x` | 修饰符色 |
| `self` / `自身` | `variable.language.self.x` / `.zh.x` | 语言内建色 |
| 注解 `@Table("users")` / `@Deprecated` | `entity.name.annotation.x` + `punctuation.definition.annotation.x` | 注解色 |
| 变量声明处 `let x` / `令 x` 的名字 | `variable.other.declaration.x` | 比引用更亮 |
| 变量引用 | `variable.other.x` | 常规前景色 |
| 函数参数 | `variable.parameter.x` | 参数色 |
| 类型 `Int` / `整数` | `support.type.primitive.x` / `.zh.x` | 青 |
| 声明关键字 `fn` `let` / `函` `令` | `storage.type.*.x` | 关键字色 |
| 控制流 `if` `for` `return` / `如果` `遍历` `返回` | `keyword.control.x` / `.zh.x` | 关键字色 |
| 词级运算符 `or` `not` / `或` `非` | `keyword.operator.word.*.x` | 运算符色 |
| 常量 `true` `false` `null` / `真` `假` `空` | `constant.language.*.x` | 常量色 |
| 字符字面量 `'a'` | `constant.character.x` | 常量色 |
| 包引用 `import @mathutil` | `entity.name.namespace.x` | 命名空间色 |

配色由你当前的主题决定，扩展只负责打 scope，不改颜色。想确认某个 token 拿到了什么 scope：`Ctrl+Shift+P` → `Developer: Inspect Editor Tokens and Scopes`。

**这份表格只有实现里真实存在的语法。** 枚举、特征、`impl`、`type`、`mod`、`match`、泛型、`super()` 都不在里面 —— 原因见下面「哪些语法被故意不高亮」。

## 为什么没引入 vscode-languageclient

0.1.0 是纯声明式扩展，只有高亮；0.2.0 加了 `main` 和语言服务客户端。加客户端时没有用官方的 `vscode-languageclient`，而是手写了一份 `src/lspClient.js`：

- **零依赖**：`require('vscode')` 由编辑器注入，`child_process` / `path` / `fs` 是 Node 内置。整个扩展没有一个 `dependencies` 条目，`vsce package` 不需要先 `npm install`。
- **能打包**：官方库带依赖树，打包前必须先装依赖。这台机器能装、那台不能装，扩展就发不出去。现在 clone 下来就能打。
- **只用到 4 个能力**：补全、悬停、参数提示、大纲。官方库为这 4 个能力提供了中间件、能力协商、feature 注册三层抽象，代价是几百 KB 与更多出错面。

手写的代价是那些映射必须自己维护，而它们**编译器管不到** —— LSP 的 `CompletionItemKind` 与 `SymbolKind` 数字编号跟 VS Code 的枚举是两套完全不同的值。所以有两个验证脚本真的启动 `x.exe lsp`、真的按协议发字节：

```bash
node tools/check-lsp.js      # 服务端：89 项
node tools/check-client.js   # 客户端：35 项（用 vscode 桩跑真 lspClient.js）
```

`tools/package.sh` 会先跑这两个再打包，避免发出「装完才发现不能用」的包。

`engines.vscode` 定在 `^1.60.0` 而非最新版：用到的 API（`registerCompletionItemProvider`、`DocumentSymbol`、`MarkdownString`）在 2018 年就稳定了，抬门槛只会把 Cursor / VSCodium / 各类衍生编辑器挡在门外。

### 关于 activationEvents

0.1.0 时期扩展没有 `main`，那时 `package.json` 里**不能**写 `activationEvents` —— VSCode 会判为 error 并跳过整个扩展：

```
[error] [.../x-lang-0.1.0]: 如果扩展没有"main"或"browser"属性，则应省略属性"activationEvents"。
```

0.2.0 有 `main` 了，规则反过来：**必须**写，否则扩展永远不激活、语言服务永不启动。

```json
"activationEvents": ["onLanguage:x", "onCommand:xlang.showServerLog", "onCommand:xlang.restartServer"]
```

`onLanguage:x` 保证打开 `.xle` 才启动，不拖编辑器启动速度。

现在 `tools/check-manifest.js` 与 `tools/package.sh` 都会校验这类一致性（`main` 与 `activationEvents` 要么都有、要么都没有），也会检查 `contributes` 引用的文件是否真实存在。

**改完 `package.json` 都要跑一遍。** 清单错误是静默的 —— 没有红屏、没有弹窗，只有"装上了但没反应"。

## 规则从哪来

**不来自设计文档。** `spec/01-语言设计.md` 描述的是完整设计，包含字符串插值 `"{expr}"`、原始串 `r"..."`、三引号多行串、`Complex` 字面量 `2.5i`、`trait`、`match` 等。但 P0 实现里**没有**这些。照文档写高亮，等于高亮一堆编译器不认的语法——用户照着颜色写，编译不过。这比没有高亮更糟。

所以全部规则从实现反推：

| 事实 | 来源 |
| --- | --- |
| 57 个英文关键字 + 56 个中文别名 | `src/lexer.c` 的 `kKeywords[]` / `kZhAliases[]` |
| 中文类型别名（且只有 6 个可用） | `src/lexer.c` 的 `kZhTypeAliases[]` + `compile.c` 的 `ty_from_name` |
| 96 个内建函数 | `src/bytecode.c` 的 `g_builtins[]` + `compile.c` 里认的特殊形式 |
| 类型只有 7 个：`Int` `Float` `Bool` `String` `Array` `void` `Void` | `src/compile.c` 的 `ty_from_name` |
| 运算符全集（含 `<<` `>>` `=>` `??`） | `src/lexer.c` 的 `tk_name[]` |
| `@` 有**两个**用途：注解 `@Table(...)` 和包引用 `import @pkg` | `src/parser.c` 的 `parse_annotations` / `parse_import` |

补全词表不是手写的：`src/lsp_data.h` 是**自动生成**的，唯一真相来源就是上面这几个 C 数组。

```bash
cd vscode-x
node tools/gen-completions.js --write    # 重新生成 ../src/lsp_data.h
```

生成器里带三层守卫，缺一层就会出现「脚本说成功、补全里却没有类关键字」：

1. **最少数量**：关键字少于 30 / 内建少于 50 就失败 —— 只能发现提取规则整体失效。
2. **具名清单**：`class` `interface` `abstract` `private` `public` … 必须逐个存在 —— 发现「某一类特性漏提」。
3. **方向断言**：死 token 一个都不能混进补全表 —— 发现黑名单逻辑被改坏。

第 2、3 层是吃了亏才加的：类系统实现后关键字从 45 涨到 57、内建从 84 涨到 96，**只查数量的脚本照样全绿**，而 `lsp_data.h` 还停留在类系统之前，补全里一个类关键字都没有。

## 哪些语法被故意不高亮

判据只有一条：**`build/x.exe check` 能不能过**。

### 57 个关键字里只有 37 个真能用

`lexer.c` 定义了 57 个英文词和 56 个中文别名，但 `parser.c` 只消费了其中一部分。逐个实测下来：

| 可用（37 英文 / 36 中文） | 内容 |
| --- | --- |
| 控制流 · 英 | `if` `else` `for` `while` `loop` `break` `continue` `return` `in` |
| 控制流 · 中 | `如果` `否则` `遍历` `当` `循环` `跳出` `继续` `返回` `于` |
| 词级运算符 | `or` `not` / `或` `非` |
| 模块 | `import` `from` `as` / `引入` `从` `作为` |
| 声明 | `fn` `let` `var` `const` / `函` `令` `变` `常` |
| 常量 | `true` `false` `null` / `真` `假` `空` |
| 修饰符 | `pub` `public` `defer` / `公开` `延后` |
| **类系统** | `class` `interface` `abstract` `final` `extends` `implements` `override` |
| **可见性** | `private` `protected` `public`（`pub` 与 `public` 同义） |
| **实例** | `self` `new` `struct` |
| **类系统 · 中** | `类` `接口` `抽象` `终态` `继承` `实现接口` `覆写` `私有` `保护` `公开` `自身` `新建` `结构` |

剩下 **20 个是死 token**：`enum` `trait` `impl` `type` `mod` `match` `case` `do` `catch` `yield` `comptime` `extern` `unsafe` `dyn` `spawn` `par` `where` `mut` `ref` `super`，以及中文的 `枚举` `特征` `实现` `类型` `模组` `匹配` `情形` `做` `捕获` `产出` `编译期` `外部` `不安全` `动态` `派生` `并行` `约束` `父类`。它们出现在代码里时，`x` 报的是「期望一个表达式，实际读到 X」—— 所以不给它们关键字色。

实测方式：

```
$ x check t.xle          # 内容: enum C { Red }
  error: 期望一个表达式，实际读到 `enum`
```

死 token 全是同一个错法 —— 它们只是 `lexer.c` 里的 token 定义，`parser.c` 从来没有对应分支。

### `super` 是最容易误判的一个

值得单独说，因为它在三个地方都「写着存在」：`kKeywords` 有 `super`、`kZhAliases` 有 `父类` → `TK_SUPER`、`x.h` 有 `TK_SUPER` 枚举。但：

```
$ grep -w TK_SUPER src/*.c
src/lexer.c
src/x.h
```

`parser.c` 里出现 **0 次**。实测 `super()` 报「期望一个表达式，实际读到 `super`」—— 语法层没有超级调用。**词法登记 ≠ 语法实现**，而这两件事在 `lexer.c` 里是挨着写的，扫一眼很容易看错。

### 三个脚本，各自防一类错

| 脚本 | 防什么 |
| --- | --- |
| `tools/probe-dead-keywords.sh` | 逐个用例实测关键字可用性，并与 `gen-completions.js` 的 `DEAD_KEYWORDS` **逐项对账**，多一个少一个都 FAIL |
| `tools/probe-samples.js` | 扫一批样例行，断言死 token 拿不到关键字 scope |
| `tools/check-grammar.js` | 57 条显式断言（scope 必须等于某个确切值），含「不是 keyword.*」的反向断言 |

判据必须写死成「期望值」，不能让脚本自己跑一下再定结论。编译器把 `super` 实现了之后，`probe-dead-keywords.sh` 会 FAIL，提醒你去改黑名单；如果脚本自己推断期望，它会安静地把 `super` 划到「可用」，而补全的黑名单还留着它 —— 补全于是永远不推荐一个其实能用的词。

### 删掉的规则

第一版按设计文档写了 `enum` / `trait` / `impl` / `type` / `mod` 的声明规则、泛型参数规则、装饰器注解 `@derive(Clone)`。**全删了**：泛型 `fn h[T: Ord]` 报「期望 `(`，实际读到 `[`」；当时 `@` 在实现里唯一的用途是 `import @mathutil`。

v0.16 实现了注解，所以 `@Name` / `@Name(args)` / `@Name(key = value)` **又加回来了** —— 但只加实测能过的形态：注解可以挂在类/接口声明和类体成员上，**不能挂在顶层函数或 struct 上**（实测 `@Deprecated` + 顶层 `fn f()` 报「期望一个表达式，实际读到 `@`」）。

留着的后果很具体：用户看到 `Point` 被标成类型色，以为某个语法可用，写完一编译才发现根本没实现。**高亮不存在的语法比不高亮更伤人**，因为它是一句编译器没做过的承诺。

### 类型清单收紧了

原来按文档列了 `Int8` `UInt8` `Size` `Float32` `Complex` `Matrix` `Char`，但 `ty_from_name` 只认 7 个名字（`Int` `Float` `Bool` `String` `Array` `void` `Void`），未知名字返回 `TY_UNKNOWN` —— 而 `compile_var_decl` 遇到 `TY_UNKNOWN` 会**跳过类型检查直接编译**。实测这几个全都能零警告编过：`let a: Int8 = 0`、`let a: Char = 'x'`、`let a: Complex = 1`。语义完全没生效，而这种静默失效比报错更难发现，所以不该给它上色。

中文别名同理：`字符`→`Char`、`复数`→`Complex` 归一化后仍是未知类型，所以只高亮 `整数` `浮点` `布尔` `字符串` `数组` `无` 这 6 个。

### 25 个数学函数名不是内建

`math` 是单入口、编译期按名字查表：`math(sqrt, 16)` 的第一个参数必须是**裸标识符**（写成 `math("sqrt", 16)` 会报「第一个参数必须是数学函数名」）。那 25 个子函数名（`pow` `sqrt` `cbrt` `floor` `ceil` `round` `trunc` `abs` `sign` `min` `max` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `log` `log2` `log10` `exp` `hypot` `fmod` `clamp`）本身不可调用：

```
$ x check t.xle          # 内容: println(sqrt(16))
  error: 未定义的函数 `sqrt`
```

所以它们不在 `#builtins` 里 —— 高亮了会让人写 `sqrt(16)`。`math` 自己在 `g_builtins` 里，已列出。

## 五个技术坑（改这份语法前必须知道）

### 一、`\b` 对中文完全失效

这是最要命的一条。`\b` 是**ASCII 单词边界**，定义是"`\w` 与 `\w` 之间的位置"，而 `\w` 只含 `[A-Za-z0-9_]`，不含 CJK。汉字两侧都是非单词字符，所以 `\b函` 这个断言**永远不成立**。

```js
'函 square'.match(/\b(函)\s+/)      // null
/(函)\s+/.test('函 square')          // true
```

症状特别隐蔽：`.xle`（英文）高亮正常，`.xlc`（中文）里 `函` `令` `如果` `返回` 全部无色。而中文编程是这门语言的一等能力，不是边角场景。

本文件里**所有**边界断言都写成显式字符类：

```json
"(?<![\\w\\u0080-\\uFFFF])(fn|函)(?![\\w\\u0080-\\uFFFF])"
```

`\u0080-\uFFFF` 覆盖 BMP 内全部汉字（`函` U+51FD、`令` U+4EE4 都在其中）。后行断言 `(?<!...)` 在 Oniguruma（VSCode 的正则引擎）和 JS 里都支持。新增规则时别偷懒用 `\b`。

### 二、规则顺序有语义

TextMate 从上到下匹配，**第一条命中即停**。顺序错了不报错，只是"某些颜色莫名其妙不对"，极难查。四条硬约束：

1. **注释、字符串排最前** —— 否则字符串里的 `//` 被当注释、注释里的 `"` 被当字符串
2. **数字排标识符前** —— 否则 `0xFF` 的 `x` 被吞进标识符
3. **声明排关键字前** —— 否则 `fn` 后面那个函数名的正则会先吃掉 `fn` 本身
4. **内建排函数调用前** —— 两条规则都对「名字 + 左括号」成立，内建在后就永远轮不到（见坑三）
5. **运算符按长度降序** —— 三字符 → 两字符 → 单字符

第 5 条是修 bug 修的。原来 `#operators` 把 `comparison`（含 `<` `>`）排在 `bitwise`（含 `<<` `>>`）前面，结果：

```
a << 2   →   "<" "<"  两个独立的 comparison，而不是一个 shift
c => 1   →   "=" ">"   赋值号 + 大于号，而不是 fat arrow
```

这两个都是「不报错、只是颜色不对」的典型。根治办法只有整组重排，把多字符运算符整体提到单字符前面去 —— 组内不同 scope 之间不重叠（`<=` 与 `<<` 不冲突、`==` 与 `=>` 不冲突），所以能安全并列在同一长度层。

另外两个单字符位置坑：

- **`///` 必须排 `//` 之前**。反过来的话，文档注释末尾两个斜杠被普通行注释吃掉，整行退化成 `comment.line`
- **`..=` 必须排 `..` 之前**。反过来的话，`..` 先吃掉两个点，剩下的 `=` 独立成赋值号。词法器里这个优先级是手工回溯解决的，正则里只能靠顺序

### 三、区域 `end` 等不到匹配时，会吞到文件尾

`fn` 签名用 `begin`/`end` 包成 `meta.function.x`，`end` 写的是 `(?=\{)` —— 「看到左大括号就收工」。这对有函数体的函数是对的，但**接口方法和抽象方法没有函数体**：

```
interface Named {
    fn name() -> String      // 行尾没有 {
}
```

`end` 永远等不到那个 `{`，区域于是继续往后吞，把该行之后的**整个文件**都染成 `meta.function.x`。症状：`interface` 之后的 `class` / `abstract` / `@注解` 全部掉色，而它们各自的规则根本没机会跑。

这个 bug 极难定位，因为它不报错、不抛异常，表现是「文件后半截没颜色」—— 很容易被当成主题问题而不是语法问题。

修法不是把 `end` 改成 `$`（那样跨行签名会在首行就断），而是加一条**排在前面**的规则，用单条 `match` 吃掉「整行 fn 签名且行尾没有左大括号」的形态：

```json
"(?<![\\w\\u0080-\\uFFFF])(fn|函)(\\s+)([A-Za-z_][\\w]*)(\\s*)(\\([^)]*\\))(\\s*)(?:(->)(\\s*)([A-Za-z_][\\w]*))?(?=\\s*(?:[}]|$))"
```

它是 `match` 不是 `begin`/`end`，因为它本来就只占一行，不需要区域。跨行签名的首行是 `fn foo(`（不以 `)` 结尾），不会误命中，仍走原来的 `begin`/`end`。

`tools/check-grammar.js` 里有一条专门的回归断言：取 `interface { fn ... }` + `class` 四行，逐行 tokenize 保持 `ruleStack` 传递，检查最后那行的 `class` 是否仍是 `storage.type.class.x`。

### 四、captures 编号按括号数，不按「想要的组」数

`import @mathutil` 这条规则，第一版 captures 写成 3=@、4=包名、5=…，整整错位一位。原因是它按「逻辑上有几段」数编号，而正则里：

```
(import|引入)      → 1
(\\s+)             → 2   << 这一组在输出里不可见，最容易忘
(@)                → 3
([A-Za-z_]...)     → 4
```

于是包名被标成了 `punctuation.definition.package`（标点色），`@` 和 `import` 同色。**光看颜色发现不了** —— 标点和命名空间名在多数主题里都偏灰。

这类错只有断言能抓：`check-grammar.js` 里「`mathutil` 必须是 `entity.name.namespace.x`」当场 FAIL。

### 五、区域 scope 会吞掉没单独声明的 token

`fn` 声明用 `begin`/`end` 包住整个签名，区间内的 token 都会继承 `meta.function.x` 作为外层 scope。这时如果某个 token 没有自己的规则，它**不是没有颜色**，而是显示成"一层没意义的底色"——看起来像漏了，其实是被父级吞了。

`->` 就是这样：原来它落在 `meta.function.x` 里，什么也看不出来。单加一条 `keyword.operator.arrow.x` 才解决。

判断方法：看 leaf scope 是否等于父级区域 scope。等于就说明这条 token 没被识别，得补规则。

## 怎么验证（别用肉眼）

手写正则模拟器会骗你。开发过程中就因此误判过两次：一次以为 `fn` 声明没命中（模拟器不处理 begin/end 区域），一次以为字符串没上色（模拟器没消费到 `end`）。**结论必须来自 VSCode 自己的引擎。**

仓库带了 `tools/`：

```bash
# 依赖装在扩展目录之外，否则 node_modules 会被 vsce 打进扩展包
mkdir -p /tmp/tm && cd /tmp/tm && npm i --no-save vscode-textmate vscode-oniguruma

cd x-lang/vscode-x/tools
NODE_PATH=/tmp/tm/node_modules node check-grammar.js    # 57 条断言，只会说 PASS/FAIL
NODE_PATH=/tmp/tm/node_modules node verify-scopes.js    # 跑真实测试文件，统计回退数
NODE_PATH=/tmp/tm/node_modules node probe-samples.js    # 跑全构造样例，逐 token 看 scope
```

三个脚本角色不同，别只跑一个：

| 脚本 | 角色 | 会失败吗 |
| --- | --- | --- |
| `check-grammar.js` | **断言集**，写死期望值 | 会。这是唯一能拦住回归的 |
| `verify-scopes.js` | **观察器**，打印所有 token 的叶子 scope | 不会。输出几百行，靠人眼看 |
| `probe-samples.js` | 样例行扫描 + 死 token 反向断言 | 会（只对死 token） |

`check-grammar.js` 是这次补的，原因是前两个都「看着对」地漏过了两个真 bug：`#builtins` 从未生效、接口无体方法吞行。**观察器不会失败，所以它不阻止回归。**

`verify-scopes.js` 末尾会打印 `FALLBACK to bare source.x: N`。**这个数必须是 0** —— 非 0 说明有 token 没被任何规则接住，落回了裸 `source.x`，换主题就会变色。

`probe-samples.js` 覆盖了一组最小样例（每种字面量、每种运算符、每个关键字、中英对照）。改完语法跑一遍，扫一眼输出里有没有「本该有独立 scope 却是父级 scope」或「本该是关键字却是 variable.other」的。

## 折叠为什么用 markers 不用 indentation

X 的大括号是显式块边界。按缩进折叠，一旦出现大括号风格不一致（比如单行 `fn f() { x }`），折叠范围就会错。所以 `folding` 只认 `// region` / `// endregion` 标记，不做缩进推断。

## 开发与安装

完整步骤（含 `x.exe` 放在哪、异地分发、四层排查）见仓库里的
**`docs/打包与异地使用.md`**。这里只列最常用的三条：

```bash
# 方式一：开发模式，改完 Reload Window 即生效
code --extensionDevelopmentPath="F:\项目\x-project\x-lang\vscode-x"

# 方式二：验证 + 打包成 vsix（会先跑两套协议验证）
cd x-lang/vscode-x && sh tools/package.sh

# 方式三：装别人给的 vsix
code --install-extension x-lang-0.3.0.vsix
```

确认装上：`code --list-extensions | grep x-lang` 应输出 `x-lang.x-lang`。

**装完要重载窗口**（`Ctrl+Shift+P` → `Developer: Reload Window`）—— `activate` 只在窗口加载时跑一次。

### 验证高亮

打开 `tests/p0/08-zh.xlc`，`Ctrl+Shift+P` → `Developer: Inspect Editor Tokens and Scopes`，鼠标点到 `函` 上，应显示 `storage.type.function.x`。

### 验证代码提示

在任意 `.xle` 里敲 `pri` → `Ctrl+Space`，应出现 `println`。没出现就看日志：

`Ctrl+Shift+P` → `X: 查看语言服务日志`。日志里应有：

```
启动：F:/.../x.exe lsp
服务端就绪：{"name":"x-lang-lsp","version":"0.1.0"}
```

只有第一行没有第二行 = 服务端起来了但握手没完成。两行都没有 = 找不到 `x.exe`，设 `xlang.executablePath`。