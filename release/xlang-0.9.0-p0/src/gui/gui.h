// gui.h — 桌面窗口的**中性接口**
//
// 这个头文件是整个窗口能力的唯一契约。它刻意不含任何平台类型：
// 没有 HWND、没有 HDC、没有 Windows 的消息常量。上层（解释器、
// AOT 生成的代码）只认这里的函数。
//
// ── 为什么单独一层 ────────────────────────────────────────
//
// 与 platform.h 的理由完全一样：语言要跨平台，就不能让 Win32 的
// 概念渗进业务代码。将来加 macOS 实现时，只需要再写一个
// gui_cocoa.c 填满这张接口，上层一行都不用改。
//
// tools/check_portable.sh 把 src/gui/ 与 platform.c 一起列为
// 「允许接触平台头文件的地方」，其余 .c 文件出现 <windows.h> 就是违规。
//
// ── 设计上的取舍 ─────────────────────────────────────────
//
// 这是一个**即时模式**（immediate mode）接口：每帧从头重画一遍，
// 没有场景图、没有控件树、没有回调。理由：
//   · 扫雷这种程序每帧画一百来个矩形，重画的开销远小于维护一棵树
//   · 即时模式下「状态」全在 X 语言的变量里，不在窗口系统里，
//     不需要跨语言同步两份状态（那是最容易出 bug 的地方）
//   · 接口只有十来个函数，容易实现也容易换平台
//
// 代价是：每帧都要自己记住「现在该画什么」。对这个规模的程序
// 完全不是问题。

#ifndef X_GUI_H
#define X_GUI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 颜色用 0xRRGGBB 表示。不引入结构体是因为 X 语言侧只有 Int，
// 传一个整数比构造结构体简单得多，也省掉一层转换。
typedef unsigned int XColor;

#define XGUI_RGB(r, g, b) \
    ((((unsigned)(r) & 0xFF) << 16) | (((unsigned)(g) & 0xFF) << 8) | ((unsigned)(b) & 0xFF))

// 鼠标按键
enum {
    XGUI_MOUSE_LEFT  = 1,
    XGUI_MOUSE_RIGHT = 2,
};

// 打开一个窗口。成功返回 1，失败返回 0。
//
// 失败的唯一常见原因是当前环境没有可用的图形界面（比如在纯 SSH
// 会话里跑）。返回 0 而不是直接 abort，让程序有机会打印一条
// 可读的错误而不是莫名消失。
int  xgui_open(const char* title, int width, int height);
void xgui_close(void);

// 窗口是否已经打开。
//
// 存在的理由不是「方便」，而是为了让错误信息能说人话：
// 忘了调 winOpen 就直接 fillRect 是个很容易犯的错，如果没有这个
// 查询，绘制调用会静默地什么都不做，用户看到的是一个全黑的窗口，
// 完全无从下手。有了它，运行时能直接说「窗口还没打开」。
int  xgui_is_open(void);

// 窗口是否已被用户关闭（点了叉）。主循环靠它退出。
int  xgui_should_close(void);

// 一帧的开始与结束。
//
// begin 清空后台缓冲（用背景色填充），end 把后台缓冲一次性贴到屏幕上。
// 中间所有绘制都发生在后台缓冲里，所以看不到闪烁 —— 这是双缓冲
// 的全部意义，也是这种程序唯一必须做对的性能相关决策。
void xgui_begin_frame(void);
void xgui_end_frame(void);

// 绘制。坐标原点在客户区左上角，y 轴向下（与屏幕一致，与数学坐标相反）。
void xgui_fill_rect(int x, int y, int w, int h, XColor color);
void xgui_border_rect(int x, int y, int w, int h, int thickness, XColor color);

// 画一行文字。(x, y) 是左上角。
//
// 内部用等宽字体，字宽固定。这样 X 语言侧算居中对齐时可以用
// strlen * 固定宽度，不需要去问「这段文字多宽」。
int  xgui_text_width(const char* s);
int  xgui_text_height(void);
void xgui_text(int x, int y, const char* s, XColor color);

// ============================================================
// 形状 / 样式（v0.17）
// ============================================================
//
// ── 这一组的共同前提：全部落在 GDI 能给的能力里 ─────────────
//
// 不引入 2D 引擎。理由是内嵌尺寸：windres 把整段运行时编进产物，
// 加一个 Cairo / Skia 立刻就是几百 KB 到几 MB，而 X 的定位是
// 「一个 exe 就能跑」。所以这里只包 GDI 真能做到的：
//
//   · 圆角       RoundRect（一次 GDI 调用，比四个扇形拼更干净）
//   · 椭圆       Ellipse
//   · 多边形     Polygon（点集自动闭合）
//   · 渐变       GradientFill（需要 msimg32.lib）
//   · 旋转/缩放  SetWorldTransform（仿射矩阵）
//   · 透明度     AlphaBlend（需要 msimg32.lib）
//
// GDI 做不到、也没打算做的：真实抗锯齿、任意贝塞尔路径、3D。
// 这些显式排除，而不是留一个「以后再说」的口子 —— 留口子会让
// 调用方写出「看起来设了但没有任何效果」的代码。

// 圆角矩形填充。radius 会被夹到 min(w,h)/2 以内。
void xgui_fill_round(int x, int y, int w, int h, int radius, XColor color);
void xgui_border_round(int x, int y, int w, int h, int radius, int thickness, XColor color);

// 椭圆。x,y,w,h 是外接矩形。
void xgui_ellipse(int x, int y, int w, int h, XColor color);

// 多边形。pts 形如 [x0,y0,x1,y1,...]，n 是**点数**（不是数组长度）。
// 自动闭合（GDI 的 Polygon 本身就闭合首尾点）。
void xgui_path(const int* pts, int n, XColor color);

// 线性渐变填充矩形。vertical 为真时上下渐变，否则左右。
void xgui_gradient(int x, int y, int w, int h, XColor c1, XColor c2, int vertical);

// ── 变换栈 ──────────────────────────────────────────────────
//
// 绕 (pivotX, pivotY) 旋转 rotDeg 度并缩放 scale 倍。
//
// 为什么是「绕轴心」而不是「绕原点」：绕原点旋转会让控件飞到
// 屏幕外，调用方还得自己算补偿位移 —— 那正是把麻烦推给用户的
// 设计。绕轴心旋转是 UI 里唯一有意义的语义（图标绕自己中心转）。
//
// 变换只影响几何，不影响 xgui_text 的文本度量：xgui_text_width
// 返回的仍是**变换前**的宽度。调用方做旋转后的命中判定时必须
// 自己做坐标反变换，这一条写在 gui.h 里就是为了让它无法被忽略。
void xgui_push_transform(double pivotX, double pivotY, double rotDeg, double scale);
void xgui_pop_transform(void);

// 透明度栈。值是乘性的：外层 0.5 + 内层 0.5 = 0.25。
//
// 乘性而不是覆盖，因为「面板半透明、里面的按钮再半透明」应该是
// 更透明，而不是被内层重置成 0.5。覆盖语义会让嵌套失效。
void xgui_set_alpha(double a);
void xgui_pop_alpha(void);

// 裁剪区栈。绘制被限制在矩形内。
void xgui_clip(int x, int y, int w, int h);
void xgui_unclip(void);

// 旋转文字。绕 (x, y) 逆时针转 rotDeg 度。
//
// 单独一个函数而不是「在变换里调 xgui_text」：GDI 的文字旋转
// 依赖字体引擎，某些字体会退化成不旋转。实测微软雅黑可以，
// 但要让它成为一个可测的独立入口，而不是藏在变换栈里。
void xgui_text_rot(int x, int y, const char* s, XColor color, double rotDeg);

// 事件轮询。
//
// 返回 1 表示取到一个点击事件，并把坐标与按键写进出参；
// 返回 0 表示当前没有待处理事件。
//
// 为什么要「取出一个」而不是「给我一个数组」：X 语言这一侧
// 处理事件就是个 while 循环，一次取一个最自然，也不需要
// 在跨语言边界上分配数组、管理生命周期。
int xgui_poll_click(int* out_x, int* out_y, int* out_button);

// 当前鼠标位置（无需按下也能读）。用于悬停高亮。
int xgui_mouse_x(void);
int xgui_mouse_y(void);

// 是否按着某个键（不消费事件，纯查询）。
int xgui_mouse_down(int button);

// 自窗口打开以来经过的秒数。用于计时器。
double xgui_time(void);

// 让出 CPU 若干毫秒。主循环里必须调用，否则会占满一个核。
void xgui_sleep(int ms);

// 把后台缓冲的内容保存成 BMP，用于自动化验证截图。
// 返回 1 成功。这个能力只在测试路径上用到，但放在正式接口里，
// 因为它没有平台无关的替代品，而「能不能截图」直接决定
// 界面类改动能不能被自动验证。
int xgui_screenshot(const char* path);

#ifdef __cplusplus
}
#endif

#endif  // X_GUI_H