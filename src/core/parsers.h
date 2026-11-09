#pragma once
// ---------------------------------------------------------------------------
// adb / fastboot 输出解析器（纯函数，无 IO —— 因此可以 100% 无设备单测）
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace adb {

enum class DeviceState {
    Unknown = 0, Device, Offline, Unauthorized, Recovery, Sideload, Bootloader, NoPermission
};

struct DeviceEntry {
    std::wstring serial;
    DeviceState  state = DeviceState::Unknown;
    std::wstring product, model, device, transportId, rawState;
    std::wstring Describe() const;
};

const wchar_t* StateName(DeviceState s);
std::vector<DeviceEntry> ParseDevices(const std::wstring& output);

struct PackageEntry {
    std::wstring package;
};
std::vector<PackageEntry> ParsePackageList(const std::wstring& output);

struct KeyEvent {
    std::wstring name;
    int          code = 0;
};
const std::vector<KeyEvent>& StandardKeyEvents();
std::vector<std::wstring> ParsePartitions(const std::wstring& output);
std::vector<std::wstring> ParseKeyValues(const std::wstring& output);
int ParseProgressPercent(const std::wstring& line);
std::wstring ExplainAdbError(const std::wstring& stderrText);

}  // namespace adb
