#include "ui_util.h"

#include <commctrl.h>
#include <windowsx.h>

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

void RelaxEditLimit(HWND edit) {
    if (!edit) return;
    // 0x7FFFFFF0 是 EDIT 能接受的最大值；用真正的大值而不是 0，
    // 避免某些控件把 0 解释成"恢复默认 30000"。
    ::SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(0x7FFFFFF0), 0);
}

void AppendTextToEdit(HWND edit, const std::wstring& text) {
    if (!edit || text.empty()) return;
    int len = ::GetWindowTextLengthW(edit);
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void ScrollToEnd(HWND edit) {
    if (!edit) return;
    // 不选中任何字符，只把光标移到末尾：原来的 EM_SETSEL 0,-1 会把整个
    // 结果框全选一遍，文本越长越慢（追加一条就全选一次 = O(n²)）。
    int len = ::GetWindowTextLengthW(edit);
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    ::SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void FollowTailIfAtBottom(HWND edit) {
    if (!edit) return;
    // 用户往上翻看历史时不要把他拽回底部：只在"本来就贴底"时跟随。
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!::GetScrollInfo(edit, SB_VERT, &si)) { ScrollToEnd(edit); return; }
    const int bottom = si.nMax - static_cast<int>(si.nPage) + 1;
    if (si.nPos >= bottom - 2) ScrollToEnd(edit);
}

bool TrimEditToBudget(HWND edit, size_t maxChars) {
    if (!edit || maxChars == 0) return false;
    const int len = ::GetWindowTextLengthW(edit);
    if (len <= 0 || static_cast<size_t>(len) <= maxChars) return false;

    // 直接裁到预算的 3/4：留出四分之一空间给后续追加，
    // 这样裁剪一次能撑很久，不会每来一行就裁一次。
    // （原来的写法只削"预算的四分之一"，文本远超预算时根本回不到预算内。）
    const size_t target = maxChars - maxChars / 4;
    const size_t cut = static_cast<size_t>(len) - target;

    std::wstring all(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(edit, all.data(), len + 1);
    all.resize(static_cast<size_t>(len));

    std::wstring keep = all.substr(cut);
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
    ::SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(keep.c_str()));
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(keep.size()),
                   static_cast<LPARAM>(keep.size()));
    return true;
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