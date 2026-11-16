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
    bool         system = false;   // 不在 pm list packages -3 里就是系统应用
    std::wstring apkPath;          // 设备上的 APK 路径（来自 pm list packages -f）
};
std::vector<PackageEntry> ParsePackageList(const std::wstring& output);

// 解析 "pm list packages -f" 的输出：package:<路径>=<包名>
// 路径里有 '=' 的情况极少，按最后一个 '=' 切分更稳
std::vector<PackageEntry> ParsePackageListWithPaths(const std::wstring& output);

struct KeyEvent {
    std::wstring name;
    int          code = 0;
};
const std::vector<KeyEvent>& StandardKeyEvents();
// 常见的 fastboot 分区名（读不到设备时给选择框兜底）
const std::vector<std::wstring>& CommonPartitions();
// 常见的系统属性名（设备读取失败时给复选框兜底）
const std::vector<std::wstring>& CommonProps();
std::vector<std::wstring> ParsePartitions(const std::wstring& output);
std::vector<std::wstring> ParseKeyValues(const std::wstring& output);
int ParseProgressPercent(const std::wstring& line);
std::wstring ExplainAdbError(const std::wstring& stderrText);

// 从 "adb mdns services" / "connect" 的输出里挑出可用的 IP:端口
std::vector<std::wstring> ParseConnectCandidates(const std::wstring& output);
// 局域网扫描结果排序：先有 adb 的，再按 IP 数字序
void SortLanHosts(std::vector<std::wstring>* hosts);

}  // namespace adb