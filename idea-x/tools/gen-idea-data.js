// gen-idea-data.js — 从 src/lsp_data.h 生成 IDEA 插件的补全词表 XCompletionData.java
//
// ── 为什么要有这个生成脚本 ─────────────────────────────────
//
// IDEA 插件是自包含的（不连 LSP），词表直接编进 jar。但 108 个内建
// 函数 + 45 个关键字 + 45 个中文别名，手工抄一遍必然抄错 —— 抄错一
// 个字母，用户敲到那个词时提示消失，而且没有任何报错。
//
// 唯一真相来源仍是编译器自己：lsp_data.h 由 vscode-x/tools/gen-completions.js
// 从 lexer.c / bytecode.c / compile.c 生成。这条链保证：
//   编译器认识的词 == VSCode 提示的词 == IDEA 提示的词。
//
// 用法：cd x-lang/idea-x && node tools/gen-idea-data.js --write
// 生成：src/com/xlang/idea/XCompletionData.java

const fs = require('fs');
const path = require('path');

const headerPath = path.resolve(__dirname, '../../src/lsp_data.h');
const outPath = path.resolve(__dirname, '../src/com/xlang/idea/XCompletionData.java');

function parseArray(src, name) {
    // 匹配 static const ... kLspXxx[] = { ... };
    const re = new RegExp('static\\s+const\\s+[\\w\\*\\s]+?' + name + '\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([\\s\\S]*?)\\};');
    const m = src.match(re);
    if (!m) throw new Error('在 lsp_data.h 里找不到数组 ' + name);
    const items = [];
    // 字符串数组元素带引号
    for (const s of m[1].match(/"((?:[^"\\]|\\.)*)"/g) || []) {
        items.push(JSON.parse(s));
    }
    // 数字数组元素不带引号
    if (items.length === 0) {
        for (const n of m[1].split(',').map(s => s.trim()).filter(Boolean)) {
            items.push(parseInt(n, 10));
        }
    }
    return items;
}

const src = fs.readFileSync(headerPath, 'utf8');
const kwEn = parseArray(src, 'kLspKeywords');
const kwZh = parseArray(src, 'kLspZhKeywords');
const tyZh = parseArray(src, 'kLspZhTypeZh');
const tyEn = parseArray(src, 'kLspZhTypeEn');
const tyNames = parseArray(src, 'kLspTypes');
const builtinName = parseArray(src, 'kLspBuiltinName');
const builtinArgc = parseArray(src, 'kLspBuiltinArgc');
const builtinRet = parseArray(src, 'kLspBuiltinRet');

if (builtinName.length !== builtinArgc.length || builtinName.length !== builtinRet.length) {
    throw new Error('lsp_data.h 内建函数三个数组长度不一致');
}

// "Unknown" 是编译器内部占位类型，不是用户能写出来的类型名 ——
// 高亮一个用户不该写的词就是在误导（与 tmLanguage 的取舍一致）。
const typesEn = tyNames.filter(t => t !== 'Unknown');
const typesZh = tyZh;

function jarr(name, items, comment) {
    return '    // ' + comment + '\n    public static final String[] ' + name + ' = {\n' +
        items.map(s => '        ' + JSON.stringify(s) + ',').join('\n') + '\n    };\n';
}

function jarrInt(name, items, comment) {
    return '    // ' + comment + '\n    public static final int[] ' + name + ' = {\n' +
        items.map(s => '        ' + s + ',').join('\n') + '\n    };\n';
}

const out = `// XCompletionData.java — 补全词表（**自动生成，不要手工编辑**）
//
// 生成命令：cd x-lang/idea-x && node tools/gen-idea-data.js --write
// 真相来源：src/lsp_data.h（由 vscode-x/tools/gen-completions.js 从编译器源码生成）
//
// ${kwEn.length} 个英文关键字、${kwZh.length} 个中文关键字、${typesEn.length} + ${typesZh.length} 个类型名、
// ${builtinName.length} 个内建函数（含参数个数与返回类型）。

package com.xlang.idea;

public final class XCompletionData {
    private XCompletionData() { }

${jarr('KEYWORDS_EN', kwEn, '英文关键字')}
${jarr('KEYWORDS_ZH', kwZh, '中文关键字（.xlc 文件 / 中文模式）')}
${jarr('TYPES_EN', typesEn, '内建类型英文名')}
${jarr('TYPES_ZH', typesZh, '内建类型中文别名')}
${jarr('BUILTIN_NAME', builtinName, '内建函数名')}
${jarrInt('BUILTIN_ARGC', builtinArgc, '内建函数参数个数（-1 表示可变参数）')}
${jarr('BUILTIN_RET', builtinRet, '内建函数返回类型')}
}
`;

if (process.argv.includes('--write')) {
    fs.writeFileSync(outPath, out);
    console.log('已生成 ' + outPath);
} else {
    process.stdout.write(out);
}
