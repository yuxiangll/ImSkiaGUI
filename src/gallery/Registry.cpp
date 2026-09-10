// ============================================================================
//  gallery/Registry.cpp — 注册表实现（分类顺序 + 汇总各 content/*.cpp）
// ============================================================================
#include "gallery/Registry.h"

#include <cstring>

namespace gallery {

namespace {

// 分类顺序 = 导航顺序 = 组件库的 10 个分组
const CategorySpec kCategories[] = {
    {category::kBasic, "基础", skiagui::uikit::Glyph::File},
    {category::kButton, "按钮", skiagui::uikit::Glyph::Box},
    {category::kContainer, "容器", skiagui::uikit::Glyph::Grid},
    {category::kInput, "输入", skiagui::uikit::Glyph::Edit},
    {category::kSelection, "选择", skiagui::uikit::Glyph::Check},
    {category::kData, "数据", skiagui::uikit::Glyph::Table},
    {category::kNavigation, "导航", skiagui::uikit::Glyph::Menu},
    {category::kOverlay, "浮层", skiagui::uikit::Glyph::Layers},
    {category::kFeedback, "反馈", skiagui::uikit::Glyph::Activity},
    {category::kGraphics, "图形", skiagui::uikit::Glyph::Chart},
};

// 每个分类的 content 函数（顺序与 kCategories 一致）
const CardSpec* (*const kProviders[])(int*) = {
    &content::BasicCards,     &content::ButtonCards,   &content::ContainerCards,
    &content::InputCards,     &content::SelectionCards, &content::DataCards,
    &content::NavigationCards, &content::OverlayCards, &content::FeedbackCards,
    &content::GraphicsCards,
};

}  // namespace

void Registry::Build() {
    if (built_) return;
    built_ = true;

    categories_.assign(kCategories, kCategories + sizeof(kCategories) / sizeof(kCategories[0]));

    const int providerCount = static_cast<int>(sizeof(kProviders) / sizeof(kProviders[0]));
    for (int i = 0; i < providerCount; ++i) {
        int n = 0;
        const CardSpec* list = kProviders[i](&n);
        for (int k = 0; k < n; ++k) {
            cards_.push_back(list[k]);
        }
    }
}

int Registry::countIn(const char* categoryId) const {
    if (!categoryId) return 0;
    int n = 0;
    for (const CardSpec& c : cards_) {
        if (c.category && std::strcmp(c.category, categoryId) == 0) ++n;
    }
    return n;
}

int Registry::categoryIndex(const char* categoryId) const {
    if (!categoryId) return -1;
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        if (std::strcmp(categories_[i].id, categoryId) == 0) return i;
    }
    return -1;
}

}  // namespace gallery
