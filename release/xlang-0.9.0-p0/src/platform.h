// platform.h — 平台边界：X 编译器与操作系统的唯一接触面
//
// ── 为什么要单独一个头文件 ──────────────────────────────────
//
// 编译器要在 Windows / Linux / macOS 三处都能编出来，而这三个系统的
// 差异并不多——真正需要 #ifdef 的地方只有五类：
//
//   1. 终端编码（Windows 是 GBK 代码页，类 Unix 默认 UTF-8）
//   2. argv 与文件路径的编码（同上）
//   3. 是否连着终端（isatty 的名字不同，还得拉不同头文件）
//   4. ANSI 颜色是否可用（Windows 需要显式开 VT 处理）
//   5. 可执行文件后缀（.exe / 无）
//
// 如果让这些 #ifdef 散落在 8 个 .c 文件里，后果不是「代码难看」，
// 而是**在 Linux 上第一次编译时会报一堆错，而你在 Windows 上看不出来**。
// 集中到这个头文件 + platform.c 之后，规则变成：
//
//   ★ 除 platform.c 之外，任何源文件都不许 include <windows.h>，
//     也不许出现 #ifdef _WIN32。
//
// 这条规则可以用一行 grep 验证，所以它不依赖人记住：
//     grep -rn "_WIN32\|windows.h" src/ | grep -v platform
// 输出必须为空。（tools/check_portable.sh 会自动查这一条。）
//
// ── 为什么不是「三个平台各写一份实现」 ──────────────────────
//
// 因为绝大多数平台差异是**编码**问题，不是 API 问题。类 Unix 那边
// 几乎所有函数都退化成了「拷一份字符串」，与 Windows 共享同一套
// 上层逻辑。真正的三分支只有 Windows / 类Unix 两支，macOS 与 Linux
// 在这一层完全同构。

#ifndef X_PLATFORM_H
#define X_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ============================================================
// 平台识别
// ============================================================

// 编译期常量，供少数确实需要分支的地方使用（例如生成目标文件名）。
// 注意：这是**宿主**平台（跑 x.exe 的平台），与 --target 指定的
// **目标**平台是两件事。交叉编译时二者不同，见 target.h 的说明。
#if defined(_WIN32)
#  define X_HOST_WINDOWS 1
#  define X_HOST_LINUX   0
#  define X_HOST_MACOS   0
#elif defined(__APPLE__)
#  define X_HOST_WINDOWS 0
#  define X_HOST_LINUX   0
#  define X_HOST_MACOS   1
#elif defined(__linux__)
#  define X_HOST_WINDOWS 0
#  define X_HOST_LINUX   1
#  define X_HOST_MACOS   0
#else
#  define X_HOST_WINDOWS 0
#  define X_HOST_LINUX   0
#  define X_HOST_MACOS   0
#endif

// 调试构建标记。
//
// tools/build.sh 用 `-DX_DEBUG` 打开 `./tools/build.sh debug`。这里给
// 一个默认值，是为了让源文件里可以无条件写 `#if X_DEBUG`，而不是
// 到处 `#ifdef X_DEBUG` —— 后者在「定义了但值为 0」时会走进分支，
// 是个很容易踩的坑。
#ifndef X_DEBUG
#  define X_DEBUG 0
#endif

// 宿主平台的可执行文件后缀：Windows 是 ".exe"，其余是空串。
const char* plat_exe_suffix(void);

// 宿主平台名，用于诊断输出与 --target=host 的解析。
// 返回 "windows" / "linux" / "macos" / "unknown"。
const char* plat_host_name(void);

// 宿主的完整三元组（如 "x86_64-windows-msvc"），由编译期的
// 架构宏与系统宏拼出来。交叉编译时用于确认「默认目标是宿主」。
const char* plat_host_triple(void);

// ============================================================
// 终端与编码
// ============================================================

// 把终端切到 UTF-8 并开启 ANSI 转义。
//
// 必须在任何输出之前调用（包括 usage、--version）。
// 幂等：多次调用只有第一次生效。
//
// Windows 上还会 atexit 登记还原，避免污染父 shell 的代码页——
// 这一点在 utf8.c 里有详细说明，属于「我的程序能正确显示」
// 不能以「弄坏别人的终端」为代价。
void plat_init_console(void);

// 把 stdin/stdout 切到二进制模式（仅 Windows 有效，类 Unix 空实现）。
//
// LSP 的分帧按 UTF-8 字节走，文本模式会把 '\n' 翻译成 "\r\n"，把
// "\r\n\r\n" 帧边界写成 "\r\r\n\r\r\n" —— 客户端按帧头找消息永远
// 找不到，症状是「服务器启动了但一直不响应」。所以必须在进入消息
// 循环前调用，且要在任何读写标准流之前。
//
// 可重复调用。
void plat_set_binary_stdio(void);

// 本地编码（argv 的编码）→ UTF-8。返回新分配的字符串，调用方 free。
// 类 Unix 上直接拷贝（argv 约定即 UTF-8）。
char* plat_from_locale(const char* s);

// UTF-8 → 本地编码（fopen 等系统 API 需要的编码）。返回新分配，调用方 free。
char* plat_to_locale(const char* s);

// ============================================================
// 与终端的连接状态
// ============================================================

// 这个流是否连着一个真终端（而不是管道/文件）。
// Windows 上走 _isatty，类 Unix 上走 isatty，调用方不用关心。
bool plat_is_tty(FILE* f);

// 终端是否支持 ANSI 颜色。
//
// 判据与 plat_is_tty 不同：连了终端不等于支持颜色。老式 cmd.exe
// 连终端但不支持，Windows Terminal 支持，xterm 系支持。
// 返回 false 时所有颜色转义应当退化为空串。
bool plat_supports_color(FILE* f);

// 终端是否支持 OSC 8 超链接（可点击的 file:line:col）。
// 比颜色更严格：需要 xterm 系新版本或 Windows Terminal。
bool plat_supports_osc8(FILE* f);

// 关掉颜色，用于 --no-color。
// 不通过改环境变量实现——那会污染子进程，且在某些平台上不可逆。
void plat_disable_color(void);

// ============================================================
// 文件 I/O
// ============================================================
//
// 所有文件操作都应该走这两个封装，而不是直接调 fopen：
// 直接调 fopen 在中文路径上会失败，而失败信息里的路径看着是对的，
// 极难定位。集中一处转换，就只需要在这一处保证正确。
//
// 它们放在 platform.h 而不是 x.h：这是**平台边界**的事，
// 而不是编译器内部逻辑的事。target.c 需要读产物文件头做格式自检，
// 但它不该因此依赖整个编译器（x.h 里有 AST、字节码、解释器）。
// 分层清楚了，依赖方向才是单向的。
//
// mode 与 fopen 一致（"rb" / "wb"）。
FILE* x_fopen(const char* path, const char* mode);

// 删除 / 重命名，同样接受 UTF-8 路径。
bool x_remove(const char* path);
bool x_rename(const char* from, const char* to);

// 路径是否是一个**存在的目录**。
//
// 不能用 fopen 判断：fopen 对目录在 Windows 上返回 NULL、在 Linux 上
// 却可能成功（glibc 允许 open 一个目录拿只读 fd），于是「目录存不存在」
// 这件事会因平台而异 —— 而调用方要的答案只有一个。
//
// 用 stat 系 API 是唯一可靠的判据。放平台层的理由与 x_fopen 相同：
// 中文路径必须经宽字符转换，不能直接用 ANSI 版的 stat。
bool x_is_dir(const char* path);

// 路径是否指向一个普通的、可读的文件（不是目录、不是设备）。
//
// 与 x_is_dir 成对。区分「目录在但文件名写错」与「目录根本没装」，
// 两者要给用户的建议完全不同 —— 只用一个 stat 就能分开。
bool x_file_exists(const char* path);

// ============================================================
// 目录遍历
// ============================================================
//
// 一个最简的「列目录」迭代器。标准库列表、包管理扫描 vendor 都要用。
//
// ── 为什么要包一层，不直接用 dirent ─────────────────────────
//
// Windows 的目录 API 是 FindFirstFileW / FindNextFileW，返回的是
// WIN32_FIND_DATAW；类 Unix 是 opendir / readdir。两边的结构体、
// 调用顺序、错误语义都不同。散在业务代码里就是一堆 #ifdef，
// 而本项目规定 #ifdef 只许出现在 platform.c（tools/check_portable.sh 会查）。
//
// ── 用法 ────────────────────────────────────────────────────
//
//   PlatDir d;
//   if (plat_dir_open(&d, dir)) {
//       while (plat_dir_next(&d)) {
//           d.name;      // 文件名（不含目录部分）
//           d.is_dir;    // 是否子目录
//       }
//       plat_dir_close(&d);
//   }
//
// pattern 支持 * 与 ? 通配（Windows 直接交给系统，类 Unix 侧自己匹配）。
// 传 NULL 等价于 "*"。
//
// 返回的名字**不含** "." 与 ".."，调用方不需要自己滤 —— 忘了滤会让
// 每个目录都凭空多出两项，而 ".." 被当成模块名时错误信息会非常费解。
typedef struct {
    // 平台私有状态。用业务无关的字段名，避免让调用方误以为能直接读。
    void*  handle;
    char   buf[1024];      // 名字缓冲（Windows 的 wchar 转换落点）
    char   pattern[600];
    bool   is_dir;
    const char* name;      // 指向 buf
} PlatDir;

bool plat_dir_open(PlatDir* d, const char* dir, const char* pattern);
bool plat_dir_next(PlatDir* d);
void plat_dir_close(PlatDir* d);

// ============================================================
// 子进程
// ============================================================

// 运行一个程序，返回它的退出码；启动失败返回 -1。
//
// argv 是 NULL 结尾的数组，argv[0] 是程序名或路径。
// **不经过 shell**：因此不会被文件名里的引号、分号、空格影响。
// 这是安全要求，不是风格偏好——用户给的路径直接拼进 system()
// 会让一个叫 `a" ; rm -rf / ; ".xle` 的文件变成命令。
//
// workdir 是子进程的工作目录，可为 NULL 表示继承当前目录。
//
// ── 为什么需要 workdir ──────────────────────────────────────
//
// MinGW 的 as.exe 把 .o 中间文件放在**当前工作目录**，而不是 TMP。
// 所以在中文路径的项目里编译时，无论怎么改 TMP/TEMP，汇编器都会报
//     Fatal error: can't create F:\<乱码>\ccXXXXXX.o
// 唯一可靠的办法是把子进程的 cwd 切到一个纯 ASCII 目录。
// _spawnvp 做不到这一点（它不控制 cwd），必须走 CreateProcess。
// 接口因此在这里多一个 workdir 参数 —— 这不是过度设计，
// 而是「在中文用户名 + 中文项目路径的 Windows 上能编出 exe」
// 这个需求的最低代价。
//
// envp 为 NULL 表示继承当前环境。
int plat_spawn(char* const argv[], const char* workdir);

// 同上，但把子进程看到的 TMP / TEMP 替换成 tmpdir，其余环境继承。
//
// ── 为什么单独有这个函数 ────────────────────────────────────
//
// MinGW 的 cc1/as 把 .o 中间文件写到 %TEMP%，而 Windows 的默认
// TEMP 是 C:\Users\<用户名>\AppData\Local\Temp。用户名含中文时，
// 汇编器会拼出一个不存在的路径并失败：
//     Fatal error: can't create C:\Users\<乱码>\AppData\Local\Temp\ccXXXX.o
// 即使把 cwd 切到纯 ASCII 目录也救不了——**只改 cwd 不够，TEMP 也必须改**。
// （`-pipe` 在这个 MinGW 版本上不生效，实测仍落 .o 到 TEMP。）
//
// 调用方只需要说「用这个目录当临时目录」，不需要知道
// CreateProcess 要求的环境块是一整块连续内存、以双 \0 结尾
// 这种平台细节。这个函数存在就是为了不把这个细节泄漏出去。
//
// 非 Windows 平台直接忽略 tmpdir，等价于 plat_spawn。
int plat_spawn_with_tmp(char* const argv[], const char* workdir, const char* tmpdir);

// 把相对路径转成绝对路径。失败时把 out 置成 in 的副本。
// 用于「子进程 cwd 变了之后，命令行里的路径必须仍是绝对路径」这个场景。
void plat_abs_path(const char* in, char* out, size_t out_len);

// 取当前工作目录（绝对路径，UTF-8）。
//
// 为什么需要这一层而不是直接调 getcwd：Windows 上 getcwd 用的是 ANSI
// 代码页（中文系统即 GBK），拿到的字节流不能直接和 UTF-8 的其它字符串
// 拼接 —— 拼出来的路径既开不了文件、也打印成人看不懂的乱码。
// 平台层用 GetCurrentDirectoryW 拿到宽字符，再转成 UTF-8。
//
// 成功返回 true。失败返回 false 并保持 out 的内容不确定，调用方应
// 退回 "."。
bool plat_getcwd(char* out, size_t out_len);

// 判断路径是否为纯 ASCII（无 >= 0x80 的字节）。
// MinGW 工具链在非 ASCII 路径下不可靠，这是判据。
bool plat_is_ascii(const char* s);

// 本机路径分隔符（Windows '\\'，类 Unix '/'）。
//
// 为什么要一个常量而不是到处写 '/'：Windows 的 CRT 两种都认，但
// 拼出来的路径如果和平台习惯不一致，某些老工具（尤其是 MinGW 的
// 汇编器与链接器）会把它当成相对路径。用常量拼就永远一致。
//
// 读路径时仍然两种都认 —— 见 resolve_path。写路径时只用这个。
#define PLAT_SEP (X_HOST_WINDOWS ? '\\' : '/')

// 取可执行文件所在目录（绝对路径，UTF-8，**不带**结尾分隔符）。
//
// ── 为什么标准库定位必须用它，而不是用 cwd ──────────────────
//
// `import std.json` 的语义是「语言自带的标准库」，与用户在哪个目录
// 敲命令无关。若按 cwd 找 stdlib，`cd games && x run main.xle` 就找不到
// 了 —— 而用户完全没做错任何事。按 exe 位置找才是对的：把 x.exe 和
// stdlib/ 放在一个文件夹，拷到哪里都能用。
//
// Windows 走 GetModuleFileNameW（ANSI 版本在中文路径下会拿到 GBK 字节，
// 拼出来的路径开不了文件），再由 win_conv 转 UTF-8。
// 类 Unix 走 /proc/self/exe，macOS 走 _NSGetExecutablePath。
//
// 成功返回 true。失败返回 false，调用方应退回「只按 cwd / 环境变量找」。
bool plat_exe_dir(char* out, size_t out_len);

// 当前进程号。用于给中间文件起一个不会撞名的 ASCII 名字
// （两个 x.exe 同时编译时，固定的 xgen_tmp.c 会互相覆盖）。
long plat_pid(void);

// ============================================================
// 崩溃处理器（只有 -DX_DEBUG 构建才真正生效）
// ============================================================
//
// 编译器自己崩掉时，把异常码、出错地址和一份迷你回溯打到 stderr。
// 非调试构建里是个空函数，零开销、零输出。
//
// 存在的理由见 crash.c 头部：不加这个的话，「编译器崩了」和
// 「源文件编译失败」在调用方看来都是非零退出码，排查要从零开始。
void crash_handler_install(void);

// ============================================================
// 流水线打点（等同崩溃处理器：只有 -DX_DEBUG 且 X_TRACE=1 才有输出）
// ============================================================
//
// 用法：在可能出问题的函数开头 `strace_enter("函数名")`，出口
// `strace_leave()`。release 构建里两者都是空函数。
//
// 为什么需要它：崩溃发生在没有帧指针的 CRT 里时（实测崩在 msvcrt
// 的堆管理内部），地址反查和栈回溯都拿不到信息，唯一的线索就是
// 「崩之前走到哪一步」。见 trace.c 头部。
void strace_enter(const char* what);
void strace_leave(void);

// ============================================================
// 文件读写
// ============================================================
//
// ── 为什么标准库需要它，为什么必须放在平台层 ────────────────
//
// 「读一个配置文件」「把结果写进文件」是任何程序都要做的事。
// 语言此前完全没有文件能力，连 println 以外的输出都做不到，
// 于是 std.fs / std.http（静态文件）/ std.db（落盘）全都无从谈起。
//
// 放在平台层而不是内建层，是因为真正的差异在编码与路径分隔符：
// Windows 的路径是 UTF-16 世界，类 Unix 是字节世界。上层调用方
// 只想「打开这个 UTF-8 路径的字符串」，不该知道这回事。

// 读整个文件。成功返回新分配的内存（调用方 free），失败返回 NULL。
// out_len 可为 NULL。文件大小为 0 时返回非 NULL 的空缓冲（不是 NULL）——
// 调用方区分「空文件」与「打不开」靠的就是这一点。
char* plat_file_read(const char* path, size_t* out_len);

// 写整个文件（覆盖）。成功返回 true。
bool plat_file_write(const char* path, const char* data, size_t len);

// 追加。文件不存在时创建。
bool plat_file_append(const char* path, const char* data, size_t len);

bool plat_file_exists(const char* path);
bool plat_file_remove(const char* path);
long plat_file_size(const char* path);          // 失败返回 -1
bool plat_mkdir(const char* path);               // 已存在视为成功
bool plat_is_dir(const char* path);

// 原子替换：先写 path + ".tmp"，再 rename 到 path。
//
// ── 为什么需要专门的函数 ────────────────────────────────────
//
// 「写临时文件再改名」是崩溃安全写入的**唯一**正确做法：进程在
// 写到一半时被杀，磁盘上要么是旧文件的完整内容，要么是新文件的
// 完整内容，不会是两者的混合。直接 open+write+truncate 的写法在
// 断电后会产生半截文件，而半截的数据库文件比没有数据库文件糟糕
// 得多（前者会在下次启动时被当成有效数据读进来）。
//
// 这个函数存在的意义就是让调用方**不必自己记得**这件事。
bool plat_file_replace(const char* path, const char* data, size_t len);

// ============================================================
// TCP
// ============================================================
//
// ── 为什么用不透明句柄而不是 int ────────────────────────────
//
// Windows 的 SOCKET 是 UINT_PTR（64 位上 8 字节），类 Unix 是 int
// （4 字节）。用 int 描述会在 Windows 64 位上截断句柄 —— 那种 bug
// 的表现是「第一个连接正常，第二个连接收到杂质数」，极难往 socket
// 句柄截断这个方向想。
//
// 所以句柄类型在这里抽象成 PlatSock（一个能装下任一者的小结构），
// 上层只当它是整数用。
typedef struct { long long v; } PlatSock;

#define PLAT_SOCK_INVALID ((PlatSock){ .v = -1 })

bool plat_sock_valid(PlatSock s);

// 初始化网络子系统。Windows 上要 WSAStartup，类 Unix 上是空操作。
// 幂等：重复调用安全。任何 tcp 调用之前必须先调一次。
bool plat_net_init(void);
void plat_net_shutdown(void);

// 监听。port 传 0 表示「让系统分配一个空闲端口」，实际端口用
// plat_tcp_local_port 查 —— 测试必须这么做，否则并发跑测试会撞端口。
PlatSock plat_tcp_listen(const char* bind_addr, int port);

// 接受一个连接。没有待处理连接时**不阻塞**，返回 INVALID。
//
// 为什么是非阻塞：即时模式的服务器（每帧 poll 一次）需要一个
// 「没有就继续干别的」的语义。阻塞的 accept 会让服务器在空闲时
// 卡死，连「该退出了吗」都检查不了。
PlatSock plat_tcp_accept(PlatSock listener);

// 接收。返回实际字节数；0 = 对端正常关闭；-1 = 无数据或出错。
// 同样是**非阻塞**：没有数据就返回 -1，不等待。
long plat_tcp_recv(PlatSock s, char* buf, size_t cap);

// 发送。返回实际发送字节数，-1 = 出错。
long plat_tcp_send(PlatSock s, const char* data, size_t len);

void plat_tcp_close(PlatSock s);

// 主动连接。成功返回句柄，失败返回 INVALID。
PlatSock plat_tcp_connect(const char* host, int port);

int plat_tcp_local_port(PlatSock listener);
const char* plat_tcp_peer_addr(PlatSock s, char* out, size_t out_len);

// 库里最近一次网络错误的可读描述。不只是给用户看的 —— 我们的
// 报错信息里带上它，「连不上」才变成「连不上，因为 Connection
// refused」，后者直接指向原因。
const char* plat_net_error(void);

// 对端连接是否还活着（未被关闭）。
//
// ── 为什么必须单独一个函数 ──────────────────────────────────
//
// plat_tcp_recv 把「没有数据」和「对端已关闭」都表达成「拿不到
// 东西」，因为非阻塞 recv 在两种情况下分别返回 -1(EAGAIN) 与 0。
// 上层服务端需要区分它们：前者是「等下一轮再读」，后者是「这个
// 连接已经废了，关掉收工」。
//
// 混为一谈的后果是句柄泄漏 —— 死连接永远留在表里，长期运行的服务
// 最终耗尽句柄，表现为「跑几天之后再也接不了新连接」。
//
// 实现用 MSG_PEEK 读 1 字节：对端已关闭时 recv 返回 0 且不消费数据；
// 有数据时返回 >0（peek 不影响后续读取）；没数据时返回 -1。
bool plat_tcp_alive(PlatSock s);

// 睡眠 ms 毫秒。ms <= 0 立即返回。
//
// ── 为什么不复用 GUI 层的 xgui_sleep ────────────────────────
//
// 那个函数在 gui_win32.c 里，而「等一会儿」是**与服务端、与 GUI
// 都无关**的通用需求：事件循环要限速、客户端轮询要退避、测试要
// 等待外部进程。把它放在 GUI 层的直接后果是：
//
//   · 只用到 sleep 的程序，bc_uses_gui 判为「用了窗口」，编译期
//     收到「加 --profile=desktop 重编」 —— 那条建议是有害的，
//     照做会得到 WINDOWS 子系统产物，此后 println 全部静默丢失；
//   · 非 Windows 平台上 GUI 层是桩实现，那里 xgui_sleep 是空函数，
//     于是「睡 50ms」变成立即返回，事件循环退化成忙等把 CPU 打满。
//
// 所以睡眠下沉到平台层，语言层暴露一个独立的 sleep 内建，与窗口
// 能力彻底解耦。
void plat_sleep_ms(int ms);

// 墙上时钟（Unix 纪元毫秒），用于时间戳。
//
// 为什么不能拿它量耗时：墙上时钟会被 NTP 校正，一次向后的校正就能
// 让「结束 - 开始」变成负数。量耗时用 plat_mono_ms。
int64_t plat_now_ms(void);

// 单调时钟（毫秒），只保证「后一次 >= 前一次」，绝对值无意义。
int64_t plat_mono_ms(void);

#endif // X_PLATFORM_H