// crash.c — 编译器自身的崩溃处理器（仅调试构建启用）
//
// ── 为什么编译器需要这个 ────────────────────────────────────
//
// 编译器不是普通程序：它的输入是别人写的、可能包含任意错误的源文件，
// 而它的输出是一个 exit code。于是「编译器自己崩了」和「用户程序
// 编译失败」在调用方看来**长得一模一样**——都是非零退出。
//
// 本次实测的具体损失：扫雷 demo 段错误，`echo $?` 得到 139，
// stdout 一行都没有。于是要先花两轮排查去确认「到底是编译期崩的
// 还是运行期崩的」，再花一轮去确认「是不是栈溢出」，因为这两种
// 假设都能解释「无输出 + 139」。而这两条信息本来是可以直接打印
// 出来的。
//
// 所以这里做两件最小的事：
//   1. 用 SetUnhandledExceptionFilter 抓住 Windows 的 SEH 异常，
//      把异常码（0xC0000005 = 访问违例，0xC00000FD = 栈溢出）
//      和出错地址打印出来。这两个码的区别直接决定了往哪查。
//   2. 打印一份「迷你回溯」：从栈上扫可执行段范围内的候选返回地址。
//
// ── 为什么只做可执行段过滤，不做完整栈展开 ─────────────────
//
// 完整展开要读 PE 的 .pdata / unwind info，几百行起步。而这里真正
// 需要的只是「崩在哪个函数附近」。用 VirtualQuery 拿每页的
// MEM_COMMIT 属性、再用模块基址范围粗筛，就能得到五六个候选地址。
// 配合 addr2line 足够定位。宁可粗，不要写一个自己会崩的调试器。
//
// 用法：
//   X_CRASH=1 x.exe build foo.xle      # 运行期开启（需 -DX_DEBUG 构建）
//   然后用 tools/where.sh <地址> 反查源码行

#include "platform.h"

#if X_DEBUG && X_HOST_WINDOWS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 为什么这里不 include <windows.h> ────────────────────────
//
// windows.h 会拖进 wtypes.h，它的 VARIANT 枚举（VT_INT / VT_BOOL /
// VT_ARRAY ...）和本项目值类型的枚举名撞车。codegen_c.c 已经被迫
// 把生成代码里的枚举改名为 XV_*，那是产物侧的妥协，不该在编译器
// 自己身上再来一次。
//
// 崩溃处理器只需要四五个声明，手写比引头文件便宜：
//   - PVOID / DWORD / ULONG_PTR 用标准类型代替
//   - 需要的那几个 Win32 函数自己声明
typedef void*         CPVOID;
typedef unsigned long CDWORD;
typedef unsigned long long CULONG_PTR;

typedef struct {
    CDWORD     ExceptionCode;
    CDWORD     ExceptionFlags;
    void*      ExceptionRecord;
    void*      ExceptionAddress;
    CDWORD     NumberParameters;
    CULONG_PTR ExceptionInformation[15];
} CExcRecord;

typedef struct {
    CDWORD     Type;
    CDWORD     Protection;
    void*      BaseAddress;
} CMemBasic;

// SetUnhandledExceptionFilter 的回调收到的是 **EXCEPTION_POINTERS***，
// 而不是 EXCEPTION_RECORD*。这是个很容易搞错的地方：
//
//     typedef struct _EXCEPTION_POINTERS {
//         PEXCEPTION_RECORD ExceptionRecord;   // 里面才有异常码与地址
//         PCONTEXT          ContextRecord;     // 里面才有寄存器和栈指针
//     } EXCEPTION_POINTERS;
//
// 直接把它当 EXCEPTION_RECORD 读会得到「异常码 0xD8FFD570，未知异常」
// 这种垃圾值 —— 第一版就是这么写的，浪费了一轮。所以这里显式地把
// 两个指针拆出来，不去猜。
typedef struct {
    void* exception_record;   // CExcRecord*
    void* context_record;     // CContext*
} CExcPointers;

// x64 的 CONTEXT 结构。这里只需要 Rsp / Rbp / Rip 三个字段，
// 但它们的偏移是由整个结构的前置字段决定的，不能只写自己那一段 ——
// 所以把前面的字段声明成占位数组，保证偏移量正确。
//
// 偏移依据 winnt.h（x64）：
//   P1Home..P6Home  0x00..0x2F
//   ContextFlags    0x30   MxCsr 0x34
//   SegCs..SegSs    0x38..0x43   EFlags 0x44
//   Dr0..Dr7        0x48..0x77
//   Rax..R15        0x78..0xF7
//   Rip             0xF8
//
// 用 unsigned char 占位而不是罗列全部寄存器名，是为了让「这个结构
// 只有偏移重要」这件事在代码里就看得出来。
typedef struct {
    unsigned char pad_to_rsp[0x98];
    unsigned long long rsp;    // 0x98
    unsigned long long rbp;    // 0xA0
    unsigned char pad_to_rip[0x50];
    unsigned long long rip;    // 0xF8
} CContext;

typedef long (*CTopFilter)(void*);

extern void*             GetModuleHandleW(const wchar_t*);
extern CMemBasic*        VirtualQuery(const void*, CMemBasic*, unsigned long long);
extern int               VirtualProtect(void*, unsigned long long, CDWORD, CDWORD*);
extern int               IsDebuggerPresent(void);
extern void              ExitProcess(unsigned int);
extern void*             AddVectoredExceptionHandler(CDWORD, void*);
extern long              SetUnhandledExceptionFilter(CTopFilter);
extern void*             GetCurrentProcess(void);
extern int               K32EnumProcessModules(void*, void**, unsigned int, CDWORD*);
extern CDWORD            GetModuleFileNameW(void*, wchar_t*, CDWORD);
extern void*             GetProcessHeap(void);

#define CPAGE_EXECUTE           0x10
#define CPAGE_EXECUTE_READ      0x20
#define CPAGE_EXECUTE_READWRITE 0x40
#define CPAGE_EXECUTE_WRITECOPY 0x80
#define CMEM_COMMIT             0x1000

static const char* exc_name(CDWORD code) {
    switch (code) {
        case 0xC0000005: return "访问违例（读写非法地址）";
        case 0xC00000FD: return "栈溢出（递归太深或局部数组太大）";
        case 0xC0000094: return "整数除零";
        case 0xC0000095: return "整数溢出";
        case 0xC000001D: return "非法指令";
        case 0xC0000374: return "堆损坏（Heap corruption）";
        case 0xC0000409: return "栈缓冲区溢出（/GS 检测到）";
        default:         return "未知异常";
    }
}

// 把一个地址映射到「哪个模块 + 模块内偏移」。
//
// ── 为什么这一步必须做 ──────────────────────────────────────
//
// addr2line 只会解析 PE 映像内的地址。但崩溃常常发生在运行时库
// 内部（memcpy、malloc、strlen），那时 addr2line 只吐一个 `??:0`，
// 于是「崩在 msvcrt 的堆管理里」和「崩在自己的代码里但行号丢了」
// 这两种完全不同的情况看起来一模一样。
//
// ── 为什么用「枚举模块 + 范围比较」而不是 VirtualQuery ─────
//
// 第一版用的是 VirtualQuery(addr) 拿分配区基址，再和模块基址比。
// 它对**有效**地址有效，但崩溃地址不一定有效 —— 如果 rip 落在
// 未映射的页上，VirtualQuery 直接返回 0，于是得到「不可查询的
// 地址」这种零信息的结果。而这种情况恰恰是最需要知道「在哪个
// 模块」的。
//
// 所以改成用 EnumProcessModules 拿全部模块的装载区间（读每个
// 模块的 PE 头取 SizeOfImage），再做一次范围查找。只要地址落在
// 某个模块的映像范围内就能报出来，完全不依赖该地址是否可读。
static void locate_module(unsigned long long addr, char* out, size_t out_len) {
    out[0] = '\0';

    void* mods[512];
    CDWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        snprintf(out, out_len, "<枚举模块失败>");
        return;
    }
    int count = (int)(need / sizeof(void*));
    if (count > 512) count = 512;

    for (int i = 0; i < count; i++) {
        unsigned long long base = (unsigned long long)(CULONG_PTR)mods[i];
        if (!base) continue;

        // PE 头的偏移：DOS 头 e_lfanew 在 0x3C，NT 头里
        // OptionalHeader.SizeOfImage 在 PE 头之后 +0x38（PE32+）。
        //
        // 直接读可能踩到坏指针，所以先用 VirtualQuery 确认这一页
        // 可读 —— 模块头一定可读，这是个安全假设，但多一道检查
        // 成本极低（崩溃处理路径，不是热路径）。
        CMemBasic mb;
        memset(&mb, 0, sizeof(mb));
        if (!VirtualQuery((const void*)(CULONG_PTR)base, &mb, sizeof(mb))) continue;
        if (mb.Type != CMEM_COMMIT) continue;

        const unsigned char* p = (const unsigned char*)(CULONG_PTR)base;
        unsigned int lfanew = *(const unsigned int*)(p + 0x3C);
        if (lfanew == 0 || lfanew > 0x1000) continue;
        unsigned int size = *(const unsigned int*)(p + lfanew + 0x38);
        if (size == 0) continue;

        if (addr < base || addr >= base + size) continue;

        wchar_t wpath[600];
        CDWORD n = GetModuleFileNameW(mods[i], wpath, 600);
        if (n == 0) continue;

        // 宽字符 → 窄字符，只保留路径最后一段（文件名）。
        // 路径里的中文对「是哪个 DLL」没有影响，统一显示为 '?'。
        char full[768];
        int k = 0;
        for (CDWORD j = 0; j < n && k < (int)sizeof(full) - 1; j++) {
            full[k++] = (wpath[j] < 128) ? (char)wpath[j] : '?';
        }
        full[k] = '\0';

        const char* leaf = full;
        for (int j = 0; full[j]; j++) {
            if (full[j] == '\\' || full[j] == '/') leaf = &full[j + 1];
        }

        snprintf(out, out_len, "%s  (基址 0x%llX, 偏移 +0x%llX)",
                 leaf, base, addr - base);
        return;
    }

    snprintf(out, out_len,
             "<不在任何已加载模块的映像范围内 —— 可能是函数指针被写坏了>");
}

// 判断一个地址是否落在可执行页里。
//
// 栈上大部分内容（局部变量、返回地址的旧副本）都在读写页上。
// 只保留可执行页里的候选，能把上百个噪声地址压到个位数。
static int is_exec_addr(const void* p) {
    CMemBasic mbi;
    memset(&mbi, 0, sizeof(mbi));
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.Type != CMEM_COMMIT) return 0;
    CDWORD prot = mbi.Protection & 0xFF;
    return prot == CPAGE_EXECUTE || prot == CPAGE_EXECUTE_READ ||
           prot == CPAGE_EXECUTE_READWRITE || prot == CPAGE_EXECUTE_WRITECOPY;
}

// 判断一个地址是否可读（用于在遍历 rbp 链时尽早停下，而不是
// 读到一半触发二次访问违例）。
static int is_readable_addr(const void* p) {
    CMemBasic mbi;
    memset(&mbi, 0, sizeof(mbi));
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    return mbi.Type == CMEM_COMMIT;
}

// 栈回溯。
//
// ── 为什么走 rbp 链而不是扫内存 ─────────────────────────────
//
// 第一版用的是「从 rsp 往上扫 4096 个槽，只留落在可执行页里的」。
// 实测几乎不可用：x64 的栈上到处是遗留的指针碎片（参数、局部变量
// 里的函数指针、被释放对象的内容），扫出来的候选有一大半是噪声，
// 而且完全**没有顺序**——越靠近崩溃点的帧并不排在最前，因为
// 中间还夹着一堆 SEH 内部帧。
//
// rbp 链是编译器主动维护的：每个函数开头 `push rbp; mov rbp, rsp`，
// 于是 [rbp] 恒等于调用者的 rbp，[rbp+8] 恒等于返回地址。顺着走
// 就是一条精确的调用链，顺序完全正确。
//
// 前提是编译时不能用 -fomit-frame-pointer。tools/build.sh 的 debug
// 构建是 -O0 -g3，MinGW 在 -O0 下默认保留 rbp，所以成立。若将来
// 改成 -O2 调试，这里会退化，届时需要读 .pdata 做 unwind。
//
// 每一跳做一次合理性检查：新 rbp 必须比旧的大（栈向下生长）、
// 4 KB 对齐、且返回地址落在可执行页里。任一条不满足就停 ——
// 栈被写坏时早停比乱走强。
static void dump_backtrace(unsigned long long rbp, unsigned long long rip) {
    fprintf(stderr, "\n调用栈（自上而下 = 从崩溃点向外）：\n");
    fprintf(stderr, "  #%-2d 0x%016llX   <- 崩溃点\n", 0, rip);

    unsigned long long frame = rbp;
    for (int depth = 1; depth < 40; depth++) {
        unsigned long long* fp = (unsigned long long*)frame;
        if (!is_exec_addr((void*)frame) && !is_readable_addr((void*)frame)) break;

        unsigned long long next_rbp = fp[0];
        unsigned long long ret_addr = fp[1];
        if (!ret_addr) break;

        if (!is_exec_addr((void*)ret_addr)) break;
        if (next_rbp <= frame || (next_rbp & 0x7)) break;

        fprintf(stderr, "  #%-2d 0x%016llX\n", depth, ret_addr);
        frame = next_rbp;
    }
    fprintf(stderr, "\n  把上面这些地址减掉 ImageBase（0x140000000）后交给\n"
                    "  addr2line -e build/x.exe -f -C -i <偏移> 即可得到源码行：\n"
                    "    tools/where.sh 0x<地址>\n");
}

static long on_crash(void* info) {
    CExcPointers* ep = (CExcPointers*)info;
    CExcRecord* er = (CExcRecord*)ep->exception_record;
    CContext* ctx = (CContext*)ep->context_record;

    // 防御：结构体是真的才继续。这里再崩一次就彻底没信息了。
    if (!er) {
        fprintf(stderr, "\n[crash] 异常记录为空，无法诊断\n");
        fflush(stderr);
        ExitProcess(70);
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "  编译器自身崩溃（这是编译器的 bug，不是你的代码的问题）\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, "  异常码   : 0x%08lX  %s\n",
            (unsigned long)er->ExceptionCode, exc_name(er->ExceptionCode));
    fprintf(stderr, "  出错地址 : 0x%p\n", er->ExceptionAddress);
    {
        char where_[768];
        locate_module((unsigned long long)(CULONG_PTR)er->ExceptionAddress,
                      where_, sizeof(where_));
        fprintf(stderr, "  出错模块 : %s\n", where_);
    }

    // 最后一条：本进程里 x.exe 的装载基址。有了它，`出错地址 -
    // 基址` 就是 PE 内的 RVA，可以直接喂给 addr2line —— 不依赖
    // 「地址落在哪个模块」的判断，因为崩溃地址可能已经越界到
    // 模块之外了（寄存器/栈被写坏时就是这样）。
    {
        extern void* GetModuleHandleW(const wchar_t*);
        void* self = GetModuleHandleW(NULL);
        unsigned long long sb = (unsigned long long)(CULONG_PTR)self;
        unsigned long long ea = (unsigned long long)(CULONG_PTR)er->ExceptionAddress;
        fprintf(stderr, "  映像基址 : 0x%llX\n", sb);
        if (ea >= sb) {
            fprintf(stderr, "  → 直接查 : tools/where.sh 0x%llX\n", ea - sb);
        }
    }
    if (ctx) {
        fprintf(stderr, "  寄存器   : rip=0x%016llX rsp=0x%016llX rbp=0x%016llX\n",
                ctx->rip, ctx->rsp, ctx->rbp);
    }

    if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
        // 第 0 个参数是「读还是写」，第 1 个是「访问的地址」。
        // 访问地址接近 0 表示空指针解引用或结构体字段偏移算错；
        // 接近 0xCCCCCCCC / 0xDDDDDDDD 表示用到了未初始化的内存
        // （MSVC 的调试填充值）；看起来正常却崩，才是真越界。
        const char* kind = er->ExceptionInformation[0] == 0 ? "读取" : "写入";
        fprintf(stderr, "  操作     : %s地址 0x%llX\n", kind,
                (unsigned long long)er->ExceptionInformation[1]);
        unsigned long long a = (unsigned long long)er->ExceptionInformation[1];
        if (a < 0x10000) {
            fprintf(stderr, "  判断     : 接近 0 —— 空指针解引用，"
                            "或结构体字段偏移算错\n");
        } else if (a == 0xCCCCCCCCCCCCCCCCull || a == 0xDDDDDDDDDDDDDDDDull ||
                   a == 0xCDCDCDCDCDCDCDCDull) {
            fprintf(stderr, "  判断     : 调试填充值 —— 用到了未初始化的指针\n");
        }
    }
    if (er->ExceptionCode == 0xC00000FD) {
        fprintf(stderr, "  判断     : 栈溢出。本项目 VM 结构体里有 1 MB 的\n"
                        "             值栈数组，它必须靠 x_alloc 放堆上；\n"
                        "             若被误放到栈上，默认 1 MB 栈立刻不够。\n");
    }

    // 用 CONTEXT 里的 rsp 而不是擦掉帧的地址：后者是 crash.c 自己的
    // 栈帧，从那里往上扫会先穿过一堆 SEH 内部帧，`#0` 往往不是
    // 真正出事的地方。
    dump_backtrace(ctx ? ctx->rbp : 0, ctx ? ctx->rip : 0);
    fprintf(stderr, "================================================================\n\n");
    fflush(stderr);

    // 让调试器有机会接管（若被调试的话），否则直接终止。
    // 不返回：返回值会触发「继续执行」，而崩溃现场继续跑通常
    // 只会产生更多噪声，把真正的位置埋掉。
    if (!IsDebuggerPresent()) ExitProcess(70);
    return 0;
}

void crash_handler_install(void) {
    // 默认开启。理由：这个处理器只在 -DX_DEBUG 构建里存在，
    // 而调试构建的唯一用途就是找 bug —— 那时你**总是**想要更多信息。
    // 设 X_CRASH=0 可以关掉（比如要故意复现原始崩溃行为）。
    const char* v = getenv("X_CRASH");
    if (v && strcmp(v, "0") == 0) return;
    SetUnhandledExceptionFilter(on_crash);
}

#else

void crash_handler_install(void) { }

#endif