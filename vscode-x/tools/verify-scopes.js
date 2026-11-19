// 用 VSCode 自己的引擎（vscode-textmate + vscode-oniguruma）真实 tokenize。
// 手写正则模拟器会和真实引擎有偏差 —— 之前就因此误判过 fn 声明没命中。
//
// 为什么不用自己写的扫描器：TextMate 的 begin/end 区域栈、captures 分组、
// Oniguruma 与 JS 正则的方言差异（比如 \u0080-\uFFFF 在 JS 里是代理对语义），
// 任何一处模仿不到位，验证结论就是错的。只有跑真引擎才作数。
//
// 用法（依赖装在扩展目录之外，避免 node_modules 被打包进扩展）：
//   mkdir -p /tmp/tm && cd /tmp/tm && npm i --no-save vscode-textmate vscode-oniguruma
//   NODE_PATH=/tmp/tm/node_modules node x-lang/vscode-x/tools/verify-scopes.js
const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const EXT = path.resolve(HERE, '..');
const GRAMMAR = path.join(EXT, 'syntaxes/x.tmLanguage.json');
const LANG_ROOT = path.resolve(EXT, '..');

const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

// 用 require.resolve 找包，再定位 wasm —— 不能假设 node_modules 在脚本旁边
const onigPkg = path.dirname(require.resolve('vscode-oniguruma/package.json'));
const wasmBin = fs.readFileSync(path.join(onigPkg, 'release/onig.wasm')).buffer;
const onigLib = oniguruma.loadWASM(wasmBin).then(() => ({
  createOnigScanner: (s) => new oniguruma.OnigScanner(s),
  createOnigString: (s) => new oniguruma.OnigString(s),
}));

const registry = new vsctm.Registry({
  onigLib,
  loadGrammar: async (scopeName) => {
    if (scopeName === 'source.x') {
      return vsctm.parseRawGrammar(fs.readFileSync(GRAMMAR, 'utf8'), GRAMMAR);
    }
    return null;
  },
});

function interesting(tok) {
  const t = tok.trim();
  if (!t) return false;
  if (/^[{}()\[\],;:.]+$/.test(t)) return false;
  return true;
}

async function run() {
  const grammar = await registry.loadGrammar('source.x');
  if (!grammar) throw new Error('语法加载失败：source.x 未注册');

  const files = [
    'tests/p0/04-fn.xle',
    'tests/p0/03-control.xle',
    'tests/p0/05-string.xle',
    'tests/p0/08-zh.xlc',
    'tests/p0/16-array.xle',
    'tests/p0/17-for.xle',
    // 类系统（v0.15 / v0.16）。这四个文件是「高亮是否跟上编译器」的判据 ——
    // 它们里面用到了 class / interface / abstract / override / private /
    // protected / new / constructor / static / @注解 / 9 个反射内建，任何一条
    // 对应的语法规则失效，这里的 scope 就会退回裸 source.x。
    'tests/p0/37-class.xle',
    'tests/p0/39-visibility.xle',
    'tests/p0/40-reflect.xle',
    'tests/p0/41-annotation.xle',

    // 新语法（v0.17）。这五个文件覆盖三元 ?: / 数组字面量 / 可选链 ?. /
    // match / 字符串插值。任何一条规则失效，对应 token 会退回裸 source.x；
    // 尤其是 `?.` —— 它的规则若排到 `\?` 之后，会被拆成 `?` + `.`，
    // 这在这里表现为 "?." 不再整体出现。
    'tests/p0/43-ternary-coalesce.xle',
    'tests/p0/44-array-literal.xle',
    'tests/p0/45-opt-chain.xle',
    'tests/p0/46-match.xle',
    'tests/p0/47-string-interp.xle',
    'tests/p0/49-interp-literal-braces.xle',
    'tests/p0/51-raw-string.xle',
  ];

  let totalTokens = 0;
  let fallbackHits = [];

  for (const f of files) {
    const full = path.join(LANG_ROOT, f);
    if (!fs.existsSync(full)) { console.log('SKIP (missing):', f); continue; }
    const lines = fs.readFileSync(full, 'utf8').split(/\r?\n/);

    console.log('\n' + '='.repeat(72));
    console.log('FILE:', f);
    console.log('='.repeat(72));

    let ruleStack = vsctm.INITIAL;
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const r = grammar.tokenizeLine(line, ruleStack);
      ruleStack = r.ruleStack;

      const shown = [];
      for (const tok of r.tokens) {
        const text = line.substring(tok.startIndex, tok.endIndex);
        if (!interesting(text)) continue;
        totalTokens++;
        const leaf = tok.scopes[tok.scopes.length - 1];
        shown.push(`    ${JSON.stringify(text).padEnd(24)} ${leaf}`);
        if (leaf === 'source.x') fallbackHits.push(`${f}:${i + 1} ${JSON.stringify(text)}`);
      }
      if (shown.length) {
        console.log(`  L${String(i + 1).padStart(3)} | ${line.trim().slice(0, 64)}`);
        shown.forEach((s) => console.log(s));
      }
    }
  }

  console.log('\n' + '='.repeat(72));
  console.log('TOTAL TOKENS:', totalTokens, '| FALLBACK to bare source.x:', fallbackHits.length);
  if (fallbackHits.length) fallbackHits.slice(0, 25).forEach((h) => console.log('   ', h));
  console.log('='.repeat(72));
}

run().catch((e) => { console.error('FAILED:', e); process.exit(1); });