// ============================================================================
//  gallery/CardBuilder.cpp — 卡片外壳实现
// ============================================================================
#include "gallery/CardBuilder.h"

namespace gallery {

BuiltCard BuildCardWidget(const CardSpec& spec, const skiagui::uikit::Theme& theme) {
    using namespace skiagui::uikit;

    BuiltCard out;

    auto card = std::make_unique<Card>(spec.name);
    card->setId(spec.id);
    card->setPadding(EdgeInsets::Uniform(14.0f));

    auto col = std::make_unique<Column>();
    col->setGap(10.0f);

    auto summary = std::make_unique<Text>(spec.summary);
    summary->setFontSize(11.0f);
    summary->setColor(theme.textMuted);
    summary->setWrap(true);
    out.summary = summary.get();
    col->addChild(std::move(summary));

    Demo demo = spec.build ? spec.build() : Demo{};
    out.demoView = demo.view.get();
    out.echoFn = std::move(demo.echo);
    out.attachedFn = std::move(demo.attached);

    if (demo.view) {
        auto box = std::make_unique<Column>();
        box->setJustify(layout::Justify::Center);
        box->layoutParams().minHeight = kCardDemoMinHeight;
        box->addChild(std::move(demo.view));
        col->addChild(std::move(box));
    }

    auto echoText = std::make_unique<Text>("");
    echoText->setId(std::string(spec.id) + ".echo");  // 自检按这个 id 找回显行
    echoText->setMonospace(true);
    echoText->setFontSize(11.0f);
    echoText->setWrap(false);
    echoText->setColor(theme.textSecondary);
    out.echo = echoText.get();
    col->addChild(std::move(echoText));

    card->addChild(std::move(col));
    out.card = std::move(card);
    return out;
}

}  // namespace gallery
