// ============================================================================
//  Ui.cpp — 即时模式（Immediate-Mode）UI 实现
// ----------------------------------------------------------------------------
//  核心思路（ImGui 那套，但完全用 Skia 手绘，不引入任何第三方 UI 库）：
//    1) 每帧重画：没有控件树、没有字体图集，所有几何都由“行游标”现算；
//    2) 命中测试与绘制共用同一套矩形：控件绘制时把矩形记进本帧命中列表
//       curHits[]，鼠标判定读上一帧的 prevHits[]（避免同帧先后顺序问题）；
//    3) 交互状态机：activeId（正在按住/拖动的控件）+ 悬停判定（上一帧矩形）；
//    4) WndProc 通过 wantsMouse()/wantsKeyboard() 决定是否吞掉消息。
//  绘制细节（圆角矩形/描边/文字）参考 src/main.cpp 的 drawRect/drawStrokeRect/
//  drawText 写法：SkPaint + SkRRect::MakeRectXY + SkTextBlob::MakeFromString。
//  注意：不使用 include/effects/ 下的渐变、模糊等头（m146 里 API 名称不稳定），
//        阴影用“多层偏移圆角矩形”模拟。
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
// m146 注意：SkTextEncoding 已从 include/core/SkTextEncoding.h 迁到 SkFontTypes.h
// （旧的 SkTextEncoding.h 在本 SDK 里不存在，只 include SkFont.h 只有前置声明）。
#include "include/core/SkFontTypes.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkTextBlob.h"
#include "include/ports/SkTypeface_win.h"

namespace skiagui { namespace ui {

namespace {

// ---------------------------------------------------------------- 布局常量
// 与 src/core/Config.h 的 kPanelWidth/kRowHeight/kTitleHeight/kPadding/
// kLabelWidth/kCornerRadius/字体尺寸保持一致；这里直接用字面量，
// 避免 UI 模块反向依赖 core/Config.h（模块耦合）。
constexpr float kLabelWidth   = 130.0f;
constexpr float kRowHeight    = 26.0f;
constexpr float kTitleHeight  = 38.0f;
constexpr float kPadding      = 14.0f;
constexpr float kCornerRadius = 10.0f;
constexpr float kFontTitle    = 15.0f;
constexpr float kFontBody     = 14.0f;
constexpr float kFontSmall    = 11.0f;

// ---------------------------------------------------------------- 主题色
// 数值与 src/core/Config.h 的 kColor* 完全一致（字面量形式，避免耦合）。
constexpr SkColor kColPanel   = 0xE8181B22;  // kColorPanel   ARGB 232,24,27,34
constexpr SkColor kColTitle   = 0xFF264E94;  // kColorTitle   ARGB 255,38,78,148
constexpr SkColor kColBorder  = 0x5A78AAFF;  // kColorBorder
constexpr SkColor kColLabel   = 0xD2D6DEEB;  // kColorLabel
constexpr SkColor kColValue   = 0xFF8CC8FF;  // kColorValue
constexpr SkColor kColText    = 0xFFE6ECF5;  // kColorText
constexpr SkColor kColAccent  = 0xFF5AAAFF;  // kColorAccent
constexpr SkColor kColTrack   = 0x46FFFFFF;  // kColorTrack
constexpr SkColor kColGood    = 0xFF46A06E;  // kColorGood（复选框开 = 绿色）
constexpr SkColor kColWarn    = 0xFFFFAA46;  // kColorWarn
constexpr SkColor kColShadow  = 0x78000000;  // kColorShadow

// 控件形状常量
constexpr float kButtonHeight = 24.0f;
constexpr float kPillWidth    = 44.0f;
constexpr float kPillHeight   = 22.0f;
constexpr float kTrackHeight  = 6.0f;
constexpr float kHandleRadius = 7.0f;
constexpr float kBarHeight    = 18.0f;
constexpr float kCloseSize    = 18.0f;

// 列表相关（逻辑像素）
constexpr float kListItemPad  = 8.0f;   // 行内文字左内边距
constexpr float kListPad      = 3.0f;   // 容器内边距（行不贴边框）
constexpr float kPathBoxH     = 24.0f;  // pathBox 高度
constexpr float kColGap       = 10.0f;  // 多列行：列与列之间的最小间隙
constexpr float kHeaderRowH   = 18.0f;  // 多列表头的行高（比数据行矮）
constexpr float kHeaderFont   = 11.0f;  // 表头字号

// ---------------------------------------------------------------- 卡片（无边框窗口风格）
constexpr float kCardRadius   = 12.0f;  // 圆角半径
constexpr float kCardTitleH   = 30.0f;  // 标题行高
constexpr float kCardPadX     = 14.0f;  // 左右内边距
constexpr float kCardPadB     = 12.0f;  // 底部内边距
constexpr float kCardFontTitle = 13.0f; // 标题字号（比面板标题小）

// 列表/选中行的配色（选中=主题蓝 kColTitle，hover=半透明白）
constexpr SkColor kColRowSel   = 0xFF264E94;  // 与标题栏同色
constexpr SkColor kColRowHover = 0x24FFFFFF;  // 浅色高亮（叠在面板底色上）
constexpr SkColor kColRowLine  = 0x1AFFFFFF;  // 多列表格的横向分隔线（很淡）

// 按钮选中态（实心主题蓝，用于单选组：一眼看出当前选项）
constexpr SkColor kColBtnSel       = 0xFF2E63B8;
constexpr SkColor kColBtnSelHover  = 0xFF3C77D0;
constexpr SkColor kColBtnSelBorder = 0xFF8CC8FF;

// 卡片配色：底色比面板底稍亮的深灰蓝，边框是很淡的白
constexpr SkColor kColCard     = 0xFF1C2027;
constexpr SkColor kColCardLine = 0x33FFFFFF;
constexpr SkColor kColCardTitle = 0xFF9AA6B8;  // 次要文字色

// 命中类型（记录在命中列表里，便于调试/扩展）
enum HitKind : uint8_t {
    kHitPanel = 0,
    kHitTitle = 1,
    kHitClose = 2,
    kHitButton = 3,
    kHitSlider = 4,
    kHitCheck = 5,
    kHitRow   = 6,   // selectable / listItem / listItemEx 的整行
    kHitList  = 7,   // 列表容器本身（整块算交互区）
    kHitPath  = 8,   // pathBox
    kHitCard  = 9,   // 卡片容器本身（整块算交互区）
};

// 判断是否含非 ASCII（含中文）——含则整串用 CJK typeface（不做字形回退）。
inline bool needsCjk(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p >= 0x80) return true;
    }
    return false;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 64 位字符串散列（FNV-1a），用于按 title 索引面板/生成稳定控件 id
inline uint64_t hashStr(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; ++s) {
        h ^= static_cast<unsigned char>(*s);
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

// 指针散列：滑块用 *value 的地址做 id，保证跨帧、跨布局变化拖动连续。
inline uint64_t hashPtr(const void* p) {
    uint64_t v = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v ? v : 1ULL;
}

}  // namespace

// ============================================================================
//  UiContext::Impl —— 所有私有状态（头文件不暴露）
// ============================================================================
struct UiContext::Impl {
    // ------------------------------------------------------------- 字体
    sk_sp<SkFontMgr> fontMgr;
    sk_sp<SkTypeface> tfLatin;  // Segoe UI
    sk_sp<SkTypeface> tfCjk;    // Microsoft YaHei
    bool fontsReady = false;

    // ------------------------------------------------------------- 帧状态
    float dpi = 1.0f;            // 逻辑像素 -> 渲染像素
    float labelW = kLabelWidth;  // label()/checkbox()/slider* 的左侧标签列宽（可调）
    SkCanvas* canvas = nullptr;
    int fbW = 0, fbH = 0;
    InputState in{};             // 本帧输入快照（拷贝一份，渲染线程独占）
    bool mouseValid = false;
    float mx = 0.0f, my = 0.0f;  // 鼠标位置（渲染像素）
    bool leftDownPrev = false;   // 上一帧左键是否按住（用于边沿判定）
    bool wantMouse = false, wantKeyboard = false;
    int panelCount = 0;

    // ------------------------------------------------------------- 命中列表
    struct Hit {
        SkRect rect{};                 // 渲染像素
        uint64_t id = 0;               // 控件稳定标识
        uint8_t kind = kHitPanel;
        bool interactive = false;      // 是否算“落在交互控件上”
    };
    static constexpr int kMaxHits = 2048;
    Hit prevHits[kMaxHits]{};          // 上一帧（鼠标命中判定用）
    int prevCount = 0;
    Hit curHits[kMaxHits]{};           // 本帧（下一帧判定用）
    int curCount = 0;

    // ------------------------------------------------------------- 交互状态
    uint64_t activeId = 0;             // 正在按住/拖动的控件
    float dragOffX = 0.0f, dragOffY = 0.0f;  // 面板拖动：鼠标 - 面板左上角（渲染像素）
    bool sliderChanged = false;        // 当前拖动的滑块是否真的改过值

    // ------------------------------------------------------------- 面板位置表
    // 按 title 字符串索引，拖动后的位置跨帧保留。
    struct PanelPos {
        char title[64] = {};
        float x = 0.0f, y = 0.0f;
        bool used = false;
    };
    static constexpr int kMaxPanelPos = 16;
    PanelPos panelPos[kMaxPanelPos]{};
    int panelPosCount = 0;

    // ------------------------------------------------------------- 面板 / 卡片栈
    // 卡片（beginCard/endCard）复用同一套栈：它就是一个“没有投影、没有关闭按钮、
    // 标题更小”的轻量面板，这样卡片内的 text/button/selectable/... 全部照常工作。
    // 卡片既可以在面板里（beginPanel 之后），也可以直接在根级（depth == 0）——
    // 无边框窗口的界面就是靠根级卡片堆叠出来的，此时没有父面板可写游标。
    struct Panel {
        bool active = false;
        bool isCard = false;          // true = 这是一张卡片（beginCard 压栈）
        uint64_t titleHash = 0;
        uint64_t cardId = 0;          // 卡片 id 散列（命中列表用）
        PanelPos* pos = nullptr;      // 位置表条目（拖动时写回；卡片为 nullptr）
        float x = 0, y = 0, w = 0, h = 0;   // 逻辑像素
        float cursorY = 0.0f;         // 下一行的“行中心”y（逻辑像素）
        float rowHeight = kRowHeight;
        float contentX = 0.0f, contentRight = 0.0f;
        float lastRowY = 0.0f;        // 上一行行中心 y（sameLine 用）
        float lastItemRight = 0.0f;   // 上一个控件右边缘（sameLine 用）
        float sameLineX = 0.0f;
        bool sameLinePending = false;
        uint32_t widgetIndex = 0;     // 面板内控件序号（生成 id）
        int saveCount = 0;            // canvas->save() 返回值（卡片结束裁剪用）
        // ---- 卡片专用：父容器游标（endCard 时还原；根级卡片无父面板） ----
        bool hasParentPanel = false;
        float parentCursorY = 0.0f;
        float parentLastRowY = 0.0f;
        float parentLastItemRight = 0.0f;
    };
    static constexpr int kMaxDepth = 8;
    Panel panels[kMaxDepth]{};
    Panel dummyPanel{};               // depth==0 时的空面板（控件全部 no-op）
    int depth = 0;

    struct RowPos { float x = 0.0f; float y = 0.0f; };

    // ------------------------------------------------------------- 列表状态
    // 一次只允许一个列表处于打开状态（嵌套列表不支持，保持实现简单）。
    // 容器矩形是逻辑像素，绘制时已 save+clipRect，命中测试靠 m.listRect 再裁一次。
    struct ListState {
        bool active = false;
        uint64_t id = 0;          // 容器 id（beginList 的 id 散列，用于稳定命名）
        int depth = 0;            // beginList 时的面板栈深度（endPanel 兜底用）
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  // 逻辑像素
        float scroll = 0.0f;
        float cursorY = 0.0f;     // 列表内 y 游标（逻辑像素，起点 = 容器顶 - scroll）
        float contentH = 0.0f;    // 全部行累计高度（含未绘制的行）
        int rows = 0;             // 已提交的行数
        int saveCount = 0;        // canvas->save() 的返回值（endList 恢复用）
    };
    ListState listState{};
    // ------------------------------------------------------------- 列表矩形表
    //  ★ 为什么是"表"而不是一个矩形：一帧里可以有多个列表（注入器就有 DLL 列表、
    //    窗口列表、日志列表三个）。以前只记"最后一次 endList 的矩形"，于是第二个
    //    列表的矩形会被第三个覆盖 —— 结果窗口列表里的行在命中判定时被拿日志列表的
    //    矩形去裁剪，永远判不中（表现就是"点进程列表没反应"）。
    //    现在按列表 id 分别记，每个列表的行只受自己那个容器的矩形裁剪。
    struct ListRect {
        uint64_t id = 0;
        SkRect rect{};
        bool valid = false;
    };
    static constexpr int kMaxLists = 8;
    ListRect listRects[kMaxLists]{};
    // 当前正在处理的行属于哪个列表（hitPrev 用它的矩形做裁剪）
    uint64_t inListId = 0;
    // 当前正在处理的控件是否位于列表内部（hitPrev 的容器裁剪开关）
    bool inListItem = false;

    ListRect* findListRect(uint64_t id) {
        for (int i = 0; i < kMaxLists; ++i) {
            if (listRects[i].valid && listRects[i].id == id) return &listRects[i];
        }
        return nullptr;
    }

    void storeListRect(uint64_t id, const SkRect& rect) {
        if (ListRect* e = findListRect(id)) {
            e->rect = rect;
            return;
        }
        for (int i = 0; i < kMaxLists; ++i) {
            if (!listRects[i].valid) {
                listRects[i].id = id;
                listRects[i].rect = rect;
                listRects[i].valid = true;
                return;
            }
        }
        // 表满：覆盖第 0 条（列表数量不可能真的超过 8 个）
        listRects[0].id = id;
        listRects[0].rect = rect;
    }

    // 当前行所属列表的容器矩形是否包含鼠标（上一帧记录的矩形）
    bool listRectContainsPrev() const {
        for (int i = 0; i < kMaxLists; ++i) {
            if (listRects[i].valid && listRects[i].id == inListId) {
                return listRects[i].rect.contains(mouseLogicalX(), mouseLogicalY());
            }
        }
        return false;
    }

    // ------------------------------------------------------------- 基础换算
    float R(float v) const { return v * dpi; }
    // m146 注意：SkRect 同时有 fLeft 成员和 left() 访问器；这里必须用 fLeft
    // （写 r.left 会被当成成员函数引用而编译失败）。
    SkRect R(const SkRect& r) const {
        return SkRect::MakeLTRB(r.fLeft * dpi, r.fTop * dpi, r.fRight * dpi, r.fBottom * dpi);
    }
    float invDpi() const { return dpi > 0.0001f ? 1.0f / dpi : 1.0f; }

    Panel& top() {
        if (depth <= 0) {
            dummyPanel.active = false;
            return dummyPanel;
        }
        return panels[depth - 1];
    }

    // ------------------------------------------------------------- 字体
    SkFont makeFont(float logicalSize, bool cjk) const {
        sk_sp<SkTypeface> tf = cjk ? tfCjk : tfLatin;
        if (!tf) tf = tfLatin ? tfLatin : tfCjk;
        if (!tf) tf = SkTypeface::MakeEmpty();  // 兜底：绝不把 null typeface 交给 SkFont
        SkFont font(tf, R(logicalSize));
        font.setEdging(SkFont::Edging::kAntiAlias);
        font.setSubpixel(true);
        return font;
    }

    // 文本宽度（逻辑像素）
    float textWidth(const char* s, float size) const {
        if (!s || !*s) return 0.0f;
        SkFont font = makeFont(size, needsCjk(s));
        return font.measureText(s, strlen(s), SkTextEncoding::kUTF8) * invDpi();
    }

    // 用“已经建好的字体”量文本宽度（逻辑像素）——避免重复构造 SkFont
    float measureWith(const SkFont& font, const char* s, size_t len) const {
        if (!s || len == 0) return 0.0f;
        return font.measureText(s, len, SkTextEncoding::kUTF8) * invDpi();
    }

    // UTF-8 前 n 个字节所覆盖的完整字符边界（不切碎多字节字符）
    static size_t utf8Boundary(const char* s, size_t len, size_t n) {
        if (n > len) n = len;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        return n;
    }

    // 超长文本按“…”截断：先用 measureText 量全长，超宽则二分找最长能放下的前缀，
    // 再拼上省略号（省略号本身也要能放下，放不下就退回更短的前缀）。
    // 返回 std::string（UTF-8），调用方直接喂给 SkTextBlob。
    std::string ellipsize(const char* text, float size, float maxWidthLogical) const {
        if (!text || !*text) return std::string();
        if (maxWidthLogical <= 0.0f) return std::string(text);
        SkFont font = makeFont(size, needsCjk(text));
        const size_t len = strlen(text);
        if (measureWith(font, text, len) <= maxWidthLogical) return std::string(text);

        const char* kEll = "\xE2\x80\xA6";  // U+2026
        const float ellW = measureWith(font, kEll, 3);
        if (ellW >= maxWidthLogical) return std::string(kEll);

        size_t lo = 0, hi = len;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            const size_t cut = utf8Boundary(text, len, mid);
            if (cut == 0) break;
            if (measureWith(font, text, cut) + ellW <= maxWidthLogical) {
                lo = cut;
            } else {
                hi = (cut > 0) ? cut - 1 : 0;
            }
        }
        const size_t cut = utf8Boundary(text, len, lo);
        return std::string(text, cut) + kEll;
    }

    // 行中心 y -> 文字基线 y（用字体度量居中，中英混排都不会偏）
    float baselineFor(float centerY, float size, bool cjk) const {
        SkFont font = makeFont(size, cjk);
        SkFontMetrics m;
        font.getMetrics(&m);
        return centerY - (m.fAscent + m.fDescent) * 0.5f * invDpi();
    }

    // ------------------------------------------------------------- 绘制
    // 参考 src/main.cpp 的 drawText/drawRRect 写法：SkPaint 设色 + setAntiAlias，
    // 圆角一律用 SkRRect::MakeRectXY，文字一律 SkTextBlob::MakeFromString + drawTextBlob。
    void fillRect(const SkRect& rLogical, SkColor c, float radius = 0.0f) {
        if (!canvas) return;
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        if (radius > 0.0f) {
            canvas->drawRRect(SkRRect::MakeRectXY(R(rLogical), R(radius), R(radius)), paint);
        } else {
            canvas->drawRect(R(rLogical), paint);
        }
    }

    // 参考 src/main.cpp 的 drawStrokeRect
    void strokeRect(const SkRect& rLogical, SkColor c, float widthLogical, float radius = 0.0f) {
        if (!canvas) return;
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(R(widthLogical));
        if (radius > 0.0f) {
            canvas->drawRRect(SkRRect::MakeRectXY(R(rLogical), R(radius), R(radius)), paint);
        } else {
            canvas->drawRect(R(rLogical), paint);
        }
    }

    void fillCircle(float cxLogical, float cyLogical, float rLogical, SkColor c) {
        if (!canvas) return;
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        canvas->drawCircle(R(cxLogical), R(cyLogical), R(rLogical), paint);
    }

    void line(float x0, float y0, float x1, float y1, SkColor c, float widthLogical) {
        if (!canvas) return;
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(R(widthLogical));
        canvas->drawLine(R(x0), R(y0), R(x1), R(y1), paint);
    }

    // 参考 src/main.cpp 的 drawText：SkTextBlob::MakeFromString + drawTextBlob
    // yLogical 是文字基线；maxWidth>0 时裁掉右侧溢出，避免控件重叠。
    void drawText(const char* s, float xLogical, float yLogical, float size, SkColor c,
                  float maxWidthLogical = 0.0f) {
        if (!canvas || !s || !*s) return;
        SkFont font = makeFont(size, needsCjk(s));
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromString(s, font);
        if (!blob) return;
        const int saveCount = canvas->save();
        if (maxWidthLogical > 0.0f) {
            canvas->clipRect(SkRect::MakeXYWH(R(xLogical), R(yLogical - size * 1.3f),
                                              R(maxWidthLogical), R(size * 1.8f)));
        }
        canvas->drawTextBlob(blob.get(), R(xLogical), R(yLogical), paint);
        canvas->restoreToCount(saveCount);
    }

    // 以“行中心”为基准画文字（布局游标给的就是行中心）
    void drawTextAtRow(const char* s, float xLogical, float centerY, float size, SkColor c,
                       float maxWidthLogical = 0.0f) {
        drawText(s, xLogical, baselineFor(centerY, size, needsCjk(s)), size, c, maxWidthLogical);
    }

    // 画一段“已经预处理过”的 UTF-8 文本（可能是截断+省略号的结果），
    // clipLogical 非空时额外裁剪到该矩形（列表行用它把文字挡在容器内）。
    void drawTextClipped(const std::string& s, float xLogical, float yLogical, float size, SkColor c,
                         const SkRect* clipLogical) {
        if (!canvas || s.empty()) return;
        SkFont font = makeFont(size, needsCjk(s.c_str()));
        SkPaint paint;
        paint.setColor(c);
        paint.setAntiAlias(true);
        sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromString(s.c_str(), font);
        if (!blob) return;
        const int saveCount = canvas->save();
        if (clipLogical) canvas->clipRect(R(*clipLogical));
        canvas->drawTextBlob(blob.get(), R(xLogical), R(yLogical), paint);
        canvas->restoreToCount(saveCount);
    }

    void drawTextAtRowClipped(const std::string& s, float xLogical, float centerY, float size,
                              SkColor c, const SkRect* clipLogical) {
        drawTextClipped(s, xLogical, baselineFor(centerY, size, needsCjk(s.c_str())), size, c,
                        clipLogical);
    }

    void drawTextRight(const char* s, float rightLogical, float centerY, float size, SkColor c) {
        const float w = textWidth(s, size);
        drawTextAtRow(s, rightLogical - w, centerY, size, c, w + 1.0f);
    }

    void drawTextCentered(const char* s, float centerX, float centerY, float size, SkColor c) {
        const float w = textWidth(s, size);
        drawTextAtRow(s, centerX - w * 0.5f, centerY, size, c, w + 1.0f);
    }

    // ------------------------------------------------------------- 命中列表
    void recordHit(const SkRect& rLogical, uint64_t id, uint8_t kind, bool interactive) {
        if (curCount >= kMaxHits) return;
        Hit& h = curHits[curCount++];
        h.rect = R(rLogical);
        h.id = id;
        h.kind = kind;
        h.interactive = interactive;
    }

    // 命中判定：上一帧的矩形列表 + 本帧鼠标位置
    // 额外规则：列表内的控件必须同时落在"它自己那个列表容器"的上一帧矩形里才算
    //           命中；鼠标在容器外（或容器被滚动到别处）时，容器内的行不判 hover/click。
    bool hitPrev(uint64_t id) const {
        if (!mouseValid) return false;
        for (int i = 0; i < prevCount; ++i) {
            if (prevHits[i].id != id || !prevHits[i].rect.contains(mx, my)) continue;
            // 只有“列表内的控件”才受容器裁剪（面板里的普通控件不受影响）
            if (listState.active && inListItem && !listRectContainsPrev()) return false;
            return true;
        }
        return false;
    }

    bool mousePressed() const { return in.clickCount > 0; }
    bool mouseReleased() const {
        return in.releaseCount > 0 || (leftDownPrev && !in.leftDown);
    }
    float mouseLogicalX() const { return mx * invDpi(); }
    float mouseLogicalY() const { return my * invDpi(); }

    // ------------------------------------------------------------- id 生成
    uint64_t makeWidgetId(uint8_t kind) {
        Panel& p = top();
        ++p.widgetIndex;
        uint64_t id = p.titleHash ^ (static_cast<uint64_t>(kind) << 56) ^
                      (static_cast<uint64_t>(p.widgetIndex) * 0x9E3779B97F4A7C15ULL);
        return id ? id : 1ULL;
    }
    static uint64_t makeSliderId(const void* valuePtr) {
        return hashPtr(valuePtr) ^ 0x5107ULL;
    }

    // ------------------------------------------------------------- 行游标
    // 取本控件的位置：sameLinePending 时复用上一行行中心，否则开新行并推进游标。
    // 列表内（listState.active）改用“列表内 y 游标”，并且**不推进面板游标**
    // ——面板游标在 beginList 时已经一次性跳过整个容器高度。
    RowPos beginRowItem(float height) {
        Panel& p = top();
        RowPos rp;
        if (listState.active) {
            rp.x = p.contentX;
            rp.y = listState.cursorY;
            listState.cursorY += (height > kRowHeight ? height : kRowHeight);
            p.sameLinePending = false;  // 列表内不支持 sameLine
            return rp;
        }
        if (p.sameLinePending) {
            rp.x = p.sameLineX;
            rp.y = p.lastRowY;
            p.sameLinePending = false;
        } else {
            rp.x = p.contentX;
            rp.y = p.cursorY;
            p.lastRowY = p.cursorY;
            p.cursorY += (height > p.rowHeight ? height : p.rowHeight);
        }
        p.lastItemRight = rp.x;
        return rp;
    }

    // ------------------------------------------------------------- 列表行辅助
    // 提交一行：推进列表游标、累计内容高度（含被裁掉的行），返回该行矩形。
    // padX 是行内水平内边距（列表内 = kListItemPad + kListPad）。
    bool beginListRow(float padX, float widthOverride, SkRect* outRect) {
        if (!listState.active) return false;
        const Panel& p = panels[listState.depth - 1];
        float w = (widthOverride > 0.0f) ? widthOverride : (p.contentRight - p.contentX - padX * 2.0f);
        if (w < 0.0f) w = 0.0f;
        const float cx = p.contentX + padX;
        const float cy = listState.cursorY;
        *outRect = SkRect::MakeXYWH(cx, cy - kRowHeight * 0.5f, w, kRowHeight);
        listState.cursorY += kRowHeight;
        listState.contentH += kRowHeight;  // 全部行都累计（含容器外的行）
        ++listState.rows;
        return true;
    }

    // 行是否落在容器矩形内（只有落在里面的行才真正绘制，省 CPU）
    bool rowVisible(const SkRect& rowRect) const {
        if (!listState.active) return false;
        return rowRect.fBottom > listState.y && rowRect.fTop < listState.y + listState.h;
    }

    // 与 beginListRow 相同，但行高可指定（表头比数据行矮）。
    // 行矩形仍然"以 cursorY 为中心、高 rowH"，调用方拿到矩形后直接用 centerY
    // 画文字即可，不必关心 rowH 的差异。
    bool beginListRowEx(float padX, float widthOverride, float rowH, SkRect* outRect) {
        if (!listState.active) return false;
        const Panel& p = panels[listState.depth - 1];
        float w = (widthOverride > 0.0f) ? widthOverride
                                         : (p.contentRight - p.contentX - padX * 2.0f);
        if (w < 0.0f) w = 0.0f;
        const float cx = p.contentX + padX;
        const float cy = listState.cursorY;
        *outRect = SkRect::MakeXYWH(cx, cy - rowH * 0.5f, w, rowH);
        listState.cursorY += rowH;
        listState.contentH += rowH;
        ++listState.rows;
        return true;
    }

    // ------------------------------------------------------------- 面板位置表
    PanelPos* findOrAddPanelPos(const char* title) {
        for (int i = 0; i < panelPosCount; ++i) {
            if (std::strcmp(panelPos[i].title, title) == 0) return &panelPos[i];
        }
        if (panelPosCount >= kMaxPanelPos) return nullptr;
        PanelPos& e = panelPos[panelPosCount++];
        std::snprintf(e.title, sizeof(e.title), "%s", title);
        e.x = 0.0f;
        e.y = 0.0f;
        e.used = false;
        return &e;
    }

    // 面板投影：多层偏移圆角矩形模拟柔和阴影（不用 effects/ 里的模糊）
    void drawShadow(const Panel& p) {
        fillRect(SkRect::MakeXYWH(p.x - 3.0f, p.y + 7.0f, p.w + 6.0f, p.h + 6.0f), 0x28000000,
                 kCornerRadius + 4.0f);
        fillRect(SkRect::MakeXYWH(p.x - 1.5f, p.y + 6.0f, p.w + 3.0f, p.h + 3.0f), 0x44000000,
                 kCornerRadius + 2.0f);
        fillRect(SkRect::MakeXYWH(p.x, p.y + 6.0f, p.w, p.h), kColShadow, kCornerRadius);
    }
};

// ============================================================================
//  构造 / 析构
// ============================================================================
UiContext::UiContext() : impl_(new Impl()) {}
UiContext::~UiContext() { delete impl_; }

// ============================================================================
//  字体初始化
// ============================================================================
bool UiContext::initFonts() {
    Impl& I = *impl_;
    if (I.fontsReady) return true;  // 可重复调用
    // 参考 src/main.cpp 的 initFonts：DirectWrite 字体管理器
    I.fontMgr = SkFontMgr_New_DirectWrite();
    if (!I.fontMgr) return false;

    I.tfLatin = I.fontMgr->matchFamilyStyle("Segoe UI", SkFontStyle::Normal());
    if (!I.tfLatin) {
        I.tfLatin = I.fontMgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
    }
    I.tfCjk = I.fontMgr->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    if (!I.tfCjk) {
        I.tfCjk = I.tfLatin;  // 没有雅黑就退回拉丁字体（至少不崩）
    }
    if (!I.tfLatin) return false;
    I.fontsReady = true;
    return true;
}

// ============================================================================
//  DPI
// ============================================================================
void UiContext::setDpiScale(float scale) {
    Impl& I = *impl_;
    I.dpi = clampf(scale, 0.5f, 4.0f);
}

float UiContext::dpiScale() const { return impl_->dpi; }

// ============================================================================
//  帧开始 / 帧结束
// ============================================================================
void UiContext::beginFrame(SkCanvas* canvas, int width, int height, const InputState& input) {
    Impl& I = *impl_;
    I.canvas = canvas;
    I.fbW = width;
    I.fbH = height;
    I.in = input;  // 消费输入快照（拷贝，之后渲染线程独占）
    I.mouseValid = input.mouseValid;
    I.mx = input.mouseX;
    I.my = input.mouseY;

    // 清空本帧命中列表与布局栈
    I.curCount = 0;
    I.depth = 0;
    I.panelCount = 0;

    // wantsMouse = 上一帧鼠标落在任一交互控件上（WndProc 据此吞掉鼠标消息）。
    // 注意：用上一帧的矩形列表判定，避免本帧控件还没画出来就判不中。
    I.wantMouse = false;
    if (I.mouseValid) {
        for (int i = 0; i < I.prevCount; ++i) {
            if (I.prevHits[i].interactive && I.prevHits[i].rect.contains(I.mx, I.my)) {
                I.wantMouse = true;
                break;
            }
        }
    }
    I.wantKeyboard = false;
}

void UiContext::endFrame() {
    Impl& I = *impl_;
    // wantsMouse/wantsKeyboard 语义：
    //   * 鼠标落在上一帧任一交互控件上（beginFrame 已按上一帧命中列表算好），或
    //   * 当前有控件处于按住/拖动状态（鼠标可能已经滑出控件，消息仍必须吞掉）。
    // WndProc 据此决定是否把鼠标/键盘消息交给宿主（游戏）。
    if (I.activeId != 0) I.wantMouse = true;
    I.wantKeyboard = I.wantMouse;

    // 本帧命中列表 -> 下一帧的判定依据（超过 kMaxHits 的部分丢弃）
    I.prevCount = (I.curCount > Impl::kMaxHits) ? Impl::kMaxHits : I.curCount;
    for (int i = 0; i < I.prevCount; ++i) I.prevHits[i] = I.curHits[i];

    // 兜底：左键已抬起就结束一切拖动（防止控件消失后 activeId 卡住）
    if (!I.in.leftDown) {
        I.activeId = 0;
        I.sliderChanged = false;
    }
    I.leftDownPrev = I.in.leftDown;
    I.depth = 0;

    // 兜底：调用方忘了 endList（比如面板中途 return）也要恢复 canvas 并清状态
    if (I.listState.active) {
        if (I.canvas) I.canvas->restoreToCount(I.listState.saveCount);
        I.storeListRect(I.listState.id,
                        SkRect::MakeXYWH(I.listState.x, I.listState.y, I.listState.w,
                                         I.listState.h));
        I.listState.active = false;
    }
    I.inListItem = false;
    I.inListId = 0;
    // 兜底：忘了 endCard —— 卡片就在面板栈里，一次 restore 到最外层卡片的 save 即可。
    // 注意只能 restore 一次：restoreToCount 会一次性弹掉它上面的所有 save。
    int minCardSave = 0;
    for (int i = I.depth - 1; i >= 0; --i) {
        if (I.panels[i].active && I.panels[i].isCard) {
            if (minCardSave == 0 || I.panels[i].saveCount < minCardSave) {
                minCardSave = I.panels[i].saveCount;
            }
            I.panels[i].active = false;
        }
    }
    if (minCardSave > 0 && I.canvas) I.canvas->restoreToCount(minCardSave);
}

bool UiContext::wantsMouse() const { return impl_->wantMouse; }
bool UiContext::wantsKeyboard() const { return impl_->wantKeyboard; }
int UiContext::panelCount() const { return impl_->panelCount; }

// ---- 诊断（自测用）----
int UiContext::hitCount() const { return impl_->curCount; }
int UiContext::prevHitCount() const { return impl_->prevCount; }
SkRect UiContext::listRectById(int index) const {
    int n = 0;
    for (int i = 0; i < Impl::kMaxLists; ++i) {
        if (!impl_->listRects[i].valid) continue;
        if (n++ == index) return impl_->listRects[i].rect;
    }
    return SkRect::MakeEmpty();
}
int UiContext::listRectCount() const {
    int n = 0;
    for (int i = 0; i < Impl::kMaxLists; ++i) {
        if (impl_->listRects[i].valid) ++n;
    }
    return n;
}
bool UiContext::hitEntryAt(int index, HitEntry* out) const {
    if (!out || index < 0 || index >= impl_->curCount) return false;
    const Impl::Hit& h = impl_->curHits[index];
    out->id = h.id;
    out->kind = h.kind;
    out->rect = SkRect::MakeLTRB(h.rect.fLeft * impl_->invDpi(),
                                 h.rect.fTop * impl_->invDpi(),
                                 h.rect.fRight * impl_->invDpi(),
                                 h.rect.fBottom * impl_->invDpi());
    out->interactive = h.interactive;
    return true;
}
bool UiContext::hitRectForId(uint64_t id, float* l, float* t, float* r, float* b) const {
    for (int i = 0; i < impl_->curCount; ++i) {
        if (impl_->curHits[i].id == id) {
            if (l) *l = impl_->curHits[i].rect.fLeft;
            if (t) *t = impl_->curHits[i].rect.fTop;
            if (r) *r = impl_->curHits[i].rect.fRight;
            if (b) *b = impl_->curHits[i].rect.fBottom;
            return true;
        }
    }
    return false;
}

// ============================================================================
//  面板
// ============================================================================
bool UiContext::beginPanel(const char* title, float x, float y, float w, float h, bool* open) {
    Impl& I = *impl_;
    if (open && !*open) return false;  // 已关闭：调用方不得调用 endPanel()
    if (!I.canvas || I.depth >= Impl::kMaxDepth) return false;

    const char* t = (title && *title) ? title : "Panel";

    Impl::Panel& p = I.panels[I.depth];
    p = Impl::Panel{};  // 重置（含 widgetIndex / sameLinePending）
    p.active = true;
    p.titleHash = hashStr(t);
    p.pos = I.findOrAddPanelPos(t);
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    // 位置：首次出现用调用方给的 x/y，之后用拖动后保存的位置
    if (p.pos) {
        if (p.pos->used) {
            p.x = p.pos->x;
            p.y = p.pos->y;
        } else {
            p.pos->x = p.x;
            p.pos->y = p.y;
            p.pos->used = true;
        }
    }

    // ------------------------------------------------ 标题栏拖动（先算位置再画）
    // 关闭按钮的矩形要先算出来：标题栏拖动区必须避开它，否则点 X 会被拖动抢走。
    const SkRect closeRect = SkRect::MakeXYWH(
            p.x + p.w - kPadding - kCloseSize,
            p.y + (kTitleHeight - kCloseSize) * 0.5f, kCloseSize, kCloseSize);
    const uint64_t titleId = p.titleHash ^ 0x7A17E0ULL;
    const uint64_t closeId = p.titleHash ^ 0x0C105EULL;
    const bool titleHovered = I.hitPrev(titleId) && !I.hitPrev(closeId);
    if (titleHovered && I.mousePressed() && I.activeId == 0) {
        I.activeId = titleId;
        I.dragOffX = I.mx - I.R(p.x);
        I.dragOffY = I.my - I.R(p.y);
    }
    if (I.activeId == titleId && I.in.leftDown) {
        p.x = I.mouseLogicalX() - I.dragOffX * I.invDpi();
        p.y = I.mouseLogicalY() - I.dragOffY * I.invDpi();
        // 限制在屏幕内：至少留出标题栏宽度，避免拖出画面找不回来
        const float minX = -p.w + 80.0f;
        const float maxX = I.fbW * I.invDpi() - 80.0f;
        const float maxY = I.fbH * I.invDpi() - kTitleHeight;
        p.x = clampf(p.x, minX, std::max(minX, maxX));
        p.y = clampf(p.y, 0.0f, std::max(0.0f, maxY));
        if (p.pos) {
            p.pos->x = p.x;
            p.pos->y = p.y;
        }
    }

    // ------------------------------------------------ 绘制面板
    // 投影（偏移 6px 的深色圆角矩形）+ 面板体 + 1px 描边
    I.drawShadow(p);
    const SkRect panelRect = SkRect::MakeXYWH(p.x, p.y, p.w, p.h);
    I.fillRect(panelRect, kColPanel, kCornerRadius);
    I.strokeRect(panelRect, kColBorder, 1.0f, kCornerRadius);

    // 标题栏：上圆角矩形 + 下方普通矩形补齐（参考 src/main.cpp 的画法）
    const SkRect titleRect = SkRect::MakeXYWH(p.x, p.y, p.w, kTitleHeight);
    I.fillRect(titleRect, kColTitle, kCornerRadius);
    I.fillRect(SkRect::MakeXYWH(p.x, p.y + kTitleHeight * 0.5f, p.w, kTitleHeight * 0.5f),
               kColTitle);
    // 基线用字体度量算（等价于 src/main.cpp 里 py+25 的写法，但中英混排都居中）
    I.drawText(t, p.x + kPadding,
               I.baselineFor(p.y + kTitleHeight * 0.5f, kFontTitle, needsCjk(t)), kFontTitle,
               SK_ColorWHITE, p.w - kPadding * 2.0f - kCloseSize - 8.0f);

    // 标题栏右侧的小关闭按钮（X）
    const bool closeHovered = I.hitPrev(closeId);
    if (closeHovered) I.fillRect(closeRect, 0x4DFFFFFF, 4.0f);
    const SkColor xColor = closeHovered ? SK_ColorWHITE : 0xCCD6E2F0;
    const float xPad = 5.0f;
    I.line(closeRect.fLeft + xPad, closeRect.fTop + xPad, closeRect.fRight - xPad,
           closeRect.fBottom - xPad, xColor, 1.6f);
    I.line(closeRect.fRight - xPad, closeRect.fTop + xPad, closeRect.fLeft + xPad,
           closeRect.fBottom - xPad, xColor, 1.6f);
    if (closeHovered && I.mousePressed() && I.activeId == 0 && open) {
        *open = false;  // 本帧仍正常绘制，下一帧 beginPanel 返回 false
    }

    // 命中记录：整块面板也算交互区（点击面板不该穿透到宿主）
    // 标题栏命中区避开关闭按钮，保证“拖动”和“点 X”互不抢占。
    I.recordHit(panelRect, p.titleHash, kHitPanel, true);
    I.recordHit(SkRect::MakeXYWH(p.x, p.y, std::max(0.0f, closeRect.fLeft - p.x - 4.0f),
                                 kTitleHeight),
                titleId, kHitTitle, true);
    I.recordHit(closeRect, closeId, kHitClose, open != nullptr);

    // ------------------------------------------------ 布局游标 + 内容裁剪
    p.contentX = p.x + kPadding;
    p.contentRight = p.x + p.w - kPadding;
    p.cursorY = p.y + kTitleHeight + kPadding + kFontBody * 0.5f;  // 第一行的行中心
    p.rowHeight = kRowHeight;
    p.lastRowY = p.cursorY;
    p.lastItemRight = p.contentX;
    p.sameLinePending = false;
    p.widgetIndex = 0;

    // 内容裁剪到面板圆角内，防止控件画到面板外
    I.canvas->save();
    I.canvas->clipRRect(SkRRect::MakeRectXY(I.R(panelRect), I.R(kCornerRadius), I.R(kCornerRadius)),
                        true);

    ++I.depth;
    ++I.panelCount;
    return true;
}

void UiContext::endPanel() {
    Impl& I = *impl_;
    if (I.depth <= 0) return;
    // 兜底：面板内忘了 endCard —— 先把卡片裁剪恢复掉（一次 restore 弹掉全部卡片 save）
    int minCardSave = 0;
    for (int i = I.depth - 1; i >= 0; --i) {
        if (I.panels[i].active && I.panels[i].isCard) {
            if (minCardSave == 0 || I.panels[i].saveCount < minCardSave) {
                minCardSave = I.panels[i].saveCount;
            }
            I.panels[i].active = false;
            --I.depth;
        }
    }
    if (minCardSave > 0 && I.canvas) I.canvas->restoreToCount(minCardSave);
    // 兜底：面板内忘了 endList —— 先恢复列表的 canvas 裁剪再结束面板裁剪
    if (I.listState.active && I.listState.depth >= I.depth) {
        if (I.canvas) I.canvas->restoreToCount(I.listState.saveCount);
        I.storeListRect(I.listState.id,
                        SkRect::MakeXYWH(I.listState.x, I.listState.y, I.listState.w,
                                         I.listState.h));
        I.listState.active = false;
    }
    if (I.canvas) I.canvas->restore();  // 结束内容裁剪
    I.panels[I.depth - 1].active = false;
    --I.depth;
}

// ============================================================================
//  卡片（无边框窗口风格容器）
// ============================================================================
//  卡片复用面板栈，所以卡片内的 text/textLine/button/selectable/listItem/
//  listItemEx/spacing/checkbox/slider* 全部沿用同一套行游标，不需要任何特殊分支。
//  与 beginPanel 的三点区别：
//    1) 不画投影、不画关闭按钮（无边框窗口里再出现投影就成了“窗中窗”）；
//    2) 标题行更矮（30）更淡（0xFF9AA6B8）、字号更小（13）；
//    3) 圆角 12、底色比面板底稍亮、1px 淡边框。
//
//  ★ 根级用法（无边框窗口的关键）：可以在 depth == 0 直接 beginCard，不需要任何
//    beginPanel。此时没有父面板可写游标（top() 返回的是 active=false 的 dummy），
//    卡片自己就是布局根：后续 beginCard 直接从屏幕坐标排布，endCard 回到 depth 0。
bool UiContext::beginCard(const char* id, const char* title, float x, float y, float w, float h) {
    Impl& I = *impl_;
    if (!I.canvas || I.depth >= Impl::kMaxDepth) return false;
    if (w <= 0.0f || h <= 0.0f) return false;

    // 父容器：面板内是真实面板（要写它的游标）；根级没有父面板（不写）
    const bool hasParent = (I.depth > 0);
    Impl::Panel& parent = hasParent ? I.panels[I.depth - 1] : I.dummyPanel;

    const char* cid = (id && *id) ? id : "card";
    const uint64_t idHash = hashStr(cid);

    Impl::Panel& p = I.panels[I.depth];
    p = Impl::Panel{};  // 重置（含 widgetIndex / sameLinePending）
    p.active = true;
    p.isCard = true;
    p.titleHash = idHash;   // 卡片内的控件 id 用卡片 id 做前缀（与面板同理）
    p.cardId = idHash;
    p.pos = nullptr;        // 卡片不可拖动、不记忆位置
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;

    // ------------------------------------------------ 绘制卡片本体
    const SkRect cardRect = SkRect::MakeXYWH(x, y, w, h);
    I.fillRect(cardRect, kColCard, kCardRadius);
    I.strokeRect(cardRect, kColCardLine, 1.0f, kCardRadius);

    // 标题行（可空）：小字号 + 次要文字色，基线按字体度量居中
    if (title && *title) {
        const float centerY = y + kCardTitleH * 0.5f;
        I.drawText(title, x + kCardPadX,
                   I.baselineFor(centerY, kCardFontTitle, needsCjk(title)), kCardFontTitle,
                   kColCardTitle, std::max(0.0f, w - kCardPadX * 2.0f));
    }

    // ------------------------------------------------ 布局游标
    // 内容区从标题行下方开始（无标题时也从标题行高度下方开始，保持卡片视觉一致）
    p.contentX = x + kCardPadX;
    p.contentRight = x + w - kCardPadX;
    p.cursorY = y + kCardTitleH + kRowHeight * 0.5f;  // 第一行的行中心
    p.rowHeight = kRowHeight;
    p.lastRowY = p.cursorY;
    p.lastItemRight = p.contentX;
    p.sameLinePending = false;
    p.widgetIndex = 0;

    // 父面板游标一次性跳过整张卡片 + 底部内边距 + 一点间距，卡片内控件不再推进它。
    // 根级卡片没有父面板：这里什么都不写，下一张卡片直接用屏幕坐标排布。
    p.hasParentPanel = hasParent;
    if (hasParent) {
        parent.sameLinePending = false;
        parent.cursorY = y + h + kCardPadB + 8.0f;
        parent.lastRowY = parent.cursorY;
        parent.lastItemRight = parent.contentRight;
        p.parentCursorY = parent.cursorY;
        p.parentLastRowY = parent.lastRowY;
        p.parentLastItemRight = parent.lastItemRight;
    }

    // 卡片登记为交互区：点卡片空白处不会穿透到宿主
    I.recordHit(cardRect, idHash, kHitCard, true);

    // 内容裁剪到卡片圆角内（卡片内的控件不会画到卡片外）
    p.saveCount = I.canvas->save();
    I.canvas->clipRRect(
            SkRRect::MakeRectXY(I.R(cardRect), I.R(kCardRadius), I.R(kCardRadius)), true);

    ++I.depth;
    return true;
}

void UiContext::endCard() {
    Impl& I = *impl_;
    if (I.depth <= 0) return;
    Impl::Panel& p = I.panels[I.depth - 1];
    if (!p.active || !p.isCard) return;  // 不是卡片：直接忽略（成对语义由调用方保证）
    // 兜底：卡片内忘了 endList
    if (I.listState.active && I.listState.depth >= I.depth) {
        if (I.canvas) I.canvas->restoreToCount(I.listState.saveCount);
        I.storeListRect(I.listState.id,
                        SkRect::MakeXYWH(I.listState.x, I.listState.y, I.listState.w,
                                         I.listState.h));
        I.listState.active = false;
    }
    if (I.canvas) I.canvas->restoreToCount(p.saveCount);  // 结束卡片内容裁剪
    p.active = false;
    --I.depth;
    // 回到父容器：还原父面板游标（根级卡片没有父面板，depth 回到 0 即可）
    if (p.hasParentPanel && I.depth > 0) {
        Impl::Panel& parent = I.panels[I.depth - 1];
        parent.cursorY = p.parentCursorY;
        parent.lastRowY = p.parentLastRowY;
        parent.lastItemRight = p.parentLastItemRight;
    }
}

// ============================================================================
//  分隔线 / 文本
// ============================================================================
void UiContext::separator() {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    p.sameLinePending = false;
    const float y = p.cursorY - 6.0f;  // 落在上一行与下一行中间偏上
    I.line(p.contentX, y, p.contentRight, y, 0x33FFFFFF, 1.0f);
    p.cursorY += 12.0f;
    p.lastItemRight = p.contentX;
}

void UiContext::label(const char* text, SkColor color) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    I.drawTextAtRow(text, rp.x, rp.y, kFontBody, color, I.labelW);
    p.lastItemRight = rp.x + I.labelW;  // 左列宽度，供 sameLine 接续
}

void UiContext::text(const char* text) { textColored(text, kColText); }

void UiContext::textColored(const char* text, SkColor color) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    I.drawTextAtRow(text, rp.x, rp.y, kFontBody, color, p.contentRight - rp.x);
    p.lastItemRight = p.contentRight;
}

void UiContext::sameLine(float spacing) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    p.sameLinePending = true;
    p.sameLineX = p.lastItemRight + spacing;
}

void UiContext::progressBar(float t01, const char* overlayText) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    const Impl::RowPos rp = I.beginRowItem(30.0f);
    const float w = p.contentRight - p.contentX;
    const SkRect bar = SkRect::MakeXYWH(p.contentX, rp.y - kBarHeight * 0.5f, w, kBarHeight);
    I.fillRect(bar, 0x33FFFFFF, kBarHeight * 0.5f);

    const float t = clampf(t01, 0.0f, 1.0f);
    if (t > 0.001f) {
        SkRect fill = bar;
        fill.fRight = bar.fLeft + w * t;
        // 圆角半径固定，避免进度极小时圆角被拉伸
        I.fillRect(fill, kColWarn, kBarHeight * 0.5f);
    }
    if (overlayText && *overlayText) {
        const float tw = I.textWidth(overlayText, kFontSmall);
        const float tx = bar.centerX() - tw * 0.5f;
        const float base = I.baselineFor(bar.centerY(), kFontSmall, needsCjk(overlayText));
        // 叠一层深色描影提高可读性（不用 MaskFilter/Blur）
        I.drawText(overlayText, tx + 1.0f, base + 1.0f, kFontSmall, 0xA0000000);
        I.drawText(overlayText, tx, base, kFontSmall, SK_ColorWHITE);
    }
    p.lastItemRight = p.contentRight;
}

void UiContext::textLine(const char* left, const char* right) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    I.drawTextAtRow(left, rp.x, rp.y, kFontBody, kColLabel, I.labelW);
    I.drawTextRight(right, p.contentRight, rp.y, kFontBody, kColValue);
    p.lastItemRight = p.contentRight;
}

// ============================================================================
//  按钮
// ============================================================================
bool UiContext::button(const char* label, float width, bool selected) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return false;

    const Impl::RowPos rp = I.beginRowItem(kButtonHeight);
    const float w = (width > 0.0f) ? width : (p.contentRight - p.contentX);
    const SkRect rect =
            SkRect::MakeXYWH(rp.x, rp.y - kButtonHeight * 0.5f, w, kButtonHeight);
    const uint64_t id = I.makeWidgetId(kHitButton);

    // 命中/按住状态机（命中用上一帧矩形，避免同帧顺序问题）
    const bool hovered = I.hitPrev(id);
    if (hovered && I.mousePressed() && I.activeId == 0) I.activeId = id;
    const bool held = (I.activeId == id);
    bool clicked = false;
    if (held && I.mouseReleased()) {
        I.activeId = 0;
        clicked = hovered;  // 抬起时鼠标仍在按钮上才算点击
    }

    // 三态样式：选中（实心蓝）> 悬停（半透明蓝）> 常态（淡白）
    SkColor fill = 0x24FFFFFF;
    SkColor border = 0x3F78AAFF;
    SkColor text = kColText;
    if (selected) {
        fill = (hovered || held) ? kColBtnSelHover : kColBtnSel;
        border = kColBtnSelBorder;
        text = SK_ColorWHITE;
    } else if (held) {
        fill = 0x7A5AAAFF;
        border = 0x8C5AAAFF;
    } else if (hovered) {
        fill = 0x4D5AAAFF;
        border = 0x8C5AAAFF;
    }
    I.fillRect(rect, fill, 6.0f);
    I.strokeRect(rect, border, selected ? 1.4f : 1.0f, 6.0f);
    // 选中态再加一条底部亮线，色弱/缩小时也能区分
    if (selected) {
        I.fillRect(SkRect::MakeXYWH(rect.fLeft + 6.0f, rect.fBottom - 2.0f,
                                    std::max(0.0f, rect.width() - 12.0f), 2.0f),
                   kColBtnSelBorder, 1.0f);
    }
    // 文字超宽时居中裁掉两侧（不换行、不溢出到相邻按钮）
    const float maxTextW = std::max(0.0f, rect.width() - 12.0f);
    const std::string shown = I.ellipsize(label, kFontBody, maxTextW);
    I.drawTextCentered(shown.c_str(), rect.centerX(), rect.centerY(), kFontBody, text);

    I.recordHit(rect, id, kHitButton, true);
    p.lastItemRight = rp.x + w;
    return clicked;
}

// ============================================================================
//  复选框（胶囊开关）
// ============================================================================
bool UiContext::checkbox(const char* label, bool* value) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !value) return false;

    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    const SkRect pill = SkRect::MakeXYWH(p.contentX + I.labelW, rp.y - kPillHeight * 0.5f,
                                         kPillWidth, kPillHeight);
    const uint64_t id = I.makeWidgetId(kHitCheck);

    const bool hovered = I.hitPrev(id);
    if (hovered && I.mousePressed() && I.activeId == 0) I.activeId = id;
    const bool held = (I.activeId == id);
    bool toggled = false;
    if (held && I.mouseReleased()) {
        I.activeId = 0;
        if (hovered) {
            *value = !*value;
            toggled = true;
        }
    }

    // 标签（左列）+ 胶囊 + 圆点
    I.drawTextAtRow(label, rp.x, rp.y, kFontBody, kColLabel, I.labelW);
    const bool on = *value;
    const SkColor pillFill = on ? kColGood : kColTrack;  // 开=绿 0xFF46A06E，关=0x46FFFFFF
    I.fillRect(pill, hovered ? (on ? 0xFF52B47E : 0x5CFFFFFF) : pillFill, kPillHeight * 0.5f);
    if (hovered || held) I.strokeRect(pill, 0x805AAAFF, 1.0f, kPillHeight * 0.5f);
    const float dotX = on ? (pill.fRight - kPillHeight * 0.5f) : (pill.fLeft + kPillHeight * 0.5f);
    I.fillCircle(dotX, pill.centerY(), 8.0f, SK_ColorWHITE);

    I.recordHit(pill, id, kHitCheck, true);
    p.lastItemRight = pill.fRight;
    return toggled;
}

// ============================================================================
//  滑块（浮点 / 整数）
// ============================================================================
bool UiContext::sliderFloat(const char* label, float* value, float minV, float maxV,
                            const char* fmt) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !value) return false;
    if (!(maxV > minV)) maxV = minV + 1.0f;

    const char* f = (fmt && *fmt) ? fmt : "%.2f";
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, static_cast<double>(*value));

    // 值文本占右列，轨道占中间
    float vw = I.textWidth(buf, kFontBody);
    if (vw > 88.0f) vw = 88.0f;
    const float trackX = p.contentX + I.labelW;
    const float trackRight = std::max(trackX + 40.0f, p.contentRight - vw - 10.0f);
    const float trackW = trackRight - trackX;

    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    const SkRect track =
            SkRect::MakeXYWH(trackX, rp.y - kTrackHeight * 0.5f, trackW, kTrackHeight);
    // 命中区比轨道高，方便按住拖柄
    const SkRect hitRect =
            SkRect::MakeXYWH(trackX - 4.0f, rp.y - 13.0f, trackW + 8.0f, 26.0f);

    const uint64_t id = Impl::makeSliderId(value);  // 用 value 地址做 id，拖动跨帧连续
    const bool hovered = I.hitPrev(id);
    const bool justPressed = hovered && I.mousePressed() && I.activeId == 0;
    if (justPressed) {
        I.activeId = id;
        I.sliderChanged = false;
    }
    const bool dragging = (I.activeId == id);

    // 按住拖柄或点击轨道任意位置都能拖动：直接用鼠标 x 反推 t
    if (dragging && (justPressed || I.in.leftDown)) {
        float t = (I.mouseLogicalX() - trackX) / trackW;
        t = clampf(t, 0.0f, 1.0f);
        const float nv = minV + t * (maxV - minV);
        if (nv != *value) {
            *value = nv;
            I.sliderChanged = true;  // 数值确实变过
        }
    }

    bool committed = false;
    if (dragging && I.mouseReleased()) {
        I.activeId = 0;
        committed = I.sliderChanged;  // 鼠标抬起时返回 true（仅在改过值时）
        I.sliderChanged = false;
    }

    // ------------------------------------------------ 绘制
    I.drawTextAtRow(label, rp.x, rp.y, kFontBody, kColLabel, I.labelW);

    float t = (*value - minV) / (maxV - minV);
    t = clampf(t, 0.0f, 1.0f);
    const float handleX = trackX + trackW * t;

    I.fillRect(track, kColTrack, kTrackHeight * 0.5f);
    if (t > 0.0f) {
        I.fillRect(SkRect::MakeXYWH(trackX, track.fTop, trackW * t, kTrackHeight), kColAccent,
                   kTrackHeight * 0.5f);
    }
    if (hovered || dragging) {
        I.fillCircle(handleX, rp.y, kHandleRadius + 4.0f, 0x335AAAFF);  // 柔光（纯色叠加）
    }
    I.fillCircle(handleX, rp.y, kHandleRadius, SK_ColorWHITE);

    // 值文本（交互后重新格式化，保证拖动中实时刷新）
    std::snprintf(buf, sizeof(buf), f, static_cast<double>(*value));
    I.drawTextRight(buf, p.contentRight, rp.y, kFontBody,
                    (hovered || dragging) ? SK_ColorWHITE : kColValue);

    I.recordHit(hitRect, id, kHitSlider, true);
    p.lastItemRight = p.contentRight;
    return committed;
}

bool UiContext::sliderInt(const char* label, int* value, int minV, int maxV) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !value) return false;
    if (maxV <= minV) maxV = minV + 1;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", *value);
    const float vw = std::min(88.0f, I.textWidth(buf, kFontBody));
    const float trackX = p.contentX + I.labelW;
    const float trackRight = std::max(trackX + 40.0f, p.contentRight - vw - 10.0f);
    const float trackW = trackRight - trackX;

    const Impl::RowPos rp = I.beginRowItem(kRowHeight);
    const SkRect track =
            SkRect::MakeXYWH(trackX, rp.y - kTrackHeight * 0.5f, trackW, kTrackHeight);
    const SkRect hitRect =
            SkRect::MakeXYWH(trackX - 4.0f, rp.y - 13.0f, trackW + 8.0f, 26.0f);

    const uint64_t id = Impl::makeSliderId(value);
    const bool hovered = I.hitPrev(id);
    const bool justPressed = hovered && I.mousePressed() && I.activeId == 0;
    if (justPressed) {
        I.activeId = id;
        I.sliderChanged = false;
    }
    const bool dragging = (I.activeId == id);

    if (dragging && (justPressed || I.in.leftDown)) {
        float t = (I.mouseLogicalX() - trackX) / trackW;
        t = clampf(t, 0.0f, 1.0f);
        const int nv = minV + static_cast<int>(std::lround(t * static_cast<float>(maxV - minV)));
        const int clamped = nv < minV ? minV : (nv > maxV ? maxV : nv);
        if (clamped != *value) {
            *value = clamped;
            I.sliderChanged = true;
        }
    }

    bool committed = false;
    if (dragging && I.mouseReleased()) {
        I.activeId = 0;
        committed = I.sliderChanged;
        I.sliderChanged = false;
    }

    I.drawTextAtRow(label, rp.x, rp.y, kFontBody, kColLabel, I.labelW);

    float t = static_cast<float>(*value - minV) / static_cast<float>(maxV - minV);
    t = clampf(t, 0.0f, 1.0f);
    const float handleX = trackX + trackW * t;

    I.fillRect(track, kColTrack, kTrackHeight * 0.5f);
    if (t > 0.0f) {
        I.fillRect(SkRect::MakeXYWH(trackX, track.fTop, trackW * t, kTrackHeight), kColAccent,
                   kTrackHeight * 0.5f);
    }
    if (hovered || dragging) I.fillCircle(handleX, rp.y, kHandleRadius + 4.0f, 0x335AAAFF);
    I.fillCircle(handleX, rp.y, kHandleRadius, SK_ColorWHITE);

    std::snprintf(buf, sizeof(buf), "%d", *value);
    I.drawTextRight(buf, p.contentRight, rp.y, kFontBody,
                    (hovered || dragging) ? SK_ColorWHITE : kColValue);

    I.recordHit(hitRect, id, kHitSlider, true);
    p.lastItemRight = p.contentRight;
    return committed;
}

// ============================================================================
//  垂直间隔
// ============================================================================
void UiContext::spacing(float height) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return;
    p.sameLinePending = false;
    // 列表内直接推进列表游标（同样累计进内容高度）
    if (I.listState.active) {
        const float h = (height > 0.0f) ? height : 0.0f;
        I.listState.cursorY += h;
        I.listState.contentH += h;
        return;
    }
    p.cursorY += (height > 0.0f) ? height : 0.0f;
    p.lastItemRight = p.contentX;
}

// ============================================================================
//  列表行：selectable / listItem
// ============================================================================
//  两套入口共用同一段实现（runRow），区别只有：
//    * listItem 只在 beginList/endList 之间有效，行宽固定占满容器；
//    * selectable 在列表内与 listItem 等价，在列表外走面板行游标（可指定宽度）。
bool UiContext::listItem(const char* text, bool selected) {
    Impl& I = *impl_;
    if (!I.listState.active || I.depth <= 0 || !I.canvas) return false;
    SkRect rowRect;
    if (!I.beginListRow(kListPad + kListItemPad, 0.0f, &rowRect)) return false;
    return runRow(text, selected, rowRect, /*inList=*/true);
}

bool UiContext::selectable(const char* text, bool selected, float width) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return false;

    SkRect rowRect;
    bool inList = false;
    if (I.listState.active) {
        // 列表内：与 listItem 同一套行游标/裁剪规则
        if (!I.beginListRow(kListPad + kListItemPad, width, &rowRect)) return false;
        inList = true;
    } else {
        // 列表外：普通面板行，宽默认占满内容区
        const Impl::RowPos rp = I.beginRowItem(kRowHeight);
        const float w = (width > 0.0f) ? width : (p.contentRight - p.contentX);
        rowRect = SkRect::MakeXYWH(rp.x, rp.y - kRowHeight * 0.5f, w, kRowHeight);
        p.lastItemRight = rp.x + w;
    }
    return runRow(text, selected, rowRect, inList);
}

// 行的公共实现：命中状态机 + 绘制 + 登记命中矩形。
// twoColumn=true 时按“左主文本 + 右标签”两列绘制（右标签用 rightColor）。
bool UiContext::runRow(const char* text, bool selected, const SkRect& rowRect, bool inList,
                       const char* rightText, SkColor rightColor, bool twoColumn) {
    Impl& I = *impl_;
    // 行 id 用“文本 + 面板/卡片标题”散列：即使同一帧里插入了别的控件，行的 id 也稳定，
    // 不会被 makeWidgetId 的序号偏移影响（否则 hover/click 会错位）。
    uint64_t id = hashStr(text && *text ? text : "row") ^ I.top().titleHash ^ 0x1209F1ULL;
    if (twoColumn && rightText && *rightText) id ^= hashStr(rightText) * 0x2545F4914F6CDD1DULL;

    // ------------------------------------------------ 命中状态机（按下 -> 松开）
    // 用“上一帧矩形”判 hover；inListItem 打开后 hitPrev 会额外要求鼠标在容器内。
    const bool prevFlag = I.inListItem;
    I.inListItem = inList;
    const bool hovered = I.hitPrev(id);
    I.inListItem = prevFlag;

    if (hovered && I.mousePressed() && I.activeId == 0) I.activeId = id;
    const bool held = (I.activeId == id);
    bool clicked = false;
    if (held && I.mouseReleased()) {
        I.activeId = 0;
        clicked = hovered;  // 抬起时鼠标仍在行上才算点击
    }

    // ------------------------------------------------ 绘制（只画落在容器内的行）
    const bool visible = inList ? I.rowVisible(rowRect) : true;
    if (visible) {
        // 选中 > hover > 透明：三层底色按优先级叠加
        if (selected) {
            I.fillRect(rowRect, kColRowSel, 4.0f);
        } else if (hovered || held) {
            I.fillRect(rowRect, kColRowHover, 4.0f);
        }
        if (held) I.strokeRect(rowRect, 0x805AAAFF, 1.0f, 4.0f);

        const SkColor textColor = selected ? SK_ColorWHITE : (hovered ? SK_ColorWHITE : kColText);
        const SkRect* clip = inList ? &rowRect : nullptr;
        const float textX = rowRect.fLeft + kListItemPad;

        if (twoColumn) {
            // 两列：右侧标签优先保留完整（右对齐、8px 右内边距），左侧拿到剩余宽度再省略。
            const float rightPad = kListItemPad;
            const float gap = 8.0f;  // 两列之间的最小间隙
            const float rightAvail = std::max(0.0f, rowRect.fRight - rightPad - textX);
            std::string rightShown;
            float rightW = 0.0f;
            if (rightText && *rightText) {
                rightShown = I.ellipsize(rightText, kFontBody, rightAvail);
                rightW = I.textWidth(rightShown.c_str(), kFontBody);
                I.drawTextAtRowClipped(rightShown, rowRect.fRight - rightPad - rightW,
                                       rowRect.centerY(), kFontBody,
                                       selected ? SK_ColorWHITE : rightColor, clip);
            }
            const float leftAvail = std::max(0.0f, rightAvail - rightW - gap);
            const std::string leftShown = I.ellipsize(text, kFontBody, leftAvail);
            I.drawTextAtRowClipped(leftShown, textX, rowRect.centerY(), kFontBody, textColor,
                                   clip);
        } else {
            // 单列：左对齐留 8px 内边距，过长用省略号
            const float maxW = std::max(0.0f, rowRect.fRight - kListItemPad - textX);
            const std::string shown = I.ellipsize(text, kFontBody, maxW);
            I.drawTextAtRowClipped(shown, textX, rowRect.centerY(), kFontBody, textColor, clip);
        }
    }

    I.recordHit(rowRect, id, kHitRow, true);
    return clicked;
}

// 两列列表行：左侧主文本 + 右侧彩色标签（渲染后端徽章这类用法）。
// 在列表内与 listItem 等价（走列表游标/裁剪），在列表外走普通行游标占满内容区。
bool UiContext::listItemEx(const char* left, const char* right, SkColor rightColor,
                           bool selected) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return false;

    SkRect rowRect;
    bool inList = false;
    if (I.listState.active) {
        if (!I.beginListRow(kListPad + kListItemPad, 0.0f, &rowRect)) return false;
        inList = true;
    } else {
        const Impl::RowPos rp = I.beginRowItem(kRowHeight);
        const float w = p.contentRight - p.contentX;
        rowRect = SkRect::MakeXYWH(rp.x, rp.y - kRowHeight * 0.5f, w, kRowHeight);
        p.lastItemRight = rp.x + w;
    }
    return runRow(left, selected, rowRect, inList, right, rightColor, /*twoColumn=*/true);
}

// ============================================================================
//  多列表格行（列对齐的关键实现）
// ============================================================================
//  为什么需要它：用 printf 的 "%-22.22s" 补空格在比例字体下**永远对不齐**
//  （空格宽 ≠ 数字宽 ≠ 汉字宽）。这里改成：每列给定逻辑宽度，逐列
//  measureText + 省略号 + 按 align 定位，所以不同行的同一列 x 完全一致。
//
//  列宽规则：
//    * width > 0  -> 固定宽度（该列的可用文字宽 = width - kColGap）
//    * width <= 0 -> 吃掉本行剩余宽度（通常给最后一列/标题列）
//  稳定 id：命中判定要求 id 跨帧稳定，默认取第一列文本；当显示文本会变
//  （窗口标题实时刷新、状态变化）时必须显式传 idText。
void UiContext::setLabelWidth(float width) {
    impl_->labelW = (width > 0.0f) ? width : kLabelWidth;
}

float UiContext::labelWidth() const { return impl_->labelW; }

bool UiContext::listItemColumns(const Column* cols, int count, bool selected,
                                const char* idText) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !cols || count <= 0) return false;

    SkRect rowRect;
    bool inList = false;
    if (I.listState.active) {
        if (!I.beginListRow(kListPad + kListItemPad, 0.0f, &rowRect)) return false;
        inList = true;
    } else {
        const Impl::RowPos rp = I.beginRowItem(kRowHeight);
        const float w = p.contentRight - p.contentX;
        rowRect = SkRect::MakeXYWH(rp.x, rp.y - kRowHeight * 0.5f, w, kRowHeight);
        p.lastItemRight = rp.x + w;
    }
    // 默认 id 用第一列文本；文本会变时调用方传 idText
    const char* id = (idText && *idText) ? idText : (cols[0].text ? cols[0].text : "row");
    return runColumns(cols, count, selected, inList, rowRect, kFontBody,
                      /*interactive=*/true, id);
}

void UiContext::headerColumns(const Column* cols, int count) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !cols || count <= 0) return;

    SkRect rowRect;
    bool inList = false;
    if (I.listState.active) {
        if (!I.beginListRowEx(kListPad + kListItemPad, 0.0f, kHeaderRowH, &rowRect)) return;
        inList = true;
    } else {
        const Impl::RowPos rp = I.beginRowItem(kHeaderRowH);
        const float w = p.contentRight - p.contentX;
        rowRect = SkRect::MakeXYWH(rp.x, rp.y - kHeaderRowH * 0.5f, w, kHeaderRowH);
        p.lastItemRight = rp.x + w;
    }
    runColumns(cols, count, /*selected=*/false, inList, rowRect, kHeaderFont,
               /*interactive=*/false, nullptr);
}

bool UiContext::runColumns(const Column* cols, int count, bool selected, bool inList,
                           const SkRect& rowRect, float fontSize, bool interactive,
                           const char* idText) {
    Impl& I = *impl_;
    const float textX = rowRect.fLeft + kListItemPad;
    const float textRight = rowRect.fRight - kListItemPad;

    // ---------------------------------------------------------- 命中状态机
    bool hovered = false, held = false, clicked = false;
    uint64_t id = 0;
    if (interactive) {
        id = hashStr(idText && *idText ? idText : "row") ^ I.top().titleHash ^ 0x51CE77ULL;
        const bool prevFlag = I.inListItem;
        I.inListItem = inList;
        hovered = I.hitPrev(id);
        I.inListItem = prevFlag;
        if (hovered && I.mousePressed() && I.activeId == 0) I.activeId = id;
        held = (I.activeId == id);
        if (held && I.mouseReleased()) {
            I.activeId = 0;
            clicked = hovered;
        }
    }

    const bool visible = inList ? I.rowVisible(rowRect) : true;
    if (visible) {
        if (selected) {
            I.fillRect(rowRect, kColRowSel, 4.0f);
        } else if (interactive && (hovered || held)) {
            I.fillRect(rowRect, kColRowHover, 4.0f);
        }
        if (held) I.strokeRect(rowRect, 0x805AAAFF, 1.0f, 4.0f);

        const SkColor base = selected ? SK_ColorWHITE
                                      : (hovered ? SK_ColorWHITE : kColText);
        const SkRect* clip = inList ? &rowRect : nullptr;

        // ------------------------------------------------------ 列宽分配
        // 先把固定列宽加起来，剩余宽度平分给"弹性列"（width<=0）。若没有弹性列，
        // 则把剩余宽度均摊给所有列——这样列不会挤在左边，右边也不会留一条空带。
        float fixed = 0.0f;
        int flexCount = 0;
        for (int i = 0; i < count; ++i) {
            if (cols[i].width > 0.0f) {
                fixed += cols[i].width;
            } else {
                ++flexCount;
            }
        }
        const float total = std::max(0.0f, textRight - textX);
        float flexW = 0.0f;
        float extraPer = 0.0f;
        if (flexCount > 0) {
            flexW = (total - fixed) / static_cast<float>(flexCount);
            if (flexW < 24.0f) flexW = 24.0f;  // 太窄时给下限，宁可裁掉也不把列压成 0
        } else if (count > 0 && fixed < total) {
            extraPer = (total - fixed) / static_cast<float>(count);
        }

        float x = textX;
        for (int i = 0; i < count; ++i) {
            const Column& c = cols[i];
            float cw = (c.width > 0.0f) ? (c.width + extraPer) : flexW;
            // 最后一列吃掉剩余宽度，避免累计误差把右边留出一条缝
            if (i == count - 1) {
                const float rest = textRight - x;
                if (rest > 0.0f) cw = rest;
            }
            const float avail = std::max(0.0f, cw - (i == count - 1 ? 0.0f : kColGap));

            const char* raw = (c.text && *c.text) ? c.text : c.fallback;
            if (raw && *raw && avail > 2.0f) {
                const std::string shown = I.ellipsize(raw, fontSize, avail);
                const float sw = I.textWidth(shown.c_str(), fontSize);
                float tx = x;
                if (c.align == Align::Right) {
                    tx = x + avail - sw;
                } else if (c.align == Align::Center) {
                    tx = x + (avail - sw) * 0.5f;
                }
                if (tx < x) tx = x;
                const SkColor color = selected ? SK_ColorWHITE
                                               : (c.color ? c.color : base);
                I.drawTextAtRowClipped(shown, tx, rowRect.centerY(), fontSize, color, clip);
            }
            x += cw;
            if (x > textRight) break;
        }

        // 数据行之间画一条很淡的分隔线，横向扫视时不会串行
        if (interactive && fontSize >= kFontBody - 0.5f) {
            I.line(rowRect.fLeft + 4.0f, rowRect.fBottom - 0.5f, rowRect.fRight - 4.0f,
                   rowRect.fBottom - 0.5f, kColRowLine, 1.0f);
        }
    }

    if (interactive) I.recordHit(rowRect, id, kHitRow, true);
    return clicked;
}

// ============================================================================
//  可滚动列表容器
// ============================================================================
void UiContext::beginList(const char* id, float width, float height, float scroll) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active || !I.canvas || I.listState.active) return;  // 不支持嵌套列表
    if (height <= 0.0f) return;

    const float w = (width > 0.0f) ? width : (p.contentRight - p.contentX);
    const float boxH = height + kListPad * 2.0f;
    const float x = p.contentX;
    const float y = p.cursorY - kRowHeight * 0.5f;  // 与普通行对齐

    // 面板游标一次性跳过整个容器（列表内部行不再推进面板游标）
    p.sameLinePending = false;
    p.cursorY = y + boxH;
    p.lastRowY = p.cursorY;
    p.lastItemRight = p.contentRight;

    // 列表状态
    I.listState.active = true;
    I.listState.id = hashStr(id && *id ? id : "list");
    // 本列表的行在命中判定时用"自己这个容器"的矩形做裁剪（不是别的列表的）
    I.inListId = I.listState.id;
    I.listState.depth = I.depth;
    I.listState.x = x;
    I.listState.y = y;
    I.listState.w = w;
    I.listState.h = boxH;
    I.listState.scroll = (scroll > 0.0f) ? scroll : 0.0f;
    // 行起点 = 容器顶 + 内边距 + 半行高 - scroll。
    // beginListRow 里行矩形 = (cursorY - 半行高, 行高)，所以这里加半行高刚好让
    // 第一行的顶边落在 y + kListPad（既不贴边框，也不会被容器裁掉）。
    I.listState.cursorY = y + kListPad + kRowHeight * 0.5f - I.listState.scroll;
    I.listState.contentH = 0.0f;
    I.listState.rows = 0;

    // 裁剪：save + clipRect，行画到容器外会被 Skia 直接丢掉
    const SkRect box = SkRect::MakeXYWH(x, y, w, boxH);
    I.listState.saveCount = I.canvas->save();
    I.canvas->clipRect(I.R(box));

    // 容器底色 + 1px 描边（在裁剪内画，边框不会溢出）
    I.fillRect(box, 0x22000000);
    I.strokeRect(box, 0x3F78AAFF, 1.0f, 6.0f);

    // 容器本身也是交互区（拖滚动条/点列表时宿主不能吞掉鼠标消息）
    I.recordHit(box, I.listState.id, kHitList, true);
}

void UiContext::endList() {
    Impl& I = *impl_;
    if (!I.listState.active) return;
    if (I.canvas) I.canvas->restoreToCount(I.listState.saveCount);
    // 按列表 id 记下容器矩形：下一帧该列表内的控件命中必须落在它里面
    I.storeListRect(I.listState.id,
                    SkRect::MakeXYWH(I.listState.x, I.listState.y, I.listState.w,
                                     I.listState.h));
    I.listState.active = false;
    I.inListItem = false;
    I.inListId = 0;
}

float UiContext::listContentHeight() const { return impl_->listState.contentH; }

// ============================================================================
//  只读路径框（点击返回 true，用于弹文件对话框）
// ============================================================================
bool UiContext::pathBox(const char* text, float width) {
    Impl& I = *impl_;
    Impl::Panel& p = I.top();
    if (!p.active) return false;

    SkRect box;
    bool inList = false;
    if (I.listState.active) {
        // 列表内：走列表游标（行高按 kRowHeight 计，框本身矮一点居中）
        if (!I.beginListRow(kListPad, width, &box)) return false;
        box.fTop = box.centerY() - kPathBoxH * 0.5f;
        box.fBottom = box.fTop + kPathBoxH;
        inList = true;
    } else {
        const Impl::RowPos rp = I.beginRowItem(kPathBoxH);
        const float w = (width > 0.0f) ? width : (p.contentRight - p.contentX);
        box = SkRect::MakeXYWH(rp.x, rp.y - kPathBoxH * 0.5f, w, kPathBoxH);
        p.lastItemRight = rp.x + w;
    }

    const uint64_t id = I.makeWidgetId(kHitPath);
    const bool prevFlag = I.inListItem;
    I.inListItem = inList;
    const bool hovered = I.hitPrev(id);
    I.inListItem = prevFlag;

    if (hovered && I.mousePressed() && I.activeId == 0) I.activeId = id;
    const bool held = (I.activeId == id);
    bool clicked = false;
    if (held && I.mouseReleased()) {
        I.activeId = 0;
        clicked = hovered;
    }

    // 绘制：深色底 + 1px 描边（hover 时描边变亮）+ 只读文本（超长右侧省略号）
    const bool visible = inList ? I.rowVisible(box) : true;
    if (visible) {
        I.fillRect(box, held ? 0x3A000000 : 0x28000000, 5.0f);
        I.strokeRect(box, (hovered || held) ? 0x8C5AAAFF : 0x4F78AAFF, 1.0f, 5.0f);
        const float textX = box.fLeft + 8.0f;
        const float maxW = std::max(0.0f, box.fRight - 8.0f - textX);
        const std::string shown = I.ellipsize(text, kFontSmall, maxW);
        const SkRect* clip = inList ? &box : nullptr;
        I.drawTextAtRowClipped(shown, textX, box.centerY(), kFontSmall,
                               (hovered || held) ? SK_ColorWHITE : kColValue, clip);
    }

    I.recordHit(box, id, kHitPath, true);
    return clicked;
}

}}  // namespace skiagui::ui
