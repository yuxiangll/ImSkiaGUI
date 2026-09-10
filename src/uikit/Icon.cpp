// ============================================================================
//  Icon.cpp — 24x24 归一化路径图标
// ============================================================================
#include "uikit/Icon.h"

#include <unordered_map>

#include "include/core/SkMatrix.h"
#include "include/core/SkPathBuilder.h"

namespace skiagui {
namespace uikit {

namespace icons {

namespace {

// ---- 小工具：所有坐标都在 24x24 空间 ------------------------------------------
//  注意：本 SDK 的 SkPath 是**不可变**的（Skia m146），所有构建命令走 SkPathBuilder。
struct P {
    SkPathBuilder b;
    P& m(float x, float y) { b.moveTo(x, y); return *this; }
    P& l(float x, float y) { b.lineTo(x, y); return *this; }
    P& h(float dx) { b.rLineTo(dx, 0); return *this; }
    P& v(float dy) { b.rLineTo(0, dy); return *this; }
    P& q(float cx, float cy, float x, float y) { b.quadTo(cx, cy, x, y); return *this; }
    P& c(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        b.cubicTo(c1x, c1y, c2x, c2y, x, y);
        return *this;
    }
    P& r(float x, float y, float w, float h, float rad = 0.0f) {
        const SkRect rc = SkRect::MakeXYWH(x, y, w, h);
        if (rad > 0.0f) b.addRRect(SkRRect::MakeRectXY(rc, rad, rad));
        else b.addRect(rc);
        return *this;
    }
    P& o(float cx, float cy, float rad) { b.addCircle(cx, cy, rad); return *this; }
    // 相对弧：a(rx, ry, rot, largeArc, sweepCW, dx, dy)
    P& a(float rx, float ry, float rot, bool largeArc, bool sweepCW, float dx, float dy) {
        b.rArcTo(SkPoint::Make(rx, ry), rot,
                 largeArc ? SkPathBuilder::kLarge_ArcSize : SkPathBuilder::kSmall_ArcSize,
                 sweepCW ? SkPathDirection::kCW : SkPathDirection::kCCW, {dx, dy});
        return *this;
    }
    P& line(float x1, float y1, float x2, float y2) { m(x1, y1).l(x2, y2); return *this; }
    P& close() { b.close(); return *this; }
    // 折线
    P& poly(std::initializer_list<SkPoint> pts) {
        bool first = true;
        for (const SkPoint& pt : pts) {
            if (first) { b.moveTo(pt); first = false; }
            else b.lineTo(pt);
        }
        return *this;
    }
};

SkPath BuildPath(Glyph g) {
    P p;
    switch (g) {
        case Glyph::None: break;

        // ---------------- 基础 ----------------
        case Glyph::Check: p.poly({{4, 12.5f}, {9.5f, 18}, {20, 6.5f}}); break;
        case Glyph::Close: p.line(6, 6, 18, 18).line(18, 6, 6, 18); break;
        case Glyph::Plus: p.line(12, 5, 12, 19).line(5, 12, 19, 12); break;
        case Glyph::Minus: p.line(5, 12, 19, 12); break;
        case Glyph::Dot: p.o(12, 12, 3.5f); break;
        case Glyph::Circle: p.o(12, 12, 8.5f); break;
        case Glyph::Square: p.r(4, 4, 16, 16, 2.0f); break;
        case Glyph::Triangle: p.poly({{12, 4}, {21, 19}, {3, 19}}).close(); break;

        // ---------------- 方向 ----------------
        case Glyph::ChevronUp: p.poly({{6, 15}, {12, 9}, {18, 15}}); break;
        case Glyph::ChevronDown: p.poly({{6, 9}, {12, 15}, {18, 9}}); break;
        case Glyph::ChevronLeft: p.poly({{15, 6}, {9, 12}, {15, 18}}); break;
        case Glyph::ChevronRight: p.poly({{9, 6}, {15, 12}, {9, 18}}); break;
        case Glyph::ArrowUp: p.line(12, 19, 12, 5).poly({{6, 11}, {12, 5}, {18, 11}}); break;
        case Glyph::ArrowDown: p.line(12, 5, 12, 19).poly({{6, 13}, {12, 19}, {18, 13}}); break;
        case Glyph::ArrowLeft: p.line(19, 12, 5, 12).poly({{11, 6}, {5, 12}, {11, 18}}); break;
        case Glyph::ArrowRight: p.line(5, 12, 19, 12).poly({{13, 6}, {19, 12}, {13, 18}}); break;
        case Glyph::ArrowUpRight: p.line(7, 17, 17, 7).poly({{9, 7}, {17, 7}, {17, 15}}); break;
        case Glyph::ArrowDownRight: p.line(7, 7, 17, 17).poly({{17, 9}, {17, 17}, {9, 17}}); break;

        // ---------------- 操作 ----------------
        case Glyph::Search: p.o(10.5f, 10.5f, 6.5f).line(15.4f, 15.4f, 20, 20); break;
        case Glyph::Settings:
            p.o(12, 12, 3.2f);
            p.m(12, 3).v(3).m(12, 18).v(3).m(3, 12).h(3).m(18, 12).h(3);
            p.m(5.6f, 5.6f).l(2.1f, 2.1f).m(18.4f, 18.4f).l(-2.1f, -2.1f);
            p.m(18.4f, 5.6f).l(2.1f, 2.1f).m(5.6f, 18.4f).l(-2.1f, -2.1f);
            break;
        case Glyph::Trash:
            p.r(5, 7, 14, 13, 1.5f);
            p.m(3.5f, 7).h(17).m(9, 7).v(-2.5f).h(6).v(2.5f);
            p.m(10, 11).v(5).m(14, 11).v(5);
            break;
        case Glyph::Edit:
            p.poly({{4, 20}, {8.5f, 19}, {20, 7.5f}, {16.5f, 4}, {5, 15.5f}}).close();
            p.m(15, 5.5f).l(3.5f, 3.5f);
            break;
        case Glyph::Copy:
            p.r(8, 8, 12, 12, 2.0f);
            p.m(16, 8).v(-2).a(2, 2, 0, false, false, -2, -2).h(-8)
                .a(2, 2, 0, false, false, -2, 2).v(8);
            break;
        case Glyph::Save:
            p.poly({{4, 4}, {17, 4}, {20, 7}, {20, 20}, {4, 20}}).close();
            p.r(8, 4, 8, 6, 0.0f);
            p.r(7, 13, 10, 7, 0.0f);
            break;
        case Glyph::Download: p.line(12, 3, 12, 15).poly({{7, 10}, {12, 15}, {17, 10}})
                                     .m(4, 19).h(16);
            break;
        case Glyph::Upload: p.line(12, 15, 12, 3).poly({{7, 8}, {12, 3}, {17, 8}})
                                   .m(4, 19).h(16);
            break;
        case Glyph::Refresh:
            p.b.addArc(SkRect::MakeXYWH(4, 4, 16, 16), 40, 280);
            p.m(19.5f, 4).l(0.8f, 5.2f).l(-5.0f, -1.6f);
            break;
        case Glyph::Undo:
            p.b.addArc(SkRect::MakeXYWH(5, 6, 15, 14), 190, -180);
            p.m(5, 6).l(0, 5).l(5, -1.5f);
            break;
        case Glyph::Redo:
            p.b.addArc(SkRect::MakeXYWH(4, 6, 15, 14), 170, 180);
            p.m(19, 6).l(0, 5).l(-5, -1.5f);
            break;
        case Glyph::Filter: p.poly({{3, 5}, {21, 5}, {14, 13}, {14, 20}, {10, 18}, {10, 13}})
                                    .close();
            break;
        case Glyph::Sort: p.line(4, 7, 16, 7).line(4, 12, 13, 12).line(4, 17, 10, 17)
                                .line(19, 6, 19, 18).poly({{16, 15}, {19, 18}, {22, 15}});
            break;
        case Glyph::Expand: p.poly({{9, 5}, {5, 5}, {5, 9}}).m(15, 5).l(4, 0).l(0, 4)
                                  .m(9, 19).l(-4, 0).l(0, -4).m(15, 19).l(4, 0).l(0, -4);
            break;
        case Glyph::Collapse: p.poly({{5, 9}, {9, 9}, {9, 5}}).m(19, 9).l(-4, 0).l(0, -4)
                                    .m(5, 15).l(4, 0).l(0, 4).m(19, 15).l(-4, 0).l(0, 4);
            break;
        case Glyph::Grip:
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 2; ++j) p.o(8.0f + j * 8.0f, 7.0f + i * 5.0f, 1.4f);
            }
            break;
        case Glyph::Fullscreen: p.poly({{4, 9}, {4, 4}, {9, 4}}).m(15, 4).l(5, 0).l(0, 5)
                                      .m(4, 15).l(0, 5).l(5, 0).m(15, 20).l(5, 0).l(0, -5);
            break;
        case Glyph::ZoomIn: p.o(10.5f, 10.5f, 6.5f).line(15.4f, 15.4f, 20, 20)
                                   .m(7.5f, 10.5f).h(6).m(10.5f, 7.5f).v(6);
            break;
        case Glyph::ZoomOut: p.o(10.5f, 10.5f, 6.5f).line(15.4f, 15.4f, 20, 20)
                                    .m(7.5f, 10.5f).h(6);
            break;

        // ---------------- 状态 ----------------
        case Glyph::Info: p.o(12, 12, 9).o(12, 8, 0.9f).m(12, 11.5f).v(5); break;
        case Glyph::Warning:
            p.poly({{12, 3}, {22, 20}, {2, 20}}).close().m(12, 9).v(5).o(12, 17, 0.9f);
            break;
        case Glyph::Error: p.o(12, 12, 9).line(8.5f, 8.5f, 15.5f, 15.5f)
                                  .line(15.5f, 8.5f, 8.5f, 15.5f);
            break;
        case Glyph::Success: p.o(12, 12, 9).poly({{7.5f, 12}, {11, 15.5f}, {16.5f, 8.5f}}); break;
        case Glyph::Question:
            p.o(12, 12, 9);
            p.m(9.4f, 9.6f).c(9.4f, 7.2f, 14.6f, 7.2f, 14.6f, 10.2f)
                .c(14.6f, 12.4f, 12, 12.2f, 12, 14.4f);
            p.o(12, 17, 0.9f);
            break;
        case Glyph::Spinner:
            p.b.addArc(SkRect::MakeXYWH(3, 3, 18, 18), -60, 270);
            break;
        case Glyph::Clock: p.o(12, 12, 9).m(12, 6.5f).v(6).l(4.2f, 2.6f); break;
        case Glyph::Bell:
            p.m(6, 16).c(6, 13, 7, 12.5f, 7, 10).c(7, 6.6f, 9, 5, 12, 5)
                .c(15, 5, 17, 6.6f, 17, 10).c(17, 12.5f, 18, 13, 18, 16).close();
            p.m(10, 19).c(10.4f, 20.4f, 13.6f, 20.4f, 14, 19);
            break;

        // ---------------- 对象 ----------------
        case Glyph::User: p.o(12, 8, 4).m(4.5f, 20)
                                  .c(4.5f, 15.5f, 8, 14, 12, 14).c(16, 14, 19.5f, 15.5f, 19.5f, 20);
            break;
        case Glyph::Users:
            p.o(9, 8, 3.4f);
            p.m(3, 19.5f).c(3, 15.6f, 6, 14, 9, 14).c(12, 14, 15, 15.6f, 15, 19.5f);
            p.m(16, 5.4f).c(18.4f, 6, 19, 8.6f, 17.4f, 10.4f);
            p.m(17, 14.2f).c(19.6f, 14.8f, 21, 16.6f, 21, 19.5f);
            break;
        case Glyph::Home: p.poly({{3, 11}, {12, 3.5f}, {21, 11}}).m(5.5f, 10.5f).v(9.5f)
                                 .h(13).v(-9.5f).m(9.5f, 20).v(-5).h(5).v(5);
            break;
        case Glyph::Folder:
            p.m(3, 6.5f).h(6).l(11, 9.5f).h(7).v(10.5f).h(-18).close();
            break;
        case Glyph::FolderOpen:
            p.poly({{3, 19}, {3, 6.5f}, {9, 6.5f}, {11, 9.5f}, {18, 9.5f}, {18, 12}})
                .m(3, 19).l(4, -7).h(17).l(-3.5f, 7).close();
            break;
        case Glyph::File:
            p.poly({{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}}).close();
            p.m(14, 3).v(5).h(5);
            break;
        case Glyph::Image:
            p.r(3, 5, 18, 14, 2.0f).o(8.5f, 10, 1.8f);
            p.m(4, 17).l(5.5f, -5).l(4, 3.5f).l(3.5f, -3).l(3, 3);
            break;
        case Glyph::Mail:
            p.r(3, 5.5f, 18, 13, 2.0f);
            p.m(4, 7).l(8, 6).l(8, -6);
            break;
        case Glyph::Link:
            p.m(9.5f, 14.5f).l(5, -5);
            p.m(8.5f, 11.5f).l(-1.5f, 1.5f).a(4.2f, 4.2f, 0, false, true, 6, 6).l(1.5f, -1.5f);
            p.m(15.5f, 12.5f).l(1.5f, -1.5f).a(4.2f, 4.2f, 0, false, true, -6, -6).l(-1.5f, 1.5f);
            break;
        case Glyph::Tag:
            p.poly({{3, 11}, {11, 3}, {21, 3}, {21, 13}, {13, 21}}).close().o(17, 7, 1.6f);
            break;
        case Glyph::Bookmark: p.poly({{6, 3}, {18, 3}, {18, 21}, {12, 16.5f}, {6, 21}}).close();
            break;
        case Glyph::Pin:
            p.m(12, 3).c(15.3f, 3, 18, 5.7f, 18, 9).c(18, 12, 12, 21, 12, 21)
                .c(12, 21, 6, 12, 6, 9).c(6, 5.7f, 8.7f, 3, 12, 3).close().o(12, 9, 2.2f);
            break;

        // ---------------- 安全 ----------------
        case Glyph::Lock:
            p.r(4.5f, 10, 15, 10.5f, 2.0f);
            p.m(8, 10).v(-3).a(4, 4, 0, false, true, 8, 0).v(3);
            break;
        case Glyph::Unlock:
            p.r(4.5f, 10, 15, 10.5f, 2.0f);
            p.m(8, 10).v(-3).a(4, 4, 0, false, true, 8, 0);
            break;
        case Glyph::Key:
            p.o(8, 15, 4).m(11, 12).l(8, -8).l(3, -3).l(3, 3).m(17, 9).l(-3, -3);
            break;
        case Glyph::Shield:
            p.m(12, 3).l(19, 6).v(6).c(19, 17, 15.6f, 20, 12, 21)
                .c(8.4f, 20, 5, 17, 5, 12).v(-6).close();
            break;
        case Glyph::Eye:
            p.m(2.5f, 12).c(6, 6.5f, 18, 6.5f, 21.5f, 12)
                .c(18, 17.5f, 6, 17.5f, 2.5f, 12).close().o(12, 12, 3);
            break;
        case Glyph::EyeOff:
            p.m(2.5f, 12).c(5.2f, 7.8f, 9.5f, 6.4f, 12.6f, 7.1f);
            p.m(21.5f, 12).c(19.4f, 15.2f, 16.4f, 16.9f, 13.4f, 17.2f);
            p.m(4, 20).l(16, -16);
            break;

        // ---------------- 媒体 ----------------
        case Glyph::Play: p.poly({{7, 4}, {20, 12}, {7, 20}}).close(); break;
        case Glyph::Pause: p.r(6, 4, 4, 16, 1.0f).r(14, 4, 4, 16, 1.0f); break;
        case Glyph::Stop: p.r(5.5f, 5.5f, 13, 13, 1.5f); break;
        case Glyph::Volume:
            p.poly({{4, 9}, {8, 9}, {13, 5}, {13, 19}, {8, 15}, {4, 15}}).close();
            p.m(16, 9).c(18.5f, 10.4f, 18.5f, 13.6f, 16, 15);
            break;
        case Glyph::Camera:
            p.r(3, 7, 18, 13, 2.0f).o(12, 13.5f, 4);
            p.m(8.5f, 7).l(1.5f, -2.5f).h(4).l(1.5f, 2.5f);
            break;

        // ---------------- 布局 / 数据 ----------------
        case Glyph::Menu: p.line(4, 7, 20, 7).line(4, 12, 20, 12).line(4, 17, 20, 17); break;
        case Glyph::MoreH:
            p.o(6, 12, 1.7f).o(12, 12, 1.7f).o(18, 12, 1.7f);
            break;
        case Glyph::MoreV:
            p.o(12, 6, 1.7f).o(12, 12, 1.7f).o(12, 18, 1.7f);
            break;
        case Glyph::Grid:
            p.r(4, 4, 7, 7, 1.0f).r(13, 4, 7, 7, 1.0f).r(4, 13, 7, 7, 1.0f)
                .r(13, 13, 7, 7, 1.0f);
            break;
        case Glyph::List:
            p.o(5.5f, 7, 1.4f).o(5.5f, 12, 1.4f).o(5.5f, 17, 1.4f);
            p.line(10, 7, 20, 7).line(10, 12, 20, 12).line(10, 17, 20, 17);
            break;
        case Glyph::Table:
            p.r(3, 5, 18, 14, 1.5f).line(3, 10, 21, 10).line(9.5f, 10, 9.5f, 19);
            break;
        case Glyph::Layers:
            p.poly({{12, 3}, {21, 8}, {12, 13}, {3, 8}}).close();
            p.m(3, 12.5f).l(9, 5).l(9, -5).m(3, 17).l(9, 5).l(9, -5);
            break;
        case Glyph::Box:
            p.poly({{12, 3}, {20, 7.5f}, {20, 16.5f}, {12, 21}, {4, 16.5f}, {4, 7.5f}}).close();
            p.m(4, 7.5f).l(8, 4.5f).l(8, -4.5f).m(12, 12).v(9);
            break;
        case Glyph::Database:
            p.r(4, 4, 16, 16, 3.0f);
            p.m(4, 9.5f).h(16).m(4, 14.5f).h(16);
            break;
        case Glyph::Terminal:
            p.r(3, 4, 18, 16, 2.0f);
            p.m(7, 10).l(3, 3).l(-3, 3).m(12.5f, 16).h(4.5f);
            break;
        case Glyph::Code: p.poly({{9, 7}, {4, 12}, {9, 17}}).m(15, 7).l(5, 5).l(-5, 5); break;
        case Glyph::Bug:
            p.o(12, 13, 5.5f);
            p.m(12, 7.5f).v(-3).m(4.5f, 8.5f).l(3, 2).m(19.5f, 8.5f).l(-3, 2);
            p.m(3.5f, 13).h(3).m(17.5f, 13).h(3).m(5, 18).l(2.6f, -2).m(19, 18).l(-2.6f, -2);
            break;
        case Glyph::Chart:
            p.line(4, 20, 20, 20).m(7, 20).v(-7).m(12, 20).v(-11).m(17, 20).v(-5);
            break;
        case Glyph::LineChart:
            p.line(4, 20, 20, 20).m(4, 20).l(4, 4);
            p.poly({{5, 16}, {9, 11}, {13, 14}, {19, 6}});
            break;
        case Glyph::BarChart:
            p.line(4, 20, 20, 20);
            p.r(5.5f, 12, 3.5f, 8, 1.0f).r(10.25f, 7, 3.5f, 13, 1.0f).r(15, 15, 3.5f, 5, 1.0f);
            break;
        case Glyph::PieChart:
            p.o(12, 12, 9).m(12, 12).l(12, 3).m(12, 12).l(19.8f, 15.5f);
            break;
        case Glyph::Activity: p.poly({{3, 13}, {8, 13}, {11, 6}, {14, 19}, {17, 13}, {21, 13}});
            break;
        case Glyph::TrendingUp: p.poly({{3, 17}, {9, 11}, {13, 15}, {21, 7}})
                                       .m(21, 12).v(-5).h(-5);
            break;
        case Glyph::TrendingDown: p.poly({{3, 7}, {9, 13}, {13, 9}, {21, 17}})
                                         .m(21, 12).v(5).h(-5);
            break;
        case Glyph::Cpu:
            p.r(6, 6, 12, 12, 2.0f).r(9.5f, 9.5f, 5, 5, 1.0f);
            p.m(9.5f, 6).v(-3).m(14.5f, 6).v(-3).m(9.5f, 18).v(3).m(14.5f, 18).v(3)
                .m(6, 9.5f).h(-3).m(6, 14.5f).h(-3).m(18, 9.5f).h(3).m(18, 14.5f).h(3);
            break;
        case Glyph::Globe:
            p.o(12, 12, 9).m(3, 12).h(18);
            p.b.addOval(SkRect::MakeXYWH(8, 3, 8, 18));
            break;

        // ---------------- 主题 ----------------
        case Glyph::Sun:
            p.o(12, 12, 4.2f);
            p.m(12, 2.5f).v(2.5f).m(12, 19).v(2.5f).m(2.5f, 12).h(2.5f).m(19, 12).h(2.5f);
            p.m(5.3f, 5.3f).l(1.8f, 1.8f).m(16.9f, 16.9f).l(1.8f, 1.8f);
            p.m(18.7f, 5.3f).l(-1.8f, 1.8f).m(7.1f, 16.9f).l(-1.8f, 1.8f);
            break;
        case Glyph::Moon:
            p.m(20, 14.5f).c(17.5f, 19.5f, 11, 20.8f, 6.5f, 17)
                .c(2, 13.2f, 2.5f, 6.5f, 7.5f, 4)
                .c(5.5f, 9, 7.5f, 14, 12.5f, 15.2f).c(15, 15.8f, 17.6f, 15.5f, 20, 14.5f)
                .close();
            break;
        case Glyph::Star:
            p.poly({{12, 3}, {14.7f, 9}, {21, 9.6f}, {16.3f, 13.9f}, {17.6f, 20.3f},
                    {12, 17.1f}, {6.4f, 20.3f}, {7.7f, 13.9f}, {3, 9.6f}, {9.3f, 9}})
                .close();
            break;
        case Glyph::Heart:
            p.m(12, 20).c(4, 14.5f, 2.5f, 10.5f, 5.2f, 7.6f)
                .c(7.6f, 5.1f, 10.6f, 6.1f, 12, 8.4f)
                .c(13.4f, 6.1f, 16.4f, 5.1f, 18.8f, 7.6f)
                .c(21.5f, 10.5f, 20, 14.5f, 12, 20).close();
            break;
        case Glyph::Power: p.o(12, 12, 8.5f).m(12, 3).v(9); break;
        case Glyph::Lightning: p.poly({{13, 2.5f}, {5.5f, 13.5f}, {11, 13.5f}, {10, 21.5f},
                                       {18.5f, 10}, {13, 10}}).close();
            break;
        case Glyph::Flame:
            p.m(12, 2.5f).c(15, 6, 19, 8.5f, 19, 14)
                .c(19, 18, 16, 21, 12, 21).c(8, 21, 5, 18, 5, 14)
                .c(5, 11, 7.5f, 9.5f, 9, 7)
                .c(9.5f, 9.5f, 11, 10, 11, 8).c(11, 6, 12, 4, 12, 2.5f).close();
            break;
        case Glyph::Calendar:
            p.r(3.5f, 5, 17, 15.5f, 2.0f);
            p.m(3.5f, 10).h(17).m(8, 5).v(-3).m(16, 5).v(-3);
            break;

        default: break;
    }
    return p.b.detach();
}

const char* const kNames[] = {
    "none", "check", "close", "plus", "minus", "dot", "circle", "square", "triangle",
    "chevron-up", "chevron-down", "chevron-left", "chevron-right",
    "arrow-up", "arrow-down", "arrow-left", "arrow-right", "arrow-up-right", "arrow-down-right",
    "search", "settings", "trash", "edit", "copy", "save", "download", "upload", "refresh",
    "undo", "redo", "filter", "sort", "expand", "collapse", "grip", "fullscreen", "zoom-in",
    "zoom-out", "info", "warning", "error", "success", "question", "spinner", "clock", "bell",
    "user", "users", "home", "folder", "file", "folder-open", "image", "mail", "link", "tag",
    "bookmark", "pin", "lock", "unlock", "key", "shield", "eye", "eye-off",
    "play", "pause", "stop", "volume", "camera",
    "menu", "more-h", "more-v", "grid", "list", "table", "layers", "box", "database", "terminal",
    "code", "bug", "chart", "line-chart", "bar-chart", "pie-chart", "activity", "trending-up",
    "trending-down", "cpu", "globe",
    "sun", "moon", "star", "heart", "power", "lightning", "flame", "calendar",
};

}  // namespace

const SkPath& Get(Glyph g) {
    static std::unordered_map<int, SkPath> cache;
    const int key = static_cast<int>(g);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    SkPath path = BuildPath(g);
    auto ins = cache.emplace(key, std::move(path));
    return ins.first->second;
}

void Draw(PaintContext& ctx, Glyph g, const Rect& box, SkColor color, float strokeWidth) {
    if (g == Glyph::None || !ctx.canvas() || box.isEmpty()) return;
    const SkPath& path = Get(g);
    if (path.isEmpty()) return;

    const float scale = std::min(box.width(), box.height()) / 24.0f;
    const float ox = box.left() + (box.width() - 24.0f * scale) * 0.5f;
    const float oy = box.top() + (box.height() - 24.0f * scale) * 0.5f;

    ctx.save();
    ctx.translate(ox, oy);
    ctx.scale(scale, scale);
    Paint p = Paint::Stroke(color, (strokeWidth > 0.0f ? strokeWidth : 2.0f) / scale);
    ctx.drawPath(path, p);
    ctx.restore();
}

const char* NameOf(Glyph g) {
    const int idx = static_cast<int>(g);
    const int n = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
    return (idx >= 0 && idx < n) ? kNames[idx] : "none";
}

Glyph FromName(const std::string& name) {
    const int n = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
    for (int i = 0; i < n; ++i) {
        if (name == kNames[i]) return static_cast<Glyph>(i);
    }
    return Glyph::None;
}

}  // namespace icons

}  // namespace uikit
}  // namespace skiagui
