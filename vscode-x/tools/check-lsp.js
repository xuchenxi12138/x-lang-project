// tools/check-lsp.js — 用真实 LSP 消息验证服务端
//
// ── 为什么必须有这个：C 里拼 JSON 没有类型系统保证 ──────────
//
// lsp.c 的响应是手拼字符串。写错一个逗号、漏一个引号，编译器
// 完全不会说话 —— 而客户端那边的表现是「语言服务器已停止」或
// 「一直转圈」，没有任何可用信息。
//
// 所以这里的做法是：真的启动 x.exe lsp，真的按协议发字节，
// 把每条响应都 JSON.parse 一遍。解析过不去就是拼错了。
//
// ── 为什么要验语义而不只是「能解析」 ────────────────────────
//
// 「返回了合法 JSON」太弱：一个永远返回空数组的补全也是合法
// JSON。所以每个用例都断言具体内容 —— 补全里有 fn / 有本文档
// 函数名、悬停说的是内建、参数提示指出正确的位置。
//
// 用法：node tools/check-lsp.js
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const ROOT = path.resolve(__dirname, '..');
// x.exe 在扩展目录的上一级（vscode-x/ 与 build/ 是兄弟）
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
    if (extra) console.log('        ' + String(extra).split('\n').join('\n        '));
  }
}

// ---------- LSP 客户端 ----------
class Client {
  constructor() {
    this.proc = spawn(EXE, ['lsp'], { stdio: ['pipe', 'pipe', 'pipe'] });
    this.buf = Buffer.alloc(0);
    this.pending = new Map();
    this.nextId = 1;
    this.stderr = '';
    this.proc.stderr.on('data', d => { this.stderr += d.toString(); });
    this.proc.stdout.on('data', d => this._onData(d));
  }

  _onData(d) {
    this.buf = Buffer.concat([this.buf, d]);
    for (;;) {
      const idx = this.buf.indexOf('\r\n\r\n');
      if (idx < 0) return;
      const header = this.buf.slice(0, idx).toString('ascii');
      const m = /Content-Length:\s*(\d+)/i.exec(header);
      if (!m) { this.buf = this.buf.slice(idx + 4); continue; }
      const len = parseInt(m[1], 10);
      const bodyStart = idx + 4;
      if (this.buf.length < bodyStart + len) return;
      const body = this.buf.slice(bodyStart, bodyStart + len).toString('utf8');
      this.buf = this.buf.slice(bodyStart + len);
      let msg;
      try { msg = JSON.parse(body); }
      catch (e) {
        fail++;
        console.log(`  FAIL  响应不是合法 JSON — ${e.message}`);
        console.log('        ' + body.slice(0, 300));
        continue;
      }
      if (msg.id !== undefined && this.pending.has(msg.id)) {
        const r = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        r(msg);
      }
    }
  }

  send(obj) {
    const s = JSON.stringify(obj);
    const b = Buffer.from(s, 'utf8');
    this.proc.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
    this.proc.stdin.write(b);
  }

  request(method, params, timeoutMs = 5000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`超时：${method}（5s 没响应）`));
      }, timeoutMs);
      this.pending.set(id, msg => { clearTimeout(t); resolve(msg); });
      this.send({ jsonrpc: '2.0', id, method, params });
    });
  }

  notify(method, params) {
    this.send({ jsonrpc: '2.0', method, params });
  }

  kill() {
    try { this.proc.kill(); } catch (e) { /* 已退出 */ }
  }
}

// UTF-16 列 → 字节偏移（测试脚本里也用同一套规则，才能验证服务端）
function utf16ToByteIndex(text, line, character) {
  let i = 0, cur = 0;
  while (i < text.length && cur < line) {
    if (text[i] === '\n') cur++;
    i++;
  }
  let u16 = 0;
  while (i < text.length && text[i] !== '\n' && text[i] !== '\r') {
    if (u16 >= character) break;
    const cp = text.codePointAt(i);
    u16 += cp > 0xffff ? 2 : 1;
    i += cp > 0xffff ? 2 : 1;
  }
  return i;
}

function byteIndexToUtf16(text, line, byteIdx) {
  // 测试文本是 ASCII+中文的混合；这里按 UTF-8 字节还原到 UTF-16 列
  const b = Buffer.from(text, 'utf8');
  let i = 0, cur = 0;
  while (i < b.length && cur < line) {
    if (b[i] === 0x0a) cur++;
    i++;
  }
  let u16 = 0;
  while (i < b.length && i < byteIdx) {
    const c = b[i];
    if (c === 0x0a || c === 0x0d) break;
    let cp, len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xe0) === 0xc0) { cp = c & 0x1f; len = 2; }
    else if ((c & 0xf0) === 0xe0) { cp = c & 0x0f; len = 3; }
    else { cp = c & 0x07; len = 4; }
    u16 += cp > 0xffff ? 2 : 1;
    i += len;
  }
  return u16;
}

// ---------- 测试用源码 ----------
const SRC = `// 测试文件
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

const URI = 'file:///test/sample.xle';

function lineOf(text, needle) {
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const c = lines[i].indexOf(needle);
    if (c >= 0) return { line: i, column: lines[i].slice(0, c).length };
  }
  return null;
}

(async () => {
  console.log('LSP 协议验证（真实子进程 + 真实字节）\n');

  const c = new Client();

  // ---- 1. initialize ----
  let init;
  try {
    init = await c.request('initialize', {
      processId: process.pid,
      rootUri: 'file:///test',
      capabilities: {},
    });
  } catch (e) {
    console.log(`  FAIL  initialize — ${e.message}`);
    console.log('        stderr: ' + c.stderr.slice(0, 400));
    c.kill();
    process.exit(1);
  }

  const caps = init.result && init.result.capabilities;
  ok(!!caps, 'initialize 返回 capabilities');
  ok(caps && caps.completionProvider !== undefined, '声明支持补全');
  ok(caps && caps.hoverProvider === true, '声明支持悬停');
  ok(caps && caps.definitionProvider === true, '声明支持定义跳转');
  ok(caps && caps.signatureHelpProvider !== undefined, '声明支持参数提示');
  ok(caps && caps.documentSymbolProvider === true, '声明支持文档大纲');
  ok(caps && caps.positionEncoding === 'utf-16', '位置编码声明为 utf-16');
  ok(init.result && init.result.serverInfo && init.result.serverInfo.name,
     '返回 serverInfo');

  c.notify('initialized', {});

  // ---- 2. didOpen ----
  c.notify('textDocument/didOpen', {
    textDocument: { uri: URI, languageId: 'x', version: 1, text: SRC },
  });
  // 给服务端一点时间处理通知（通知没有响应可等）
  await new Promise(r => setTimeout(r, 150));

  // ---- 3. 补全：空前缀应给出全部类别 ----
  {
    const pos = lineOf(SRC, 'println(out)');
    const r = await c.request('textDocument/completion', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: 4 },
    });
    const items = (r.result && r.result.items) || [];
    const labels = items.map(i => i.label);

    ok(items.length > 100, `补全数量合理（${items.length} 项）`);
    ok(labels.includes('println'), '补全里有内建 println');
    ok(labels.includes('arrPush'), '补全里有内建 arrPush');
    ok(labels.includes('fn'), '补全里有关键字 fn');
    ok(labels.includes('如果'), '补全里有中文关键字 如果');
    ok(labels.includes('Int'), '补全里有类型 Int');
    ok(labels.includes('整数'), '补全里有中文类型 整数');
    ok(labels.includes('greet'), '补全里有本文档定义的 greet');
    ok(labels.includes('total'), '补全里有本文档定义的 total');

    // ── 类系统（v0.15 / v0.16）────────────────────────────────
    //
    // 这几条断言的是「补全词表跟上了编译器」。它们曾经全部不成立：
    // lsp_data.h 停留在类系统实现之前，补全里一个类关键字都没有，
    // 而生成脚本因为只查「数量是否够」照样报成功（57>40、96>50）。
    // 所以必须逐名断言，不能只看总数。
    for (const k of ['class', 'interface', 'extends', 'implements',
                     'abstract', 'override', 'private', 'protected',
                     'public', 'new', 'self', 'final']) {
      ok(labels.includes(k), `补全里有关键字 ${k}`);
    }
    for (const k of ['类', '接口', '继承', '实现接口', '抽象',
                     '覆写', '私有', '保护', '公开', '新建', '终态']) {
      ok(labels.includes(k), `补全里有中文关键字 ${k}`);
    }
    // 反射与注解内建
    for (const b of ['className', 'classParent', 'classIs', 'classFields',
                     'classMethods', 'fieldGet', 'fieldSet', 'fieldHas',
                     'methodHas', 'annoHas', 'annoGet', 'annoGetKV', 'annoAll']) {
      ok(labels.includes(b), `补全里有内建 ${b}`);
    }

    // ── 反向：死 token 不能被推荐 ──────────────────────────────
    //
    // super 是这里最值得盯的一个：lexer 英文表有、中文表有 `父类`、
    // x.h 有 TK_SUPER，三处都「写着存在」，但 parser.c 里 TK_SUPER
    // 出现 0 次（`super()` 报「期望一个表达式」）。补全推荐它等于
    // 主动教用户写编不过的代码。
    // 注意这里不能写 `结构` —— 它是 `struct` 的中文别名，而 struct 是
    // **活的**（实测 `struct P { x: Int, y: Int }` 能编过）。第一版顺手
    // 把它列进死 token，断言立刻 FAIL，反而证明补全是对的。
    // 死的只有 enum/trait/impl 那一批：枚举 特征 实现 模组 情形 做 捕获。
    // v0.17 起 match / 匹配 **不在**这个列表里 —— parser 已实现 EX_MATCH，
    // 它们由死转活，断言随之挪到下面的正向检查。
    for (const d of ['super', '父类', 'enum', 'trait', 'impl',
                     'dyn', 'spawn', 'unsafe', '枚举', '特征', '情形']) {
      ok(!labels.includes(d), `补全里没有死 token ${d}`);
    }

    // 正向：新增的活 token 必须被推荐。
    // 这条断言是「词表 / 黑名单 / 实测」三者一致的最后一道门 ——
    // 只在 gen-completions.js 里删黑名单是不够的，产物里的词表
    // 要真的更新到 x.exe，这里才看得出来。
    for (const a of ['match', '匹配']) {
      ok(labels.includes(a), `补全里有活 token ${a}`);
    }

    // 内建项的 insertText 应带括号（光标放进去）
    const pr = items.find(i => i.label === 'println');
    ok(pr && pr.insertText && pr.insertText.includes('('),
       '内建 println 的 insertText 带括号', pr && JSON.stringify(pr));

    // 种类要正确 —— 全是 keyword 说明分类没生效
    const kinds = new Set(items.map(i => i.kind));
    ok(kinds.size >= 4, `补全项种类多样（${[...kinds].join(',')}）`);
  }

  // ---- 4. 补全：有前缀时过滤 ----
  {
    // 在 `let out = greet("x")` 后面开一行，敲 `gr`
    const text2 = SRC.replace('    println(out)',
      '    gr\n    println(out)');
    c.notify('textDocument/didChange', {
      textDocument: { uri: URI, version: 2 },
      contentChanges: [{ text: text2 }],
    });
    await new Promise(r => setTimeout(r, 100));

    const pos = lineOf(text2, 'gr');
    const r = await c.request('textDocument/completion', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },
    });
    const items = (r.result && r.result.items) || [];
    const labels = items.map(i => i.label);

    ok(labels.includes('greet'), '前缀 gr 能补出 greet');
    ok(!labels.includes('printf'), '前缀 gr 过滤掉了不相关项');
    const allStartGr = labels.every(l => l.startsWith('gr'));
    ok(allStartGr, '所有候选都以 gr 开头', labels.join(','));
  }

  // ---- 4b. 把文档恢复成 SRC ----
  //
  // 这一步不能省：第 4 步给文档插了一行 `gr`，后面几个用例却都是按
  // SRC 算行号的。不恢复的话行号整体下移一行，悬停会落在空行上、
  // 返回 null —— 而错误信息只会说「悬停没识别成内建函数」，看不出
  // 是测试自己把文档改坏了。
  c.notify('textDocument/didChange', {
    textDocument: { uri: URI, version: 3 },
    contentChanges: [{ text: SRC }],
  });
  await new Promise(r => setTimeout(r, 100));

  // ---- 5. 悬停：内建 ----
  {
    const pos = lineOf(SRC, 'println(out)');
    const r = await c.request('textDocument/hover', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },
    });
    const v = r.result && r.result.contents && r.result.contents.value;
    ok(typeof v === 'string' && v.includes('内建函数'),
       '悬停 println 说的是内建函数', v);
    ok(typeof v === 'string' && v.includes('参数'),
       '悬停内容包含参数个数', v);
  }

  // ---- 6. 悬停：本文档函数 ----
  {
    const pos = lineOf(SRC, 'greet("x")');
    const r = await c.request('textDocument/hover', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },
    });
    const v = r.result && r.result.contents && r.result.contents.value;
    ok(typeof v === 'string' && v.includes('函数'),
       '悬停 greet 认成本文档函数', v);
    ok(typeof v === 'string' && v.includes('定义于本文件'),
       '悬停给出定义位置', v);
  }

  // ---- 7. 悬停：空白处应返回 null ----
  {
    const r = await c.request('textDocument/hover', {
      textDocument: { uri: URI },
      position: { line: 0, character: 0 },
    });
    ok(r.result === null, '悬停在注释/空白处返回 null');
  }

  // ---- 8. 参数提示 ----
  {
    // 光标放在 `println(` 之后
    const text3 = SRC.replace('println(out)', 'println()');
    c.notify('textDocument/didChange', {
      textDocument: { uri: URI, version: 3 },
      contentChanges: [{ text: text3 }],
    });
    await new Promise(r => setTimeout(r, 100));

    const pos = lineOf(text3, 'println()');
    const r = await c.request('textDocument/signatureHelp', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 8 },
    });
    const sigs = r.result && r.result.signatures;
    ok(Array.isArray(sigs) && sigs.length > 0, '参数提示返回签名', JSON.stringify(r.result));
    if (sigs && sigs.length) {
      ok(/println/.test(sigs[0].label), '签名标签含函数名', sigs[0].label);
      ok(typeof r.result.activeParameter === 'number',
         '给出 activeParameter', String(r.result.activeParameter));
    }
  }

  // ---- 9. 文档大纲 ----
  {
    const r = await c.request('textDocument/documentSymbol', {
      textDocument: { uri: URI },
    });
    const syms = r.result || [];
    ok(Array.isArray(syms) && syms.length > 0, `大纲有内容（${syms.length} 项）`);
    const names = syms.map(s => s.name);
    ok(names.includes('greet'), '大纲含 greet');
    ok(names.includes('total'), '大纲含 total');
    ok(names.includes('main'), '大纲含 main');
    // 位置字段必须齐全，否则编辑器跳转不了
    const g = syms.find(s => s.name === 'greet');
    ok(g && g.range && g.range.start && typeof g.range.start.line === 'number',
       '大纲项带 range.start.line');

    // 大纲里不能出现类型名。
    //
    // 曾经这里混进了 String/Int —— 参数扫描把「任何左括号」都当成参数
    // 列表，于是 `greet("x")` 的实参和 `name: String` 的类型名都被收成
    // 符号。大纲看上去「多了几项」不痛不痒，但按大纲跳转会落到莫名其妙
    // 的位置，而且补全里会多出拿不出来的候选。
    const TYPES = ['Int', 'Float', 'Bool', 'String', 'Array', 'void', 'Void'];
    const typeLeak = names.filter(n => TYPES.includes(n));
    ok(typeLeak.length === 0, '大纲里没有混入类型名', typeLeak.join(','));

    // 函数调用里的实参名也不该被当成定义（示例里有 greet("x")）
    ok(!names.includes('x'), '大纲里没有混入调用实参名', names.join(','));
  }

  // ---- 10. 中文标识符不破坏位置计算 ----
  {
    const zh = `fn 打招呼(名字: String) -> String {\n    return "你好" + 名字\n}\n`;
    const uri2 = 'file:///test/zh.xlc';
    c.notify('textDocument/didOpen', {
      textDocument: { uri: uri2, languageId: 'x', version: 1, text: zh },
    });
    await new Promise(r => setTimeout(r, 150));

    const r = await c.request('textDocument/completion', {
      textDocument: { uri: uri2 },
      position: { line: 1, character: 4 },
    });
    const labels = ((r.result && r.result.items) || []).map(i => i.label);
    ok(labels.includes('打招呼'), '中文函数名进入补全', labels.slice(0, 20).join(','));
  }

  // ---- 11. 未知方法要回错误，不能沉默 ----
  //
  // （原先这里用 textDocument/definition 当「未知方法」的反面教材 ——
  // definition 实现之后换成真正没实现的 references，断言才仍然成立。）
  {
    const r = await c.request('textDocument/references', {
      textDocument: { uri: URI },
      position: { line: 0, character: 0 },
    });
    ok(r.error && r.error.code === -32601, '未实现的方法返回 -32601',
       JSON.stringify(r));
  }

  // ---- 11a. 定义跳转：本文档函数 ----
  {
    const pos = lineOf(SRC, 'greet("x")');
    const r = await c.request('textDocument/definition', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },  // 落在 greet 上
    });
    const locs = r.result || [];
    ok(Array.isArray(locs) && locs.length >= 1,
       'ctrl+点击 greet 能返回定义位置', JSON.stringify(r.result));
    if (locs.length) {
      const l = locs[0];
      ok(l.uri === URI, '定义的 uri 正确', l.uri);
      ok(l.range.start.line === 1, '定义落在 fn greet 那一行',
         JSON.stringify(l.range));
      ok(l.range.start.character === 3, '定义落在 greet 名字开头',
         JSON.stringify(l.range));
    }
  }

  // ---- 11b. 定义跳转：参数 ----
  {
    const pos = lineOf(SRC, '+ name');
    const r = await c.request('textDocument/definition', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },  // 落在 name 上
    });
    const locs = r.result || [];
    ok(Array.isArray(locs) && locs.length >= 1,
       'ctrl+点击参数 name 能跳到参数声明', JSON.stringify(r.result));
    if (locs.length) {
      ok(locs[0].range.start.line === 1 &&
         locs[0].range.start.character === 9,
         '参数定义位置正确（fn greet( 后第一个字符）',
         JSON.stringify(locs[0].range));
    }
  }

  // ---- 11c. 定义跳转：没有定义的内建返回 null ----
  {
    const pos = lineOf(SRC, 'println(out)');
    const r = await c.request('textDocument/definition', {
      textDocument: { uri: URI },
      position: { line: pos.line, character: pos.column + 2 },
    });
    ok(r.result === null, '内建函数 println 无定义可跳，返回 null',
       JSON.stringify(r.result));
  }

  // ---- 11d. 定义跳转：类 ----
  {
    const cls = `class User {\n    name: String\n}\n\nfn main() {\n    let u = new User()\n    println(u)\n}\n`;
    const uri3 = 'file:///test/cls.xle';
    c.notify('textDocument/didOpen', {
      textDocument: { uri: uri3, languageId: 'x', version: 1, text: cls },
    });
    await new Promise(r => setTimeout(r, 150));

    const pos = lineOf(cls, 'new User()');
    const r = await c.request('textDocument/definition', {
      textDocument: { uri: uri3 },
      position: { line: pos.line, character: pos.column + 5 },  // 落在 User 上
    });
    const locs = r.result || [];
    ok(Array.isArray(locs) && locs.length >= 1,
       'ctrl+点击 new User() 里的 User 能跳到 class User', JSON.stringify(r.result));
    if (locs.length) {
      ok(locs[0].uri === uri3 && locs[0].range.start.line === 0,
         '类定义位置正确', JSON.stringify(locs[0]));
    }
  }

  // ---- 11e. 定义跳转：跨打开文档 ----
  {
    // 在 11d 打开的 cls.xle 里点 greet —— 定义在 sample.xle 里，
    // 两个文档都开着，必须能跨文档找到。
    const cls = `class User {\n    name: String\n}\n\nfn main() {\n    let s = greet("x")\n    println(s)\n}\n`;
    const uri3 = 'file:///test/cls.xle';
    c.notify('textDocument/didChange', {
      textDocument: { uri: uri3, version: 2 },
      contentChanges: [{ text: cls }],
    });
    await new Promise(r => setTimeout(r, 150));

    const pos = lineOf(cls, 'greet("x")');
    const r = await c.request('textDocument/definition', {
      textDocument: { uri: uri3 },
      position: { line: pos.line, character: pos.column + 2 },
    });
    const locs = r.result || [];
    ok(Array.isArray(locs) && locs.length >= 1,
       '跨打开文档能找到 greet 的定义', JSON.stringify(r.result));
    if (locs.length) {
      ok(locs[0].uri === URI && locs[0].range.start.line === 1,
         '跨文档定义的 uri 与行号正确', JSON.stringify(locs[0]));
    }
  }

  // ---- 12. shutdown / exit ----
  {
    const r = await c.request('shutdown', null);
    ok(r.result === null, 'shutdown 返回 null');
    c.notify('exit', null);
    await new Promise(r => setTimeout(r, 300));
    ok(c.proc.exitCode !== null || c.proc.killed, 'exit 之后进程退出');
  }

  c.kill();

  console.log(`\n  ${pass} 项通过${fail ? `，${fail} 项失败` : ''}`);
  if (fail) {
    console.log('\n服务端 stderr：');
    console.log(c.stderr.split('\n').slice(0, 30).map(l => '  ' + l).join('\n'));
  } else {
    console.log('lsp ok');
  }
  process.exit(fail ? 1 : 0);
})();