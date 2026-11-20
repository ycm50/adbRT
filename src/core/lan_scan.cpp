// FD_SETSIZE 默认只有 64，批量 select 需要更大；本文件是唯一用 select 的地方。
// 4096 是 Winsock 的实际上限附近：再大 select 会直接返回错误。
#ifndef FD_SETSIZE
#define FD_SETSIZE 4096
#endif

#include "lan_scan.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "core.h"
#include "device.h"
#include "parsers.h"

namespace adb {

namespace {

// 一次 select 最多盯多少个套接字。
// 扫描 2 万个端口：512 一批要 40 轮、2048 一批只要 10 轮，
// 而每轮固定要等一个超时周期，所以批量越大越快。2048 是实测的甜点：
// 再大就会触发对端/本机的半开连接上限，反而丢掉真实开放的端口。
constexpr size_t kBatch = 2048;

std::wstring IpToString(DWORD ip) {
    return Format(L"%lu.%lu.%lu.%lu", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
                  (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

std::wstring PrefixOf(const std::wstring& ip) {
    size_t last = ip.rfind(L'.');
    return last == std::wstring::npos ? ip : ip.substr(0, last);
}

// Winsock 只需初始化一次；进程退出时系统自己回收，不必 WSACleanup
void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

struct Target {
    DWORD ip = 0;
    int   port = 0;
    int   tag = 0;          // 调用方自己用的下标（这里放 host 号）
};

struct BatchOut {
    PortState state = PortState::Filtered;
    int       rttMs = -1;   // 连上/RST 所用的毫秒数
};

// ---------------------------------------------------------------------------
// 批量非阻塞 connect
//   一次把几百个套接字全发出去，再用一个 select 等结果。
//   比"一个端口一个线程"省得多：2 万个端口也只有一个线程、几十轮 select。
//   返回 Open / Refused（对端回了 RST，说明主机活着）/ Filtered（超时）
// ---------------------------------------------------------------------------
std::vector<BatchOut> BatchConnect(const std::vector<Target>& targets, int timeoutMs,
                                   std::atomic<bool>* cancel) {
    std::vector<BatchOut> res(targets.size());
    if (targets.empty()) return res;
    EnsureWinsock();
    if (timeoutMs < 1) timeoutMs = 1;

    for (size_t base = 0; base < targets.size(); base += kBatch) {
        const size_t end = std::min(base + kBatch, targets.size());
        const int n = static_cast<int>(end - base);

        std::vector<SOCKET> socks(static_cast<size_t>(n), INVALID_SOCKET);
        std::vector<uint64_t> started(static_cast<size_t>(n), 0);
        std::vector<int> pending;
        pending.reserve(static_cast<size_t>(n));

        // ① 全部发起非阻塞 connect
        for (int i = 0; i < n; ++i) {
            if (cancel && cancel->load()) return res;
            SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            u_long nonBlocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(targets[base + static_cast<size_t>(i)].port));
            addr.sin_addr.S_un.S_addr = targets[base + static_cast<size_t>(i)].ip;

            uint64_t t0 = ::GetTickCount64();
            if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                res[base + static_cast<size_t>(i)].state = PortState::Open;
                res[base + static_cast<size_t>(i)].rttMs = static_cast<int>(::GetTickCount64() - t0);
                ::closesocket(s);
                continue;
            }
            int err = ::WSAGetLastError();
            if (err == WSAECONNREFUSED) {
                res[base + static_cast<size_t>(i)].state = PortState::Refused;
                res[base + static_cast<size_t>(i)].rttMs = static_cast<int>(::GetTickCount64() - t0);
                ::closesocket(s);
                continue;
            }
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                ::closesocket(s);
                continue;
            }
            socks[static_cast<size_t>(i)] = s;
            started[static_cast<size_t>(i)] = t0;
            pending.push_back(i);
        }

        // ② 一轮轮 select，谁先有结果就先结算谁
        const uint64_t deadline = ::GetTickCount64() + static_cast<uint64_t>(timeoutMs);
        while (!pending.empty()) {
            if (cancel && cancel->load()) break;
            uint64_t now = ::GetTickCount64();
            if (now >= deadline) break;

            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            for (int i : pending) {
                SOCKET s = socks[static_cast<size_t>(i)];
                if (s == INVALID_SOCKET) continue;
                FD_SET(s, &wr);
                FD_SET(s, &ex);
            }

            uint64_t left = deadline - now;
            timeval tv{};
            tv.tv_sec = static_cast<long>(left / 1000);
            tv.tv_usec = static_cast<long>((left % 1000) * 1000);

            int rc = ::select(0, nullptr, &wr, &ex, &tv);
            if (rc <= 0) break;                     // 超时或出错，剩下的都算 Filtered

            std::vector<int> still;
            still.reserve(pending.size());
            for (int i : pending) {
                SOCKET s = socks[static_cast<size_t>(i)];
                if (s == INVALID_SOCKET) continue;
                if (!FD_ISSET(s, &wr) && !FD_ISSET(s, &ex)) { still.push_back(i); continue; }
                int so = 0;
                int len = sizeof(so);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
                size_t idx = base + static_cast<size_t>(i);
                if (so == 0) res[idx].state = PortState::Open;
                else if (so == WSAECONNREFUSED) res[idx].state = PortState::Refused;
                res[idx].rttMs = static_cast<int>(::GetTickCount64() - started[static_cast<size_t>(i)]);
                ::closesocket(s);
                socks[static_cast<size_t>(i)] = INVALID_SOCKET;
            }
            pending.swap(still);
        }

        for (int i : pending) {
            SOCKET s = socks[static_cast<size_t>(i)];
            if (s != INVALID_SOCKET) ::closesocket(s);
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// ARP 蜂群：把 /24 全段的邻居一次性问出来
//
//   逐个调 SendARP 是不行的 —— 它每个地址要等 1~2 秒（不可配），
//   254 个就算 254 线程也要好几秒。
//
//   正确做法：对这 254 个地址各发一个 UDP 包（不存在的端口 9），
//   内核会为每个目标并行发 ARP 请求；等 150ms 直接查邻居表。
//   实测整段 125ms 出结果，而且只拿得到真正应答的主机。
//
//   返回的地址必须"有非零 MAC 且 phyLen>=6"才算数：
//   Windows 对没应答的地址也会塞一条全零 MAC 的占位记录，
//   不过滤的话 253 个假主机全都会被当成在线。
// ---------------------------------------------------------------------------
std::vector<DWORD> ArpSwarm(DWORD baseIp, int timeoutMs, std::atomic<bool>* cancel) {
    std::vector<DWORD> found;
    EnsureWinsock();

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);          // discard 端口，没人会听
    auto* b = reinterpret_cast<const uint8_t*>(&baseIp);
    for (int h = 1; h <= 254; ++h) {
        if (cancel && cancel->load()) { ::closesocket(s); return found; }
        auto* o = reinterpret_cast<uint8_t*>(&to.sin_addr.S_un.S_addr);
        o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = static_cast<uint8_t>(h);
        ::sendto(s, "x", 1, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    ::closesocket(s);

    // 等内核把 ARP 应答收回来。分几段轮询，早应答的能早点被看到。
    const uint64_t deadline = ::GetTickCount64() + static_cast<uint64_t>(timeoutMs);
    while (true) {
        if (cancel && cancel->load()) break;

        ULONG size = 64 * 1024;
        std::vector<uint8_t> buf(size);
        ULONG rc = ::GetIpNetTable(reinterpret_cast<PMIB_IPNETTABLE>(buf.data()), &size, FALSE);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            buf.resize(size);
            rc = ::GetIpNetTable(reinterpret_cast<PMIB_IPNETTABLE>(buf.data()), &size, FALSE);
        }
        if (rc == NO_ERROR) {
            found.clear();
            auto* t = reinterpret_cast<PMIB_IPNETTABLE>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& row = t->table[i];
                if (row.dwType == MIB_IPNET_TYPE_INVALID) continue;
                if (row.dwPhysAddrLen < 6) continue;
                bool allZero = true;
                for (int k = 0; k < 6; ++k) if (row.bPhysAddr[k]) { allZero = false; break; }
                if (allZero) continue;            // 没应答的占位记录
                const auto* ab = reinterpret_cast<const uint8_t*>(&row.dwAddr);
                if (ab[0] != b[0] || ab[1] != b[1] || ab[2] != b[2]) continue;
                found.push_back(row.dwAddr);
            }
        }
        if (::GetTickCount64() >= deadline) break;
        ::Sleep(50);
    }
    return found;
}

}  // namespace

std::vector<LocalNet> ListLocalNets() {
    std::vector<LocalNet> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        std::wstring name = a->FriendlyName ? a->FriendlyName : L"";
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            DWORD ip = sa->sin_addr.s_addr;   // 内存序即 192.168.1.x 的书写序
            std::wstring s = IpToString(ip);
            if (StartsWith(s, L"127.") || StartsWith(s, L"169.254.")) continue;

            LocalNet n;
            n.adapter = name;
            n.localIp = s;
            n.prefix = PrefixOf(s);
            n.prefixLen = 24;
            if (a->FirstGatewayAddress &&
                a->FirstGatewayAddress->Address.lpSockaddr &&
                a->FirstGatewayAddress->Address.lpSockaddr->sa_family == AF_INET) {
                auto* gw = reinterpret_cast<sockaddr_in*>(a->FirstGatewayAddress->Address.lpSockaddr);
                n.gateway = IpToString(gw->sin_addr.s_addr);
            }
            // 同一个网段只留一条
            bool dup = false;
            for (const auto& e : out) if (e.prefix == n.prefix) { dup = true; break; }
            if (!dup) out.push_back(n);
        }
    }

    // 排序：真网卡（WLAN / 以太网 / Wi-Fi）排前面，
    // WSL / VMware / VirtualBox / Hyper-V 这类虚拟网卡排后面 —— 用户要连的手机只会在真网卡里。
    auto isVirtual = [](const std::wstring& name) {
        static const wchar_t* kMarks[] = {L"vEthernet", L"WSL", L"VMware", L"VirtualBox",
                                          L"Hyper-V", L"Loopback", L"TAP-", L"Tailscale",
                                          L"ZeroTier", L"Radmin"};
        for (const wchar_t* m : kMarks) {
            if (name.find(m) != std::wstring::npos) return true;
        }
        return false;
    };
    std::stable_sort(out.begin(), out.end(), [&](const LocalNet& a, const LocalNet& b) {
        bool va = isVirtual(a.adapter), vb = isVirtual(b.adapter);
        if (va != vb) return !va;
        // 都是真网卡时，有网关的优先（说明真的接了路由器）
        if (a.gateway.empty() != b.gateway.empty()) return !a.gateway.empty();
        return false;
    });
    return out;
}

PortState ProbeTcpPortEx(const std::wstring& ip, int port, int timeoutMs) {
    EnsureWinsock();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return PortState::Filtered;

    u_long nonBlocking = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (::InetPtonW(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::closesocket(s);
        return PortState::Filtered;
    }

    PortState state = PortState::Filtered;
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        state = PortState::Open;
    } else {
        int err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv{};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            int rc = ::select(0, nullptr, &wr, &ex, &tv);
            if (rc > 0) {
                int so = 0;
                int len = sizeof(so);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len);
                state = (so == 0) ? PortState::Open : PortState::Refused;
            }   // rc == 0 → 超时，保持 Filtered
        } else if (err == WSAECONNREFUSED) {
            state = PortState::Refused;
        }
    }
    ::closesocket(s);
    return state;
}

bool ProbeTcpPort(const std::wstring& ip, int port, int timeoutMs) {
    return ProbeTcpPortEx(ip, port, timeoutMs) == PortState::Open;
}

std::vector<int> ScanPortRange(const std::wstring& ip, int lo, int hi, int timeoutMs,
                               std::atomic<bool>* cancel) {
    std::vector<int> open;
    if (lo < 1) lo = 1;
    if (hi > 65535) hi = 65535;
    if (lo >= hi) return open;

    in_addr addr{};
    if (::InetPtonW(AF_INET, ip.c_str(), &addr) != 1) return open;

    for (int base = lo; base < hi; base += static_cast<int>(kBatch)) {
        if (cancel && cancel->load()) break;
        int end = std::min(hi, base + static_cast<int>(kBatch));

        std::vector<Target> batch;
        batch.reserve(static_cast<size_t>(end - base));
        for (int p = base; p < end; ++p) batch.push_back(Target{addr.S_un.S_addr, p, p});

        std::vector<BatchOut> st = BatchConnect(batch, timeoutMs, cancel);
        for (size_t i = 0; i < st.size(); ++i)
            if (st[i].state == PortState::Open) open.push_back(batch[i].port);
    }
    std::sort(open.begin(), open.end());
    return open;
}

// 已经连上过的设备直接问 adb devices，不用再扫端口
std::wstring EndpointFromDeviceList(const std::wstring& ip) {
    RunOptions o;
    o.timeoutMs = 6000;
    auto r = ::adb::DeviceSession::Instance().Run({L"devices"}, o);
    if (!r.started) return {};
    for (const auto& d : ParseDevices(r.OutW())) {
        if (d.state != DeviceState::Device) continue;      // offline / unauthorized 不算
        if (d.serial.find(ip + L":") == 0) return d.serial;
    }
    return {};
}

// connect 之后用 adb devices 复核，比匹配 "connected to" 字样可靠得多：
// 已经连着的端口回的是 "already connected to ..."，字样匹配容易误判。
bool DeviceReady(const std::wstring& endpoint) {
    RunOptions o;
    o.timeoutMs = 6000;
    auto r = ::adb::DeviceSession::Instance().Run({L"devices"}, o);
    if (!r.started) return false;
    for (const auto& d : ParseDevices(r.OutW()))
        if (d.serial == endpoint) return d.state == DeviceState::Device;
    return false;
}

std::wstring FindAdbEndpoint(const std::wstring& ip, int timeoutMs,
                             std::atomic<bool>* cancel) {
    if (ip.empty()) return {};

    // 0) 可能已经连着了（上一轮扫描连上过、或者用户手动连过）
    std::wstring known = EndpointFromDeviceList(ip);
    if (!known.empty()) return known;
    if (cancel && cancel->load()) return {};

    // 1) mDNS 先问：一次 adb mdns services 只要 20ms，无线调试的端口只能从这里拿到。
    //    注意它只在 adb server 刚起/handle 变化后短时间有缓存，问不到也正常。
    RunOptions o;
    o.timeoutMs = 6000;
    o.cancel = cancel;
    auto r = ::adb::DeviceSession::Instance().Run({L"mdns", L"services"}, o);
    if (r.started) {
        for (const auto& cand : ParseConnectCandidates(r.OutW())) {
            size_t colon = cand.rfind(L':');
            if (colon == std::wstring::npos) continue;
            if (cand.substr(0, colon) != ip) continue;
            if (DeviceReady(cand)) return cand;
        }
    }
    if (cancel && cancel->load()) return {};

    // 2) 默认端口（"adb tcpip 5555" 之后的设备都在这儿）
    if (ProbeTcpPort(ip, 5555, timeoutMs)) return ip + L":5555";
    if (cancel && cancel->load()) return {};

    // 3) 兜底：扫 Android 无线调试的随机端口段。
    //    这里用固定的 250ms 连接超时，不跟"局域网时延阈值"混用 ——
    //    50ms 是判断"这台机器在不在我局域网"的标准，扫端口是点对点的，
    //    给太短会漏掉真实开放的端口（实测 50ms 时好时坏）。
    const int portTimeout = 250;
    std::vector<int> ports = ScanPortRange(ip, 30000, 50000, portTimeout, cancel);
    for (int p : ports) {
        if (cancel && cancel->load()) return {};
        std::wstring endpoint = ip + L":" + std::to_wstring(p);
        RunOptions co;
        co.timeoutMs = 6000;
        co.cancel = cancel;
        ::adb::DeviceSession::Instance().Run({L"connect", endpoint}, co);
        if (DeviceReady(endpoint)) return endpoint;
    }
    return {};
}

ScanResult ScanLan(const std::wstring& prefix, int timeoutMs, int adbPort,
                   std::atomic<bool>* cancel) {
    ScanResult result;
    if (prefix.empty()) return result;
    if (timeoutMs < 1) timeoutMs = 1;
    if (timeoutMs > kLanMaxLatencyMs) timeoutMs = kLanMaxLatencyMs;
    const uint64_t t0 = ::GetTickCount64();
    result.probed = 254;

    // 每台主机的结论：0 = 未知，1 = 在线
    std::vector<char> alive(256, 0);
    std::vector<char> hasAdb(256, 0);
    std::vector<char> settled(256, 0);   // 已经有结论的，不必再 ICMP
    std::vector<int>  rtt(256, -1);

    in_addr baseAddr{};
    if (::InetPtonW(AF_INET, (prefix + L".0").c_str(), &baseAddr) != 1) return result;
    DWORD baseIp = baseAddr.S_un.S_addr;

    // ---------------------------------------------------------------- 阶段 1
    // 批量非阻塞 connect 打 adb 端口：254 个套接字一口气发出去，一个 select 收完。
    //   Open     → 主机活着，而且开着 adb
    //   Refused  → 主机活着（回了 RST），只是没开这个端口
    // 时延超过 50ms 的一律不算（跨网段 / 丢包重传，不是同一个局域网）
    {
        std::vector<Target> batch;
        batch.reserve(254);
        for (int h = 1; h <= 254; ++h) {
            DWORD ip = 0;
            auto* b = reinterpret_cast<uint8_t*>(&baseIp);
            auto* o = reinterpret_cast<uint8_t*>(&ip);
            o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = static_cast<uint8_t>(h);
            batch.push_back(Target{ip, adbPort, h});
        }
        std::vector<BatchOut> st = BatchConnect(batch, timeoutMs, cancel);
        for (size_t i = 0; i < st.size(); ++i) {
            int h = static_cast<int>(i) + 1;
            if (st[i].rttMs > kLanMaxLatencyMs) continue;
            if (st[i].state == PortState::Open) {
                alive[h] = 1; hasAdb[h] = 1; settled[h] = 1; rtt[h] = st[i].rttMs;
            } else if (st[i].state == PortState::Refused) {
                alive[h] = 1; settled[h] = 1; rtt[h] = st[i].rttMs;
            }
        }
    }

    if (cancel && cancel->load()) {
        result.elapsedMs = ::GetTickCount64() - t0;
        return result;
    }

    // ---------------------------------------------------------------- 阶段 2
    // ARP 蜂群：一次把整段"谁在二层"问出来。
    // 这是判定"在不在同一个局域网"最可靠的手段 —— 不受手机对 ICMP 降优先级影响
    // （实测手机 ping 要 120ms 才回、还常常直接丢，而 ARP 125ms 就扫完 254 个）。
    {
        std::vector<DWORD> hosts = ArpSwarm(baseIp, timeoutMs < 150 ? 150 : timeoutMs, cancel);
        for (DWORD ip : hosts) {
            const auto* ab = reinterpret_cast<const uint8_t*>(&ip);
            int h = ab[3];
            if (h >= 1 && h <= 254) { alive[h] = 1; settled[h] = 1; }
        }
    }

    // ---------------------------------------------------------------- 阶段 4
    // 最后剩下极少数（ARP 也没回的），补一次 ICMP。
    // 手机普遍对 ICMP 降优先级，实测要 120ms+ 才回 —— 但这里已经是兜底，
    // 而且按"实测 RTT ≤ 50ms"才认，保证不会把跨网段/高延迟主机算进局域网。
    {
        std::vector<int> todo;
        for (int h = 1; h <= 254; ++h) if (!settled[h]) todo.push_back(h);
        if (!todo.empty()) {
            std::mutex mu;
            std::atomic<size_t> next{0};
            unsigned threads = std::min<unsigned>(static_cast<unsigned>(todo.size()), 254u);
            result.threads = std::max(result.threads, static_cast<int>(threads));

            auto worker = [&]() {
                HANDLE icmp = ::IcmpCreateFile();
                if (icmp == INVALID_HANDLE_VALUE) return;
                std::vector<char> reply(sizeof(ICMP_ECHO_REPLY) + 64, 0);
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= todo.size()) break;
                    if (cancel && cancel->load()) break;
                    int h = todo[i];

                    std::wstring ipStr = prefix + L"." + std::to_wstring(h);
                    IN_ADDR dst{};
                    if (::InetPtonW(AF_INET, ipStr.c_str(), &dst) != 1) continue;

                    uint64_t s0 = ::GetTickCount64();
                    DWORD r = ::IcmpSendEcho(icmp, dst.S_un.S_addr, nullptr, 0, nullptr,
                                             reply.data(), static_cast<DWORD>(reply.size()),
                                             static_cast<DWORD>(timeoutMs));
                    int wall = static_cast<int>(::GetTickCount64() - s0);
                    if (r == 0 || wall > kLanMaxLatencyMs) continue;

                    std::lock_guard<std::mutex> lk(mu);
                    alive[h] = 1;
                    rtt[h] = wall;
                }
                ::IcmpCloseHandle(icmp);
            };

            std::vector<std::thread> pool;
            pool.reserve(threads);
            for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
            for (auto& t : pool) t.join();
        }
    }

    for (int h = 1; h <= 254; ++h) {
        if (!alive[h]) continue;
        ScanHost host;
        host.ip = prefix + L"." + std::to_wstring(h);
        host.rttMs = rtt[h];
        host.adb = hasAdb[h] != 0;
        result.hosts.push_back(host);
        result.alive.push_back(host.ip);
        if (host.adb) result.adbHosts.push_back(host.ip);
    }
    SortLanHosts(&result.alive);
    SortLanHosts(&result.adbHosts);
    std::sort(result.hosts.begin(), result.hosts.end(),
              [](const ScanHost& a, const ScanHost& b) { return a.ip < b.ip; });
    result.elapsedMs = ::GetTickCount64() - t0;
    return result;
}

}  // namespace adb