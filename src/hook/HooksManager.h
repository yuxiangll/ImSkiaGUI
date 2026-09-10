// ============================================================================
//  HooksManager.h — MinHook 装载与 DX12 钩子管理
// ----------------------------------------------------------------------------
//  钩取目标（全部是 dxgi.dll / d3d12.dll 里的**函数**，不是某个对象的 vtable，
//  这样宿主进程里所有交换链/队列都会被覆盖，和 kiero/ImGui 的做法一致）：
//
//    IDXGISwapChain::Present          vtable[8]   —— 每帧的渲染时机
//    IDXGISwapChain1::Present1        vtable[22]  —— 有些引擎走这个
//    IDXGISwapChain::ResizeBuffers    vtable[13]  —— 尺寸变化时释放/重建资源
//    ID3D12CommandQueue::ExecuteCommandLists [10] —— 捕获宿主的 DIRECT 队列
//
//  索引来源（不是猜的，见 core/Config.h 注释）：
//    dxgi.h:  IDXGISwapChain : IDXGIDeviceSubObject : IDXGIObject : IUnknown
//             => GetDevice 占槽 7，Present 占槽 8，ResizeBuffers 占槽 13，
//                IDXGISwapChain1::Present1 占槽 22
//    d3d12.h: ID3D12CommandQueue 自身 vtable 只有 19 个槽（0..18），
//             ExecuteCommandLists 是第 11 个 => 槽 10。
//             ⚠ 网上常见的 54 是「Device(44)+Queue(19)+... 拼接表」的编号，
//               不是对象 vtable 下标，用它钩不到（本项目实测踩过，见
//               docs/architecture.md 第 2.3 节）。
//
//  取 vtable 的办法：自己造一个 1x1 的隐藏窗口 + 临时 D3D12 设备/队列/交换链，
//  读出这些函数的地址，然后释放临时对象、销毁窗口，只留下 MinHook 的钩子。
//  参考 ./ref/Dx12HookExample-master/Dx12HookExample/dllmain.cpp 的 mainThread()
//  与 ./ref/D3D12-Hook-ImGui-master/main.h 的 DirectX12::Init()。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

namespace skiagui {
namespace hooks {

// 本 DLL 的 HMODULE（在 Initialize 时记录，供其它模块定位 skia.dll）。
HMODULE SelfModule();

// 1) MH_Initialize
// 2) 造临时 D3D12 设备/队列/交换链，取出 4 个目标函数地址
// 3) MH_CreateHook + MH_EnableHook
// 4) 释放临时对象
// 返回 false 时钩子未安装，DLL 会继续待着（不注入任何东西）。
bool Initialize(HMODULE selfModule);

// 卸载全部钩子并 MH_Uninitialize。幂等。
// 必须在确认没有线程还在执行我们的钩子之后才能调用 FreeLibrary，
// 见 dllmain.cpp 的卸载序列。
void Shutdown();

bool Installed();

// 从 ExecuteCommandLists 钩子捕获到的宿主 DIRECT 命令队列（不持有引用）。
ID3D12CommandQueue* CapturedCommandQueue();

// 从 Present 钩子捕获到的 D3D12 设备（不持有引用，仅诊断用）。
ID3D12Device* CapturedDevice();

// 卸载请求（END 热键触发），由工作线程轮询。
void RequestEject();
bool EjectRequested();

// 当前正在执行我们钩子的线程数。卸载前必须等到 0。
long ActiveCallCount();

// Present 被调用的次数（诊断用）。
// 用途：如果注入到一个根本不使用 D3D 的进程（记事本、压缩软件、Qt 程序…），
// 钩子装上了但 Present 永远不会被调用，自然也不会有任何画面。
// 工作线程会用它来判断并给出明确提示。
long PresentCallCount();

// 标记"当前线程正处在我们的 WndProc 里"。
// Raw Input 钩子会据此跳过清零 —— 否则我们自己在 WndProc 里读鼠标增量时，
// 会被自己的钩子抹成 0，软件光标就不动了。
void EnterOurWndProc();
void LeaveOurWndProc();

}  // namespace hooks
}  // namespace skiagui
