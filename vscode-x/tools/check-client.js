// tools/check-client.js — 用真客户端逻辑跑一遍完整链路
//
// ── 为什么 check-lsp.js 还不够 ──────────────────────────────
//
// check-lsp.js 验证的是「服务端按协议说话」。但用户看到的是扩展，
// 从服务端到编辑器中间还有一层：src/lspClient.js。那层里的映射
// （kind 数字 → VSCode 枚举、LSP Position → VSCode Position、
// 分帧字节数）全是手写的，一样会错。
//
// 所以这里做两件事：
//   1. 把 lspClient.js 里不依赖 vscode 的纯函数抠出来单独验
//      （分帧、位置换算、kind 映射）。
//   2. 用一个最小 vscode 桩替身，真的 require 整个 lspClient.js，
//      走一遍 activate() → 补全 / 悬停 / 参数提示 / 大纲。
//      这样「客户端代码本身有语法错或调用了不存在的方法」能被抓出来。
//
// 这个桩不能证明「在 VSCode 里长得对」——那只能用真编辑器看。
// 但它能证明「逻辑对」，而那才是写代码时最容易错的部分。
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const Module = require('module');

const ROOT = path.resolve(__dirname, '..');
const EXE = path.join(ROOT, '..', 'build', 'x.exe');

if (!fs.existsSync(EXE)) {
  console.error(`FAIL: 找不到 ${EXE}，先跑 tools/build.sh`);
  process.exit(1);
}

let pass = 0, fail = 0;
function ok(cond, what, extra) {
  if (cond) { pass++; console.log(`  PASS  ${what}`); }
  else {
    fail++;
    console.log(`  FAIL  ${what}`);
    if (extra !== undefined) {
      console.log('        ' + String(extra).split('\n').join('\n        '));
    }
  }
}

// ══════════════════════════════════════════════════════════
// 一、vscode 桩
// ══════════════════════════════════════════════════════════
//
// 只实现 lspClient.js 真正用到的那些。刻意做得「真」一些：
// CompletionItem 等类保留 label/kind 字段，Range 保留行号 ——
// 这样断言能检查内容，而不是只检查「没抛异常」。
const registered = { completion: [], hover: [], signature: [], symbols: [], definition: [] };
const commands = {};
let cfgValues = {};

class Position {
  constructor(line, character) { this.line = line; this.character = character; }
}
class Range {
  constructor(sl, sc, el, ec) {
    this.start = new Position(sl, sc);
    this.end = new Position(el, ec);
  }
}
class CompletionItem {
  constructor(label, kind) {
    this.label = label;
    this.kind = kind;
    this.insertText = undefined;
    this.detail = undefined;
    this.documentation = undefined;
    this.sortText = undefined;
    this.filterText = undefined;
  }
}
class CompletionList {
  constructor(items, isIncomplete) {
    this.items = items;
    this.isIncomplete = !!isIncomplete;
  }
}
class SnippetString {
  constructor(v) { this.value = v; }
}
class MarkdownString {
  constructor(v) { this.value = v; this.isTrusted = false; }
}
class Hover {
  constructor(contents) { this.contents = contents; }
}
class SignatureHelp {
  constructor() { this.signatures = []; this.activeSignature = 0; this.activeParameter = 0; }
}
class DocumentSymbol {
  constructor(name, detail, kind, range, selectionRange) {
    this.name = name;
    this.detail = detail;
    this.kind = kind;
    this.range = range;
    this.selectionRange = selectionRange;
    this.children = [];
  }
}
class Location {
  constructor(uri, range) { this.uri = uri; this.range = range; }
}

const CompletionItemKind = {
  Text: 0, Method: 1, Function: 2, Constructor: 3, Field: 4, Variable: 5,
  Class: 6, Interface: 7, Module: 8, Property: 9, Unit: 10, Value: 11,
  Enum: 12, Keyword: 13, Snippet: 14, Color: 15, File: 16, Reference: 17,
  Folder: 18, EnumMember: 19, Constant: 20, Struct: 21, Event: 22,
  Operator: 23, TypeParameter: 24,
};
const SymbolKind = {
  File: 0, Module: 1, Namespace: 2, Package: 3, Class: 4, Method: 5,
  Property: 6, Field: 7, Constructor: 8, Enum: 9, Interface: 10,
  Function: 11, Variable: 12, Constant: 13, String: 14, Number: 15,
  Boolean: 16, Array: 17, Object: 18, Key: 19, Null: 20, EnumMember: 21,
  Struct: 22, Event: 23, Operator: 24, TypeParameter: 25,
};

const Uri = {
  file: (p) => ({
    scheme: 'file',
    fsPath: p,
    toString: () => 'file:///' + p.replace(/\\/g, '/').replace(/^\//, ''),
  }),
  parse: (s) => ({
    scheme: 'file',
    fsPath: s.replace(/^file:\/\/\//, '').replace(/\//g, path.sep),
    toString: () => s,
  }),
};

const createdDocs = [];
const vscodeStub = {
  Position, Range, CompletionItem, CompletionList, SnippetString,
  MarkdownString, Hover, SignatureHelp, DocumentSymbol, Location,
  CompletionItemKind, SymbolKind, Uri,
  window: {
    createOutputChannel: () => ({
      appendLine: () => {},
      show: () => {},
      dispose: () => {},
    }),
    showWarningMessage: (m) => { console.log('        [warn] ' + m); },
    showErrorMessage: (m) => { console.log('        [error] ' + m); },
    showInformationMessage: () => {},
  },
  workspace: {
    workspaceFolders: [{ uri: Uri.file(path.resolve(ROOT, '..')), name: 'x' }],
    textDocuments: createdDocs,
    getConfiguration: () => ({
      get: (k) => (k in cfgValues ? cfgValues[k] : undefined),
    }),
    onDidOpenTextDocument: () => ({ dispose() {} }),
    onDidChangeTextDocument: () => ({ dispose() {} }),
    onDidCloseTextDocument: () => ({ dispose() {} }),
  },
  languages: {
    registerCompletionItemProvider: (sel, prov) => {
      registered.completion.push({ sel, prov });
      return { dispose() {} };
    },
    registerHoverProvider: (sel, prov) => {
      registered.hover.push({ sel, prov });
      return { dispose() {} };
    },
    registerSignatureHelpProvider: (sel, prov) => {
      registered.signature.push({ sel, prov });
      return { dispose() {} };
    },
    registerDocumentSymbolProvider: (sel, prov) => {
      registered.symbols.push({ sel, prov });
      return { dispose() {} };
    },
    registerDefinitionProvider: (sel, prov) => {
      registered.definition.push({ sel, prov });
      return { dispose() {} };
    },
  },
  commands: {
    registerCommand: (name, fn) => { commands[name] = fn; return { dispose() {} }; },
  },
};

// 把 'vscode' 这个 require 拦下来换桩
const origResolve = Module._resolveFilename;
Module._resolveFilename = function (req, ...rest) {
  if (req === 'vscode') return 'vscode';
  return origResolve.call(this, req, ...rest);
};
require.cache['vscode'] = {
  id: 'vscode',
  filename: 'vscode',
  loaded: true,
  exports: vscodeStub,
};

// ══════════════════════════════════════════════════════════
// 二、activate
// ══════════════════════════════════════════════════════════
const SRC = `// 客户端链路测试
fn greet(name: String) -> String {
    return "hi " + name
}

fn total(a: Int, b: Int) -> Int {
    let sum = a + b
    return sum
}

fn main() -> Int {
    let out = greet("x")
    println(out)
    return 0
}
`;

function lineOf(text, needle, nth = 0) {
  const lines = text.split('\n');
  let seen = 0;
  for (let i = 0; i < lines.length; i++) {
    const c = lines[i].indexOf(needle);
    if (c >= 0) {
      if (seen === nth) return { line: i, column: c };
      seen++;
    }
  }
  return { line: 0, column: 0 };
}

async function main() {
  console.log('客户端链路验证（用 vscode 桩 run 真 lspClient.js）\n');

  cfgValues = { executablePath: EXE };
  const client = require(path.join(ROOT, 'src', 'lspClient.js'));

  // 模拟一份已打开的文档
  const doc = {
    languageId: 'x',
    uri: Uri.file(path.join(ROOT, '..', 'test', 'client.xle')),
    version: 1,
    getText: () => SRC,
  };
  createdDocs.push(doc);

  const ctx = { subscriptions: [] };
  client.activate(ctx);

  // 等 initialize + didOpen 走完
  await new Promise(r => setTimeout(r, 900));

  // ---- 1. 能力注册齐全 ----
  ok(registered.completion.length === 1, '注册了 1 个补全 provider');
  ok(registered.hover.length === 1, '注册了 1 个悬停 provider');
  ok(registered.signature.length === 1, '注册了 1 个参数提示 provider');
  ok(registered.symbols.length === 1, '注册了 1 个文档大纲 provider');
  ok(registered.definition.length === 1, '注册了 1 个定义跳转 provider');
  ok(registered.completion[0].sel.language === 'x', '补全 provider 绑定到 x 语言');
  ok(registered.completion[0].sel.scheme === 'file', '补全 provider 绑定到 file scheme');
  ok(Object.keys(commands).includes('xlang.showServerLog'), '注册了日志命令');

  // ---- 1b. x.exe 路径查找（不设 executablePath，走自动查找）----
  //
  // ── 这组断言为什么必须单独立出来 ──────────────────────────
  //
  // 上面所有断言都是在 cfgValues.executablePath 已设为 EXE 的前提下跑的。
  // 那等于把「找 x.exe」这一步整个短路了 —— 而真实用户几乎不会去设这个
  // 设置项，他们期望的是「放到目录里就能用」。
  //
  // 结果就是：32 项断言全绿，扩展装到真机上却一行提示都没有，因为
  // 自动查找那一路从没被测过。这是一个「测试全绿 + 功能全废」的组合，
  // 比测试失败更难发现。
  //
  // 这里直接调扩展里同一个查找逻辑的等价实现会很脆弱（它不在 exports 里），
  // 所以改测「行为」：以仓库根为工作区（x.exe 在其 x-lang/build 子目录下），
  // 不设 executablePath，看能否成功启动服务端。
  {
    const { execFileSync } = require('child_process');
    // 用一个隔离子进程跑，避免影响上面已建立的连接
    const probe = `
      const Module=require('module'),path=require('path'),fs=require('fs');
      const log=[];
      const ch=()=>({appendLine:s=>log.push(s),show(){},dispose(){}});
      class P{constructor(l,c){this.line=l;this.character=c}}
      class R{constructor(a,b,c,d){this.start=new P(a,b);this.end=new P(c,d)}}
      const stub={
        Position:P,Range:R,
        CompletionItem:class{constructor(l,k){this.label=l}},
        CompletionList:class{constructor(i){this.items=i}},
        SnippetString:class{constructor(v){this.value=v}},
        MarkdownString:class{constructor(v){this.value=v}},
        Hover:class{},SignatureHelp:class{constructor(){this.signatures=[]}},
        DocumentSymbol:class{},CompletionItemKind:{},SymbolKind:{},
        Uri:{file:p=>({scheme:'file',fsPath:p,toString:()=>'file:///'+p.replace(/\\\\/g,'/')}),
             parse:s=>({scheme:'file',fsPath:s.replace('file:///',''),toString:()=>s})},
        window:{createOutputChannel:ch,showWarningMessage:m=>log.push('WARN '+m),
                showErrorMessage:m=>log.push('ERR '+m),showInformationMessage(){}},
        workspace:{
          workspaceFolders:[{uri:{scheme:'file',fsPath:process.env.PROBE_ROOT,toString:()=>'file:///'+process.env.PROBE_ROOT},name:'x'}],
          textDocuments:[],
          getConfiguration:()=>({get:()=>''}),   // 关键：不设 executablePath
          onDidOpenTextDocument:()=>({dispose(){}}),
          onDidChangeTextDocument:()=>({dispose(){}}),
          onDidCloseTextDocument:()=>({dispose(){}}),
        },
        languages:{registerCompletionItemProvider:()=>({dispose(){}}),
                   registerHoverProvider:()=>({dispose(){}}),
                   registerSignatureHelpProvider:()=>({dispose(){}}),
                   registerDocumentSymbolProvider:()=>({dispose(){}}),
                   registerDefinitionProvider:()=>({dispose(){}})},
        commands:{registerCommand:()=>({dispose(){}})},
      };
      const o=Module._resolveFilename;
      Module._resolveFilename=function(r,...a){return r==='vscode'?'vscode':o.call(this,r,...a)};
      require.cache['vscode']={id:'vscode',filename:'vscode',loaded:true,exports:stub};
      const m=require(process.env.PROBE_EXT);
      m.activate({subscriptions:[]});
      setTimeout(()=>{ console.log(log.join('\\n')); process.exit(0); }, 2200);
    `;
    let out = '';
    try {
      out = execFileSync(process.execPath, ['-e', probe], {
        timeout: 20000,
        encoding: 'utf8',
        env: {
          ...process.env,
          // 工作区根 = 仓库根（扩展目录的上一级），x.exe 在其子目录下
          PROBE_ROOT: path.resolve(ROOT, '..'),
          PROBE_EXT: path.join(ROOT, 'src', 'lspClient.js'),
        },
      });
    } catch (e) {
      out = (e.stdout || '') + (e.stderr || '');
    }
    ok(/x\.exe[：:]/.test(out) && /来源/.test(out),
       '不设 executablePath 也能找到 x.exe', out.trim().slice(0, 300));
    ok(/服务端就绪/.test(out),
       '自动找到的 x.exe 能完成握手', out.trim().slice(0, 300));
    ok(!/WARN|ERR/.test(out),
       '自动查找路径没有触发警告', out.trim().slice(0, 300));
  }

  const cp = registered.completion[0].prov;
  const hp = registered.hover[0].prov;
  const sp = registered.signature[0].prov;
  const dp = registered.symbols[0].prov;

  // ---- 2. 补全：全量 ----
  {
    const pos = new Position(lineOf(SRC, 'println(out)').line, 4);
    const list = await cp.provideCompletionItems(doc, pos, {}, {});
    ok(list instanceof CompletionList, '补全返回 CompletionList');
    const labels = list.items.map(i => i.label);
    ok(labels.length > 100, `补全项数量合理（${labels.length}）`);
    ok(labels.includes('greet'), '补全含本文档函数 greet');
    ok(labels.includes('println'), '补全含内建 println');
    ok(labels.includes('fn'), '补全含关键字 fn');
    ok(labels.includes('如果'), '补全含中文关键字 如果');

    // kind 映射要真的生效（不是全落回 Text=0）
    const kinds = new Set(list.items.map(i => i.kind));
    ok(kinds.size >= 4, `CompletionItem 的 kind 有多样性（${[...kinds].sort((a,b)=>a-b).join(',')}）`);
    ok(list.items.every(i => typeof i.kind === 'number'), '每项都有数字 kind');

    // 带括号的插入用 SnippetString
    const pr = list.items.find(i => i.label === 'println');
    ok(pr && pr.insertText !== undefined, 'println 带 insertText');
    const isSnippet = pr && (typeof pr.insertText === 'object' || String(pr.insertText).includes('('));
    ok(!!isSnippet, 'println 的 insertText 能放光标进括号', pr && JSON.stringify(pr.insertText));
  }

  // ---- 3. 悬停 ----
  {
    const pl = lineOf(SRC, 'println(out)');
    const h = await hp.provideHover(doc, new Position(pl.line, pl.column + 2), {}, {});
    ok(h instanceof Hover, '悬停返回 Hover 对象');
    const md = h && h.contents && h.contents.value;
    ok(typeof md === 'string' && md.includes('内建函数'), '悬停说 println 是内建函数', md);
  }

  // ---- 4. 悬停本文档函数 ----
  {
    const gl = lineOf(SRC, 'greet("x")');
    const h = await hp.provideHover(doc, new Position(gl.line, gl.column + 2), {}, {});
    const md = h && h.contents && h.contents.value;
    ok(typeof md === 'string' && md.includes('greet'), '悬停认出来本文档函数 greet', md);
  }

  // ---- 5. 参数提示：括号里 ----
  {
    // 光标放在 greet( 之后
    const gl = lineOf(SRC, 'greet("x")');
    const sh = await sp.provideSignatureHelp(doc, new Position(gl.line, gl.column + 6), {}, {});
    ok(sh instanceof SignatureHelp, '参数提示返回 SignatureHelp');
    ok(sh && sh.signatures.length >= 1, '至少一个签名');
    ok(sh && sh.signatures[0].label.includes('greet'), '签名标签含 greet',
       sh && sh.signatures[0] && sh.signatures[0].label);
    ok(sh && typeof sh.activeParameter === 'number', '带 activeParameter');
  }

  // ---- 6. 文档大纲 ----
  {
    const syms = await dp.provideDocumentSymbols(doc, {}, {});
    ok(Array.isArray(syms) && syms.length > 0, `大纲非空（${syms && syms.length} 项）`);
    const names = (syms || []).map(s => s.name);
    ok(names.includes('greet'), '大纲含 greet', names.join(','));
    ok(names.includes('main'), '大纲含 main', names.join(','));
    ok(syms[0] instanceof DocumentSymbol, '大纲项是 DocumentSymbol');
    ok(syms[0].range instanceof Range, '大纲项带 Range');
    const fnSyms = syms.filter(s => s.kind === SymbolKind.Function);
    ok(fnSyms.length >= 3, `至少 3 个被认成函数（${fnSyms.length}）`);
  }

  // ---- 6b. 定义跳转（ctrl+左键的完整链路）----
  {
    const defp = registered.definition[0].prov;
    const gl = lineOf(SRC, 'greet("x")');
    const locs = await defp.provideDefinition(doc, new Position(gl.line, gl.column + 2), {}, {});
    ok(Array.isArray(locs) && locs.length >= 1, '定义跳转返回 Location 数组',
       JSON.stringify(locs));
    if (locs && locs.length) {
      ok(locs[0] instanceof Location, '跳转目标是 Location 实例');
      ok(locs[0].uri.toString() === doc.uri.toString(), '跳转 uri 与文档一致',
         String(locs[0].uri));
      ok(locs[0].range.start.line === 1, '跳转落在 fn greet 定义行',
         JSON.stringify(locs[0].range));
    }
  }

  // ---- 7. 空白处不该崩 ----
  {
    const h = await hp.provideHover(doc, new Position(0, 0), {}, {});
    ok(h === undefined || h === null, '注释行悬停返回空而不是抛错', h === undefined ? 'undefined' : String(h));
  }

  // ---- 8. 改文档后补全能跟上 ----
  {
    const changed = SRC.replace('fn main() -> Int {',
      'fn 新增函数(a: Int) -> Int {\n    return a\n}\n\nfn main() -> Int {');
    doc.getText = () => changed;
    doc.version = 2;
    const pl = lineOf(changed, 'println(out)');
    const list = await cp.provideCompletionItems(doc, new Position(pl.line, 4), {}, {});
    ok(list && list.items.length > 0, '文档变化后补全仍然工作');
  }

  // ---- 清理 ----
  //
  // 这一步不能只调 deactivate()：它是空函数（真实 VSCode 里由
  // context.subscriptions 里的 dispose 负责收尾）。这里必须把
  // subscriptions 全部 dispose 掉，否则 x.exe lsp 子进程还活着，
  // 它的 stdio 管道会把 node 的事件循环吊住 —— 脚本逻辑早跑完了，
  // 进程却永远不退出，上层 `sh tools/package.sh | tail` 就会卡死。
  client.deactivate();
  for (const s of ctx.subscriptions) {
    try { if (s && typeof s.dispose === 'function') s.dispose(); } catch (e) { /* 已释放 */ }
  }
  await new Promise(r => setTimeout(r, 600));

  console.log(`\n  ${pass} 项通过${fail ? `，${fail} 项失败` : ''}`);
  // 显式 exit：即使有句柄没关干净也立刻结束，不让上层管道挂住
  if (fail) {
    console.log('client fail');
    process.exit(1);
  }
  console.log('client ok');
  process.exit(0);
}

// ── 硬保底：无论走到哪一步，90 秒后必须结束 ──────────────────
//
// 这个脚本会被 tools/run_tests.sh 与 tools/package.sh 调用，两者都
// 通过管道读它的输出。一旦这里有句柄没关干净（x.exe lsp 的 stdio
// 就是典型），上层会**无限期**等下去 —— 表现是「测试跑不完」，
// 而不是「测试失败」，排查成本极高。
//
// 所以除了正常的 process.exit(0)，再加一道定时器兜底。它平时不会
// 触发（正常路径 1 秒内就退出），只在出意外时把挂死变成失败。
const hull = setTimeout(() => {
  console.error('FAIL: 脚本超过 90 秒未结束 —— 有句柄没关干净（多半是 x.exe lsp 子进程）');
  process.exit(2);
}, 90000);
hull.unref && hull.unref();

main().catch(e => {
  console.error('FAIL: 脚本自身异常 —', e.stack);
  process.exit(1);
});