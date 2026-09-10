// ============================================================================
//  Utf8.h — UTF-8 工具（文本编辑 / 光标 / 选区都要按"字符"而不是字节）
// ----------------------------------------------------------------------------
//  Skia 的 drawString 直接吃 UTF-8，但编辑类控件必须能：
//    * 把光标位置（字节偏移）前后移动一个**字符**（中文/emoji 是 3~4 字节）；
//    * 把鼠标 x 坐标换成字节偏移；
//    * 安全截断（不切半个字符）。
//  这里统一提供这些操作，全部按 UTF-8 码点边界处理。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace skiagui {
namespace uikit {

namespace utf8 {

// 解码一个码点；返回消耗的字节数（0 表示非法/结束）
int Decode(const char* s, size_t len, uint32_t* outCodepoint);

// 把码点编码成 UTF-8 追加到 out
void Encode(uint32_t cp, std::string* out);

// ---- 位置移动（返回新的字节偏移）---------------------------------------------
size_t NextOffset(const std::string& s, size_t offset);        // 右移一个字符
size_t PrevOffset(const std::string& s, size_t offset);        // 左移一个字符
size_t ClampOffset(const std::string& s, size_t offset);       // 吸附到字符边界

// 行首 / 行尾
size_t LineStart(const std::string& s, size_t offset);
size_t LineEnd(const std::string& s, size_t offset);

// 词边界（Ctrl+左右 / 双击选词）
size_t WordLeft(const std::string& s, size_t offset);
size_t WordRight(const std::string& s, size_t offset);

// 字符数 / 第 n 个字符的字节偏移
size_t Length(const std::string& s);
size_t OffsetOfIndex(const std::string& s, size_t index);
size_t IndexOfOffset(const std::string& s, size_t offset);

// 取一个码点
uint32_t CodepointAt(const std::string& s, size_t offset);
// 是否是"空白"（空格/制表/全角空格）
bool IsSpace(uint32_t cp);
// 是否需要在换行时按字符断（CJK）
bool IsBreakableEverywhere(uint32_t cp);

// 安全截断（按字符，不切半个）
std::string Truncate(const std::string& s, size_t maxChars);
// 按显示宽度截断，超出加省略号（宽度计算由调用方给的测量函数提供）
std::string Ellipsize(const std::string& s, float maxWidth,
                      float (*measure)(const std::string&, void*), void* user);

// 去除首尾空白
std::string Trim(const std::string& s);

// 大小写（仅 ASCII，够用）
std::string ToLower(const std::string& s);
std::string ToUpper(const std::string& s);

}  // namespace utf8
}  // namespace uikit
}  // namespace skiagui
