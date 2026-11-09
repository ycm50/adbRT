// ---------------------------------------------------------------------------
// 对话框：设置（路径支持相对/绝对/环境变量）、普通确认、Breaker 级确认
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <string>

#include "../core/core.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

struct SettingsUi {
    HWND hAdb = nullptr;
    HWND hFastboot = nullptr;
    HWND hExpert = nullptr;
    HWND hPathEnv = nullptr;
    HWND hDesktop = nullptr;
    bool pathTouched = false;
    bool desktopTouched = false;
};

void FillSettings(HWND dlg, SettingsUi* ui) {
    auto& st = adb::Settings::Instance();
    st.Reload(true);
    SetTextOf(ui->hAdb, st.AdbSpec());
    SetTextOf(ui->hFastboot, st.FastbootSpec());
    ::SendMessageW(ui->hExpert, BM_SETCHECK, st.ExpertFastboot() ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(ui->hPathEnv, BM_SETCHECK, IsInUserPath(adb::GetExeDir()) ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(ui->hDesktop, BM_SETCHECK, DesktopShortcutExists() ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SetWindowTextW(dlg, L"AdbRT 设置");
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<SettingsUi*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ui = reinterpret_cast<SettingsUi*>(lp);
            ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
            ui->hAdb = ::GetDlgItem(dlg, IDC_EDIT_ADB);
            ui->hFastboot = ::GetDlgItem(dlg, IDC_EDIT_FASTBOOT);
            ui->hExpert = ::GetDlgItem(dlg, IDC_CHK_EXPERT);
            ui->hPathEnv = ::GetDlgItem(dlg, IDC_CHK_PATHENV);
            ui->hDesktop = ::GetDlgItem(dlg, IDC_CHK_DESKTOP);
            HFONT f = CreateUiFont(9, false);
            for (HWND c : {ui->hAdb, ui->hFastboot, ui->hExpert, ui->hPathEnv, ui->hDesktop})
                ApplyFont(c, f);
            FillSettings(dlg, ui);
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_BTN_BROWSE_ADB || id == IDC_BTN_BROWSE_FB) {
                std::wstring picked = AskFile(dlg, L"选择可执行文件",
                                              L"可执行文件\0*.exe\0所有文件\0*.*\0");
                if (!picked.empty()) {
                    std::wstring spec = adb::MakeRelativeTo(adb::GetExeDir(), picked);
                    if (spec.empty()) spec = picked;      // 不在程序目录下 → 存绝对路径
                    SetTextOf(id == IDC_BTN_BROWSE_ADB ? ui->hAdb : ui->hFastboot, spec);
                }
                return TRUE;
            }
            if (id == IDC_CHK_PATHENV) { ui->pathTouched = true; return TRUE; }
            if (id == IDC_CHK_DESKTOP) { ui->desktopTouched = true; return TRUE; }
            if (id == IDC_BTN_SAVE || id == IDOK) {
                auto& st = adb::Settings::Instance();
                st.SetAdbSpec(adbgui::GetTextOf(ui->hAdb));
                st.SetFastbootSpec(adbgui::GetTextOf(ui->hFastboot));
                st.SetExpertFastboot(::SendMessageW(ui->hExpert, BM_GETCHECK, 0, 0) == BST_CHECKED);

                // PATH / 桌面快捷方式：只在用户动过勾选时改，避免"打开又关掉"就改了系统
                if (ui->pathTouched) {
                    bool want = ::SendMessageW(ui->hPathEnv, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (want) adb::AddToUserPath(adb::GetExeDir());
                    else adb::RemoveFromUserPath(adb::GetExeDir());
                }
                if (ui->desktopTouched) {
                    bool want = ::SendMessageW(ui->hDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SetDesktopShortcut(want);
                }
                if (!st.Save()) {
                    ::MessageBoxW(dlg, L"配置写入失败：%APPDATA%\\AdbRT\\config.json 不可写。",
                                  L"AdbRT", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                ::EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDC_BTN_CANCEL2 || id == IDCANCEL) {
                ::EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }
        case WM_CLOSE:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

struct ConfirmUi {
    const std::wstring* expect = nullptr;
    std::wstring* body = nullptr;
};

INT_PTR CALLBACK ConfirmProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<ConfirmUi*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
        case WM_INITDIALOG: {
            ui = reinterpret_cast<ConfirmUi*>(lp);
            ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
            SetTextOf(::GetDlgItem(dlg, IDC_STATIC_CONFIRM), *ui->body);
            SetTextOf(::GetDlgItem(dlg, IDC_STATIC_PATH), *ui->expect);
            HFONT f = CreateUiFont(9, false);
            for (HWND c : {::GetDlgItem(dlg, IDC_STATIC_CONFIRM), ::GetDlgItem(dlg, IDC_STATIC_PATH),
                           ::GetDlgItem(dlg, IDC_EDIT_CONFIRM), ::GetDlgItem(dlg, IDC_STATIC_CONFIRMTIP)})
                ApplyFont(c, f);
            ::SetFocus(::GetDlgItem(dlg, IDC_EDIT_CONFIRM));
            return FALSE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDOK) {
                std::wstring typed = adbgui::Trim(adbgui::GetTextOf(::GetDlgItem(dlg, IDC_EDIT_CONFIRM)));
                if (!adb::EqualsNoCase(typed, *ui->expect)) {
                    ::MessageBoxW(dlg, L"确认词不匹配，操作已取消。", L"AdbRT",
                                  MB_OK | MB_ICONWARNING);
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                }
                ::EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) { ::EndDialog(dlg, IDCANCEL); return TRUE; }
            return FALSE;
        }
        case WM_CLOSE: ::EndDialog(dlg, IDCANCEL); return TRUE;
        default: break;
    }
    return FALSE;
}

}  // namespace

void ShowSettings(HWND parent) {
    SettingsUi ui;
    ::DialogBoxParamW(reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                      MAKEINTRESOURCEW(IDD_SETTINGS), parent, SettingsProc,
                      reinterpret_cast<LPARAM>(&ui));
}

bool ShowConfirmDialog(HWND parent, const std::wstring& title, const std::wstring& message) {
    return ::MessageBoxW(parent, message.c_str(), title.c_str(),
                         MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

bool ShowBreakerDialog(HWND parent, const std::wstring& title, const std::wstring& message,
                       const std::wstring& expectText) {
    ConfirmUi ui;
    std::wstring body = message + L"\r\n\r\n这一步可能让设备无法开机，请确认你刷入的是正确镜像。";
    ui.expect = &expectText;
    ui.body = &body;
    INT_PTR rc = ::DialogBoxParamW(
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
        MAKEINTRESOURCEW(IDD_CONFIRM), parent, ConfirmProc, reinterpret_cast<LPARAM>(&ui));
    ::SetWindowTextW(parent, L"AdbRT");   // 标题可能被 EndDialog 留成"确认"
    (void)title;
    return rc == IDOK;
}

}  // namespace adbgui
