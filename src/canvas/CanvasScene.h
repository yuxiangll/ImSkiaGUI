// ============================================================================
//  CanvasScene.h — 注入后用 Canvas2D 画的演示场景
// ----------------------------------------------------------------------------
//  这个类只依赖 canvas::Context2D，不碰钩子/GPU/输入线程 —— 好处是它既能被
//  注入式 overlay（skiagui_canvas.dll）调用，也能被离屏自测（canvas_selftest）
//  调用，用来验证"移植过来的 Canvas2D API 能不能画出真实界面"。
//
//  场景里刻意覆盖了移植的每一个能力点：
//    圆角矩形+阴影 / 线性+锥形渐变 / 图案平铺 / CSS 滤镜 / 混合模式 /
//    裁剪 / 文本对齐+基线+字距+装饰 / 文本轮廓 / 虚线描边 / 变换 /
//    鼠标交互高亮 / 性能统计。
// ============================================================================
#pragma once

#include <string>

#include "canvas/Context2D.h"
#include "canvas/Image.h"

namespace skiagui {
namespace canvas {

// 每帧传给场景的运行时信息
struct SceneContext {
    float time = 0.0f;      // 累计秒数
    float dt = 0.0f;        // 本帧耗时
    float fps = 0.0f;
    float width = 0.0f;     // 后备缓冲尺寸（像素）
    float height = 0.0f;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool mouseValid = false;
    bool mouseDown = false;
    const char* backend = "?";
    unsigned long long framesDrawn = 0;
    unsigned long long framesSkipped = 0;
};

class CanvasScene {
public:
    CanvasScene();

    // 面板是否显示（INSERT 切换）
    bool visible() const { return visible_; }
    void SetVisible(bool v) { visible_ = v; }
    void Toggle() { visible_ = !visible_; }

    // 鼠标是否落在面板上（决定是否吞掉宿主消息）
    bool WantsMouse() const { return hoverPanel_; }

    // 绘制一帧。ctx 的尺寸必须已经等于 width/height。
    void Draw(Context2D& ctx, const SceneContext& scene);

    // 面板区域（屏幕像素），供命中测试
    SkRect panelRect() const { return panel_; }

private:
    void DrawBackground(Context2D& ctx, const SceneContext& s);
    void DrawHeader(Context2D& ctx, const SceneContext& s);
    void DrawSwatches(Context2D& ctx, const SceneContext& s);
    void DrawTypography(Context2D& ctx, const SceneContext& s);
    void DrawEffects(Context2D& ctx, const SceneContext& s);
    void DrawStats(Context2D& ctx, const SceneContext& s);
    void DrawHud(Context2D& ctx, const SceneContext& s);

    bool visible_ = true;
    bool hoverPanel_ = false;
    float scale_ = 1.0f;      // UI 缩放（1080p 为基准）
    SkRect panel_ = SkRect::MakeEmpty();
    Image logo_;              // 离屏生成的小图标（演示 drawImage / 图案）
    bool logoReady_ = false;
};

}  // namespace canvas
}  // namespace skiagui
