// ============================================================================
//  gallery/Echo.h — 卡片回显（节流轮询 + 预分配缓冲）
// ----------------------------------------------------------------------------
//  设计决策（Q11/Q21）：
//    * 回显用**轮询 getter**，不给 76 个组件手写事件回调；
//    * 但每帧格式化字符串会堆分配，违反 overlay「每帧不分配」硬约束，
//      所以这里按 100ms 节流 + 值变化才刷新，并且只写预分配的 char[64]；
//    * 绘制路径只读 Text 控件，不做任何格式化。
// ============================================================================
#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "uikit/UiKit.h"

namespace gallery {

class Echo {
public:
    // 注册一张卡片的回显：fn 把状态写进 buf，target 是卡片里那一行等宽文本。
    void Add(std::function<void(char*, std::size_t)> fn, skiagui::uikit::Text* target);

    // 每帧调用（now = 单调时钟秒数）。内部按 kIntervalSeconds 节流。
    void Update(double nowSeconds);

    int count() const { return static_cast<int>(entries_.size()); }
    void Clear();

private:
    struct Entry {
        std::function<void(char*, std::size_t)> fn;
        skiagui::uikit::Text* target = nullptr;
        char buf[64] = {};
    };

    std::vector<Entry> entries_;
    double nextUpdate_ = 0.0;
    static constexpr double kIntervalSeconds = 0.1;
    static constexpr std::size_t kBufSize = 64;
};

}  // namespace gallery
