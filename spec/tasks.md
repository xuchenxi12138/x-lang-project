# 任务清单：显式具名导入 + 最小依赖裁剪

依据：`spec/spec.md`（待批准）。按顺序执行，每步完成后可独立编译验证。

## T1 数据模型与 parser 语法

- [x] `src/x.h`：`Module` 增加 `import_names` / `import_name_counts` / `import_targets`。
- [x] `src/x.h`：`FuncDecl`、`StructDecl`、`ClassDecl`、`Stmt` 增加 `tok_begin` / `tok_end`。
- [x] `src/parser.c`：`Parser` 增加平行的名字数组与容量。
- [x] `src/parser.c`：重写 `parse_import`，实现 `from <target> import a, b`、括号多行、尾逗号；
      裸 `import`、`import *`、`as`、空列表按 spec §2.2 报错。
- [x] `src/parser.c`：`parse()` 中为函数/结构体/类/顶层语句记录 token 跨度，并写入 `Module`。
- 验证：用临时 `C:\xtmp\t1.xle` 分别写合法与 4 种非法写法，`build/x.exe check` 输出符合 spec。

## T2 合并裁剪与依赖闭包

- [x] `src/main.c`：`ModFile` 保留 `toks`；`compilation_free` 释放。
- [x] `src/main.c`：`mod_load` 回填 `Module.import_targets`。
- [x] `src/main.c`：新增 `build_minimal_merge(ModGraph*, int entry_slot)`，实现 spec §4.2 的导出表、请求集合、不动点闭包、动态类回退、按名重建 merged。
- [x] `src/main.c`：`compile_file` 用 `build_minimal_merge` 替换 846-891 行的全量合并；顶层语句拼接改为只合并被请求的 const/let（入口全部保留）。
- 验证：V2/V3/V4/V5；`tests/p0` 抽查 10 个。

## T3 字节码可达性裁剪 + AOT

- [x] `src/compile.c`：新增 `bc_mark_reachable` / `bc_prune_unreachable`，在 `compile_to_bytecode` 末尾调用。
- [x] 重排 `funcs`、压缩 `code`/`pos`、重写 `BC_CALL.a`、`ClassType.ctor_fn`、`vtable[]`。
- [x] `src/codegen_c.c`：gcc 参数追加 `-ffunction-sections -fdata-sections -Wl,--gc-sections`。
- 验证：V6（`x disasm` 对比）、V9（AOT 差分）。

## T4 全仓迁移 import

- [x] 在 `$env:TEMP` 写一次性 `migrate_imports.ps1`（spec §6.1），扫描 `stdlib/`、`examples/`、`tests/`、`games/`、`pkg/`，排除 `build/`、`release/`。
- [x] 执行迁移，人工核对 5 条 `@pkg` import。
- [x] 删除临时脚本。
- [x] `src/main.c` `usage()` 与 `spec/03-标准库与包管理.md` 同步新语法。
- 验证：V11（0 条裸 import）。

## T5 全量构建与回归

- [x] 用 `C:\xgcc\mingw64\bin\gcc.exe` + `tools/build.sh` 的源文件/CFLAGS 直接构建，产出 `build/x.exe`。
- [x] 逐个执行 `build/x.exe run tests/p0/*.xle`，记录通过/失败。
- [x] 执行 V7（std.ui 类）、V8（classNew/methodCall）、V10（发行布局冒烟）。
- [x] 失败项按诊断定位并修复，直到 V1-V11 全绿。

## 完成标准

- `grep` 全仓 `.xle/.xlc`：无裸 `import`。
- 未声明跨文件名字无法编译。
- 只导入一个函数时，`x disasm` 不包含同模块无关函数。
- 解释档与 AOT 对同一测试输出一致。
- 标准库在发行布局下无需绝对路径即可导入。
