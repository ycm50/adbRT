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
    size_t            droppedBytes = 0;           // 因超出捕获上限被丢掉的字节数
    std::atomic<bool> stop{false};                // sink 要求中止
    std::atomic<bool> finished{false};            // 读线程已收工
    HANDLE            pipe = INVALID_HANDLE_VALUE;
    bool              isStderr = false;
    LineSink          sink;
    std::atomic<bool>* cancel = nullptr;
    std::mutex*       sinkMutex = nullptr;
};

// 把一行放进受预算保护的累积缓冲。必须在持有 mu 时调用。
// 超预算就丢掉最旧的一半 —— 保留最近的输出对用户更有价值，
// 而且整块丢弃比逐字节搬迁便宜得多。
void AppendBounded(std::string* text, size_t* dropped, const std::string& line) {
    text->append(line);
    text->append("\n");
    if (text->size() <= kMaxCaptureBytes) return;
    const size_t keep = kMaxCaptureBytes / 2;
    const size_t cut = text->size() - keep;
    *dropped += cut;
    text->erase(0, cut);
}

struct ReaderArg {
    std::shared_ptr<PipeShared> sp;
};

DWORD WINAPI ReaderThread(LPVOID param) {
    std::unique_ptr<ReaderArg> arg(static_cast<ReaderArg*>(param));
    std::shared_ptr<PipeShared> sp = arg->sp;

    auto emit = [&sp](const std::string& line) {
        // 超长行先截断再记账：无换行的二进制垃圾（exec-out 之类）
        // 会让 pending 一直长，也会让 UI 侧的单行渲染卡死。
        if (line.size() > kMaxLineBytes) {
            std::string cut = line.substr(0, kMaxLineBytes);
            cut += "...[truncated]";
            {
                std::lock_guard<std::mutex> lk(sp->mu);
                AppendBounded(&sp->text, &sp->droppedBytes, cut);
            }
            if (sp->sink) {
                std::lock_guard<std::mutex> lk(*sp->sinkMutex);
                if (!sp->sink(cut, sp->isStderr)) sp->stop = true;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(sp->mu);
            AppendBounded(&sp->text, &sp->droppedBytes, line);
        }
        if (sp->sink) {
            std::lock_guard<std::mutex> lk(*sp->sinkMutex);
            if (!sp->sink(line, sp->isStderr)) sp->stop = true;
        }
    };

    char buf[4096];
    std::string pending;
    size_t scanned = 0;          // pending 里"已经确认没有换行"的前缀长度
    for (;;) {
        DWORD got = 0;
        BOOL ok = ::ReadFile(sp->pipe, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        pending.append(buf, got);

        // 用游标扫描而不是"切一段再 erase 一段"：
        // 超长单行（几 MB 无换行）时后者是 O(n²)，读线程会原地卡死。
        size_t start = 0;
        for (;;) {
            size_t pos = pending.find('\n', scanned);
            if (pos == std::string::npos) { scanned = pending.size(); break; }
            std::string line = pending.substr(start, pos - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            emit(line);
            start = pos + 1;
            scanned = start;
        }
        if (start > 0) pending.erase(0, start);

        // 一行迟迟等不到换行：攒到单行上限就强制切一行出去，
        // 否则 pending 会跟着进程一起无限长下去。
        while (pending.size() >= kMaxLineBytes) {
            emit(pending.substr(0, kMaxLineBytes));
            pending.erase(0, kMaxLineBytes);
        }
        scanned = pending.size();

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

// adb 客户端会把 server fork 成常驻进程，fastboot 没有这个机制。
bool IsPersistentServerTool(const std::wstring& exe) {
    std::wstring name = ToLowerAscii(FileNameOf(exe));
    return name == L"adb.exe" || name == L"adb";
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

    // 对会 fork 常驻 server 的工具（adb）不挂 Job：
    // server 会继承 Job，RunProcess 返回时 Job 一关就把它连坐杀掉，
    // 导致每条命令都重新启动一次 adb 服务（实测每次多 0.5~1.5 秒）。
    const bool keepServerAlive = IsPersistentServerTool(exe);
    HANDLE job = nullptr;
    if (!keepServerAlive) {
        job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            ::AssignProcessToJobObject(job, pi.hProcess);
        }
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
        result.droppedBytes += outShared->droppedBytes;
    }
    {
        std::lock_guard<std::mutex> lk(errShared->mu);
        result.stderrUtf8 = errShared->text;
        result.droppedBytes += errShared->droppedBytes;
    }

    result.elapsedMs = ::GetTickCount64() - t0;
    return result;
}

}  // namespace adb
