// ============================================================================
//  gallery/App.cpp — 画廊实现
// ============================================================================
#include "gallery/App.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include "gallery/CardBuilder.h"

namespace gallery {

namespace {

constexpr int kSampleCount = 300;      // P95 环形缓冲（Q55）
constexpr int kP95RecalcEvery = 10;    // 每 10 帧重算一次
constexpr float kNavWidth = 200.0f;

using Clock = std::chrono::steady_clock;

double NowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// 大小写不敏感的子串匹配（中文不受影响）
bool ContainsLower(const char* hay, const std::string& needleLower) {
    if (!hay) return false;
    if (needleLower.empty()) return true;
    const std::string h = skiagui::uikit::utf8::ToLower(hay);
    return h.find(needleLower) != std::string::npos;
}

int CountWidgets(const skiagui::uikit::Widget* w) {
    if (!w) return 0;
    int n = 1;
    for (const auto& c : w->children()) n += CountWidgets(c.get());
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
//  构造 / 销毁
// ---------------------------------------------------------------------------
App::App() = default;
App::~App() { Shutdown(); }

bool App::Init(const skiagui::uikit::Theme& theme) {
    using namespace skiagui::uikit;

    if (tree_) return true;

    dark_ = theme.dark;
    theme_ = theme;
    registry_.Build();
    echo_.Clear();
    cards_.clear();
    pages_.clear();
    navCategories_.clear();
    pendingAttach_.clear();
    renderSamples_.clear();
    sampleCursor_ = 0;

    const Theme& th = theme;

    // ---- 根：顶栏 + 主体（窗口模式下会被包进 DraggableWindow）----
    auto contentRoot = std::make_unique<Column>();
    contentRoot->setId("gallery.root");
    contentRoot->setGap(0.0f);

    auto topBar = std::make_unique<Row>();
    topBar->setId("gallery.topbar");
    topBar->setGap(8.0f);
    topBar->setAlign(layout::Align::Center);
    topBar->setPadding(EdgeInsets::Symmetric(8.0f, 12.0f));
    topBar->fixedHeight(52.0f);

    auto title = std::make_unique<Text>("skiagui · UI Kit 组件画廊");
    title->setId("gallery.title");
    title->setFontSize(18.0f);
    title->setWeight(700);
    topBar->addChild(std::move(title));

    auto spacer = std::make_unique<Spacer>();
    spacer->grow(1.0f);
    topBar->addChild(std::move(spacer));

    auto search = std::make_unique<SearchBox>("搜索组件名或说明");
    search->setId("gallery.search");
    search->fixedWidth(260.0f);
    search_ = search.get();
    topBar->addChild(std::move(search));

    auto themeBtn = std::make_unique<Button>("Dark / Light");
    themeBtn->setId("gallery.theme");
    themeBtn_ = themeBtn.get();
    topBar->addChild(std::move(themeBtn));

    auto hudBtn = std::make_unique<Button>("HUD");
    hudBtn->setId("gallery.hud");
    hudBtn_ = hudBtn.get();
    topBar->addChild(std::move(hudBtn));

    contentRoot->addChild(std::move(topBar));

    auto body = std::make_unique<Row>();
    body->setId("gallery.body");
    body->setGap(0.0f);
    // flex: 1 —— flexBasis=0 很关键：否则 pass 1 会拿"内容自然高度"（ScrollView 的
    // 全内容高度，上万像素）当主轴基准，主轴溢出后 flexShrink 会把固定高度的顶栏
    // 按比例压小，压缩率随内容测量值逐帧变化 → 整棵树纵向抖动、点击命中错位。
    body->layoutParams().flexBasis = 0.0f;
    body->grow(1.0f);

    auto nav = std::make_unique<Sidebar>();
    nav->setId("gallery.nav");
    nav->setHeader("分类");
    nav->setExpandedWidth(kNavWidth);
    nav->setItemHeight(34.0f);
    nav_ = nav.get();
    body->addChild(std::move(nav));

    auto content = std::make_unique<ScrollView>();
    content->setId("gallery.content");
    content->setGap(14.0f);
    content->setPadding(EdgeInsets::Uniform(16.0f));
    content->grow(1.0f);
    content_ = content.get();

    // 每个分类一页（Column），全部建好、靠可见性切换 —— 保住交互状态（Q47）
    for (int i = 0; i < registry_.categoryCount(); ++i) {
        auto page = std::make_unique<Column>();
        page->setId(std::string("gallery.page.") + registry_.categories()[i].id);
        page->setGap(14.0f);
        page->setVisible(false);
        pages_.push_back(page.get());
        content->addChild(std::move(page));
    }

    // 搜索无结果时的提示行
    auto hint = std::make_unique<Text>("");
    hint->setId("gallery.emptyhint");
    hint->setFontSize(13.0f);
    hint->setColor(th.textMuted);
    hint->setVisible(false);
    emptyHint_ = hint.get();
    content->addChild(std::move(hint));

    // 卡片（一次性构建）
    for (const CardSpec& spec : registry_.cards()) {
        const int ci = registry_.categoryIndex(spec.category);
        if (ci < 0) continue;
        pages_[ci]->addChild(BuildCard(spec, ci));
    }

    body->addChild(std::move(content));
    contentRoot->addChild(std::move(body));

    // ---- HUD 浮层（挂在覆盖层，右下角，不吃点击）----
    auto hudLayer = std::make_unique<Stack>(HAlign::Right, VAlign::Bottom);
    hudLayer->setId("gallery.hud.layer");
    hudLayer->setHitTransparent(true);
    hudLayer->setPadding(EdgeInsets::Uniform(12.0f));
    hudLayer->setVisible(false);

    auto hudCard = std::make_unique<Card>();
    hudCard->setId("gallery.hud.card");
    hudCard->setHitTransparent(true);
    hudCard->setPadding(EdgeInsets::Uniform(10.0f));

    auto hudText = std::make_unique<Text>("");
    hudText->setId("gallery.hud.text");
    hudText->setMonospace(true);
    hudText->setFontSize(11.0f);
    hudText->setColor(th.textSecondary);
    hudText_ = hudText.get();
    hudCard->addChild(std::move(hudText));
    hudLayer->addChild(std::move(hudCard));
    hudLayer_ = hudLayer.get();

    // ---- 装配根：窗口模式把内容包进可拖动窗口（HUD 也放进去）----
    std::unique_ptr<Widget> root;
    if (windowMode_) {
        auto win = std::make_unique<DraggableWindow>("skiagui · UI Kit 组件画廊");
        win->setId("gallery.window");
        win->setWindowSize(windowSize_.w, windowSize_.h);
        win->setWindowPos(24.0f, 24.0f);
        win->setClampToParent(true);
        win->setShadow(false);  // CPU 光栅下每帧画大阴影太贵，靠边框区分窗口边界
        win->setCloseButton(windowCloseButton_);
        win->setOnClose([this] {
            if (window_) window_->setVisible(false);
        });
        window_ = win.get();

        auto winContent = std::make_unique<Stack>(HAlign::Stretch, VAlign::Stretch);
        winContent->setId("gallery.window.content");
        winContent->addChild(std::move(contentRoot));
        // HUD 放进窗口内部：这样点它也落在窗口矩形里，不会被判成"点到了宿主"
        winContent->addChild(std::move(hudLayer));
        win->addChild(std::move(winContent));

        auto stack = std::make_unique<Stack>(HAlign::Stretch, VAlign::Stretch);
        stack->setId("gallery.rootstack");
        stack->addChild(std::move(win));
        root = std::move(stack);
    } else {
        root = std::move(contentRoot);
    }

    tree_ = std::make_unique<WidgetTree>(std::move(root));
    tree_->setTheme(th);
    if (!windowMode_) tree_->overlayRoot()->addChild(std::move(hudLayer));

    // ---- 接线 ----
    if (search_) {
        search_->setOnChange([this](const std::string& t) {
            searchText_ = t;
            ApplyFilter();
        });
    }
    if (themeBtn_) themeBtn_->setOnClick([this] { ToggleTheme(); });
    if (hudBtn_) hudBtn_->setOnClick([this] { ToggleHud(); });
    if (nav_) {
        nav_->setOnChange([this](int idx) { OnNavChanged(idx); });
    }

    // 浮层控件必须等挂树之后才能 addOverlayChild
    for (auto& p : pendingAttach_) {
        if (p.second && p.first) p.second(p.first);
    }
    pendingAttach_.clear();

    stats_.echoEntries = echo_.count();
    RebuildNav();
    SelectCategory(0, true);
    ApplyFilter();
    UpdateHudText();
    return true;
}

void App::Shutdown() {
    if (!tree_) return;
    // 先断开回调，避免销毁过程中触发回调访问半销毁状态
    if (search_) search_->setOnChange(nullptr);
    if (themeBtn_) themeBtn_->setOnClick(nullptr);
    if (hudBtn_) hudBtn_->setOnClick(nullptr);
    if (nav_) nav_->setOnChange(nullptr);

    nav_ = nullptr;
    content_ = nullptr;
    search_ = nullptr;
    themeBtn_ = nullptr;
    hudBtn_ = nullptr;
    emptyHint_ = nullptr;
    hudLayer_ = nullptr;
    hudText_ = nullptr;
    pages_.clear();
    cards_.clear();
    navCategories_.clear();
    pendingAttach_.clear();
    echo_.Clear();
    tree_.reset();
}

// ---------------------------------------------------------------------------
//  建卡片
// ---------------------------------------------------------------------------
std::unique_ptr<skiagui::uikit::Widget> App::BuildCard(const CardSpec& spec, int categoryIndex) {
    using namespace skiagui::uikit;

    // 外壳与演示内容来自与离屏装配共用的同一份实现（Q39）
    BuiltCard built = BuildCardWidget(spec, dark_ ? Theme::Dark() : Theme::Light());

    CardEntry e;
    e.spec = &spec;
    e.card = built.card.get();
    e.summary = built.summary;
    e.echo = built.echo;
    e.categoryIndex = categoryIndex;
    cards_.push_back(e);

    if (built.echoFn) echo_.Add(built.echoFn, built.echo);
    if (built.attachedFn && built.demoView) {
        pendingAttach_.emplace_back(built.demoView, built.attachedFn);
    }

    return std::move(built.card);
}

// ---------------------------------------------------------------------------
//  过滤 / 导航 / 分类切换
// ---------------------------------------------------------------------------
int App::visibleCountInCategory(int categoryIndex) const {
    int n = 0;
    for (const CardEntry& e : cards_) {
        if (e.categoryIndex == categoryIndex && e.card && e.card->visible()) ++n;
    }
    return n;
}

void App::ApplyFilter() {
    using namespace skiagui::uikit;

    const std::string needle = utf8::ToLower(utf8::Trim(searchText_));

    int visibleCards = 0;
    std::vector<int> perCategory(registry_.categoryCount(), 0);
    for (CardEntry& e : cards_) {
        const bool match = needle.empty() || ContainsLower(e.spec->name, needle) ||
                           ContainsLower(e.spec->summary, needle);
        if (e.card) e.card->setVisible(match);
        if (match) {
            ++visibleCards;
            if (e.categoryIndex >= 0 && e.categoryIndex < static_cast<int>(perCategory.size())) {
                ++perCategory[e.categoryIndex];
            }
        }
    }
    stats_.visibleCards = visibleCards;

    // 空分类从导航里消失（Q41）
    RebuildNav();

    // 当前分类被过滤空了 -> 跳到第一个非空分类
    if (selectedCategory_ >= 0 && selectedCategory_ < static_cast<int>(perCategory.size()) &&
        perCategory[selectedCategory_] == 0) {
        for (int i = 0; i < static_cast<int>(perCategory.size()); ++i) {
            if (perCategory[i] > 0) {
                SelectCategory(i, true);
                break;
            }
        }
    }

    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        pages_[i]->setVisible(i == selectedCategory_);
    }

    // 无结果提示
    if (emptyHint_) {
        if (visibleCards == 0) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "没有匹配 \"%s\" 的组件", searchText_.c_str());
            emptyHint_->setText(buf);
            emptyHint_->setVisible(true);
        } else {
            emptyHint_->setVisible(false);
        }
    }
}

void App::RebuildNav() {
    using namespace skiagui::uikit;

    if (!nav_) return;

    suppressNavCallback_ = true;
    nav_->clearItems();
    navCategories_.clear();

    for (int i = 0; i < registry_.categoryCount(); ++i) {
        const int n = visibleCountInCategory(i);
        if (n == 0) continue;

        Sidebar::Item item;
        item.label = registry_.categories()[i].name;
        item.glyph = registry_.categories()[i].glyph;
        item.badge = std::to_string(n);
        nav_->addItem(item);
        navCategories_.push_back(i);
    }

    int navIdx = -1;
    for (int k = 0; k < static_cast<int>(navCategories_.size()); ++k) {
        if (navCategories_[k] == selectedCategory_) navIdx = k;
    }
    if (navIdx >= 0) nav_->setSelectedIndex(navIdx);
    suppressNavCallback_ = false;
}

void App::OnNavChanged(int navIndex) {
    if (suppressNavCallback_) return;
    if (navIndex < 0 || navIndex >= static_cast<int>(navCategories_.size())) return;
    SelectCategory(navCategories_[navIndex], true);
}

void App::SelectCategory(int categoryIndex, bool resetScroll) {
    if (categoryIndex < 0 || categoryIndex >= registry_.categoryCount()) return;
    selectedCategory_ = categoryIndex;

    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        pages_[i]->setVisible(i == categoryIndex);
    }
    if (resetScroll && content_) content_->setScrollY(0.0f);  // 切分类重置滚动（Q41）

    int navIdx = -1;
    for (int k = 0; k < static_cast<int>(navCategories_.size()); ++k) {
        if (navCategories_[k] == categoryIndex) navIdx = k;
    }
    if (navIdx >= 0 && nav_) {
        suppressNavCallback_ = true;
        nav_->setSelectedIndex(navIdx);
        suppressNavCallback_ = false;
    }
}

void App::setSearch(const std::string& text) {
    searchText_ = text;
    if (search_) search_->setText(text);
    ApplyFilter();
}

// ---------------------------------------------------------------------------
//  主题 / HUD
// ---------------------------------------------------------------------------
void App::SetTheme(const skiagui::uikit::Theme& t) {
    using namespace skiagui::uikit;

    dark_ = t.dark;
    theme_ = t;
    if (tree_) tree_->setTheme(t);

    // 显式设过颜色的文本要跟着主题走
    for (CardEntry& e : cards_) {
        if (e.summary) e.summary->setColor(t.textMuted);
        if (e.echo) e.echo->setColor(t.textSecondary);
    }
    if (hudText_) hudText_->setColor(t.textSecondary);
    if (emptyHint_) emptyHint_->setColor(t.textMuted);
    UpdateHudText();
}

void App::ToggleTheme() {
    using namespace skiagui::uikit;
    SetTheme(dark_ ? Theme::Light() : Theme::Dark());
}

void App::setHudVisible(bool v) {
    hudVisible_ = v;
    if (hudLayer_) hudLayer_->setVisible(v);
    if (v) UpdateHudText();
}

void App::ToggleHud() { setHudVisible(!hudVisible_); }

void App::UpdateHudText() {
    if (!hudText_) return;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "widgets=%d cards=%d echo=%d | upd=%.2fms ren=%.2fms P95=%.2fms present=%.2fms | %.0ffps",
                  stats_.widgets, stats_.visibleCards, stats_.echoEntries, stats_.updateMs,
                  stats_.renderMs, stats_.renderP95Ms, stats_.presentMs, stats_.fps);
    hudText_->setText(buf);
}

void App::RecomputeP95() {
    if (renderSamples_.empty()) {
        stats_.renderP95Ms = 0.0f;
        return;
    }
    std::vector<float> tmp = renderSamples_;
    const std::size_t idx = static_cast<std::size_t>(
            std::min<double>(tmp.size() - 1, std::floor(0.95 * (tmp.size() - 1) + 0.5)));
    std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
    stats_.renderP95Ms = tmp[idx];
}

// ---------------------------------------------------------------------------
//  每帧
// ---------------------------------------------------------------------------
void App::OnInput(const skiagui::uikit::UiInputFrame& in) {
    if (!tree_) return;
    lastMouse_ = skiagui::uikit::Point{in.mouseX, in.mouseY};
    lastMouseValid_ = in.mouseValid;

    const double t0 = NowSeconds();
    tree_->update(in);
    stats_.updateMs = static_cast<float>((NowSeconds() - t0) * 1000.0);
}

void App::Tick(float dt) {
    if (!tree_) return;
    elapsed_ += dt;

    const double t0 = NowSeconds();
    tree_->tick(dt);
    echo_.Update(elapsed_);
    stats_.updateMs += static_cast<float>((NowSeconds() - t0) * 1000.0);

    if (dt > 0.0f && dt < 0.5f) {
        const float inst = 1.0f / dt;
        stats_.fps = stats_.fps <= 0.0f ? inst : stats_.fps * 0.9f + inst * 0.1f;
    }
}

void App::Render(skiagui::uikit::PaintContext& ctx, skiagui::uikit::Size available) {
    if (!tree_) return;

    const double t0 = NowSeconds();

    // 窗口首次拿到画布尺寸时定尺寸/位置：取可用区域的 ~78%/82%，居中偏上。
    // 不能取"父容器减边距"——那样窗口刚好占满，夹紧后完全无法拖动。
    if (window_ && !windowSized_ && available.w > 1.0f && available.h > 1.0f) {
        windowSized_ = true;
        const float w = std::min(1280.0f, std::max(640.0f, available.w * 0.78f));
        const float h = std::min(800.0f, std::max(420.0f, available.h * 0.82f));
        window_->setWindowSize(w, h);
        window_->setWindowPos(std::max(16.0f, (available.w - w) * 0.5f),
                              std::max(16.0f, (available.h - h) * 0.35f));
    }

    // 先铺底色：画廊自己不画背景，宿主也不清屏的话，帧间残留会涂抹
    if (backgroundFill_ && available.w > 0.0f && available.h > 0.0f) {
        ctx.fillRect(skiagui::uikit::Rect::MakeWH(available.w, available.h), theme_.background);
    }

    tree_->render(ctx, available);
    stats_.renderMs = static_cast<float>((NowSeconds() - t0) * 1000.0);
    lastRenderSize_ = available;
    if (window_) windowRect_ = window_->windowRect();  // 供 WantsMouse 判定吞没边界

    // P95 采样（Q55：环形缓冲 + 每 10 帧重算，避免排序本身成为开销）
    if (static_cast<int>(renderSamples_.size()) < kSampleCount) {
        renderSamples_.push_back(stats_.renderMs);
    } else {
        renderSamples_[sampleCursor_] = stats_.renderMs;
        sampleCursor_ = (sampleCursor_ + 1) % kSampleCount;
    }

    if (++framesSinceP95_ >= kP95RecalcEvery) {
        framesSinceP95_ = 0;
        RecomputeP95();
        stats_.widgets = tree_->root() ? CountWidgets(tree_->root()) : 0;
        stats_.echoEntries = echo_.count();
        if (hudVisible_) UpdateHudText();
    }
}

bool App::WantsMouse() const {
    using namespace skiagui::uikit;
    if (!tree_ || !lastMouseValid_ || !windowVisible()) return false;
    if (windowMode_) {
        // 窗口模式：**整个窗口矩形**都吞，包括 hitTransparent 的 HUD 面板。
        // 只用 hitTest 判定的老做法会让"点在 HUD 上"穿透到宿主/游戏。
        return windowRect_.contains(lastMouse_.x(), lastMouse_.y());
    }
    Widget* hit = tree_->hitTest(lastMouse_);
    return hit != nullptr && !hit->hitTransparent();
}

// ---------------------------------------------------------------------------
//  截图（离屏重绘 + PNG）
// ---------------------------------------------------------------------------
bool App::SaveScreenshot(const char* path) {
    using namespace skiagui::uikit;

    if (!tree_ || !path) return false;

    float w = lastRenderSize_.w;
    float h = lastRenderSize_.h;
    if (w < 1.0f || h < 1.0f) {
        w = 1600.0f;
        h = 1000.0f;
    }
    const int iw = static_cast<int>(w);
    const int ih = static_cast<int>(h);

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    if (!surface) return false;

    const Theme& th = theme_;
    surface->getCanvas()->clear(th.background);

    PaintContext ctx(surface->getCanvas());
    tree_->render(ctx, Size{w, h});

    sk_sp<SkImage> image = surface->makeImageSnapshot();
    if (!image) return false;
    sk_sp<SkData> png = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
    if (!png) return false;

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        // 目录可能还不存在（默认写到 output\artifacts\），补建一次再试
        std::error_code ec;
        const std::filesystem::path p(path);
        if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path(), ec);
        f = std::fopen(path, "wb");
    }
    if (!f) return false;
    const std::size_t written = std::fwrite(png->data(), 1, png->size(), f);
    std::fclose(f);
    return written == png->size();
}

}  // namespace gallery
