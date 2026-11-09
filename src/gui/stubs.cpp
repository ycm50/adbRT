// 尚未实现的界面部件：占位，保证链接通过（后续增量替换，
// 见 docs/技术实现设计.md "未实现" 一节。这里**不静默失败**，都给出明确提示。）
#include <windows.h>

#include <string>

#include "../core/command_spec.h"
#include "gui.h"

namespace adbgui {

void ShowSettings(HWND parent) {
    ::MessageBoxW(parent,
                  L"设置窗口尚未实现（增量的下一步）。\n"
                  L"现在可以直接编辑 %APPDATA%\\AdbRT\\config.json：\n"
                  L"  adb.path / fastboot.path / device.current / expert.fastboot",
                  L"AdbRT", MB_OK | MB_ICONINFORMATION);
}

bool ShowConfirmDialog(HWND parent, const std::wstring& title, const std::wstring& message) {
    return ::MessageBoxW(parent, message.c_str(), title.c_str(),
                         MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

bool ShowBreakerDialog(HWND parent, const std::wstring& title, const std::wstring& message,
                       const std::wstring& expectText) {
    std::wstring text = message + L"\n\n这是破坏性操作。请在弹出的提示中确认。";
    (void)expectText;
    return ::MessageBoxW(parent, text.c_str(), title.c_str(),
                         MB_OKCANCEL | MB_ICONERROR) == IDOK;
}

bool RunBuiltinCommand(HWND hwnd, const adb::CommandSpec& spec, std::wstring* error) {
    (void)hwnd;
    if (error) {
        *error = L"内置功能「" + adb::Utf8ToWide(spec.key) + L"」尚未实现（下一步）。";
    }
    return false;
}

}  // namespace adbgui
