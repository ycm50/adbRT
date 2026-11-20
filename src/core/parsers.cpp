#include "parsers.h"

#include <algorithm>

#include "core.h"

namespace adb {

const wchar_t* StateName(DeviceState s) {
    switch (s) {
        case DeviceState::Device:       return L"device";
        case DeviceState::Offline:      return L"offline";
        case DeviceState::Unauthorized: return L"unauthorized";
        case DeviceState::Recovery:     return L"recovery";
        case DeviceState::Sideload:     return L"sideload";
        case DeviceState::Bootloader:   return L"bootloader";
        case DeviceState::NoPermission: return L"no permission";
        default:                        return L"unknown";
    }
}

std::wstring DeviceEntry::Describe() const {
    std::wstring s = serial;
    if (!model.empty()) s += L" \x00b7 " + model;
    else if (!product.empty()) s += L" \x00b7 " + product;
    s += L" [";
    s += StateName(state);
    s += L"]";
    return s;
}

static DeviceState MapState(const std::wstring& raw) {
    if (raw == L"device") return DeviceState::Device;
    if (raw == L"offline") return DeviceState::Offline;
    if (raw == L"unauthorized") return DeviceState::Unauthorized;
    if (raw == L"recovery") return DeviceState::Recovery;
    if (raw == L"sideload") return DeviceState::Sideload;
    if (raw == L"bootloader" || raw == L"fastboot" || raw == L"fastbootd")
        return DeviceState::Bootloader;
    if (raw.find(L"no permissions") != std::wstring::npos) return DeviceState::NoPermission;
    return DeviceState::Unknown;
}

std::vector<DeviceEntry> ParseDevices(const std::wstring& output) {
    std::vector<DeviceEntry> out;
    for (const auto& rawLine : SplitLines(output)) {
        std::wstring line = Trim(rawLine);
        if (line.empty()) continue;
        if (StartsWith(line, L"List of devices")) continue;
        if (StartsWith(line, L"*")) continue;
        if (EqualsNoCase(line, L"no devices")) continue;

        std::vector<std::wstring> tok = SplitWhitespace(line);
        if (tok.size() < 2) continue;

        DeviceEntry e;
        e.serial = tok[0];
        e.rawState = tok[1];
        if (e.rawState == L"no") e.rawState = line.substr(line.find(L"no"));
        e.state = MapState(e.rawState);

        for (size_t i = 2; i < tok.size(); ++i) {
            const std::wstring& t = tok[i];
            auto take = [&](const wchar_t* key, std::wstring* dst) {
                std::wstring prefix = std::wstring(key) + L":";
                if (StartsWith(t, prefix)) { *dst = t.substr(prefix.size()); return true; }
                return false;
            };
            if (take(L"product", &e.product)) continue;
            if (take(L"model", &e.model)) continue;
            if (take(L"device", &e.device)) continue;
            take(L"transport_id", &e.transportId);
        }
        out.push_back(e);
    }
    return out;
}

std::vector<PackageEntry> ParsePackageList(const std::wstring& output) {
    std::vector<PackageEntry> out;
    for (const auto& rawLine : SplitLines(output)) {
        std::wstring line = Trim(rawLine);
        if (!StartsWith(line, L"package:")) continue;
        PackageEntry e;
        e.package = line.substr(8);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const PackageEntry& a, const PackageEntry& b) {
                  return a.package < b.package;
              });
    return out;
}

const std::vector<KeyEvent>& StandardKeyEvents() {
    static const std::vector<KeyEvent> k = {
        {L"BACK(\x8fd4\x56de)", 4},
        {L"HOME(\x4e3b\x9875)", 3},
        {L"APP_SWITCH(\x6700\x8fd1\x4efb\x52a1)", 187},
        {L"POWER(\x7535\x6e90)", 26},
        {L"VOLUME_UP(\x97f3\x91cf+)", 24},
        {L"VOLUME_DOWN(\x97f3\x91cf-)", 25},
        {L"MENU", 82},
        {L"ENTER", 66},
        {L"DEL(\x9000\x683c)", 67},
        {L"TAB", 61},
        {L"SPACE(\x7a7a\x683c)", 62},
        {L"ESCAPE", 111},
        {L"DPAD_UP", 19},
        {L"DPAD_DOWN", 20},
        {L"DPAD_LEFT", 21},
        {L"DPAD_RIGHT", 22},
        {L"DPAD_CENTER", 23},
        {L"CAMERA", 27},
        {L"WAKEUP(\x5524\x9192)", 224},
        {L"SLEEP(\x4f11\x7720)", 223},
    };
    return k;
}

const std::vector<std::wstring>& CommonPartitions() {
    static const std::vector<std::wstring> k = {
        L"boot", L"init_boot", L"vendor_boot", L"recovery", L"dtbo", L"vbmeta",
        L"vbmeta_system", L"super", L"system", L"vendor", L"product", L"system_ext",
        L"userdata", L"cache", L"metadata", L"persist", L"modem", L"abl", L"xbl",
        L"tz", L"hyp", L"devcfg", L"misc",
    };
    return k;
}

const std::vector<std::wstring>& CommonProps() {
    static const std::vector<std::wstring> k = {
        L"ro.product.model", L"ro.product.brand", L"ro.product.manufacturer",
        L"ro.product.device", L"ro.build.version.release", L"ro.build.version.sdk",
        L"ro.build.version.incremental", L"ro.build.type", L"ro.build.tags",
        L"ro.serialno", L"ro.bootloader", L"ro.hardware", L"ro.board.platform",
        L"ro.debuggable", L"ro.secure", L"ro.build.fingerprint",
        L"persist.sys.locale", L"persist.sys.timezone", L"gsm.sim.state",
        L"gsm.operator.alpha", L"ro.sf.lcd_density", L"ro.product.cpu.abi",
        L"sys.boot_completed", L"sys.usb.state", L"service.adb.tcp.port",
    };
    return k;
}

std::vector<std::wstring> ParsePartitions(const std::wstring& output) {
    std::vector<std::wstring> out;
    for (const auto& rawLine : SplitLines(output)) {
        std::wstring line = Trim(rawLine);
        if (line.empty()) continue;
        size_t first = line.find(L':');
        if (first == std::wstring::npos) continue;
        size_t second = line.find(L':', first + 1);
        std::wstring name = (second == std::wstring::npos)
                                ? line.substr(first + 1)
                                : line.substr(first + 1, second - first - 1);
        if (name.empty()) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::wstring> ParseKeyValues(const std::wstring& output) {
    std::vector<std::wstring> out;
    for (const auto& rawLine : SplitLines(output)) {
        std::wstring line = Trim(rawLine);
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

int ParseProgressPercent(const std::wstring& line) {
    size_t pct = line.find(L'%');
    if (pct == std::wstring::npos) return -1;
    size_t b = pct;
    while (b > 0 && line[b - 1] >= L'0' && line[b - 1] <= L'9') --b;
    if (b == pct) return -1;
    int v = _wtoi(line.substr(b, pct - b).c_str());
    return (v >= 0 && v <= 100) ? v : -1;
}

std::vector<std::wstring> ParseConnectCandidates(const std::wstring& output) {
    // 同时兼容两种输出：
    //   · adb mdns services → "adb-XXXX	_system-adb._tcp.	192.168.1.7:39145"
    //   · 已经连过的设备 → "192.168.1.7:5555	device"
    std::vector<std::wstring> out;
    for (const auto& rawLine : SplitLines(output)) {
        std::wstring line = Trim(rawLine);
        if (line.empty()) continue;
        std::vector<std::wstring> tok = SplitWhitespace(line);
        if (tok.empty()) continue;
        std::wstring cand = tok.back();
        // 只接受 host:port 形态
        size_t colon = cand.rfind(L':');
        if (colon == std::wstring::npos || colon == 0 || colon + 1 >= cand.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < colon; ++i) {
            wchar_t ch = cand[i];
            if (!((ch >= L'0' && ch <= L'9') || ch == L'.')) { ok = false; break; }
        }
        if (!ok) continue;
        for (size_t i = colon + 1; i < cand.size(); ++i)
            if (cand[i] < L'0' || cand[i] > L'9') { ok = false; break; }
        if (!ok) continue;
        if (std::find(out.begin(), out.end(), cand) == out.end()) out.push_back(cand);
    }
    return out;
}

void SortLanHosts(std::vector<std::wstring>* hosts) {
    if (!hosts) return;
    auto key = [](const std::wstring& ip) -> uint32_t {
        uint32_t v = 0;
        int part = 0;
        for (wchar_t c : ip) {
            if (c == L'.') { v = (v << 8) | static_cast<uint32_t>(part & 0xFF); part = 0; continue; }
            if (c < L'0' || c > L'9') { part = 0; continue; }
            part = part * 10 + (c - L'0');
        }
        return (v << 8) | static_cast<uint32_t>(part & 0xFF);
    };
    std::sort(hosts->begin(), hosts->end(),
              [&](const std::wstring& a, const std::wstring& b) { return key(a) < key(b); });
}

std::wstring ExplainAdbError(const std::wstring& err) {
    if (err.find(L"more than one device") != std::wstring::npos)
        return L"\x8fde\x63a5\x4e86\x591a\x53f0\x8bbe\x5907\xff0c\x8bf7\x5148\x5728\x9876\x90e8\x9009\x62e9\x76ee\x6807\x8bbe\x5907\x3002";
    if (err.find(L"no devices found") != std::wstring::npos ||
        err.find(L"device not found") != std::wstring::npos)
        return L"\xe6\xb2\xa1\xe6\x9c\x89\xe6\x89\xbe\xe5\x88\xb0\xe8\xae\xbe\xe5\xa4\x87\xef\xbc\x9a\xe8\xaf\xb7\xe7\xa1\xae\xe8\xae\xa4 USB \xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5\xef\xbc\x8c\xe5\xb9\xb6\xe5\xbc\x80\xe5\x90\xaf\xe3\x80\x8cUSB \xe8\xb0\x83\xe8\xaf\x95\xe3\x80\x8d\xe3\x80\x82";
    if (err.find(L"unauthorized") != std::wstring::npos)
        return L"\xe8\xae\xbe\xe5\xa4\x87\xe6\x9c\xaa\xe6\x8e\x88\xe6\x9d\x83\xef\xbc\x9a\xe8\xaf\xb7\xe5\x9c\xa8\xe6\x89\x8b\xe6\x9c\xba\xe5\xb1\x8f\xe5\xb9\x95\xe4\xb8\x8a\xe7\x82\xb9\xe3\x80\x8c\xe5\x85\x81\xe8\xae\xb8 USB \xe8\xb0\x83\xe8\xaf\x95\xe3\x80\x8d\xe5\x90\x8e\xe9\x87\x8d\xe8\xaf\x95\xe3\x80\x82";
    if (err.find(L"INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::wstring::npos)
        return L"\xe7\xad\xbe\xe5\x90\x8d\xe4\xb8\x8d\xe4\xb8\x80\xe8\x87\xb4\xef\xbc\x9a\xe9\x9c\x80\xe8\xa6\x81\xe5\x85\x88\xe5\x8d\xb8\xe8\xbd\xbd\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xe7\x9a\x84\xe7\x89\x88\xe6\x9c\xac\xe3\x80\x82";
    if (err.find(L"INSTALL_FAILED_VERSION_DOWNGRADE") != std::wstring::npos)
        return L"\xe7\x89\x88\xe6\x9c\xac\xe9\x99\x8d\xe7\xba\xa7\xe8\xa2\xab\xe6\x8b\x92\xe7\xbb\x9d\xef\xbc\x9a\xe5\x8b\xbe\xe9\x80\x89\xe3\x80\x8c\xe5\x85\x81\xe8\xae\xb8\xe9\x99\x8d\xe7\xba\xa7 -d\xe3\x80\x8d\xe5\x90\x8e\xe9\x87\x8d\xe8\xaf\x95\xe3\x80\x82";
    if (err.find(L"INSTALL_FAILED_INSUFFICIENT_STORAGE") != std::wstring::npos)
        return L"\xe8\xae\xbe\xe5\xa4\x87\xe5\xad\x98\xe5\x82\xa8\xe7\xa9\xba\xe9\x97\xb4\xe4\xb8\x8d\xe8\xb6\xb3\xe3\x80\x82";
    if (err.find(L"INSTALL_PARSE_FAILED") != std::wstring::npos)
        return L"APK \xe8\xa7\xa3\xe6\x9e\x90\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a\xe6\x96\x87\xe4\xbb\xb6\xe5\x8f\xaf\xe8\x83\xbd\xe6\x8d\x9f\xe5\x9d\x8f\xe3\x80\x82";
    if (err.find(L"cannot connect") != std::wstring::npos ||
        err.find(L"failed to connect") != std::wstring::npos)
        return L"\xe6\x97\xa0\xe6\xb3\x95\xe8\xbf\x9e\xe6\x8e\xa5\xef\xbc\x9a\xe8\xaf\xb7\xe6\xa3\x80\xe6\x9f\xa5 IP/\xe7\xab\xaf\xe5\x8f\xa3\xe4\xb8\x8e\xe5\x90\x8c\xe4\xb8\x80\xe7\xbd\x91\xe7\xbb\x9c\xe3\x80\x82";
    if (err.find(L"protocol fault") != std::wstring::npos ||
        err.find(L"error: closed") != std::wstring::npos)
        return L"\xe8\xbf\x9e\xe6\x8e\xa5\xe4\xb8\xad\xe6\x96\xad\xef\xbc\x9a\xe5\x8f\xaf\xe4\xbb\xa5\xe5\xb0\x9d\xe8\xaf\x95\xe9\x87\x8d\xe6\x8e\xa5\xe8\xae\xbe\xe5\xa4\x87\xe6\x88\x96\xe9\x87\x8d\xe6\x8f\x92 USB\xe3\x80\x82";
    return {};
}

}  // namespace adb