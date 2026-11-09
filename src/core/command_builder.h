#pragma once
// ---------------------------------------------------------------------------
// 命令构造：CommandSpec + 用户输入 → 可执行的 argv
//   · 唯一入口：所有 adb / fastboot 调用都必须经过这里（注入防护）
//   · flags 只接受 FlagSpec 表内的 key
//   · 任何情况下都不拼接 shell 字符串，只产出 argv 数组
// ---------------------------------------------------------------------------

#include <map>
#include <string>
#include <vector>

#include "command_spec.h"
#include "core.h"

namespace adb {

struct BuildInput {
    const CommandSpec* spec = nullptr;
    std::wstring toolExe;          // adb.exe / fastboot.exe 绝对路径
    std::wstring serial;           // 当前设备；空 = 不注入 -s
    std::vector<std::wstring> paths;                        // 本地路径（已展开）
    std::map<std::string, std::wstring> flags;              // key → 值
    std::map<std::string, std::wstring> params;             // paramKey → 值
};

struct BuiltCommand {
    std::vector<std::vector<std::wstring>> argvList;   // 顺序执行的多条命令（不含 exe）
    std::wstring                           display;    // 等价命令行（多行）
    std::vector<std::wstring>              notes;      // 给用户看的提示
};

struct BuildError {
    uint32_t     code = 0;
    std::wstring message;
    const char*  field = nullptr;
};

bool BuildCommand(const BuildInput& in, BuiltCommand* out, BuildError* err);
std::wstring QuoteArg(const std::wstring& arg);
std::wstring BuildDisplayText(const BuiltCommand& cmd);

// 参数校验
bool IsValidPackageName(std::wstring_view s);
bool IsValidRemotePath(std::wstring_view s);
bool IsValidHostPort(std::wstring_view s);
bool IsValidPartitionName(std::wstring_view s);
bool IsValidSerial(std::wstring_view s);
bool IsValidCoords(std::wstring_view s, size_t expectCount);

}  // namespace adb