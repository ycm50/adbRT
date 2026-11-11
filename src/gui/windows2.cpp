// ---------------------------------------------------------------------------
// 截屏 / 交互终端 / 实时 logcat
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

HINSTANCE InstOf(HWND hwnd) {
    return reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
}

// ------------------------------------------------------------------ 截屏
struct CaptureArg {
    HWND parent = nullptr;
};

DWORD WINAPI CaptureWorker(LPVOID param) {
    auto* arg = static_cast<CaptureArg*>(param);
    auto& session = adb::DeviceSession::Instance();
    std::wstring dir = adb::Settings::Instance().CaptureDir();
    adb::EnsureDirectory(dir);
    std::wstring file = adb::JoinPath(dir, L"adbrt-" + adb::NowStamp() + L".png");
    std::wstring remote = L"/sdcard/adbrt-shot.png";

    adb::RunOptions opts;
    opts.timeoutMs = 20000;

    // 走 "设备侧截图 + pull"，避免 exec-out 的二进制被按行读取截断
    auto shot = adb::RunProcess(session.AdbExe(),
                                session.WithSerial({L"shell", L"screencap", L"-p", remote}), opts);
    bool ok = shot.started && shot.exitCode == 0;
    std::wstring err = shot.error + shot.ErrW();

    if (ok) {
        adb::RunOptions popts;
        popts.timeoutMs = 30000;
        auto pull = adb::RunProcess(session.AdbExe(),
                                    session.WithSerial({L"pull", remote, file}), popts);
        ok = pull.Ok() && adb::PathExists(file);
        if (!ok) err += pull.error + pull.ErrW();
        adb::RunOptions rm;
        rm.timeoutMs = 8000;
        adb::RunProcess(session.AdbExe(), session.WithSerial({L"shell", L"rm", L"-f", remote}), rm);
    }

    std::wstring msg = ok ? (L"已保存：\n" + file) : (L"截屏失败：\n" + err);
    ::MessageBoxW(arg->parent, msg.c_str(), L"AdbRT",
                  MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    if (ok) ::ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    delete arg;
    return 0;
}

// ---------------------------------------------------------------- logcat
// 后台 logcat 线程与窗口共享的状态：线程只认这个，
// 窗口关掉时置 stop，线程随后看到的都是安全数据，不会踩已释放的窗口对象。
struct LogShared {
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::mutex mutex;
    std::vector<std::wstring> lines;
    HWND edit = nullptr;      // 仅用于 PostMessage 通知刷新
};

struct LogCtx {
    HWND   edit = nullptr;
    HWND   filterEdit = nullptr;
    HWND   pauseBtn = nullptr;
    std::shared_ptr<LogShared> shared;
    size_t cap = 4000;
    std::wstring filter;
};

LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void LogRefresh(LogCtx* ctx) {
    if (!ctx) return;
    auto shared = ctx->shared;
    std::wstring text;
    std::wstring f = adb::ToLowerAscii(ctx->filter);
    {
        std::lock_guard<std::mutex> lk(shared->mutex);
        size_t start = shared->lines.size() > ctx->cap ? shared->lines.size() - ctx->cap : 0;
        for (size_t i = start; i < shared->lines.size(); ++i) {
            if (!f.empty() && adb::ToLowerAscii(shared->lines[i]).find(f) == std::wstring::npos)
                continue;
            text += shared->lines[i];
            text += L"\r\n";
        }
    }
    SetTextOf(ctx->edit, text);
    ScrollToEnd(ctx->edit);
}

// 后台 logcat 线程：只持有 shared state 与 edit 的数值句柄，
// 窗口销毁后 PostMessage 会失败，但绝不会解引用已释放的对象。
struct LogArg {
    std::shared_ptr<LogShared> shared;
    HWND notify = nullptr;      // 用来 PostMessage 的窗口（父窗口）
};

DWORD WINAPI LogWorker(LPVOID param) {
    std::unique_ptr<LogArg> arg(static_cast<LogArg*>(param));
    auto shared = arg->shared;
    HWND notify = arg->notify;
    auto& session = adb::DeviceSession::Instance();

    adb::RunOptions opts;
    opts.cancel = &shared->stop;
    opts.sink = [shared, notify](const std::string& line, bool) -> bool {
        if (shared->stop.load()) return false;
        if (shared->paused.load()) return true;
        {
            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->lines.push_back(adb::Utf8ToWide(line));
            // 超过 2 万行就砍掉前一半，避免长时间跑 logcat 把内存吃干
            if (shared->lines.size() > 20000)
                shared->lines.erase(shared->lines.begin(), shared->lines.begin() + 10000);
        }
        if (notify) ::PostMessageW(notify, WM_APP + 40, 0, 0);
        return !shared->stop.load();
    };
    adb::RunProcess(session.AdbExe(), session.WithSerial({L"logcat", L"-v", L"threadtime"}), opts);
    if (notify) ::PostMessageW(notify, WM_APP + 41, 0, 0);
    return 0;
}

LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<LogCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = new LogCtx();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->filterEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_FILTER), nullptr, nullptr);
            ctx->edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_OUTPUT), nullptr, nullptr);
            ctx->pauseBtn = ::CreateWindowExW(0, L"BUTTON", L"暂停",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_PAUSE), nullptr, nullptr);
            HWND save = ::CreateWindowExW(0, L"BUTTON", L"保存…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_SAVELOG), nullptr, nullptr);
            HWND clearBtn = ::CreateWindowExW(0, L"BUTTON", L"清空",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_ACTION2), nullptr, nullptr);

            ApplyFont(ctx->filterEdit, CreateUiFont(9, false));
            ApplyFont(ctx->edit, CreateUiFont(9, true));
            for (HWND b : {ctx->pauseBtn, save, clearBtn}) ApplyFont(b, CreateUiFont(9, false));

            ::SetWindowTextW(hwnd, L"实时日志");

            ctx->shared = std::make_shared<LogShared>();
            ctx->shared->edit = ctx->edit;
            auto* arg = new LogArg{ctx->shared, hwnd};
            DWORD tid = 0;
            HANDLE th = ::CreateThread(nullptr, 0, LogWorker, arg, 0, &tid);
            if (th) ::CloseHandle(th);
            else delete arg;
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(ctx->filterEdit, 4, 4, w - 200, 24, TRUE);
            ::MoveWindow(ctx->pauseBtn, w - 190, 4, 60, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_BTN_SAVELOG), w - 126, 4, 60, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_BTN_ACTION2), w - 62, 4, 58, 24, TRUE);
            ::MoveWindow(ctx->edit, 4, 32, w - 8, h - 40, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_EDIT_FILTER && code == EN_CHANGE) {
                ctx->filter = GetTextOf(ctx->filterEdit);
                LogRefresh(ctx);
                return 0;
            }
            if (id == IDC_BTN_PAUSE) {
                bool nowPaused = !ctx->shared->paused.load();
                ctx->shared->paused = nowPaused;
                ::SetWindowTextW(ctx->pauseBtn, nowPaused ? L"继续" : L"暂停");
                return 0;
            }
            if (id == IDC_BTN_ACTION2) {
                {
                    std::lock_guard<std::mutex> lk(ctx->shared->mutex);
                    ctx->shared->lines.clear();
                }
                SetTextOf(ctx->edit, L"");
                return 0;
            }
            if (id == IDC_BTN_SAVELOG) {
                std::wstring dir = adb::Settings::Instance().DownloadDir();
                adb::EnsureDirectory(dir);
                std::wstring file = adb::JoinPath(dir, L"logcat-" + adb::NowStamp() + L".txt");
                wchar_t buf[MAX_PATH * 2] = {0};
                lstrcpynW(buf, file.c_str(), MAX_PATH * 2);
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"日志文件\0*.txt\0";
                ofn.lpstrFile = buf;
                ofn.nMaxFile = MAX_PATH * 2;
                ofn.lpstrTitle = L"保存日志";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (!::GetSaveFileNameW(&ofn)) return 0;
                std::wstring all;
                {
                    std::lock_guard<std::mutex> lk(ctx->shared->mutex);
                    for (const auto& l : ctx->shared->lines) { all += l; all += L"\r\n"; }
                }
                HANDLE h = ::CreateFileW(buf, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    std::string utf8 = adb::WideToUtf8(all);
                    DWORD written = 0;
                    ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
                    ::CloseHandle(h);
                }
                return 0;
            }
            return 0;
        }
        case WM_APP + 40: {
            static ULONGLONG last = 0;
            ULONGLONG now = ::GetTickCount64();
            if (now - last > 500) {   // 最多每 500ms 刷一次界面，避免刷爆
                last = now;
                LogRefresh(ctx);
            }
            return 0;
        }
        case WM_APP + 41:
            ::SetWindowTextW(hwnd, L"实时日志（已结束）");
            return 0;
        case WM_CLOSE:
            if (ctx && ctx->shared) ctx->shared->stop = true;
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (ctx) {
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                // 只置取消位就释放窗口上下文：后台线程持有 shared_ptr，
                // 之后它只会看到 stop=true 并自行退出，不会碰到已释放的对象。
                if (ctx->shared) ctx->shared->stop = true;
                delete ctx;
            }
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureLogClass(HINSTANCE hinst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LogProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AdbRTLogView";
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void CaptureScreenshot(HWND parent) {
    if (adb::DeviceSession::Instance().CurrentSerial().empty()) {
        ShowTextView(parent, L"截屏", L"没有选中设备。请先连接设备并点「刷新设备」。");
        return;
    }
    auto* arg = new CaptureArg{parent};
    DWORD tid = 0;
    HANDLE th = ::CreateThread(nullptr, 0, CaptureWorker, arg, 0, &tid);
    if (th) ::CloseHandle(th);
    else delete arg;
}

void OpenInteractiveShell(HWND parent) {
    auto& session = adb::DeviceSession::Instance();
    std::wstring serial = session.CurrentSerial();
    if (serial.empty()) {
        ShowTextView(parent, L"交互终端", L"没有选中设备。");
        return;
    }
    std::wstring adbPath = session.AdbExe();
    std::wstring args = L"/k \"\"" + adbPath + L"\" -s " + serial + L" shell\"";
    ::ShellExecuteW(parent, L"open", L"cmd.exe", args.c_str(),
                    adb::GetExeDir().c_str(), SW_SHOWNORMAL);
}

void ShowLogcat(HWND parent) {
    if (adb::DeviceSession::Instance().CurrentSerial().empty()) {
        ShowTextView(parent, L"实时日志", L"没有选中设备。");
        return;
    }
    EnsureLogClass(InstOf(parent));
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, L"AdbRTLogView", L"实时日志",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1000, 640, parent, nullptr, InstOf(parent), nullptr);
    if (hwnd) { ::ShowWindow(hwnd, SW_SHOW); ::UpdateWindow(hwnd); }
}

}  // namespace adbgui