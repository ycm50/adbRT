// ---------------------------------------------------------------------------
// CLI 门面：让命令表可以被脚本直接驱动（与 GUI 走同一条 argv 构造路径）
//
//   AdbRT.exe --run <key> [--param k=v]... [--flag k=1]...
//                        [--serial <序列号>] [--dry-run] [--out <文件>]
//
// 为什么要有它：
//   · 全量测试需要在不点界面的前提下把每条命令真的跑一遍
//   · 用户脚本化（CI / 批处理）也用得上
//   · 关键：**仍然经过 BuildCommand**，不存在"绕过白名单"的旁路
// ---------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/settings.h"
#include "gui.h"
#include "reports.h"
#include "version.h"

namespace adbgui {

namespace {

// GUI 子系统（-mwindows）默认没有 stdout。两种情况都要能输出：
//   ① 被 Start-Process -RedirectStandardOutput 重定向 → 句柄本身可用，直接 WriteFile
//   ② 挂在父控制台上 → 先 AttachConsole，再写
void AttachToParentConsole() {
    if (::GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE) return;
    ::AttachConsole(ATTACH_PARENT_PROCESS);
    HANDLE h = ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) ::SetStdHandle(STD_OUTPUT_HANDLE, h);
}

void WriteRaw(const std::string& utf8) {
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD written = 0;
    ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void WriteOut(const std::wstring& text) {
    WriteRaw(adb::WideToUtf8(text));
    if (HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE); h && h != INVALID_HANDLE_VALUE)
        ::FlushFileBuffers(h);
}

// 解析 "k=v"
bool SplitKv(const std::wstring& s, std::string* k, std::wstring* v) {
    size_t eq = s.find(L'=');
    if (eq == std::wstring::npos || eq == 0) return false;
    *k = adb::WideToUtf8(s.substr(0, eq));
    *v = s.substr(eq + 1);
    return true;
}

}  // namespace

int RunCli(const std::vector<std::wstring>& args) {
    AttachToParentConsole();

    std::wstring key;
    std::wstring serial;
    std::wstring adbOverride;
    std::wstring outFile;
    bool dryRun = false;
    adb::BuildInput in;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--run" && i + 1 < args.size()) { key = args[++i]; continue; }
        if (a == L"--serial" && i + 1 < args.size()) { serial = args[++i]; continue; }
        if (a == L"--adb" && i + 1 < args.size()) { adbOverride = args[++i]; continue; }
        if (a == L"--out" && i + 1 < args.size()) { outFile = args[++i]; continue; }
        if (a == L"--dry-run") { dryRun = true; continue; }
        if (a == L"--param" && i + 1 < args.size()) {
            std::string k; std::wstring v;
            if (SplitKv(args[++i], &k, &v)) in.params[k] = v;
            continue;
        }
        if (a == L"--flag" && i + 1 < args.size()) {
            std::string k; std::wstring v;
            if (SplitKv(args[++i], &k, &v)) in.flags[k] = v;
            continue;
        }
        // 位置参数：当作本地路径（推送/安装类命令用）
        if (!a.empty() && a[0] != L'-') in.paths.push_back(adb::NormalizePath(a));
    }

    std::wstring log;
    // --out 时只写文件（保证文件一定被写出来，不受提前 return 影响）
    auto flushOut = [&]() {
        if (outFile.empty()) { WriteOut(log); return; }
        HANDLE h = ::CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { WriteOut(log); return; }
        std::string utf8 = adb::WideToUtf8(log);
        DWORD written = 0;
        ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        ::CloseHandle(h);
    };
    auto emit = [&](const std::wstring& s) { log += s; };

    if (key.empty()) {
        emit(L"用法：AdbRT.exe --run <命令key> [--param k=v] [--flag k=1] [--serial X] [--dry-run] [--out 文件]\r\n\r\n");
        emit(L"可用命令：\r\n");
        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const auto& c = adb::CommandTable()[i];
            if (c.exec == adb::ExecKind::Internal) continue;
            emit(adb::Format(L"  %-22ls %ls\r\n", adb::Utf8ToWide(c.key).c_str(),
                             adb::CommandTitle(c)));
        }
        flushOut();
        return 2;
    }

    const adb::CommandSpec* spec = adb::FindCommandByKey(adb::WideToUtf8(key));
    if (!spec) {
        emit(L"找不到命令：" + key + L"\r\n");
        flushOut();
        return 2;
    }
    // 界面内部命令：用同一份报告生成器输出文本，这样"只读信息类"也能被脚本全量验证
    if (spec->exec == adb::ExecKind::Internal) {
        std::string k = spec->key;
        std::wstring body;
        if (k == "device.list")        body = BuildDeviceListText();
        else if (k == "device.info")   body = BuildDeviceInfoText();
        else if (k == "app.list")      body = BuildAppListText(true);
        else if (k == "tool.doctor")   body = BuildDoctorText();
        else if (k == "tool.about")    body = BuildAboutText();
        else if (k == "device.select") {
            auto& sess = adb::DeviceSession::Instance();
            body = L"当前设备：" +
                   (sess.CurrentSerial().empty() ? std::wstring(L"（未选择）") : sess.CurrentSerial()) +
                   L"\r\n\r\n" + BuildDeviceListText();
        }
        if (body.empty()) {
            emit(L"该命令只能在界面里执行（会打开窗口）：" + key + L"\r\n");
            flushOut();
            return 2;
        }
        emit(body);
        flushOut();
        return 0;
    }

    // --adb 临时覆盖；--serial 同时也写进会话，让"内部命令"的报告走同一台设备
    if (!adbOverride.empty()) adb::SetTransientAdbOverride(adbOverride);
    auto& session = adb::DeviceSession::Instance();
    if (!serial.empty()) session.SetCurrentSerial(serial);

    in.spec = spec;
    in.toolExe = spec->needsFastboot ? session.FastbootExe() : session.AdbExe();
    in.serial = serial.empty() ? session.CurrentSerial() : serial;

    if (in.toolExe.empty()) {
        emit(std::wstring(L"找不到可执行文件（") +
             (spec->needsFastboot ? L"fastboot.exe" : L"adb.exe") + L"）\r\n");
        flushOut();
        return 3;
    }
    if (spec->needsDevice && in.serial.empty()) {
        emit(L"没有可用设备：请用 --serial 指定序列号，或先连接设备\r\n");
        flushOut();
        return 3;
    }

    adb::BuiltCommand built;
    adb::BuildError err;
    if (!adb::BuildCommand(in, &built, &err)) {
        emit(L"命令行构造失败：" + err.message + L"\r\n");
        flushOut();
        return 2;
    }

    emit(L"命令：");
    emit(adb::FileNameOf(in.toolExe) + L" " + built.display + L"\r\n");
    if (dryRun) { flushOut(); return 0; }

    int rc = 0;
    for (const auto& argv : built.argvList) {
        adb::RunOptions opts;
        opts.sink = [&](const std::string& line, bool isStderr) -> bool {
            std::wstring w = adb::Utf8ToWide(line);
            if (isStderr) w = L"[err] " + w;
            emit(w + L"\r\n");
            return true;
        };
        adb::RunResult r = adb::RunProcess(in.toolExe, argv, opts);
        if (!r.started) {
            emit(L"启动失败：" + r.error + L"\r\n");
            rc = 4;
            break;
        }
        if (r.exitCode != 0) {
            rc = static_cast<int>(r.exitCode);
            std::wstring explain = adb::ExplainAdbError(r.ErrW() + L"\n" + r.OutW());
            if (!explain.empty()) emit(L"[提示] " + explain + L"\r\n");
            break;
        }
    }

    flushOut();
    return rc;
}

// --list-commands：纯文本列出命令表（脚本发现用）
int ListCommands(const std::wstring& outFile) {
    AttachToParentConsole();
    std::wstring text;
    for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
        const auto& c = adb::CommandTable()[i];
        text += adb::Format(L"%-22ls %-10ls %-12ls %ls\r\n",
                            adb::Utf8ToWide(c.key).c_str(),
                            adb::GroupName(c.group),
                            adb::Utf8ToWide(adb::DangerName(c.danger)).c_str(),
                            adb::CommandTitle(c));
    }
    if (!outFile.empty()) {
        HANDLE h = ::CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string utf8 = adb::WideToUtf8(text);
            DWORD written = 0;
            ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            ::CloseHandle(h);
            return 0;
        }
    }
    WriteOut(text);
    return 0;
}

}  // namespace adbgui