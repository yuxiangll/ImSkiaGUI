// ============================================================================
//  Shaders.h — 内嵌 HLSL（运行时用 D3DCompile 编译）
// ----------------------------------------------------------------------------
//  为什么运行时编译而不是离线 fxc/dxc：
//    1) 工程只需要 Windows SDK 自带的 d3dcompiler_47.dll，构建链最简单；
//    2) 注入型 DLL 体积小，改 shader 不用改构建脚本；
//    3) 全屏三角形 + 一次采样，编译耗时可忽略（只编译一次，约 1~3 ms）。
//  若要零依赖，可换成离线编译出 .cso 字节码数组，见 docs/architecture.md。
// ============================================================================
#pragma once

namespace skiagui {
namespace render {
namespace shaders {

// 顶点着色器：不绑定顶点缓冲，用 SV_VertexID 生成一个覆盖全屏的三角形。
// vid=0 -> uv(0,0) 左上；vid=1 -> uv(2,0)；vid=2 -> uv(0,2)。
// 屏幕内的部分 uv 恰好落在 [0,1]，与覆盖层纹理 1:1 对应。
inline constexpr const char* kQuadVs = R"(
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);          // (0,0) (2,0) (0,2)
    o.uv  = uv;
    // D3D 的 NDC：y 向上为正，所以这里把 uv.y 翻过来，让 v=0 在屏幕顶部，
    // 与 Skia 的自上而下像素布局一致。
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

// 像素着色器：采样 Skia 覆盖层纹理并乘以全局不透明度（根常量 b0.a）。
// 纹理内容是 Skia 的 N32Premul（预乘 alpha），所以混合必须是 ONE / INV_SRC_ALPHA。
inline constexpr const char* kQuadPs = R"(
Texture2D    overlayTex     : register(t0);
SamplerState overlaySampler : register(s0);

float4 tint : register(b0);   // 根常量：rgb 预留，a = 全局不透明度

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = overlayTex.Sample(overlaySampler, uv);
    // 预乘 alpha：直接乘全局不透明度即可（颜色本身已经乘过 alpha）。
    c *= tint.a;
    return c;
}
)";

}  // namespace shaders
}  // namespace render
}  // namespace skiagui
