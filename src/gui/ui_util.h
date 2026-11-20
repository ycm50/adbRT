#pragma once
#include <windows.h>

#include <string>

namespace adbgui {

HFONT CreateUiFont(int pt, bool mono);
void  ApplyFont(HWND control, HFONT font);
std::wstring GetTextOf(HWND control);
void  SetTextOf(HWND control, const std::wstring& text);
// EDIT 控件默认只接受 30000 字符，超出部分会被静默吞掉（看起来像"输出丢了"）。
// 需要显示大量文本的控件在创建后必须调用它放宽上限。
void  RelaxEditLimit(HWND edit);
void  AppendTextToEdit(HWND edit, const std::wstring& text);
void  ScrollToEnd(HWND edit);
// 只在用户本来就停在末尾时跟随新输出；用户往上翻看历史时不打断
void  FollowTailIfAtBottom(HWND edit);
// 把 EDIT 里的文本裁到 maxChars 以内（超了裁掉最前面的一段）。
// 返回 true 表示真的裁了，调用方应据此重新定位滚动位置。
bool  TrimEditToBudget(HWND edit, size_t maxChars);
std::wstring GetDpiScaledFontName();

int   ScaleForDpi(HWND hwnd, int value);
std::wstring Trim(const std::wstring& s);

}  // namespace adbgui