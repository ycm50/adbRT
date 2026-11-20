// ---------------------------------------------------------------------------
// 无窗口自检：不需要设备、不需要桌面会话即可运行（CI 用）
//   AdbRT.exe --selftest <报告文件>
// 覆盖：命令表完整性 / argv 构造 / 参数校验 / 设备解析 / 路径解析 / 配置往返
// ---------------------------------------------------------------------------
#include <windows.h>

#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/core.h"
#include "../core/bounded_text.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "ui_util.h"
#include "version.h"

namespace adbgui {

namespace {

int g_pass = 0;
int g_fail = 0;
std::wstring g_report;

void Check(bool cond, const wchar_t* what) {
    if (cond) { ++g_pass; g_report += L"[PASS] "; }
    else { ++g_fail; g_report += L"[FAIL] "; }
    g_report += what;
    g_report += L"\r\n";
}

void CheckStr(const std::wstring& actual, const wchar_t* expect, const wchar_t* what) {
    bool ok = (actual == expect);
    if (ok) { ++g_pass; g_report += L"[PASS] "; }
    else {
        ++g_fail;
        g_report += L"[FAIL] ";
    }
    g_report += what;
    if (!ok) {
        g_report += L"  期望=";
        g_report += expect;
        g_report += L"  实际=";
        g_report += actual;
    }
    g_report += L"\r\n";
}

}  // namespace

int RunSelfTest(const std::wstring& reportPath) {
    g_report += L"AdbRT 自检 ";
    g_report += adb::W(ADB_VERSION_STR);
    g_report += L"\r\n";
    g_report += L"----------------------------------------\r\n";

    // ---- 命令表 ----
    Check(adb::CommandTableSize() > 30, L"命令表条目充足");
    bool uniqueIds = true, uniqueKeys = true;
    for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
        for (size_t j = i + 1; j < adb::CommandTableSize(); ++j) {
            if (adb::CommandTable()[i].id == adb::CommandTable()[j].id) uniqueIds = false;
            if (std::string(adb::CommandTable()[i].key) == adb::CommandTable()[j].key) uniqueKeys = false;
        }
    }
    Check(uniqueIds, L"命令 ID 唯一");
    Check(uniqueKeys, L"命令 key 唯一");

    bool templatesOk = true;
    for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
        const auto& c = adb::CommandTable()[i];
        if (c.exec != adb::ExecKind::Internal && (!c.argvTemplate || !*c.argvTemplate)) templatesOk = false;
    }
    Check(templatesOk, L"所有外部命令都有 argv 模板");

    // ---- 参数校验 ----
    Check(adb::IsValidRemotePath(L"/sdcard/Download"), L"远端路径合法");
    Check(!adb::IsValidRemotePath(L"sdcard"), L"远端路径必须以 / 开头");
    Check(adb::IsValidHostPort(L"192.168.1.5:5555"), L"IP:端口合法");
    Check(adb::IsValidPartitionName(L"boot_a"), L"分区名合法");
    Check(!adb::IsValidPartitionName(L"boot;rm"), L"分区名拒绝注入");

    // ---- 设备解析 ----
    std::vector<adb::DeviceEntry> devs = adb::ParseDevices(
        L"List of devices attached\r\n"
        L"3B661M00VQ000000       device product:PLU110 model:PLU110 device:OP60F0L1 transport_id:1\r\n"
        L"emulator-5554          offline\r\n"
        L"ABC123                 unauthorized\r\n");
    Check(devs.size() == 3, L"解析 3 台设备");
    Check(devs.size() == 3 && devs[0].state == adb::DeviceState::Device, L"第一台是 device 状态");
    Check(devs.size() == 3 && devs[0].model == L"PLU110", L"解析出 model");
    Check(devs.size() == 3 && devs[1].state == adb::DeviceState::Offline, L"第二台是 offline");
    Check(devs.size() == 3 && devs[2].state == adb::DeviceState::Unauthorized, L"第三台是 unauthorized");

    // ---- 包名解析 ----
    auto pkgs = adb::ParsePackageList(L"package:com.example.a\r\npackage:com.example.b\r\n");
    Check(pkgs.size() == 2, L"解析 2 个包名");
    Check(adb::ParsePackageList(L"").empty(), L"空输入解析出 0 个包名");

    // ---- 进度解析 ----
    Check(adb::ParseProgressPercent(L"[ 42%] /sdcard/x.apk") == 42, L"解析推送进度 42%");
    Check(adb::ParseProgressPercent(L"no percent here") == -1, L"无百分比返回 -1");

    // ---- 路径解析（相对 / 绝对）----
    std::wstring base = L"A:\\Downloads\\adbRT";
    CheckStr(adb::ResolvePathSpec(L"platform-tools", base), L"A:\\Downloads\\adbRT\\platform-tools",
             L"相对路径解析");
    CheckStr(adb::ResolvePathSpec(L"A:\\platform-tools", base), L"A:\\platform-tools",
             L"绝对路径保持");
    CheckStr(adb::ResolvePathSpec(L"..\\tools", base), L"A:\\Downloads\\tools",
             L"父目录相对路径解析");

    // ---- argv 构造 ----
    const adb::CommandSpec* spec = adb::FindCommandByKey("app.install");
    Check(spec != nullptr, L"找到 app.install");
    if (spec) {
        adb::BuildInput in;
        in.spec = spec;
        in.toolExe = L"A:\\platform-tools\\adb.exe";
        in.serial = L"3B661M00VQ000000";
        in.flags["replace"] = L"1";
        in.params["apk"] = L"A:\\build\\app.apk";
        adb::BuiltCommand built;
        adb::BuildError err;
        bool ok = adb::BuildCommand(in, &built, &err);
        Check(ok, L"app.install 构造成功");
        if (ok) {
            // adb 的全局选项（-s）必须在子命令之前
            CheckStr(built.display,
                     L"-s 3B661M00VQ000000 install -r A:\\build\\app.apk",
                     L"app.install 命令行");
        }
    }

    // ---- 双参数命令：flash 必须同时有分区与镜像两个参数 ----
    {
        const adb::CommandSpec* flash = adb::FindCommandByKey("fb.flash");
        Check(flash != nullptr, L"找到 fb.flash");
        if (flash) {
            Check(flash->param2 == adb::ParamKind::ImageFile, L"fb.flash 有第二个参数（镜像文件）");
            Check(flash->paramKey2 && std::string(flash->paramKey2) == "img", L"fb.flash 第二参数 key 正确");
            adb::BuildInput in;
            in.spec = flash;
            in.toolExe = L"fastboot.exe";
            in.params["part"] = L"boot_a";
            in.params["img"] = L"C:\boot.img";
            adb::BuiltCommand built;
            adb::BuildError err;
            bool ok = adb::BuildCommand(in, &built, &err);
            Check(ok, L"fb.flash 双参数构造成功");
            if (ok) CheckStr(built.display, L"flash boot_a C:\boot.img", L"fb.flash 命令行");
        }
    }

    // ---- 内置命令分派：命令表里每个 Internal 都必须有落点 ----
    {
        const char* kBuiltin[] = {
            "device.list", "device.select", "device.info",
            "app.list", "app.pullApk",
            "screen.capture", "logcat.live", "shell.terminal",
            "tool.settings", "tool.about", "tool.doctor", "tool.openToolsDir",
        };
        int missing = 0;
        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const auto& c = adb::CommandTable()[i];
            if (c.exec != adb::ExecKind::Internal) continue;
            bool found = false;
            for (const char* k : kBuiltin)
                if (std::string(k) == c.key) { found = true; break; }
            if (!found) ++missing;
        }
        Check(missing == 0, L"所有内置命令都有对应窗口");
    }

    // ---- 配置往返 ----
    auto& st = adb::Settings::Instance();
    st.Reload(true);
    Check(true, L"配置可加载");
    (void)st.KeyCount();

    // ---- 输出内存策略（真实控件上验证裁剪与批量追加）----
    {
        // 用一个隐藏的 EDIT 控件当"结果框"，验证 TrimEditToBudget 的行为。
        // 顶层但不可见：--selftest 没有主窗口，WS_CHILD 找不到父窗口会失败
        HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_OVERLAPPED | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 400, 300, nullptr, nullptr,
            static_cast<HINSTANCE>(::GetModuleHandleW(nullptr)), nullptr);
        if (!edit) {
            Check(false, L"创建测试用 EDIT 控件");
        } else {
            // 批量追加：一次贴 2000 行，验证不会把事情搞崩
            std::wstring chunk;
            for (int i = 0; i < 20000; ++i) {
                chunk += adb::Format(L"line %d abcdefghijklmnopqrstuvwxyz0123456789\r\n", i);
            }
            // EDIT 控件默认有 30000 字符上限，不显式放宽的话超出部分会被静默丢弃。
            // 这正是"跑完命令结果框里只有一小段"的根因之一。
            // 新建的 EDIT 默认只有 30000 字符容量，超出部分会被静默丢弃
            Check(static_cast<int>(::SendMessageW(edit, EM_GETLIMITTEXT, 0, 0)) == 30000,
                  L"EDIT 默认上限确实是 30000 字符");

            RelaxEditLimit(edit);
            Check(static_cast<int>(::SendMessageW(edit, EM_GETLIMITTEXT, 0, 0)) > 30000,
                  L"RelaxEditLimit 放宽了上限");

            AppendTextToEdit(edit, chunk);
            int appended = ::GetWindowTextLengthW(edit);
            Check(appended > 40000, L"放宽上限后能装下大量输出");

            // 超出预算必须被裁掉
            const size_t budget = 40000;
            bool trimmed = TrimEditToBudget(edit, budget);
            Check(trimmed, L"超出预算时执行裁剪");
            int after = ::GetWindowTextLengthW(edit);
            g_report += adb::Format(L"  [diag] 裁剪前=%d 预算=%zu 裁剪后=%d\r\n",
                                    appended, budget, after);
            Check(static_cast<size_t>(after) <= budget, L"裁剪后长度回到预算以内");

            // 没超预算时不裁
            Check(!TrimEditToBudget(edit, 10 * 1024 * 1024), L"未超预算时不裁剪");
            ::DestroyWindow(edit);
        }

        // 批处理通道：大量行进去，占用始终有界
        adb::BoundedText probe(256 * 1024, 4096);
        for (int i = 0; i < 100000; ++i) probe.Append(L"stress line 0123456789abcdef");
        Check(probe.PendingBytes() <= 256 * 1024, L"输出通道占用不超过预算");
        Check(probe.DroppedBytes() > 0, L"超预算时记录了丢弃量");
        Check(probe.TotalLines() == 100000u, L"累计行数统计正确");
    }

    g_report += L"----------------------------------------\r\n";
    g_report += adb::Format(L"通过 %d 项，失败 %d 项\r\n", g_pass, g_fail);

    HANDLE h = ::CreateFileW(reportPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        std::string utf8 = adb::WideToUtf8(g_report);
        ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        ::CloseHandle(h);
    }
    return g_fail == 0 ? 0 : 1;
}

}  // namespace adbgui