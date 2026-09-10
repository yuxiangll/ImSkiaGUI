// ============================================================================
//  gallery/Offscreen.h — 离屏"全展开"装配（供 uikit_selftest / PNG 回归用）
// ----------------------------------------------------------------------------
//  Q18 定了两种装配模式：
//    * Interactive（gallery::App）—— 左导航 + 每分类一页 + 搜索过滤；
//    * AllCards（这里）           —— 40 张卡片全部可见、按分类分组，一次渲染完，
//                                    专供离屏断言与 PNG 导出。
//  两者共用 gallery/CardBuilder.h 的同一份卡片外壳与演示内容（Q39）。
//
//  Q62 定了断言方式：**遍历注册表逐组件渲染**，不再依赖 12 个旧分区 id。
//  所以这里额外暴露 echoFns，测试可以直接调它们验证回显不是空串。
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "gallery/Card.h"
#include "gallery/Registry.h"
#include "uikit/UiKit.h"

namespace gallery {

struct OffscreenGallery {
    std::unique_ptr<skiagui::uikit::Widget> root;  // 直接交给 WidgetTree
    // 挂树之后必须调用（浮层控件的 addOverlayChild 需要已 attach 的 view）
    std::vector<std::pair<skiagui::uikit::Widget*,
                          std::function<void(skiagui::uikit::Widget*)>>>
            attached;
    // 每张卡片的回显函数（可为空），顺序与 registry().cards() 一致
    std::vector<std::function<void(char*, std::size_t)>> echoFns;
};

// 一次构建全部卡片（按分类分组，每组前面一个分类标题）
OffscreenGallery BuildAllCards(const skiagui::uikit::Theme& theme =
                                       skiagui::uikit::Theme::Dark());

}  // namespace gallery
