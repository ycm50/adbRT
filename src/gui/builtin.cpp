// ---------------------------------------------------------------------------
// 内嵌功能：设备列表 / 设备信息 / 已安装应用 / 导出 APK / 文件浏览
//   · 所有 adb 往返都走 argv（不经 cmd.exe）
//   · 窗口只是壳，真正的数据由 core 里已有的会话与解析器提供
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "reports.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

HINSTANCE InstOf(HWND hwnd) {
    return reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
}

// 列表框里当前选中的文本；没有选中返回空
std::wstring SelectedText(HWND list) {
    int sel = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (sel < 0) return {};
    int len = static_cast<int>(::SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(sel), 0));
    if (len <= 0) return {};
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    ::SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(s.data()));
    s.resize(static_cast<size_t>(len));
    return s;
}

// ============================================================ 只读文本窗口
constexpr const wchar_t* kTextWndClass = L"AdbRTTextView";

// 后台加载线程与窗口共享的最小状态：窗口销毁后线程只看这个，
// 不碰 TextWin（它随时可能被 delete）。
struct TextShared {
    std::atomic<bool> cancelled{false};
    HWND hwnd = nullptr;
};

struct TextWin {
    std::wstring title;
    std::wstring body;                 // 立即可显示的内容（可为占位）
    std::function<std::wstring()> loader;   // 非空 = 后台线程算完再替换
    HWND  hwnd = nullptr;
    std::shared_ptr<TextShared> shared;     // 与后台线程共享
};

constexpr UINT WM_APP_TEXT_READY = WM_APP + 60;

struct LoadResult {
    HWND hwnd = nullptr;
    std::wstring text;
};

struct TextArg {
    std::shared_ptr<TextShared> shared;
    std::function<std::wstring()> loader;
    HWND hwnd = nullptr;
};

DWORD WINAPI TextLoaderProc(LPVOID param) {
    std::unique_ptr<TextArg> arg(static_cast<TextArg*>(param));
    std::wstring result;
    if (arg->loader) result = arg->loader();
    if (arg->shared->cancelled.load() || !arg->shared->hwnd) return 0;
    auto* heap = new LoadResult{arg->hwnd, std::move(result)};
    if (!::PostMessageW(arg->hwnd, WM_APP_TEXT_READY, 0, reinterpret_cast<LPARAM>(heap)))
        delete heap;
    return 0;
}

LRESULT CALLBACK TextProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<TextWin*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = reinterpret_cast<TextWin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;
            HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ctx->body.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_OUTPUT), nullptr, nullptr);
            ApplyFont(edit, CreateUiFont(9, true));
            ::SetWindowTextW(hwnd, ctx->title.c_str());
            if (ctx->loader) {
                ctx->shared = std::make_shared<TextShared>();
                ctx->shared->hwnd = hwnd;
                auto* arg = new TextArg{ctx->shared, ctx->loader, hwnd};
                DWORD tid = 0;
                HANDLE th = ::CreateThread(nullptr, 0, TextLoaderProc, arg, 0, &tid);
                if (th) ::CloseHandle(th);
                else delete arg;
            }
            return 0;
        }
        case WM_APP_TEXT_READY: {
            auto* res = reinterpret_cast<LoadResult*>(lp);
            if (res && ctx) {
                HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_OUTPUT);
                if (edit) ::SetWindowTextW(edit, res->text.c_str());
                ctx->body = res->text;
                delete res;
            }
            return 0;
        }
        case WM_SIZE: {
            HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_OUTPUT);
            if (edit) ::MoveWindow(edit, 4, 4, LOWORD(lp) - 8, HIWORD(lp) - 8, TRUE);
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (ctx) {
                if (ctx->shared) ctx->shared->cancelled = true;
                ctx->hwnd = nullptr;
                ::Sleep(80);          // 给后台线程一点时间看到 cancelled
            }
            delete ctx;
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureTextClass(HINSTANCE hinst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TextProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kTextWndClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

// ------------------------------------------------------------------ 文本窗口
void ShowTextView(HWND parent, const std::wstring& title, const std::wstring& text) {
    EnsureTextClass(InstOf(parent));
    auto* ctx = new TextWin{title, text, nullptr, nullptr, nullptr};
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, kTextWndClass, title.c_str(),
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  900, 600, parent, nullptr, InstOf(parent), ctx);
    if (hwnd) { ::ShowWindow(hwnd, SW_SHOW); ::UpdateWindow(hwnd); }
    else delete ctx;
}

// 异步版：窗口立刻出现，内容由后台线程填（设备信息要十几个往返，不能卡住 UI）
void ShowTextViewAsync(HWND parent, const std::wstring& title, const std::wstring& placeholder,
                       std::function<std::wstring()> loader) {
    EnsureTextClass(InstOf(parent));
    auto* ctx = new TextWin{title, placeholder, std::move(loader), nullptr, nullptr};
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, kTextWndClass, title.c_str(),
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  900, 600, parent, nullptr, InstOf(parent), ctx);
    if (hwnd) { ::ShowWindow(hwnd, SW_SHOW); ::UpdateWindow(hwnd); }
    else delete ctx;
}

// --------------------------------------------------------------- 设备列表
void ShowDeviceList(HWND parent) {
    ShowTextView(parent, L"设备列表", BuildDeviceListText());
}

// --------------------------------------------------------------- 设备信息
void ShowDeviceInfo(HWND parent) {
    auto& session = adb::DeviceSession::Instance();
    if (session.CurrentSerial().empty()) {
        ShowTextView(parent, L"设备信息", L"没有选中设备。请先连接设备并点「刷新设备」。");
        return;
    }
    ShowTextViewAsync(parent, L"设备信息 - " + session.CurrentSerial(),
                      L"正在读取设备信息…（需要十几次 adb 往返，请稍候）",
                      [] { return BuildDeviceInfoText(); });
}

// ------------------------------------------------------------- 已安装应用
namespace {

constexpr const wchar_t* kAppsWndClass = L"AdbRTAppsView";

struct AppsCtx {
    std::vector<std::wstring> packages;
};

void FillAppsList(HWND hwnd, AppsCtx* ctx) {
    HWND list = ::GetDlgItem(hwnd, IDC_LIST_MAIN);
    HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_FILTER);
    std::wstring f = adb::ToLowerAscii(GetTextOf(edit));
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& p : ctx->packages)
        if (f.empty() || adb::ToLowerAscii(p).find(f) != std::wstring::npos)
            ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
}

void DoExportApk(HWND hwnd) {
    HWND list = ::GetDlgItem(hwnd, IDC_LIST_MAIN);
    std::wstring pkg = SelectedText(list);
    if (pkg.empty()) {
        ::MessageBoxW(hwnd, L"请先选择一个应用。", L"AdbRT", MB_OK | MB_ICONINFORMATION);
        return;
    }

    auto& session = adb::DeviceSession::Instance();
    std::wstring reply = session.Shell(L"pm path " + pkg, 12000);
    size_t pos = reply.find(L"package:");
    if (pos == std::wstring::npos) {
        ::MessageBoxW(hwnd, (L"拿不到该应用的 APK 路径：\n" + reply).c_str(),
                      L"AdbRT", MB_OK | MB_ICONWARNING);
        return;
    }
    size_t eol = reply.find(L'\n', pos);
    std::wstring remotePath = adb::Trim(reply.substr(pos + 8, eol == std::wstring::npos
                                                                 ? std::wstring::npos
                                                                 : eol - pos - 8));

    wchar_t buf[MAX_PATH * 2] = {0};
    std::wstring suggested = adb::JoinPath(adb::Settings::Instance().DownloadDir(),
                                           pkg + L".apk");
    adb::EnsureDirectory(adb::Settings::Instance().DownloadDir());
    lstrcpynW(buf, suggested.c_str(), MAX_PATH * 2);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"APK\0*.apk\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"保存 APK 到";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&ofn)) return;

    adb::RunOptions opts;
    opts.timeoutMs = 120000;
    auto r = adb::RunProcess(session.AdbExe(), session.WithSerial({L"pull", remotePath, buf}), opts);
    if (r.Ok() && adb::PathExists(buf)) {
        ::MessageBoxW(hwnd, (L"已导出到：\n" + std::wstring(buf)).c_str(),
                      L"AdbRT", MB_OK | MB_ICONINFORMATION);
    } else {
        ::MessageBoxW(hwnd, (L"导出失败：\n" + r.error + r.ErrW() + r.OutW()).c_str(),
                      L"AdbRT", MB_OK | MB_ICONERROR);
    }
}

LRESULT CALLBACK AppsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<AppsCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = reinterpret_cast<AppsCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_FILTER), nullptr, nullptr);
            HWND list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_MAIN), nullptr, nullptr);
            HWND btn = ::CreateWindowExW(0, L"BUTTON", L"导出 APK…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_ACTION1), nullptr, nullptr);
            ApplyFont(edit, CreateUiFont(9, false));
            ApplyFont(list, CreateUiFont(9, true));
            ApplyFont(btn, CreateUiFont(9, false));
            FillAppsList(hwnd, ctx);
            ::SetWindowTextW(hwnd, L"已安装应用");
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_EDIT_FILTER), 4, 4, w - 8, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_LIST_MAIN), 4, 32, w - 8, h - 72, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_BTN_ACTION1), w - 116, h - 34, 106, 26, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_EDIT_FILTER && code == EN_CHANGE) { FillAppsList(hwnd, ctx); return 0; }
            if (id == IDC_BTN_ACTION1 || (id == IDC_LIST_MAIN && code == LBN_DBLCLK)) {
                DoExportApk(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            delete reinterpret_cast<AppsCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureAppsClass(HINSTANCE hinst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AppsProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kAppsWndClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowInstalledApps(HWND parent) {
    auto& session = adb::DeviceSession::Instance();
    if (session.CurrentSerial().empty()) {
        ShowTextView(parent, L"已安装应用", L"没有选中设备。请先连接设备并点「刷新设备」。");
        return;
    }
    auto* ctx = new AppsCtx();
    for (const auto& p : session.InstalledPackages(false)) ctx->packages.push_back(p.package);
    EnsureAppsClass(InstOf(parent));
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, kAppsWndClass, L"已安装应用",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  620, 640, parent, nullptr, InstOf(parent), ctx);
    if (hwnd) { ::ShowWindow(hwnd, SW_SHOW); ::UpdateWindow(hwnd); }
    else delete ctx;
}

void ShowPullApk(HWND parent) { ShowInstalledApps(parent); }

}  // namespace adbgui