// ============================================================================
//  gallery/Card.h — 组件画廊的卡片契约（宿主无关）
// ----------------------------------------------------------------------------
//  一张卡片 = 元数据（名称 / 分类 / 说明）+ 一个演示区构建器 + 一个回显函数。
//  画廊外壳（导航 / 滚动 / 卡片外观）由 App 负责，**这里只描述演示内容**，
//  所以每个 content/*.cpp 只依赖 uikit，不依赖宿主、不依赖 App。
//
//  为什么用普通函数指针而不是 std::function：
//    注册表是静态常量数组，函数指针可以常量初始化，避免静态初始化顺序问题。
//
//  回显约定（见 docs/gallery.md）：
//    * 由 App 每 ~100ms 或值变化时调用一次，**不是每帧**；
//    * 只写 buf 的前 n-1 字节（含结尾 0），一行纯文本，不要换行；
//    * 只能读取自己 build() 时创建的控件（指针在卡片存活期间有效）。
// ============================================================================
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "uikit/UiKit.h"

namespace gallery {

// 演示区内容 + 回显
struct Demo {
    // 会被放进卡片内容区（宽度自适应）。可以为 nullptr（纯说明卡片）。
    std::unique_ptr<skiagui::uikit::Widget> view;
    // 在 view 已经挂到树上之后调用一次（此时 view->tree() != nullptr）。
    // Dialog / Toast / Popup / Tooltip / ContextMenu / Menu 这些浮层控件**必须**
    // 在这里用 view->addOverlayChild(...) 挂上去，之后才能 show()/open()。
    std::function<void(skiagui::uikit::Widget* view)> attached;
    // 回显：把当前状态写进 buf。为空表示该组件没有可回显的状态。
    std::function<void(char* buf, std::size_t n)> echo;
};

// 一张卡片的静态描述
struct CardSpec {
    const char* id;        // 唯一 id，形如 "basic.text"
    const char* category;  // 分类 id，见 category:: 常量
    const char* name;      // 显示名（卡片标题）
    const char* summary;   // 一句说明（卡片副标题）
    Demo (*build)();       // 构建演示内容
};

// 分类 id（与 content/*.cpp 里的 spec 必须一致）
namespace category {
constexpr const char* kBasic = "basic";
constexpr const char* kButton = "button";
constexpr const char* kContainer = "container";
constexpr const char* kInput = "input";
constexpr const char* kSelection = "selection";
constexpr const char* kData = "data";
constexpr const char* kNavigation = "navigation";
constexpr const char* kOverlay = "overlay";
constexpr const char* kFeedback = "feedback";
constexpr const char* kGraphics = "graphics";
}  // namespace category

// 每个分类一个 .cpp，返回**静态**数组（长度写进 *count）
namespace content {
const CardSpec* BasicCards(int* count);
const CardSpec* ButtonCards(int* count);
const CardSpec* ContainerCards(int* count);
const CardSpec* InputCards(int* count);
const CardSpec* SelectionCards(int* count);
const CardSpec* DataCards(int* count);
const CardSpec* NavigationCards(int* count);
const CardSpec* OverlayCards(int* count);
const CardSpec* FeedbackCards(int* count);
const CardSpec* GraphicsCards(int* count);
}  // namespace content

}  // namespace gallery
