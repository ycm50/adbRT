#pragma once
// ---------------------------------------------------------------------------
// AdbRT 核心基础层：字符串 / 路径 / 环境变量 / 日志
//   · 只依赖 Win32 + 标准库，零第三方依赖
//   · 这里的每个函数都允许被 GUI 线程调用，因此不做任何阻塞 I/O
//     （日志除外，且日志只在出错时才写盘）
// ---------------------------------------------------------------------------

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adb {

// ------------------------------------------------------------------- 字符串
std::wstring Utf8ToWide(std::string_view s);
std::string  WideToUtf8(std::wstring_view s);
inline std::string  U8(std::wstring_view s) { return WideToUtf8(s); }
inline std::wstring W(std::string_view s)  { return Utf8ToWide(s); }

std::wstring ToLowerAscii(std::wstring s);
std::wstring Trim(std::wstring_view s);
bool         StartsWith(std::wstring_view s, std::wstring_view prefix);
bool         EndsWith(std::wstring_view s, std::wstring_view suffix);
bool         EqualsNoCase(std::wstring_view a, std::wstring_view b);
std::wstring ReplaceAll(std::wstring s, std::wstring_view from, std::wstring_view to);
std::wstring Format(const wchar_t* fmt, ...);
std::vector<std::wstring> SplitLines(std::wstring_view text);
std::vector<std::wstring> SplitWhitespace(std::wstring_view text);
std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep);

// ---------------------------------------------------------------------- 路径
// NormalizePath：折叠 . / ..、统一反斜杠、去掉尾部反斜杠（盘符根除外），不改大小写
std::wstring NormalizePath(std::wstring p);
bool         IsAbsolutePath(std::wstring_view p);
bool         PathExists(std::wstring_view p);
bool         PathIsDirectory(std::wstring_view p);
bool         EnsureDirectory(std::wstring_view p);
std::wstring JoinPath(std::wstring_view a, std::wstring_view b);
std::wstring FileNameOf(std::wstring_view p);
std::wstring ParentOf(std::wstring_view p);
std::wstring GetExePath();
std::wstring GetExeDir();

// 展开 %VAR% 形式的环境变量（未定义的原样保留）。支持 %% 转义成一个 %。
std::wstring ExpandEnvVars(std::wstring_view in);

// ---------------------------------------------------------- 相对 / 绝对目录
// 需求：配置里的路径既可以是绝对目录，也可以是"相对目录"。
//   · 绝对 → 原样规范化
//   · 相对 → 相对于 baseDir（默认是 exe 所在目录，见 ResolveAgainstExeDir）
// 结果一律是绝对路径；空输入返回空。
std::wstring ResolvePathSpec(std::wstring_view spec, std::wstring_view baseDir);
std::wstring ResolveAgainstExeDir(std::wstring_view spec);
// 反过来：如果 abs 在 baseDir 之下，返回相对路径（正斜杠形式，便于写进配置）；
// 否则返回空，表示只能存绝对路径。
std::wstring MakeRelativeTo(std::wstring_view baseDir, std::wstring_view absPath);
// 给用户看的形态：绝对路径就显示绝对，相对路径显示 "相对程序目录"
std::wstring DescribePathSpec(std::wstring_view spec);

// ---------------------------------------------------------------------- 环境
std::optional<std::wstring> GetEnvVar(const wchar_t* name);
// 用户 PATH（不是进程 PATH）：写入 HKCU\Environment 的 Path —— 这是安装器唯一会写的
// 一处系统状态。放在 core 里是为了让 install 脚本与 doctor 命令共用同一套逻辑。
bool AddToUserPath(std::wstring_view dir);
bool RemoveFromUserPath(std::wstring_view dir);
std::vector<std::wstring> GetUserPathEntries();
bool SetUserEnvVar(const wchar_t* name, std::wstring_view value);   // 空值 = 删除
bool BroadcastEnvironmentChange();

// ---------------------------------------------------------------------- 日志
enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void         LogInit(const std::wstring& dir = {}, LogLevel minLevel = LogLevel::Info);
void         LogSetLevel(LogLevel lv);
LogLevel     LogGetLevel();
void         LogWrite(LogLevel lv, const char* category, const std::string& msg);  // msg 为 UTF-8
void         LogFlush();
std::wstring LogFilePath();
std::wstring DefaultLogDir();

template <typename... Args>
inline void LogF(LogLevel lv, const char* cat, const wchar_t* fmt, Args... args) {
    LogWrite(lv, cat, WideToUtf8(Format(fmt, args...)));
}

#define ADB_LOGE(cat, ...) ::adb::LogF(::adb::LogLevel::Error, cat, __VA_ARGS__)
#define ADB_LOGW(cat, ...) ::adb::LogF(::adb::LogLevel::Warn,  cat, __VA_ARGS__)
#define ADB_LOGI(cat, ...) ::adb::LogF(::adb::LogLevel::Info,  cat, __VA_ARGS__)
#define ADB_LOGD(cat, ...) ::adb::LogF(::adb::LogLevel::Debug, cat, __VA_ARGS__)

// ------------------------------------------------------------------ 实用工具
std::wstring FormatBytes(uint64_t bytes);
std::wstring NowStamp();          // 20261109-133900
std::wstring FormatDuration(uint64_t ms);

}  // namespace adb
