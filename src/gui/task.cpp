#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/bounded_text.h"
#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

// ---------------------------------------------------------------------------
// 运行结果的内存策略
//
// 原来：每输出一行就 PostMessage 一次，主线程收到就 EM_REPLACESEL 追加，
// 再 ScrollToEnd（内部是 EM_SETSEL 0,-1 全选）—— 输出越多越慢，
// EDIT 里的文本还完全没有上限。logcat 这种高频输出直接把消息队列打爆。
//
// 现在：
//   · 工作线程把行写进有界的 BoundedText（超预算丢最旧的一半并记账）
//   · 主线程用 100ms 定时器批量取走，一次只做一次追加
//   · EDIT 自身也按字节预算裁剪，避免控件内部无限长
// ---------------------------------------------------------------------------

namespace {

// 待投递缓冲的预算：比 UI 的显示预算大一些，允许一个刷新周期内积压
constexpr size_t kPendingBudget = 2 * 1024 * 1024;

adb::BoundedText& Pending() {
    static adb::BoundedText buf(kPendingBudget, adb::kMaxLineBytes);
    return buf;
}

// 工作线程把行塞进来；主线程用定时器取走
void PushLine(int taskId, const std::wstring& line) {
    (void)taskId;
    Pending().Append(line);
}

}  // namespace

void PostLine(int taskId, const std::wstring& line) {
    // 保留原接口：内部改成写入有界缓冲，不再一行一次 PostMessage
    PushLine(taskId, line);
}

void PostDone(int taskId, bool ok, bool cancelled, DWORD exitCode, const std::wstring& msg) {
    auto* outcome = new TaskOutcome{ok, cancelled, exitCode, msg, 0, 0};
    if (!::PostMessageW(g_app.hwnd, WM_APP_TASK_DONE, static_cast<WPARAM>(taskId),
                        reinterpret_cast<LPARAM>(outcome))) {
        delete outcome;
    }
}

void ClearOutput() {
    Pending().Clear();
    SetTextOf(g_app.hOutput, L"");
}

void AppendOutput(const std::wstring& text) {
    AppendTextToEdit(g_app.hOutput, text);
    ScrollToEnd(g_app.hOutput);
}

// 主线程批量刷新：循环取走积压（每次最多 kOutputBatchBytes），最后只滚一次屏。
// 原来只取一批就返回，进程结束得比刷新快时剩下的输出就永远留在缓冲里
// —— 表现为"跑得快的命令结果只显示了一小段"。
void FlushOutput() {
    adb::BoundedText& pending = Pending();
    if (pending.PendingBytes() == 0) return;

    bool appended = false;
    bool trimmed = false;
    // 一帧内最多搬 kOutputBudgetBytes，避免一次刷新把 UI 卡住
    size_t moved = 0;
    while (pending.PendingBytes() > 0 && moved < kOutputBudgetBytes) {
        std::wstring batch = pending.TakeBatch(kOutputBatchBytes);
        if (batch.empty()) break;
        moved += batch.size();
        AppendTextToEdit(g_app.hOutput, batch);
        appended = true;
        if (TrimEditToBudget(g_app.hOutput, kOutputBudgetBytes)) trimmed = true;
    }

    if (appended) {
        // 裁剪后原来的滚动位置失效，直接跳到新的尾部
        if (trimmed) ScrollToEnd(g_app.hOutput);
        else         FollowTailIfAtBottom(g_app.hOutput);
    }

    const size_t dropped = pending.DroppedBytes();
    if (dropped != 0 && dropped != g_app.lastDroppedNotified) {
        g_app.lastDroppedNotified = dropped;
        SetStatus(adb::Format(L"输出过多，已丢弃较早的 %zu 字（结果框保留上限 %zu MiB）",
                              dropped, kOutputBudgetBytes / (1024 * 1024)).c_str());
    }
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

        PushLine(arg->taskId, L"> " + adb::BuildCommandLineForLog(arg->exe, argv));

        adb::RunOptions opts;
        opts.cancel = &g_app.cancelFlag;
        opts.sink = [&](const std::string& line, bool isStderr) -> bool {
            std::wstring w = adb::Utf8ToWide(line);
            if (isStderr) w = L"[err] " + w;
            PushLine(arg->taskId, w);
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
            PushLine(arg->taskId, L"[错误] " + r.error);
            allOk = false;
        } else if (r.cancelled) {
            allOk = false;
        } else if (r.exitCode != 0) {
            allOk = false;
            std::wstring explain = adb::ExplainAdbError(r.ErrW() + L"\n" + r.OutW());
            if (!explain.empty()) PushLine(arg->taskId, L"[提示] " + explain);
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
    // 内置命令：不开工作线程，直接把窗口叫起来
    if (g_app.current && g_app.current->exec == adb::ExecKind::Internal) {
        std::wstring error;
        if (!RunBuiltinCommand(g_app.hwnd, *g_app.current, &error) && !error.empty())
            ::MessageBoxW(g_app.hwnd, error.c_str(), L"AdbRT", MB_OK | MB_ICONINFORMATION);
        return;
    }

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

    // 危险等级确认
    if (g_app.current) {
        std::wstring title = adb::CommandTitle(*g_app.current);
        if (g_app.current->danger == adb::Danger::Breaker) {
            std::wstring expect = g_app.serial.empty() ? std::wstring(L"FLASH") : g_app.serial;
            std::wstring msg = std::wstring(L"\u5373\u5c06\u6267\u884c\uff1a\r\n") + built.display +
                               L"\r\n\r\n\u8fd9\u662f\u53ef\u80fd\u8ba9\u8bbe\u5907\u65e0\u6cd5\u5f00\u673a\u7684\u64cd\u4f5c\u3002";
            if (!ShowBreakerDialog(g_app.hwnd, title, msg, expect)) return;
        } else if (g_app.current->danger == adb::Danger::Destructive) {
            std::wstring msg = std::wstring(L"\u5373\u5c06\u6267\u884c\uff1a\r\n") + built.display +
                               L"\r\n\r\n\u4f1a\u4e22\u5931\u6570\u636e\uff0c\u7ee7\u7eed\uff1f";
            if (!ShowConfirmDialog(g_app.hwnd, title, msg)) return;
        }
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
    ::SetTimer(g_app.hwnd, kOutputTimerId, kOutputFlushMs, nullptr);
}

void CancelRun() {
    g_app.cancelFlag = true;
    SetStatus(L"正在取消…");
}

}  // namespace adbgui
