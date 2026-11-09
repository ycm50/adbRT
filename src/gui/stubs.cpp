// ---------------------------------------------------------------------------
// 内置命令分派：把命令表里 ExecKind::Internal 的条目接到具体窗口
// ---------------------------------------------------------------------------
#include <windows.h>

#include <string>

#include "../core/command_spec.h"
#include "gui.h"

namespace adbgui {

bool RunBuiltinCommand(HWND hwnd, const adb::CommandSpec& spec, std::wstring* error) {
    std::string key = spec.key;

    if (key == "device.list")        { ShowDeviceList(hwnd);      return true; }
    if (key == "device.info")        { ShowDeviceInfo(hwnd);      return true; }
    if (key == "app.list")           { ShowInstalledApps(hwnd);   return true; }
    if (key == "app.pullApk")        { ShowPullApk(hwnd);         return true; }
    if (key == "file.browse")        { ShowFileBrowser(hwnd);     return true; }
    if (key == "screen.capture")     { CaptureScreenshot(hwnd);   return true; }
    if (key == "logcat.live")        { ShowLogcat(hwnd);          return true; }
    if (key == "shell.terminal")     { OpenInteractiveShell(hwnd);return true; }
    if (key == "tool.settings")      { ShowSettings(hwnd);        return true; }

    if (error) *error = std::wstring(L"内置功能「") + adb::CommandTitle(spec) + L"」尚未实现。";
    return false;
}

}  // namespace adbgui