#pragma once
// ---------------------------------------------------------------------------
// 局域网 mDNS（DNS-SD）发现 —— 找 Android 11+「无线调试」的随机端口
//
//   为什么不能只靠 adb mdns services：
//     adb >= 31 的 mdns 是「事件缓存」模型，只有 adb 自己已经连过的服务才会
//     出现在列表里；全新配对/首次连接的设备永远是空表。实测 36.0.2：
//     mdns check 正常，mdns services 恒为空 —— 于是上层只能猜 5555，
//     而 5555 只对 "adb tcpip 5555" 的老式无线调试成立。
//
//   所以这里自己发 DNS-SD 组播查询：
//     _adb-tls-connect._tcp.local   → 已配对、可 adb connect 的端口
//     _adb-tls-pairing._tcp.local   → 正在等待配对的端口（adb pair 用）
//     _adb._tcp.local               → 更老的实现（一并兼容）
//
//   全过程只读：只发查询、只收应答，不改任何系统设置。
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace adb {

struct MdnsAdbService {
    std::wstring ip;
    int          port = 0;
    bool         pairing = false;   // true = _adb-tls-pairing（这个端口要给 adb pair 用）
    std::wstring instance;          // 服务实例名，界面上做区分用
};

// 一条 DNS 报文里我们关心的三类记录（纯数据，方便单测）
struct MdnsDnsRecords {
    // PTR: owner（服务类型）→ target（服务实例）
    std::vector<std::pair<std::string, std::string>> ptr;
    struct Srv { std::string target; int port = 0; };
    // SRV: owner（服务实例）→ 端口 + 目标主机名
    std::vector<std::pair<std::string, Srv>> srv;
    // A: owner（主机名）→ IPv4
    std::vector<std::pair<std::string, std::wstring>> a;
};

// 解析一条 mDNS 报文（回答 + 附加段）。名字压缩指针、越界、截断报文都安全返回 false。
bool ParseMdnsPacket(const uint8_t* data, size_t len, MdnsDnsRecords* out);

// 把若干报文里的记录拼成「IP:端口」服务列表（PTR → SRV → A 链表）。
// 单测入口：不碰网络。
std::vector<MdnsAdbService> AssembleMdnsServices(const std::vector<MdnsDnsRecords>& packets);

// 解析 `adb mdns services` 的文本输出（兼容空表、老式 "_adb._tcp"）。
std::vector<MdnsAdbService> ParseMdnsAdbServices(const std::wstring& output);

// 同时走两条路：adb 自带缓存 + 自己发的 DNS-SD 组播查询，结果去重合并。
// budgetMs 是组播查询的总预算（默认 1500ms，够收两轮应答）；可取消。
std::vector<MdnsAdbService> QueryAdbMdns(int budgetMs = 1500, std::atomic<bool>* cancel = nullptr);

}  // namespace adb
