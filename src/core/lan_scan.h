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

// 扫描时要探的 adb 端口候选。
//   5555 只是 "adb tcpip 5555" 的老式无线调试；Android 11+ 的「无线调试」
//   端口是随机的高位端口，只能靠 mDNS 报出来 —— 所以这里必须能带自定义端口。
std::vector<int> DefaultAdbPortCandidates();

struct ScanHost {
    std::wstring ip;
    int  rttMs = -1;            // 实测时延；-1 表示不是 ICMP 测出来的（TCP/ARP 证明）
    bool adb = false;           // 是否探到了开着的 adb 端口
    std::vector<int> adbPorts;  // 实测开着的端口（按端口号升序），端口号是设备真实端口
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
//   ① 批量非阻塞 connect 打每个候选端口 —— 254×N 个套接字一口气发出去，一个 select 收完
//   ② 本机 ARP 邻居表 —— 已经在表里的地址必然是活的，零成本
//   ③ 剩下的补 ICMP echo —— 多线程并发，单次超时就是 50ms 上限
// 时延超过 kLanMaxLatencyMs 的响应不算"在同一局域网"，直接丢弃。
// ports 为空时用 DefaultAdbPortCandidates()。
ScanResult ScanLan(const std::wstring& prefix, int timeoutMs = kLanMaxLatencyMs,
                   const std::vector<int>& ports = {}, std::atomic<bool>* cancel = nullptr);

// 在 [lo, hi) 里找开着的端口。
// 用非阻塞 connect + 批量 select（一次最多 2048 个），不开线程 ——
// 扫 4 万个端口也就几十批，比"一个端口一个线程"省得多。
std::vector<int> ScanPortRange(const std::wstring& ip, int lo, int hi,
                               int timeoutMs = 250,
                               std::atomic<bool>* cancel = nullptr);

// 选中一台主机后，找出它真正可用的 adb 端点，四级：
//   0) adb devices 里已经连着 → 直接用那个序列号
//   1) mDNS（adb 自带缓存 + 自己发 DNS-SD 查询）报出来的端口
//   2) knownPorts（扫描阶段实测开着的端口）逐个 connect 并用 adb devices 复核
//   3) 只有 deepScan=true 时才扫 20000-65000 全端口段
// 注意：**不再默认猜 5555**。找不到就返回空，由界面提示用户重新打开无线调试
// 或手填端口 —— 猜一个错端口只会让用户以为"连上了"然后所有命令都超时。
std::wstring FindAdbEndpoint(const std::wstring& ip, const std::vector<int>& knownPorts,
                             int timeoutMs = kLanMaxLatencyMs, bool deepScan = false,
                             std::atomic<bool>* cancel = nullptr);

}  // namespace adb
