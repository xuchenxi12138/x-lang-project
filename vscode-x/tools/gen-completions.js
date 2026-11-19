// tools/gen-completions.js — 从 C 源码提取补全词表
//
// ── 为什么是「提取」而不是「写一份清单」 ────────────────────
//
// 补全列表如果手写，它会和编译器分叉：C 里加了内建、语言里删了
// 关键字，补全还在推荐旧名字。用户照着补全写代码，编译不过 ——
// 这比没有补全更糟，因为它主动误导。
//
// 所以这份表的唯一来源是：
//   src/lexer.c     kKeywords[] / kZhAliases[] / kZhTypeAliases[]
//   src/bytecode.c  g_builtins[]
//
// 提取而不是手抄，还有一个现实好处：编译器那边加了新内建，重跑
// 一次这个脚本就同步了，不需要有人记得来改这里。
//
// 用法：
//   node tools/gen-completions.js            # 打印统计
//   node tools/gen-completions.js --write     # 写入 ../src/lsp_data.h
//
// 输出的是 C 头文件而不是 JSON：词表要编进 x.exe（LSP 服务端由
// x.exe 自己当）。让 x.exe 运行时去读一个 JSON 会引入「文件找不到
// 就静默没有补全」的新失败模式 —— 编译进去就没有这个问题。

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const LEXER = path.join(ROOT, '..', 'src', 'lexer.c');
const BYTECODE = path.join(ROOT, '..', 'src', 'bytecode.c');

// ---------- 读文件 ----------
function read(p, what) {
  if (!fs.existsSync(p)) {
    console.error(`FAIL: 找不到 ${what}：${p}`);
    process.exit(1);
  }
  return fs.readFileSync(p, 'utf8');
}

// ---------- 切出一段 { ... } 里的内容 ----------
//
// 用括号配对而不是正则：数组里可能有注释、嵌套的 {}、
// 字符串里的括号。手写配对扫描比调正则可靠。
function slice_block(src, startIdx) {
  // 从 startIdx 往后找第一个 '{'
  let i = src.indexOf('{', startIdx);
  if (i < 0) return null;
  let depth = 0;
  const begin = i;
  let inStr = false, inChr = false, inLine = false, inBlock = false;
  for (; i < src.length; i++) {
    const c = src[i], n = src[i + 1];
    if (inLine) { if (c === '\n') inLine = false; continue; }
    if (inBlock) { if (c === '*' && n === '/') { inBlock = false; i++; } continue; }
    if (inStr)  { if (c === '\\') { i++; } else if (c === '"') inStr = false; continue; }
    if (inChr)  { if (c === '\\') { i++; } else if (c === "'") inChr = false; continue; }
    if (c === '/' && n === '/') { inLine = true; i++; continue; }
    if (c === '/' && n === '*') { inBlock = true; i++; continue; }
    if (c === '"') { inStr = true; continue; }
    if (c === "'") { inChr = true; continue; }
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return src.slice(begin, i + 1); }
  }
  return null;
}

// ---------- 从数组块里抽出所有顶层 {} 项 ----------
//
// block 形如：{ {"as",TK_AS}, {"break",TK_BREAK}, ... }
// 最外层的 {} 是数组本身，要跳过它；它内部的每个 {} 是一项。
//
// ── 这里踩过一个坑 ──────────────────────────────────────
//
// 第一版写成「遇到 '{' 就 depth++，depth===1 时开始收集」，
// 结果最外层那个 '{' 让 depth 变成 1，然后第一项自己的 '{' 让
// depth 变成 2 —— 于是收集条件永远不满足，或者把整个数组当成
// 一项。表现是「提取出 1 个关键字」。
//
// 正确做法是：先把最外层括号剥掉，再在剩下的内容里按 depth===1
// 收集。剥括号这一步不能省。
function items(block) {
  const out = [];
  // 剥掉最外层 { }
  let s = block.trim();
  if (s.startsWith('{')) s = s.slice(1);
  if (s.endsWith('}')) s = s.slice(0, -1);

  let depth = 0, cur = '';
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === '{') {
      depth++;
      if (depth === 1) { cur = ''; continue; }
    }
    if (c === '}') {
      depth--;
      if (depth === 0) { out.push(cur); continue; }
    }
    if (depth >= 1) cur += c;
  }
  return out;
}

// 从一项里取所有双引号字符串
function strs(s) {
  const r = [];
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let m;
  while ((m = re.exec(s)) !== null) r.push(m[1]);
  return r;
}

// ============================================================
// 1. 关键字（英文）
// ============================================================
const lexerSrc = read(LEXER, 'lexer.c');

const kwIdx = lexerSrc.indexOf('kKeywords[]');
const kwBlock = slice_block(lexerSrc, kwIdx);
if (!kwBlock) { console.error('FAIL: 切不出 kKeywords'); process.exit(1); }

const keywords = items(kwBlock)
  .map(it => strs(it)[0])
  .filter(Boolean);

// ── 死 token 黑名单：存在于词法表，但语法层不认 ────────────────
//
// 这是这份生成器里唯一一处「人工维护」的清单，所以要讲清楚为什么
// 它必须存在，以及为什么放在这里而不是 lsp.c。
//
// ── 问题 ───────────────────────────────────────────────────
//
// kKeywords 是词法层的表：它只管「这个拼写是不是关键字」，不管
// 「这个关键字能不能被用」。P0 阶段有大量 token 已经登记进词法表
// （设计文档里有、TokKind 枚举里有），但 parser.c 一次都没读过。
// 后果是 `enum Color { Red }` 报「期望一个表达式，实际读到 enum」——
// 词法认成关键字，语法完全不支持。
//
// 补全如果照 kKeywords 全量表吐，就会主动推荐这 20 个词，用户照着
// 写完必然编译失败。这正是这个脚本开头说的「比没有补全更糟」。
//
// ── 判据 ───────────────────────────────────────────────────
//
// 双重验证，两者必须都给 DEAD：
//   1. 静态：grep -w TK_XXX src/*.c，只命中 lexer.c 和 x.h（即
//      词法登记了、语法没消费）
//   2. 实测：写一段最小可编译用例交给 build/x.exe check，退出码非 0
//      且报「期望一个表达式，实际读到 XXX」（或参数列表处的同类报错）
//
// 实测过一遍的结论（v0.16，逐个用例见 tools/probe-dead-keywords.sh）：
//
//   enum trait impl type mod match do catch spawn dyn where unsafe
//   comptime extern case yield mut ref par super
//
// 其中 super 特别值得记一笔：kZhAliases 里注册了 `父类`，TokKind 也有
// TK_SUPER，看起来是最「像实现了」的一个 —— 但 `super()` 报「期望一个
// 表达式，实际读到 super」，语法层没有超级调用。中文别名注册不等于
// 语法实现，这两件事在 lexer.c 里挨着写，很容易看错。
//
// ── 为什么不在 lsp.c 里过滤 ────────────────────────────────
//
// lsp.c 是消费方，它拿到的是数据。死 token 是**数据源的边界知识**
// （词法表比语法能力宽），属于生成阶段该收敛掉的信息。在 lsp.c 里
// 过滤会让「词表里有但补全时消失」这件事散落在消费端，且高亮那边
// （x.tmLanguage.json）也要同样的清单 —— 一份结论两处维护就会分叉。
//
// 生成器是唯一同时知道「词法认什么」和「语法能算什么」的地方。
// 死 token：词法器能识别，但 parser.c 不消费，写出来必然报错。
//
// ── match 为什么不在里面（v0.17）──────────────────────────
//
// 它以前在这里。EX_MATCH 实现之后 parser 已消费 TK_MATCH，
// `match x { 1 => "a", _ => "b" }` 实测可编过（tools/probe-dead-keywords.sh
// 的退出码为 0）。留在黑名单里的后果是「语法支持但补全不给」，
// 用户写得出、提示不出来 —— 比不支持更让人困惑。
//
// 判断某词该不该在这里，唯一判据是实测能不能编过，不是「有没有实现」。
const DEAD_KEYWORDS = new Set([
  'enum', 'trait', 'impl', 'type', 'mod', 'do', 'catch',
  'spawn', 'dyn', 'where', 'unsafe', 'comptime', 'extern', 'case',
  'yield', 'mut', 'ref', 'par', 'super',
]);

// 中文别名侧按 TokKind 对齐：同一个 TokKind 既然是死的，它的中文
// 别名也一定是死的。这里不按词形写死，而是先查英文那一份的
// TokKind，再从中文表里剔 —— 将来中文别名改名，这里不用跟着改。
//
// 注意 `实现`(TK_IMPL) 与 `实现接口`(TK_IMPLEMENTS) 是前缀关系，
// 但两者 TokKind 不同，所以只剔 `实现`、保留 `实现接口`。这正是
// 「按 TokKind 剔」而不是「按字符串剔」的价值。
//
// ── 正则不能带结尾的 `}` ────────────────────────────────────
//
// 第一版写的是 /,\s*(TK_[A-Z_]+)\s*\}/，要求 TK_ 后面紧跟 `}`。
// 结果 deadKinds 恒为空，中文侧 56 进 56 出、一个都没剔掉，而脚本
// 一声不响 —— 因为 items() 在剥最外层大括号时，把每一项自己的 `}`
// 也一起剥了（见 items() 里 "if (s.endsWith('}')) s = s.slice(0,-1)"）。
// items() 返回的是 {"as",TK_AS} 里面那层 `"as",TK_AS`，没有收尾括号。
//
// 症状的隐蔽之处：英文侧照常正确（那边按词形过滤，不依赖正则），
// 只有中文侧静默失效。所以只要剔 TokKind，就必须同时断言剔到了东西。
const deadKinds = new Set();
for (const it of items(kwBlock)) {
  const s = strs(it);
  if (s.length >= 1 && DEAD_KEYWORDS.has(s[0])) {
    const m = it.match(/,\s*(TK_[A-Z_]+)\s*$/);
    if (m) deadKinds.add(m[1]);
  }
}
if (deadKinds.size !== DEAD_KEYWORDS.size) {
  console.error(`FAIL: 只从关键字表里取到 ${deadKinds.size} 个 TokKind，应取 ${DEAD_KEYWORDS.size} 个`);
  console.error(`      取到的：${[...deadKinds].join(' ')}`);
  console.error('      items() 的返回格式变了？见上面「正则不能带结尾的 }」那段');
  process.exit(1);
}

const keywordsLive = keywords.filter(k => !DEAD_KEYWORDS.has(k));
const keywordsDead = keywords.filter(k => DEAD_KEYWORDS.has(k));
if (keywordsDead.length !== DEAD_KEYWORDS.size) {
  // 黑名单里有的词在词法表里找不到 —— 说明黑名单过期了（编译器
  // 删了 token，或者拼写改了）。这不会造成错误补全，但会让这份
  // 清单慢慢变成谎言，所以直接报出来。
  const missing = [...DEAD_KEYWORDS].filter(k => !keywords.includes(k));
  console.error(`FAIL: 黑名单里的词不在 kKeywords 里：${missing.join(', ')}`);
  console.error('      编译器删了这些 token？同步删掉 DEAD_KEYWORDS 里的条目');
  process.exit(1);
}

// ============================================================
// 2. 中文别名
// ============================================================
const zhIdx = lexerSrc.indexOf('kZhAliases[]');
const zhBlock = slice_block(lexerSrc, zhIdx);
if (!zhBlock) { console.error('FAIL: 切不出 kZhAliases'); process.exit(1); }

// 中文别名一行写好几个（{"函", TK_FN}, {"令", TK_LET}），
// items() 按括号层级切，能正确处理。
const zhKeywordsAll = items(zhBlock)
  .map(it => strs(it)[0])
  .filter(Boolean);

// 死 token 的中文别名一起剔（判据见上面 DEAD_KEYWORDS 那段）。
//
// 这里必须按 TokKind 剔、不能按词形剔：英文 `impl` 死了，它的中文
// 是 `实现`；而 `实现接口` 对应的是 TK_IMPLEMENTS（活的）。两个词
// 长得像前缀，按字符串剔会把活的也剔掉。
const zhKeywords = items(zhBlock)
  .filter(it => {
    const m = it.match(/,\s*(TK_[A-Z_]+)\s*$/);
    return !(m && deadKinds.has(m[1]));
  })
  .map(it => strs(it)[0])
  .filter(Boolean);

// 剔完必须有变化，否则就是静默失效（第一版正是这样：56 进 56 出，
// 中文补全会推荐 `枚举` `特征` `匹配` 这些写不出来的词）。
if (zhKeywords.length === zhKeywordsAll.length) {
  console.error('FAIL: 中文别名一个都没剔掉 —— TokKind 提取没生效');
  console.error('      预期至少剔掉 枚举/特征/实现/模组/匹配/情形/做/捕获/派生/动态…');
  process.exit(1);
}

// ============================================================
// 3. 中文类型别名 → 英文
// ============================================================
const ztIdx = lexerSrc.indexOf('kZhTypeAliases[]');
const ztBlock = slice_block(lexerSrc, ztIdx);
if (!ztBlock) { console.error('FAIL: 切不出 kZhTypeAliases'); process.exit(1); }

const zhTypes = items(ztBlock)
  .map(it => {
    const s = strs(it);
    return s.length >= 2 ? { zh: s[0], en: s[1] } : null;
  })
  .filter(Boolean);

// ============================================================
// 4. 内建函数
// ============================================================
const bcSrc = read(BYTECODE, 'bytecode.c');
const biIdx = bcSrc.indexOf('g_builtins[]');
const biBlock = slice_block(bcSrc, biIdx);
if (!biBlock) { console.error('FAIL: 切不出 g_builtins'); process.exit(1); }

// 每项形如： "winOpen", XBI_WIN_OPEN, 3, "Bool"
// 第一个字符串是名字，最后一个字符串是返回类型，倒数第二个是 argc。
const builtins = items(biBlock).map(it => {
  const s = strs(it);
  if (s.length < 2) return null;
  // 倒数： ret 是最后一个字符串；argc 在 ret 之前
  const ret = s[s.length - 1];
  const name = s[0];
  // argc：在倒数第二个字符串之后、最后一个字符串之前的整数
  let argc = null;
  const tail = it.slice(it.lastIndexOf('"' + ret + '"') >= 0
    ? it.lastIndexOf('"' + ret + '"') : 0);
  // 换个更稳的办法：按逗号分段，第二段是枚举，第三段是 argc
  const parts = it.split(',').map(x => x.trim());
  if (parts.length >= 3) {
    const m = parts[2].match(/^-?\d+$/);
    if (m) argc = parseInt(m[0], 10);
  }
  return { name, argc, ret };
}).filter(Boolean);

// 去重（g_builtins 里不应有重名，但真有了要先发现）
const seen = new Set();
for (const b of builtins) {
  if (seen.has(b.name)) console.error(`WARN: 内建重名 ${b.name}`);
  seen.add(b.name);
}

// ── 不在 g_builtins[] 里的内建：print / println 等特殊形式 ─────
//
// 它们是编译器里的特殊形式，不走内建函数表 —— 因为参数个数可变、
// 要做隐式转字符串，直接编成 OP_PRINT 更省事。后果是任何「从
// g_builtins 提取内建」的工具都会漏掉它们。
//
// 这里从 compile.c 实际认的名字里取，而不是再硬编码一遍：将来
// 编译器改名或加别名，生成器会自动跟上。
//
// 坑：同一个函数里（ty_from_name 附近）也在用 strcmp(n, "Int")
// 判断类型名。所以必须先把类型名提出来，再从「特殊形式」里剔掉，
// 否则补全会推荐 `Int(` 这种根本不是函数的东西。
const compileSrc = read(path.join(ROOT, '..', 'src', 'compile.c'), 'compile.c');

// 先取类型名（ty_from_name 的实际实现）
//
// ── indexOf 找到的是注释里的名字，不是定义 ──────────────────
//
// 第一版写 `compileSrc.indexOf('ty_from_name')`，它命中 compile.c 第 67 行
// 的**注释**（「…而不是把 Bytecode 一路传进来：ty_from_name」）。从那里
// 往后找第一个 `{`，抓到的是隔了几十行的 drive 结构体定义，于是正则一条
// 都没匹配上，类型名恒为 0 个。
//
// 这个 bug 的症状极隐蔽：脚本不报错、关键字和内建都对，只有
// 「类型名 0 」那一行看着有点怪 —— 而它导致的后果是 `Int` 这类类型名
// 没被登记，后面「剔除类型名」那一步于是不生效，补全会推荐 `Int(`。
//
// 修法：用**函数定义的形态**定位，不用裸名字。`static TyKind ty_from_name(`
// 只在定义处出现一次。
function findFnBody(src, signature) {
  const i = src.indexOf(signature);
  return i < 0 ? null : slice_block(src, i);
}

const tyNames = [];
{
  const body = findFnBody(compileSrc, 'static TyKind ty_from_name(');
  if (!body) {
    console.error('FAIL: 切不出 ty_from_name 的函数体 —— 函数签名变了？');
    process.exit(1);
  }
  const re = /strcmp\(\s*n\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"/g;
  let m;
  while ((m = re.exec(body)) !== null) tyNames.push(m[1]);
}
const tySet = new Set(tyNames);

// 再取特殊形式，剔掉类型名
{
  const re = /strcmp\(\s*n\s*,\s*"(\w+)"\)\s*==\s*0/g;
  let m;
  const extra = [];
  while ((m = re.exec(compileSrc)) !== null) {
    const nm = m[1];
    if (seen.has(nm)) continue;
    if (tySet.has(nm)) continue;   // 是类型，不是函数
    extra.push(nm);
  }
  for (const nm of [...new Set(extra)]) {
    if (seen.has(nm)) continue;
    // 参数个数标 -1（可变），返回 Unknown（toInt 之类各自不同，
    // 悬停里再按名字给准确签名）
    builtins.push({ name: nm, argc: -1, ret: 'Unknown' });
    seen.add(nm);
  }
  if (extra.length) {
    console.log(`  编译器特殊形式（不在 g_builtins 里）：${[...new Set(extra)].join(', ')}`);
  }
}

// ---------- 统计 ----------
console.log('从实现提取（唯一真相来源）');
console.log(`  英文关键字      ${keywordsLive.length}  （词法表 ${keywords.length}，剔除死 token ${keywordsDead.length}）`);
console.log(`  中文关键字别名  ${zhKeywords.length}  （词法表 ${zhKeywordsAll.length}）`);
console.log(`  中文类型别名    ${zhTypes.length}`);
console.log(`  内建函数        ${builtins.length}`);
console.log(`  类型名          ${tyNames.length}  ${tyNames.join(' ')}`);
console.log(`  已剔除死 token  ${keywordsDead.join(' ')}`);

// 阈值要跟着「剔除后」的数量走。
//
// 第一版中文阈值写的 40，是照剔除前的 56 定的。剔除死 token 之后
// 只剩 36，阈值立刻变成假警报 —— 而假警报的下场是人开始习惯性
// 忽略 FAIL。阈值应当定在「比当前实测略低一点」，作用是发现提取
// 规则整体失效（比如切不出数组、正则全不匹配），不是发现能力增长。
if (keywordsLive.length < 30) {
  console.error('FAIL: 可用关键字少于 30 个，提取规则可能失效了');
  process.exit(1);
}
if (builtins.length < 50) {
  console.error('FAIL: 内建少于 50 个，提取规则可能失效了');
  process.exit(1);
}
if (zhKeywords.length < 30) {
  console.error('FAIL: 中文别名少于 30 个，提取规则可能失效了');
  process.exit(1);
}

// ── 最少数量守卫的边界在哪 ──────────────────────────────────
//
// 上面那几个「少于 N 个就失败」只能发现「提取规则整体失效」，
// 发现不了「某一类特性漏提」。实测过一次：类系统实现后关键字
// 从 45 涨到 57、内建从 84 涨到 96，但 lsp_data.h 是旧的 ——
// 脚本照样全过，因为 57 > 40、96 > 50。补全里一个类关键字都没有，
// 而生成器说「成功」。
//
// 所以再加一层**具名断言**：关键能力的关键词必须存在。这些是
// 「一旦实现就必须出现在补全里」的词，漏了就是生成器或数据源
// 出了问题，而不是「数量还没到」。
const mustHaveKeyword = [
  'class', 'interface', 'extends', 'implements', 'abstract',
  'override', 'private', 'protected', 'public', 'new', 'self',
  'struct', 'fn', 'let', 'if', 'for', 'return', 'import',
];
const kwSet = new Set(keywordsLive);
const missKw = mustHaveKeyword.filter(k => !kwSet.has(k));
if (missKw.length) {
  console.error(`FAIL: 关键字表缺少已实现的语法：${missKw.join(', ')}`);
  console.error('      这些词在 lexer.c 的 kKeywords 里一定有 —— 检查提取逻辑');
  process.exit(1);
}

// 反向断言：死 token 必须一个都不在补全表里。
//
// 上面那条正向断言只保证「该有的有」，这条保证「不该有的没有」。
// 少了这条，黑名单一旦因为重构失效（比如 filter 写成了 map），
// `super` 就会悄悄回到补全里，而所有正向断言依然全绿。
const leakedDead = keywordsLive.filter(k => DEAD_KEYWORDS.has(k));
if (leakedDead.length) {
  console.error(`FAIL: 死 token 漏进补全表：${leakedDead.join(', ')}`);
  process.exit(1);
}

// 反射与注解内建同理：它们是 v0.16 的新增能力，漏了就说明
// g_builtins 的提取路径断了。
const mustHaveBuiltin = [
  'className', 'classParent', 'classIs', 'classFields', 'classMethods',
  'fieldGet', 'fieldSet', 'fieldHas', 'methodHas',
  'annoHas', 'annoGet', 'annoGetKV', 'annoAll',
];
const biSet = new Set(builtins.map(b => b.name));
const missBi = mustHaveBuiltin.filter(k => !biSet.has(k));
if (missBi.length) {
  console.error(`FAIL: 内建表缺少已实现的能力：${missBi.join(', ')}`);
  console.error('      这些在 bytecode.c 的 g_builtins 里一定有 —— 检查提取逻辑');
  process.exit(1);
}

// 中文侧：类系统的中文别名必须齐（否则 .xlc 用户补全不到）
const mustHaveZh = ['类', '接口', '抽象', '继承', '私有', '保护', '公开', '新建'];
const zhSet = new Set(zhKeywords);
const missZh = mustHaveZh.filter(k => !zhSet.has(k));
if (missZh.length) {
  console.error(`FAIL: 中文别名表缺少类系统关键字：${missZh.join(', ')}`);
  process.exit(1);
}

// ---------- 生成 ----------
if (process.argv.includes('--write')) {
  const esc = s => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');

  const L = [];
  L.push('// lsp_data.h — 补全词表（**自动生成，不要手工编辑**）');
  L.push('//');
  L.push('// 生成命令：cd vscode-x && node tools/gen-completions.js --write');
  L.push('//');
  L.push('// 唯一真相来源是编译器自己：');
  L.push('//   src/lexer.c     kKeywords / kZhAliases / kZhTypeAliases');
  L.push('//   src/bytecode.c  g_builtins');
  L.push('//   src/compile.c   ty_from_name');
  L.push('//');
  L.push('// ── 为什么编进 x.exe 而不是运行时读 JSON ────────────────');
  L.push('//');
  L.push('// LSP 服务端由 x.exe 自己当（见 src/lsp.c）。词表若放在外部');
  L.push('// 文件里，就多了一种失败模式：文件被删、被挪、版本不匹配时');
  L.push('// 补全静默为空。编进产物之后，能启动就一定有词表。');
  L.push('');
  L.push('#ifndef X_LSP_DATA_H');
  L.push('#define X_LSP_DATA_H');
  L.push('');

  function emitArr(cname, arr) {
    L.push(`static const char* const ${cname}[] = {`);
    let line = '   ';
    for (const s of arr) {
      const piece = ` "${esc(s)}",`;
      if (line.length + piece.length > 92) { L.push(line); line = '   '; }
      line += piece;
    }
    if (line.trim()) L.push(line);
    L.push('};');
    L.push(`#define ${cname}_COUNT ((int)(sizeof(${cname}) / sizeof(${cname}[0])))`);
    L.push('');
  }

  emitArr('kLspKeywords', keywordsLive);
  emitArr('kLspZhKeywords', zhKeywords);

  // 中文类型别名：成对
  L.push('static const char* const kLspZhTypeZh[] = {');
  L.push('   ' + zhTypes.map(t => ` "${esc(t.zh)}",`).join(''));
  L.push('};');
  L.push('#define kLspZhTypeZh_COUNT ((int)(sizeof(kLspZhTypeZh) / sizeof(kLspZhTypeZh[0])))');
  L.push('');
  L.push('static const char* const kLspZhTypeEn[] = {');
  L.push('   ' + zhTypes.map(t => ` "${esc(t.en)}",`).join(''));
  L.push('};');
  L.push('#define kLspZhTypeEn_COUNT ((int)(sizeof(kLspZhTypeEn) / sizeof(kLspZhTypeEn[0])))');
  L.push('');

  // 内建：名字 + argc + ret，三条平行数组
  //
  // 平行数组而不是结构体数组：这个文件是生成的，结构体数组要处理
  // 每项的字段顺序与类型，生成逻辑复杂、出错点更多。三条平行数组
  // 生成简单、审查容易，用的时候按下标访问。
  emitArr('kLspBuiltinName', builtins.map(b => b.name));
  L.push('static const int kLspBuiltinArgc[] = {');
  {
    let line = '   ';
    for (const b of builtins) {
      const piece = ` ${b.argc === null ? -2 : b.argc},`;
      if (line.length + piece.length > 92) { L.push(line); line = '   '; }
      line += piece;
    }
    if (line.trim()) L.push(line);
  }
  L.push('};');
  L.push('#define kLspBuiltinArgc_COUNT ((int)(sizeof(kLspBuiltinArgc) / sizeof(kLspBuiltinArgc[0])))');
  L.push('');
  emitArr('kLspBuiltinRet', builtins.map(b => b.ret));

  emitArr('kLspTypes', tyNames);

  L.push('#endif // X_LSP_DATA_H');
  L.push('');

  const outPath = path.join(ROOT, '..', 'src', 'lsp_data.h');
  fs.writeFileSync(outPath, L.join('\n'), 'utf8');
  console.log(`\n已写入 ${outPath}`);
} else {
  console.log('\n（只统计，没写文件。加 --write 生成 src/lsp_data.h）');
}