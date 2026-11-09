#include "command_spec.h"

#include <cstring>

namespace adb {

namespace {

constexpr FlagSpec kInstallFlags[] = {
    {"replace",  "-r", FlagKind::Toggle, nullptr, 0, true,  false},
    {"grant",    "-g", FlagKind::Toggle, nullptr, 0, false, false},
    {"downgrade","-d", FlagKind::Toggle, nullptr, 0, false, false},
    {"testonly", "-t", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr FlagSpec kMultiInstallFlags[] = {
    {"replace",  "-r", FlagKind::Toggle, nullptr, 0, true,  false},
    {"grant",    "-g", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr FlagSpec kUninstallFlags[] = {
    {"keep", "-k", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr FlagSpec kRebootFlags[] = {
    {"target", "{v}", FlagKind::Value, nullptr, 0, false, false},
};

constexpr FlagSpec kPushFlags[] = {
    {"sync", "--sync", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr FlagSpec kPullFlags[] = {
    {"preserve", "-a", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr FlagSpec kScreenrecordFlags[] = {
    {"size",     "--size={v}",                 FlagKind::Value, nullptr, 0, false, false},
    {"bitrate",  "--bit-rate={v}",             FlagKind::Value, nullptr, 0, false, false},
    {"timelimit","--time-limit={v}",           FlagKind::Value, nullptr, 0, false, false},
};

constexpr FlagSpec kLogcatFlags[] = {
    {"clear", "--clear", FlagKind::Toggle, nullptr, 0, false, false},
};

constexpr CommandSpec C(CommandId id, const char* key, GroupId g, uint16_t title,
                        SelMask sel, bool dev, bool fb, Danger dg,
                        ParamKind pk, ParamSource ps, const char* pkey,
                        ExecKind ex, const FlagSpec* fl, uint8_t fn, const char* tmpl,
                        ParamKind pk2 = ParamKind::None, ParamSource ps2 = ParamSource::None,
                        const char* pkey2 = nullptr) {
    return CommandSpec{id, key, g, title, 0, sel, dev, fb, dg, pk, ps, pkey,
                       pk2, ps2, pkey2, ex, fl, fn, tmpl};
}

}  // namespace

static const CommandSpec kCommands[] = {
    // ------------------------------------------------------------ 设备
    C(1001, "device.list", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1002, "device.select", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::Devices, "serial", ExecKind::Internal, nullptr, 0, nullptr),
    C(1003, "device.info", GroupId::Device, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1004, "device.reboot", GroupId::Device, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::CliPanel, kRebootFlags, 1,
      "reboot {flags}"),
    C(1005, "device.tcpip", GroupId::Device, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::Port, ParamSource::None, "port", ExecKind::Cli, nullptr, 0, "tcpip {port}"),
    C(1006, "device.connect", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::HostPort, ParamSource::None, "host", ExecKind::Cli, nullptr, 0,
      "connect {host}"),
    C(1007, "device.pair", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::HostPort, ParamSource::None, "host", ExecKind::Cli, nullptr, 0,
      "pair {host} {code}"),
    C(1008, "device.disconnect", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::HostPort, ParamSource::None, "host", ExecKind::Cli, nullptr, 0,
      "disconnect {host}"),
    C(1009, "device.root", GroupId::Device, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0, "root"),
    C(1010, "device.unroot", GroupId::Device, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0, "unroot"),
    C(1011, "device.reconnect", GroupId::Device, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0, "reconnect"),
    C(1012, "device.serverRestart", GroupId::Device, 0, kSelNone, false, false, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0,
      "kill-server ; start-server"),

    // ------------------------------------------------------------ 应用
    C(1101, "app.install", GroupId::App, 0, kSelApk | kSelFile, true, false, Danger::Careful,
      ParamKind::ApkFile, ParamSource::None, "apk", ExecKind::CliStream, kInstallFlags, 4,
      "install {flags} {apk}"),
    C(1102, "app.installMultiple", GroupId::App, 0, kSelApk | kSelFile, true, false,
      Danger::Careful, ParamKind::LocalPaths, ParamSource::None, "apk", ExecKind::CliStream,
      kMultiInstallFlags, 2, "install-multiple {flags} {paths}"),
    C(1103, "app.uninstall", GroupId::App, 0, kSelNone, true, false, Danger::Destructive,
      ParamKind::PackageName, ParamSource::InstalledPackagesAll, "pkg", ExecKind::Cli,
      kUninstallFlags, 1, "uninstall {flags} {pkg}"),
    C(1104, "app.start", GroupId::App, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::PackageName, ParamSource::InstalledPackages3p, "pkg", ExecKind::Cli, nullptr, 0,
      "shell monkey -p {pkg} -c android.intent.category.LAUNCHER 1"),
    C(1105, "app.forceStop", GroupId::App, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::PackageName, ParamSource::InstalledPackages3p, "pkg", ExecKind::Cli, nullptr, 0,
      "shell am force-stop {pkg}"),
    C(1106, "app.clearData", GroupId::App, 0, kSelNone, true, false, Danger::Destructive,
      ParamKind::PackageName, ParamSource::InstalledPackages3p, "pkg", ExecKind::CliPanel,
      nullptr, 0, "shell pm clear {pkg}"),
    C(1107, "app.grant", GroupId::App, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::Permission, ParamSource::Permissions, "perm", ExecKind::Cli, nullptr, 0,
      "shell pm grant {pkg} {perm}"),
    C(1108, "app.list", GroupId::App, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1109, "app.pullApk", GroupId::App, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::PackageName, ParamSource::InstalledPackages3p, "pkg", ExecKind::Internal,
      nullptr, 0, nullptr),

    // ------------------------------------------------------------ 文件
    C(1201, "file.push", GroupId::File, 0, kSelFile | kSelDir, true, false, Danger::Careful,
      ParamKind::RemotePath, ParamSource::None, "remote", ExecKind::CliStream, kPushFlags, 1,
      "push {flags} {paths} {remote}"),
    C(1202, "file.pull", GroupId::File, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::RemotePath, ParamSource::None, "remote", ExecKind::CliStream, kPullFlags, 1,
      "pull {flags} {remote} {local}"),
    C(1203, "file.browse", GroupId::File, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::RemotePath, ParamSource::None, "remote", ExecKind::Internal, nullptr, 0, nullptr),
    C(1204, "file.mkdir", GroupId::File, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::RemotePath, ParamSource::None, "remote", ExecKind::Cli, nullptr, 0,
      "shell mkdir -p {remote}"),
    C(1205, "file.delete", GroupId::File, 0, kSelNone, true, false, Danger::Destructive,
      ParamKind::RemotePath, ParamSource::None, "remote", ExecKind::CliPanel, nullptr, 0,
      "shell rm -rf {remote}"),

    // ------------------------------------------------------------ 屏幕与输入
    C(1301, "screen.capture", GroupId::Screen, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1302, "screen.record", GroupId::Screen, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::CliStream, kScreenrecordFlags, 3,
      "shell screenrecord {flags} /sdcard/adbrt-record.mp4"),
    C(1303, "input.tap", GroupId::Screen, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::Text, ParamSource::None, "xy", ExecKind::Cli, nullptr, 0,
      "shell input tap {xy}"),
    C(1304, "input.swipe", GroupId::Screen, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::Text, ParamSource::None, "xys", ExecKind::Cli, nullptr, 0,
      "shell input swipe {xys}"),
    C(1305, "input.text", GroupId::Screen, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::Text, ParamSource::None, "text", ExecKind::Cli, nullptr, 0,
      "shell input text {text}"),
    C(1306, "input.key", GroupId::Screen, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::KeyCode, ParamSource::KeyEvents, "key", ExecKind::Cli, nullptr, 0,
      "shell input keyevent {key}"),

    // ------------------------------------------------------------ Shell 与日志
    C(1401, "shell.run", GroupId::Shell, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::ShellCommand, ParamSource::None, "cmd", ExecKind::Cli, nullptr, 0,
      "shell {cmd}"),
    C(1402, "shell.terminal", GroupId::Shell, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1403, "logcat.live", GroupId::Shell, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, kLogcatFlags, 1, nullptr),
    C(1404, "logcat.dump", GroupId::Shell, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0,
      "logcat -d -v threadtime"),
    C(1405, "bugreport", GroupId::Shell, 0, kSelNone, true, false, Danger::Careful,
      ParamKind::LocalPaths, ParamSource::None, "out", ExecKind::CliStream, nullptr, 0,
      "bugreport {out}"),
    C(1406, "shell.getprop", GroupId::Shell, 0, kSelNone, true, false, Danger::Safe,
      ParamKind::PropKey, ParamSource::None, "prop", ExecKind::Cli, nullptr, 0,
      "shell getprop {prop}"),

    // ------------------------------------------------------------ Fastboot（专家模式）
    C(1501, "fb.devices", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0, "devices -l"),
    C(1502, "fb.reboot", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Careful,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Cli, nullptr, 0, "reboot bootloader"),
    C(1503, "fb.getvar", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Safe,
      ParamKind::Text, ParamSource::None, "var", ExecKind::Cli, nullptr, 0, "getvar {var}"),
    C(1504, "fb.boot", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Careful,
      ParamKind::ImageFile, ParamSource::None, "img", ExecKind::CliStream, nullptr, 0,
      "boot {img}"),
    C(1505, "fb.setActive", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Destructive,
      ParamKind::SlotName, ParamSource::Slots, "slot", ExecKind::CliPanel, nullptr, 0,
      "set_active {slot}"),
    C(1506, "fb.flash", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Breaker,
      ParamKind::Partition, ParamSource::Partitions, "part", ExecKind::CliPanel, nullptr, 0,
      "flash {part} {img}", ParamKind::ImageFile, ParamSource::None, "img"),
    C(1507, "fb.erase", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Breaker,
      ParamKind::Partition, ParamSource::Partitions, "part", ExecKind::CliPanel, nullptr, 0,
      "erase {part}"),
    C(1508, "fb.unlock", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Breaker,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::CliPanel, nullptr, 0,
      "flashing unlock"),
    C(1509, "fb.lock", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Breaker,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::CliPanel, nullptr, 0,
      "flashing lock"),
    C(1510, "fb.flashall", GroupId::Fastboot, 0, kSelNone, false, true, Danger::Breaker,
      ParamKind::LocalPaths, ParamSource::None, "dir", ExecKind::CliStream, nullptr, 0,
      "flashall"),

    // ------------------------------------------------------------ 应用自身
    C(1901, "tool.settings", GroupId::Tool, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1902, "tool.doctor", GroupId::Tool, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1903, "tool.about", GroupId::Tool, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
    C(1904, "tool.openToolsDir", GroupId::Tool, 0, kSelNone, false, false, Danger::Safe,
      ParamKind::None, ParamSource::None, nullptr, ExecKind::Internal, nullptr, 0, nullptr),
};

const CommandSpec* CommandTable() { return kCommands; }
size_t CommandTableSize() { return sizeof(kCommands) / sizeof(kCommands[0]); }

const CommandSpec* FindCommand(CommandId id) {
    for (const auto& c : kCommands)
        if (c.id == id) return &c;
    return nullptr;
}

const CommandSpec* FindCommandByKey(std::string_view key) {
    for (const auto& c : kCommands)
        if (key == c.key) return &c;
    return nullptr;
}

const FlagSpec* FindFlag(const CommandSpec& spec, std::string_view key) {
    for (uint8_t i = 0; i < spec.flagCount; ++i)
        if (key == spec.flags[i].key) return &spec.flags[i];
    return nullptr;
}

const wchar_t* CommandTitle(const CommandSpec& spec) {
    struct Entry { const char* key; const wchar_t* title; };
    static const Entry kTitles[] = {
        {"device.list",        L"设备列表"},
        {"device.select",      L"选择当前设备"},
        {"device.info",        L"设备信息"},
        {"device.reboot",      L"重启到\u2026"},
        {"device.tcpip",       L"开启无线调试（tcpip）"},
        {"device.connect",     L"连接无线设备\u2026"},
        {"device.pair",        L"配对无线调试\u2026"},
        {"device.disconnect",  L"断开无线设备\u2026"},
        {"device.root",        L"root（需要 userdebug）"},
        {"device.unroot",      L"unroot"},
        {"device.reconnect",   L"重连设备"},
        {"device.serverRestart", L"重启 adb 服务"},

        {"app.install",        L"安装 APK\u2026"},
        {"app.installMultiple",L"安装多个 APK\u2026"},
        {"app.uninstall",      L"卸载应用\u2026"},
        {"app.start",          L"启动应用\u2026"},
        {"app.forceStop",      L"强制停止\u2026"},
        {"app.clearData",      L"清除应用数据\u2026"},
        {"app.grant",          L"授权权限\u2026"},
        {"app.list",           L"已安装应用"},
        {"app.pullApk",        L"导出 APK\u2026"},

        {"file.push",          L"推送到设备\u2026"},
        {"file.pull",          L"从设备拉取\u2026"},
        {"file.browse",        L"设备文件浏览\u2026"},
        {"file.mkdir",         L"新建目录\u2026"},
        {"file.delete",        L"删除设备文件\u2026"},

        {"screen.capture",     L"截屏到本地"},
        {"screen.record",      L"录屏\u2026"},
        {"input.tap",          L"点击（x y）\u2026"},
        {"input.swipe",        L"滑动（x1 y1 x2 y2 ms）\u2026"},
        {"input.text",         L"输入文本\u2026"},
        {"input.key",          L"按键\u2026"},

        {"shell.run",          L"执行 shell 命令\u2026"},
        {"shell.terminal",     L"打开交互终端"},
        {"logcat.live",        L"实时日志"},
        {"logcat.dump",        L"导出当前日志"},
        {"bugreport",          L"抓取 bugreport\u2026"},
        {"shell.getprop",      L"读取系统属性\u2026"},

        {"fb.devices",         L"Fastboot 设备列表"},
        {"fb.reboot",          L"重启到 bootloader"},
        {"fb.getvar",          L"读取变量\u2026"},
        {"fb.boot",            L"临时启动镜像\u2026"},
        {"fb.setActive",       L"切换槽位\u2026"},
        {"fb.flash",           L"刷写分区\u2026"},
        {"fb.erase",           L"擦除分区\u2026"},
        {"fb.unlock",          L"解锁 bootloader"},
        {"fb.lock",            L"上锁 bootloader"},
        {"fb.flashall",        L"全量刷机"},

        {"tool.settings",      L"设置\u2026"},
        {"tool.doctor",        L"自检"},
        {"tool.about",         L"关于"},
        {"tool.openToolsDir",  L"打开 platform-tools 目录"},
    };
    for (const auto& e : kTitles)
        if (std::strcmp(e.key, spec.key) == 0) return e.title;
    return L"(未命名命令)";
}

const wchar_t* ParamLabel(ParamKind kind) {
    switch (kind) {
        case ParamKind::ApkFile:      return L"APK 文件";
        case ParamKind::LocalPaths:   return L"本地路径";
        case ParamKind::RemotePath:   return L"设备路径（如 /sdcard/Download）";
        case ParamKind::PackageName:  return L"应用包名";
        case ParamKind::ActivityName: return L"组件名（包/类）";
        case ParamKind::HostPort:     return L"地址（IP:端口）";
        case ParamKind::PairCode:     return L"配对码";
        case ParamKind::Partition:    return L"分区名";
        case ParamKind::ImageFile:    return L"镜像文件";
        case ParamKind::ShellCommand: return L"命令";
        case ParamKind::Text:         return L"文本";
        case ParamKind::Url:          return L"地址";
        case ParamKind::KeyCode:      return L"按键";
        case ParamKind::Permission:   return L"权限名";
        case ParamKind::PropKey:      return L"属性名";
        case ParamKind::UserId:       return L"用户 ID";
        case ParamKind::SlotName:     return L"槽位（a / b）";
        case ParamKind::Port:         return L"端口";
        default:                      return L"参数";
    }
}

std::wstring FlagLabel(const FlagSpec& f) {
    struct Entry { const char* key; const wchar_t* label; };
    static const Entry kFlags[] = {
        {"replace",   L"覆盖安装 (-r)"},
        {"grant",     L"授予全部权限 (-g)"},
        {"downgrade", L"允许降级 (-d)"},
        {"testonly",  L"允许测试包 (-t)"},
        {"keep",      L"保留数据 (-k)"},
        {"sync",      L"只推有变化的 (--sync)"},
        {"preserve",  L"保留时间戳 (-a)"},
        {"clear",     L"先清空缓冲 (--clear)"},
        {"size",      L"分辨率（如 1280x720）"},
        {"bitrate",   L"码率（如 8M）"},
        {"timelimit", L"时长上限（秒，最长 180）"},
        {"target",    L"目标（recovery / bootloader / 留空=系统）"},
    };
    for (const auto& e : kFlags)
        if (std::strcmp(e.key, f.key) == 0) return e.label;
    return Utf8ToWide(f.key);
}

const wchar_t* GroupName(GroupId g) {
    switch (g) {
        case GroupId::Device:   return L"设备";
        case GroupId::App:      return L"应用";
        case GroupId::File:     return L"文件";
        case GroupId::Screen:   return L"屏幕与输入";
        case GroupId::Shell:    return L"Shell 与日志";
        case GroupId::Fastboot: return L"Fastboot（专家）";
        case GroupId::Tool:     return L"工具";
        default:                return L"";
    }
}

const char* GroupKey(GroupId g) {
    switch (g) {
        case GroupId::Device:   return "device";
        case GroupId::App:      return "app";
        case GroupId::File:     return "file";
        case GroupId::Screen:   return "screen";
        case GroupId::Shell:    return "shell";
        case GroupId::Fastboot: return "fastboot";
        case GroupId::Tool:     return "tool";
        default:                return "?";
    }
}

const char* DangerName(Danger d) {
    switch (d) {
        case Danger::Safe:        return "safe";
        case Danger::Careful:     return "careful";
        case Danger::Destructive: return "destructive";
        default:                  return "breaker";
    }
}

}  // namespace adb