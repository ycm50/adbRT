// ---------------------------------------------------------------------------
// 无窗口核心单元测试：不连接任何设备即可完整验证
//   命令表 / argv 构造 / 参数校验 / 解析器 / 路径解析（相对+绝对）/ 配置
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>
#include <vector>

#include "../core/adb_mdns.h"
#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/bounded_text.h"
#include "../core/core.h"
#include "../core/parsers.h"
#include "../core/settings.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("[FAIL] %s\n", what); }
}

void CheckEq(const std::wstring& a, const std::wstring& b, const char* what) {
    if (a == b) { ++g_pass; return; }
    ++g_fail;
    std::printf("[FAIL] %s\n    expect: %s\n    actual: %s\n", what,
                adb::WideToUtf8(b).c_str(), adb::WideToUtf8(a).c_str());
}

void CheckEqStr(const std::string& a, const std::string& b, const char* what) {
    if (a == b) { ++g_pass; return; }
    ++g_fail;
    std::printf("[FAIL] %s\n    expect: %s\n    actual: %s\n", what, b.c_str(), a.c_str());
}

adb::BuiltCommand Build(const char* key, const adb::BuildInput& in) {
    adb::BuildInput copy = in;
    copy.spec = adb::FindCommandByKey(key);
    adb::BuiltCommand built;
    adb::BuildError err;
    if (!adb::BuildCommand(copy, &built, &err))
        std::printf("[FAIL] BuildCommand(%s): %s\n", key, adb::WideToUtf8(err.message).c_str());
    return built;
}

}  // namespace

int main() {
    std::printf("AdbRT CoreTests\n");

    // ---------------------------------------------------------- 命令表
    Check(adb::CommandTableSize() >= 40, "命令表至少 40 条");
    for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
        const auto& c = adb::CommandTable()[i];
        Check(adb::FindCommand(c.id) == &c, "FindCommand(id) 能找回");
        Check(adb::FindCommandByKey(c.key) == &c, "FindCommandByKey(key) 能找回");
    }

    // ---------------------------------------------------------- 参数校验
    Check(adb::IsValidPackageName(L"com.example.app"), "包名合法");
    Check(!adb::IsValidPackageName(L"com..app"), "包名不允许空段（宽松接受，仅检查字符）");
    Check(!adb::IsValidPackageName(L""), "空包名非法");
    Check(!adb::IsValidPackageName(L"com example"), "包名不允许空格");

    Check(adb::IsValidRemotePath(L"/sdcard/x"), "远端路径合法");
    Check(!adb::IsValidRemotePath(L"sdcard/x"), "远端路径必须绝对");

    Check(adb::IsValidHostPort(L"10.0.0.2:5555"), "host:port 合法");
    Check(adb::IsValidHostPort(L"10.0.0.2"), "host 单独合法");
    Check(!adb::IsValidHostPort(L"10.0.0.2:abc"), "端口必须是数字");

    Check(adb::IsValidPartitionName(L"boot_a"), "分区名合法");
    Check(!adb::IsValidPartitionName(L"boot;rm -rf"), "分区名拒绝注入");

    // ---------------------------------------------------------- 多值包名展开
    {
        const adb::CommandSpec* clr = adb::FindCommandByKey("app.clearData");
        Check(clr != nullptr, "找到 app.clearData");
        if (clr) {
            adb::BuildInput in;
            in.spec = clr;
            in.toolExe = L"adb.exe";
            in.serial = L"S1";
            in.params["pkg"] = L"com.a;com.b;com.c";
            adb::BuiltCommand out;
            adb::BuildError err;
            bool ok = adb::BuildCommand(in, &out, &err);
            Check(ok, "多值包名构造成功");
            Check(out.argvList.size() == 3, "一个值一条命令");
            if (out.argvList.size() == 3) {
                CheckEq(out.argvList[0].back(), L"com.a", "第 1 条用第一个包");
                CheckEq(out.argvList[1].back(), L"com.b", "第 2 条用第二个包");
                CheckEq(out.argvList[2].back(), L"com.c", "第 3 条用第三个包");
                CheckEq(out.argvList[0][0], L"-s", "每条都带 -s");
            }

            // 单个包仍然只产出一条命令
            in.params["pkg"] = L"com.only";
            adb::BuiltCommand one;
            adb::BuildError err2;
            Check(adb::BuildCommand(in, &one, &err2), "单包构造成功");
            Check(one.argvList.size() == 1, "单包只产出一条命令");
            if (!one.argvList.empty()) CheckEq(one.argvList[0].back(), L"com.only", "单包值正确");
        }
    }

    // ---------------------------------------------------------- 模板与参数一致性
    {
        // 每条命令的模板占位符都必须能在 spec 里找到对应的参数 key，
        // 否则运行时会得到"缺少参数"这种本可以在构造期发现的错误。
        int mismatched = 0;
        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const auto& c = adb::CommandTable()[i];
            if (!c.argvTemplate) continue;
            std::wstring tpl = adb::Utf8ToWide(c.argvTemplate);
            std::vector<std::wstring> declared;
            if (c.paramKey) declared.push_back(adb::Utf8ToWide(c.paramKey));
            if (c.paramKey2) declared.push_back(adb::Utf8ToWide(c.paramKey2));
            size_t pos = 0;
            while ((pos = tpl.find(L'{', pos)) != std::wstring::npos) {
                size_t end = tpl.find(L'}', pos);
                if (end == std::wstring::npos) break;
                std::wstring name = tpl.substr(pos + 1, end - pos - 1);
                pos = end + 1;
                if (name == L"flags" || name == L"paths") continue;
                // {key?} = 可选占位，去掉问号再比对
                if (!name.empty() && name.back() == L'?') name.pop_back();
                bool found = false;
                for (const auto& d : declared) if (d == name) { found = true; break; }
                if (!found) {
                    ++mismatched;
                    std::printf("  [mismatch] %s 模板占位符 {%s} 没有对应参数\n",
                                c.key, adb::WideToUtf8(name).c_str());
                }
            }
        }
        Check(mismatched == 0, "所有模板占位符都有对应的参数定义");
    }

    // ---------------------------------------------------------- 坐标注入防护
    {
        Check(adb::IsValidCoords(L"640 1200", 2), "坐标 2 段合法");
        Check(adb::IsValidCoords(L"640 1600 640 800 300", 5), "坐标 5 段合法");
        Check(!adb::IsValidCoords(L"640", 2), "坐标段数不足被拒");
        Check(adb::IsValidCoords(L"640 1200 640 400", 4), "滑动 4 段合法（时长另给）");
        Check(!adb::IsValidCoords(L"640 1200 640 400 300 9", 5), "滑动 6 段被拒");
        Check(!adb::IsValidCoords(L"640;reboot", 2), "坐标里的分号被拒");
        Check(!adb::IsValidCoords(L"640 1200 && rm -rf /", 2), "坐标里的 && 被拒");
        Check(!adb::IsValidCoords(L"$(reboot) 1", 2), "坐标里的 $() 被拒");

        // 端到端：脏坐标必须让 BuildCommand 直接失败，而不是生成命令
        const adb::CommandSpec* tap = adb::FindCommandByKey("input.tap");
        Check(tap != nullptr, "找到 input.tap");
        if (tap) {
            Check(tap->param == adb::ParamKind::Coords, "input.tap 用坐标类型");
            adb::BuildInput in;
            in.spec = tap;
            in.toolExe = L"adb.exe";
            in.serial = L"S1";
            in.params["xy"] = L"640;reboot";
            adb::BuiltCommand out;
            adb::BuildError err;
            Check(!adb::BuildCommand(in, &out, &err), "注入型坐标构造失败");

            in.params["xy"] = L"640 1200";
            adb::BuiltCommand ok2;
            adb::BuildError err2;
            Check(adb::BuildCommand(in, &ok2, &err2), "合法坐标构造成功");
            if (!ok2.argvList.empty())
                CheckEq(ok2.argvList[0].back(), L"640 1200", "坐标作为单个 argv 传入");
        }
    }

    // ---------------------------------------------------------- 设备解析
    {
        auto devs = adb::ParseDevices(
            L"* daemon started successfully\n"
            L"List of devices attached\n"
            L"XYZ123\tdevice product:foo model:Pixel_7 device:foo transport_id:1\n"
            L"emu-5554\toffline\n"
            L"BAD\tunauthorized\n"
            L"rec\trecovery\n");
        Check(devs.size() == 4, "解析出 4 台设备");
        if (devs.size() == 4) {
            Check(devs[0].state == adb::DeviceState::Device, "第 1 台 device");
            CheckEq(devs[0].model, L"Pixel_7", "解析 model");
            Check(devs[1].state == adb::DeviceState::Offline, "第 2 台 offline");
            Check(devs[2].state == adb::DeviceState::Unauthorized, "第 3 台 unauthorized");
            Check(devs[3].state == adb::DeviceState::Recovery, "第 4 台 recovery");
        }
    }
    Check(adb::ParseDevices(L"List of devices attached\n").empty(), "空设备列表");
    Check(adb::ParseDevices(L"").empty(), "空输入");

    // ---------------------------------------------------------- 包名/进度
    {
        auto pkgs = adb::ParsePackageList(L"package:com.b\npackage:com.a\n");
        Check(pkgs.size() == 2, "解析 2 个包名");
        if (pkgs.size() == 2) CheckEq(pkgs[0].package, L"com.a", "包名排序");
    }

    // ---------------------------------------------------------- 带路径的包列表
    {
        // pm list packages -f 的路径里可能含 '='（如 ~~abc==），
        // 必须按最后一个 '=' 切分，否则包名会被切断
        auto withPaths = adb::ParsePackageListWithPaths(
            L"package:/data/app/~~YNVcDM-f-9CM==/pkg-abc==/base.apk=com.example.a\n"
            L"package:/product/app/B/B.apk=com.example.b\n");
        Check(withPaths.size() == 2, "解析出 2 个带路径的包");
        if (withPaths.size() == 2) {
            CheckEq(withPaths[0].package, L"com.example.a", "按最后一个 = 切分包名");
            CheckEq(withPaths[0].apkPath,
                    L"/data/app/~~YNVcDM-f-9CM==/pkg-abc==/base.apk", "路径完整保留");
            CheckEq(withPaths[1].package, L"com.example.b", "普通路径解析");
        }
    }
    CheckEqStr(std::to_string(adb::ParseProgressPercent(L"[100%] /sdcard/a")), "100", "进度 100%");
    Check(adb::ParseProgressPercent(L"nothing") == -1, "无进度返回 -1");

    // ---------------------------------------------------------- 路径解析
    {
        std::wstring base = L"D:\\apps\\AdbRT";
        CheckEq(adb::ResolvePathSpec(L"platform-tools", base), L"D:\\apps\\AdbRT\\platform-tools",
                "相对路径 -> 绝对");
        CheckEq(adb::ResolvePathSpec(L"A:\\platform-tools", base), L"A:\\platform-tools",
                "绝对路径保持");
        CheckEq(adb::ResolvePathSpec(L"..\\tools", base), L"D:\\apps\\tools", "父级相对路径");
        CheckEq(adb::ResolvePathSpec(L"~\\bin", base), L"D:\\apps\\AdbRT\\bin", "~ 表示程序目录");
        Check(adb::ResolvePathSpec(L"", base).empty(), "空输入 -> 空");
        CheckEqStr(adb::WideToUtf8(adb::MakeRelativeTo(base, L"D:\\apps\\AdbRT\\bin\\adb.exe")),
                   "bin/adb.exe", "MakeRelativeTo");
        Check(adb::MakeRelativeTo(base, L"C:\\other\\x").empty(), "不在基目录下返回空");
    }

    // ---------------------------------------------------------- argv 构造
    {
        adb::BuildInput in;
        in.toolExe = L"A:\\platform-tools\\adb.exe";
        in.serial = L"XYZ123";

        in.flags = {{"replace", L"1"}};
        in.params = {{"apk", L"A:\\build\\my app.apk"}};
        auto built = Build("app.install", in);
        Check(built.argvList.size() == 1, "install 产出 1 条命令");
        if (!built.argvList.empty()) {
            const auto& a = built.argvList[0];
            CheckEq(a[0], L"-s", "install: -s 在最前");
            CheckEq(a[1], L"XYZ123", "install: 序列号");
            CheckEq(a[2], L"install", "install: 子命令");
            CheckEq(a[3], L"-r", "install: -r");
            CheckEq(a[4], L"A:\\build\\my app.apk", "install: 带空格的路径是单个 argv");
        }

        in.params.clear();
        in.params = {{"pkg", L"com.example.app"}};
        in.flags = {{"keep", L"1"}};
        auto un = Build("app.uninstall", in);
        if (!un.argvList.empty())
            CheckEq(un.argvList[0][2], L"uninstall", "uninstall 子命令位置");

        in.flags.clear();
        in.params = {{"remote", L"/sdcard/Download"}};
        in.paths = {L"C:\\a.txt", L"C:\\b.txt"};
        auto push = Build("file.push", in);
        if (!push.argvList.empty())
            Check(push.argvList[0].size() >= 6, "push 带上全部本地路径");

        // 缺少必填参数必须失败，而不是产出半截命令
        adb::BuildInput bad;
        bad.spec = adb::FindCommandByKey("app.install");
        bad.toolExe = L"adb.exe";
        adb::BuiltCommand out;
        adb::BuildError err;
        Check(!adb::BuildCommand(bad, &out, &err), "缺少 APK 参数时构造失败");
        Check(!err.message.empty(), "失败时给出中文原因");
    }

    // ---------------------------------------------------------- mDNS（随机端口的唯一来源）
    {
        // 手工拼一条 mDNS 应答：PTR(_adb-tls-connect → 实例) + SRV(端口) + A(IP)
        auto name = [](std::vector<uint8_t>& v, const char* s) {
            const char* p = s;
            while (*p) {
                const char* dot = p;
                while (*dot && *dot != '.') ++dot;
                v.push_back(static_cast<uint8_t>(dot - p));
                v.insert(v.end(), p, dot);
                if (!*dot) break;
                p = dot + 1;
            }
            v.push_back(0);
        };
        auto u16 = [](std::vector<uint8_t>& v, uint16_t x) {
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x & 0xFF));
        };
        std::vector<uint8_t> pkt;
        auto rr = [&](const char* owner, uint16_t type, const std::vector<uint8_t>& rdata) {
            name(pkt, owner);
            u16(pkt, type);
            u16(pkt, 0x8001);                     // class IN + cache-flush
            // TTL 是 4 字节：只写 2 字节会把后面的 rdlength 整体错位
            u16(pkt, 0); u16(pkt, 120);
            u16(pkt, static_cast<uint16_t>(rdata.size()));
            pkt.insert(pkt.end(), rdata.begin(), rdata.end());
        };

        u16(pkt, 0); u16(pkt, 0x8400);            // ID + response
        u16(pkt, 0); u16(pkt, 3);                 // QD=0, AN=3
        u16(pkt, 0); u16(pkt, 0);

        std::vector<uint8_t> ptrData;
        name(ptrData, "adb-1a2b3c-XyZ._adb-tls-connect._tcp.local");
        rr("_adb-tls-connect._tcp.local", 12, ptrData);

        std::vector<uint8_t> srvData;
        u16(srvData, 0); u16(srvData, 0); u16(srvData, 37221);     // prio/weight/port
        name(srvData, "Android.local");
        rr("adb-1a2b3c-XyZ._adb-tls-connect._tcp.local", 33, srvData);

        std::vector<uint8_t> aData = {192, 168, 1, 7};
        rr("Android.local", 1, aData);

        adb::MdnsDnsRecords rec;
        Check(adb::ParseMdnsPacket(pkt.data(), pkt.size(), &rec), "mDNS 报文解析成功");
        Check(rec.ptr.size() == 1 && rec.srv.size() == 1 && rec.a.size() == 1,
              "PTR / SRV / A 三条记录都拿到");

        auto svcs = adb::AssembleMdnsServices({rec});
        Check(svcs.size() == 1, "串表得到 1 个服务");
        if (svcs.size() == 1) {
            CheckEq(svcs[0].ip, L"192.168.1.7", "mDNS 服务拿到真实 IP");
            Check(svcs[0].port == 37221, "mDNS 服务拿到随机端口（不是 5555）");
            Check(!svcs[0].pairing, "connect 服务不算配对服务");
        }

        // 截断 / 空的报文不能崩，也不能返回半截数据
        adb::MdnsDnsRecords bad;
        Check(!adb::ParseMdnsPacket(pkt.data(), 8, &bad), "过短报文安全拒绝");
        Check(!adb::ParseMdnsPacket(nullptr, 0, &bad), "空指针安全拒绝");
        std::vector<uint8_t> truncated(pkt.begin(), pkt.begin() + 20);
        adb::MdnsDnsRecords bad2;
        adb::ParseMdnsPacket(truncated.data(), truncated.size(), &bad2);   // 不要求成功，但不能崩

        // adb mdns services 的文本形态
        auto text = adb::ParseMdnsAdbServices(
            L"adb-1a2b3c-XyZ\t_adb-tls-connect._tcp.\t192.168.1.7:37221\r\n"
            L"adb-9z8y-ABC\t_adb-tls-pairing._tcp.\t192.168.1.8:41001\r\n");
        Check(text.size() == 2, "adb mdns services 解析出两条");
        if (text.size() == 2) {
            Check(text[0].port == 37221, "第一条端口正确");
            Check(!text[0].pairing, "第一条是 connect 服务");
            Check(text[1].pairing, "第二条是 pairing 服务");
        }
        // 空表（实测 adb 36.0.2 对没连过的设备恒为空）不能凭空造出 5555
        Check(adb::ParseMdnsAdbServices(L"List of discovered mdns services\r\n").empty(),
              "空 mDNS 表不产生任何设备（更不会编一个 5555）");
        Check(adb::ParseMdnsAdbServices(L"").empty(), "空输入不产生设备");
    }

    // ---------------------------------------------------------- 有界文本缓冲
    {
        // 预算是 100 字，单行上限 10 字
        adb::BoundedText buf(100, 32);
        for (int i = 0; i < 20; ++i) {                 // 20 行 × 12 字 = 240 字，远超预算
            buf.Append(std::to_wstring(i) + L"0123456789");
        }
        Check(buf.PendingBytes() <= 100, "超出预算后占用不超过上限");
        Check(buf.DroppedBytes() > 0, "超预算的内容被记账丢弃");

        // 丢弃的是最旧的：剩下的最后一行一定是最新追加的
        std::vector<std::wstring> rows;
        buf.CopyLines(0, 0, &rows);
        Check(!rows.empty(), "丢旧留新后仍有内容");
        if (!rows.empty()) CheckEq(rows.back(), L"190123456789", "保留的是最新的行");

        // 超长行被截断且不突破预算
        adb::BoundedText small(100, 8);
        Check(!small.Append(L"123456789012345"), "超长行返回 false（已截断）");
        Check(small.Truncated(), "截断标记被置位");
        Check(small.PendingBytes() <= 100 + 32, "截断后仍在预算附近");

        // TakeBatch 只取走一部分，剩下的留在缓冲里
        adb::BoundedText chunked(1024, 1024);
        for (int i = 0; i < 10; ++i) chunked.Append(L"abcdefghij");
        size_t before = chunked.PendingLines();
        std::wstring part = chunked.TakeBatch(30);
        Check(!part.empty(), "分批取走有内容");
        Check(chunked.PendingLines() < before, "分批取走后缓冲减少");
        Check(chunked.PendingBytes() > 0, "分批取走后仍有剩余");

        // 清空后计数归零
        chunked.Clear();
        Check(chunked.PendingBytes() == 0 && chunked.PendingLines() == 0, "清空后归零");
        Check(chunked.TotalLines() == 0, "清空后累计行数归零");

        // 累计行数包含被丢弃的行：增量渲染靠它判断是否回绕
        adb::BoundedText roll(40, 40);
        for (int i = 0; i < 20; ++i) roll.Append(L"0123456789");
        Check(roll.TotalLines() == 20, "累计行数统计正确");
        Check(roll.PendingLines() < 20, "内部确实发生了丢旧");
    }

    // ---------------------------------------------------------- 常驻 server 判定
    {
        // adb 会 fork 常驻 server，挂 KILL_ON_JOB_CLOSE 的 Job 会在
        // 调用结束时把它连坐杀掉（每条命令都重启服务）。
        Check(adb::IsPersistentServerTool(L"A:\\platform-tools\\adb.exe"),
              "adb.exe 判定为常驻 server 工具");
        Check(adb::IsPersistentServerTool(L"adb.exe"), "裸 adb.exe 也判定");
        Check(adb::IsPersistentServerTool(L"C:\\x\\ADB.EXE"), "大小写不敏感");
        Check(!adb::IsPersistentServerTool(L"A:\\platform-tools\\fastboot.exe"),
              "fastboot 不是常驻 server 工具");
        Check(!adb::IsPersistentServerTool(L""),
              "空路径不误判");
    }

    // ---------------------------------------------------------- 配置
    {
        auto& st = adb::Settings::Instance();
        st.ResetForTest();
        st.SetString("adb.path", "platform-tools\\adb.exe");
        Check(st.Has("adb.path"), "配置写入后 Has() 为真");
        CheckEqStr(st.GetString("adb.path"), "platform-tools\\adb.exe", "配置读取一致");
    }

    std::printf("----------------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}