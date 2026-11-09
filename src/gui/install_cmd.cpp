// ---------------------------------------------------------------------------
// 安装 / 卸载（**不写注册表**）
//   AdbRT.exe --install [安装目录]     铺文件由 PowerShell 完成，这里只做：
//                                      ① 把安装目录写进「用户 PATH」
//                                      ② 创建桌面快捷方式（.lnk）
//   AdbRT.exe --uninstall              ① 从用户 PATH 移除
//                                      ② 删除桌面快捷方式
//
// 说明：用户 PATH 在注册表 HKCU\Environment 里，但那是**环境变量**而不是
//       "程序写注册表条目"——这是用户明确要求的唯一一处系统状态。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

#include "../core/core.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "version.h"

namespace {

std::wstring GetDesktopDir() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) out = path;
    if (path) ::CoTaskMemFree(path);
    return out;
}

bool CreateDesktopShortcut(const std::wstring& targetExe, const std::wstring& workDir,
                           const std::wstring& args) {
    std::wstring desktop = GetDesktopDir();
    if (desktop.empty()) return false;
    std::wstring linkPath = adb::JoinPath(desktop, L"AdbRT.lnk");

    HRESULT hrInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        if (SUCCEEDED(hrInit)) ::CoUninitialize();
        return false;
    }

    link->SetPath(targetExe.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetArguments(args.c_str());
    link->SetDescription(L"AdbRT - Android 平台工具图形外壳");
    link->SetIconLocation(targetExe.c_str(), 0);

    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) && file) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    if (SUCCEEDED(hrInit)) ::CoUninitialize();
    return ok;
}

void RemoveDesktopShortcut() {
    std::wstring desktop = GetDesktopDir();
    if (desktop.empty()) return;
    ::DeleteFileW(adb::JoinPath(desktop, L"AdbRT.lnk").c_str());
}

}  // namespace

namespace adbgui {

// 返回进程退出码
int RunInstall(const std::wstring& installDir, bool addPath, bool desktopShortcut, bool quiet) {
    std::wstring dir = adb::ResolvePathSpec(installDir, adb::GetExeDir());
    if (dir.empty()) dir = adb::GetExeDir();

    std::wstring report;
    report += L"安装目录：" + dir + L"\r\n";

    if (addPath) {
        bool ok = adb::AddToUserPath(dir);
        report += ok ? L"已加入用户 PATH\r\n" : L"加入用户 PATH 失败\r\n";
    } else {
        report += L"已跳过 PATH（--no-path）\r\n";
    }

    if (desktopShortcut) {
        std::wstring exe = adb::JoinPath(dir, L"AdbRT.exe");
        bool ok = CreateDesktopShortcut(exe, dir, L"");
        report += ok ? L"已创建桌面快捷方式\r\n" : L"创建桌面快捷方式失败\r\n";
    }
    report += L"（未写入任何注册表项）\r\n";

    if (!quiet) ::MessageBoxW(nullptr, report.c_str(), L"AdbRT 安装", MB_OK | MB_ICONINFORMATION);
    return 0;
}

int RunUninstall(bool removePath, bool desktopShortcut, bool quiet) {
    std::wstring dir = adb::GetExeDir();
    std::wstring report;
    if (removePath) {
        bool ok = adb::RemoveFromUserPath(dir);
        report += ok ? L"已从用户 PATH 移除\r\n" : L"从用户 PATH 移除失败\r\n";
    }
    if (desktopShortcut) {
        RemoveDesktopShortcut();
        report += L"已删除桌面快捷方式\r\n";
    }
    report += L"（未改动任何注册表项）\r\n";
    if (!quiet) ::MessageBoxW(nullptr, report.c_str(), L"AdbRT 卸载", MB_OK | MB_ICONINFORMATION);
    return 0;
}

}  // namespace adbgui