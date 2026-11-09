// ---------------------------------------------------------------------------
// 无窗口核心单元测试：不连接任何设备即可完整验证
//   命令表 / argv 构造 / 参数校验 / 解析器 / 路径解析（相对+绝对）/ 配置
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>
#include <vector>

#include "../core/command_builder.h"
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

    // ---------------------------------------------------------- 坐标注入防护
    {
        Check(adb::IsValidCoords(L"640 1200", 2), "坐标 2 段合法");
        Check(adb::IsValidCoords(L"640 1600 640 800 300", 5), "坐标 5 段合法");
        Check(!adb::IsValidCoords(L"640", 2), "坐标段数不足被拒");
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