# 显式具名导入 + 最小依赖裁剪（Spec）

状态：待批准
日期：2026-11-14
涉及用户问题：
1. 语言安装到系统后，标准依赖怎么引入？是否能不用绝对路径？
2. 必须强制 `import` 时声明具体引入了什么，并从语言层面保证未使用的引用不占资源。

---

## 1. 结论先行

### Q1 标准库怎么引入

**已经实现，不需要新增机制，也不需要绝对路径。**

标准库不是"封装成一组全局可用的名字"，而是**编译器内置的模块路径解析**：

- 语法：`from std.json import parse, stringify`
- 解析：`resolve_std_module()` 把 `std.a.b` 映射为 `<stdlib 根>/a/b.xle`；
  `b.xle` 不存在时回退 `<stdlib 根>/a/b/index.xle`。
- 根目录查找顺序（[main.c](file:///e:/项目/x-project/x-lang/src/main.c) `find_stdlib_root`）：
  1. `X_STDLIB_PATH`
  2. `<x.exe 所在目录>/stdlib`
  3. `<x.exe 所在目录>/../lib/stdlib`（发行包形态）
  4. `<x.exe 所在目录>/lib/stdlib`
  5. `<x.exe 所在目录>/../stdlib`（源码树形态）
  6. `<cwd>/stdlib`
- 发行布局：`bin/x.exe` + `lib/stdlib/*.xle`。整包拷到任意目录都能 `from std.x import ...`。

因此用户侧永远只写逻辑模块名，绝对路径只存在于编译器内部。**本次不改这部分实现**，只把 `--help` 与 `spec/03` 中旧的 `import std.x` 示例改成新语法。

### Q2 强制具名 + 零无用开销

分两层实现：

1. **语法层强制具名**：只剩 `from <目标> import a, b` 一种导入形态。裸 `import X`、`from X import *`、`as` 别名全部报编译错误并给出迁移提示。
2. **合并层按名裁剪**：被导入文件只把"被显式声明的名字 + 该声明的文件内依赖闭包"并入编译单元；未声明的名字根本不进入字节码。
3. **字节码层可达性裁剪**：编译结束后从 `main` 做可达性分析，删除任何不可达函数并重写所有函数索引；AOT 再加 `-ffunction-sections -fdata-sections -Wl,--gc-sections`。

两层裁剪叠加后的保证：**不 import 的名字不参与编译；import 了但程序不可达的函数不进入最终字节码/可执行文件。**

---

## 2. 语法规范（决策）

### 2.1 唯一合法形态

```
import_decl := 'from' target 'import' name_list
target      := STRING                      // "board.xle"
             | IDENT ('.' IDENT)*          // board / std.json / std.db.sqlite
             | '@' IDENT                   // @util
name_list   := IDENT (',' IDENT)* ','?
             | '(' IDENT (',' IDENT)* ','? ')'   // 允许括号+换行
```

- 中文关键字自动可用：`从 std.json 引入 parse, stringify`（`从`=TK_FROM，`引入`=TK_IMPORT）。
- 名字列表**至少一个**。
- 名字必须是 `IDENT`；允许重复，记录时去重。
- 尾部逗号允许。
- 括号形态仅用于多行书写，语义与单行完全一致。

### 2.2 必须报错的旧形态

| 输入 | 诊断 |
| --- | --- |
| `import X` / `import "x.xle"` / `import @x` | error：`import` 必须写明引入哪些名字；help 给出对应的 `from X import ...` 写法 |
| `from X import *` | error：不支持通配导入，必须逐个列出名字 |
| `from X import a as b` | error：别名暂不支持（明确报错，不静默忽略） |
| `from X import`（空列表） | error：至少声明一个名字 |
| `from X` 后没有 `import` | error：期望 `import` |

所有错误都计入 `g_error_count`，编译中止（与现有诊断体系一致）。

### 2.3 语义

- `from A import f, C` 等价于：把 A 中的函数 `f`、类 `C` 及其文件内依赖闭包并入当前编译单元，名字仍是全局的（P0 文本合并语义不变）。
- 同文件内部互相调用不需要导入。
- 未声明的跨文件名字**不会被合并**，因此在使用点得到现有的"未定义函数/类型"错误。
- `import @pkg` 的清单解析逻辑不变，只是套上 `from ... import ...`。

---

## 3. 数据模型改动

### 3.1 [x.h](file:///e:/项目/x-project/x-lang/src/x.h)

`Module`（当前 845-867 行）新增：

```c
// 与 imports / import_lines 平行的具名列表。
// import_names[i][j] 是第 i 条 import 声明的第 j 个名字。
char***    import_names;
int*       import_name_counts;
// 与 imports 平行的解析后绝对路径。mod_load 解析完依赖后回填，
// 供合并阶段的闭包计算直接使用，避免二次解析（@包/标准库规则不同）。
char**     import_targets;
```

`FuncDecl` / `StructDecl` / `ClassDecl` / `Stmt` 各新增两个 int：

```c
int tok_begin;   // 声明首 token 在文件 token 数组中的下标
int tok_end;     // 声明末 token 的下标（含）
```

用途：合并阶段按声明扫描 token 计算依赖闭包。`tok_end < tok_begin` 表示"无跨度"，调用方跳过。

### 3.2 [parser.c](file:///e:/项目/x-project/x-lang/src/parser.c)

- `Parser` 增加与 `imports` / `import_lines` / `import_cap` 平行的：
  `char*** import_names; int* import_name_counts;`
  扩容必须与现有平行数组**同一次判断、同一次分配**（现有注释已记录过一次段错误教训）。
- 重写 `parse_import`（2906-3073）：
  - 进入时记录首 token 是否为 `TK_FROM`。
  - `TK_IMPORT` 开头：仍解析目标以便恢复，但立即 `diag_error`；记录空名字列表，不参与合并。
  - `TK_FROM`：解析目标 → 必须 `TK_IMPORT` → 解析名字列表（支持括号、逗号、尾逗号）。
  - `as` / `*`：报错并继续消费到本条语句结束（只消费到本行语义边界，不吞下一条语句）。
  - 记录路径、行号、名字列表。
- 在 `parse()` 中，函数/结构体/类/顶层语句的入口处记录 `p.pos` 作为 `tok_begin`，解析完成后记录 `p.pos - 1` 作为 `tok_end`。
- `parse()` 末尾把名字数组转移到 `Module`。

---

## 4. 合并裁剪与依赖闭包（[main.c](file:///e:/项目/x-project/x-lang/src/main.c)）

### 4.1 保留 token

- `ModFile` 增加 `Token* toks; size_t tok_count;`。
- `mod_load` 不再在合并后 `free(toks)`；token 数组随 `Compilation` 生命周期存活，`compilation_free` 统一释放。
- `mod_load` 在解析每个依赖后，把已解析的绝对路径写入 `m->import_targets[i]`（arena 字符串），并保留现有错误诊断。

### 4.2 新增流程 `build_minimal_merge(ModGraph* g, int entry_slot)`

在 `mod_load` 返回后、拼接顶层语句前调用，替代当前 846-891 行的全量合并。

1. **建立导出表**
   每个文件按名字索引：`fn` 名、`struct` 名、`class`/`interface` 名、顶层 `const`/`let` 名。
   重名保持现状（交给编译期重定义诊断），这里只取第一个用于闭包。
2. **请求集合**
   `requested[slot]` 为字符串集合：
   - 入口文件：自身全部导出名。
   - 每个文件 F 的每条 import：把 `import_names[i]` 里的每个名字加入 `requested[target_slot]`。
   - 若目标文件没有该导出名：error 在 `(F->file, import_lines[i])`，
     `"模块 `<target>` 没有导出 `X`"`，help `"检查拼写，或改成导入该模块实际导出的名字"`。
3. **不动点闭包**
   反复扫描直到没有新增：
   ```
   for F in files:
     for N in requested[F]:
       E = exports[F][N]
       for t in toks[E.tok_begin .. E.tok_end]:
          if t.kind == TK_IDENT:
             if exports[F] 含 t.text:      requested[F].add(t.text)
             if F.import_map 含 t.text:    requested[target].add(t.text)
   ```
   - `import_map` 由 F 的 import 声明构建（名字 → 目标 slot）。
   - 入口文件的**全部顶层语句**作为额外根，逐个按 `tok_begin..tok_end` 扫描。
   - 类声明跨度覆盖整个类体（所有方法自动保留）。
4. **动态类回退**
   闭包期间若任一被保留 token 等于 `classNew` 或 `methodCall`（`BC_NEW_CLASS_NAME` / `BC_CALL_NAME` 的来源），
   置 `dynamic_classes = true`；此时把所有文件的全部导出类加入请求集合再跑一次闭包。
   理由：类名可能来自运行期字符串，静态无法证明未使用。
5. **重建 merged**
   - 函数/结构体/类：只并入 `requested[自己所在文件]` 中的名字。
   - 顶层 `const`/`let`：非入口文件只并入被请求的名字；入口文件全部保留（入口顶层语句就是 main）。
   - 顺序保持现有后序入槽顺序，保证常量初始化顺序不变。

### 4.3 复杂度

每个文件的 token 数 × 被请求名字数，最坏 O(tokens × names)，文件规模在数千 token 量级；不动点最多迭代"标称名字总数"轮。实现时用线性数组 + 线性查找即可，不做哈希表（与仓库现有风格一致）。

---

## 5. 字节码可达性裁剪

### 5.1 新增 `static void bc_prune_unreachable(Bytecode* bc)`（compile.c）

在 `compile_to_bytecode()` 末尾（合成 main 之后、返回之前）调用。

1. 找到 `main` 的 `bc_index` 作为唯一根（用户 main 或合成 main）。
2. 工作队列遍历指令：
   - `BC_CALL`：`a` 是函数索引 → 标记。
   - `BC_NEW_CLASS`：`a` 是类索引 → 标记 `ctor_fn`、`vtable` 中所有 `>= 0` 的索引，并递归父类。
   - `BC_NEW_CLASS_NAME` / `BC_CALL_NAME`：置 `dynamic_classes = true`。
   - 其余指令不携带函数索引（实现前用 `bytecode.c` 的 `bc_disasm` 分支再核对一遍；已知只有 `BC_CALL` 带函数下标）。
3. 若 `dynamic_classes`：把所有类的 `ctor_fn` 与 `vtable` 全部标为可达（保守，不裁剪类方法）。
4. 压缩：
   - 建立 `old_to_new[func_count]`（不可达为 -1）。
   - 按原顺序重排 `funcs`；顺序保持原相对顺序，便于差分与调试。
   - 重新拼接 `code` / `pos`：逐函数复制 `code_len` 条，更新 `code_start`。
   - 重写指令中的 `BC_CALL.a`。
   - 重写 `ClassType.ctor_fn` 与 `vtable[]`（不可达填 -1）。
   - `consts` 本次不压缩（体积小、且被跨函数共享），在注释中标注为非目标。

### 5.2 AOT

[codegen_c.c](file:///e:/项目/x-project/x-lang/src/codegen_c.c) 的 gcc 参数（4408 起）追加：

```
-ffunction-sections -fdata-sections
-Wl,--gc-sections
```

保证生成的 `static` 函数即使被保留在 C 源码里，链接期也会被丢弃。

### 5.3 .xbc 版本

裁剪不改变磁盘格式与指令语义，**不提升 `XBC_VERSION`**。

---

## 6. 迁移全部现有 import

规模：229 个文件、701 条 import。策略：**行为等价迁移**——把每个裸 import 改成导入提供方当前全部顶层导出名，保证迁移后程序与迁移前功能一致；随后由字节码裁剪消除未调用函数。

### 6.1 迁移脚本（临时，不进入仓库）

位置：`$env:TEMP\migrate_imports.ps1`，执行完删除。

1. 枚举 `x-lang` 下全部 `.xle` / `.xlc`，排除 `build/`、`release/`。
2. 构建 `路径 → 导出名[]`：
   - 行首（第 0 列）匹配：
     - `^(pub\s+)?fn\s+NAME` / `^函\s+NAME`
     - `^(pub\s+)?(abstract\s+|final\s+)?(class|interface)\s+NAME` / `(抽象\s*)?类\s+NAME` / `接口\s+NAME`
     - `^(pub\s+)?struct\s+NAME` / `^结构\s+NAME`
     - `^(pub\s+)?(const|let)\s+NAME` / `^(常|令)\s+NAME`
   - NAME 允许 Unicode 标识符字符。
3. 解析目标到实际文件：
   - `std.a.b`：`stdlib/a/b.xle`，否则 `stdlib/a/b/index.xle`。
   - 字符串/裸标识符：相对当前文件解析。
   - `@name`：向上找 `x.toml`，取 `[dependencies] name` 的 `path`，再用 `<path>/src/lib.xle`。
4. 若提供方无自有导出（纯转发入口），递归合并其 import 目标的自有导出。
5. 生成 `from <原目标写法> import <导出名逗号列表>`，已以 `from` 开头的行跳过（幂等）。
6. 输出未解析清单；`@pkg` 的 5 条人工核对。

### 6.2 文档同步

- [main.c](file:///e:/项目/x-project/x-lang/src/main.c) `usage()` 中 `import @mathutil` 示例改为新语法。
- `spec/03-标准库与包管理.md` 中 import 语法示例同步。
- 其余历史文档不在本次范围（记录为非目标）。

---

## 7. 验证矩阵

| # | 场景 | 期望 |
| --- | --- | --- |
| V1 | `build/x.exe check` 编译现有 `tests/p0/*.xle` 全部 | 全部通过，无新增诊断 |
| V2 | 旧语法 `import std.math` | 报错并提示 `from std.math import ...` |
| V3 | `from std.math import 不存在` | 报"模块没有导出"，位置指向 import 行 |
| V4 | `from std.math import abs` 后调用 `sqrt` | 未定义函数错误（证明未声明不合并） |
| V5 | `from std.math import abs` 后调用 `abs` | 正常编译运行 |
| V6 | `from std.math import abs` 的 `x disasm` | 输出中不出现 `math.xle` 的其它未调用函数（如 `sqrt`），证明 DCE 生效 |
| V7 | 导入路径中含类的模块（如 `std.ui` 组件） | 类、方法、构造、虚调用正常 |
| V8 | 使用 `classNew` / `methodCall` 的程序 | 仍能按字符串实例化，证明动态回退生效 |
| V9 | `x build --emit=exe` 冒烟 | 生成的 exe 运行输出与解释档一致 |
| V10 | 临时发行布局 `bin/x.exe` + `lib/stdlib` | `from std.math import abs` 可运行，证明 Q1 |
| V11 | 全仓迁移后 `grep -rn "^\s*import\b"`（xle/xlc） | 0 条裸 import |

构建命令：`C:\xgcc\mingw64\bin\gcc.exe` + `tools/build.sh` 的源文件清单与 CFLAGS（无 sh，用 PowerShell 直接调用），输出 `build/x.exe`。

---

## 8. 风险与非目标

| 风险 | 处置 |
| --- | --- |
| token 跨度法把局部变量名误判为依赖 | 只会多保留名字，不改变语义；不影响正确性 |
| 动态类名（字符串）导致类被裁掉 | `classNew`/`methodCall` 出现时全量保留类方法 |
| 迁移漏掉某个导出导致运行期缺符号 | 编译期"未定义"立即暴露；迁移后全量跑 tests/p0 |
| 顶层 const 初始化顺序 | merged 顺序沿用后序入槽，不变 |

非目标：
- 不做命名空间/别名（`as` 明确报错）。
- 不做常量池压缩。
- 不迁移历史文档中的旧示例（只改 `--help` 与 spec/03）。
- 不改标准库路径解析算法。

---

## 9. 需要批准的决定

1. **废弃裸 `import`，只保留 `from ... import a, b`**（旧代码全部机械迁移）。
2. **`as` 别名直接报错**（当前是警告且不生效）。
3. **`from X import *` 不支持**，必须逐个列名。
4. **字节码 DCE 遇到 `classNew`/`methodCall` 时保守保留全部类方法**。
5. **迁移采用"导入提供方全部导出名"的等价迁移**，不逐文件人工精简。
