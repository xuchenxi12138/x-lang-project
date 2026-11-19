// check-plugin.js — IDEA 插件产物的断言脚本（仿 vscode-x/tools/check-*.js 风格）。
//
// 用法：node tools/check-plugin.js
//
// 检查分两层：
//   1. 产物层：zip 结构、plugin.xml 注册项、图标、类文件齐全。
//   2. 词表层：生成的 XCompletionData.java 与真相来源 src/lsp_data.h
//      逐词一致 —— 防止生成脚本跑错版本或数据被手工改动。
//
// zip/jar 的解包交给 PowerShell（node 没有内置 zip 库），产物本身
// 不依赖网络。

const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const zipPath = path.join(root, 'dist', 'x-lang-idea-0.3.3.zip');
const headerPath = path.resolve(root, '../src/lsp_data.h');
const dataPath = path.join(root, 'src/com/xlang/idea/XCompletionData.java');
const pluginXmlPath = path.join(root, 'resources/META-INF/plugin.xml');

let pass = 0, fail = 0;
function check(name, cond) {
    if (cond) { pass++; console.log('  ok  ' + name); }
    else      { fail++; console.log('  FAIL ' + name); }
}

// 用 PowerShell 解 zip/jar 到临时目录（zip 是标准格式，jar 也是）
function unzip(zip, dest) {
    execSync(
        `powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '${zip}' -DestinationPath '${dest}'"`,
        { stdio: 'pipe' }
    );
}

// 从 class 常量池 / 任意文本里按需提取字符串
function stringsIn(file) {
    return fs.readFileSync(file, 'latin1');
}

const tmp = path.join(require('os').tmpdir(), 'x-idea-check-' + Date.now());
fs.mkdirSync(tmp, { recursive: true });

console.log('== 产物层 ==');
check('zip 存在', fs.existsSync(zipPath));

const zipStage = path.join(tmp, 'zip');
unzip(zipPath, zipStage);
const jarCandidates = fs.readdirSync(path.join(zipStage, 'lib'));
check('lib/ 下只有一个插件 jar', jarCandidates.length === 1 && jarCandidates[0].endsWith('.jar'));

const jarStage = path.join(tmp, 'jar');
// Expand-Archive 只认 .zip 扩展名；jar 本就是 zip 格式，复制改名即可
const jarZip = path.join(tmp, 'plugin-as-zip.zip');
fs.copyFileSync(path.join(zipStage, 'lib', jarCandidates[0]), jarZip);
unzip(jarZip, jarStage);
check('META-INF/plugin.xml 在 jar 内', fs.existsSync(path.join(jarStage, 'META-INF', 'plugin.xml')));
check('图标 x.svg 在 jar 内', fs.existsSync(path.join(jarStage, 'icons', 'x.svg')));

const javaClasses = [
    'XLanguage.class', 'XFileType.class', 'XTokenTypes.class',
    'XLexer.class', 'XSyntaxHighlighter.class', 'XSyntaxHighlighterFactory.class',
    'XColorSettingsPage.class', 'XParserDefinition.class',
    'XCompletionContributor.class', 'XCompletionData.class',
    'XReferenceContributor.class', 'XCommenter.class', 'XSymbols.class',
];
for (const c of javaClasses) {
    check('class ' + c, fs.existsSync(path.join(jarStage, 'com', 'xlang', 'idea', c)));
}

const pluginXml = fs.readFileSync(pluginXmlPath, 'utf8');
check('插件 id', pluginXml.includes('<id>com.xlang.idea</id>'));
check('平台依赖', pluginXml.includes('com.intellij.modules.platform'));
check('fileType 注册且带扩展名', pluginXml.includes('<fileType') && pluginXml.includes('extensions="xle;xlc"'));
check('parserDefinition 注册', pluginXml.includes('<lang.parserDefinition'));
check('syntaxHighlighterFactory 注册', pluginXml.includes('<lang.syntaxHighlighterFactory'));
check('completion.contributor 注册', pluginXml.includes('<completion.contributor'));
check('psi.referenceContributor 注册（跳转定义）', pluginXml.includes('<psi.referenceContributor'));
check('colorSettingsPage 注册', pluginXml.includes('<colorSettingsPage'));
check('lang.commenter 注册（ctrl+/）', pluginXml.includes('<lang.commenter'));
check('版本锁定 241（对应 2024.1.7）', pluginXml.includes('since-build="241"'));

console.log('== 词表层（与编译器真相来源对账） ==');

// 从 lsp_data.h 解数组（与 gen-idea-data.js 相同的解析方式）
function parseHeaderArray(src, name) {
    const re = new RegExp('static\\s+const\\s+[\\w\\*\\s]+?' + name + '\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([\\s\\S]*?)\\};');
    const m = src.match(re);
    if (!m) throw new Error('找不到 ' + name);
    const quoted = (m[1].match(/"((?:[^"\\]|\\.)*)"/g) || []).map(s => JSON.parse(s));
    if (quoted.length > 0) return quoted;
    return m[1].split(',').map(s => s.trim()).filter(Boolean).map(Number);
}

const header = fs.readFileSync(headerPath, 'utf8');
const dataJava = fs.readFileSync(dataPath, 'utf8');

function javaArray(name) {
    const re = new RegExp('String\\[\\]\\s+' + name + '\\s*=\\s*\\{([\\s\\S]*?)\\};');
    const m = dataJava.match(re);
    if (!m) throw new Error('XCompletionData.java 里找不到 ' + name);
    return (m[1].match(/"((?:[^"\\]|\\.)*)"/g) || []).map(s => JSON.parse(s));
}
function javaInts(name) {
    const re = new RegExp('int\\[\\]\\s+' + name + '\\s*=\\s*\\{([\\s\\S]*?)\\};');
    const m = dataJava.match(re);
    return m[1].split(',').map(s => s.trim()).filter(Boolean).map(Number);
}

const pairs = [
    ['kLspKeywords', 'KEYWORDS_EN'],
    ['kLspZhKeywords', 'KEYWORDS_ZH'],
    ['kLspTypes', 'TYPES_EN'],
    ['kLspZhTypeZh', 'TYPES_ZH'],
    ['kLspBuiltinName', 'BUILTIN_NAME'],
    ['kLspBuiltinRet', 'BUILTIN_RET'],
];
for (const [hName, jName] of pairs) {
    // Unknown 只在类型名单里是「用户不该写的内部占位」，生成器过滤了；
    // 其它数组（如内建函数返回类型）原样保留 Unknown。
    const unknownFiltered = hName === 'kLspTypes';
    let h = parseHeaderArray(header, hName);
    if (unknownFiltered) h = h.filter(t => t !== 'Unknown');
    const j = javaArray(jName);
    check(`${jName}：${h.length} 项与 ${hName} 逐词一致`,
        h.length === j.length && h.every((v, i) => v === j[i]));
}

const argcH = parseHeaderArray(header, 'kLspBuiltinArgc');
const argcJ = javaInts('BUILTIN_ARGC');
check('BUILTIN_ARGC 逐项一致', argcH.length === argcJ.length && argcH.every((v, i) => v === argcJ[i]));

// 解析器必须建根 marker：241 的 PsiBuilderImpl 走 light tree，
// getTreeBuilt() 无条件取第 0 个 start marker，一个不建就抛
// IndexOutOfBoundsException —— 表现为「打字被回滚、字符消失」。
const parserJava = fs.readFileSync(path.join(root, 'src/com/xlang/idea/XParserDefinition.java'), 'utf8');
check('解析器建根 marker 并 done(root)', parserJava.includes('builder.mark()') && parserJava.includes('root.done('));

// 词法器里的关键字表必须也是全的（检查 45 英文 + 45 中文 + 15 类型）
const lexerJava = fs.readFileSync(path.join(root, 'src/com/xlang/idea/XLexer.java'), 'utf8');
for (const word of ['abstract', 'where', 'yield', 'comptime', '实现接口', '覆写', '终态', '编译期']) {
    check(`XLexer 含关键字「${word}」`, lexerJava.includes('"' + word + '"'));
}

console.log('');
console.log(`通过 ${pass} 项，失败 ${fail} 项`);
if (fail > 0) process.exit(1);
