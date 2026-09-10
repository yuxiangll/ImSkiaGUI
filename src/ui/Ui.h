// ============================================================================
//  Ui.h — 即时模式（Immediate-Mode）UI 的公开接口
// ----------------------------------------------------------------------------
//  设计目标：
//    * 不做保留模式控件树、不做字体图集、不依赖 ImGui —— 每帧用 Skia 直接重画；
//    * 命中测试与绘制共用同一套矩形：控件绘制时把矩形记进本帧命中列表，
//      鼠标判定读上一帧列表（避免同帧先后顺序导致“点不到”的问题）；
//    * WndProc 通过 wantsMouse()/wantsKeyboard() 决定是否吞掉消息。
//  坐标约定：
//    * 所有公开 API 的 x/y/w/h 都是“逻辑像素”（96 DPI = 1.0）；
//    * InputState 里的鼠标坐标是渲染像素；内部按 dpiScale 换算。
//  线程约定：UiContext 只在渲染线程使用，内部不加锁。
// ============================================================================
#pragma once
#include "input/InputState.h"   // 已存在，定义了 skiagui::ui::InputState
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkRect.h"
#include "include/core/SkTypeface.h"
#include <cstdint>

namespace skiagui { namespace ui {

class UiContext {
public:
    UiContext();
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    // 初始化 DirectWrite 字体管理器（拉丁用 Segoe UI，中文用 Microsoft YaHei）。
    // 失败返回 false，调用方会放弃 UI 但继续渲染。可重复调用。
    bool initFonts();

    // DPI 缩放：96 DPI = 1.0。必须在 beginFrame 之前设置。
    void setDpiScale(float scale);

    // 每帧开始：清空布局游标、消费输入快照。
    // width/height 是渲染目标像素尺寸；input 里的鼠标坐标也是渲染像素。
    // 内部把所有“逻辑像素”乘以 dpiScale 变成渲染像素。
    void beginFrame(SkCanvas* canvas, int width, int height, const InputState& input);

    // 每帧结束：结算 wantsMouse()/wantsKeyboard()（供 WndProc 决定是否吞掉消息）。
    void endFrame();

    // 面板。x/y/w/h 都是逻辑像素。open 可为 nullptr（不可关闭）。
    // 返回 false 表示面板处于关闭状态（*open==false），此时**不要**调用 endPanel()。
    bool beginPanel(const char* title, float x, float y, float w, float h, bool* open = nullptr);
    void endPanel();

    void separator();
    void label(const char* text, SkColor color);           // 左列标签（kLabelWidth 宽）
    void text(const char* text);                           // 整行普通文本
    void textColored(const char* text, SkColor color);
    void sameLine(float spacing);                          // 之后的行与上一行同一水平线
    void progressBar(float t01, const char* overlayText);  // overlayText 可为 nullptr
    void textLine(const char* left, const char* right);    // 一行“标签 : 值”

    // 控件。width<=0 表示占满面板剩余宽度。
    // selected=true 时画成"已选中"样式（实心主题蓝 + 白字 + 亮描边），
    // 用于单选按钮组 / 注入方式选择这类需要显示当前选项的场合。
    bool button(const char* label, float width, bool selected = false);
    bool checkbox(const char* label, bool* value);
    bool sliderFloat(const char* label, float* value, float minV, float maxV, const char* fmt);
    bool sliderInt(const char* label, int* value, int minV, int maxV);

    // 垂直间隔（逻辑像素）
    void spacing(float height);

    // 列表行：整行可点，返回 true 表示这一帧被点击（松开时）。
    // width<=0 表示占满当前容器宽度。selected 为 true 时高亮。
    bool selectable(const char* text, bool selected, float width = 0.0f);

    // 可滚动列表容器：内部用 Skia clip 裁剪绘制，命中测试也限制在容器矩形内。
    // scroll 是调用方维护的滚动偏移（逻辑像素，0 = 顶部）。
    // 必须在 beginPanel/endPanel 之间调用；listItem 只能在 beginList/endList 之间调用。
    void beginList(const char* id, float width, float height, float scroll);
    void endList();
    bool listItem(const char* text, bool selected);      // beginList 内使用
    float listContentHeight() const;                     // 最近一次 beginList 的内容总高度

    // 只读文本输入框（用于显示 DLL 路径，不支持编辑；点击返回 true 以便弹文件对话框）
    bool pathBox(const char* text, float width = 0.0f);

    // 卡片：现代卡片风格容器（圆角 + 细边框 + 可选小标题 + 内边距）。
    // 与 beginPanel 的区别：没有投影偏移、没有关闭按钮、标题更小更淡，
    // 用于在无边框窗口里纵向堆叠多张卡片。
    // x/y/w/h 逻辑像素；title 可为 nullptr（不画标题行）。
    // 返回 true 表示可以继续往里加控件；调用方必须成对调用 endCard()。
    bool beginCard(const char* id, const char* title, float x, float y, float w, float h);
    void endCard();

    // 两列列表行：左侧主文本（左对齐、超长省略号），右侧小标签（右对齐、可着色，
    // 用于显示渲染后端这类"徽章"）。selected 时整行高亮，返回 true 表示本帧被点击。
    bool listItemEx(const char* left, const char* right, SkColor rightColor, bool selected);

    // ------------------------------------------------------------------------
    //  多列行（表格对齐）—— 修复"名字/pid/描述对不齐"
    // ------------------------------------------------------------------------
    //  背景：以前调用方用 "%-22.22s pid %-6lu" 这种 printf 补空格来对齐，但
    //  Skia 用的是比例字体（Segoe UI / 雅黑），空格宽度 ≠ 数字宽度，列永远歪。
    //  这里改成"每列给定宽度 + 逐个 measureText 定位"：列边界由调用方按逻辑像素
    //  显式指定（宽度<=0 表示吃掉该行剩余宽度），因此各行的列一定严格对齐。
    //  Align：左对齐 / 右对齐（数字列用它，位数变化也不会串列）/ 居中。
    enum class Align : uint8_t { Left = 0, Right, Center };

    struct Column {
        const char* text = "";
        float width = 0.0f;                  // 逻辑像素；<=0 = 占满剩余宽度
        Align align = Align::Left;
        SkColor color = 0;                    // 0 = 用默认文字色
        const char* fallback = nullptr;       // 本列为空时显示它（如 "(无标题)"）
    };

    // 多列表格行：整行可点（列表内走列表游标 + 裁剪，列表外走普通行游标）。
    // idText 用于生成**稳定行 id**（默认取第一列文本）——命中判定靠 id 跨帧稳定，
    // 所以当显示文本会变（窗口标题刷新、状态变化）时必须传入不变的 idText，
    // 否则按下与抬起之间文本一变，点击就丢失。
    bool listItemColumns(const Column* cols, int count, bool selected,
                         const char* idText = nullptr);
    // 表头行（不可点、字号更小、次要颜色、行高更矮），列宽应与数据行一致。
    void headerColumns(const Column* cols, int count);
    // 设置 label()/checkbox()/slider* 的左侧标签列宽（默认 130）。
    void setLabelWidth(float width);
    float labelWidth() const;

    bool wantsMouse() const;      // 上一帧鼠标是否落在交互控件上（WndProc 拦截依据）
    bool wantsKeyboard() const;
    float dpiScale() const;
    int panelCount() const;       // 本帧打开的面板数（自测用）
    // ---- 诊断（自测用）----
    int hitCount() const;                 // 本帧登记的命中矩形数
    int prevHitCount() const;             // 上一帧的命中矩形数
    int listRectCount() const;            // 已知的列表容器数（本帧之前登记过的）
    SkRect listRectById(int index) const; // 第 index 个列表容器的矩形（逻辑像素）
    bool hitRectForId(uint64_t id, float* l, float* t, float* r, float* b) const;

    // 命中条目（只读快照，给外部诊断/自动化验证用：能拿到"哪一行在屏幕上的矩形"）
    struct HitEntry {
        uint64_t id = 0;
        uint8_t kind = 0;
        SkRect rect{};      // 逻辑像素
        bool interactive = false;
    };
    // 取第 index 个命中条目；越界返回 false。
    bool hitEntryAt(int index, HitEntry* out) const;

private:
    struct Impl;                  // 所有私有状态放这里，头文件不暴露
    Impl* impl_;

    // selectable / listItem / listItemEx 共用的行实现（命中状态机 + 绘制 + 登记命中矩形）。
    // inList=true 时只画落在容器内的行，并且命中被容器矩形裁剪。
    // rightText 非空时按“左主文本 + 右标签”两列绘制（右标签用 rightColor）。
    bool runRow(const char* text, bool selected, const SkRect& rowRect, bool inList,
                const char* rightText = nullptr, SkColor rightColor = 0, bool twoColumn = false);

    // 多列行的公共实现（listItemColumns / headerColumns 共用）：
    // 按 Column.width 分配 x，逐列 measureText + 省略号，只画落在容器内的行。
    // interactive=false 时（表头）不登记命中、不参与 hover/click。
    bool runColumns(const Column* cols, int count, bool selected, bool inList,
                    const SkRect& rowRect, float fontSize, bool interactive,
                    const char* idText);
};
}}  // namespace
