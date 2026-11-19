// src/lspClient.js — 极简 LSP 客户端
//
// ── 为什么不用 vscode-languageclient ────────────────────────
//
// 官方那个库是一整套工程：文档同步、能力协商、feature 注册、
// 诊断合并、进度上报、多份 workaround。它能装的场景很好用，但
// 它自己也带依赖树，而本扩展的全部代码量不到 1000 行、只有一个
// 服务端、只用到 4 个能力。
//
// 更要紧的是打包：这个扩展要能在没装 Node 工具链的机器上装。
// 引入 vscode-languageclient 就得先 npm install 才能 vsce package，
// 一旦那台机器连不上 npm 或版本对不上，整个扩展就打不出来了。
// 零依赖的代价是这里多写 200 行，换来的是「clone 下来就能打包」。
//
// ── 协议里真正容易错的地方 ─────────────────────────────────
//
// 1. 分帧必须是字节级。Content-Length 数的是 UTF-8 字节，不是
//    字符 —— 中文一个字 3 字节。用 JS 字符串长度算就会断在半个
//    汉字上，客户端表现为「响应永远不完整」。
// 2. 位置编码必须是 UTF-16 码元。VSCode 的 Position.character 是
//    这个单位，而服务端内部按 UTF-8 字节走，两边各自换算，不要在
//    中间再插一层「字符数」。
// 3. 服务端可执行文件要能被找到。找不到时必须明确告诉用户去改
//    哪个设置项，而不是静默不工作 —— 静默不工作正是这次要修的
//    那个 bug 的形态。
const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');

const LANG_ID = 'x';
// 与服务端 capabilities 里的 textDocumentSync 对齐：1 = 全量同步。
// 全量在超大文件上费带宽，但换来的是「服务端永远有一份完整正确
// 的文本」，不会因为增量区间算错而错位。
const TEXT_DOC_SYNC_FULL = 1;

class LspClient {
    constructor(exePath, cwd) {
        this.exePath = exePath;
        this.cwd = cwd;
        this.proc = null;
        this.buf = Buffer.alloc(0);
        this.pending = new Map();
        this.nextId = 1;
        this.ready = false;
        this.lastError = '';
        // 崩溃重启相关：stopping 为真表示「我们自己要关的」，此时退出不重启。
        this.stopping = false;
        this.restartTimer = null;
        this.initRoot = null;
        this.initCaps = null;
        this.onRestarted = null;
        this._out = vscode.window.createOutputChannel('X Language Server');
    }

    log(s) {
        this._out.appendLine(s);
    }

    // ── 启动 ────────────────────────────────────────────────
    start() {
        if (this.proc) return true;
        if (!this.exePath || !fs.existsSync(this.exePath)) {
            this.lastError = `找不到 x 可执行文件：${this.exePath || '(未设置)'}`;
            this.log(this.lastError);
            return false;
        }
        this.log(`启动：${this.exePath} lsp`);

        this.proc = cp.spawn(this.exePath, ['lsp'], {
            cwd: this.cwd,
            stdio: ['pipe', 'pipe', 'pipe'],
            windowsHide: true,
        });

        this.proc.stdout.on('data', (d) => this._onData(d));
        this.proc.stderr.on('data', (d) => {
            // 服务端把诊断写 stderr。它不污染协议通道，但值得记录：
            // JSON 解析失败之类的问题只在这里可见。
            const s = d.toString().trimEnd();
            if (s) this.log(s);
        });
        this.proc.on('exit', (code) => {
            this.log(`服务端退出，code=${code}`);
            this.proc = null;
            this.ready = false;
            for (const [, v] of this.pending) v.reject(new Error('服务端已退出'));
            this.pending.clear();
            // 不是我们主动关的，就重启。
            //
            // 之前没有这一步：服务端一旦退出（编译器被重新编译、被外部
            // 杀掉、偶发崩溃），扩展就永久哑掉 —— 补全/悬停/跳转全部
            // 静默返回 null，界面上和「插件没装」一模一样，只有重新加载
            // 窗口才能恢复。这次 VSCode 里 Ctrl+左键失效就是这么来的：
            // 进程 16:13 就退了，窗口之后一直活着。
            if (!this.stopping) this._scheduleRestart();
        });
        this.proc.on('error', (e) => {
            this.lastError = `无法启动服务端：${e.message}`;
            this.log(this.lastError);
        });
        return true;
    }

    // ── 分帧读取 ────────────────────────────────────────────
    _onData(d) {
        this.buf = Buffer.concat([this.buf, d]);
        for (;;) {
            const sep = this.buf.indexOf('\r\n\r\n');
            if (sep < 0) return;
            const header = this.buf.slice(0, sep).toString('ascii');
            const m = /Content-Length:\s*(\d+)/i.exec(header);
            if (!m) {
                // 头部没有长度：这条没法读，扔掉头部继续找下一条。
                // 不能在这里 return —— 否则整条流就卡死了。
                this.log(`无法识别的头部：${header.slice(0, 120)}`);
                this.buf = this.buf.slice(sep + 4);
                continue;
            }
            const len = parseInt(m[1], 10);
            const bodyStart = sep + 4;
            if (this.buf.length < bodyStart + len) return; // 还没收全
            const body = this.buf.slice(bodyStart, bodyStart + len).toString('utf8');
            this.buf = this.buf.slice(bodyStart + len);

            let msg;
            try {
                msg = JSON.parse(body);
            } catch (e) {
                this.log(`响应不是合法 JSON（${e.message}）：${body.slice(0, 200)}`);
                continue;
            }
            this._dispatch(msg);
        }
    }

    _dispatch(msg) {
        // 服务端发起的请求：本服务端一个都没有，但按协议必须回应，
        // 否则它可能等着我们。回 null 是安全的兜底。
        if (msg.id !== undefined && msg.method) {
            this._write({
                jsonrpc: '2.0',
                id: msg.id,
                result: null,
            });
            return;
        }
        // 通知（无 id）：服务端不发诊断，忽略即可。
        if (msg.id === undefined) return;

        const p = this.pending.get(msg.id);
        if (!p) return;
        this.pending.delete(msg.id);
        if (msg.error) p.reject(new Error(msg.error.message || 'LSP 错误'));
        else p.resolve(msg.result);
    }

    _write(obj) {
        if (!this.proc || !this.proc.stdin.writable) return false;
        const s = JSON.stringify(obj);
        const b = Buffer.from(s, 'utf8');
        // 头体必须一次写完：中间插进别的写入会让客户端读错长度。
        this.proc.stdin.write(`Content-Length: ${b.length}\r\n\r\n`);
        this.proc.stdin.write(b);
        return true;
    }

    request(method, params, timeoutMs = 10000) {
        if (!this.proc) {
            return Promise.reject(new Error('服务端未运行'));
        }
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`请求超时：${method}`));
            }, timeoutMs);
            this.pending.set(id, {
                resolve: (v) => { clearTimeout(t); resolve(v); },
                reject: (e) => { clearTimeout(t); reject(e); },
            });
            this._write({ jsonrpc: '2.0', id, method, params });
        });
    }

    notify(method, params) {
        this._write({ jsonrpc: '2.0', method, params });
    }

    async initialize(rootUri, capabilities) {
        // 记下初始化参数：崩溃重启后要用同一份参数重新握手。
        this.initRoot = rootUri || null;
        this.initCaps = capabilities || {};
        const res = await this.request('initialize', {
            processId: process.pid,
            rootUri: rootUri || null,
            capabilities: capabilities || {},
            workspaceFolders: rootUri ? [{ uri: rootUri, name: 'root' }] : null,
        }, 20000);
        this.notify('initialized', {});
        this.ready = true;
        return res;
    }

    // 退出后延迟重启，并重新 initialize / 重发 didOpen。
    _scheduleRestart() {
        if (this.restartTimer) return;
        this.log('服务端意外退出，1 秒后重启');
        this.restartTimer = setTimeout(async () => {
            this.restartTimer = null;
            if (this.stopping) return;
            if (!this.start()) return;
            try {
                await this.initialize(this.initRoot, this.initCaps);
                this.log('服务端已重启');
                if (this.onRestarted) this.onRestarted();
            } catch (e) {
                this.log(`重启后初始化失败：${e.message}`);
            }
        }, 1000);
    }

    async stop() {
        this.stopping = true;
        if (this.restartTimer) {
            clearTimeout(this.restartTimer);
            this.restartTimer = null;
        }
        if (!this.proc) return;
        const p = this.proc;
        try {
            await this.request('shutdown', null, 2000);
            this.notify('exit', null);
        } catch (e) {
            // 超时或已退出：协议不强制要求优雅，直接杀掉。
        }
        // request 依赖 this.proc 非空，所以上面那次请求必须在前。
        this.proc = null;
        this.ready = false;
        // ── 必须真的杀掉，并解开管道 ────────────────────────
        //
        // 早先这里是 setTimeout(300) 里 kill。两个问题：
        //   1. 定时器让进程多活 300ms，而在测试脚本里这 300ms 内
        //      node 认为还有活动句柄，于是不退出 —— 表现是脚本
        //      "跑完了但不结束"，上层管道永久挂住。
        //   2. 只 kill 不 destroy 流，stdout/stderr 的读端没有
        //      收尾事件，句柄仍被算作活跃。
        //
        // 所以：立刻 kill，并主动 destroy 三个流。这不会影响
        // 编辑器里的正常关闭（那里进程随窗口一起走），但能让
        // 「脚本结束」这件事变得确定。
        try { p.kill(); } catch (e) { /* 已死 */ }
        for (const s of [p.stdin, p.stdout, p.stderr]) {
            try { if (s && s.destroy) s.destroy(); } catch (e) { /* 已关 */ }
        }
    }

    dispose() {
        this.stop();
        this._out.dispose();
    }
}

// ── 位置换算：VSCode Position ↔ LSP Position ─────────────────
//
// VSCode 的 Position.character 已经是 UTF-16 码元偏移，与本服务端
// 声明的 positionEncoding 一致，所以这里**不做任何换算**。
// 保留这个注释是刻意的：以后有人看到「为什么要写个恒等函数」时
// 能立刻明白，而不是顺手改成「按字节算」——那会让含中文的行全错。
function toLspPos(p) {
    return { line: p.line, character: p.character };
}

// ── 把 LSP 的 CompletionList 转成 VSCode 的补全项 ─────────────
function toCompletionItems(list) {
    const items = (list && list.items) || [];
    const out = [];
    for (const it of items) {
        // LSP 的 kind 是数字，VSCode 的是枚举，两边编号不同，必须映射。
        // 漏映射的后果是「所有补全项图标都一样」，不难看但会误导。
        let kind = vscode.CompletionItemKind.Text;
        switch (it.kind) {
            case 2:  kind = vscode.CompletionItemKind.Method; break;
            case 3:  kind = vscode.CompletionItemKind.Function; break;
            case 5:  kind = vscode.CompletionItemKind.Field; break;
            case 6:  kind = vscode.CompletionItemKind.Variable; break;
            case 7:  kind = vscode.CompletionItemKind.Class; break;
            case 8:  kind = vscode.CompletionItemKind.Interface; break;
            case 9:  kind = vscode.CompletionItemKind.Module; break;
            case 12: kind = vscode.CompletionItemKind.Value; break;
            case 13: kind = vscode.CompletionItemKind.Enum; break;
            case 14: kind = vscode.CompletionItemKind.Keyword; break;
            case 15: kind = vscode.CompletionItemKind.Snippet; break;
            case 21: kind = vscode.CompletionItemKind.Constant; break;
            case 22: kind = vscode.CompletionItemKind.Struct; break;
            case 25: kind = vscode.CompletionItemKind.TypeParameter; break;
        }
        const ci = new vscode.CompletionItem(it.label, kind);
        if (it.insertText) {
            // 带括号的条目用 snippet 才能把光标放进括号里
            const hasParen = it.insertText.includes('(');
            if (hasParen && it.insertFormat === 2) {
                ci.insertText = new vscode.SnippetString(it.insertText);
            } else {
                ci.insertText = it.insertText;
            }
        }
        if (it.detail) ci.detail = it.detail;
        if (it.documentation) {
            ci.documentation = typeof it.documentation === 'string'
                ? it.documentation
                : it.documentation.value;
        }
        // sortText：让本文档符号排在内建前面。编码器给的 sortText
        // 已经带好前缀（0xx 本文档 / 2xx 内建），这里只用过滤。
        if (it.sortText) ci.sortText = it.sortText;
        if (it.filterText) ci.filterText = it.filterText;
        out.push(ci);
    }
    return out;
}

function toHover(h) {
    if (!h || !h.contents) return null;
    const c = h.contents;
    const md = typeof c === 'string' ? c : (c.value || '');
    if (!md) return null;
    const m = new vscode.MarkdownString(md);
    m.isTrusted = false;
    return new vscode.Hover(m);
}

// 把 LSP 的 SymbolKind 数字映射成 VSCode 的枚举
//
// ── 这一层为什么必须有 ──────────────────────────────────────
//
// 两边的编号完全不是一回事：LSP 里 Function = 12、Variable = 13；
// 而 VSCode 的 SymbolKind 里 Function = 11、Variable = 12、Struct = 22。
// 直接把 LSP 的数字塞进 DocumentSymbol 不会报错，只会得到错误的
// 图标与分组 —— 大纲里函数显示成「结构体」，而所有项都落在同一个
// 分类下。这种错很难从界面上反推回代码。
//
// 早先这里只映射了 12/13/14 三个值，还把 12 当成 Struct（那是把
// 另一套编号记混了），结果所有函数都不被认成函数。
function toSymbolKind(lspKind) {
    switch (lspKind) {
        case 1:  return vscode.SymbolKind.Module;
        case 2:  return vscode.SymbolKind.Namespace;
        case 3:  return vscode.SymbolKind.Package;
        case 4:  return vscode.SymbolKind.Class;
        case 5:  return vscode.SymbolKind.Method;
        case 6:  return vscode.SymbolKind.Property;
        case 7:  return vscode.SymbolKind.Field;
        case 8:  return vscode.SymbolKind.Constructor;
        case 9:  return vscode.SymbolKind.Enum;
        case 10: return vscode.SymbolKind.Interface;
        case 11: return vscode.SymbolKind.Function;   // LSP: Function
        case 12: return vscode.SymbolKind.Function;   // LSP: Function（本服务端用的是 12）
        case 13: return vscode.SymbolKind.Variable;   // LSP: Variable
        case 14: return vscode.SymbolKind.Constant;   // LSP: Constant
        case 22: return vscode.SymbolKind.Struct;
        case 23: return vscode.SymbolKind.Event;
        case 24: return vscode.SymbolKind.Operator;
        case 26: return vscode.SymbolKind.TypeParameter;
        default: return vscode.SymbolKind.Variable;
    }
}

// ── 激活 ────────────────────────────────────────────────────
//
// 两个要点：
//  1. onLanguage:x 激活。扩展在打开 .xle 之前完全不启动，不拖编辑器
//     启动速度。
//  2. 用 registerCompletionItemProvider 等**每个能力单独注册**，
//     而不是依赖 languageclient 的中间件。中件层能做的事这里不需要，
//     显式注册让「哪个能力没生效」变得可定位。
function activate(context) {
    const out = vscode.window.createOutputChannel('X Language');

    const cfg = () => vscode.workspace.getConfiguration('xlang');

    // ── 找 x.exe ────────────────────────────────────────────
    //
    // 早先这里只查「工作区根/build/x.exe」，而实际开发时工作区往往是
    // 仓库根（F:/项目/x-project），x.exe 在 x-lang/build/ 下一层 ——
    // 于是永远找不到，退回裸名 'x'，spawn 失败，代码提示全无。
    //
    // 这个 bug 之所以隐蔽：它不报错，因为裸名 'x' 看起来是个合法的
    // 命令名，只是 PATH 里没有。而「PATH 里没有」和「路径写错了」在
    // 界面上表现完全一样：没提示。
    //
    // 现在按三级找：
    //   1. 设置项显式指定的路径（最高优先，用户说了算）
    //   2. 工作区根目录下的一层子目录：<root>/*/build/x.exe
    //      —— 覆盖「仓库根打开、编译器在子项目里」这个最常见情况
    //   3. 工作区根目录的 build/ 与根目录本身
    //   4. PATH
    function exeNames() {
        const win = process.platform === 'win32';
        return win ? ['x.exe'] : ['x'];
    }

    function resolveExe() {
        const explicit = (cfg().get('executablePath') || '').trim();
        if (explicit) return { p: explicit, how: '设置项 xlang.executablePath' };

        const names = exeNames();
        const folders = (vscode.workspace.workspaceFolders || [])
            .filter(f => f.uri.scheme === 'file');

        for (const f of folders) {
            const base = f.uri.fsPath;

            // 二级：根/build/、根/ 本身
            for (const nm of names) {
                for (const rel of [['build', nm], [nm]]) {
                    const p = path.join(base, ...rel);
                    if (fs.existsSync(p)) return { p, how: `工作区 ${p}` };
                }
            }

            // 一级子目录：<root>/*/build/x.exe、<root>/*/x.exe
            //
            // 限深一层是刻意的。往下递归搜索整个目录树会在打开大仓库时
            // 扫几十万文件，而扩展激活是在打开文件的关键路径上 ——
            // 卡住的代价远大于「多一层就找不到」的不便。
            let subs = [];
            try {
                subs = fs.readdirSync(base, { withFileTypes: true })
                    .filter(d => d.isDirectory() && !d.name.startsWith('.')
                        && d.name !== 'node_modules')
                    .map(d => path.join(base, d.name));
            } catch (e) { /* 没权限读就算了，继续下一级判断 */ }

            for (const sub of subs) {
                for (const nm of names) {
                    for (const rel of [['build', nm], [nm]]) {
                        const p = path.join(sub, ...rel);
                        if (fs.existsSync(p)) return { p, how: `子目录 ${p}` };
                    }
                }
            }
        }

        // 最后交给系统 PATH 解析（spawn 支持裸名字，existsSync 不行）
        return { p: names[0].replace('.exe', ''), how: '系统 PATH' };
    }

    const found = resolveExe();
    const exe = found.p;
    const root = (vscode.workspace.workspaceFolders || [])
        .filter(f => f.uri.scheme === 'file')
        .map(f => f.uri.toString())[0] || null;

    const client = new LspClient(exe, root ? vscode.Uri.parse(root).fsPath : undefined);
    // 把「用哪个 exe、怎么找到的」写进日志。排查时第一眼看这里 ——
    // 「找到了」和「找到了个错的东西」在功能上都表现为「没提示」，
    // 而只有这一行能区分它们。
    out.appendLine(`x.exe：${exe}（来源：${found.how}）`);
    if (!client.start()) {
        // 起不来就要说清楚。这一条是这次修 bug 的核心教训：
        // 之前扩展根本没有任何语言服务，于是「代码提示没生效」这件事
        // 完全静默 —— 用户只能反复猜配置。现在起不来至少会有提示。
        vscode.window.showWarningMessage(
            `X Language：${client.lastError}。请在设置里配置 xlang.executablePath 指向 x.exe。`
        );
        out.appendLine(client.lastError);
    }

    client.initialize(root, {
        textDocument: {
            synchronization: { dynamicRegistration: false },
            completion: {
                dynamicRegistration: false,
                completionItem: { snippetSupport: true },
            },
            hover: { dynamicRegistration: false, contentFormat: ['markdown', 'plaintext'] },
            signatureHelp: { dynamicRegistration: false },
            documentSymbol: { dynamicRegistration: false, hierarchicalDocumentSymbolSupport: true },
        },
        workspace: { workspaceFolders: true },
    }).then((res) => {
        out.appendLine('服务端就绪：' + JSON.stringify(res && res.serverInfo || {}));
    }).catch((e) => {
        out.appendLine('初始化失败：' + e.message);
        vscode.window.showErrorMessage(`X Language 语言服务初始化失败：${e.message}`);
    });

    // ── 文档同步 ────────────────────────────────────────────
    //
    // 服务端声明 textDocumentSync:1（全量），所以 didChange 发整份文本。
    // 发增量要自己维护版本链和区间换算，出错时是「补全结果对不上代码」，
    // 比浪费一点带宽难查得多。
    const sendOpen = (doc) => {
        if (doc.languageId !== LANG_ID) return;
        client.notify('textDocument/didOpen', {
            textDocument: {
                uri: doc.uri.toString(),
                languageId: LANG_ID,
                version: doc.version,
                text: doc.getText(),
            },
        });
    };

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(sendOpen),
        vscode.workspace.onDidChangeTextDocument((e) => {
            const doc = e.document;
            if (doc.languageId !== LANG_ID) return;
            if (!e.contentChanges || e.contentChanges.length === 0) return;
            client.notify('textDocument/didChange', {
                textDocument: { uri: doc.uri.toString(), version: doc.version },
                contentChanges: [{ text: doc.getText() }],
            });
        }),
        vscode.workspace.onDidCloseTextDocument((doc) => {
            if (doc.languageId !== LANG_ID) return;
            client.notify('textDocument/didClose', {
                textDocument: { uri: doc.uri.toString() },
            });
        })
    );

    // 已经打开着的文档：activate 可能晚于 onDidOpenTextDocument
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === LANG_ID) sendOpen(doc);
    }

    // 服务端崩溃重启后，新进程手里没有已打开文档的副本，
    // 必须重发 didOpen，否则跳转/悬停会当成「文件没打开」。
    client.onRestarted = () => {
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.languageId === LANG_ID) sendOpen(doc);
        }
    };

    const guard = (fn) => async (...args) => {
        if (!client.proc) return null;
        try {
            return await fn(...args);
        } catch (e) {
            out.appendLine(`请求失败：${e.message}`);
            return null;
        }
    };

    // ── 补全 ────────────────────────────────────────────────
    // 中文用户的输入习惯是「边打边看」，所以补全必须对每个字符都响应。
    // 触发字符在 package.json 里声明了 . @ (，其余交给默认（字母/中文）。
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            { language: LANG_ID, scheme: 'file' },
            {
                provideCompletionItems: guard(async (doc, pos, token) => {
                    const r = await client.request('textDocument/completion', {
                        textDocument: { uri: doc.uri.toString() },
                        position: toLspPos(pos),
                    });
                    if (!r) return null;
                    return new vscode.CompletionList(toCompletionItems(r), false);
                }),
            },
            '.', '@', '(', ' '
        )
    );

    // ── 悬停 ────────────────────────────────────────────────
    context.subscriptions.push(
        vscode.languages.registerHoverProvider(
            { language: LANG_ID, scheme: 'file' },
            {
                provideHover: guard(async (doc, pos) => {
                    const r = await client.request('textDocument/hover', {
                        textDocument: { uri: doc.uri.toString() },
                        position: toLspPos(pos),
                    });
                    return toHover(r);
                }),
            }
        )
    );

    // ── 参数提示 ────────────────────────────────────────────
    context.subscriptions.push(
        vscode.languages.registerSignatureHelpProvider(
            { language: LANG_ID, scheme: 'file' },
            {
                provideSignatureHelp: guard(async (doc, pos) => {
                    const r = await client.request('textDocument/signatureHelp', {
                        textDocument: { uri: doc.uri.toString() },
                        position: toLspPos(pos),
                    });
                    if (!r || !r.signatures || !r.signatures.length) return null;
                    const sh = new vscode.SignatureHelp();
                    sh.signatures = r.signatures.map(s => ({
                        label: s.label,
                        documentation: s.documentation
                            ? (typeof s.documentation === 'string'
                                ? s.documentation
                                : s.documentation.value)
                            : undefined,
                        parameters: (s.parameters || []).map(p => ({ label: p.label })),
                    }));
                    sh.activeSignature = r.activeSignature || 0;
                    sh.activeParameter = r.activeParameter || 0;
                    return sh;
                }),
            },
            '(',
            ','
        )
    );

    // ── 定义跳转（ctrl+鼠标左键 / F12）──────────────────────
    //
    // 这是编辑器体验的底线功能：光标停在一个名字上按 ctrl+左键，
    // 必须跳到函数 / 类 / 结构体的定义处。服务端回 Location 数组
    // （LSP 里 uri/range 都是 snake_case），这里只做逐字段搬运。
    // 注意：不能注册成 `*` 全局选择器 —— 那样对所有语言的每次
    // ctrl+点击都会打到我们头上，必须限定到 x 语言。
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider(
            { language: LANG_ID, scheme: 'file' },
            {
                provideDefinition: guard(async (doc, pos) => {
                    const r = await client.request('textDocument/definition', {
                        textDocument: { uri: doc.uri.toString() },
                        position: toLspPos(pos),
                    });
                    const arr = Array.isArray(r) ? r : (r ? [r] : []);
                    return arr.map(loc => new vscode.Location(
                        vscode.Uri.parse(loc.uri),
                        new vscode.Range(
                            loc.range.start.line, loc.range.start.character,
                            loc.range.end.line, loc.range.end.character
                        )
                    ));
                }),
            }
        )
    );

    // ── 文档大纲（面包屑 + 大纲视图）────────────────────────
    //
    // 这一项最容易被忽略但感知最强：VSCode 顶部会显示「当前在哪个函数里」。
    // 语言服务不提供它就只是空白，用户会以为扩展没装好。
    context.subscriptions.push(
        vscode.languages.registerDocumentSymbolProvider(
            { language: LANG_ID, scheme: 'file' },
            {
                provideDocumentSymbols: guard(async (doc) => {
                    const r = await client.request('textDocument/documentSymbol', {
                        textDocument: { uri: doc.uri.toString() },
                    });
                    if (!r) return null;
                    return r.map(s => {
                        const range = new vscode.Range(
                            s.range.start.line, s.range.start.character,
                            s.range.end.line, s.range.end.character
                        );
                        const sel = s.selectionRange
                            ? new vscode.Range(
                                s.selectionRange.start.line, s.selectionRange.start.character,
                                s.selectionRange.end.line, s.selectionRange.end.character)
                            : range;
                        let kind = toSymbolKind(s.kind);
                        const item = new vscode.DocumentSymbol(
                            s.name,
                            s.detail || '',
                            kind,
                            range,
                            sel
                        );
                        if (s.children && s.children.length) {
                            item.children = s.children.map(c => new vscode.DocumentSymbol(
                                c.name, c.detail || '', toSymbolKind(c.kind),
                                new vscode.Range(
                                    c.range.start.line, c.range.start.character,
                                    c.range.end.line, c.range.end.character),
                                c.selectionRange
                                    ? new vscode.Range(
                                        c.selectionRange.start.line, c.selectionRange.start.character,
                                        c.selectionRange.end.line, c.selectionRange.end.character)
                                    : range));
                        }
                        return item;
                    });
                }),
            }
        )
    );

    // ── 命令：查看日志 / 重启服务 ───────────────────────────
    context.subscriptions.push(
        vscode.commands.registerCommand('xlang.showServerLog', () => client._out.show()),
        vscode.commands.registerCommand('xlang.restartServer', async () => {
            await client.stop();
            vscode.window.showInformationMessage('X Language 服务已停止，重新加载窗口后启动。');
        })
    );

    context.subscriptions.push({ dispose: () => client.dispose() });
}

function deactivate() {
    // 客户端在 context.subscriptions 里注册了 dispose，VSCode 会调用。
    // 这里留空是刻意的：deactivate 里再 stop 一遍会与 dispose 竞争。
}

module.exports = { activate, deactivate };