// ---------------------------------------------------------------------------
// 内置命令分派：把命令表里 ExecKind::Internal 的条目接到具体窗口
// ---------------------------------------------------------------------------
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "../core/command_spec.h"
#include "../core/device.h"
#include "../core/settings.h"
#include "gui.h"
#include "reports.h"
#include "resource.h"
#include "ui_util.h"
#include "version.h"

namespace adbgui {

namespace {

// 设备选择：把当前设备的选定结果写回主窗口
void ShowDeviceChooser(HWND parent) {
    auto& session = adb::DeviceSession::Instance();
    auto devices = session.ListAdb();
    if (devices.empty()) {
        ShowTextView(parent, L"选择设备", L"没有检测到设备。");
        return;
    }
    std::wstring text = L"当前设备：" +
                        (session.CurrentSerial().empty() ? std::wstring(L"（未选择）")
                                                         : session.CurrentSerial()) +
                        L"\r\n\r\n可用设备：\r\n";
    for (size_t i = 0; i < devices.size(); ++i) {
        text += adb::Format(L"  %zu. %ls\r\n", i + 1, devices[i].Describe().c_str());
    }
    text += L"\r\n在主窗口顶部的下拉框里切换即可（选中即生效并记住）。";
    ShowTextView(parent, L"选择设备", text);
}

void ShowAbout(HWND parent) {
    ::MessageBoxW(parent, BuildAboutText().c_str(), L"关于 AdbRT", MB_OK | MB_ICONINFORMATION);
}

void OpenToolsFolder(HWND parent) {
    std::wstring adbPath = adb::DeviceSession::Instance().AdbExe();
    std::wstring dir = adbPath.empty() ? adb::GetExeDir() : adb::ParentOf(adbPath);
    if (!adb::PathIsDirectory(dir)) {
        ::MessageBoxW(parent, L"找不到 platform-tools 目录。请在「设置」里指定 adb 路径。",
                      L"AdbRT", MB_OK | MB_ICONWARNING);
        return;
    }
    ::ShellExecuteW(parent, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RunDoctor(HWND parent) {
    ::MessageBoxW(parent, BuildDoctorText().c_str(), L"AdbRT 自检", MB_OK | MB_ICONINFORMATION);
}

}  // namespace

bool RunBuiltinCommand(HWND hwnd, const adb::CommandSpec& spec, std::wstring* error) {
    std::string key = spec.key;

    if (key == "device.list")          { ShowDeviceList(hwnd);       return true; }
    if (key == "device.select")        { ShowDeviceChooser(hwnd);    return true; }
    if (key == "device.info")          { ShowDeviceInfo(hwnd);       return true; }
    if (key == "app.list")             { ShowInstalledApps(hwnd);    return true; }
    if (key == "app.pullApk")          { ShowPullApk(hwnd);          return true; }
    if (key == "screen.capture")       { CaptureScreenshot(hwnd);    return true; }
    if (key == "logcat.live")          { ShowLogcat(hwnd);           return true; }
    if (key == "shell.terminal")       { OpenInteractiveShell(hwnd); return true; }
    if (key == "tool.settings")        { ShowSettings(hwnd);         return true; }
    if (key == "tool.about")           { ShowAbout(hwnd);            return true; }
    if (key == "tool.doctor")          { RunDoctor(hwnd);            return true; }
    if (key == "tool.openToolsDir")    { OpenToolsFolder(hwnd);      return true; }

    if (error) *error = std::wstring(L"内置功能「") + adb::CommandTitle(spec) + L"」尚未实现。";
    return false;
}

}  // namespace adbgui