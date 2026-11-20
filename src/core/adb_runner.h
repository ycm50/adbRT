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
    size_t       droppedBytes = 0;   // 因超出捕获上限被丢掉的字节数

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

// ---------------------------------------------------------------------------
// 为什么要区分"会不会 fork 常驻进程"的工具
//
// RunProcess 给子进程挂了一个 KILL_ON_JOB_CLOSE 的 Job，用来在取消时
// 连整棵子进程树一起结束。但 adb 客户端发现 server 没跑时，会 fork 出
// "adb -L tcp:5037 fork-server server" 并让它常驻 —— 这个 server 会
// 继承同一个 Job，于是 RunProcess 一返回、Job 句柄一关，server 就被连坐杀掉。
// 下一条命令又要重新 fork/启动 server，每条命令都卡上 0.5~1.5 秒。
//
// 实测（关 Job 后观察 fork 出的常驻进程）：
//   不挂 Job -> 存活；挂 KILL_ON_JOB_CLOSE 的 Job -> 被杀。
//
// 所以对 adb 这类自带常驻 server 的工具不挂 Job：取消时退化成只结束直接
// 子进程，server 作为共享资源留着（杀它反而有害）。
// ---------------------------------------------------------------------------
bool IsPersistentServerTool(const std::wstring& exe);

// ---------------------------------------------------------------------------
// 输出捕获的内存上限（唯一真相源，GUI 与测试共用）
//
// 为什么需要：RunProcess 原来把整份 stdout/stderr 一股脑 append 进
// std::string，进程活多久它就长多久。一条 logcat 或 ls -R 能吃掉几百 MB，
// 这是"越用越卡"的第一号原因。现在改成有预算的捕获：
//   · 单行超过 kMaxLineBytes 就截断（超长行通常是无换行的二进制垃圾）
//   · 累计超过 kMaxCaptureBytes 就丢掉最旧的一半，并记账丢弃字节数
// 生产端（读线程）永不因为这条上限等待 —— 阻塞会写满 adb 的管道把它卡死。
// ---------------------------------------------------------------------------
constexpr size_t kMaxLineBytes    = 8 * 1024;          // 单行上限
constexpr size_t kMaxCaptureBytes = 2 * 1024 * 1024;   // 每条流捕获上限

// 同步执行，阻塞直到结束。严重错误（找不到 exe）通过 result.error 返回。
RunResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& argv,
                     const RunOptions& opts);

// 用 Windows 的命令行拼接规则生成 "exe arg1 arg2"（仅用于日志/预览）
std::wstring BuildCommandLineForLog(const std::wstring& exe,
                                    const std::vector<std::wstring>& argv);

}  // namespace adb
