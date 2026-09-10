// ============================================================================
//  gallery/Registry.h — 组件注册表（元数据集中表）
// ----------------------------------------------------------------------------
//  设计决策（docs/gallery.md §架构）：
//    * 元数据放在画廊侧，**不**在 uikit 里加注册宏 —— 公共库不该为了一个
//      画廊引入自注册（静态初始化顺序、链接器丢对象都是新坑）；
//    * 集中表让"漏了哪个组件"一眼可见，并能在 uikit_selftest 里断言
//      "每个条目都能 build 出非空树 + id 唯一"。
//
//  Registry 是值类型（不是全局单例）：由 gallery::App 持有，随 App 销毁，
//  两个宿主 / 多个实例互不干扰（见 Q29）。
// ============================================================================
#pragma once

#include <vector>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

namespace gallery {

struct CategorySpec {
    const char* id;                  // 与 CardSpec::category 对应
    const char* name;                // 导航里显示的中文名
    skiagui::uikit::Glyph glyph;     // 导航图标
};

class Registry {
public:
    // 幂等：从 content::*Cards() 汇总。重复调用不会重复追加。
    void Build();
    bool built() const { return built_; }

    const std::vector<CategorySpec>& categories() const { return categories_; }
    const std::vector<CardSpec>& cards() const { return cards_; }
    int cardCount() const { return static_cast<int>(cards_.size()); }
    int categoryCount() const { return static_cast<int>(categories_.size()); }

    // 某个分类下的卡片数（导航徽章用）
    int countIn(const char* categoryId) const;
    // 分类 id -> 下标，找不到返回 -1
    int categoryIndex(const char* categoryId) const;

private:
    bool built_ = false;
    std::vector<CategorySpec> categories_;
    std::vector<CardSpec> cards_;
};

}  // namespace gallery
