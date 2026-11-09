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

    if (!spec) return;

    int y = 0;
    const int left = 8;
    const int width = 420;

    // ---- 参数（按 kind 决定控件） ----
    if (spec->param != adb::ParamKind::None && spec->paramKey) {
        std::wstring label;
        switch (spec->param) {
            case adb::ParamKind::ApkFile:     label = L"APK 文件"; break;
            case adb::ParamKind::LocalPaths:  label = L"本地路径"; break;
            case adb::ParamKind::RemotePath:  label = L"设备路径（如 /sdcard/Download）"; break;
            case adb::ParamKind::PackageName: label = L"应用包名"; break;
            case adb::ParamKind::Partition:   label = L"分区名"; break;
            case adb::ParamKind::ImageFile:   label = L"镜像文件"; break;
            case adb::ParamKind::HostPort:    label = L"地址（IP:端口）"; break;
            case adb::ParamKind::Port:        label = L"端口"; break;
            case adb::ParamKind::ShellCommand:label = L"命令"; break;
            case adb::ParamKind::KeyCode:     label = L"按键"; break;
            case adb::ParamKind::SlotName:    label = L"槽位（a / b）"; break;
            default:                          label = adb::Utf8ToWide(spec->paramKey); break;
        }
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
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BROWSE_ADB)),
                                        nullptr, nullptr);
        ApplyFont(browse, g_app.font);
        (void)browse;
        y += 32;
    }

    // ---- flags ----
    for (uint8_t i = 0; i < spec->flagCount; ++i) {
        const adb::FlagSpec& f = spec->flags[i];
        if (f.kind == adb::FlagKind::Value) {
            MakeLabel(host, adb::Utf8ToWide(f.key), left, y, width, 18);
            y += 20;
            HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                          left, y, width, 24, host, nullptr, nullptr, nullptr);
            ApplyFont(edit, g_app.fontMono);
            g_flags.push_back({edit, f.key, adb::ParamKind::None, f.kind});
            y += 30;
        } else {
            HWND chk = ::CreateWindowExW(0, L"BUTTON", adb::Utf8ToWide(f.key).c_str(),
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