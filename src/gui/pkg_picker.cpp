// ---------------------------------------------------------------------------
// 应用包选择器：带复选框的应用列表（含系统应用）
//   · 列：应用名称 | 包名 | 类型
//   · 名称来自 APK 资源解析（主机没有 aapt、dumpsys 也不给 label）
//   · 解析结果按设备缓存，第二次打开秒开；首次打开后台补全
//   · 支持多选，确定后把包名用 ";" 连接写回参数框（构造器会展开成多条命令）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/device.h"
#include "../core/label_cache.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

constexpr const wchar_t* kPkgWndClass = L"AdbRTPkgPicker";
constexpr UINT WM_LABEL_READY = WM_APP + 70;

// 后台解析线程与窗口共享的状态。窗口先关时把 cancel 置位，
// 线程拿着 shared_ptr，绝不会碰到已经 delete 的 PkgCtx。
struct PkgState {
    std::mutex mu;                        // 保护 all
    std::vector<adb::AppLabel> all;       // 全部包（含解析后的名称）
    std::atomic<bool> cancel{false};
};

struct PkgCtx {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND filter = nullptr;
    HWND chkSystem = nullptr;
    HWND status = nullptr;

    std::shared_ptr<PkgState>      state;  // 与后台线程共享（见 PkgState）
    std::vector<int>               shown;  // 当前显示的行 → all 下标
    std::vector<std::wstring>      selected;
    bool  showSystem = true;
    bool  done = false;
    bool  filling = false;

    std::atomic<bool> resolving{false};
    HANDLE resolveThread = nullptr;

    std::function<void(const std::vector<std::wstring>&)> onDone;
};


// 数据下标 → 列表框行号（-1 表示当前没显示）
int RowOfIndex(const PkgCtx* ctx, size_t index) {
    for (size_t r = 0; r < ctx->shown.size(); ++r)
        if (static_cast<size_t>(ctx->shown[r]) == index) return static_cast<int>(r);
    return -1;
}

void UpdateStatus(PkgCtx* ctx) {
    int sys = 0, third = 0;
    for (const auto& p : ctx->state->all) (p.system ? sys : third++);
    SetTextOf(ctx->status,
              adb::Format(L"共 %zu 个应用（系统 %d / 第三方 %d）  显示 %zu  已勾选 %zu%s",
                          ctx->state->all.size(), sys, third, ctx->shown.size(), ctx->selected.size(),
                          ctx->resolving.load() ? L"  · 正在读取应用名称…" : L""));
}

// 写一行（名称 / 包名 / 类型）
void FillRow(PkgCtx* ctx, int row, const adb::AppLabel& a) {
    // 列序：包名 | 应用名称 | 类型
    std::wstring name = a.label.empty() ? L"…" : a.label;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = const_cast<wchar_t*>(a.package.c_str());
    ::SendMessageW(ctx->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));

    item.iSubItem = 1;
    item.pszText = const_cast<wchar_t*>(name.c_str());
    ::SendMessageW(ctx->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));

    item.iSubItem = 2;
    item.pszText = const_cast<wchar_t*>(a.system ? L"系统" : L"第三方");
    ::SendMessageW(ctx->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
}

void RefillList(PkgCtx* ctx) {
    ctx->filling = true;
    ::SendMessageW(ctx->list, LVM_DELETEALLITEMS, 0, 0);
    ctx->shown.clear();

    std::wstring f = adb::ToLowerAscii(GetTextOf(ctx->filter));
    for (size_t i = 0; i < ctx->state->all.size(); ++i) {
        const auto& p = ctx->state->all[i];
        if (!ctx->showSystem && p.system) continue;
        if (!f.empty()) {
            std::wstring hay = adb::ToLowerAscii(p.package) + L" " + adb::ToLowerAscii(p.label);
            if (hay.find(f) == std::wstring::npos) continue;
        }
        ctx->shown.push_back(static_cast<int>(i));
    }

    for (size_t row = 0; row < ctx->shown.size(); ++row) {
        const auto& a = ctx->state->all[static_cast<size_t>(ctx->shown[row])];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(row);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(a.package.c_str());
        item.lParam = ctx->shown[row];
        int idx = static_cast<int>(::SendMessageW(ctx->list, LVM_INSERTITEMW, 0,
                                                  reinterpret_cast<LPARAM>(&item)));
        FillRow(ctx, idx, a);
    }

    ctx->filling = false;
    UpdateStatus(ctx);
}

void CollectChecked(PkgCtx* ctx) {
    ctx->selected.clear();
    int count = static_cast<int>(::SendMessageW(ctx->list, LVM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        LRESULT state = ::SendMessageW(ctx->list, LVM_GETITEMSTATE, static_cast<WPARAM>(i),
                                       LVIS_STATEIMAGEMASK);
        if (state != INDEXTOSTATEIMAGEMASK(2)) continue;   // 2 = 勾选
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ::SendMessageW(ctx->list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
        size_t idx = static_cast<size_t>(item.lParam);
        if (idx < ctx->state->all.size()) ctx->selected.push_back(ctx->state->all[idx].package);
    }
}

std::vector<adb::AppLabel> LoadPackages(const std::wstring& serial) {
    auto& session = adb::DeviceSession::Instance();
    std::vector<adb::AppLabel> out;
    if (serial.empty()) return out;

    std::map<std::wstring, std::wstring> labels;
    std::vector<adb::AppLabel> cached;
    if (adb::LoadLabelCache(serial, &cached))
        for (const auto& c : cached) labels[c.package] = c.label;

    for (const auto& p : session.InstalledPackages(true)) {
        adb::AppLabel a;
        a.package = p.package;
        a.system = p.system;
        auto it = labels.find(p.package);
        if (it != labels.end()) a.label = it->second;
        out.push_back(a);
    }
    return out;
}

// 后台线程：给还没有名称的包逐个 pull + 解析，解析一个就通知界面刷新
// 只在 shared state 上工作，窗口提前关掉也不会碰到悬垂指针
struct ResolveArg {
    HWND hwnd = nullptr;
    std::shared_ptr<PkgState> state;
};

DWORD WINAPI ResolveLabelsProc(LPVOID param) {
    std::unique_ptr<ResolveArg> arg(static_cast<ResolveArg*>(param));
    std::shared_ptr<PkgState> st = arg->state;
    HWND hwnd = arg->hwnd;

    auto& session = adb::DeviceSession::Instance();
    std::wstring serial = session.CurrentSerial();
    if (serial.empty()) return 0;

    std::wstring tmpDir = adb::JoinPath(adb::Settings::Instance().DownloadDir(), L"AdbRT-tmp");
    adb::EnsureDirectory(tmpDir);
    std::wstring local = adb::JoinPath(tmpDir, L"adbrt-label.apk");

    int resolved = 0;
    for (size_t i = 0;; ++i) {
        if (st->cancel.load()) break;
        std::wstring pkg;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            if (i >= st->all.size()) break;
            if (!st->all[i].label.empty()) continue;
            pkg = st->all[i].package;
        }

        std::wstring reply = session.Shell(L"pm path " + pkg, 10000);
        size_t pos = reply.find(L"package:");
        if (pos == std::wstring::npos) continue;
        size_t eol = reply.find(L'\n', pos);
        std::wstring remote = adb::Trim(reply.substr(
            pos + 8, eol == std::wstring::npos ? std::wstring::npos : eol - pos - 8));
        if (remote.empty()) continue;

        adb::RunOptions opts;
        opts.timeoutMs = 30000;
        opts.cancel = &st->cancel;
        auto pull = adb::RunProcess(session.AdbExe(),
                                    session.WithSerial({L"pull", remote, local}), opts);
        if (!pull.Ok()) continue;

        std::wstring label = adb::ReadApkLabel(local);
        ::DeleteFileW(local.c_str());
        if (label.empty()) continue;
        if (st->cancel.load()) break;

        {
            std::lock_guard<std::mutex> lk(st->mu);
            if (i < st->all.size()) st->all[i].label = label;
        }
        ++resolved;
        auto* heap = new size_t(i);
        if (!::PostMessageW(hwnd, WM_LABEL_READY, 0, reinterpret_cast<LPARAM>(heap)))
            delete heap;
    }

    {
        std::lock_guard<std::mutex> lk(st->mu);
        if (!st->cancel.load()) adb::SaveLabelCache(serial, st->all);
    }
    adb::LogF(adb::LogLevel::Info, "picker", L"应用名称解析完成，新增 %d 个", resolved);
    auto* done = new size_t(0);
    if (!::PostMessageW(hwnd, WM_LABEL_READY, 1, reinterpret_cast<LPARAM>(done)))
        delete done;
    return 0;
}

LRESULT CALLBACK PkgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<PkgCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = reinterpret_cast<PkgCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;

            ctx->filter = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_FILTER), nullptr, nullptr);
            ::SendMessageW(ctx->filter, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"按名称或包名过滤…"));

            ctx->chkSystem = ::CreateWindowExW(0, L"BUTTON", L"显示系统应用",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CHK_EXPERT), nullptr, nullptr);
            ::SendMessageW(ctx->chkSystem, BM_SETCHECK,
                           ctx->showSystem ? BST_CHECKED : BST_UNCHECKED, 0);

            ctx->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_MAIN), nullptr, nullptr);
            ::SendMessageW(ctx->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                           LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                           LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            // 列序按用户要求：包名 → 应用名称 → 类型
            col.pszText = const_cast<wchar_t*>(L"包名");
            col.cx = 320;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
            col.pszText = const_cast<wchar_t*>(L"应用名称");
            col.cx = 230;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));
            col.pszText = const_cast<wchar_t*>(L"类型");
            col.cx = 70;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&col));

            HWND okBtn = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancelBtn = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            ctx->status = ::CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATUSBAR), nullptr, nullptr);

            for (HWND c : {ctx->filter, ctx->chkSystem, okBtn, cancelBtn, ctx->status})
                ApplyFont(c, CreateUiFont(9, false));

            ctx->state = std::make_shared<PkgState>();
            ctx->state->all = LoadPackages(adb::DeviceSession::Instance().CurrentSerial());
            RefillList(ctx);
            ::SetWindowTextW(hwnd, L"选择应用（可勾选多个）");

            // 有缺名称的就在后台补
            bool needResolve = false;
            for (const auto& a : ctx->state->all) if (a.label.empty()) { needResolve = true; break; }
            if (needResolve) {
                ctx->resolving = true;
                auto* arg = new ResolveArg{ctx->hwnd, ctx->state};
                DWORD tid = 0;
                HANDLE th = ::CreateThread(nullptr, 0, ResolveLabelsProc, arg, 0, &tid);
                if (th) ::CloseHandle(th);      // 线程对象结束后由系统回收
                else { delete arg; ctx->resolving = false; }
                UpdateStatus(ctx);
            }
            return 0;
        }
        case WM_LABEL_READY: {
            auto* idx = reinterpret_cast<size_t*>(lp);
            if (!idx || !ctx) return 0;
            bool finished = wp == 1;
            if (!finished) {
                size_t i = *idx;
                adb::AppLabel item;
                bool found = false;
                {
                    // 只在锁里拷一份，发控件消息放到锁外 —— 持锁调用窗口消息是隐患
                    std::lock_guard<std::mutex> lk(ctx->state->mu);
                    if (i < ctx->state->all.size()) { item = ctx->state->all[i]; found = true; }
                }
                if (found) {
                    int row = RowOfIndex(ctx, i);
                    if (row >= 0) FillRow(ctx, row, item);
                }
                delete idx;
            } else {
                delete idx;
                ctx->resolving = false;
                UpdateStatus(ctx);
            }
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(ctx->filter, 6, 6, w - 160, 24, TRUE);
            ::MoveWindow(ctx->chkSystem, w - 148, 6, 142, 24, TRUE);
            ::MoveWindow(ctx->list, 6, 36, w - 12, h - 36 - 34 - 24, TRUE);
            ::MoveWindow(ctx->status, 6, h - 28, w - 220, 20, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDOK), w - 200, h - 30, 90, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDCANCEL), w - 104, h - 30, 90, 24, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_EDIT_FILTER && code == EN_CHANGE) { RefillList(ctx); return 0; }
            if (id == IDC_CHK_EXPERT && code == BN_CLICKED) {
                ctx->showSystem = ::SendMessageW(ctx->chkSystem, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RefillList(ctx);
                return 0;
            }
            if (id == IDOK) {
                CollectChecked(ctx);
                ctx->done = true;
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<LPNMHDR>(lp);
            if (nm->idFrom == IDC_LIST_MAIN && nm->code == LVN_ITEMCHANGED) {
                if (ctx->filling) return 0;      // 批量填充期间不处理，避免 O(n²)
                auto* nv = reinterpret_cast<LPNMLISTVIEW>(lp);
                if ((nv->uChanged & LVIF_STATE) &&
                    ((nv->uNewState ^ nv->uOldState) & LVIS_STATEIMAGEMASK)) {
                    CollectChecked(ctx);
                    UpdateStatus(ctx);
                }
            }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY: {
            auto* c = reinterpret_cast<PkgCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (c) {
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                // 后台线程只持有 shared state 与 hwnd 数值，
                // 这里置位取消后立刻释放窗口上下文是安全的：
                // 线程后续的 PostMessage 会因为窗口已销毁而失败，指针不会被解引用。
                c->resolving = false;
                if (c->state) c->state->cancel = true;
                if (!c->done) c->selected.clear();
                if (c->onDone) c->onDone(c->selected);
                delete c;
            }
            return 0;
        }
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsurePkgClass(HINSTANCE hinst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PkgProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPkgWndClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowPackagePicker(HWND parent, bool showSystemDefault,
                       std::function<void(const std::vector<std::wstring>&)> onDone) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    EnsurePkgClass(inst);

    auto* ctx = new PkgCtx();
    ctx->showSystem = showSystemDefault;
    ctx->onDone = std::move(onDone);

    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, kPkgWndClass,
                                  L"选择应用（可勾选多个）",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 760, 660,
                                  parent, nullptr, inst, ctx);
    if (!hwnd) { delete ctx; return; }

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    ::EnableWindow(parent, FALSE);
    MSG msg;
    while (::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::EnableWindow(parent, TRUE);
    ::SetActiveWindow(parent);
}

}  // namespace adbgui