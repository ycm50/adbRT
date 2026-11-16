#include "device.h"

#include <algorithm>
#include <unordered_set>

namespace adb {

DeviceSession& DeviceSession::Instance() {
    static DeviceSession inst;
    return inst;
}

std::wstring DeviceSession::AdbExe() { return Settings::Instance().AdbPath(); }
std::wstring DeviceSession::FastbootExe() { return Settings::Instance().FastbootPath(); }

RunResult DeviceSession::Run(const std::vector<std::wstring>& args, const RunOptions& opts) {
    return RunProcess(AdbExe(), args, opts);
}

std::vector<DeviceEntry> DeviceSession::ListAdb() {
    RunOptions o;
    o.timeoutMs = 15000;
    auto r = RunProcess(AdbExe(), {L"devices", L"-l"}, o);
    if (!r.started) return {};
    return ParseDevices(r.OutW());
}

std::vector<DeviceEntry> DeviceSession::ListFastboot() {
    RunOptions o;
    o.timeoutMs = 15000;
    auto r = RunProcess(FastbootExe(), {L"devices", L"-l"}, o);
    if (!r.started) return {};
    return ParseDevices(r.OutW());
}

std::wstring DeviceSession::CurrentSerial() {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_serialLoaded) {
            m_serial = Settings::Instance().CurrentDevice();
            m_serialLoaded = true;
        }
        if (!m_serial.empty()) return m_serial;
    }

    // 自动选中只在线程外做一次；ListAdb 会起进程，不能持锁调用
    auto devices = ListAdb();
    std::vector<DeviceEntry> online;
    for (const auto& d : devices)
        if (d.state == DeviceState::Device) online.push_back(d);
    if (online.size() != 1) {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_serial;
    }

    std::wstring tentative = online.front().serial;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_serial.empty()) return m_serial;   // 别的线程抢先定了
        m_serial = tentative;
    }
    Settings::Instance().SetCurrentDevice(tentative);
    Settings::Instance().Save();
    return tentative;
}

void DeviceSession::SetCurrentSerial(const std::wstring& serial) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_serial = serial;
        m_serialLoaded = true;
    }
    Settings::Instance().SetCurrentDevice(serial);
    Settings::Instance().Save();
}

bool DeviceSession::CurrentOnline() {
    std::wstring want = CurrentSerial();
    if (want.empty()) return false;
    for (const auto& d : ListAdb())
        if (d.serial == want) return d.state == DeviceState::Device;
    return false;
}

DeviceEntry DeviceSession::CurrentEntry() {
    std::wstring want = CurrentSerial();
    for (const auto& d : ListAdb())
        if (d.serial == want) return d;
    return {};
}

std::wstring DeviceSession::Shell(const std::wstring& cmd, uint32_t timeoutMs) {
    std::vector<std::wstring> args = {L"shell", cmd};
    RunOptions o;
    o.timeoutMs = timeoutMs;
    auto r = RunProcess(AdbExe(), WithSerial(args), o);
    if (!r.started || r.exitCode != 0) return {};
    std::wstring out = r.OutW();
    return Trim(out);
}

std::vector<PackageEntry> DeviceSession::InstalledPackages(bool includeSystem) {
    // 只要第三方时直接 -3；要全量时再补一次 -3 用来标记哪些是系统应用
    std::vector<std::wstring> base = {L"shell", L"pm", L"list", L"packages"};
    RunOptions o;
    o.timeoutMs = 30000;

    if (!includeSystem) {
        auto r = RunProcess(AdbExe(), WithSerial({L"shell", L"pm", L"list", L"packages", L"-3"}), o);
        if (!r.started) return {};
        return ParsePackageList(r.OutW());
    }

    auto all = RunProcess(AdbExe(), WithSerial(base), o);
    if (!all.started) return {};
    auto thirdParty = RunProcess(AdbExe(),
                                 WithSerial({L"shell", L"pm", L"list", L"packages", L"-3"}), o);

    // 用哈希集合判定"是不是第三方"：原来是每个包都对整个 third 列表
    // 做一次线性 find，包多的时候就是 O(n²)，列表窗口一开就肉眼可见地卡。
    std::unordered_set<std::wstring> third;
    for (const auto& p : ParsePackageList(thirdParty.OutW())) third.insert(p.package);

    std::vector<PackageEntry> out = ParsePackageList(all.OutW());
    for (auto& p : out) {
        p.system = third.find(p.package) == third.end();
    }
    return out;
}

std::vector<PackageEntry> DeviceSession::InstalledPackagesWithPaths(bool includeSystem) {
    RunOptions o;
    o.timeoutMs = 30000;
    auto r = RunProcess(AdbExe(), WithSerial({L"shell", L"pm", L"list", L"packages", L"-f"}), o);
    if (!r.started) return {};

    std::vector<PackageEntry> out = ParsePackageListWithPaths(r.OutW());
    if (!includeSystem) {
        // 再问一次 -3 标记哪些是第三方（-f 和 -3 组合不可靠）
        auto t = RunProcess(AdbExe(),
                            WithSerial({L"shell", L"pm", L"list", L"packages", L"-3"}), o);
        std::unordered_set<std::wstring> third;
        for (const auto& p : ParsePackageList(t.OutW())) third.insert(p.package);
        std::vector<PackageEntry> kept;
        kept.reserve(out.size());
        for (auto& p : out) {
            bool isSystem = third.find(p.package) == third.end();
            if (isSystem) continue;
            p.system = false;
            kept.push_back(std::move(p));
        }
        return kept;
    }
    // 全量时也要标出系统应用
    auto t = RunProcess(AdbExe(),
                        WithSerial({L"shell", L"pm", L"list", L"packages", L"-3"}), o);
    std::unordered_set<std::wstring> third;
    for (const auto& p : ParsePackageList(t.OutW())) third.insert(p.package);
    for (auto& p : out) p.system = third.find(p.package) == third.end();
    return out;
}

std::vector<std::wstring> DeviceSession::Partitions() {
    RunOptions o;
    o.timeoutMs = 15000;
    auto r = RunProcess(FastbootExe(), WithSerial({L"getvar", L"all"}), o);
    if (!r.started) return {};
    return ParsePartitions(r.ErrW() + L"\n" + r.OutW());
}

std::wstring DeviceSession::DeviceSummary() {
    std::wstring s;
    for (const auto& d : ListAdb()) {
        if (!s.empty()) s += L"\n";
        s += d.Describe();
    }
    return s;
}

std::vector<std::wstring> DeviceSession::WithSerial(const std::vector<std::wstring>& args) const {
    std::wstring serial;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        serial = m_serial;
    }
    if (serial.empty()) return args;
    std::vector<std::wstring> out;
    out.push_back(L"-s");
    out.push_back(serial);
    out.insert(out.end(), args.begin(), args.end());
    return out;
}

}  // namespace adb