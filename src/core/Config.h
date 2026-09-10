// ============================================================================
//  Config.h — 编译期配置与全局常量
// ----------------------------------------------------------------------------
//  所有可调项集中在这里，避免散落在各模块里的魔数。
//  说明：本项目的渲染路线是 README 第 5 节的「路线 A：CPU 光栅 + 纹理上传」，
//  原因是 sdk\skia.dll 未启用 Ganesh 的 D3D 后端（没有 GrDirectContexts::
//  MakeDirect3D 符号）。要走路线 C 只需重新编译 skia.dll 并替换
//  render/D3D12Backend 中的上传路径，见 docs/architecture.md 第 3.3 节。
// ============================================================================
#pragma once

#include <cstdint>

namespace skiagui {
namespace config {

// ---------------------------------------------------------------- 版本 / 日志
// 日志文件名带上 PID：同一台机器可能同时有多个进程加载本 DLL（游戏 + 其它程序），
// 固定文件名会互相截断/覆盖，排查时拿到的日志是坏的。
inline constexpr const char* kLogFileName = "skiagui_overlay_<pid>.log";
inline constexpr const char* kSkiaDllName = "skia.dll";

// ---------------------------------------------------------------- Hook 索引
// 以下索引由 Windows SDK 头文件的接口继承顺序直接推导（不是猜的）：
//   dxgi.h:  IDXGISwapChain : public IDXGIDeviceSubObject
//            IDXGIDeviceSubObject : public IDXGIObject
//            IDXGIObject : public IUnknown
//   => IUnknown{QueryInterface,AddRef,Release}            = 0..2
//      IDXGIObject{SetPrivateData,SetPrivateDataInterface,
//                  GetPrivateData,GetParent}               = 3..6
//      IDXGIDeviceSubObject{GetDevice}                     = 7
//      IDXGISwapChain{Present,...}                         = 8..
//   验证：dxgi.h 中 IDXGISwapChain1 的第一个方法是 GetDesc1，
//         若 IDXGISwapChain 占 18 个槽（0..17），则 Present1 = 22，
//         与 ./ref/Dx12HookExample-master 的 vtable[22] 完全一致。
//   d3d12.h: ID3D12CommandQueue 的**自身 vtable**只有 19 个槽：
//              IUnknown{QueryInterface,AddRef,Release}         = 0..2
//              ID3D12Object{GetPrivateData,SetPrivateData,
//                           SetPrivateDataInterface,SetName}   = 3..6
//              ID3D12DeviceChild{GetDevice}                    = 7
//              ID3D12CommandQueue{UpdateTileMappings,
//                                 CopyTileMappings,
//                                 ExecuteCommandLists, ...}     = 8..
//            => ExecuteCommandLists 是该接口第 11 个方法 = 槽 10。
//   ⚠ 常见误区（本项目踩过）：网上很多代码（包括 ./ref/D3D12-Hook-ImGui-master
//     的 main.h）用的是「把 ID3D12Device(44) + CommandQueue(19) + ... 拼成一张
//     大表」后的**拼接表下标 54**。那是拼接表的编号，不是对象 vtable 的下标；
//     拿 54 去索引一个 ID3D12CommandQueue 对象的 vtable 会越界读到别的
//     接口的静态表，钩子永远不触发。实测已确认（见 docs/architecture.md）。
inline constexpr uint16_t kSwapChainVtbl_Present = 8;
inline constexpr uint16_t kSwapChainVtbl_GetBuffer = 9;
// SetFullscreenState / ResizeTarget 也必须钩：独占全屏切换时宿主要求我们先放掉
// 所有后备缓冲引用，否则 SetFullscreenState/ResizeBuffers 会失败或画面卡死。
inline constexpr uint16_t kSwapChainVtbl_SetFullscreenState = 10;
inline constexpr uint16_t kSwapChainVtbl_ResizeBuffers = 13;
inline constexpr uint16_t kSwapChainVtbl_ResizeTarget = 14;
inline constexpr uint16_t kSwapChain1Vtbl_Present1 = 22;
inline constexpr uint16_t kCommandQueueVtbl_ExecuteCommandLists = 10;

// ---------------------------------------------------------------- 热键 / 行为
// 菜单显隐：INSERT（与 ref 一致，绝大多数游戏不占用）
inline constexpr int kMenuToggleVk = 0x2D;  // VK_INSERT
// 卸载热键：END。按下后渲染线程在下一帧安全卸载 DLL。
inline constexpr int kEjectVk = 0x23;  // VK_END
// 卸载确认：必须在连续 N 次 Present 内没有别的线程在跑我们的代码。
inline constexpr int kEjectDrainSpinCount = 2000;

// ---------------------------------------------------------------- 渲染
// DX12 资源在飞行中的帧数上限（= 后备缓冲数上限）。3 覆盖 flip 三缓冲。
inline constexpr uint32_t kMaxFramesInFlight = 3;
// 上传纹理行距必须按 256 字节对齐（D3D12 对 UPLOAD 堆资源的要求）。
inline constexpr uint32_t kUploadRowPitchAlign = 256;
// 描述符堆大小：RTV 堆 = 后备缓冲数；SRV 堆 = 1（我们的 Skia 覆盖层纹理）。
inline constexpr uint32_t kRtvHeapSize = kMaxFramesInFlight;
inline constexpr uint32_t kSrvHeapSize = 1;
// 围栏等待上限（毫秒）。超时只记日志、不阻塞宿主。
inline constexpr uint32_t kFenceWaitTimeoutMs = 2000;

// ---------------------------------------------------------------- 输入 / 鼠标锁定
// 菜单打开时，每帧调用 ClipCursor(nullptr) 抢回光标（游戏通常每帧 ClipCursor 锁回去，
// 所以必须每帧抢）。只在 UI 需要鼠标时做，避免影响正常游戏。
inline constexpr bool kReleaseCursorClipWhileMenuOpen = true;
// 软件光标：游戏用 Raw Input 且把系统光标锁死时，系统光标不会跟着动，
// 这时由 Skia 自己画一个箭头。
inline constexpr float kCursorSize = 18.0f;

// ---------------------------------------------------------------- UI
// 面板尺寸（96 DPI 下的逻辑像素，实际会按 DPI 缩放）
inline constexpr float kPanelWidth = 460.0f;
inline constexpr float kPanelHeight = 420.0f;
inline constexpr float kPanelMargin = 24.0f;
inline constexpr float kRowHeight = 26.0f;
inline constexpr float kTitleHeight = 38.0f;
inline constexpr float kPadding = 14.0f;
inline constexpr float kLabelWidth = 130.0f;
inline constexpr float kCornerRadius = 10.0f;
// 字体大小
inline constexpr float kFontSizeTitle = 15.0f;
inline constexpr float kFontSizeBody = 14.0f;
inline constexpr float kFontSizeSmall = 11.0f;

// ---------------------------------------------------------------- 主题色
// 参考 ./src/main.cpp 的配色（深色玻璃面板 + 蓝色高亮）。
inline constexpr uint32_t kColorPanel = 0xE8181B22;        // ARGB 232,24,27,34
inline constexpr uint32_t kColorTitle = 0xFF264E94;        // ARGB 255,38,78,148
inline constexpr uint32_t kColorBorder = 0x5A78AAFF;
inline constexpr uint32_t kColorLabel = 0xD2D6DEEB;
inline constexpr uint32_t kColorValue = 0xFF8CC8FF;
inline constexpr uint32_t kColorText = 0xFFE6ECF5;
inline constexpr uint32_t kColorAccent = 0xFF5AAAFF;
inline constexpr uint32_t kColorTrack = 0x46FFFFFF;
inline constexpr uint32_t kColorGood = 0xFF46A06E;
inline constexpr uint32_t kColorWarn = 0xFFFFAA46;
inline constexpr uint32_t kColorShadow = 0x78000000;

}  // namespace config
}  // namespace skiagui
