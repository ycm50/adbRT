#include "adb_runner.h"

#include <algorithm>

namespace adb {

namespace {

// 按 CommandLineToArgvW 的规则给参数加引号
std::wstring QuoteOne(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\x5c') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\x5c');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\x5c');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\x5c');
    out.push_back(L'"');
    return out;
}

struct ReadCtx {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool   isStderr = false;
    std::string* collect = nullptr;
    LineSink* sink = nullptr;
    std::atomic<bool>* cancel = nullptr;
    std::atomic<bool>* stop = nullptr;
    std::mutex* sinkMutex = nullptr;
};

DWORD WINAPI ReaderThread(LPVOID param) {
    auto* ctx = static_cast<ReadCtx*>(param);
    char buf[4096];
    std::string pending;
    for (;;) {
        DWORD got = 0;
        BOOL ok = ::ReadFile(ctx->pipe, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        pending.append(buf, got);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (ctx->collect) {
                ctx->collect->append(line);
                ctx->collect->append("\n");
            }
            if (ctx->sink && *ctx->sink) {
                std::lock_guard<std::mutex> lk(*ctx->sinkMutex);
                if (!(*ctx->sink)(line, ctx->isStderr) && ctx->stop) *ctx->stop = true;
            }
        }
        if (ctx->cancel && *ctx->cancel) break;
    }
    if (!pending.empty()) {
        if (ctx->collect) { ctx->collect->append(pending); ctx->collect->append("\n"); }
        if (ctx->sink && *ctx->sink) {
            std::lock_guard<std::mutex> lk(*ctx->sinkMutex);
            (*ctx->sink)(pending, ctx->isStderr);
        }
    }
    return 0;
}

}  // namespace

std::wstring BuildCommandLineForLog(const std::wstring& exe,
                                    const std::vector<std::wstring>& argv) {
    std::wstring line = QuoteOne(exe);
    for (const auto& a : argv) { line.push_back(L' '); line += QuoteOne(a); }
    return line;
}

RunResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& argv,
                     const RunOptions& opts) {
    RunResult result;
    if (exe.empty() || !PathExists(exe)) {
        result.error = Format(L"找不到可执行文件：%ls", exe.c_str());
        return result;
    }

    uint64_t t0 = ::GetTickCount64();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!::CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !::CreatePipe(&errRead, &errWrite, &sa, 0)) {
        result.error = L"创建管道失败";
        return result;
    }
    ::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = BuildCommandLineForLog(exe, argv);
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');

    std::wstring cwd = opts.cwd.empty() ? GetExeDir() : opts.cwd;

    BOOL created = ::CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                    cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    ::CloseHandle(outWrite);
    ::CloseHandle(errWrite);

    if (!created) {
        DWORD err = ::GetLastError();
        result.error = Format(L"启动失败：%ls（错误码 %lu）", exe.c_str(), err);
        ::CloseHandle(outRead);
        ::CloseHandle(errRead);
        return result;
    }

    result.started = true;

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        ::AssignProcessToJobObject(job, pi.hProcess);
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    std::atomic<bool> stopFlag{false};
    std::mutex sinkMutex;
    std::string errBuf;

    ReadCtx outCtx{outRead, false, &result.stdoutUtf8, const_cast<LineSink*>(&opts.sink),
                   opts.cancel, &stopFlag, &sinkMutex};
    ReadCtx errCtx{errRead, true, &errBuf, const_cast<LineSink*>(&opts.sink),
                   opts.cancel, &stopFlag, &sinkMutex};

    HANDLE t1 = ::CreateThread(nullptr, 0, ReaderThread, &outCtx, 0, nullptr);
    HANDLE t2 = ::CreateThread(nullptr, 0, ReaderThread, &errCtx, 0, nullptr);

    DWORD waitMs = opts.timeoutMs ? opts.timeoutMs : INFINITE;
    DWORD wr = ::WaitForSingleObject(pi.hProcess, waitMs == INFINITE ? INFINITE : waitMs);
    if (wr == WAIT_TIMEOUT) {
        result.cancelled = true;
        if (job) ::TerminateJobObject(job, 1);
        else ::TerminateProcess(pi.hProcess, 1);
    }

    // 用户取消
    while (opts.cancel && opts.cancel->load() && ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        result.cancelled = true;
        if (job) ::TerminateJobObject(job, 1);
        else ::TerminateProcess(pi.hProcess, 1);
        if (::WaitForSingleObject(pi.hProcess, 3000) != WAIT_TIMEOUT) break;
    }

    ::WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = exitCode;

    ::WaitForSingleObject(t1, 3000);
    ::WaitForSingleObject(t2, 3000);
    ::CloseHandle(t1);
    ::CloseHandle(t2);
    ::CloseHandle(outRead);
    ::CloseHandle(errRead);
    ::CloseHandle(pi.hProcess);
    if (job) ::CloseHandle(job);

    result.stderrUtf8 = errBuf;
    result.elapsedMs = ::GetTickCount64() - t0;
    return result;
}

}  // namespace adb
