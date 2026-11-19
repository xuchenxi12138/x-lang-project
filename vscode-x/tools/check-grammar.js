// tools/check-grammar.js — 断言关键 token 的 scope（高亮自检）
//
// ── 和 verify-scopes.js 的分工 ────────────────────────────────
//
// verify-scopes.js 是**观察器**：把文件里每个 token 的叶子 scope 全打出来，
// 给人看。它的价值是排查「为什么这个字没颜色」，但它不判断对错 ——
// 输出几百行，人眼扫一遍没发现异常就过去了。
//
// 这个脚本是**断言集**：写清「class 必须是 storage.type.class.x」
// 「@Table 必须是 entity.name.annotation.x」，跑完只会说 PASS / FAIL。
// 差别在于它会失败。观察器不会失败，所以它不会阻止回归。
//
// ── 为什么必须有它 ────────────────────────────────────────────
//
// 这份语法踩过两个「颜色看着正常、其实全错」的坑：
//
//   1. #builtins 从来没生效过。函数调用规则排在它前面，第一条命中即停，
//      于是 println / arrPush / className 全被标成 support.function.call。
//      视觉上是「函数色」，看起来没问题 —— 但内建和用户函数同色，
//      等于 96 个内建的 scope 白写了。
//
//   2. 接口里的无体方法（fn name() -> String，行尾没有 {）让 #fnDecl 的
//      begin/end 区域一直吞到文件尾。后果是 interface 之后的 class /
//      abstract / @注解 全部掉色。症状是「文件后半截没颜色」——
//      很容易被当成主题问题而不是语法问题。
//
// 两个坑都不会报错、都不会抛异常、都能通过「看着对」的人工检查。
// 只有断言能拦住它们。
//
// 用法：
//   mkdir -p /tmp/tm && cd /tmp/tm && npm i --no-save vscode-textmate vscode-oniguruma
//   NODE_PATH=/tmp/tm/node_modules node vscode-x/tools/check-grammar.js

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const EXT = path.resolve(HERE, '..');
const GRAMMAR = path.join(EXT, 'syntaxes/x.tmLanguage.json');

const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

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

// ── 断言集 ───────────────────────────────────────────────────
//
// 每项的期望值都是「编译器支持这个语法」的直接结果，不是顺手写的。
// 一个 token 可能被切成若干段（captures 边界），所以按**整行**断言：
// 先在行内找到目标文本，再取它所在 token 的叶子 scope。
//
// 注意 case 用 code 而不是整份测试文件：这些是原子断言，一个用例只测
// 一件事，失败时能立刻定位到是该规则本身坏了，而不是上下文干扰。
const CASES = [
  // ── 类系统声明 ──
  { code: 'class Counter { }',                    text: 'class',       want: 'storage.type.class.x' },
  { code: 'class Counter { }',                    text: 'Counter',     want: 'entity.name.type.class.x' },
  { code: 'interface Named { }',                  text: 'interface',   want: 'storage.type.class.x' },
  { code: 'interface Named { }',                  text: 'Named',       want: 'entity.name.type.class.x' },
  { code: 'struct P { x: Int }',                  text: 'struct',      want: 'storage.type.class.x' },
  { code: 'struct P { x: Int }',                  text: 'P',           want: 'entity.name.type.class.x' },

  // ── 修饰符 ──
  { code: 'abstract class A { }',                 text: 'abstract',    want: 'storage.modifier.x' },
  { code: 'final class B { }',                    text: 'final',       want: 'storage.modifier.x' },
  { code: 'class A { private x: Int }',           text: 'private',     want: 'storage.modifier.x' },
  { code: 'class A { protected x: Int }',         text: 'protected',   want: 'storage.modifier.x' },
  { code: 'class A { public x: Int }',            text: 'public',      want: 'storage.modifier.x' },
  { code: 'class A { override fn f() { } }',      text: 'override',    want: 'storage.modifier.x' },
  // static 不是关键字，靠 strcmp 认 —— 但它必须着色，否则类里的静态方法没颜色
  { code: 'class A { static fn f() { } }',        text: 'static',      want: 'storage.modifier.x' },

  // ── 继承 ──
  { code: 'class B extends A { }',                text: 'extends',     want: 'keyword.other.inheritance.x' },
  { code: 'class B extends A { }',                text: 'A',           want: 'entity.name.type.x' },
  { code: 'class B implements I { }',             text: 'implements',  want: 'keyword.other.inheritance.x' },

  // ── new 与构造 ──
  { code: 'let u = new User()',                   text: 'new',         want: 'keyword.operator.new.x' },
  { code: 'let u = new User()',                   text: 'User',        want: 'entity.name.type.class.x' },
  { code: 'constructor(n: String) { }',           text: 'constructor', want: 'entity.name.function.constructor.x' },
  // self 在**参数位置**就是参数（fn f(self)），在方法体里是语言内建引用。
  // 两条断言分开写，因为它们走的是不同规则：#params 与 variable.language。
  // 一开始只写了参数那条、期望写成 variable.language，实际拿到的是
  // variable.parameter —— 断言没错，是我的期望错了。
  { code: 'fn f(self) { }',                       text: 'self',        want: 'variable.parameter.x' },
  { code: 'self.n = 1',                           text: 'self',        want: 'variable.language.self.x' },

  // ── 注解 ──
  { code: '@Deprecated',                          text: '@',           want: 'punctuation.definition.annotation.x' },
  { code: '@Deprecated',                          text: 'Deprecated',  want: 'entity.name.annotation.x' },
  { code: '@Table("users")',                      text: 'Table',       want: 'entity.name.annotation.x' },
  { code: '@Route("/x", method = "GET")',         text: 'method',      want: 'variable.parameter.annotation.x' },

  // ── 包引用必须与注解区分开 ──
  { code: 'import @mathutil',                     text: 'mathutil',    want: 'entity.name.namespace.x' },

  // ── 内建函数（分组 scope）──
  { code: 'println("hi")',                        text: 'println',     want: 'support.function.builtin.special.x' },
  { code: 'let a = array(0, 0)',                  text: 'array',       want: 'support.function.builtin.special.x' },
  { code: 'arrPush(a, 1)',                        text: 'arrPush',     want: 'support.function.builtin.x' },
  { code: 'strUpper("a")',                        text: 'strUpper',    want: 'support.function.builtin.x' },
  { code: 'fsRead("a")',                          text: 'fsRead',      want: 'support.function.builtin.x' },
  { code: 'className(a)',                         text: 'className',   want: 'support.function.builtin.reflect.x' },
  { code: 'fieldGet(a, "x")',                     text: 'fieldGet',    want: 'support.function.builtin.reflect.x' },
  { code: 'annoHas(a, "class", "", "T")',         text: 'annoHas',     want: 'support.function.builtin.annotation.x' },
  { code: 'annoGetKV(a, "field", "x", "C")',      text: 'annoGetKV',   want: 'support.function.builtin.annotation.x' },
  // 用户函数必须和内建区分开 —— 这正是 #builtins 排错顺序时丢掉的信号
  { code: 'myFunc(1)',                            text: 'myFunc',      want: 'support.function.call.x' },

  // ── 绘制原语（v0.16 新增的 12 个）──
  //
  // 这 12 个是本组最容易漏的一批：它们和 winFillRect 同前缀，但
  // 加进 g_builtins 之后如果忘了同步语法文件，症状是「能编译、
  // 但新 API 一片灰」。而灰着看久了会以为「这函数是不是不存在」。
  // 逐个断言，漏一个就红。
  { code: 'winFillRound(0,0,10,10,4,0)',          text: 'winFillRound',   want: 'support.function.builtin.x' },
  { code: 'winBorderRound(0,0,10,10,4,1,0)',      text: 'winBorderRound', want: 'support.function.builtin.x' },
  { code: 'winEllipse(0,0,10,10,0)',              text: 'winEllipse',     want: 'support.function.builtin.x' },
  { code: 'winPath(p, 0)',                        text: 'winPath',        want: 'support.function.builtin.x' },
  { code: 'winGradient(0,0,10,10,1,2,true)',      text: 'winGradient',    want: 'support.function.builtin.x' },
  { code: 'winPushTransform(0,0,45.0,1.0)',       text: 'winPushTransform', want: 'support.function.builtin.x' },
  { code: 'winPopTransform()',                    text: 'winPopTransform',  want: 'support.function.builtin.x' },
  { code: 'winAlpha(0.5)',                        text: 'winAlpha',       want: 'support.function.builtin.x' },
  { code: 'winPopAlpha()',                        text: 'winPopAlpha',    want: 'support.function.builtin.x' },
  { code: 'winClip(0,0,10,10)',                   text: 'winClip',        want: 'support.function.builtin.x' },
  { code: 'winUnclip()',                          text: 'winUnclip',      want: 'support.function.builtin.x' },
  { code: 'winTextRot(0,0,"x",0,45.0)',           text: 'winTextRot',     want: 'support.function.builtin.x' },

  // ── 反向断言：死 token 不能被染成关键字 ──
  //
  // 这几项断言「没有关键字色」，而不是断言某个具体 scope。
  //
  // 第一版把期望值写成 variable.other.x，结果 super 那条 FAIL ——
  // 实际是 support.function.call.x。原因是 `super()` 后面有左括号，
  // 落进了函数调用规则。这不是语法文件的 bug，而是我的断言问错了问题：
  // 真正要保证的是「它**不是**关键字色」，至于它被当成函数还是变量，
  // 对一个写不出来的 token 来说不重要。
  //
  // 所以这里改成断言「不属于 keyword.* / storage.*」。
  { code: 'super()',                              text: 'super',       notKeyword: true },
  { code: 'enum Color { Red }',                   text: 'enum',        notKeyword: true },
  { code: 'trait Show { }',                       text: 'trait',       notKeyword: true },
  { code: 'impl P { }',                           text: 'impl',        notKeyword: true },

  // match 在 v0.17 由死转活（parser.c 已实现 EX_MATCH），断言随之反转：
  // 从「不是关键字」变成「必须是关键字」。
  { code: 'match x { }',                          text: 'match',       want: 'keyword.control.x' },
  { code: '匹配 x { }',                            text: '匹配',        want: 'keyword.control.zh.x' },

  // 可选链运算符。断言它单独成一条 operator scope，
  // 而不是被 `?` 吃掉 —— 那正是它被排到 `\?` 前面要防的失败模式。
  { code: 'a?.b',                                 text: '?.',          want: 'keyword.operator.optional.x' },

  // 原始字符串 r"..."（v0.17）。断言查**内容段**而不是整串：
// 引号是单独的 punctuation token，整串不会作为一个 token 出现。
  // 内容落进 string.quoted.raw.x 才说明原始串规则生效了；
  // 若落回 string.quoted.double.x，说明规则没排到普通串之前。
  { code: 'let a = r"xyz"',                       text: 'xyz',         want: 'string.quoted.raw.x' },

  // ── 内置类型 ──
  { code: 'let x: Int = 1',                       text: 'Int',         want: 'support.type.primitive.x' },
  { code: 'let x: Int = 1 (整数)',                 text: '整数',        want: 'support.type.primitive.zh.x' },
  // 签名里的返回类型。注意期望是 support.type.primitive 而非 entity.name.type：
  // #typeRefs 里先 include #builtinTypes，内置类型优先命中，只有**非内置**的
  // 大写标识符（用户类名）才落到 entity.name.type。这是对的 —— 内置类型和
  // 用户类型本来就该分色，一开始我把这条也写错了。
  { code: 'fn f() -> Int { }',                    text: 'Int',         want: 'support.type.primitive.x' },
  { code: 'fn f() -> MyThing { }',                text: 'MyThing',     want: 'entity.name.type.x' },

  // ── 用户自定义类型在「声明注解处」的使用 ──
  //
  // 0.2.3 的语法里这些名字全是 variable.other.x —— 这正是「类型关键字
  // 不高亮」投诉的另一半：内建类型有色，自定义类型整段灰着，写代码的
  // 人看不出一个名字是类型还是变量。
  { code: 'let u: User = 1',                      text: 'User',        want: 'entity.name.type.x' },
  { code: 'class B { b: Board }',                 text: 'Board',       want: 'entity.name.type.x' },
  { code: 'struct P { x: Int, y: Int }',          text: 'Int',         want: 'support.type.primitive.x' },

  // ── 中文类系统 ──
  { code: '类 甲 { }',                             text: '类',          want: 'storage.type.class.x' },
  { code: '类 甲 { }',                             text: '甲',          want: 'entity.name.type.class.x' },
  { code: '抽象 类 甲 { }',                        text: '抽象',        want: 'storage.modifier.zh.x' },
  { code: '终态 类 甲 { }',                        text: '终态',        want: 'storage.modifier.zh.x' },
  { code: '接口 甲 { }',                           text: '接口',        want: 'storage.type.class.x' },
  { code: '类 甲 继承 乙 { }',                     text: '继承',        want: 'keyword.other.inheritance.zh.x' },
  { code: '类 甲 实现接口 乙 { }',                 text: '实现接口',    want: 'keyword.other.inheritance.zh.x' },
  { code: '类 甲 实现接口 乙 { }',                 text: '乙',          want: 'entity.name.type.x' },
  { code: '私有 x: 整数',                          text: '私有',        want: 'storage.modifier.zh.x' },
  { code: '新建 甲(1)',                            text: '新建',        want: 'keyword.operator.new.x' },
  { code: '覆盖 自身.x',                           text: '自身',        want: 'variable.language.self.zh.x' },
];

// 找到「行内某段文本」所在的叶子 scope。
//
// 注意要找**整个 token 的文本**匹配，不能只比首字符 —— @Table("users")
// 里 "Table" 是完整的第二个 token，而 "Table(" 这种切法对不上。
function scopeOf(grammar, line, target) {
  const r = grammar.tokenizeLine(line, vsctm.INITIAL);
  // 有些用例把代码和注释写在同一行（如 'let x: Int = 1 (整数)'），
  // 那种情况下目标是独立的 token 文本；先精确比，再退回 indexOf。
  for (const tok of r.tokens) {
    const txt = line.substring(tok.startIndex, tok.endIndex);
    if (txt === target) return tok.scopes[tok.scopes.length - 1];
  }
  // 退回：目标可能是某 token 的一部分（如 'let x: Int = 1' 里的 Int 是独立 token，
  // 但 'new User()' 的 User 可能被新规则整段捕获）
  const idx = line.indexOf(target);
  if (idx < 0) return null;
  // 重新 tokenize，这次只看覆盖 idx 的那个 token 的**最内层** scope。
  //
  // 为什么取叶子而不是「包含 target 的所有 scope 里最具体的那个」：
  // TextMate 的 scopes 数组本身就是从外到内排列的，最后一项就是最具体的。
  // 唯一的问题是当 target 只是某个大 token 的前半段时（比如 meta.function.x
  // 区域里的 "() "），叶子会是大区域的 scope —— 那种情况我们对不上，
  // 直接报 null，让断言失败，而不是猜一个看起来合理的答案。
  for (const tok of r.tokens) {
    if (tok.startIndex <= idx && idx < tok.endIndex) {
      const txt = line.substring(tok.startIndex, tok.endIndex);
      if (txt.includes(target)) return tok.scopes[tok.scopes.length - 1];
    }
  }
  return null;
}

async function run() {
  const grammar = await registry.loadGrammar('source.x');
  if (!grammar) throw new Error('语法加载失败：source.x 未注册');

  let pass = 0;
  const fails = [];

  for (const c of CASES) {
    const line = Array.isArray(c.code) ? c.code[0] : c.code;
    const got = scopeOf(grammar, line, c.text);
    if (c.notKeyword) {
      // 「不是关键字色」类的断言：任何 keyword.* / storage.* 都算失败。
      // 关键字色是「编译器认这个词」的背书，死 token 不能拿这个背书。
      const bad = got && (/^keyword\./.test(got) || /^storage\./.test(got));
      if (!bad) { pass++; }
      else { fails.push({ ...c, line, got }); }
      continue;
    }
    if (got === c.want) {
      pass++;
    } else {
      fails.push({ ...c, line, got });
    }
  }

  // ── 附加断言：接口无体方法不能吞掉后续行 ──────────────────
  //
  // 这是那个「文件后半截掉色」的 bug 的回归测试。做法：取一段包含
  // interface + class 的多行代码，逐行 tokenize（保持 ruleStack 传递），
  // 然后检查最后一行 class 的 class 关键字是否仍然是 storage.type.class.x。
  // 如果 #fnDecl 又吞到文件尾，最后那行的 class 会落进 meta.function.x。
  const MULTI = [
    'interface Named {',
    '    fn name() -> String',
    '}',
    'class User { }',
  ];
  let stack = vsctm.INITIAL;
  let lastLine = null;
  for (const l of MULTI) {
    const r = grammar.tokenizeLine(l, stack);
    stack = r.ruleStack;
    lastLine = { line: l, tokens: r.tokens };
  }
  {
    const idx = lastLine.line.indexOf('class');
    let got = null;
    for (const tok of lastLine.tokens) {
      if (tok.startIndex <= idx && idx < tok.endIndex) {
        got = tok.scopes[tok.scopes.length - 1];
      }
    }
    if (got === 'storage.type.class.x') pass++;
    else fails.push({ text: 'class（接口之后的最后一行）', want: 'storage.type.class.x', got, line: lastLine.line, note: '无体方法吞行 bug 回归' });
  }

  console.log(`断言 ${CASES.length + 1} 条：通过 ${pass}，失败 ${fails.length}`);
  if (fails.length) {
    console.log('');
    for (const f of fails) {
      console.log(`  FAIL  ${JSON.stringify(f.text)}`);
      console.log(`        行：${f.line}`);
      console.log(`        期望 ${f.notKeyword ? '不是 keyword.* / storage.*' : f.want}`);
      console.log(`        实际 ${f.got === null ? '(找不到这个 token)' : f.got}`);
      if (f.note) console.log(`        备注：${f.note}`);
    }
    console.log('');
    console.log('失败通常意味着 syntaxes/x.tmLanguage.json 的规则顺序或 captures 编号变了。');
    console.log('改完记得重跑 tools/verify-scopes.js 看整体效果。');
    process.exit(1);
  }
}

run().catch((e) => { console.error('FAILED:', e); process.exit(1); });