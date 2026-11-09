#pragma once
// ---------------------------------------------------------------------------
// 进程封装：所有 adb / fastboot 调用都经由这里
//   · CreateProcessW + 匿名管道，绝不经过 cmd.exe（无 shell 注入面）
//   · Job object 保证取消时连同子进程树一起结束
//   · 输出按 UTF-8 解码（adb 在现代平台上输出 UTF-8）
// ---------------------------------------------------------------------------

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core.h"

namespace adb {

struct RunResult {
    bool         started = false;
    bool         cancelled = false;
    DWORD        exitCode = 0xFFFFFFFFu;
    std::string  stdoutUtf8;
    std::string  stderrUtf8;
    std::wstring error;      // 启动失败时的中文原因
    uint64_t     elapsedMs = 0;

    std::wstring OutW() const { return Utf8ToWide(stdoutUtf8); }
    std::wstring ErrW() const { return Utf8ToWide(stderrUtf8); }
    bool Ok() const { return started && !cancelled && exitCode == 0; }
};

// 每收到一行输出就回调一次（已去掉行尾 CR/LF）；返回 false 表示请求终止进程
using LineSink = std::function<bool(const std::string& line, bool isStderr)>;

struct RunOptions {
    std::wstring cwd;                 // 空 = 用 exe 目录
    LineSink     sink;                // 可为空（纯收集）
    std::atomic<bool>* cancel = nullptr;   // 可空
    uint32_t     timeoutMs = 0;       // 0 = 不超时
};

// 同步执行，阻塞直到结束。严重错误（找不到 exe）通过 result.error 返回。
RunResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& argv,
                     const RunOptions& opts);

// 用 Windows 的命令行拼接规则生成 "exe arg1 arg2"（仅用于日志/预览）
std::wstring BuildCommandLineForLog(const std::wstring& exe,
                                    const std::vector<std::wstring>& argv);

}  // namespace adb
