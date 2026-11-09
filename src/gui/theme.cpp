#include "theme.h"

#include <string>
#include <unordered_map>

namespace adbgui {
namespace theme {

namespace {

int g_dpi = 96;
std::unordered_map<COLORREF, HBRUSH>* g_brushes = nullptr;

HFONT MakeFont(int pt, int weight, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return ::CreateFontIndirectW(&lf);
}

}  // namespace

void InitForWindow(HWND hwnd) {
    HDC dc = ::GetDC(hwnd);
    if (dc) {
        g_dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
        ::ReleaseDC(hwnd, dc);
    }
    if (g_dpi <= 0) g_dpi = 96;
}

int Dpi(int value) { return ::MulDiv(value, g_dpi, 96); }

HFONT FontNormal() {
    static HFONT f = nullptr;
    if (!f) f = MakeFont(9, FW_NORMAL, L"Microsoft YaHei UI");
    return f;
}

HFONT FontBold() {
    static HFONT f = nullptr;
    if (!f) f = MakeFont(9, FW_SEMIBOLD, L"Microsoft YaHei UI");
    return f;
}

HFONT FontTitle() {
    static HFONT f = nullptr;
    if (!f) f = MakeFont(12, FW_SEMIBOLD, L"Microsoft YaHei UI");
    return f;
}

HFONT FontMono() {
    static HFONT f = nullptr;
    if (!f) f = MakeFont(9, FW_NORMAL, L"Consolas");
    return f;
}

HBRUSH Brush(COLORREF c) {
    if (!g_brushes) g_brushes = new std::unordered_map<COLORREF, HBRUSH>();
    auto it = g_brushes->find(c);
    if (it != g_brushes->end()) return it->second;
    HBRUSH b = ::CreateSolidBrush(c);
    (*g_brushes)[c] = b;
    return b;
}

void FillRectColor(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH b = Brush(c);
    ::FillRect(dc, &rc, b);
}

void DrawBorder(HDC dc, const RECT& rc, COLORREF c) {
    RECT r = rc;
    HBRUSH b = Brush(c);
    ::FrameRect(dc, &r, b);
}

void DrawSectionTitle(HDC dc, const RECT& rc, const wchar_t* text) {
    RECT r = rc;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kTextDim);
    HFONT old = static_cast<HFONT>(::SelectObject(dc, FontBold()));
    ::DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);
}

void DrawPill(HDC dc, const RECT& rc, const wchar_t* text, COLORREF fg, COLORREF bg) {
    HBRUSH b = Brush(bg);
    HBRUSH oldB = static_cast<HBRUSH>(::SelectObject(dc, b));
    HPEN nullPen = static_cast<HPEN>(::SelectObject(dc, ::GetStockObject(NULL_PEN)));
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, Dpi(10), Dpi(10));
    ::SelectObject(dc, oldB);
    ::SelectObject(dc, nullPen);

    RECT r = rc;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, fg);
    HFONT oldF = static_cast<HFONT>(::SelectObject(dc, FontNormal()));
    ::DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, oldF);
}

}  // namespace theme
}  // namespace adbgui
