// 用一批最小样例覆盖所有语法构造，逐 token 打印 scope。
// 用途：改完语法后跑一遍，肉眼扫 output，找 "看起来该有颜色但没有独立 scope" 的 token。
// 依赖方式同 verify-scopes.js（NODE_PATH 指向外部 node_modules）。
const fs = require('fs'), path = require('path');
const vsctm = require('vscode-textmate'), onig = require('vscode-oniguruma');
const G = path.resolve(__dirname, '../syntaxes/x.tmLanguage.json');
const onigLib = onig.loadWASM(
  fs.readFileSync(path.join(path.dirname(require.resolve('vscode-oniguruma/package.json')), 'release/onig.wasm')).buffer
).then(() => ({ createOnigScanner: s => new onig.OnigScanner(s), createOnigString: s => new onig.OnigString(s) }));
const reg = new vsctm.Registry({ onigLib, loadGrammar: async s => s === 'source.x' ? vsctm.parseRawGrammar(fs.readFileSync(G, 'utf8'), G) : null });

// 死 token 清单：lexer.c 里定义、parser.c 里没有解析分支的那些词。
//
// ── 这份清单变过，注意别照旧结论抄 ─────────────────────────
//
// v0.15 之前这里是 struct enum trait impl type mod match case do catch
// yield comptime extern unsafe dyn spawn par where self mut ref —— 一共 22 个。
//
// v0.16 实现类系统后重新实测，**struct 和 self 变成活的了**：
//   struct P { x: Int, y: Int } + P { x: 1, y: 2 }   能编过
//   fn f(self) / self.n = 1                          能编过
// 于是它们从这份清单里移出去了。留着它们的后果恰恰是本脚本会 FAIL ——
// 断言「struct 不能有关键字色」而语法文件正确地给了它类声明色。
//
// 现在这份清单与 gen-completions.js 的 DEAD_KEYWORDS 严格一致（20 个）。
// 一致性由 tools/probe-dead-keywords.sh 自动对账，不用手工维护两处。
const DEAD = new Set([
  'enum', 'trait', 'impl', 'type', 'mod', 'match', 'case', 'do',
  'catch', 'yield', 'comptime', 'extern', 'unsafe', 'dyn', 'spawn',
  'par', 'where', 'mut', 'ref', 'super',
  '枚举', '特征', '实现', '类型', '模组', '匹配', '情形', '做',
  '捕获', '产出', '编译期', '外部', '不安全', '动态', '派生', '并行',
  '约束',
]);

// 样例分三组：
//   1. 真实可编译语法 —— 必须有独立 scope
//   2. 死 token（词法器有、解析器不认）—— 必须**没有**关键字 scope，落回 variable.other
//   3. 边界情况 —— 容易误染的构造
const SAMPLES = [
  // ---- 1. 真实可编译 ----
  'let x = 1', 'var y = 2', 'const Z = 3',
  'let s = "hi\\n"', "let c = 'a'", 'let b = true', 'let n = null',
  'let h = 0xFF', 'let f = 1.5e3', 'let big = 1_000_000',
  'fn add(a: Int, b: Int) -> Int {', 'fn f() -> void {', 'fn g(a, b) {',
  'if a { } else if b { } else { }', 'while a { }', 'loop { break }', 'loop { continue }',
  'for x in 0..3 { }', 'for x in 0..=3 { }',
  'return x', 'let v = true or false', 'let w = not true', 'defer println(1)',
  'let r = a < b && c > d || e != f', 'let t = a << 2 | b & c ^ d',
  'let q = a >> 1', 'let u = a >= b', 'let p = a <= b', 'let eq = a == b', 'let ne = a != b',
  'let as1 = a & 1', 'let oor = a | 1', 'let xx = a ^ 1', 'let nn = ~a', 'let bang = !a',
  'print("x")', 'len(arr)', 'array(3, 0)', 'toInt(1.5)', 'toFloat(1)', 'toString(1)', 'abs(-1)', 'println(1)',
  'import @mathutil', 'import @mathutil as u',
  'pub fn f2() { }',

  // ---- 中文，逐项与英文对应 ----
  '令 x = 1', '变 y = 2', '常 Z = 3',
  '函 square(n: 浮点) -> 浮点 {', '公开 函 foo(n: 整数) -> 整数 {',
  '如果 a { } 否则 { }', '当 a { }', '遍历 x 于 0..3 { }', '循环 { 跳出 }', '循环 { 继续 }',
  '返回 n', '令 a1 = 真 或 假', '令 b1 = 非 真', '延后 输出(1)',
  '真', '假', '空',
  '引入 @zh', '从 x 作为 y',

  // ---- 1b. 类系统（v0.15 / v0.16，实测可编译）----
  'struct P { x: Int }', '无', 'self.n = 1', 'fn f(self) -> Int {',
  'class Counter {', 'interface Named {', 'abstract class Shape {',
  'final class User {', 'class Dog extends Animal {',
  'class Tagged implements Named {',
  'class A { private x: Int }', 'protected y: Int', 'public z: Int',
  'override fn speak() -> String {', 'abstract fn area() -> Int',
  'static fn double(x: Int) -> Int {',
  'constructor(start: Int) {', 'let u = new User()',
  'class Mathy { static fn double(x: Int) -> Int {',
  'className(a)', 'fieldGet(a, "x")', 'annoHas(a, "class", "", "T")',
  '@Deprecated', '@Table("users")', '@Route("/x", method = "GET")',
  '类 甲 {', '接口 形状 {', '抽象 类 基 {', '终态 类 末 {',
  '类 子 继承 基 实现接口 甲 {', '私有 x: 整数', '保护 y: 整数', '公开 z: 整数',
  '覆写 函 求() -> 整数 {', '新建 甲(1)', '自身.x = 1',
  '结构 P { x: 整数 }',

  // ---- 2. 死 token：必须落回 variable.other，不能是关键字色 ----
  'enum C { Red }', 'trait S { }', 'impl P { }',
  'type T = Int', 'mod m', 'match x', 'case 1', 'do a', 'catch e', 'yield 1',
  'comptime', 'extern', 'unsafe', 'dyn', 'spawn', 'par', 'where',
  'mut a', 'ref a', 'super()',
  '枚举 C { 红 }', '特征 S { }', '实现 P { }',
  '类型 T', '模组 m', '匹配 x', '情形 1', '做 a', '捕获 e', '产出 1',
  '编译期', '外部', '不安全', '动态', '派生', '并行', '约束', '父类()',

  // ---- 3. 边界 ----
  'fn main() { let xs = array(3, 0) }',
  '@derive(Clone)',   // v0.16 起 @Name(args) 是真正的注解语法，derive 应是 annotation
  'import @mathutil',  // 这条才是包引用，mathutil 应是 namespace
  '/// 文档注释', '// 行注释', '/* 块 /* 嵌套 */ 注释 */',
];

(async () => {
  const g = await reg.loadGrammar('source.x');
  const violations = [];
  for (const line of SAMPLES) {
    const r = g.tokenizeLine(line, vsctm.INITIAL);
    const parts = [];
    for (const t of r.tokens) {
      const txt = line.substring(t.startIndex, t.endIndex);
      if (!txt.trim()) continue;
      const leaf = t.scopes[t.scopes.length - 1];
      parts.push(`${JSON.stringify(txt)}=${leaf}`);
      // 断言：死 token 绝不能拿到关键字 scope
      if (DEAD.has(txt) && /keyword|storage\.(type|modifier)/.test(leaf)) {
        violations.push(`${line}  →  ${JSON.stringify(txt)} 被染成 ${leaf}`);
      }
    }
    console.log(`\nIN : ${line}`);
    parts.forEach(p => console.log('   ' + p));
  }

  console.log('\n' + '='.repeat(72));
  if (violations.length) {
    console.log(`FAIL: ${violations.length} 个死 token 被染成了关键字色`);
    violations.forEach(v => console.log('   ' + v));
    process.exitCode = 1;
  } else {
    console.log('PASS: 没有死 token 被染成关键字色');
  }
  console.log('='.repeat(72));
})();
