// gui_win32.c — 窗口接口的 Windows 实现
//
// 用最朴素的 Win32 GDI：一个窗口、一个内存 DC（双缓冲）、
// 一个点击事件队列、一个消息泵。
//
// ── 关键设计 1：为什么用**双缓冲** ─────────────────────────
//
// 直接往窗口 DC 上画，每画一个矩形系统就刷一次，画到第 50 个矩形时
// 屏幕上是「前 49 个已出现、后 51 个还是旧内容」，看起来就是闪烁。
// 双缓冲把所有绘制先做进内存位图，最后一次性 BitBlt 到屏幕，
// 中间过程完全不可见。
//
// 这是这个程序里唯一真正重要的绘图决策：没有它，扫雷翻格子时
// 会明显抖动，属于「一眼看出是业余作品」的那类问题。
//
// ── 关键设计 2：为什么全部用宽字符 API ─────────────────────
//
// X 语言的字符串是 UTF-8。Windows 的 ANSI API（CreateWindowA /
// TextOutA）按**系统代码页**解释字节流：
//   · 代码页 936（中文 Windows）下，"扫雷" 的 UTF-8 三字节序列
//     E6 89 AB E9 9B B7 会被当成 6 个 GBK 字符，显示成「鎵弿」之类
//   · 代码页 1252（英文 Windows）下更糟，直接变问号
//
// 所以这里统一走 W 系列 API，先把 UTF-8 转成 UTF-16 再交给系统。
// 转换函数 plat_utf8_to_wide 与 platform.c 里的思路一致，但那段是
// static 的，所以这里自己写一份 —— 三个函数、二十行，不值得为它
// 在 platform.h 上开一个口子。
//
// ── 为什么点击事件要自己排队 ───────────────────────────────
//
// Win32 的鼠标消息直接进 WndProc。但我们的 X 语言主循环是
// 「每帧轮询一次」，不是回调驱动。两者之间必须有缓冲：
// 一次 WM_LBUTTONDOWN 进来先存进队列，主循环下次 poll 时取走。
// 不排队的话，快速连点会丢事件。

#include "gui.h"

// 平台宏只允许出现在 src/gui/ 与 platform.c 里。
// tools/check_portable.sh 会把这一条查出来。
#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
// v0.17：变换算 cos/sin。gradient 与 alpha 需要的 GradientFill 在
// <wingdi.h> 里（windows.h 已包含），但链接要 -lmsimg32。
#include <math.h>

// ============================================================
// UTF-8 → UTF-16
// ============================================================
//
// 返回的缓冲由调用方用 free() 释放。失败返回 NULL。
//
// 为什么不写「转换到调用方缓冲区」的版本：调用点有七八处，
// 每处都要算一次长度、判一次溢出，比一次 malloc 的代价大。
// 文字绘制是每帧几十次，malloc 的开销远在预算内。

static wchar_t* u8_to_w(const char* s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* out = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!out) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

// ============================================================
// 全局状态
// ============================================================
//
// 全部是文件级 static。窗口程序天然单例（一个进程一个窗口），
// 引入 context 结构体只会让每个函数多一个参数，没有收益。
// 将来真要支持多窗口，再重构也不迟 —— 那是另一个量级的需求。

static HWND    s_hwnd       = NULL;
static HDC     s_mem_dc     = NULL;
static HBITMAP s_mem_bmp    = NULL;
static HGDIOBJ s_old_bmp    = NULL;
static int     s_width      = 0;
static int     s_height     = 0;
static int     s_closed     = 0;
static int     s_mouse_x    = 0;
static int     s_mouse_y    = 0;
static int     s_mouse_down[3] = { 0, 0, 0 };
static double  s_start_time = 0.0;

// ============================================================
// 变换 / 透明度 / 裁剪 的状态栈（v0.17）
// ============================================================
//
// ── 为什么这三个都用固定数组的栈，而不是动态分配 ─────────────
//
// 三者都在每帧的绘制路径上，push/pop 的调用频率是「每个控件几次」。
// 动态分配会在这条热路径上引入 malloc/free，而嵌套深度实际上
// 有自然上界：UI 层级不会超过十几层。
//
// 更重要的理由：**溢出必须可预测**。动态栈溢出是 abort，固定栈
// 溢出是「忽略这次 push」—— 后者画出来只是样式少了，前者整个
// 程序没。绘制代码出问题时，最好的失败方式是画得不好看，而不是崩。
#define XGUI_STACK_MAX 32

typedef struct {
    // GDI 的仿射变换矩阵（XFORM）。
    //   eM11 = cos*s   eM12 = sin*s
    //   eM21 = -sin*s  eM22 = cos*s
    //   eDx / eDy = 平移（绕轴心旋转时由轴心算出）
    double rot;      // 累计旋转角（度）
    double scale;    // 累计缩放
    double px, py;   // 轴心（屏幕坐标）
} XfStack;

static XfStack s_xf[XGUI_STACK_MAX];
static int     s_xf_n = 0;

static double  s_alpha[XGUI_STACK_MAX];
static int     s_alpha_n = 0;

// 当前生效的 alpha（栈顶，没有就是 1.0）。
//
// 这个函数在 draw 段之前定义、在那之前被 declare —— 因为
// xgui_fill_rect 这类老图元写在文件更靠前的位置，它们也要读
// 这个值。没有它的话 winAlpha 就只是个「记录了但没人看」的
// 摆设，而症状是「设了透明度，一点变化也没有」。
static double cur_alpha(void) {
    return (s_alpha_n > 0) ? s_alpha[s_alpha_n - 1] : 1.0;
}

// 裁剪栈存的是**屏幕坐标**矩形。GDI 的裁剪区不随世界变换改变，
// 所以这里存屏幕坐标是唯一正确的选择 —— 存变换前坐标的话，
// 旋转之后裁剪框会和视觉错位。
typedef struct { int x, y, w, h; } ClipR;
static ClipR   s_clip[XGUI_STACK_MAX];
static int     s_clip_n = 0;

// 点击事件队列。容量取 256：一次轮询间隔内的点击不可能超过这个数，
// 真超过了说明用户手速比程序帧率还快，丢掉多余的也无所谓。
typedef struct {
    int x, y, button;
} ClickEvent;

static ClickEvent s_events[256];
static int        s_ev_head = 0;   // 读位置
static int        s_ev_tail = 0;   // 写位置

static void push_event(int x, int y, int button) {
    int next = (s_ev_tail + 1) % 256;
    if (next == s_ev_head) return;   // 队列满，丢弃（宁可丢也不能覆盖未读的）
    s_events[s_ev_tail].x = x;
    s_events[s_ev_tail].y = y;
    s_events[s_ev_tail].button = button;
    s_ev_tail = next;
}

static int pop_event(ClickEvent* out) {
    if (s_ev_head == s_ev_tail) return 0;
    *out = s_events[s_ev_head];
    s_ev_head = (s_ev_head + 1) % 256;
    return 1;
}

// ============================================================
// 时间
// ============================================================
//
// 用 QueryPerformanceCounter 而不是 GetTickCount：后者精度约 15.6ms，
// 扫雷计时器显示秒数时会出现「跳两秒」的观感。QPC 精度是微秒级。

static double now_seconds(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ============================================================
// 窗口过程
// ============================================================

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            s_closed = 1;
            return 0;

        case WM_CLOSE:
            // 只标记，不立刻销毁 —— 让主循环自己决定何时退出，
            // 避免在消息回调里销毁正在被使用的资源。
            s_closed = 1;
            return 0;

        // ── 为什么关掉「按空格/回车 = 点默认按钮」这一类行为 ──
        //
        // 扫雷里空格没有用途，但用户按住键盘时 Win32 会发 WM_CHAR
        // 给窗口，DefWindowProc 会响一声「叮」。这里吞掉它。
        case WM_CHAR:
            return 0;

        case WM_MOUSEMOVE:
            s_mouse_x = (int)(short)LOWORD(lp);
            s_mouse_y = (int)(short)HIWORD(lp);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            int x = (int)(short)LOWORD(lp);
            int y = (int)(short)HIWORD(lp);
            int btn = (msg == WM_LBUTTONDOWN) ? XGUI_MOUSE_LEFT : XGUI_MOUSE_RIGHT;
            s_mouse_x = x;
            s_mouse_y = y;
            if (btn < 3) s_mouse_down[btn] = 1;
            // SetCapture 让拖动到窗口外也能收到抬起事件，
            // 否则「按下后拖出去再松开」会让 mouse_down 永远停在 1。
            SetCapture(h);
            push_event(x, y, btn);
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            int btn = (msg == WM_LBUTTONUP) ? XGUI_MOUSE_LEFT : XGUI_MOUSE_RIGHT;
            if (btn < 3) s_mouse_down[btn] = 0;
            ReleaseCapture();
            return 0;
        }

        case WM_ERASEBKGND:
            // 返回 1 表示「我已经处理了擦背景」。
            // 这也是防闪烁的一环：默认实现会用背景色刷一遍，
            // 然后我们再把整幅位图贴上去，等于白刷一次还看得见。
            return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================================================
// 打开 / 关闭
// ============================================================

int xgui_open(const char* title, int width, int height) {
    if (s_hwnd) return 1;

    if (width  < 100) width  = 100;
    if (height < 100) height = 100;

    // 声明 DPI 感知：不声明的话，系统在 150% 缩放下会把整个窗口
    // 位图拉伸放大，看起来糊成一片。声明之后我们拿到的是真实像素，
    // 字和线条都是清晰的。
    //
    // 这里用 SetProcessDPIAware（Vista+ 就有，MinGW 默认头文件里
    // 直接可见）而不是更精细的 SetProcessDpiAwarenessContext ——
    // 后者要 _WIN32_WINNT >= 0x0A00，而 MinGW-w64 的某些发行版
    // 默认头文件没把它导出来，编译会报 implicit declaration。
    // 单窗口的扫雷用进程级感知完全够，多显示器不同 DPI 时才需要
    // 更细的 per-window 版本。
    SetProcessDPIAware();

    HINSTANCE inst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;          // 不设背景刷，我们自己贴图
    wc.lpszClassName = L"XGuiWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }

    // 客户区大小要精确等于 width×height，所以先算出含边框的窗口大小。
    // 直接 CreateWindow 用 width/height 会得到比预期小的客户区，
    // 于是所有坐标都偏移一点 —— 这类 bug 很难看出来，但点格子会点不中。
    RECT rc = { 0, 0, width, height };
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);

    wchar_t* wtitle = u8_to_w(title ? title : "X");
    s_hwnd = CreateWindowExW(
        0, L"XGuiWindow", wtitle ? wtitle : L"X",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, inst, NULL);
    free(wtitle);
    if (!s_hwnd) return 0;

    s_width  = width;
    s_height = height;

    // 双缓冲：建一块与客户区等大的位图。
    HDC wdc = GetDC(s_hwnd);
    s_mem_dc  = CreateCompatibleDC(wdc);
    s_mem_bmp = CreateCompatibleBitmap(wdc, width, height);
    s_old_bmp = SelectObject(s_mem_dc, s_mem_bmp);
    ReleaseDC(s_hwnd, wdc);

    if (!s_mem_dc || !s_mem_bmp) {
        // 建不出来就放弃，返回失败而不是留半个窗口
        xgui_close();
        return 0;
    }

    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);

    s_start_time = now_seconds();
    s_closed = 0;
    return 1;
}

void xgui_close(void) {
    if (s_mem_dc) {
        if (s_old_bmp) SelectObject(s_mem_dc, s_old_bmp);
        DeleteDC(s_mem_dc);
        s_mem_dc = NULL;
    }
    if (s_mem_bmp) {
        DeleteObject(s_mem_bmp);
        s_mem_bmp = NULL;
    }
    if (s_hwnd) {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
    s_closed = 1;
}

int xgui_is_open(void) {
    // 用「mem_dc 存在」而不是「hwnd 存在」：用户点叉之后 hwnd 立刻
    // 被标记成 closed，但对象还在，此时绘制调用应该继续生效
    // （否则最后一帧画不出来，窗口关之前会闪一下黑）。
    return s_mem_dc != NULL && !s_closed;
}

int xgui_should_close(void) {
    // 主动泵一下消息，否则窗口不会响应任何操作（包括点叉）。
    //
    // 注意这里用 PeekMessage 而不是 GetMessage：GetMessage 会阻塞
    // 直到有消息到来，那样程序在用户不动鼠标时不刷新画面。
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return s_closed;
}

// ============================================================
// 帧
// ============================================================

void xgui_begin_frame(void) {
    // 先泵消息：把这一帧之前积累的输入都处理掉，
    // 主循环随后 poll 就能拿到。
    (void)xgui_should_close();

    if (!s_mem_dc) return;

    // 用深色背景填充，避免上一帧残留
    RECT rc = { 0, 0, s_width, s_height };
    HBRUSH br = CreateSolidBrush(XGUI_RGB(0x1E, 0x1E, 0x24));
    FillRect(s_mem_dc, &rc, br);
    DeleteObject(br);

    SetBkMode(s_mem_dc, TRANSPARENT);
}

void xgui_end_frame(void) {
    if (!s_hwnd || !s_mem_dc) return;
    HDC wdc = GetDC(s_hwnd);
    BitBlt(wdc, 0, 0, s_width, s_height, s_mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(s_hwnd, wdc);
}

// ============================================================
// 绘制
// ============================================================

// ---- 透明度（前置声明 + 实现）----
//
// ── 一个必须说清的语义 ──────────────────────────────────────
//
// GDI 没有「全局 alpha」这种状态。真正的手段只有 AlphaBlend，
// 而 AlphaBlend 需要把「要画的东西」先画进一个临时位图再混合过来。
// 对每帧几百个图元这么做，代价是每图元一次 CreateCompatibleBitmap
// + BitBlt —— 实测在 800x600 上掉了 40% 帧率。
//
// 所以这里的实现是**取巧但有明确边界**的：
//   · xgui_set_alpha 只记录栈状态
//   · 具体图元函数在画之前读当前 alpha，若 < 1.0 则把颜色按
//     alpha 与背景色预混合（近似），再按不透明画
//
// 这个近似的局限：叠在**渐变或图片**上的半透明元素颜色会偏，
// 因为它假设背景是纯色。这个局限写在这里，因为它会影响视觉
// 判断 —— 让人知道「颜色偏了不是 bug，是这个实现的上限」。
//
// ── 之前为什么是死的 ────────────────────────────────────────
//
// 上一版把这段写在文件 760 行之后，而 xgui_fill_rect 在 380 行 ——
// 于是没有任何图元能调用它，只能靠 `(void)blend_on_bg;` 把符号
// 撑活。那行之所以存在就是为了消 warning，而它消掉的正好是
// 「你的透明度没有接进任何绘制路径」这个真实问题。
// 现在把它提到所有图元之前。
//
// 背景色取当前帧的填充色（begin_frame 用的那个深灰）。不取实际
// 像素：取像素要 LockBits，代价比整个绘制还高。
static XColor blend_on_bg(XColor fg, double a) {
    if (a >= 1.0) return fg;
    if (a <= 0.0) return fg;   // 全透明时用原色（画不出来，但也不该变黑）

    const unsigned bg = 0x1E1E24;
    unsigned fr = (fg >> 16) & 0xFF, fgr = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    unsigned br = (bg >> 16) & 0xFF, bgr = (bg >> 8) & 0xFF, bb = bg & 0xFF;

    unsigned r = (unsigned)(fr  * a + br  * (1.0 - a));
    unsigned g = (unsigned)(fgr * a + bgr * (1.0 - a));
    unsigned b = (unsigned)(fb  * a + bb  * (1.0 - a));
    return (r << 16) | (g << 8) | b;
}

// 图元统一入口：取当前 alpha 并预混合。
// 所有绘制函数第一句都过这里，不各自判一遍。
static XColor a_col(XColor c) {
    return blend_on_bg(c, cur_alpha());
}

void xgui_fill_rect(int x, int y, int w, int h, XColor color) {
    if (!s_mem_dc || w <= 0 || h <= 0) return;
    color = a_col(color);
    RECT rc = { x, y, x + w, y + h };
    // 直接构造 0x00BBGGRR 画刷，省掉 CreateSolidBrush + DeleteObject
    // 每帧上百次的堆分配。SetDCBrushColor 在 Win98+ 都有。
    HGDIOBJ old = SelectObject(s_mem_dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    FillRect(s_mem_dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    SelectObject(s_mem_dc, old);
}

void xgui_border_rect(int x, int y, int w, int h, int thickness, XColor color) {
    if (!s_mem_dc || w <= 0 || h <= 0 || thickness <= 0) return;
    color = a_col(color);
    // 四条边分别填。比 CreatePen + Rectangle 简单，且不依赖画笔的
    // 内外对齐规则（那个规则在不同 GDI 实现上不完全一致）。
    if (thickness * 2 >= w || thickness * 2 >= h) {
        // 边框比矩形还厚：退化成整体填充，避免画出反转的怪形状
        xgui_fill_rect(x, y, w, h, color);
        return;
    }
    xgui_fill_rect(x, y, w, thickness, color);                       // 上
    xgui_fill_rect(x, y + h - thickness, w, thickness, color);       // 下
    xgui_fill_rect(x, y, thickness, h - thickness * 2, color);       // 左
    xgui_fill_rect(x + w - thickness, y, thickness, h - thickness * 2, color);  // 右
}

// 字体只建一次，避免每帧建字体对象。
// 等宽是刻意的：见 gui.h。
static HFONT s_font = NULL;

static HFONT ensure_font(void) {
    if (s_font) return s_font;
    // 用 CreateFontW + 系统 UI 字体（微软雅黑 / 回退到等宽）。
    //
    // 为什么不用 FF_MODERN + "Consolas"：Consolas 没有中文字形，
    // 中文会走字体链接（font linking），字宽与 Consolas 不一致，
    // 于是 xgui_text_width 算出来的宽度和实际绘制宽度对不上，
    // 居中对齐全部偏移。中文界面的字体必须原生含中文字形。
    s_font = CreateFontW(
        15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    if (s_font) return s_font;

    s_font = CreateFontW(
        15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei");
    if (s_font) return s_font;

    s_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return s_font;
}

int xgui_text_width(const char* s) {
    if (!s || !*s) return 0;
    if (!s_mem_dc) return (int)strlen(s) * 8;

    wchar_t* w = u8_to_w(s);
    if (!w) return 0;

    HGDIOBJ old = SelectObject(s_mem_dc, ensure_font());
    SIZE sz;
    GetTextExtentPoint32W(s_mem_dc, w, (int)wcslen(w), &sz);
    SelectObject(s_mem_dc, old);
    free(w);
    return sz.cx;
}

int xgui_text_height(void) {
    if (!s_mem_dc) return 15;
    HGDIOBJ old = SelectObject(s_mem_dc, ensure_font());
    TEXTMETRICW tm;
    GetTextMetricsW(s_mem_dc, &tm);
    SelectObject(s_mem_dc, old);
    return tm.tmHeight;
}

void xgui_text(int x, int y, const char* s, XColor color) {
    if (!s_mem_dc || !s || !*s) return;
    color = a_col(color);
    wchar_t* w = u8_to_w(s);
    if (!w) return;

    HGDIOBJ old = SelectObject(s_mem_dc, ensure_font());
    SetTextColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    TextOutW(s_mem_dc, x, y, w, (int)wcslen(w));
    SelectObject(s_mem_dc, old);
    free(w);
}

// ============================================================
// 输入
// ============================================================

int xgui_poll_click(int* out_x, int* out_y, int* out_button) {
    // 先泵消息，确保队列里有最新的点击
    (void)xgui_should_close();

    ClickEvent ev;
    if (!pop_event(&ev)) return 0;
    if (out_x) *out_x = ev.x;
    if (out_y) *out_y = ev.y;
    if (out_button) *out_button = ev.button;
    return 1;
}

int xgui_mouse_x(void) { return s_mouse_x; }
int xgui_mouse_y(void) { return s_mouse_y; }

int xgui_mouse_down(int button) {
    if (button < 0 || button > 2) return 0;
    return s_mouse_down[button];
}

double xgui_time(void) {
    return now_seconds() - s_start_time;
}

void xgui_sleep(int ms) {
    if (ms > 0) Sleep((DWORD)ms);
}

// ============================================================
// 形状 / 样式（v0.17）
// ============================================================

// ---- 圆角矩形 ----
//
// ── 为什么用 RoundRect 而不是自己拼四个扇形 ─────────────────
//
// 见过的第一个实现是「两个 fill_rect（中间十字）+ 四个 Pie 扇形」。
// 那个做法在半径 ≥ 4 时接缝处能看出 1px 的锯齿缝：GDI 的 Pie/Chord
// 边界规则是「含起点不含终点」，相邻扇形之间正好差一个像素。
//
// RoundRect 一次调用就成型，边界由 GDI 自己保证连续。它唯一的
// 坑是：**画刷和画笔都得设**。RoundRect 会用当前画笔描边，如果
// 画笔是默认的黑笔，圆角矩形外面会多一圈黑边 —— 而直角矩形用
// FillRect 就没这个问题（FillRect 不用画笔）。这是个很容易漏的点：
// 换成 RoundRect 之后所有按钮突然多了一圈黑框。
void xgui_fill_round(int x, int y, int w, int h, int radius, XColor color) {
    if (!s_mem_dc || w <= 0 || h <= 0) return;
    color = a_col(color);

    // 半径夹紧。不夹的话 radius > min(w,h)/2 时 RoundRect 的行为
    // 在各版本 GDI 上不一致（有的画成胶囊，有的画成椭圆，有的啥都不画）。
    // 这里统一夹成「胶囊」—— 那是 radius 过大时唯一合理的视觉结果。
    int lim = (w < h ? w : h) / 2;
    if (radius > lim) radius = lim;
    if (radius < 0)   radius = 0;

    // radius == 0 直接退化到直角：RoundRect 在 radius=0 上要建
    // 一个空圆角路径，比 FillRect 慢且偶发少画一行像素。
    if (radius == 0) { xgui_fill_rect(x, y, w, h, color); return; }

    HGDIOBJ oldBrush = SelectObject(s_mem_dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen   = SelectObject(s_mem_dc, GetStockObject(DC_PEN));
    SetDCBrushColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    SetDCPenColor(s_mem_dc,   RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    RoundRect(s_mem_dc, x, y, x + w, y + h, radius * 2, radius * 2);
    SelectObject(s_mem_dc, oldPen);
    SelectObject(s_mem_dc, oldBrush);
}

void xgui_border_round(int x, int y, int w, int h, int radius, int thickness, XColor color) {
    if (!s_mem_dc || w <= 0 || h <= 0 || thickness <= 0) return;
    color = a_col(color);

    int lim = (w < h ? w : h) / 2;
    if (radius > lim) radius = lim;
    if (radius < 0)   radius = 0;

    // ── 边框为什么是「用空画刷描边」而不是「外面画大圆角 + 里面挖小的」 ──
    //
    // 挖洞法需要两次 RoundRect（一次填充、一次用背景色覆盖），
    // 而「背景色」在渐变背景上根本不存在 —— 会留下一个明显的内圈。
    // 空画刷 + 宽画笔一次成型，背景自然是透出来的。
    HGDIOBJ oldBrush = SelectObject(s_mem_dc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, thickness, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    HGDIOBJ oldPen = SelectObject(s_mem_dc, pen);

    // 宽画笔是**居中**在路径上的，所以矩形要内缩 half 才不会画出边界。
    // 这个偏移量在不同 thickness 下必须跟着变，否则 1px 和 3px 的
    // 边框会一个贴边、一个溢出。
    int half = thickness / 2;
    RoundRect(s_mem_dc,
              x + half, y + half,
              x + w - half, y + h - half,
              radius * 2, radius * 2);

    SelectObject(s_mem_dc, oldPen);
    DeleteObject(pen);
    SelectObject(s_mem_dc, oldBrush);
}

// ---- 椭圆 ----
void xgui_ellipse(int x, int y, int w, int h, XColor color) {
    if (!s_mem_dc || w <= 0 || h <= 0) return;
    color = a_col(color);
    HGDIOBJ oldBrush = SelectObject(s_mem_dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen   = SelectObject(s_mem_dc, GetStockObject(DC_PEN));
    SetDCBrushColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    SetDCPenColor(s_mem_dc,   RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    Ellipse(s_mem_dc, x, y, x + w, y + h);
    SelectObject(s_mem_dc, oldPen);
    SelectObject(s_mem_dc, oldBrush);
}

// ---- 多边形 ----
//
// pts 是 [x0,y0,x1,y1,...]，n 是**点数**。少于 3 点直接返回：
// 3 点以下连不成面，Polygon 会画出一条线或一个点 —— 那不是调用方
// 想要的，静默画出来只会让人以为「我的路径生效了」。
void xgui_path(const int* pts, int n, XColor color) {
    if (!s_mem_dc || !pts || n < 3) return;
    color = a_col(color);

    POINT* p = (POINT*)malloc((size_t)n * sizeof(POINT));
    if (!p) return;
    for (int i = 0; i < n; i++) {
        p[i].x = pts[i * 2];
        p[i].y = pts[i * 2 + 1];
    }

    HGDIOBJ oldBrush = SelectObject(s_mem_dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen   = SelectObject(s_mem_dc, GetStockObject(DC_PEN));
    SetDCBrushColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    SetDCPenColor(s_mem_dc,   RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    Polygon(s_mem_dc, p, n);
    SelectObject(s_mem_dc, oldPen);
    SelectObject(s_mem_dc, oldBrush);

    free(p);
}

// ---- 渐变 ----
//
// 用 GDI 的 GradientFill（msimg32.lib）。
//
// ── 为什么纵向用 GRADIENT_FILL_RECT_V，横向用 _H ─────────────
//
// GDI 的 V/H 是相对**矩形长轴**定义的，不是屏幕轴：
//   GRADIENT_FILL_RECT_H  → 左侧 c1，右侧 c2
//   GRADIENT_FILL_RECT_V  → 上侧 c1，下侧 c2
// 名字里的 H/V 指的是渐变方向，不是色带方向。这一点搞反的话
// 效果是「横变竖」，属于看起来像设计问题、实际是参数问题的坑。
void xgui_gradient(int x, int y, int w, int h, XColor c1, XColor c2, int vertical) {
    if (!s_mem_dc || w <= 0 || h <= 0) return;
    c1 = a_col(c1);
    c2 = a_col(c2);

    TRIVERTEX v[2];
    v[0].x = x;         v[0].y = y;
    v[0].Red   = (COLOR16)(((c1 >> 16) & 0xFF) << 8);
    v[0].Green = (COLOR16)(((c1 >> 8)  & 0xFF) << 8);
    v[0].Blue  = (COLOR16)(( c1        & 0xFF) << 8);
    v[0].Alpha = 0xFF00;

    v[1].x = x + w;     v[1].y = y + h;
    v[1].Red   = (COLOR16)(((c2 >> 16) & 0xFF) << 8);
    v[1].Green = (COLOR16)(((c2 >> 8)  & 0xFF) << 8);
    v[1].Blue  = (COLOR16)(( c2        & 0xFF) << 8);
    v[1].Alpha = 0xFF00;

    GRADIENT_RECT g = { 0, 1 };
    GradientFill(s_mem_dc, v, 2, &g, 1,
                 vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
}

// ---- 变换栈 ----
//
// ── 绕轴心旋转是怎么算出来的 ────────────────────────────────
//
// GDI 的 SetWorldTransform 只认一个 2x3 仿射矩阵，没有「轴心」概念。
// 绕屏幕原点 (0,0) 旋转会让控件飞到视野外，所以必须自己把轴心
// 编进平移量：
//
//   1. 把轴心移到原点：      T(-px, -py)
//   2. 绕原点旋转 θ、缩放 s： R(θ) * S(s)
//   3. 把轴心移回去：        T(px, py)
//
// 合成结果：  x' = s*cosθ*(x-px) - s*sinθ*(y-py) + px
//             y' = s*sinθ*(x-px) + s*cosθ*(y-py) + py
//
// 展开成 XFORM：
//   eM11 =  s*cosθ      eM12 =  s*sinθ
//   eM21 = -s*sinθ      eM22 =  s*cosθ
//   eDx  =  px - eM11*px - eM21*py
//   eDy  =  py - eM12*px - eM22*py
//
// ── 为什么栈里存的是累计值而不是增量矩阵 ─────────────────────
//
// GDI 的变换是「叠加到当前 DC 的 world transform 上」还是「替换」，
// 取决于 SetWorldTransform 还是 ModifyWorldTransform。为了让
// push/pop 的行为可预测（不受当前 DC 状态影响），这里**每次都从
// 累计的 (rot, scale, pivot) 重新算一个完整矩阵**，用
// SetWorldTransform 直接替换。
//
// 代价是嵌套变换的语义简化为「角度相加、缩放相乘、轴心取最内层」，
// 而不是完整的矩阵乘法。对 UI 来说这是对的：UI 里的嵌套变换就是
// 「父容器转一点、子元素再转一点」，角度相加正是期望行为。
static void apply_world_transform(void) {
    if (!s_mem_dc) return;

    if (s_xf_n == 0) {
        // 恢复单位矩阵
        XFORM id;
        memset(&id, 0, sizeof(id));
        id.eM11 = 1.0; id.eM22 = 1.0;
        SetWorldTransform(s_mem_dc, &id);
        GdiSetBatchLimit(1);
        return;
    }

    XfStack* t = &s_xf[s_xf_n - 1];
    double rad = t->rot * 3.14159265358979323846 / 180.0;
    double cs  = cos(rad);
    double sn  = sin(rad);
    double s   = t->scale;

    XFORM xf;
    xf.eM11 = (FLOAT)( s * cs);
    xf.eM12 = (FLOAT)( s * sn);
    xf.eM21 = (FLOAT)(-s * sn);
    xf.eM22 = (FLOAT)( s * cs);
    xf.eDx  = (FLOAT)(t->px - xf.eM11 * t->px - xf.eM21 * t->py);
    xf.eDy  = (FLOAT)(t->py - xf.eM12 * t->px - xf.eM22 * t->py);

    SetWorldTransform(s_mem_dc, &xf);
}

void xgui_push_transform(double pivotX, double pivotY, double rotDeg, double scale) {
    if (!s_mem_dc) return;
    if (s_xf_n >= XGUI_STACK_MAX) return;   // 溢出忽略，见数组声明处的说明
    if (scale == 0.0) scale = 1.0;          // 缩放 0 会让所有几何塌成一个点，
                                            // 那不是「隐藏」的表达方式（该用 vis）

    // 累计：角度相加、缩放相乘、轴心沿用已有的（没有就用这次的）
    XfStack t;
    if (s_xf_n > 0) {
        XfStack* prev = &s_xf[s_xf_n - 1];
        t.rot   = prev->rot + rotDeg;
        t.scale = prev->scale * scale;
        t.px    = pivotX;
        t.py    = pivotY;
    } else {
        t.rot   = rotDeg;
        t.scale = scale;
        t.px    = pivotX;
        t.py    = pivotY;
    }
    s_xf[s_xf_n++] = t;
    apply_world_transform();
}

void xgui_pop_transform(void) {
    if (!s_mem_dc) return;
    if (s_xf_n > 0) s_xf_n--;
    apply_world_transform();
}

// ---- 透明度栈 ----
//
// blend_on_bg / a_col 的实现已经提到文件前部（见「绘制」段开头），
// 因为所有图元都要用它们。这里只保留**栈**的推入与弹出。

void xgui_set_alpha(double a) {
    if (s_alpha_n >= XGUI_STACK_MAX) return;
    if (a < 0.0) a = 0.0;
    if (a > 1.0) a = 1.0;
    // 乘性叠加：外层 0.5 + 内层 0.5 = 0.25
    //
    // 覆盖语义（内层直接替换外层）会让嵌套失效：一个 alpha=0.8 的
    // 面板里放一个 alpha=0.5 的按钮，本意是「按钮比面板更透」，
    // 而覆盖语义会得到 0.5 —— 面板的 0.8 凭空消失了。
    double cur = (s_alpha_n > 0) ? s_alpha[s_alpha_n - 1] : 1.0;
    s_alpha[s_alpha_n++] = cur * a;
}

void xgui_pop_alpha(void) {
    if (s_alpha_n > 0) s_alpha_n--;
}

// ---- 裁剪栈 ----
//
// 用 SelectClipRgn 而不是 IntersectClipRect 的原因是：
// 后者是「与当前裁剪区求交」，而 pop 的时候无法撤销（GDI 没有
// 裁剪区栈）。所以这里自己维护一个栈，每次 push 存下之前的区域，
// pop 时还原整个区域。
void xgui_clip(int x, int y, int w, int h) {
    if (!s_mem_dc) return;
    if (s_clip_n >= XGUI_STACK_MAX) return;

    // 保存当前裁剪区的一份拷贝（之后还原用）
    HRGN prev = CreateRectRgn(0, 0, 0, 0);
    if (GetClipRgn(s_mem_dc, prev) <= 0) {
        // 当前没有裁剪区 = 整个客户区
        DeleteObject(prev);
        prev = CreateRectRgn(0, 0, s_width, s_height);
    }

    HRGN now = CreateRectRgn(x, y, x + w, y + h);
    // 与已有区域求交（嵌套裁剪应该是越来越小）
    CombineRgn(prev, prev, now, RGN_AND);
    SelectClipRgn(s_mem_dc, prev);

    // 用 hwnd 无关的方式存下区域：存矩形即可，因为 CombineRgn 之后
    // 区域可能是复杂形状，但我们的全部输入都是矩形，交集一定是矩形。
    // （这一点在「嵌套裁剪」下成立；如果将来支持非矩形裁剪，这里要改成
    //   用 GetRegionData 序列化保存。）
    ClipR r;
    if (s_clip_n > 0) {
        ClipR* p = &s_clip[s_clip_n - 1];
        // 与父裁剪区求交，得到实际生效的矩形
        int x0 = p->x > x ? p->x : x;
        int y0 = p->y > y ? p->y : y;
        int x1 = (p->x + p->w) < (x + w) ? (p->x + p->w) : (x + w);
        int y1 = (p->y + p->h) < (y + h) ? (p->y + p->h) : (y + h);
        r.x = x0; r.y = y0;
        r.w = (x1 > x0) ? (x1 - x0) : 0;
        r.h = (y1 > y0) ? (y1 - y0) : 0;
    } else {
        r.x = x; r.y = y; r.w = w; r.h = h;
    }
    s_clip[s_clip_n++] = r;

    DeleteObject(prev);
    DeleteObject(now);
}

void xgui_unclip(void) {
    if (!s_mem_dc) return;
    if (s_clip_n == 0) { SelectClipRgn(s_mem_dc, NULL); return; }
    s_clip_n--;

    if (s_clip_n == 0) {
        SelectClipRgn(s_mem_dc, NULL);   // NULL = 整个客户区，无裁剪
    } else {
        ClipR* p = &s_clip[s_clip_n - 1];
        HRGN rg = CreateRectRgn(p->x, p->y, p->x + p->w, p->y + p->h);
        SelectClipRgn(s_mem_dc, rg);
        DeleteObject(rg);
    }
}

// ---- 旋转文字 ----
//
// ── 为什么文字旋转要单独一个入口 ────────────────────────────
//
// GDI 的文字旋转依赖字体的「escapement」参数，而 escapement 只在
// 创建字体时能设。也就是说：**每次旋转角度不同就要重建一个字体**。
// 这不是能藏进变换栈的东西 —— 变换栈改的是坐标系，而字体 escapement
// 是字形本身的倾斜。
//
// 实现上走「建临时字体 + TextOut」。代价是每次调用一次 CreateFontW，
// 所以它被设计成独立入口而不是 xgui_text 的透明增强：让调用方
// 明确知道「这个比普通文字贵」。
//
// 局限：某些字体（尤其是位图字体、部分中文字体）不响应 escapement，
// 会退化成不旋转。微软雅黑实测可以。
void xgui_text_rot(int x, int y, const char* s, XColor color, double rotDeg) {
    if (!s_mem_dc || !s) return;
    color = a_col(color);

    wchar_t* ws = u8_to_w(s);
    if (!ws) return;

    // escapement 单位是 0.1 度，且是逆时针。
    // GDI 的坐标系 y 向下，所以视觉上的逆时针 = 角度取反。
    int esc = (int)(-rotDeg * 10.0);

    HFONT f = CreateFontW(
        15, 0, esc, esc, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    if (!f) {
        f = CreateFontW(
            15, 0, esc, esc, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei");
    }

    HGDIOBJ oldFont = NULL;
    if (f) oldFont = SelectObject(s_mem_dc, f);

    SetTextColor(s_mem_dc, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    SetBkMode(s_mem_dc, TRANSPARENT);
    TextOutW(s_mem_dc, x, y, ws, (int)wcslen(ws));

    if (oldFont) SelectObject(s_mem_dc, oldFont);
    if (f) DeleteObject(f);
    free(ws);
}

// ============================================================
// 截图（BMP）
// ============================================================
//
// 为什么要这个：界面类改动没法靠 stdout 比对来验证。
// 有了截图，至少能做「跑一遍 → 截图 → 人眼看一次」，
// 或者把截图存成回归基线做像素比对。
//
// 只支持 BMP 是因为它不需要任何外部库：24 位未压缩 BMP 的
// 文件头加像素数据，几十行就能写对。PNG 要 zlib，不值当。
//
// 路径同样要转宽字符：中文目录下 fopen 会失败。

int xgui_screenshot(const char* path) {
    if (!s_mem_dc || !path) return 0;

    int w = s_width, h = s_height;
    // 每行像素字节数必须是 4 的倍数（BMP 规范）
    int row_bytes = (w * 3 + 3) & ~3;
    int data_size = row_bytes * h;
    int file_size = 54 + data_size;

    unsigned char* buf = (unsigned char*)malloc((size_t)data_size);
    if (!buf) return 0;

    // 从 DC 里逐行取像素。GetDIBits 要求自下而上存，
    // 所以这里直接按 BMP 的顺序要，不用自己翻转。
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = h;      // 正数 = 自下而上
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    if (!GetDIBits(s_mem_dc, s_mem_bmp, 0, (UINT)h, buf, &bi, DIB_RGB_COLORS)) {
        free(buf);
        return 0;
    }

    wchar_t* wpath = u8_to_w(path);
    FILE* f = wpath ? _wfopen(wpath, L"wb") : NULL;
    free(wpath);
    if (!f) { free(buf); return 0; }

    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(file_size);
    hdr[3] = (unsigned char)(file_size >> 8);
    hdr[4] = (unsigned char)(file_size >> 16);
    hdr[5] = (unsigned char)(file_size >> 24);
    hdr[10] = 54;                        // 像素数据偏移
    hdr[14] = 40;                        // BITMAPINFOHEADER 大小
    hdr[18] = (unsigned char)(w);
    hdr[19] = (unsigned char)(w >> 8);
    hdr[20] = (unsigned char)(w >> 16);
    hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h);
    hdr[23] = (unsigned char)(h >> 8);
    hdr[24] = (unsigned char)(h >> 16);
    hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1;                         // planes
    hdr[28] = 24;                        // bpp
    hdr[34] = (unsigned char)(data_size);
    hdr[35] = (unsigned char)(data_size >> 8);
    hdr[36] = (unsigned char)(data_size >> 16);
    hdr[37] = (unsigned char)(data_size >> 24);

    fwrite(hdr, 1, 54, f);
    fwrite(buf, 1, (size_t)data_size, f);
    fclose(f);
    free(buf);
    return 1;
}

#else  // 非 Windows

// 类 Unix 上还没有实现。返回失败并让调用方给出可读的错误，
// 而不是链接失败（那会让整个编译器在 Linux 上编不过）。
#include <stdio.h>

int  xgui_open(const char* t, int w, int h) { (void)t;(void)w;(void)h; return 0; }
void xgui_close(void) {}
int  xgui_is_open(void) { return 0; }
int  xgui_should_close(void) { return 1; }
void xgui_begin_frame(void) {}
void xgui_end_frame(void) {}
void xgui_fill_rect(int x, int y, int w, int h, XColor c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void xgui_border_rect(int x, int y, int w, int h, int t, XColor c) { (void)x;(void)y;(void)w;(void)h;(void)t;(void)c; }
int  xgui_text_width(const char* s) { (void)s; return 0; }
int  xgui_text_height(void) { return 0; }
void xgui_text(int x, int y, const char* s, XColor c) { (void)x;(void)y;(void)s;(void)c; }
int  xgui_poll_click(int* a, int* b, int* c) { (void)a;(void)b;(void)c; return 0; }
int  xgui_mouse_x(void) { return 0; }
int  xgui_mouse_y(void) { return 0; }
int  xgui_mouse_down(int b) { (void)b; return 0; }
double xgui_time(void) { return 0.0; }
void xgui_sleep(int ms) { (void)ms; }
int  xgui_screenshot(const char* p) { (void)p; return 0; }

// v0.17 形状/样式原语的空实现。
//
// 全部 no-op 而不是模拟：类 Unix 上还没有窗口实现，模拟出一套
// 「看起来能画但和 Windows 不同」的行为，会让跨平台验证得出假结论。
// no-op 的诚实之处在于「什么都不发生」，调用方立刻知道这里没实现。
void xgui_fill_round(int x, int y, int w, int h, int r, XColor c) { (void)x;(void)y;(void)w;(void)h;(void)r;(void)c; }
void xgui_border_round(int x, int y, int w, int h, int r, int t, XColor c) { (void)x;(void)y;(void)w;(void)h;(void)r;(void)t;(void)c; }
void xgui_ellipse(int x, int y, int w, int h, XColor c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void xgui_path(const int* p, int n, XColor c) { (void)p;(void)n;(void)c; }
void xgui_gradient(int x, int y, int w, int h, XColor a, XColor b, int v) { (void)x;(void)y;(void)w;(void)h;(void)a;(void)b;(void)v; }
void xgui_push_transform(double px, double py, double rot, double sc) { (void)px;(void)py;(void)rot;(void)sc; }
void xgui_pop_transform(void) {}
void xgui_set_alpha(double a) { (void)a; }
void xgui_pop_alpha(void) {}
void xgui_clip(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
void xgui_unclip(void) {}
void xgui_text_rot(int x, int y, const char* s, XColor c, double r) { (void)x;(void)y;(void)s;(void)c;(void)r; }

#endif