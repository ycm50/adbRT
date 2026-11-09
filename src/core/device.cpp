#include "device.h"

#include <algorithm>

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
    if (!m_serialLoaded) {
        m_serial = Settings::Instance().CurrentDevice();
        m_serialLoaded = true;
    }
    if (!m_serial.empty()) return m_serial;

    auto devices = ListAdb();
    std::vector<DeviceEntry> online;
    for (const auto& d : devices)
        if (d.state == DeviceState::Device) online.push_back(d);
    if (online.size() == 1) {
        m_serial = online.front().serial;
        Settings::Instance().SetCurrentDevice(m_serial);
        Settings::Instance().Save();
    }
    return m_serial;
}

void DeviceSession::SetCurrentSerial(const std::wstring& serial) {
    m_serial = serial;
    m_serialLoaded = true;
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
    std::vector<std::wstring> args = {L"shell", L"pm", L"list", L"packages"};
    if (!includeSystem) { args.push_back(L"-3"); }
    RunOptions o;
    o.timeoutMs = 30000;
    auto r = RunProcess(AdbExe(), WithSerial(args), o);
    if (!r.started) return {};
    return ParsePackageList(r.OutW());
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
    if (m_serial.empty()) return args;
    std::vector<std::wstring> out;
    out.push_back(L"-s");
    out.push_back(m_serial);
    out.insert(out.end(), args.begin(), args.end());
    return out;
}

}  // namespace adb
