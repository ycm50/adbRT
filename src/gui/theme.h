#pragma once
// ---------------------------------------------------------------------------
// 视觉主题：所有颜色、字体、圆角、间距集中在这里（改一处 = 全局生效）
//   只依赖 Win32 + GDI，不引入任何第三方界面库
// ---------------------------------------------------------------------------
#include <windows.h>

namespace adbgui {
namespace theme {

// ---- 调色板（浅色，接近现代 Windows 11 风格的克制配色）----
inline constexpr COLORREF kWindowBg     = RGB(0xF3, 0xF4, 0xF6);   // 窗口底色
inline constexpr COLORREF kPaneBg       = RGB(0xFF, 0xFF, 0xFF);   // 面板/编辑框
inline constexpr COLORREF kHeaderBg     = RGB(0xFF, 0xFF, 0xFF);   // 顶部栏
inline constexpr COLORREF kBorder       = RGB(0xE2, 0xE5, 0xEA);   // 1px 分隔线
inline constexpr COLORREF kText         = RGB(0x1F, 0x24, 0x2B);   // 主文字
inline constexpr COLORREF kTextDim      = RGB(0x6B, 0x72, 0x80);   // 次要文字
inline constexpr COLORREF kAccent       = RGB(0x25, 0x63, 0xEB);   // 主色（按钮/选中）
inline constexpr COLORREF kAccentHover  = RGB(0x1D, 0x4E, 0xD8);
inline constexpr COLORREF kAccentSoft   = RGB(0xE8, 0xEF, 0xFD);   // 选中行底色
inline constexpr COLORREF kDanger       = RGB(0xD1, 0x24, 0x2F);
inline constexpr COLORREF kDangerSoft   = RGB(0xFD, 0xE8, 0xEA);
inline constexpr COLORREF kWarn         = RGB(0xB4, 0x53, 0x09);
inline constexpr COLORREF kWarnSoft     = RGB(0xFE, 0xF3, 0xC7);
inline constexpr COLORREF kOk           = RGB(0x15, 0x80, 0x3D);
inline constexpr COLORREF kOkSoft       = RGB(0xDC, 0xFC, 0xE7);
inline constexpr COLORREF kCodeBg       = RGB(0x0F, 0x17, 0x2A);   // 命令行预览（深色）
inline constexpr COLORREF kCodeText     = RGB(0xE5, 0xE7, 0xEB);
inline constexpr COLORREF kCodeDim      = RGB(0x94, 0xA3, 0xB8);

// ---- 间距 ----
inline constexpr int kGap      = 8;
inline constexpr int kGapSmall = 6;
inline constexpr int kPad      = 10;
inline constexpr int kRowH     = 22;

// ---- 资源 ----
HFONT      FontNormal();
HFONT      FontBold();
HFONT      FontTitle();
HFONT      FontMono();
HBRUSH     Brush(COLORREF c);

// 按当前窗口 DPI 缩放
void       InitForWindow(HWND hwnd);
int        Dpi(int value);

// 绘制小工具
void       FillRectColor(HDC dc, const RECT& rc, COLORREF c);
void       DrawBorder(HDC dc, const RECT& rc, COLORREF c);
void       DrawSectionTitle(HDC dc, const RECT& rc, const wchar_t* text);
void       DrawPill(HDC dc, const RECT& rc, const wchar_t* text, COLORREF fg, COLORREF bg);

}  // namespace theme
}  // namespace adbgui
