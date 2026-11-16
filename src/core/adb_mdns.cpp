#include "adb_mdns.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core.h"
#include "device.h"

namespace adb {

namespace {

// 5353 是 mDNS 固定端口。别的程序（Bonjour / adb 自己的 mdns）也可能占着，
// 所以一律带 SO_REUSEADDR 一起加入共享，拿不到就退到临时端口 + 单播应答。
constexpr int kMdnsPort = 5353;

const char* const kServiceConnect = "_adb-tls-connect._tcp.local";
const char* const kServicePairing = "_adb-tls-pairing._tcp.local";
const char* const kServiceLegacy  = "_adb._tcp.local";

void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

std::string LowerNoDot(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool IsServiceType(const std::string& lowered) {
    return lowered == kServiceConnect || lowered == kServicePairing ||
           lowered == kServiceLegacy;
}

// 读一个 DNS 名字：支持 0xC0 压缩指针（带跳转深度限制，防止报文互相指环）
bool ReadName(const uint8_t* d, size_t len, size_t* pos, std::string* out, int depth) {
    if (depth > 8) return false;
    std::string name;
    size_t p = *pos;
    for (;;) {
        if (p >= len) return false;
        const uint8_t l = d[p];
        if (l == 0) { ++p; break; }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) return false;
            const size_t off = (static_cast<size_t>(l & 0x3F) << 8) | d[p + 1];
            p += 2;
            std::string tail;
            size_t p2 = off;
            if (!ReadName(d, len, &p2, &tail, depth + 1)) return false;
            if (!name.empty() && !tail.empty()) name += ".";
            name += tail;
            *pos = p;
            *out = name;
            return true;
        }
        if (p + 1 + l > len) return false;
        if (!name.empty()) name += ".";
        name.append(reinterpret_cast<const char*>(d + p + 1), l);
        p += 1 + static_cast<size_t>(l);
    }
    *pos = p;
    *out = name;
    return true;
}

void AppendName(std::vector<uint8_t>* buf, const std::string& name) {
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        std::string label = name.substr(start, dot == std::string::npos
                                                   ? std::string::npos : dot - start);
        if (!label.empty()) {
            buf->push_back(static_cast<uint8_t>(label.size()));
            buf->insert(buf->end(), label.begin(), label.end());
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    buf->push_back(0);
}

// 组一个 DNS-SD 查询报文。unicast=true 时把问题的 class 最高位置 1（QU），
// 请求对方用单播回我们 —— 用在「5353 被别人占用、只能绑临时端口」的退路里。
std::vector<uint8_t> BuildQuery(bool unicast) {
    std::vector<uint8_t> q;
    const uint16_t flags = 0x0000;
    const uint16_t qd = 3;
    q.push_back(0); q.push_back(0);                      // ID = 0（mDNS 不用 ID）
    q.push_back(static_cast<uint8_t>(flags >> 8)); q.push_back(static_cast<uint8_t>(flags & 0xFF));
    q.push_back(static_cast<uint8_t>(qd >> 8)); q.push_back(static_cast<uint8_t>(qd & 0xFF));
    for (int i = 0; i < 6; ++i) q.push_back(0);          // AN / NS / AR 全 0
    for (const char* svc : {kServiceConnect, kServicePairing, kServiceLegacy}) {
        AppendName(&q, svc);
        q.push_back(0); q.push_back(12);                 // QTYPE = PTR
        const uint16_t cls = static_cast<uint16_t>(0x0001 | (unicast ? 0x8000 : 0));
        q.push_back(static_cast<uint8_t>(cls >> 8)); q.push_back(static_cast<uint8_t>(cls & 0xFF));
    }
    return q;
}

// 在每个有 IPv4 地址的网卡上加入 224.0.0.251 组播组。
// 不 join 就只能收到发给本机的单播应答，Android 的应答是组播，会全丢。
void JoinMulticastAllInterfaces(SOCKET s) {
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

    ip_mreq req{};
    ::InetPtonA(AF_INET, "224.0.0.251", &req.imr_multiaddr);
    int joined = 0;
    if (rc == NO_ERROR) {
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (!ua->Address.lpSockaddr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                req.imr_interface = sa->sin_addr;
                if (::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                 reinterpret_cast<const char*>(&req), sizeof(req)) == 0)
                    ++joined;
            }
        }
    }
    if (joined == 0) {
        // 枚举失败时退回「默认接口」，至少能在单网卡机器上工作
        req.imr_interface.s_addr = htonl(INADDR_ANY);
        ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<const char*>(&req), sizeof(req));
    }
}

std::wstring IpOf(const std::wstring& ip) { return ip; }

}  // namespace

bool ParseMdnsPacket(const uint8_t* d, size_t len, MdnsDnsRecords* out) {
    if (!d || !out || len < 12) return false;
    auto u16 = [&](size_t p) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint16_t>(d[p]) << 8) | d[p + 1]);
    };

    const uint16_t qd = u16(4);
    const uint32_t total = static_cast<uint32_t>(u16(6)) + u16(8) + u16(10);

    size_t pos = 12;
    for (uint16_t i = 0; i < qd; ++i) {
        std::string nm;
        if (!ReadName(d, len, &pos, &nm, 0)) return false;
        if (pos + 4 > len) return false;
        pos += 4;
    }

    bool any = false;
    for (uint32_t i = 0; i < total; ++i) {
        std::string owner;
        if (!ReadName(d, len, &pos, &owner, 0)) break;
        if (pos + 10 > len) break;
        const uint16_t type = u16(pos);
        const uint16_t rdlen = u16(pos + 8);
        const size_t rdata = pos + 10;
        if (rdata + rdlen > len) break;

        if (type == 12) {                    // PTR
            std::string target;
            size_t p = rdata;
            if (ReadName(d, len, &p, &target, 0)) out->ptr.push_back({owner, target});
            any = true;
        } else if (type == 33) {             // SRV
            if (rdlen >= 7) {
                MdnsDnsRecords::Srv srv;
                srv.port = u16(rdata + 4);
                std::string target;
                size_t p = rdata + 6;
                if (ReadName(d, len, &p, &target, 0)) srv.target = target;
                out->srv.push_back({owner, srv});
                any = true;
            }
        } else if (type == 1) {              // A
            if (rdlen == 4) {
                wchar_t ip[24];
                std::swprintf(ip, 24, L"%u.%u.%u.%u",
                              static_cast<unsigned>(d[rdata]), static_cast<unsigned>(d[rdata + 1]),
                              static_cast<unsigned>(d[rdata + 2]), static_cast<unsigned>(d[rdata + 3]));
                out->a.push_back({owner, ip});
                any = true;
            }
        }
        pos = rdata + rdlen;
    }
    return any;
}

std::vector<MdnsAdbService> AssembleMdnsServices(const std::vector<MdnsDnsRecords>& packets) {
    // 记录可能分散在多条报文里（PTR 一条、SRV+A 另一条），所以先全部收齐再串表
    struct SrvInfo { std::string target; int port = 0; };
    std::vector<std::pair<std::string, SrvInfo>> srvAll;
    std::vector<std::pair<std::string, std::wstring>> aAll;
    std::vector<std::pair<std::string, bool>> instances;   // 实例名, 是否配对服务

    for (const auto& p : packets) {
        for (const auto& e : p.srv) srvAll.push_back({LowerNoDot(e.first), {e.second.target, e.second.port}});
        for (const auto& e : p.a) aAll.push_back({LowerNoDot(e.first), e.second});
        for (const auto& e : p.ptr) {
            const std::string owner = LowerNoDot(e.first);
            if (!IsServiceType(owner)) continue;
            const std::string inst = LowerNoDot(e.second);
            const bool pairing = (owner == kServicePairing);
            bool dup = false;
            for (const auto& x : instances)
                if (x.first == inst) { dup = true; break; }
            if (!dup) instances.push_back({inst, pairing});
        }
    }

    std::vector<MdnsAdbService> out;
    for (const auto& inst : instances) {
        const SrvInfo* srv = nullptr;
        for (const auto& s : srvAll)
            if (s.first == inst.first) { srv = &s.second; break; }
        if (!srv || srv->port <= 0) continue;

        const std::string host = LowerNoDot(srv->target);
        std::wstring ip;
        for (const auto& a : aAll)
            if (a.first == host) { ip = a.second; break; }
        if (ip.empty()) continue;            // 没有 A 记录就连不上，丢掉

        bool dup = false;
        for (const auto& e : out)
            if (e.ip == IpOf(ip) && e.port == srv->port) { dup = true; break; }
        if (dup) continue;

        MdnsAdbService svc;
        svc.ip = ip;
        svc.port = srv->port;
        svc.pairing = inst.second;
        svc.instance = Utf8ToWide(inst.first);
        out.push_back(svc);
    }

    // 私网地址排前面（用户要连的手机一定在私网里）
    auto isPrivate = [](const std::wstring& ip) {
        return StartsWith(ip, L"10.") || StartsWith(ip, L"192.168.") ||
               StartsWith(ip, L"172.16.") || StartsWith(ip, L"172.17.") ||
               StartsWith(ip, L"172.18.") || StartsWith(ip, L"172.19.") ||
               StartsWith(ip, L"172.2") || StartsWith(ip, L"172.30.") ||
               StartsWith(ip, L"172.31.");
    };
    std::stable_sort(out.begin(), out.end(), [&](const MdnsAdbService& x, const MdnsAdbService& y) {
        const bool px = isPrivate(x.ip), py = isPrivate(y.ip);
        if (px != py) return px;
        return x.ip < y.ip;
    });
    return out;
}

std::vector<MdnsAdbService> ParseMdnsAdbServices(const std::wstring& output) {
    // 形如： adb-XXXX\t_adb-tls-connect._tcp.\t192.168.1.7:37221
    std::vector<MdnsAdbService> out;
    for (const auto& raw : SplitLines(output)) {
        const std::wstring line = Trim(raw);
        if (line.empty()) continue;
        const auto tok = SplitWhitespace(line);
        if (tok.empty()) continue;

        bool pairing = false;
        for (const auto& t : tok)
            if (t.find(L"_adb-tls-pairing") != std::wstring::npos) pairing = true;

        const std::wstring& cand = tok.back();
        const size_t colon = cand.rfind(L':');
        if (colon == std::wstring::npos || colon == 0 || colon + 1 >= cand.size()) continue;
        const std::wstring host = cand.substr(0, colon);
        const std::wstring portStr = cand.substr(colon + 1);
        bool ok = true;
        for (wchar_t c : host)
            if (!((c >= L'0' && c <= L'9') || c == L'.')) { ok = false; break; }
        if (!ok) continue;
        for (wchar_t c : portStr)
            if (c < L'0' || c > L'9') { ok = false; break; }
        if (!ok) continue;

        MdnsAdbService svc;
        svc.ip = host;
        svc.port = _wtoi(portStr.c_str());
        svc.pairing = pairing;
        svc.instance = tok.size() > 1 ? tok.front() : cand;
        if (svc.port <= 0) continue;

        bool dup = false;
        for (const auto& e : out)
            if (e.ip == svc.ip && e.port == svc.port) { dup = true; break; }
        if (!dup) out.push_back(svc);
    }
    return out;
}

std::vector<MdnsAdbService> QueryAdbMdns(int budgetMs, std::atomic<bool>* cancel) {
    std::vector<MdnsAdbService> out;
    if (budgetMs < 200) budgetMs = 200;
    EnsureWinsock();

    auto addAll = [&out](const std::vector<MdnsAdbService>& more) {
        for (const auto& s : more) {
            bool dup = false;
            for (const auto& e : out)
                if (e.ip == s.ip && e.port == s.port) { dup = true; break; }
            if (!dup) out.push_back(s);
        }
    };

    // ① adb 自带的缓存：便宜，有就直接用（adb 刚连过某台设备时这里有内容）
    {
        RunOptions o;
        o.timeoutMs = 1500;
        o.cancel = cancel;
        auto r = DeviceSession::Instance().Run({L"mdns", L"services"}, o);
        if (r.started) addAll(ParseMdnsAdbServices(r.OutW()));
    }
    if (cancel && cancel->load()) return out;

    // ② 自己发 DNS-SD 组播查询
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return out;

    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<u_short>(kMdnsPort));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bool shareGroup = (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    if (shareGroup) {
        JoinMulticastAllInterfaces(s);
    } else {
        // 5353 被别人独占：改绑临时端口，并在查询里置 QU 位请求单播应答
        ::closesocket(s);
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return out;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        local.sin_port = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            ::closesocket(s);
            return out;
        }
    }

    DWORD ttl = 255;
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    BOOL loop = FALSE;
    ::setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<u_short>(kMdnsPort));
    ::InetPtonA(AF_INET, "224.0.0.251", &to.sin_addr);

    const std::vector<uint8_t> query = BuildQuery(!shareGroup);
    auto sendQuery = [&]() {
        ::sendto(s, reinterpret_cast<const char*>(query.data()),
                 static_cast<int>(query.size()), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to));
    };
    sendQuery();

    // mDNS 是 UDP，丢一个查询很正常：在预算内重发三次
    const uint64_t t0 = ::GetTickCount64();
    const uint64_t resendAt[2] = {300, 900};
    size_t resent = 0;
    std::vector<MdnsDnsRecords> packets;

    for (;;) {
        if (cancel && cancel->load()) break;
        const uint64_t now = ::GetTickCount64();
        const uint64_t elapsed = now - t0;
        if (elapsed >= static_cast<uint64_t>(budgetMs)) break;
        while (resent < 2 && elapsed >= resendAt[resent]) { sendQuery(); ++resent; }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;                 // 100ms 一片，方便看取消标志与重发计时
        const int rc = ::select(0, &rd, nullptr, nullptr, &tv);
        if (rc <= 0 || !FD_ISSET(s, &rd)) continue;

        uint8_t buf[2048];
        const int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 12) continue;
        MdnsDnsRecords rec;
        if (ParseMdnsPacket(buf, static_cast<size_t>(n), &rec)) packets.push_back(std::move(rec));
    }
    ::closesocket(s);

    addAll(AssembleMdnsServices(packets));
    return out;
}

}  // namespace adb
