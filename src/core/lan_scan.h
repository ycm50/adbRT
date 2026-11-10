#pragma once
// ---------------------------------------------------------------------------
// 局域网扫描（无线调试用）
//   · 只做两件事：列出本机网卡的 IPv4 段、并发探测哪些主机在线
//   · 用 Win32 原生 API（GetAdaptersAddresses + ICMP / TCP 探测），无第三方库
//   · 一切都是"只读探测"，不会改动网络设置
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace adb {

struct LocalNet {
    std::wstring adapter;     // 网卡名（给用户看）
    std::wstring localIp;     // 本机 IPv4（如 192.168.1.23）
    std::wstring prefix;      // 前 24 位（如 192.168.1）
    int          prefixLen = 24;
    std::wstring gateway;
};

// 本机所有可用 IPv4 网段（跳过回环、169.254 与没有网线的）
std::vector<LocalNet> ListLocalNets();

// 并发扫一个 /24 段（prefix 形如 "192.168.1"）：
//   · 先用 ICMP 快速判断主机在不在
//   · 再对在线主机做一次 adb 端口（5555）的 TCP 连接探测
//   · 结果按 IP 数字序排列，adbHosts 里是能连上 5555 的
struct ScanResult {
    std::vector<std::wstring> alive;      // 在线主机 IP
    std::vector<std::wstring> adbHosts;   // 其中开着 5555 的
};

ScanResult ScanLan(const std::wstring& prefix, int timeoutMs, int adbPort = 5555);

// 端口连接结果
enum class PortState {
    Refused,   // 有回 RST —— 说明主机是活的，只是这个端口没开
    Open,      // 连上了
    Filtered,  // 超时没响应 —— 多半主机不在，或者被防火墙挡了
};

PortState ProbeTcpPortEx(const std::wstring& ip, int port, int timeoutMs);
bool      ProbeTcpPort(const std::wstring& ip, int port, int timeoutMs);

// 在 [lo, hi) 里找开着的端口（多线程并发，单次超时很短）
std::vector<int> ScanPortRange(const std::wstring& ip, int lo, int hi, int timeoutMs = 250);

// 选中一台主机后，找出它的 adb 端口：
//   1) 先试默认的 5555
//   2) 再问 adb mdns services（无线调试配对后端口是随机的，只有 mDNS 知道）
//   3) 都没有就扫 30000-50000 这段（Android 无线调试的随机端口就落在这里），
//      对每个开着的端口真的 adb connect 一次，能连上才算
std::wstring FindAdbEndpoint(const std::wstring& ip, int timeoutMs = 700);

}  // namespace adb