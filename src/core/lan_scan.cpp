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

std::wstring IpToString(DWORD ip) {
    return Format(L"%lu.%lu.%lu.%lu", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
                  (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

std::wstring PrefixOf(const std::wstring& ip) {
    size_t last = ip.rfind(L'.');
    return last == std::wstring::npos ? ip : ip.substr(0, last);
}

// Winsock 只需初始化一次
void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    });
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

std::vector<int> ScanPortRange(const std::wstring& ip, int lo, int hi, int timeoutMs) {
    std::vector<int> open;
    if (lo < 1) lo = 1;
    if (hi > 65535) hi = 65535;
    if (lo >= hi) return open;

    std::atomic<int> next{lo};
    std::mutex mu;
    std::vector<std::thread> pool;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    unsigned threads = std::min<unsigned>(hw * 24, 512u);

    for (unsigned i = 0; i < threads; ++i) {
        pool.emplace_back([&]() {
            for (;;) {
                int p = next.fetch_add(1);
                if (p >= hi) break;
                if (ProbeTcpPortEx(ip, p, timeoutMs) == PortState::Open) {
                    std::lock_guard<std::mutex> lk(mu);
                    open.push_back(p);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    std::sort(open.begin(), open.end());
    return open;
}

std::wstring FindAdbEndpoint(const std::wstring& ip, int timeoutMs) {
    if (ip.empty()) return {};

    // 1) 默认端口（"adb tcpip 5555" 之后的设备都在这儿）
    if (ProbeTcpPort(ip, 5555, timeoutMs)) return ip + L":5555";

    // 2) mDNS：无线调试（配对码那一套）用的随机端口只能从这里问出来
    RunOptions o;
    o.timeoutMs = 8000;
    auto r = ::adb::DeviceSession::Instance().Run({L"mdns", L"services"}, o);
    if (r.started) {
        for (const auto& cand : ParseConnectCandidates(r.OutW())) {
            size_t colon = cand.rfind(L':');
            if (colon == std::wstring::npos) continue;
            if (cand.substr(0, colon) != ip) continue;
            return cand;
        }
    }

    // 3) 兜底：扫 Android 无线调试的随机端口段，逐个 open 的端口真的连一次
    std::vector<int> ports = ScanPortRange(ip, 30000, 50000, 250);
    for (int p : ports) {
        std::wstring endpoint = ip + L":" + std::to_wstring(p);
        RunOptions co;
        co.timeoutMs = 8000;
        auto cr = ::adb::DeviceSession::Instance().Run({L"connect", endpoint}, co);
        if (cr.started && (cr.OutW() + cr.ErrW()).find(L"connected to") != std::wstring::npos)
            return endpoint;
    }
    return {};
}

ScanResult ScanLan(const std::wstring& prefix, int timeoutMs, int adbPort) {
    ScanResult result;
    if (prefix.empty()) return result;

    std::mutex mu;
    std::vector<std::wstring> alive;
    std::vector<std::wstring> adb;
    std::atomic<int> next{1};

    // 每台主机做两件事：
    //   ① ICMP echo（快，很多手机会回）
    //   ② TCP 连 adb 端口（有些设备屏蔽 ICMP，但 adb 端口是开的）
    // 任一成功就算"在线"；TCP 成功直接记进 adbHosts。
    auto worker = [&]() {
        HANDLE icmp = ::IcmpCreateFile();
        char reply[sizeof(ICMP_ECHO_REPLY) + 64] = {0};
        for (;;) {
            int host = next.fetch_add(1);
            if (host > 254) break;
            std::wstring ip = prefix + L"." + std::to_wstring(host);

            // TCP 探测的三种结果：Open = 有 adb 端口；Refused = 主机活着但没开；
            // Filtered = 没响应，才需要再补一次 ICMP
            PortState st = ProbeTcpPortEx(ip, adbPort, 400);
            bool isAdb = (st == PortState::Open);
            bool isAlive = isAdb || (st == PortState::Refused);
            if (!isAlive && icmp != INVALID_HANDLE_VALUE) {
                IN_ADDR dst{};
                if (::InetPtonW(AF_INET, ip.c_str(), &dst) == 1) {
                    DWORD r = ::IcmpSendEcho(icmp, dst.S_un.S_addr, nullptr, 0, nullptr,
                                             reply, sizeof(reply),
                                             static_cast<DWORD>(timeoutMs));
                    isAlive = (r > 0);
                }
            }
            if (!isAlive) continue;

            std::lock_guard<std::mutex> lk(mu);
            alive.push_back(ip);
            if (isAdb) adb.push_back(ip);
        }
        if (icmp != INVALID_HANDLE_VALUE) ::IcmpCloseHandle(icmp);
    };

    // 并发别开太大：ICMP 一次发太多会互相挤掉超时，反而漏主机
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    unsigned threads = std::min<unsigned>(hw * 2, 16u);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    SortLanHosts(&alive);
    SortLanHosts(&adb);
    result.alive = alive;
    result.adbHosts = adb;
    return result;
}

}  // namespace adb