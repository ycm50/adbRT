// ---------------------------------------------------------------------------
// 应用包选择器：带复选框的应用列表（含系统应用）
//   · 列：包名 | 类型
//   · 只显示设备上真实存在的包名，不做任何 APK 解析
//     （Android 的 dumpsys / pm 都不输出应用名称，要拿到名称只能拉 APK 再解析
//      资源表 —— 每个包动辄上百 MB，488 个包要跑几十分钟，代价太大，已移除）
//   · 支持多选，确定后把包名用 ";" 连接写回参数框（构造器会展开成多条命令）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

constexpr const wchar_t* kPkgWndClass = L"AdbRTPkgPicker";

struct PkgCtx {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND filter = nullptr;
    HWND chkSystem = nullptr;
    HWND status = nullptr;

    std::vector<adb::PackageEntry> all;    // 全部包
    std::vector<int>               shown;  // 当前显示的行 → all 下标
    std::vector<std::wstring>      hay;    // 每行的小写索引串（过滤用）
    std::vector<int>               rowOf;  // all 下标 → 列表框行号（-1 = 未显示）
    std::vector<std::wstring>      selected;
    bool  showSystem = true;
    bool  done = false;
    bool  filling = false;

    std::function<void(const std::vector<std::wstring>&)> onDone;
};


// 数据下标 → 列表框行号（-1 表示当前没显示）
// 原来每次名称解析完都线性扫一遍 shown，几百个包就是 O(n²)；
// 现在用反查表，解析阶段一次性建好。
void RebuildRowIndex(PkgCtx* ctx) {
    ctx->rowOf.assign(ctx->all.size(), -1);
    for (size_t r = 0; r < ctx->shown.size(); ++r) {
        const size_t idx = static_cast<size_t>(ctx->shown[r]);
        if (idx < ctx->rowOf.size()) ctx->rowOf[idx] = static_cast<int>(r);
    }
}

void UpdateStatus(PkgCtx* ctx) {
    int sys = 0, third = 0;
    for (const auto& p : ctx->all) (p.system ? sys : third++);
    SetTextOf(ctx->status,
              adb::Format(L"共 %zu 个应用（系统 %d / 第三方 %d）  显示 %zu  已勾选 %zu",
                          ctx->all.size(), sys, third, ctx->shown.size(), ctx->selected.size()));
}

// 过滤用的"小写 haystack"缓存：包名是只读的，
// 原来每敲一个字就为每一行重新 ToLowerAscii（几千行 = 上万次分配），
// 这是搜索框打字卡顿的直接原因。这里一次性算好，之后直接比较。
void BuildHaystacks(PkgCtx* ctx) {
    ctx->hay.clear();
    ctx->hay.reserve(ctx->all.size());
    for (const auto& a : ctx->all) ctx->hay.push_back(adb::ToLowerAscii(a.package));
}

void RefillList(PkgCtx* ctx) {
    ctx->filling = true;
    ::SendMessageW(ctx->list, LVM_DELETEALLITEMS, 0, 0);
    ctx->shown.clear();

    std::wstring f = adb::ToLowerAscii(GetTextOf(ctx->filter));
    const size_t total = ctx->all.size();
    if (ctx->hay.size() != total) BuildHaystacks(ctx);
    ctx->shown.reserve(total);

    for (size_t i = 0; i < total; ++i) {
        const auto& p = ctx->all[i];
        if (!ctx->showSystem && p.system) continue;
        if (!f.empty() && ctx->hay[i].find(f) == std::wstring::npos) continue;
        ctx->shown.push_back(static_cast<int>(i));
    }

    // 一次性批量插入 + 只设置文本，避免每行再额外 FillRow 两次 SetItem
    ::SendMessageW(ctx->list, WM_SETREDRAW, FALSE, 0);
    for (size_t row = 0; row < ctx->shown.size(); ++row) {
        const auto& a = ctx->all[static_cast<size_t>(ctx->shown[row])];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(row);
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(a.package.c_str());
        item.lParam = ctx->shown[row];
        int idx = static_cast<int>(::SendMessageW(ctx->list, LVM_INSERTITEMW, 0,
                                                  reinterpret_cast<LPARAM>(&item)));
        if (idx < 0) continue;
        LVITEMW sub{};
        sub.mask = LVIF_TEXT;
        sub.iItem = idx;
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t*>(a.system ? L"系统" : L"第三方");
        ::SendMessageW(ctx->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));
    }
    ::SendMessageW(ctx->list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(ctx->list, nullptr, TRUE);

    RebuildRowIndex(ctx);
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
        if (idx < ctx->all.size()) ctx->selected.push_back(ctx->all[idx].package);
    }
}

std::vector<adb::PackageEntry> LoadPackages(const std::wstring& serial) {
    auto& session = adb::DeviceSession::Instance();
    if (serial.empty()) return {};
    // 两次 adb 调用就能拿到全部包名与"系统/第三方"归属，秒开
    return session.InstalledPackages(true);
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
                           reinterpret_cast<LPARAM>(L"按包名过滤…"));

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
            // 列序：包名 | 类型
            col.pszText = const_cast<wchar_t*>(L"包名");
            col.cx = 480;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
            col.pszText = const_cast<wchar_t*>(L"类型");
            col.cx = 80;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

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

            ctx->all = LoadPackages(adb::DeviceSession::Instance().CurrentSerial());
            RefillList(ctx);
            ::SetWindowTextW(hwnd, L"选择应用（可勾选多个）");
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