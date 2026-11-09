// ---------------------------------------------------------------------------
// AdbRT 入口：gui（默认）/ --selftest / --version / --help / --adb <path>
//   · 不写注册表；配置全部在 %APPDATA%\AdbRT\config.json
//   · 支持被右键菜单以外的任何方式启动（命令行、快捷方式）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/device.h"
#include "../core/settings.h"
#include "gui.h"
#include "version.h"

namespace adbgui {
AppState g_app;
}

static void ShowUsage() {
    std::wstring msg;
    msg += L"AdbRT " + std::wstring(adb::W(ADB_VERSION_STR)) + L"\r\n\r\n";
    msg += L"用法：\r\n";
    msg += L"  AdbRT.exe                     打开主窗口\r\n";
    msg += L"  AdbRT.exe <文件或目录>         打开主窗口并带入该路径\r\n";
    msg += L"  AdbRT.exe --selftest <文件>    跑无窗口自检，把报告写到文件\r\n";
    msg += L"  AdbRT.exe --adb <路径>         临时指定 adb 路径\r\n";
    msg += L"  AdbRT.exe --version            显示版本\r\n";
    ::MessageBoxW(nullptr, msg.c_str(), L"AdbRT", MB_OK | MB_ICONINFORMATION);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int showCmd) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    adb::Settings::Instance().Reload(true);
    adb::LogInit({}, adb::LogLevel::Info);

    adbgui::AppContext ctx;
    std::wstring selftestPath;
    std::wstring tempAdb;
    bool doctorMode = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--version") {
            ::MessageBoxW(nullptr, (std::wstring(L"AdbRT ") + adb::W(ADB_VERSION_STR)).c_str(),
                          L"AdbRT", MB_OK);
            if (argv) ::LocalFree(argv);
            return 0;
        }
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            ShowUsage();
            if (argv) ::LocalFree(argv);
            return 0;
        }
        if (a == L"--selftest" && i + 1 < argc) { selftestPath = argv[++i]; continue; }
        if (a == L"--doctor") { doctorMode = true; continue; }
        if (a == L"--install") {
            std::wstring dir = (i + 1 < argc && argv[i + 1][0] != L'-') ? argv[++i] : adb::GetExeDir();
            bool addPath = true, desktop = true, quiet = false;
            for (int k = i + 1; k < argc; ++k) {
                std::wstring f = argv[k];
                if (f == L"--no-path") addPath = false;
                if (f == L"--no-desktop") desktop = false;
                if (f == L"--quiet") quiet = true;
            }
            if (argv) ::LocalFree(argv);
            return adbgui::RunInstall(dir, addPath, desktop, quiet);
        }
        if (a == L"--uninstall") {
            bool removePath = true, desktop = true, quiet = false;
            for (int k = i + 1; k < argc; ++k) {
                std::wstring f = argv[k];
                if (f == L"--keep-path") removePath = false;
                if (f == L"--keep-desktop") desktop = false;
                if (f == L"--quiet") quiet = true;
            }
            if (argv) ::LocalFree(argv);
            return adbgui::RunUninstall(removePath, desktop, quiet);
        }
        if (a == L"--adb" && i + 1 < argc) { tempAdb = argv[++i]; continue; }
        if (a == L"--open" && i + 1 < argc) {
            ctx.initialCommand = adb::WideToUtf8(argv[++i]);
            continue;
        }
        if (!a.empty() && a[0] != L'-') {
            ctx.initialPath = a;
            std::wstring lower = adb::ToLowerAscii(adb::FileNameOf(a));
            if (adb::EndsWith(lower, L".apk")) ctx.fromApk = true;
        }
    }
    if (argv) ::LocalFree(argv);

    // --adb 只临时生效，不落盘（避免测桩把用户配置改掉）
    if (!tempAdb.empty()) adb::SetTransientAdbOverride(tempAdb);

    if (doctorMode) {
        ctx.initialCommand = "tool.doctor";
    }

    if (!selftestPath.empty()) {
        int rc = adbgui::RunSelfTest(selftestPath);
        return rc;
    }

    return adbgui::RunApp(hinst, ctx, showCmd);
}