#pragma once
// ---------------------------------------------------------------------------
// 声明式命令表：菜单标题、参数 schema、flags 白名单、危险级别全部是数据
//   · argv 的唯一来源（GUI / CLI / 未来 AI 都走这里）
//   · 本表不含任何进程调用，可以安全地在 UI 线程构建
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string_view>

#include "core.h"

namespace adb {

using CommandId = uint16_t;

enum class GroupId : uint8_t {
    Device = 0, App, File, Screen, Shell, Fastboot, Tool, Count
};

// 右键 / 窗口上下文：这个命令适用于哪种本地选择
using SelMask = uint16_t;
constexpr SelMask kSelNone = 0x0000;
constexpr SelMask kSelFile = 0x0001;
constexpr SelMask kSelDir  = 0x0002;
constexpr SelMask kSelBg   = 0x0004;
constexpr SelMask kSelApk  = 0x0008;
constexpr SelMask kSelAny  = 0xFFFF;

enum class Danger : uint8_t {
    Safe,         // 只读
    Careful,      // 会改设备状态，可恢复
    Destructive,  // 会丢数据 / 卸应用
    Breaker       // 可能变砖：需要手打确认词
};

enum class ParamKind : uint8_t {
    None, ApkFile, LocalPaths, RemotePath, PackageName, ActivityName,
    HostPort, PairCode, Partition, ImageFile, ShellCommand, Text,
    Url, KeyCode, Permission, PropKey, UserId, SlotName, Port,
    Coords,        // 只允许数字与空格（如 "640 1200"），杜绝 shell 注入
    LocalFiles,    // 本机文件（可多选 / 整个文件夹），写入 BuildInput::paths
    RemotePick,    // 推送目标：/tmp 或 /sdcard 下拉 + 相对路径输入
    RemoteDyn,     // 设备目录：动态列出 /sdcard 下的文件夹 + 相对路径输入
    RebootTarget,  // 重启目标（下拉）
    PropKeys,      // 系统属性（多选复选框，展开成多条 getprop）
    ShellPipeline, // 三段式 shell 命令（自定义编辑器 + 管道）
    Duration,      // 毫秒数（滑动时长），可为空
    LanHost        // 局域网地址：可手填，也可点「扫描局域网」挑一台
};

enum class ParamSource : uint8_t {
    None, InstalledPackages3p, InstalledPackagesAll, Partitions, KeyEvents,
    Permissions, Devices, Slots, Users
};

enum class ExecKind : uint8_t {
    Internal,    // GUI 自己处理（开窗口 / 探测）
    Cli,         // 一次性命令，就地显示结果
    CliPanel,    // 参数面板拼 argv，执行前预览
    CliStream    // 流式（logcat / screenrecord / push 进度）
};

enum class FlagKind : uint8_t {
    Toggle,       // 勾上 → adbArg 整个进 argv
    Radio,        // 单选（radioGroup 内互斥）
    Value,        // 值型：填了就进 argv，adbArg 里的 {v} 被替换
    Dangerous,    // 勾上后执行需二次确认
    ToggleValue   // 勾上才生效，值可留空（如 --user）
};

struct FlagSpec {
    const char* key;
    const char* adbArg;      // → argv；Value/ToggleValue 用 {v} 占位；nullptr = 纯 UI 开关
    FlagKind    kind;
    const char* radioGroup;
    uint16_t    labelRes;
    bool        defaultOn;
    bool        danger;
};

struct CommandSpec {
    CommandId       id;
    const char*     key;         // "app.install"
    GroupId         group;
    uint16_t        titleRes;
    uint16_t        descRes;
    SelMask         applies;
    bool            needsDevice; // 是否需要一台在线设备
    bool            needsFastboot;
    Danger          danger;
    ParamKind       param;
    ParamSource     paramSource;
    const char*     paramKey;
    ParamKind       param2;        // 第二条参数（如 flash 的镜像文件），None = 没有
    ParamSource     paramSource2;
    const char*     paramKey2;
    ExecKind        exec;
    const FlagSpec* flags;
    uint8_t         flagCount;
    const char*     argvTemplate;   // "install {flags} {apk}"；多命令用 " ; " 分隔
                                    // 占位写成 {key?} 表示可选：值为空时整段丢弃
    bool            pkgHideSystem;  // 应用选择器默认隐藏系统应用（卸载这类高危命令用）
};

const CommandSpec* CommandTable();
size_t             CommandTableSize();
const CommandSpec* FindCommand(CommandId id);
const CommandSpec* FindCommandByKey(std::string_view key);
const FlagSpec*    FindFlag(const CommandSpec& spec, std::string_view key);
const wchar_t*     CommandTitle(const CommandSpec& spec);
const wchar_t*     ParamLabel(ParamKind kind);
std::wstring       FlagLabel(const FlagSpec& flag);
const wchar_t*     GroupName(GroupId g);
const char*        GroupKey(GroupId g);
const char*        DangerName(Danger d);

}  // namespace adb