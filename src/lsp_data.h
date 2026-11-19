// lsp_data.h — 补全词表（**自动生成，不要手工编辑**）
//
// 生成命令：cd vscode-x && node tools/gen-completions.js --write
//
// 唯一真相来源是编译器自己：
//   src/lexer.c     kKeywords / kZhAliases / kZhTypeAliases
//   src/bytecode.c  g_builtins
//   src/compile.c   ty_from_name
//
// ── 为什么编进 x.exe 而不是运行时读 JSON ────────────────
//
// LSP 服务端由 x.exe 自己当（见 src/lsp.c）。词表若放在外部
// 文件里，就多了一种失败模式：文件被删、被挪、版本不匹配时
// 补全静默为空。编进产物之后，能启动就一定有词表。

#ifndef X_LSP_DATA_H
#define X_LSP_DATA_H

static const char* const kLspKeywords[] = {
    "as", "break", "const", "continue", "defer", "else", "false", "fn", "for", "from", "if",
    "import", "in", "let", "loop", "match", "not", "null", "or", "pub", "public", "return",
    "self", "struct", "true", "var", "void", "while", "class", "extends", "implements",
    "abstract", "interface", "override", "private", "protected", "new", "final",
};
#define kLspKeywords_COUNT ((int)(sizeof(kLspKeywords) / sizeof(kLspKeywords[0])))

static const char* const kLspZhKeywords[] = {
    "函", "令", "变", "常", "结构", "引入", "从", "公开", "自身", "真", "假", "空", "作为", "非", "如果", "否则",
    "当", "遍历", "于", "循环", "跳出", "继续", "匹配", "返回", "延后", "或", "无", "类", "继承", "实现接口", "抽象",
    "接口", "覆写", "私有", "保护", "新建", "终态",
};
#define kLspZhKeywords_COUNT ((int)(sizeof(kLspZhKeywords) / sizeof(kLspZhKeywords[0])))

static const char* const kLspZhTypeZh[] = {
    "整数", "浮点", "布尔", "字符串", "字符", "复数", "数组", "任意", "无",
};
#define kLspZhTypeZh_COUNT ((int)(sizeof(kLspZhTypeZh) / sizeof(kLspZhTypeZh[0])))

static const char* const kLspZhTypeEn[] = {
    "Int", "Float", "Bool", "String", "Char", "Complex", "Array", "Any", "void",
};
#define kLspZhTypeEn_COUNT ((int)(sizeof(kLspZhTypeEn) / sizeof(kLspZhTypeEn[0])))

static const char* const kLspBuiltinName[] = {
    "winOpen", "winClose", "winShouldClose", "winBeginFrame", "winEndFrame", "winFillRect",
    "winBorderRect", "winText", "winTextWidth", "winTextHeight", "winPollClick",
    "winMouseX", "winMouseY", "winMouseDown", "winTime", "winSleep", "winScreenshot",
    "winFillRound", "winBorderRound", "winEllipse", "winPath", "winGradient",
    "winPushTransform", "winPopTransform", "winAlpha", "winPopAlpha", "winClip",
    "winUnclip", "winTextRot", "arrPush", "arrPop", "arrInsert", "arrRemove", "arrSlice",
    "arrConcat", "arrReverse", "arrIndexOf", "arrContains", "arrFill", "arrSort", "arrCopy",
    "strAt", "strSub", "strFind", "strFindFrom", "strSplit", "strJoin", "strUpper",
    "strLower", "strTrim", "strReplace", "strStarts", "strEnds", "strRepeat", "strCode",
    "strFromCode", "strParseInt", "strParseFloat", "math", "random", "randomSeed", "fail",
    "typeOf", "fsRead", "fsReadOr", "fsWrite", "fsAppend", "fsExists", "fsIsDir",
    "fsRemove", "fsSize", "fsList", "fsMkdir", "fsReplace", "netListen", "netAccept",
    "netRecv", "netSend", "netClose", "netConnect", "netPort", "netPeer", "netError",
    "netPending", "sleep", "nowMillis", "nowSeconds", "monoMillis", "strFromBytes",
    "strBytes", "className", "classParent", "classIs", "classFields", "classMethods",
    "fieldGet", "fieldSet", "fieldHas", "methodHas", "annoHas", "annoGet", "annoGetKV",
    "annoAll", "classAll", "methodParams", "winPollKey", "winPollWheel", "winKeyDown",
    "winWidth", "winHeight", "winSetTitle", "winSetFont", "winSetCursor", "winClipGet",
    "winClipSet", "winOpenFile", "winSaveFile", "winMessage", "winSetResizable", "memRead8",
    "memRead16", "memRead32", "memWrite8", "memWrite16", "memWrite32", "portIn8",
    "portIn16", "portOut8", "portOut16", "cpuHlt", "cpuCli", "cpuSti", "cpuInvlpg", "print",
    "println", "toInt", "toFloat", "toString", "len", "array", "abs", "classNew",
    "methodCall",
};
#define kLspBuiltinName_COUNT ((int)(sizeof(kLspBuiltinName) / sizeof(kLspBuiltinName[0])))

static const int kLspBuiltinArgc[] = {
    3, 0, 0, 0, 0, 5, 6, 4, 1, 0, 0, 0, 0, 1, 0, 1, 1, 6, 7, 5, 2, 7, 4, 0, 1, 0, 4, 0, 5,
    2, 1, 3, 2, 3, 2, 1, 2, 2, 2, 1, 1, 2, 3, 2, 3, 2, 2, 1, 1, 1, 3, 2, 2, 2, 1, 1, 1, 1,
    -1, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 1, 2, 1, 1, 0, 1, 1, 0, 0,
    0, 1, 1, 1, 1, 2, 1, 1, 2, 3, 2, 2, 4, 4, 4, 3, 0, 2, 0, 0, 1, 0, 0, 1, 3, 1, 0, 1, 3,
    3, 3, 1, 1, 1, 1, 2, 2, 2, 1, 1, 2, 2, 0, 0, 0, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1,
};
#define kLspBuiltinArgc_COUNT ((int)(sizeof(kLspBuiltinArgc) / sizeof(kLspBuiltinArgc[0])))

static const char* const kLspBuiltinRet[] = {
    "Bool", "Null", "Bool", "Null", "Null", "Null", "Null", "Null", "Int", "Int", "Array",
    "Int", "Int", "Bool", "Float", "Null", "Bool", "Null", "Null", "Null", "Null", "Null",
    "Null", "Null", "Null", "Null", "Null", "Null", "Null", "Int", "Unknown", "Int",
    "Unknown", "Array", "Array", "Null", "Int", "Bool", "Null", "Null", "Array", "String",
    "String", "Int", "Int", "Array", "String", "String", "String", "String", "String",
    "Bool", "Bool", "String", "Int", "String", "Int", "Float", "Unknown", "Float", "Null",
    "Null", "String", "String", "String", "Bool", "Bool", "Bool", "Bool", "Bool", "Int",
    "Array", "Bool", "Bool", "Int", "Int", "String", "Int", "Null", "Int", "Int", "String",
    "String", "Bool", "Null", "Int", "Int", "Int", "String", "Array", "String", "String",
    "Bool", "Array", "Array", "Unknown", "Null", "Bool", "Bool", "Bool", "Array", "Array",
    "Array", "Array", "Array", "Array", "Int", "Bool", "Int", "Int", "Null", "Bool", "Null",
    "String", "Bool", "String", "String", "Int", "Null", "Int", "Int", "Int", "Null",
    "Null", "Null", "Int", "Int", "Null", "Null", "Null", "Null", "Null", "Null", "Unknown",
    "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown",
};
#define kLspBuiltinRet_COUNT ((int)(sizeof(kLspBuiltinRet) / sizeof(kLspBuiltinRet[0])))

static const char* const kLspTypes[] = {
    "Int", "Float", "Bool", "String", "Array", "void", "Void", "Unknown",
};
#define kLspTypes_COUNT ((int)(sizeof(kLspTypes) / sizeof(kLspTypes[0])))

#endif // X_LSP_DATA_H
