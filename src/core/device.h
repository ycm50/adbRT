#pragma once
// ---------------------------------------------------------------------------
// 设备会话：当前设备、设备枚举、常用查询（都需要真实 adb，但可被桩替代）
//   exe 路径来自 Settings：支持相对目录 / 绝对目录 / 环境变量
// ---------------------------------------------------------------------------

#include <mutex>
#include <string>
#include <vector>

#include "adb_runner.h"
#include "parsers.h"
#include "settings.h"

namespace adb {

class DeviceSession {
public:
    static DeviceSession& Instance();

    std::wstring AdbExe();
    std::wstring FastbootExe();

    // 直接调 adb，不做校验（内部用）
    RunResult Run(const std::vector<std::wstring>& args, const RunOptions& opts = {});

    std::vector<DeviceEntry> ListAdb();
    std::vector<DeviceEntry> ListFastboot();

    // 当前设备：若未设置且只有一台在线设备，自动选中
    std::wstring CurrentSerial();
    void         SetCurrentSerial(const std::wstring& serial);
    bool         CurrentOnline();
    DeviceEntry  CurrentEntry();

    // 便捷查询（失败返回空）
    std::wstring Shell(const std::wstring& cmd, uint32_t timeoutMs = 15000);
    std::vector<PackageEntry> InstalledPackages(bool includeSystem);
    std::vector<std::wstring> Partitions();
    std::wstring DeviceSummary();

    // 构造带 -s 的 argv（serial 为空则不带）
    std::vector<std::wstring> WithSerial(const std::vector<std::wstring>& args) const;

private:
    DeviceSession() = default;

    // 后台线程（应用名称解析、logcat、扫描）也会读当前设备，
    // 原来 m_serial / m_serialLoaded 完全没有保护 —— 并发读改写会撞车。
    mutable std::mutex m_mu;
    std::wstring       m_serial;
    bool               m_serialLoaded = false;
};

}  // namespace adb