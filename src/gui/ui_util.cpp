#include "ui_util.h"

#include <map>
#include <mutex>

#include "../core/core.h"
#include "theme.h"

namespace adbgui {

namespace {

// 字体缓存：CreateUiFont 在每次开窗时都会被调用，
// 原来每次都 CreateFontIndirectW 且从不删除 —— 用久了 GDI 对象会一直涨。
// 字号 + 字体族是有限组合，缓存起来既省句柄也省时间。
struct FontKey {
    int  pt = 0;
    bool mono = false;
    bool operator<(const FontKey& o) const {
        if (pt != o.pt) return pt < o.pt;
        return mono < o.mono;
    }
};

std::map<FontKey, HFONT>& Fonts() {
    static std::map<FontKey, HFONT> m;
    return m;
}
std::mutex& FontMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

HFONT CreateUiFont(int pt, bool mono) {
    {
        std::lock_guard<std::mutex> lk(FontMutex());
        auto it = Fonts().find(FontKey{pt, mono});
        if (it != Fonts().end()) return it->second;
    }

    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(pt, theme::Dpi(96), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    const wchar_t* face = mono ? L"Consolas" : L"Microsoft YaHei UI";
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    HFONT f = ::CreateFontIndirectW(&lf);

    std::lock_guard<std::mutex> lk(FontMutex());
    Fonts()[FontKey{pt, mono}] = f;
    return f;
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

std::wstring Trim(const std::wstring& s) { return adb::Trim(s); }

}  // namespace adbgui