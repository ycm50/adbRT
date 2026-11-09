#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <string>
#include <vector>

#include "../core/command_builder.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

struct CtrlRef {
    HWND hwnd = nullptr;
    std::string key;
    adb::ParamKind kind = adb::ParamKind::None;
    adb::FlagKind  flagKind = adb::FlagKind::Toggle;
};

std::vector<CtrlRef> g_params;
std::vector<CtrlRef> g_flags;
WNDPROC g_oldParamProc = nullptr;

// 参数区的"浏览…"按钮：需要它时按 kind 打开文件 / 目录对话框
void BrowseForParam(HWND host, adb::ParamKind kind, bool second) {
    if (g_params.empty()) return;
    HWND edit = (second && g_params.size() > 1) ? g_params[1].hwnd : g_params.front().hwnd;
    if (kind == adb::ParamKind::ImageFile) {
        std::wstring f = AskFile(host, L"选择镜像文件", L"镜像文件\0*.img;*.bin\0所有文件\0*.*\0");
        if (!f.empty()) SetTextOf(edit, f);
    } else if (kind == adb::ParamKind::LocalPaths) {
        std::wstring d = AskFolder(host, L"选择目录");
        if (!d.empty()) SetTextOf(edit, d);
    } else {
        std::wstring f = AskFile(host, L"选择文件", L"APK\0*.apk\0所有文件\0*.*\0");
        if (!f.empty()) SetTextOf(edit, f);
    }
}

LRESULT CALLBACK ParamHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED &&
        (LOWORD(wp) == IDC_BTN_ACTION1 || LOWORD(wp) == IDC_BTN_ACTION2)) {
        const bool second = LOWORD(wp) == IDC_BTN_ACTION2;
        adb::ParamKind kind = adb::ParamKind::ApkFile;
        if (g_app.current) kind = second ? g_app.current->param2 : g_app.current->param;
        BrowseForParam(hwnd, kind, second);
        return 0;
    }
    return ::CallWindowProcW(g_oldParamProc, hwnd, msg, wp, lp);
}

HWND MakeLabel(HWND host, const std::wstring& text, int x, int y, int w, int height) {
    HWND label = ::CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   x, y, w, height, host, nullptr, nullptr, nullptr);
    ApplyFont(label, g_app.font);
    return label;
}

}  // namespace

std::wstring AskFile(HWND parent, const wchar_t* title, const wchar_t* filter) {
    wchar_t buf[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!::GetOpenFileNameW(&ofn)) return {};
    return adb::NormalizePath(buf);
}

std::wstring AskFolder(HWND parent, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = parent;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return {};
    wchar_t path[MAX_PATH] = {0};
    bool ok = ::SHGetPathFromIDListW(idl, path) != FALSE;
    ::CoTaskMemFree(idl);
    return ok ? std::wstring(path) : std::wstring();
}

void BuildParamPanel(HWND host, const adb::CommandSpec* spec) {
    // 清空
    HWND child = ::GetWindow(host, GW_CHILD);
    while (child) {
        HWND next = ::GetWindow(child, GW_HWNDNEXT);
        ::DestroyWindow(child);
        child = next;
    }
    g_params.clear();
    g_flags.clear();

    if (!g_oldParamProc)
        g_oldParamProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(host, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ParamHostProc)));

    if (!spec) return;

    int y = 0;
    const int left = 8;
    const int width = 420;

    // ---- 参数（按 kind 决定控件） ----
    if (spec->param != adb::ParamKind::None && spec->paramKey) {
        std::wstring label = adb::ParamLabel(spec->param);
        MakeLabel(host, label, left, y, width, 18);
        y += 20;

        HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      left, y, width - 70, 24, host,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PARAM_HOST)),
                                      nullptr, nullptr);
        ApplyFont(edit, g_app.fontMono);
        g_params.push_back({edit, spec->paramKey, spec->param, adb::FlagKind::Value});

        HWND browse = ::CreateWindowExW(0, L"BUTTON", L"浏览…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        left + width - 64, y, 64, 24, host,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ACTION1)),
                                        nullptr, nullptr);
        ApplyFont(browse, g_app.font);
        y += 32;
    }

    // ---- 第二条参数（如 flash 的镜像文件） ----
    if (spec->param2 != adb::ParamKind::None && spec->paramKey2) {
        MakeLabel(host, adb::ParamLabel(spec->param2), left, y, width, 18);
        y += 20;
        HWND edit2 = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       left, y, width - 70, 24, host,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PARAM_HOST_2)),
                                       nullptr, nullptr);
        ApplyFont(edit2, g_app.fontMono);
        g_params.push_back({edit2, spec->paramKey2, spec->param2, adb::FlagKind::Value});
        HWND browse2 = ::CreateWindowExW(0, L"BUTTON", L"浏览…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         left + width - 64, y, 64, 24, host,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_ACTION2)),
                                         nullptr, nullptr);
        ApplyFont(browse2, g_app.font);
        y += 32;
    }

    // ---- flags ----
    for (uint8_t i = 0; i < spec->flagCount; ++i) {
        const adb::FlagSpec& f = spec->flags[i];
        std::wstring flagLabel = adb::FlagLabel(f);
        if (f.kind == adb::FlagKind::Value) {
            MakeLabel(host, flagLabel, left, y, width, 18);
            y += 20;
            HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          left, y, width, 24, host, nullptr, nullptr, nullptr);
            ApplyFont(edit, g_app.fontMono);
            g_flags.push_back({edit, f.key, adb::ParamKind::None, f.kind});
            y += 30;
        } else {
            HWND chk = ::CreateWindowExW(0, L"BUTTON", flagLabel.c_str(),
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         left, y, width, 22, host, nullptr, nullptr, nullptr);
            ApplyFont(chk, g_app.font);
            ::SendMessageW(chk, BM_SETCHECK, f.defaultOn ? BST_CHECKED : BST_UNCHECKED, 0);
            g_flags.push_back({chk, f.key, adb::ParamKind::None, f.kind});
            y += 26;
        }
    }
}

bool ReadParamPanel(adb::BuildInput* in, std::wstring* error) {
    (void)error;
    for (const auto& ref : g_params) {
        std::wstring v = adb::Trim(GetTextOf(ref.hwnd));
        in->params[ref.key] = v;
    }
    for (const auto& ref : g_flags) {
        if (ref.flagKind == adb::FlagKind::Value) {
            in->flags[ref.key] = adb::Trim(GetTextOf(ref.hwnd));
        } else {
            bool checked = ::SendMessageW(ref.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            in->flags[ref.key] = checked ? L"1" : L"0";
        }
    }
    return true;
}

}  // namespace adbgui