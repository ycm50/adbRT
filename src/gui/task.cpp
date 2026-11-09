#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

void PostLine(int taskId, const std::wstring& line) {
    auto* heap = new std::wstring(line);
    if (!::PostMessageW(g_app.hwnd, WM_APP_TASK_LINE, static_cast<WPARAM>(taskId),
                        reinterpret_cast<LPARAM>(heap))) {
        delete heap;
    }
}

void PostDone(int taskId, bool ok, bool cancelled, DWORD exitCode, const std::wstring& msg) {
    auto* outcome = new TaskOutcome{ok, cancelled, exitCode, msg, 0};
    if (!::PostMessageW(g_app.hwnd, WM_APP_TASK_DONE, static_cast<WPARAM>(taskId),
                        reinterpret_cast<LPARAM>(outcome))) {
        delete outcome;
    }
}

void ClearOutput() {
    SetTextOf(g_app.hOutput, L"");
}

void AppendOutput(const std::wstring& text) {
    AppendTextToEdit(g_app.hOutput, text);
    ScrollToEnd(g_app.hOutput);
}

void SetStatus(const wchar_t* text) {
    if (g_app.hStatus) ::SetWindowTextW(g_app.hStatus, text);
}

namespace {

struct WorkerArg {
    int  taskId = 0;
    std::wstring exe;
    std::vector<std::vector<std::wstring>> argvList;
};

// 逐条执行 argv，把输出流式回传
DWORD WINAPI WorkerProc(LPVOID param) {
    auto* arg = static_cast<WorkerArg*>(param);
    bool allOk = true;
    uint64_t elapsed = 0;
    DWORD lastExit = 0;

    for (const auto& argv : arg->argvList) {
        if (g_app.cancelFlag.load()) { allOk = false; break; }

        PostLine(arg->taskId, L"> " + adb::BuildCommandLineForLog(arg->exe, argv));

        adb::RunOptions opts;
        opts.cancel = &g_app.cancelFlag;
        opts.sink = [&](const std::string& line, bool isStderr) -> bool {
            std::wstring w = adb::Utf8ToWide(line);
            if (isStderr) w = L"[err] " + w;
            PostLine(arg->taskId, w);
            int pct = adb::ParseProgressPercent(w);
            if (pct >= 0) {
                ::PostMessageW(g_app.hwnd, WM_APP_TASK_PROGRESS,
                               static_cast<WPARAM>(arg->taskId), static_cast<LPARAM>(pct));
            }
            return !g_app.cancelFlag.load();
        };

        adb::RunResult r = adb::RunProcess(arg->exe, argv, opts);
        elapsed += r.elapsedMs;
        lastExit = r.exitCode;
        if (!r.started) {
            PostLine(arg->taskId, L"[错误] " + r.error);
            allOk = false;
        } else if (r.cancelled) {
            allOk = false;
        } else if (r.exitCode != 0) {
            allOk = false;
            std::wstring explain = adb::ExplainAdbError(r.ErrW() + L"\n" + r.OutW());
            if (!explain.empty()) PostLine(arg->taskId, L"[提示] " + explain);
        }
    }

    bool cancelled = g_app.cancelFlag.load();
    std::wstring msg = allOk ? L"完成" : (cancelled ? L"已取消" : L"失败");
    PostDone(arg->taskId, allOk, cancelled, lastExit, msg);
    (void)elapsed;
    delete arg;
    return 0;
}

}  // namespace

void StartRun() {
    adb::BuildInput in;
    std::wstring error;
    if (!CollectInput(&in, &error)) {
        ::MessageBoxW(g_app.hwnd, error.c_str(), L"AdbRT", MB_OK | MB_ICONWARNING);
        return;
    }

    adb::BuiltCommand built;
    adb::BuildError berr;
    if (!adb::BuildCommand(in, &built, &berr)) {
        ::MessageBoxW(g_app.hwnd, berr.message.c_str(), L"AdbRT", MB_OK | MB_ICONWARNING);
        return;
    }

    g_app.cancelFlag = false;
    g_app.busy = true;
    ::EnableWindow(g_app.hRun, FALSE);
    ::EnableWindow(g_app.hCancel, TRUE);
    SetStatus(L"正在执行…");
    ClearOutput();

    auto* arg = new WorkerArg();
    arg->taskId = ++g_app.nextTaskId;
    g_app.activeTaskId = arg->taskId;
    arg->exe = in.toolExe;
    arg->argvList = built.argvList;

    DWORD tid = 0;
    HANDLE th = ::CreateThread(nullptr, 0, WorkerProc, arg, 0, &tid);
    if (!th) {
        delete arg;
        g_app.busy = false;
        ::EnableWindow(g_app.hRun, TRUE);
        ::EnableWindow(g_app.hCancel, FALSE);
        SetStatus(L"启动工作线程失败");
        return;
    }
    ::CloseHandle(th);
}

void CancelRun() {
    g_app.cancelFlag = true;
    SetStatus(L"正在取消…");
}

}  // namespace adbgui