// ============================================================================
//  Icon.h — 矢量图标库（文档 §十四 Graphics / Icon）
// ----------------------------------------------------------------------------
//  为什么不用字体图标 / SVG？
//    * 本 SDK 没有 SkSVGDOM（见 AGENTS.md §7），也不能加载外部字体文件；
//    * 路径图标可以任意缩放、任意着色、任意线宽，且不占字体槽位。
//
//  所有图标都定义在 **24x24** 的归一化坐标系里，用描边绘制（stroke），
//  Draw() 会按目标矩形缩放。路径按 Glyph 缓存在静态表里，只构建一次。
// ============================================================================
#pragma once

#include <string>

#include "include/core/SkPath.h"

#include "uikit/PaintContext.h"
#include "uikit/UiTypes.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

enum class Glyph : int {
    None = 0,
    // 基础
    Check, Close, Plus, Minus, Dot, Circle, Square, Triangle,
    // 方向
    ChevronUp, ChevronDown, ChevronLeft, ChevronRight,
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight, ArrowUpRight, ArrowDownRight,
    // 操作
    Search, Settings, Trash, Edit, Copy, Save, Download, Upload, Refresh,
    Undo, Redo, Filter, Sort, Expand, Collapse, Grip, Fullscreen, ZoomIn, ZoomOut,
    // 状态
    Info, Warning, Error, Success, Question, Spinner, Clock, Bell,
    // 对象
    User, Users, Home, Folder, File, FolderOpen, Image, Mail, Link, Tag, Bookmark, Pin,
    // 安全
    Lock, Unlock, Key, Shield, Eye, EyeOff,
    // 媒体
    Play, Pause, Stop, Volume, Camera,
    // 布局 / 数据
    Menu, MoreH, MoreV, Grid, List, Table, Layers, Box, Database, Terminal, Code, Bug,
    Chart, LineChart, BarChart, PieChart, Activity, TrendingUp, TrendingDown, Cpu, Globe,
    // 主题
    Sun, Moon, Star, Heart, Power, Lightning, Flame, Calendar,
};

namespace icons {

// 取图标的 24x24 路径（首次调用构建，之后返回缓存副本）
const SkPath& Get(Glyph g);

// 在 box 里绘制图标（等比缩放，居中）
void Draw(PaintContext& ctx, Glyph g, const Rect& box, SkColor color, float strokeWidth = 0.0f);

// 名称 <-> 枚举（做图标选择器 / 调试用）
const char* NameOf(Glyph g);
Glyph FromName(const std::string& name);

}  // namespace icons

// ---------------------------------------------------------------------------
//  IconWidget —— 一个只画图标的叶子控件
// ---------------------------------------------------------------------------
class IconWidget : public Widget {
public:
    explicit IconWidget(Glyph g = Glyph::None, float size = 0.0f) : glyph_(g), size_(size) {
        id_ = "icon";
    }

    Glyph glyph() const { return glyph_; }
    void setGlyph(Glyph g) { glyph_ = g; }
    void setColor(SkColor c) { color_ = c; hasColor_ = true; }
    void setSize(float s) { size_ = s; }
    void setStrokeWidth(float w) { strokeWidth_ = w; }

    Size onMeasure(Size available) override {
        const float s = size_ > 0.0f ? size_ : theme().iconSize;
        measuredSize_ = Size{s, s};
        (void)available;
        return measuredSize_;
    }

    void onPaint(PaintContext& ctx) override {
        const SkColor c = hasColor_ ? color_ : (state_.enabled ? theme().text : theme().textDisabled);
        icons::Draw(ctx, glyph_, localRect(), c, strokeWidth_);
    }

private:
    Glyph glyph_ = Glyph::None;
    float size_ = 0.0f;
    float strokeWidth_ = 0.0f;
    SkColor color_ = SK_ColorWHITE;
    bool hasColor_ = false;
};

}  // namespace uikit
}  // namespace skiagui
