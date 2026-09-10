// ============================================================================
//  gallery/content/OverlayContent.cpp — Overlay 分类的卡片内容
// ----------------------------------------------------------------------------
//  浮层控件不是普通子节点：它们必须挂到 WidgetTree 的覆盖层上（永远画在最上层、
//  命中测试优先、不参与父容器布局）。覆盖层只有 view 挂到树上之后才拿得到，
//  所以每张卡片都是「Row + 触发按钮」，真正的浮层对象在 Demo::attached 里用
//  view->addOverlayChild(...) 创建，再由按钮的 onClick 打开。
//
//  ★ 一律不开 setAutoRemove(true)：关闭只是 visible = false，对象仍在，按钮可以
//    反复打开同一个浮层；回显里持有的裸指针也不会悬垂（见 Overlay.h 关闭语义）。
//  ★ 覆盖层控件在 attached 里才创建，回显要用它就必须跨回调传递指针：这里统一用
//    std::make_shared<T*>(nullptr) 当"槽位"（build 时分配一次，echo 只读不解引用
//    空指针，也不做任何分配）。
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

// Theme::Tone -> 名字（回显用，返回静态字符串，不分配）
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

// ---------------------------------------------------------------------------
//  overlay.tooltip —— 气泡提示
//  不拦鼠标（hitTransparent），点击按钮显示/收起，气泡跟着按钮的 bounds 定位。
// ---------------------------------------------------------------------------
Demo BuildTooltipCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto btn = std::make_unique<Button>("显示气泡提示");
    btn->setVariant(ButtonVariant::Outline);
    btn->setId("overlay.tooltip.trigger");
    Button* trigger = btn.get();
    view->addChild(std::move(btn));

    auto hint = std::make_unique<Text>("再点一次收起；气泡本身不参与布局");
    hint->setId("overlay.tooltip.hint");
    hint->setFontSize(12.0f);
    view->addChild(std::move(hint));

    auto slot = std::make_shared<Tooltip*>(nullptr);

    Demo d;
    d.view = std::move(view);
    d.attached = [trigger, slot](Widget* v) {
        auto tip = std::make_unique<Tooltip>();
        tip->setId("overlay.tooltip.overlay");
        tip->setDelay(0.0f);  // 演示里不等悬停延迟
        tip->setPlacement(Tooltip::Placement::Bottom);
        tip->setMaxWidth(260.0f);
        Tooltip* raw = static_cast<Tooltip*>(v->addOverlayChild(std::move(tip)));
        *slot = raw;
        trigger->setOnClick([trigger, raw] {
            if (raw->isShowing()) {
                raw->hide();
            } else {
                // anchor 用按钮的绝对 bounds（根坐标系），浮层自己算避让方向
                raw->show(trigger->bounds(), "浮层挂在覆盖层上，不参与父容器布局");
            }
        });
    };
    d.echo = [slot](char* buf, std::size_t n) {
        const Tooltip* tip = *slot;
        snprintf(buf, n, "showing=%d", (tip && tip->isShowing()) ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  overlay.popup —— 弹出层
//  内容只在 attached 里 setContent 一次，之后每次打开只换锚点（openAt 传 nullptr
//  内容表示复用已有内容），所以反复开合不会反复重建子节点。
// ---------------------------------------------------------------------------
std::unique_ptr<Widget> MakePopupBody() {
    auto panel = std::make_unique<Panel>("弹出层内容");
    panel->setId("overlay.popup.content");
    panel->setPadding(EdgeInsets::Uniform(10.0f));
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);
    auto l1 = std::make_unique<Text>("Popup 是通用弹出面板：内容由调用方提供。");
    l1->setId("overlay.popup.content.line1");
    col->addChild(std::move(l1));
    auto l2 = std::make_unique<Text>("点面板外面或按 Esc 关闭，面板会避开屏幕边缘。");
    l2->setId("overlay.popup.content.line2");
    l2->setFontSize(12.0f);
    col->addChild(std::move(l2));
    panel->addChild(std::move(col));
    return panel;
}

Demo BuildPopupCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto btn = std::make_unique<Button>("打开弹出层");
    btn->setVariant(ButtonVariant::Outline);
    btn->setId("overlay.popup.trigger");
    Button* trigger = btn.get();
    view->addChild(std::move(btn));

    auto hint = std::make_unique<Text>("弹出面板会自动避让屏幕边界");
    hint->setId("overlay.popup.hint");
    hint->setFontSize(12.0f);
    view->addChild(std::move(hint));

    auto slot = std::make_shared<Popup*>(nullptr);

    Demo d;
    d.view = std::move(view);
    d.attached = [trigger, slot](Widget* v) {
        auto popup = std::make_unique<Popup>();
        popup->setId("overlay.popup.overlay");
        popup->setMinWidth(220.0f);
        popup->setMaxHeight(260.0f);
        popup->setDismissOnOutsideClick(true);
        popup->setDismissOnEsc(true);
        popup->setContent(MakePopupBody());
        Popup* raw = static_cast<Popup*>(v->addOverlayChild(std::move(popup)));
        *slot = raw;
        trigger->setOnClick([trigger, raw] {
            if (raw->isOpen()) {
                raw->close();
                return;
            }
            // 传 nullptr 内容 = 复用 setContent 装好的内容，只更新锚点
            raw->openAt(trigger->bounds(), nullptr);
        });
    };
    d.echo = [slot](char* buf, std::size_t n) {
        const Popup* popup = *slot;
        snprintf(buf, n, "open=%d", (popup && popup->isOpen()) ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  overlay.dialog —— 对话框
//  动作按钮由 addAction 创建，回调返回后 Dialog 会自己 close()，所以回调里
//  只需要写业务逻辑；关闭次数用共享计数器统计（onClose 回调里 ++）。
// ---------------------------------------------------------------------------
Demo BuildDialogCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto btn = std::make_unique<Button>("打开对话框");
    btn->setVariant(ButtonVariant::Primary);
    btn->setId("overlay.dialog.trigger");
    Button* trigger = btn.get();
    view->addChild(std::move(btn));

    auto hint = std::make_unique<Text>("Esc / 点遮罩 / 点按钮都能关闭");
    hint->setId("overlay.dialog.hint");
    hint->setFontSize(12.0f);
    view->addChild(std::move(hint));

    auto slot = std::make_shared<Dialog*>(nullptr);
    auto closes = std::make_shared<int>(0);

    Demo d;
    d.view = std::move(view);
    d.attached = [trigger, slot, closes](Widget* v) {
        auto dlg = std::make_unique<Dialog>();
        dlg->setId("overlay.dialog.overlay");
        dlg->setTitle("确认删除");
        dlg->setWidth(420.0f);
        dlg->setDismissOnScrimClick(true);
        dlg->setDismissOnEsc(true);
        auto body = std::make_unique<Column>();
        body->setGap(8.0f);
        auto msg = std::make_unique<Text>("删除后不可恢复，确定要继续吗？");
        msg->setId("overlay.dialog.message");
        body->addChild(std::move(msg));
        dlg->setContent(std::move(body));
        // 回调体留空：addAction 内部会先跑回调再 close()（见 Overlay.cpp）
        dlg->addAction("取消", [] {}, false);
        dlg->addAction("确定", [] {}, true);
        Dialog* raw = static_cast<Dialog*>(v->addOverlayChild(std::move(dlg)));
        raw->setOnClose([closes] { ++*closes; });
        *slot = raw;
        trigger->setOnClick([raw] { raw->show(); });
    };
    d.echo = [slot, closes](char* buf, std::size_t n) {
        const Dialog* dlg = *slot;
        snprintf(buf, n, "open=%d closes=%d", (dlg && dlg->isOpen()) ? 1 : 0, *closes);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  overlay.modal —— 模态框
//  Modal 是 Dialog 的"遮罩不可随手关"版本，dismissible 决定点遮罩 / Esc 是否生效，
//  这里用一个 Switch 实时切换它，回显直接读 Modal::dismissible()。
// ---------------------------------------------------------------------------
Demo BuildModalCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto btn = std::make_unique<Button>("打开模态框");
    btn->setVariant(ButtonVariant::Primary);
    btn->setId("overlay.modal.trigger");
    Button* trigger = btn.get();
    view->addChild(std::move(btn));

    auto sw = std::make_unique<Switch>(true);
    sw->setId("overlay.modal.dismissible");
    sw->setLabel("点遮罩可关闭");
    Switch* toggle = sw.get();
    view->addChild(std::move(sw));

    auto slot = std::make_shared<Modal*>(nullptr);

    Demo d;
    d.view = std::move(view);
    d.attached = [trigger, toggle, slot](Widget* v) {
        auto modal = std::make_unique<Modal>();
        modal->setId("overlay.modal.overlay");
        modal->setTitle("提交审批");
        modal->setWidth(420.0f);
        modal->setDismissible(toggle->checked());
        auto body = std::make_unique<Column>();
        body->setGap(8.0f);
        auto msg = std::make_unique<Text>("提交后进入审批流程，期间不可修改。");
        msg->setId("overlay.modal.message");
        body->addChild(std::move(msg));
        modal->setContent(std::move(body));
        modal->addAction("稍后再说", [] {}, false);
        modal->addAction("立即提交", [] {}, true);
        Modal* raw = static_cast<Modal*>(v->addOverlayChild(std::move(modal)));
        *slot = raw;
        toggle->setOnChange([raw](bool on) { raw->setDismissible(on); });
        trigger->setOnClick([raw] { raw->show(); });
    };
    d.echo = [slot](char* buf, std::size_t n) {
        const Modal* modal = *slot;
        snprintf(buf, n, "open=%d dismissible=%d",
                 (modal && modal->isOpen()) ? 1 : 0,
                 (modal && modal->dismissible()) ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  overlay.toast —— 轻提示
//  Toast 默认 hitTransparent，不挡底下界面的鼠标；多条会自己堆叠。
//  tone() 是真实 getter，回显直接打印当前语气；shows 统计弹出次数。
// ---------------------------------------------------------------------------
Demo BuildToastCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto ok = std::make_unique<Button>("弹出成功提示");
    ok->setVariant(ButtonVariant::Outline);
    ok->setId("overlay.toast.trigger-success");
    Button* success = ok.get();
    view->addChild(std::move(ok));

    auto bad = std::make_unique<Button>("弹出错误提示");
    bad->setVariant(ButtonVariant::Outline);
    bad->setId("overlay.toast.trigger-danger");
    Button* danger = bad.get();
    view->addChild(std::move(bad));

    auto slot = std::make_shared<Toast*>(nullptr);
    auto shows = std::make_shared<int>(0);

    Demo d;
    d.view = std::move(view);
    d.attached = [success, danger, slot, shows](Widget* v) {
        auto toast = std::make_unique<Toast>();
        toast->setId("overlay.toast.overlay");
        toast->setAnchor(Toast::Anchor::TopRight);
        toast->setWidth(260.0f);
        toast->setMaxWidth(320.0f);
        toast->setDuration(2.5f);
        toast->setShowIcon(true);
        Toast* raw = static_cast<Toast*>(v->addOverlayChild(std::move(toast)));
        *slot = raw;
        success->setOnClick([raw, shows] {
            raw->show("设置已保存", Theme::Tone::Success, 2.5f);
            ++*shows;
        });
        danger->setOnClick([raw, shows] {
            raw->show("网络请求失败，请重试", Theme::Tone::Danger, 3.0f);
            ++*shows;
        });
    };
    d.echo = [slot, shows](char* buf, std::size_t n) {
        const Toast* toast = *slot;
        snprintf(buf, n, "active=%d tone=%s shows=%d",
                 (toast && toast->isActive()) ? 1 : 0,
                 toast ? ToneName(toast->tone()) : "None", *shows);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  overlay.contextmenu —— 右键菜单
//  条目不是子控件，而是 ContextMenu 内部数据（分隔符 / 禁用态 / 快捷键统一排版）；
//  点击条目会先 close() 再回调，所以回调里只累加计数。
// ---------------------------------------------------------------------------
Demo BuildContextMenuCard() {
    auto view = std::make_unique<Row>();
    view->setGap(12.0f);
    view->setAlign(layout::Align::Center);

    auto btn = std::make_unique<Button>("打开右键菜单");
    btn->setVariant(ButtonVariant::Outline);
    btn->setId("overlay.contextmenu.trigger");
    Button* trigger = btn.get();
    view->addChild(std::move(btn));

    auto hint = std::make_unique<Text>("菜单从按钮左下角弹出，支持上下键 + 回车");
    hint->setId("overlay.contextmenu.hint");
    hint->setFontSize(12.0f);
    view->addChild(std::move(hint));

    auto slot = std::make_shared<ContextMenu*>(nullptr);
    auto picks = std::make_shared<int>(0);

    Demo d;
    d.view = std::move(view);
    d.attached = [trigger, slot, picks](Widget* v) {
        auto menu = std::make_unique<ContextMenu>();
        menu->setId("overlay.contextmenu.overlay");
        menu->setMinWidth(180.0f);
        menu->addItem("复制", Glyph::Copy, [picks] { ++*picks; });
        menu->addItem("粘贴", Glyph::Copy, [picks] { ++*picks; });
        menu->addItem("重命名", Glyph::Edit, [picks] { ++*picks; });
        menu->addSeparator();
        menu->addItem("导出为 CSV", Glyph::Download, false, [picks] { ++*picks; });
        menu->addItem("删除", Glyph::Trash, [picks] { ++*picks; });
        ContextMenu* raw = static_cast<ContextMenu*>(v->addOverlayChild(std::move(menu)));
        *slot = raw;
        trigger->setOnClick([trigger, raw] {
            const Rect b = trigger->bounds();  // 根坐标系
            if (raw->isOpen()) {
                raw->close();
            } else {
                raw->openAt(Point{b.left(), b.bottom()});
            }
        });
    };
    d.echo = [slot, picks](char* buf, std::size_t n) {
        const ContextMenu* menu = *slot;
        snprintf(buf, n, "open=%d picks=%d", (menu && menu->isOpen()) ? 1 : 0, *picks);
    };
    return d;
}

}  // namespace

static const CardSpec kCards[] = {
    {"overlay.tooltip", category::kOverlay, "气泡提示",
     "点击按钮在按钮下方弹出气泡：Tooltip 不拦鼠标，延迟为 0 时立即显示，再点一次收起。",
     &BuildTooltipCard},
    {"overlay.popup", category::kOverlay, "弹出层",
     "内容只装一次、每次打开只换锚点，面板放不下会自动翻到锚点上方并避开屏幕边缘。",
     &BuildPopupCard},
    {"overlay.dialog", category::kOverlay, "对话框",
     "动作按钮由 addAction 创建，回调返回后 Dialog 自己关闭；Esc、点遮罩同样计入关闭次数。",
     &BuildDialogCard},
    {"overlay.modal", category::kOverlay, "模态框",
     "Modal 比 Dialog 多一个 dismissible 开关：关掉后点遮罩和 Esc 都无效，必须点按钮。",
     &BuildModalCard},
    {"overlay.toast", category::kOverlay, "轻提示",
     "Toast 默认不吃鼠标且会按存活数量自动堆叠，两种语气共用一个对象、可反复弹出。",
     &BuildToastCard},
    {"overlay.contextmenu", category::kOverlay, "右键菜单",
     "菜单条目是内部数据结构，支持图标、分隔符与禁用项；点击条目先关闭再触发回调。",
     &BuildContextMenuCard},
};

const CardSpec* OverlayCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
