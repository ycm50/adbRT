#include "adb_runner.h"

#include <algorithm>
#include <memory>
#include <mutex>

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

// 一条管道的共享状态。读线程和调用方各持一份 shared_ptr：
// 即使进程在超时内没退干净，也不会有人踩到已经释放的内存。
struct PipeShared {
    std::mutex        mu;
    std::string       text;                       // 累积输出（受 mu 保护）
    std::atomic<bool> stop{false};                // sink 要求中止
    std::atomic<bool> finished{false};            // 读线程已收工
    HANDLE            pipe = INVALID_HANDLE_VALUE;
    bool              isStderr = false;
    LineSink          sink;
    std::atomic<bool>* cancel = nullptr;
    std::mutex*       sinkMutex = nullptr;
};

struct ReaderArg {
    std::shared_ptr<PipeShared> sp;
};

DWORD WINAPI ReaderThread(LPVOID param) {
    std::unique_ptr<ReaderArg> arg(static_cast<ReaderArg*>(param));
    std::shared_ptr<PipeShared> sp = arg->sp;

    auto emit = [&sp](const std::string& line) {
        {
            std::lock_guard<std::mutex> lk(sp->mu);
            sp->text.append(line);
            sp->text.append("\n");
        }
        if (sp->sink) {
            std::lock_guard<std::mutex> lk(*sp->sinkMutex);
            if (!sp->sink(line, sp->isStderr)) sp->stop = true;
        }
    };

    char buf[4096];
    std::string pending;
    for (;;) {
        DWORD got = 0;
        BOOL ok = ::ReadFile(sp->pipe, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        pending.append(buf, got);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            emit(line);
        }
        if (sp->stop.load()) break;
        if (sp->cancel && sp->cancel->load()) break;
    }
    if (!pending.empty()) emit(pending);

    if (sp->pipe != INVALID_HANDLE_VALUE) ::CloseHandle(sp->pipe);
    sp->pipe = INVALID_HANDLE_VALUE;
    sp->finished = true;
    return 0;
}

// 起读线程；句柄立刻关掉（线程对象在线程结束后由系统回收）
bool StartReader(const std::shared_ptr<PipeShared>& sp) {
    auto* arg = new ReaderArg{sp};
    DWORD tid = 0;
    HANDLE th = ::CreateThread(nullptr, 0, ReaderThread, arg, 0, &tid);
    if (!th) { delete arg; return false; }
    ::CloseHandle(th);
    return true;
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

    const uint64_t t0 = ::GetTickCount64();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;

    // 两次 CreatePipe 只要有一处失败，前面建好的句柄必须关掉，
    // 否则每失败一次就漏两个句柄，句柄耗尽后整个程序就起不了进程了。
    auto closePipes = [&]() {
        HANDLE* all[] = {&outRead, &outWrite, &errRead, &errWrite};
        for (HANDLE* h : all) {
            if (*h) { ::CloseHandle(*h); *h = nullptr; }
        }
    };

    if (!::CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !::CreatePipe(&errRead, &errWrite, &sa, 0)) {
        closePipes();
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
    outWrite = errWrite = nullptr;

    if (!created) {
        DWORD err = ::GetLastError();
        result.error = Format(L"启动失败：%ls（错误码 %lu）", exe.c_str(), err);
        closePipes();
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

    std::mutex sinkMutex;

    auto outShared = std::make_shared<PipeShared>();
    outShared->pipe = outRead;
    outShared->isStderr = false;
    outShared->sink = opts.sink;
    outShared->cancel = opts.cancel;
    outShared->sinkMutex = &sinkMutex;

    auto errShared = std::make_shared<PipeShared>();
    errShared->pipe = errRead;
    errShared->isStderr = true;
    errShared->sink = opts.sink;
    errShared->cancel = opts.cancel;
    errShared->sinkMutex = &sinkMutex;

    if (!StartReader(outShared)) { ::CloseHandle(outRead); outShared->pipe = INVALID_HANDLE_VALUE; }
    if (!StartReader(errShared)) { ::CloseHandle(errRead); errShared->pipe = INVALID_HANDLE_VALUE; }

    DWORD waitMs = opts.timeoutMs ? opts.timeoutMs : INFINITE;
    DWORD wr = ::WaitForSingleObject(pi.hProcess, waitMs);
    if (wr == WAIT_TIMEOUT) {
        result.cancelled = true;
        if (job) ::TerminateJobObject(job, 1);
        else ::TerminateProcess(pi.hProcess, 1);
    }

    // 用户取消
    while (opts.cancel && opts.cancel->load() &&
           ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        result.cancelled = true;
        if (job) ::TerminateJobObject(job, 1);
        else ::TerminateProcess(pi.hProcess, 1);
        if (::WaitForSingleObject(pi.hProcess, 3000) != WAIT_TIMEOUT) break;
    }

    ::WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = exitCode;

    ::CloseHandle(pi.hProcess);
    if (job) ::CloseHandle(job);

    // 子进程退出 → 写端全关 → ReadFile 返回 0 → 读线程收尾。
    // 给它们最多 1 秒把最后几行落进 shared；超时也不阻塞主流程
    // （shared_ptr 保证这时候读数据是安全的）。
    for (int i = 0; i < 100; ++i) {
        if (outShared->finished.load() && errShared->finished.load()) break;
        ::Sleep(10);
    }
    {
        std::lock_guard<std::mutex> lk(outShared->mu);
        result.stdoutUtf8 = outShared->text;
    }
    {
        std::lock_guard<std::mutex> lk(errShared->mu);
        result.stderrUtf8 = errShared->text;
    }

    result.elapsedMs = ::GetTickCount64() - t0;
    return result;
}

}  // namespace adb
