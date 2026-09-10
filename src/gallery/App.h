// ============================================================================
//  gallery/App.h — 组件画廊（宿主无关）
// ----------------------------------------------------------------------------
//  宿主只做三件事：喂输入、给画布、呈现。其余全在这里（Q33）。
//
//  每帧顺序（与 uikit 的契约一致）：
//      app.OnInput(frame);   // frame 由 gallery::InputBridge 翻译而来（逻辑坐标）
//      app.Tick(dt);
//      uikit::PaintContext ctx(canvas);
//      app.Render(ctx, size);   // size 是逻辑尺寸
//
//  布局（Q6/Q14/Q22）：
//      顶栏（标题 / 搜索 / 主题 / HUD 按钮）
//      主体 = Sidebar 导航（可折叠，徽章显示组件数）+ 每分类一页的 ScrollView
//      卡片 = 纵向三段（标题栏由 Card 画 / 说明 / 演示区 / 回显行）
//
//  注意（M1 范围）：本轮**不含** uikit 的失效机制，树每帧全量 measure/layout/paint，
//  性能基线由 stats() 提供，M3 再按实测数据决定优化。
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gallery/Echo.h"
#include "gallery/InputBridge.h"
#include "gallery/Registry.h"
#include "uikit/UiKit.h"

namespace gallery {

// 头文件里少写一层命名空间，读起来清爽
namespace uikit = skiagui::uikit;

class App {
public:
    // 性能数据：三段分离才能判断瓶颈在 uikit 还是呈现层（Q30）
    struct Stats {
        float updateMs = 0.0f;     // OnInput + Tick
        float renderMs = 0.0f;     // 本帧 Render
        float renderP95Ms = 0.0f;  // 最近 300 帧的 P95（每 10 帧重算）
        float presentMs = 0.0f;    // 由宿主人为填入（App 不碰呈现层）
        float fps = 0.0f;
        int widgets = 0;
        int visibleCards = 0;
        int echoEntries = 0;
    };

    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // 一次性建树（**不在每帧建**）。重复调用是空操作。
    bool Init(const uikit::Theme& theme = uikit::Theme::Dark());
    void Shutdown();
    bool ready() const { return tree_ != nullptr; }

    void OnInput(const uikit::UiInputFrame& in);
    void Tick(float dt);
    void Render(uikit::PaintContext& ctx, uikit::Size available);

    // 命中测试：鼠标落在画廊内容上才要求宿主让出鼠标（Q53）
    bool WantsMouse() const;
    // 画廊可见且窗口没被关闭时才吞键盘（输入焦点在画廊里时打字不能穿透到宿主）
    bool WantsKeyboard() const { return tree_ != nullptr && windowVisible(); }

    // 每帧是否先把整块画布填成主题背景色。
    //   窗口宿主：true（画廊占满客户区，必须清屏，否则帧间残留会涂抹）；
    //   注入宿主：false（只画可拖动窗口，让宿主画面透出来）。
    void setBackgroundFill(bool v) { backgroundFill_ = v; }
    bool backgroundFill() const { return backgroundFill_; }
    const uikit::Theme& theme() const { return theme_; }

    // ---- 窗口模式（默认开）----
    // true  = 画廊画在一个可拖动窗口里（注入宿主必须用这个，不能糊住宿主画面）；
    // false = 全屏铺满（老行为，保留给离屏/回归用）。
    void setWindowMode(bool v) { windowMode_ = v; }
    bool windowMode() const { return windowMode_; }
    // 窗口矩形（屏幕/画布坐标）；每帧 Render 后刷新。鼠标只在这个矩形内被吞。
    uikit::Rect windowRect() const { return windowRect_; }
    bool windowVisible() const { return window_ ? window_->visible() : true; }
    // 是否给窗口加关闭按钮（注入宿主用；关闭 = 隐藏窗口，F9 再显示）
    void setWindowCloseButton(bool v) { windowCloseButton_ = v; }
    void setWindowSize(float w, float h) { windowSize_ = uikit::Size{w, h}; }
    uikit::DraggableWindow* window() const { return window_; }

    void SetTheme(const uikit::Theme& t);
    void ToggleTheme();
    bool dark() const { return dark_; }

    bool hudVisible() const { return hudVisible_; }
    void setHudVisible(bool v);
    void ToggleHud();

    int selectedCategory() const { return selectedCategory_; }
    void SelectCategory(int categoryIndex, bool resetScroll);
    void setSearch(const std::string& text);
    const std::string& search() const { return searchText_; }

    // 用最近一次渲染的尺寸离屏重绘一帧并编码成 PNG（两个宿主共用）
    bool SaveScreenshot(const char* path);

    const Stats& stats() const { return stats_; }
    void setPresentMs(float ms) { stats_.presentMs = ms; }
    const Registry& registry() const { return registry_; }
    uikit::WidgetTree* tree() { return tree_.get(); }

private:
    struct CardEntry {
        const CardSpec* spec = nullptr;
        uikit::Widget* card = nullptr;
        uikit::Text* summary = nullptr;
        uikit::Text* echo = nullptr;
        int categoryIndex = -1;
    };

    std::unique_ptr<uikit::Widget> BuildCard(const CardSpec& spec, int categoryIndex);
    void ApplyFilter();
    void RebuildNav();
    void OnNavChanged(int navIndex);
    void RecomputeP95();
    void UpdateHudText();
    int visibleCountInCategory(int categoryIndex) const;

    Registry registry_;
    InputBridge input_;
    Echo echo_;

    std::unique_ptr<uikit::WidgetTree> tree_;
    uikit::Sidebar* nav_ = nullptr;
    uikit::ScrollView* content_ = nullptr;
    uikit::SearchBox* search_ = nullptr;
    uikit::Button* themeBtn_ = nullptr;
    uikit::Button* hudBtn_ = nullptr;
    uikit::Text* emptyHint_ = nullptr;
    uikit::Widget* hudLayer_ = nullptr;
    uikit::Text* hudText_ = nullptr;

    std::vector<uikit::Widget*> pages_;       // 每分类一个 Column
    std::vector<CardEntry> cards_;            // 40 张卡片
    std::vector<int> navCategories_;          // 导航项下标 -> 分类下标
    std::vector<std::pair<uikit::Widget*, std::function<void(uikit::Widget*)>>> pendingAttach_;

    std::string searchText_;
    int selectedCategory_ = 0;
    bool dark_ = true;
    bool hudVisible_ = false;
    bool backgroundFill_ = true;
    bool windowMode_ = true;
    bool windowCloseButton_ = false;
    bool windowSized_ = false;
    uikit::Size windowSize_{1280.0f, 800.0f};
    uikit::DraggableWindow* window_ = nullptr;
    uikit::Rect windowRect_ = SkRect::MakeEmpty();
    bool suppressNavCallback_ = false;
    uikit::Theme theme_;

    uikit::Size lastRenderSize_{};
    float elapsed_ = 0.0f;
    uikit::Point lastMouse_{};
    bool lastMouseValid_ = false;

    Stats stats_;
    std::vector<float> renderSamples_;
    int sampleCursor_ = 0;
    int framesSinceP95_ = 0;
};

}  // namespace gallery
