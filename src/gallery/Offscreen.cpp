// ============================================================================
//  gallery/Offscreen.cpp — 全展开装配实现
// ============================================================================
#include "gallery/Offscreen.h"

#include "gallery/CardBuilder.h"

namespace gallery {

OffscreenGallery BuildAllCards(const skiagui::uikit::Theme& theme) {
    using namespace skiagui::uikit;

    OffscreenGallery out;

    Registry registry;
    registry.Build();

    auto root = std::make_unique<Column>();
    root->setId("offscreen.root");
    root->setGap(18.0f);
    root->setPadding(EdgeInsets::Uniform(20.0f));

    for (const CategorySpec& cat : registry.categories()) {
        auto section = std::make_unique<Column>();
        section->setId(std::string("offscreen.section.") + cat.id);
        section->setGap(14.0f);

        auto title = std::make_unique<Text>(cat.name);
        title->setId(std::string("offscreen.sectiontitle.") + cat.id);
        title->setFontSize(20.0f);
        title->setWeight(700);
        section->addChild(std::move(title));

        for (const CardSpec& spec : registry.cards()) {
            if (!spec.category || std::string(spec.category) != cat.id) continue;
            BuiltCard built = BuildCardWidget(spec, theme);
            if (built.attachedFn && built.demoView) {
                out.attached.emplace_back(built.demoView, built.attachedFn);
            }
            out.echoFns.push_back(built.echoFn);
            section->addChild(std::move(built.card));
        }

        root->addChild(std::move(section));
    }

    out.root = std::move(root);
    return out;
}

}  // namespace gallery
