// ============================================================================
//  gallery/CardBuilder.h — 卡片外壳的唯一实现（App 与离屏装配共用）
// ----------------------------------------------------------------------------
//  为什么抽出来：Q18/Q39/Q62 定了"交互模式"和"离屏断言模式"必须用**同一份**
//  卡片内容，否则两份画廊会漂移、断言也证明不了屏幕上看到的东西。
//
//  卡片结构（纵向三段，Q22）：
//      Card 标题栏（组件显示名）
//        说明（11px muted，可换行）
//        演示区（最小高度 72px，内容由 CardSpec::build 提供）
//        回显行（11px 等宽，单行）
// ============================================================================
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

namespace gallery {

// 建好的一张卡片：外壳 + 需要外部接线的东西
struct BuiltCard {
    std::unique_ptr<skiagui::uikit::Widget> card;      // 外壳（Card）
    skiagui::uikit::Text* summary = nullptr;           // 说明文本（主题切换要改色）
    skiagui::uikit::Text* echo = nullptr;              // 回显文本
    skiagui::uikit::Widget* demoView = nullptr;        // 演示区根（attached 回调的入参）
    std::function<void(char*, std::size_t)> echoFn;    // 回显函数（可为空）
    std::function<void(skiagui::uikit::Widget*)> attachedFn;  // 挂树后调用（浮层用）
};

// 演示区最小高度（回显与演示挤在一起会很难看）
constexpr float kCardDemoMinHeight = 72.0f;

BuiltCard BuildCardWidget(const CardSpec& spec, const skiagui::uikit::Theme& theme);

}  // namespace gallery
