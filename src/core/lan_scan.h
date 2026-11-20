#pragma once
// ---------------------------------------------------------------------------
// 局域网扫描（无线调试用）
//   · 列出本机网卡的 IPv4 段；并发探测哪些主机在线；并发找 adb 端口
//   · 用 Win32 原生 API（GetAdaptersAddresses / SendARP / ICMP / 非阻塞 connect）
//   · 一切都是"只读探测"，不会改动网络设置
//   · 每个耗时接口都接受可选的取消标志，关窗口能立刻停下来
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace adb {

// 判定"在同一个局域网"的时延上限：超过这个数的响应一律不算（可能是跨网段或丢包重传）
constexpr int kLanMaxLatencyMs = 50;

struct LocalNet {
    std::wstring adapter;     // 网卡名（给用户看）
    std::wstring localIp;     // 本机 IPv4（如 192.168.1.23）
    std::wstring prefix;      // 前 24 位（如 192.168.1）
    int          prefixLen = 24;
    std::wstring gateway;
};

// 本机所有可用 IPv4 网段（跳过回环、169.254 与没有网线的）
std::vector<LocalNet> ListLocalNets();

// 端口连接结果
enum class PortState {
    Refused,   // 有回 RST —— 说明主机是活的，只是这个端口没开
    Open,      // 连上了
    Filtered,  // 超时没响应 —— 多半主机不在，或者被防火墙挡了
};

PortState ProbeTcpPortEx(const std::wstring& ip, int port, int timeoutMs);
bool      ProbeTcpPort(const std::wstring& ip, int port, int timeoutMs);

struct ScanHost {
    std::wstring ip;
    int  rttMs = -1;      // 实测时延；-1 表示不是 ICMP 测出来的（TCP/ARP 证明）
    bool adb = false;     // adb 端口（默认 5555）开着
};

struct ScanResult {
    std::vector<ScanHost> hosts;          // 全部在线主机（含时延）
    std::vector<std::wstring> alive;      // 在线主机 IP（按数字序）
    std::vector<std::wstring> adbHosts;   // 其中开着 adb 端口的
    int  probed = 0;                      // 实际探测了多少个地址
    int  threads = 0;                     // 用到的并发线程数
    uint64_t elapsedMs = 0;               // 扫描耗时，界面上直接显示
};

// 并发扫一个 /24 段（prefix 形如 "192.168.1"）。三阶段，全程并发：
//   ① 批量非阻塞 connect 打 adb 端口 —— 一次发 254 个，一个 select 收完
//   ② 本机 ARP 邻居表 —— 已经在表里的地址必然是活的，零成本
//   ③ 剩下的补 ICMP echo —— 多线程并发，单次超时就是 50ms 上限
// 时延超过 kLanMaxLatencyMs 的响应不算"在同一局域网"，直接丢弃。
ScanResult ScanLan(const std::wstring& prefix, int timeoutMs = kLanMaxLatencyMs,
                   int adbPort = 5555, std::atomic<bool>* cancel = nullptr);

// 在 [lo, hi) 里找开着的端口。
// 用非阻塞 connect + 批量 select（一次最多 512 个），不开线程 ——
// 扫 2 万个端口也就几十批，比"一个端口一个线程"省得多。
std::vector<int> ScanPortRange(const std::wstring& ip, int lo, int hi,
                               int timeoutMs = 250,
                               std::atomic<bool>* cancel = nullptr);

// 选中一台主机后，找出它的 adb 端口：
//   1) 先试默认的 5555
//   2) 再问 adb mdns services（无线调试配对后端口是随机的，只有 mDNS 知道）
//   3) 都没有就扫 30000-50000 这段，对每个开着的端口真的 adb connect 一次
//   0) 先看 adb devices 里是不是已经连着了
//   1) 再试默认的 5555
//   2) 再问 adb mdns services（无线调试配对后端口是随机的，只有 mDNS 知道）
//   3) 都没有就扫 30000-50000 这段，每个开着的端口真的 adb connect 一次并用 devices 复核
std::wstring FindAdbEndpoint(const std::wstring& ip, int timeoutMs = kLanMaxLatencyMs,
                             std::atomic<bool>* cancel = nullptr);

}  // namespace adb