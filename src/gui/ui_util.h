#pragma once
#include <windows.h>

#include <string>

namespace adbgui {

HFONT CreateUiFont(int pt, bool mono);
void  ApplyFont(HWND control, HFONT font);
std::wstring GetTextOf(HWND control);
void  SetTextOf(HWND control, const std::wstring& text);
void  AppendTextToEdit(HWND edit, const std::wstring& text);
void  ScrollToEnd(HWND edit);
std::wstring GetDpiScaledFontName();

int   ScaleForDpi(HWND hwnd, int value);

}  // namespace adbgui
