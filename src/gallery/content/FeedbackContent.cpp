// ============================================================================
//  gallery/content/FeedbackContent.cpp — Feedback 分类的卡片内容
// ----------------------------------------------------------------------------
//  状态反馈类控件的特点：要么"会动"（indeterminate / spinner），要么"会被用户
//  改掉状态"（Alert 被关闭）。所以卡片一律把主控件的关键状态绑到一个可交互的
//  从属控件上（Slider / Switch / RadioGroup），回显只读**真实 getter**：
//      ProgressBar::normalized()      Switch::checked()
//      CircularProgress::value()      LoadingSpinner::angle()
//      Alert::dismissed()             RadioGroup::selected()
//  注意 ProgressBar / CircularProgress 只有 setIndeterminate 没有 getter，
//  所以 indet 字段读的是驱动它的那个 Switch 的 checked()（二者在回调里同步）。
// ============================================================================
#include <cstddef>
#include <cstdio>
#include <memory>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

namespace gallery {
namespace content {
namespace {

using namespace skiagui::uikit;

// 回显用的语气名（静态字符串，无分配）
const char* ToneName(Theme::Tone t) {
    switch (t) {
        case Theme::Tone::Neutral: return "Neutral";
        case Theme::Tone::Accent: return "Accent";
        case Theme::Tone::Success: return "Success";
        case Theme::Tone::Warning: return "Warning";
        case Theme::Tone::Danger: return "Danger";
        case Theme::Tone::Info: return "Info";
    }
    return "Unknown";
}

// RadioGroup 的业务值 -> Tone（0/1/2 分别对应 Info/Warning/Danger）
Theme::Tone ToneFromValue(int v) {
    switch (v) {
        case 0: return Theme::Tone::Info;
        case 1: return Theme::Tone::Warning;
        case 2: return Theme::Tone::Danger;
        default: break;
    }
    return Theme::Tone::Neutral;
}

// ---------------------------------------------------------------------------
//  feedback.progressbar —— 线性进度条
//  Slider 直接驱动 setValue，Switch 驱动 setIndeterminate（此时 value 被忽略，
//  进度条改成来回移动的滑块）。
// ---------------------------------------------------------------------------
Demo BuildProgressBarCard() {
    auto col = std::make_unique<Column>();
    col->setGap(10.0f);

    auto pb = std::make_unique<ProgressBar>();
    pb->setId("feedback.progressbar.bar");
    pb->setValue(0.65f);
    pb->setShowLabel(true);
    pb->setTone(Theme::Tone::Accent);
    pb->setThickness(10.0f);
    ProgressBar* bar = pb.get();
    col->addChild(std::move(pb));

    auto row = std::make_unique<Row>();
    row->setGap(14.0f);
    row->setAlign(layout::Align::Center);

    auto slider = std::make_unique<Slider>(0.0f, 1.0f, 0.65f);
    slider->setId("feedback.progressbar.slider");
    slider->setLabel("进度");
    slider->setShowValue(true);
    slider->setStep(0.05f);
    slider->setOnChange([bar](float v) { bar->setValue(v); });
    slider->grow(1.0f);
    row->addChild(std::move(slider));

    auto sw = std::make_unique<Switch>(false);
    sw->setId("feedback.progressbar.indeterminate");
    sw->setLabel("不确定态");
    Switch* indeterminate = sw.get();
    indeterminate->setOnChange([bar](bool on) { bar->setIndeterminate(on); });
    row->addChild(std::move(sw));

    col->addChild(std::move(row));

    Demo d;
    d.view = std::move(col);
    d.echo = [bar, indeterminate](char* buf, std::size_t n) {
        snprintf(buf, n, "value=%.2f indet=%d", bar->normalized(),
                 indeterminate->checked() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  feedback.circularprogress —— 环形进度
//  和线性进度条同一套驱动方式：Slider 改值、Switch 切不确定态。
// ---------------------------------------------------------------------------
Demo BuildCircularProgressCard() {
    auto row = std::make_unique<Row>();
    row->setGap(20.0f);
    row->setAlign(layout::Align::Center);

    auto ring = std::make_unique<CircularProgress>();
    ring->setId("feedback.circularprogress.ring");
    ring->setValue(0.30f);
    ring->setSize(72.0f);
    ring->setThickness(7.0f);
    ring->setShowLabel(true);
    ring->setTone(Theme::Tone::Accent);
    CircularProgress* circle = ring.get();
    row->addChild(std::move(ring));

    auto col = std::make_unique<Column>();
    col->setGap(10.0f);
    col->grow(1.0f);

    auto slider = std::make_unique<Slider>(0.0f, 1.0f, 0.30f);
    slider->setId("feedback.circularprogress.slider");
    slider->setLabel("进度");
    slider->setShowValue(true);
    slider->setStep(0.05f);
    slider->setOnChange([circle](float v) { circle->setValue(v); });
    col->addChild(std::move(slider));

    auto sw = std::make_unique<Switch>(false);
    sw->setId("feedback.circularprogress.indeterminate");
    sw->setLabel("不确定态（无限旋转）");
    Switch* indeterminate = sw.get();
    indeterminate->setOnChange([circle](bool on) { circle->setIndeterminate(on); });
    col->addChild(std::move(sw));

    row->addChild(std::move(col));

    Demo d;
    d.view = std::move(row);
    d.echo = [circle, indeterminate](char* buf, std::size_t n) {
        snprintf(buf, n, "value=%.2f indet=%d", circle->value(),
                 indeterminate->checked() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  feedback.loadingspinner —— 加载动画
//  12 根渐隐刻度，angle 每帧由 WidgetTree 的 onTick 推进，回显读的就是当前角度。
// ---------------------------------------------------------------------------
Demo BuildLoadingSpinnerCard() {
    auto row = std::make_unique<Row>();
    row->setGap(20.0f);
    row->setAlign(layout::Align::Center);

    auto sp = std::make_unique<LoadingSpinner>();
    sp->setId("feedback.loadingspinner.spinner");
    sp->setSize(30.0f);
    sp->setTone(Theme::Tone::Accent);
    sp->setSpeed(0.9f);
    LoadingSpinner* spinner = sp.get();
    row->addChild(std::move(sp));

    auto sp2 = std::make_unique<LoadingSpinner>();
    sp2->setId("feedback.loadingspinner.spinner-fine");
    sp2->setSize(20.0f);
    sp2->setSegments(8);
    sp2->setStrokeWidth(2.0f);
    sp2->setTone(Theme::Tone::Success);
    row->addChild(std::move(sp2));

    auto hint = std::make_unique<Text>("刻度数 12 / 8，角度由 onTick 每帧推进");
    hint->setId("feedback.loadingspinner.hint");
    hint->setFontSize(12.0f);
    row->addChild(std::move(hint));

    Demo d;
    d.view = std::move(row);
    d.echo = [spinner](char* buf, std::size_t n) {
        snprintf(buf, n, "angle=%.0f", spinner->angle());
    };
    return d;
}

// ---------------------------------------------------------------------------
//  feedback.alert —— 警告条
//  点右上角 × 会 dismissed=true 并隐藏自己（隐藏不等于销毁），"重置"把它放回来；
//  语气由 RadioGroup 切换（Alert 只暴露 dismissed()，没有 tone() getter，
//  所以语气名从 RadioGroup::selected() 映射得到）。
// ---------------------------------------------------------------------------
Demo BuildAlertCard() {
    auto col = std::make_unique<Column>();
    col->setGap(12.0f);

    auto alert = std::make_unique<Alert>();
    alert->setId("feedback.alert.banner");
    alert->setTitle("磁盘空间不足");
    alert->setMessage("剩余 3% 空间，请及时清理缓存目录后重试。");
    alert->setTone(Theme::Tone::Warning);
    alert->setIcon(Glyph::Warning);
    alert->setClosable(true);
    alert->setHideOnClose(true);
    Alert* banner = alert.get();
    col->addChild(std::move(alert));

    auto row = std::make_unique<Row>();
    row->setGap(16.0f);
    row->setAlign(layout::Align::Center);

    auto tones = std::make_unique<RadioGroup>();
    tones->setId("feedback.alert.tone");
    tones->setVertical(false);
    tones->addOption("信息", 0);
    tones->addOption("警告", 1);
    tones->addOption("危险", 2);
    tones->setSelected(1);
    RadioGroup* toneGroup = tones.get();
    toneGroup->setOnChange([banner](int v) { banner->setTone(ToneFromValue(v)); });
    row->addChild(std::move(tones));

    auto reset = std::make_unique<Button>("重置提示条");
    reset->setVariant(ButtonVariant::Outline);
    reset->setId("feedback.alert.reset");
    reset->setOnClick([banner] {
        banner->resetDismissed();
        banner->setVisible(true);
    });
    row->addChild(std::move(reset));

    col->addChild(std::move(row));

    Demo d;
    d.view = std::move(col);
    d.echo = [banner, toneGroup](char* buf, std::size_t n) {
        snprintf(buf, n, "dismissed=%d tone=%s", banner->dismissed() ? 1 : 0,
                 ToneName(ToneFromValue(toneGroup->selected())));
    };
    return d;
}

}  // namespace

static const CardSpec kCards[] = {
    {"feedback.progressbar", category::kFeedback, "进度条",
     "滑块改进度、开关切不确定态；不确定态下 value 被忽略，改成来回移动的滑块。",
     &BuildProgressBarCard},
    {"feedback.circularprogress", category::kFeedback, "环形进度",
     "环形进度可当仪表用：中心显示百分比，不确定态时按固定转速无限旋转。",
     &BuildCircularProgressCard},
    {"feedback.loadingspinner", category::kFeedback, "加载动画",
     "12 根渐隐刻度按固定转速旋转，刻度数可调；回显打印的是当前旋转角而非时间。",
     &BuildLoadingSpinnerCard},
    {"feedback.alert", category::kFeedback, "警告条",
     "点右上角 × 只是隐藏（dismissed 变真），重置按钮能把它放回来，语气可实时切换。",
     &BuildAlertCard},
};

const CardSpec* FeedbackCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
