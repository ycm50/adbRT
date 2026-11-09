#include "reports.h"

#include <windows.h>

#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "version.h"

namespace adbgui {

std::wstring BuildDeviceListText() {
    auto devices = adb::DeviceSession::Instance().ListAdb();
    if (devices.empty()) {
        return L"没有检测到设备。\r\n\r\n请确认：\r\n"
               L"  1. USB 线已连接，手机上开启「开发者选项 → USB 调试」\r\n"
               L"  2. 手机弹出的「允许 USB 调试」已点允许\r\n"
               L"  3. 已安装对应厂商的 USB 驱动\r\n"
               L"  4. 无线调试时电脑与手机在同一网络\r\n";
    }
    std::wstring text = adb::Format(L"共 %zu 台设备：\r\n\r\n", devices.size());
    for (const auto& d : devices) {
        text += L"序列号   : " + d.serial + L"\r\n";
        text += L"状态     : " + std::wstring(adb::StateName(d.state)) + L"\r\n";
        if (!d.product.empty())     text += L"product  : " + d.product + L"\r\n";
        if (!d.model.empty())       text += L"model    : " + d.model + L"\r\n";
        if (!d.device.empty())      text += L"device   : " + d.device + L"\r\n";
        if (!d.transportId.empty()) text += L"transport: " + d.transportId + L"\r\n";
        text += L"\r\n";
    }
    return text;
}

std::wstring BuildDeviceInfoText() {
    auto& session = adb::DeviceSession::Instance();
    std::wstring serial = session.CurrentSerial();
    if (serial.empty()) return L"没有选中设备。请先连接设备并点「刷新设备」。\r\n";

    // ★ 一次 adb shell 拿全：无线 adb 每次往返 ~700ms，
    //   分成 14 次就是十几秒；合成一次往返 + 设备侧只调一次 getprop 最快。
    // 用 "key=值" 的形态输出：比解析 getprop 原生的 "[k]: [v]" 更稳，也更省设备侧开销
    const wchar_t* kOneShot =
        L"echo '##prop'; "
        L"for p in ro.product.brand ro.product.model ro.build.version.release "
        L"ro.build.version.sdk ro.product.cpu.abi ro.build.type ro.debuggable "
        L"ro.secure ro.boot.slot_suffix ro.serialno; do echo \"$p=$(getprop $p)\"; done; "
        L"echo '##wm'; wm size; wm density; "
        L"echo '##df'; df -h /data; "
        L"echo '##mem'; head -n 3 /proc/meminfo";

    uint64_t t0 = ::GetTickCount64();
    std::wstring raw = session.Shell(kOneShot, 25000);
    adb::LogF(adb::LogLevel::Info, "devinfo", L"单次往返取全部信息 -> %llu ms",
              static_cast<unsigned long long>(::GetTickCount64() - t0));

    // 按段落切开
    auto section = [&](const wchar_t* tag) -> std::wstring {
        size_t b = raw.find(tag);
        if (b == std::wstring::npos) return {};
        b = raw.find(L'\n', b);
        if (b == std::wstring::npos) return {};
        ++b;
        size_t e = raw.size();
        for (const wchar_t* next : {L"##wm", L"##df", L"##mem"}) {
            size_t p = raw.find(next, b);
            if (p != std::wstring::npos && p < e) e = p;
        }
        return raw.substr(b, e - b);
    };

    std::wstring props = adb::Trim(section(L"##prop"));
    std::wstring wm = adb::Trim(section(L"##wm"));
    std::wstring df = adb::Trim(section(L"##df"));
    std::wstring mem = adb::Trim(section(L"##mem"));

    // props 行形如 "ro.product.model=AOSP on ARM64"
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    for (const auto& l : adb::SplitLines(props)) {
        std::wstring t = adb::Trim(l);
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        kv.emplace_back(t.substr(0, eq), adb::Trim(t.substr(eq + 1)));
    }
    auto propOf = [&kv](const wchar_t* key, const wchar_t* def) -> std::wstring {
        for (const auto& p : kv)
            if (p.first == key && !p.second.empty()) return p.second;
        return def;
    };

    std::wstring text = L"设备：" + serial + L"\r\n\r\n";
    auto line = [&](const wchar_t* label, const wchar_t* key) {
        text += adb::Format(L"%-14ls %ls\r\n", label, propOf(key, L"（未知）").c_str());
    };
    line(L"厂商", L"ro.product.brand");
    line(L"型号", L"ro.product.model");
    line(L"Android 版本", L"ro.build.version.release");
    line(L"SDK 版本", L"ro.build.version.sdk");
    line(L"CPU 架构", L"ro.product.cpu.abi");
    line(L"构建类型", L"ro.build.type");
    line(L"可调试", L"ro.debuggable");
    line(L"Secure", L"ro.secure");
    line(L"当前槽位", L"ro.boot.slot_suffix");
    line(L"序列号", L"ro.serialno");
    if (!wm.empty())
        text += adb::Format(L"%-14ls %ls\r\n", L"屏幕", wm.c_str());
    if (!df.empty()) text += L"\r\n存储：\r\n" + df + L"\r\n";
    if (!mem.empty()) text += L"\r\n内存：\r\n" + mem + L"\r\n";
    return text;
}

std::wstring BuildAppListText(bool thirdPartyOnly) {
    auto& session = adb::DeviceSession::Instance();
    if (session.CurrentSerial().empty()) return L"没有选中设备。\r\n";
    auto pkgs = session.InstalledPackages(!thirdPartyOnly);
    std::wstring text = adb::Format(L"%ls 共 %zu 个应用：\r\n\r\n",
                                    thirdPartyOnly ? L"第三方" : L"全部", pkgs.size());
    for (const auto& p : pkgs) text += p.package + L"\r\n";
    return text;
}

std::wstring BuildDoctorText() {
    auto& st = adb::Settings::Instance();
    st.Reload(true);
    std::wstring text;
    text += L"AdbRT " + adb::W(ADB_VERSION_STR) + L"\r\n\r\n";
    text += L"adb 配置 : " + adb::DescribePathSpec(st.AdbSpec()) + L"\r\n";
    text += L"fastboot : " + adb::DescribePathSpec(st.FastbootSpec()) + L"\r\n";
    text += L"配置文件 : " + st.Path() + L"\r\n";
    text += L"程序目录 : " + adb::GetExeDir() + L"\r\n";
    text += L"日志文件 : " + adb::LogFilePath() + L"\r\n\r\n";

    auto& session = adb::DeviceSession::Instance();
    std::wstring adbPath = session.AdbExe();
    std::wstring fbPath = session.FastbootExe();
    text += L"解析到 adb : " + (adbPath.empty() ? std::wstring(L"（未找到）") : adbPath) + L"\r\n";
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
    return text;
}

std::wstring BuildAboutText() {
    std::wstring text = L"AdbRT ";
    text += adb::W(ADB_VERSION_STR);
    text += L"\r\n\r\nAndroid 平台工具（adb / fastboot）的 Windows 图形外壳。\r\n\r\n";
    text += L"· 配置：%APPDATA%\\AdbRT\\config.json\r\n";
    text += L"· 程序目录：" + adb::GetExeDir() + L"\r\n";
    text += L"· 日志：" + adb::LogFilePath() + L"\r\n\r\n";
    text += L"不写注册表；安装只改用户 PATH 与桌面快捷方式。\r\nMIT License";
    return text;
}

}  // namespace adbgui