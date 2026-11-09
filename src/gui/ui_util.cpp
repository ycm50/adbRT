#include "ui_util.h"

#include "../core/core.h"

namespace adbgui {

HFONT CreateUiFont(int pt, bool mono) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, 96, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    const wchar_t* face = mono ? L"Consolas" : L"Microsoft YaHei UI";
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return ::CreateFontIndirectW(&lf);
}

void ApplyFont(HWND control, HFONT font) {
    if (control && font) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::wstring GetTextOf(HWND control) {
    if (!control) return {};
    int len = ::GetWindowTextLengthW(control);
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(control, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    return buf;
}

void SetTextOf(HWND control, const std::wstring& text) {
    if (control) ::SetWindowTextW(control, text.c_str());
}

void AppendTextToEdit(HWND edit, const std::wstring& text) {
    if (!edit || text.empty()) return;
    int len = ::GetWindowTextLengthW(edit);
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void ScrollToEnd(HWND edit) {
    if (!edit) return;
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
    ::SendMessageW(edit, EM_SETSEL, -1, -1);
}

int ScaleForDpi(HWND hwnd, int value) {
    HDC dc = ::GetDC(hwnd);
    int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ::ReleaseDC(hwnd, dc);
    return ::MulDiv(value, dpi, 96);
}

std::wstring GetDpiScaledFontName() { return L"Microsoft YaHei UI"; }

}  // namespace adbgui
