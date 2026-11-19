// 扩展清单自检。用 VSCode 的校验规则，在部署前把「会被静默拒绝加载」的错误挡住。
//
// 为什么需要这个：package.json 写错时 VSCode **不会崩溃**，它只是把扩展标记为
// 无效、整个跳过 —— 于是语言不注册、语法不高亮、所有 contributes 全部无声失效。
// 表面现象是「装了但没用」，很容易误判成语法文件写错了，去改 x.tmLanguage.json，
// 越改越偏。实际经验：曾经因为这个白查了两小时。
//
// 只覆盖纯声明式扩展会踩的坑，不是完整 schema 实现。
// 用法：node tools/check-manifest.js
const fs = require('fs');
const path = require('path');

const EXT = path.resolve(__dirname, '..');
const errors = [];
const warns = [];

function err(m) { errors.push(m); }
function warn(m) { warns.push(m); }

// ---------- 1. 读取与编码 ----------
const pkgPath = path.join(EXT, 'package.json');
if (!fs.existsSync(pkgPath)) {
  console.error('FAIL: 找不到 package.json');
  process.exit(1);
}
const raw = fs.readFileSync(pkgPath);

// BOM 会让 JSON.parse 在某些工具链里失败，vsce 也抱怨
if (raw[0] === 0xef && raw[1] === 0xbb && raw[2] === 0xbf) {
  err('package.json 带 UTF-8 BOM，去掉它');
}
if (raw.includes(0x00)) err('package.json 含 NUL 字节，文件已损坏');

let pkg;
try {
  pkg = JSON.parse(raw.toString('utf8').replace(/^\uFEFF/, ''));
} catch (e) {
  console.error('FAIL: package.json 不是合法 JSON —', e.message);
  console.error('  提示：JSON 不允许尾随逗号、不允许 // 注释。');
  process.exit(1);
}

// ---------- 2. main 与 activationEvents 必须同时有或同时没有 ----------
//
// 这条是 VSCode 的硬校验，两个方向都会让扩展彻底不工作：
//
//   · 有 main 但没 activationEvents
//       → 扩展永不激活。0.1.0 时期（纯声明式）这是对的，0.2.0 加了
//         语言服务客户端之后就是致命错误：语言服务永远不会启动，
//         界面上只有高亮能用，代码提示全无且无任何报错。
//
//   · 没 main 但有 activationEvents
//       → VSCode 直接报 error 并把整个扩展跳过，语言都不注册。
//
// 两个方向的共同点是**都静默**。所以这里必须两边都检查，不能只挡一边。
const hasMain = typeof pkg.main === 'string' && pkg.main.length > 0;
const hasBrowser = typeof pkg.browser === 'string' && pkg.browser.length > 0;
const hasActs = Array.isArray(pkg.activationEvents) && pkg.activationEvents.length > 0;

if (!hasMain && !hasBrowser && 'activationEvents' in pkg) {
  err(
    '没有 main / browser 字段，却写了 activationEvents。\n' +
    '    VSCode 会报「如果扩展没有 main 或 browser 属性，则应省略属性 activationEvents」\n' +
    '    并把整个扩展跳过 —— 语言不注册、语法不高亮。纯声明式扩展请整段删除该字段。'
  );
}

if ((hasMain || hasBrowser) && !hasActs) {
  err(
    '有 main 字段却没有 activationEvents。\n' +
    '    扩展永远不会被激活 —— main 里的代码一行都不会执行，语言服务不会启动。\n' +
    '    表现是「语法高亮正常，但补全/悬停/参数提示/大纲全都没有」，且没有任何报错。\n' +
    '    至少要有 onLanguage:<语言 id>。'
  );
}

// main 指向的文件必须真实存在 —— 路径写错时 VSCode 只说「无法激活扩展」
if (hasMain) {
  const mp = path.join(EXT, pkg.main);
  if (!fs.existsSync(mp)) {
    err(`main 指向不存在的文件: ${pkg.main}`);
  } else {
    // 粗查语法：能 require 说明至少没有语法错误。
    // 不真的执行它 —— 里面 require('vscode') 在编辑器外不存在。
    try {
      new Function(fs.readFileSync(mp, 'utf8').replace(/^#!.*\n/, ''));
    } catch (e) {
      err(`main 文件有语法错误: ${pkg.main} — ${e.message}`);
    }
  }
}

// 激活事件里的语言 id 必须真的在 contributes 里注册过，否则永不触发
if (hasActs) {
  const langIds = new Set(((pkg.contributes || {}).languages || []).map(l => l.id));
  for (const a of pkg.activationEvents) {
    const m = /^onLanguage:(.+)$/.exec(a);
    if (m && !langIds.has(m[1])) {
      err(`activationEvents 里的 onLanguage:${m[1]} 没有对应的语言注册`);
    }
  }
}

// ---------- 3. 必要字段 ----------
for (const k of ['name', 'publisher', 'version', 'engines']) {
  if (!pkg[k]) err(`缺少必填字段 ${k}`);
}
if (pkg.engines && !pkg.engines.vscode) err('engines.vscode 未声明');
if (pkg.name && !/^[a-z0-9][a-z0-9-]*$/.test(pkg.name)) {
  err(`name "${pkg.name}" 只能含小写字母、数字、连字符`);
}
// semver 粗检：x.y.z
if (pkg.version && !/^\d+\.\d+\.\d+/.test(pkg.version)) {
  err(`version "${pkg.version}" 不是合法 semver`);
}

// ---------- 4. contributes 里的文件必须真实存在 ----------
const c = pkg.contributes || {};

for (const lang of c.languages || []) {
  if (!lang.id) err('contributes.languages 有一项缺 id');
  for (const ext of lang.extensions || []) {
    if (!ext.startsWith('.')) err(`语言 ${lang.id} 的扩展名 "${ext}" 必须以点开头`);
  }
  if (lang.configuration) {
    const p = path.join(EXT, lang.configuration);
    if (!fs.existsSync(p)) err(`language.configuration 指向不存在的文件: ${lang.configuration}`);
    else {
      try { JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, '')); }
      catch (e) { err(`language-configuration 不是合法 JSON: ${e.message}`); }
    }
  }
}

for (const g of c.grammars || []) {
  if (!g.language) err('contributes.grammars 有一项缺 language');
  if (!g.scopeName) err('contributes.grammars 有一项缺 scopeName');
  if (!g.path) { err('contributes.grammars 有一项缺 path'); continue; }
  const p = path.join(EXT, g.path);
  if (!fs.existsSync(p)) { err(`grammar 指向不存在的文件: ${g.path}`); continue; }
  // 语法文件必须是合法 JSON，且顶层有 scopeName / patterns
  let gram;
  try {
    gram = JSON.parse(fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, ''));
  } catch (e) {
    err(`grammar 不是合法 JSON: ${g.path} — ${e.message}`);
    continue;
  }
  if (gram.scopeName && gram.scopeName !== g.scopeName) {
    err(`scopeName 不一致: package.json 写 ${g.scopeName}，语法文件写 ${gram.scopeName}`);
  }
  if (!gram.patterns && !gram.repository) {
    err(`grammar 既没有 patterns 也没有 repository: ${g.path}`);
  }
  // 检查 include 引用完整性
  const txt = fs.readFileSync(p, 'utf8');
  const refs = new Set([...txt.matchAll(/"#([A-Za-z0-9_.-]+)"/g)].map(m => m[1]));
  const have = new Set(Object.keys(gram.repository || {}));
  for (const r of refs) {
    if (!have.has(r)) err(`grammar 引用了不存在的分组 #${r}`);
  }
  for (const h of have) {
    if (!refs.has(h)) warn(`grammar 定义了但没被引用的分组 #${h}`);
  }
}

// 每个 grammar 的 language 都得在 languages 里注册过
const langIds = new Set((c.languages || []).map(l => l.id));
for (const g of c.grammars || []) {
  if (g.language && !langIds.has(g.language)) {
    err(`grammar 的 language "${g.language}" 未在 contributes.languages 中注册`);
  }
}

// ---------- 5. main 里的依赖必须是零外部依赖 ----------
//
// 这一项必须放在「输出 + 退出」之前。踩过：早先它写在 PASS 分支里，
// 也就是 `if (errors.length) process.exit(1)` 之后 —— 于是那条检查
// 永远只在「其它检查都过了」时才跑，而且它的 err() 也永远不会被
// 打印出来（打印代码已经走过了）。看起来是「检查通过」，实际根本没查。
let reqCount = 0;
if (hasMain && fs.existsSync(path.join(EXT, pkg.main))) {
  const src = fs.readFileSync(path.join(EXT, pkg.main), 'utf8');
  const reqs = [...src.matchAll(/require\(\s*['"]([^'"]+)['"]\s*\)/g)].map(m => m[1]);
  reqCount = reqs.length;
  const NODE_BUILTIN = new Set(['child_process', 'path', 'fs', 'os', 'util', 'events',
    'stream', 'url', 'crypto', 'net', 'http', 'https', 'zlib', 'buffer', 'assert']);
  const external = reqs.filter(r => r !== 'vscode' && !NODE_BUILTIN.has(r));
  if (external.length) {
    err(`main 里 require 了非内置模块: ${external.join(', ')}\n` +
        '    扩展必须零依赖 —— 否则 vsce package 之前要先 npm install，\n' +
        '    而没有 Node 工具链的机器就打不出包。');
  }
}

// ---------- 6. 输出 ----------
console.log('检查目录:', EXT);
if (warns.length) {
  console.log('\n警告:');
  warns.forEach(w => console.log('  ! ' + w));
}
if (errors.length) {
  console.log(`\nFAIL: ${errors.length} 个错误会让扩展无法加载或失效`);
  errors.forEach(e => console.log('  x ' + e));
  process.exit(1);
}
console.log('\nPASS: 清单可被 VSCode 接受');
if (!hasMain && !hasBrowser) {
  console.log('  纯声明式扩展（无 main/browser），contributes 静态生效，无需激活事件');
} else {
  console.log(`  有 main（${pkg.main}），激活事件 ${pkg.activationEvents.length} 条`);
  console.log(`  require 的模块全部可用（${reqCount} 个，无外部依赖）`);
}