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
    std::wstring text = L"AdbRT ";
    text += adb::W(ADB_VERSION_STR);
    text += L"\r\n\r\nAndroid 平台工具（adb / fastboot）的 Windows 图形外壳。\r\n\r\n";
    text += L"· 配置：%APPDATA%\\AdbRT\\config.json\r\n";
    text += L"· 程序目录：";
    text += adb::GetExeDir();
    text += L"\r\n· 日志：";
    text += adb::LogFilePath();
    text += L"\r\n\r\n不写注册表；安装只改用户 PATH 与桌面快捷方式。\r\nMIT License";
    ::MessageBoxW(parent, text.c_str(), L"关于 AdbRT", MB_OK | MB_ICONINFORMATION);
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
    auto& st = adb::Settings::Instance();
    st.Reload(true);
    std::wstring text;
    text += L"AdbRT " + adb::W(ADB_VERSION_STR) + L"\r\n\r\n";
    text += L"adb 配置 : " + adb::DescribePathSpec(st.AdbSpec()) + L"\r\n";
    text += L"fastboot : " + adb::DescribePathSpec(st.FastbootSpec()) + L"\r\n";
    text += L"配置文件 : " + st.Path() + L"\r\n";
    text += L"程序目录 : " + adb::GetExeDir() + L"\r\n\r\n";

    auto& session = adb::DeviceSession::Instance();
    std::wstring adbPath = session.AdbExe();
    text += L"解析到 adb : " + (adbPath.empty() ? std::wstring(L"（未找到）") : adbPath) + L"\r\n";
    std::wstring fbPath = session.FastbootExe();
    text += L"解析到 fastboot : " + (fbPath.empty() ? std::wstring(L"（未找到）") : fbPath) + L"\r\n";

    if (!adbPath.empty()) {
        adb::RunOptions o;
        o.timeoutMs = 10000;
        auto v = adb::RunProcess(adbPath, {L"version"}, o);
        if (v.started) {
            std::wstring line = adb::Trim(v.OutW());
            size_t nl = line.find(L'\n');
            text += L"adb 版本 : " + (nl == std::wstring::npos ? line : line.substr(0, nl)) + L"\r\n";
        }
        auto list = session.ListAdb();
        text += adb::Format(L"在线设备 : %zu 台\r\n", list.size());
        for (const auto& d : list) text += L"  - " + d.Describe() + L"\r\n";
    }
    text += L"\r\n用户 PATH 包含程序目录 : ";
    text += IsInUserPath(adb::GetExeDir()) ? L"是" : L"否";
    text += L"\r\n桌面快捷方式 : ";
    text += DesktopShortcutExists() ? L"存在" : L"不存在";

    ::MessageBoxW(parent, text.c_str(), L"AdbRT 自检", MB_OK | MB_ICONINFORMATION);
}

}  // namespace

bool RunBuiltinCommand(HWND hwnd, const adb::CommandSpec& spec, std::wstring* error) {
    std::string key = spec.key;

    if (key == "device.list")          { ShowDeviceList(hwnd);       return true; }
    if (key == "device.select")        { ShowDeviceChooser(hwnd);    return true; }
    if (key == "device.info")          { ShowDeviceInfo(hwnd);       return true; }
    if (key == "app.list")             { ShowInstalledApps(hwnd);    return true; }
    if (key == "app.pullApk")          { ShowPullApk(hwnd);          return true; }
    if (key == "file.browse")          { ShowFileBrowser(hwnd);      return true; }
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
