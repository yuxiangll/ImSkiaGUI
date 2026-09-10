// ============================================================================
//  Clipboard.h — 剪贴板（TextField / TextArea / SelectableText 的复制粘贴）
// ----------------------------------------------------------------------------
//  直接用 Win32 剪贴板（CF_UNICODETEXT）。注意：overlay 的每帧路径禁止 IO，
//  所以**只在用户按下 Ctrl+C/V/X 的那一刻**调用，绝不在绘制里调。
// ============================================================================
#pragma once

#include <string>

namespace skiagui {
namespace uikit {

namespace clipboard {

// 读取剪贴板文本（UTF-8）；失败返回空串
std::string GetText();
// 写入剪贴板文本（UTF-8）；返回是否成功
bool SetText(const std::string& utf8);

}  // namespace clipboard

}  // namespace uikit
}  // namespace skiagui
