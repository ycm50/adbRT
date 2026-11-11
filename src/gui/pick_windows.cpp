// ---------------------------------------------------------------------------
// 参数面板用到的几个专用窗口
//   · 文件复选（文件夹内挑文件，与"整个文件夹推送"互斥的另一半）
//   · 系统属性多选（勾选后依次 getprop）
//   · 三段式 shell 命令编辑器（命令下拉 + 参数复选 + 参数值 + 管道）
//   · 截图取点（点击 / 滑动都在手机截图上用鼠标完成）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/lan_scan.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "ui_util.h"

namespace adbgui {

namespace {

using namespace theme;


HWND RootOf(HWND hwnd) {
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

HINSTANCE InstOf(HWND hwnd) {
    // 参数面板是个子控件（STATIC），它的 GWLP_HINSTANCE 不可靠，
    // 拿它去 RegisterClassExW 会失败 —— 必须用进程模块句柄。
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    if (!inst) inst = static_cast<HINSTANCE>(::GetModuleHandleW(nullptr));
    return inst;
}

// 模态：禁用整个顶层窗口后自己跑消息循环
void RunModal(HWND parent, HWND hwnd) {
    HWND root = ::GetAncestor(parent, GA_ROOT);
    if (!root) root = parent;
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    ::EnableWindow(root, FALSE);
    MSG msg;
    while (::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::EnableWindow(root, TRUE);
    ::SetActiveWindow(root);
}

// 关窗口前把结果写进 out
struct PickerBase {
    HWND hwnd = nullptr;
    bool accepted = false;
};

// ------------------------------------------------------------- 文件复选
struct FilesCtx : PickerBase {
    std::wstring folder;
    std::vector<std::wstring> names;
    std::vector<std::wstring>* out = nullptr;
};

void FilesCollect(FilesCtx* ctx, HWND list) {
    if (!ctx->out) return;
    ctx->out->clear();
    int n = static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < n; ++i) {
        if (::SendMessageW(list, LB_GETSEL, static_cast<WPARAM>(i), 0) <= 0) continue;
        int len = static_cast<int>(::SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0));
        if (len <= 0) continue;
        std::wstring buf(static_cast<size_t>(len) + 1, L' ');
        ::SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(buf.data()));
        buf.resize(static_cast<size_t>(len));
        ctx->out->push_back(buf);
    }
}

LRESULT CALLBACK FilesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<FilesCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = static_cast<FilesCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;

            HWND list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT |
                    WS_TABSTOP,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_MAIN), nullptr, nullptr);
            ApplyFont(list, g_app.font);

            HWND all = ::CreateWindowExW(0, L"BUTTON", L"全选",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_ACTION1), nullptr, nullptr);
            HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            ApplyFont(all, g_app.font);
            ApplyFont(ok, g_app.font);
            ApplyFont(cancel, g_app.font);

            ::SetWindowTextW(hwnd, (L"勾选要推送的文件 - " + ctx->folder).c_str());
            ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"（正在读取文件夹…）"));

            // 目录可能很大，读到 4000 个就够用了
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((ctx->folder + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
                size_t count = 0;
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    ::SendMessageW(list, LB_ADDSTRING, 0,
                                   reinterpret_cast<LPARAM>(fd.cFileName));
                    if (++count >= 4000) break;
                } while (::FindNextFileW(h, &fd));
                ::FindClose(h);
            }
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_LIST_MAIN), 6, 6, w - 12, h - 6 - 36, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_BTN_ACTION1), 6, h - 30, 80, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDOK), w - 194, h - 30, 90, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDCANCEL), w - 98, h - 30, 90, 24, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_BTN_ACTION1) {
                HWND list = ::GetDlgItem(hwnd, IDC_LIST_MAIN);
                int n = static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
                for (int i = 0; i < n; ++i)
                    ::SendMessageW(list, LB_SETSEL, TRUE, static_cast<LPARAM>(i));
                return 0;
            }
            if (id == IDOK) {
                FilesCollect(ctx, ::GetDlgItem(hwnd, IDC_LIST_MAIN));
                if (ctx->out && ctx->out->empty()) {
                    ::MessageBoxW(hwnd, L"至少要勾选一个文件。", L"AdbRT", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ctx->accepted = true;
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ListView 默认行高偏大，塞一个 1px 的 image list 把行距压回正常水平
void ShrinkRows(HWND list) {
    HIMAGELIST il = ::ImageList_Create(1, ::GetSystemMetrics(SM_CYSMICON),
                                       ILC_COLOR4, 1, 1);
    if (il) ::SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL, reinterpret_cast<LPARAM>(il));
}

// ------------------------------------------------------------- 属性多选
struct PropsCtx : PickerBase {
    std::wstring current;                 // 进来时已经选中的（";" 连接）
    std::wstring* out = nullptr;
    std::vector<std::pair<std::wstring, std::wstring>> items;   // key, value
    HWND list = nullptr;
    bool filling = false;
};

void PropsFill(PropsCtx* ctx, const std::wstring& filter) {
    ctx->filling = true;
    ::SendMessageW(ctx->list, LVM_DELETEALLITEMS, 0, 0);
    std::wstring f = adb::ToLowerAscii(filter);
    for (size_t i = 0; i < ctx->items.size(); ++i) {
        const auto& kv = ctx->items[i];
        if (!f.empty()) {
            std::wstring hay = adb::ToLowerAscii(kv.first) + L" " + adb::ToLowerAscii(kv.second);
            if (hay.find(f) == std::wstring::npos) continue;
        }
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(::SendMessageW(ctx->list, LVM_GETITEMCOUNT, 0, 0));
        it.iSubItem = 0;
        it.pszText = const_cast<wchar_t*>(kv.first.c_str());
        it.lParam = static_cast<LPARAM>(i);
        int row = static_cast<int>(::SendMessageW(ctx->list, LVM_INSERTITEMW, 0,
                                                  reinterpret_cast<LPARAM>(&it)));
        LVITEMW sub{};
        sub.mask = LVIF_TEXT;
        sub.iItem = row;
        sub.iSubItem = 1;
        sub.pszText = const_cast<wchar_t*>(kv.second.c_str());
        ::SendMessageW(ctx->list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&sub));

        // 已经在 current 里的默认勾上
        bool on = false;
        std::wstring want = L";" + ctx->current + L";";
        if (want.find(L";" + kv.first + L";") != std::wstring::npos) on = true;
        LVITEMW st{};
        st.mask = LVIF_STATE;
        st.stateMask = LVIS_STATEIMAGEMASK;
        st.state = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
        ::SendMessageW(ctx->list, LVM_SETITEMSTATE, static_cast<WPARAM>(row),
                       reinterpret_cast<LPARAM>(&st));
    }
    ctx->filling = false;
}

LRESULT CALLBACK PropsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<PropsCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = static_cast<PropsCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;

            HWND filter = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_FILTER), nullptr, nullptr);
            ::SendMessageW(filter, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"按属性名过滤…"));
            ::SendMessageW(filter, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"按属性名过滤…"));

            ctx->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_LIST_MAIN), nullptr, nullptr);
            ::SendMessageW(ctx->list, LVM_SETEXTENDEDLISTVIEWSTYLE,
                           LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                           LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ShrinkRows(ctx->list);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(L"属性名");
            col.cx = 300;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
            col.pszText = const_cast<wchar_t*>(L"当前值");
            col.cx = 320;
            ::SendMessageW(ctx->list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

            HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            for (HWND c : {filter, ok, cancel}) ApplyFont(c, g_app.font);
            ApplyFont(ctx->list, g_app.font);

            // 读设备属性：getprop 每行 "[key]: [value]"
            std::wstring reply = adb::DeviceSession::Instance().Shell(L"getprop", 20000);
            for (const auto& line : adb::SplitLines(reply)) {
                std::wstring t = adb::Trim(line);
                if (t.size() < 4 || t.front() != L'[') continue;
                size_t close = t.find(L"]:");
                if (close == std::wstring::npos) continue;
                std::wstring key = t.substr(1, close - 1);
                std::wstring value;
                size_t vb = t.find(L'[', close);
                if (vb != std::wstring::npos) {
                    size_t ve = t.find(L']', vb);
                    value = t.substr(vb + 1, ve == std::wstring::npos ? std::wstring::npos : ve - vb - 1);
                }
                ctx->items.emplace_back(key, value);
            }
            if (ctx->items.empty()) {
                for (const auto& k : adb::CommonProps()) ctx->items.emplace_back(k, L"");
            }
            PropsFill(ctx, L"");
            ::SetWindowTextW(hwnd, L"选择要读取的系统属性（可多选）");
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_EDIT_FILTER), 6, 6, w - 12, 24, TRUE);
            ::MoveWindow(ctx->list, 6, 36, w - 12, h - 36 - 34 - 6, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDOK), w - 194, h - 30, 90, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDCANCEL), w - 98, h - 30, 90, 24, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_EDIT_FILTER && code == EN_CHANGE) {
                PropsFill(ctx, GetTextOf(::GetDlgItem(hwnd, IDC_EDIT_FILTER)));
                return 0;
            }
            if (id == IDOK) {
                if (ctx->out) {
                    ctx->out->clear();
                    int n = static_cast<int>(::SendMessageW(ctx->list, LVM_GETITEMCOUNT, 0, 0));
                    for (int i = 0; i < n; ++i) {
                        LRESULT st = ::SendMessageW(ctx->list, LVM_GETITEMSTATE,
                                                    static_cast<WPARAM>(i), LVIS_STATEIMAGEMASK);
                        if (st != INDEXTOSTATEIMAGEMASK(2)) continue;
                        LVITEMW it{};
                        it.mask = LVIF_PARAM;
                        it.iItem = i;
                        ::SendMessageW(ctx->list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it));
                        size_t idx = static_cast<size_t>(it.lParam);
                        if (idx >= ctx->items.size()) continue;
                        if (!ctx->out->empty()) *ctx->out += L";";
                        *ctx->out += ctx->items[idx].first;
                    }
                }
                ctx->accepted = true;
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ------------------------------------------------------------- 三段式 shell
struct ShellParamDef {
    const wchar_t* label;
    const wchar_t* arg;      // 进命令行的形态；空 = 纯值参数（只放值）
    bool           hasValue; // 勾上后还要填一个值
};

struct ShellCmdDef {
    const wchar_t* name;
    std::vector<ShellParamDef> params;
};

// 常用命令 + 每个命令的可用参数（参数勾上才进命令行，值非必填）
const std::vector<ShellCmdDef>& ShellCommands() {
    static const std::vector<ShellCmdDef> k = {
        {L"pm list packages", {{L"只列第三方 (-3)", L"-3", false},
                               {L"含已停用 (-d)", L"-d", false},
                               {L"含系统 (-s)", L"-s", false},
                               {L"显示 APK 路径 (-f)", L"-f", false},
                               {L"指定用户 (--user)", L"--user", true}}},
        {L"pm uninstall", {{L"保留数据 (-k)", L"-k", false},
                           {L"指定用户 (--user)", L"--user", true},
                           {L"包名", L"", true}}},
        {L"pm clear", {{L"包名", L"", true}}},
        {L"pm path", {{L"包名", L"", true}}},
        {L"pm grant", {{L"包名", L"", true}, {L"权限名", L"", true}}},
        {L"pm list permissions", {{L"只列危险权限 (-g)", L"-g", false},
                                  {L"只列第三方 (-3)", L"-3", false}}},
        {L"am force-stop", {{L"包名", L"", true}}},
        {L"am start", {{L"组件 (-n)", L"-n", true},
                       {L"Action (-a)", L"-a", true},
                       {L"Data (-d)", L"-d", true}}},
        {L"am broadcast", {{L"Action (-a)", L"-a", true}, {L"组件 (-n)", L"-n", true}}},
        {L"dumpsys", {{L"服务名", L"", true}, {L"只看该服务的列表 (-l)", L"-l", false}}},
        {L"getprop", {{L"属性名（留空=全部）", L"", true}}},
        {L"setprop", {{L"属性名", L"", true}, {L"值", L"", true}}},
        {L"settings get", {{L"命名空间 (system/secure/global)", L"", true},
                           {L"键名", L"", true}}},
        {L"settings put", {{L"命名空间", L"", true}, {L"键名", L"", true}, {L"值", L"", true}}},
        {L"wm size", {{L"分辨率（如 1080x2400）", L"", true}}},
        {L"wm density", {{L"密度（如 420）", L"", true}}},
        {L"input text", {{L"文本", L"", true}}},
        {L"input keyevent", {{L"键码", L"", true}}},
        {L"ls", {{L"详细 (-l)", L"-l", false}, {L"含隐藏 (-a)", L"-a", false},
                 {L"路径", L"", true}}},
        {L"cat", {{L"路径", L"", true}}},
        {L"ps", {{L"全部进程 (-A)", L"-A", false}}},
        {L"df", {{L"人类可读 (-h)", L"-h", false}}},
        {L"id", {}},
        {L"cmd", {{L"服务名", L"", true}, {L"子命令", L"", true}}},
        {L"dpm", {{L"子命令", L"", true}}},
    };
    return k;
}

// 值里有空格就加单引号；值里带单引号则拒绝加引号（避免破坏设备侧引号配对）
std::wstring ShellQuote(const std::wstring& v) {
    if (v.empty()) return v;
    if (v.find(L'\x27') != std::wstring::npos) return v;
    for (wchar_t c : v) {
        if (c == L' ' || c == L'\t') return L"'" + v + L"'";
    }
    return v;
}

struct SegParamUi {
    int  index = 0;
    HWND chk = nullptr;
    HWND edit = nullptr;
};

struct Segment {
    int  cmdIndex = 0;
    HWND combo = nullptr;
    std::vector<SegParamUi> params;
};

struct ShellCtx : PickerBase {
    std::vector<Segment> segs;
    std::wstring* out = nullptr;
    int scrollY = 0;
    int contentH = 0;
    HWND preview = nullptr;
    bool refreshing = false;   // 防止 SetWindowText 触发的 EN_CHANGE 递归刷新
};

int SegmentHeight(const Segment& s) {
    return 34 + static_cast<int>(s.params.size()) * 28 + 10;
}

std::wstring SegmentText(const Segment& s) {
    if (s.cmdIndex < 0 || s.cmdIndex >= static_cast<int>(ShellCommands().size())) return {};
    const ShellCmdDef& def = ShellCommands()[static_cast<size_t>(s.cmdIndex)];
    std::wstring out = def.name;
    for (const auto& pu : s.params) {
        if (pu.index < 0 || pu.index >= static_cast<int>(def.params.size())) continue;
        if (::SendMessageW(pu.chk, BM_GETCHECK, 0, 0) != BST_CHECKED) continue;
        const ShellParamDef& pd = def.params[static_cast<size_t>(pu.index)];
        std::wstring value = (pd.hasValue && pu.edit) ? adb::Trim(GetTextOf(pu.edit)) : std::wstring();
        if (!pd.arg || !*pd.arg) {
            if (!value.empty()) out += L" " + ShellQuote(value);
            continue;
        }
        out += L" ";
        out += pd.arg;
        if (pd.hasValue && !value.empty()) out += L" " + ShellQuote(value);
    }
    return out;
}

std::wstring ShellFullText(ShellCtx* ctx) {
    std::wstring text;
    for (size_t i = 0; i < ctx->segs.size(); ++i) {
        if (i) text += L" | ";
        text += SegmentText(ctx->segs[i]);
    }
    return text;
}

void ShellRefreshPreview(ShellCtx* ctx) {
    if (!ctx->preview || ctx->refreshing) return;
    ctx->refreshing = true;
    std::wstring text = ShellFullText(ctx);
    ::SetWindowTextW(ctx->preview, text.empty() ? std::wstring(L"shell ").c_str()
                                                : (L"shell " + text).c_str());
    ctx->refreshing = false;
}

void ShellFreeSegment(Segment& s) {
    for (auto& p : s.params) {
        if (p.chk) ::DestroyWindow(p.chk);
        if (p.edit) ::DestroyWindow(p.edit);
    }
    s.params.clear();
    if (s.combo) ::DestroyWindow(s.combo);
    s.combo = nullptr;
}

void ShellLayout(ShellCtx* ctx) {
    RECT rc;
    ::GetClientRect(ctx->hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;

    // 底部：预览 + 按钮行
    const int btnH = 26;
    const int btnY = h - 8 - btnH;
    const int previewY = btnY - 8 - 24;

    int y = 6 - ctx->scrollY;
    for (auto& s : ctx->segs) {
        if (s.combo) ::MoveWindow(s.combo, 8, y, (std::min)(300, w - 24), 240, TRUE);
        int py = y + 36;
        for (auto& p : s.params) {
            if (p.chk) ::MoveWindow(p.chk, 8, py, (std::min)(300, w - 24), 22, TRUE);
            if (p.edit) ::MoveWindow(p.edit, 316, py - 1, w - 316 - 24, 24, TRUE);
            py += 28;
        }
        y += SegmentHeight(s);
    }
    ctx->contentH = 0;
    for (const auto& s : ctx->segs) ctx->contentH += SegmentHeight(s);

    if (ctx->preview) ::MoveWindow(ctx->preview, 8, previewY, w - 16, 24, TRUE);
    ::MoveWindow(::GetDlgItem(ctx->hwnd, IDC_PIPE_ADD), 8, btnY, 110, btnH, TRUE);
    ::MoveWindow(::GetDlgItem(ctx->hwnd, IDC_PIPE_DEL), 124, btnY, 110, btnH, TRUE);
    ::MoveWindow(::GetDlgItem(ctx->hwnd, IDOK), w - 194, btnY, 90, btnH, TRUE);
    ::MoveWindow(::GetDlgItem(ctx->hwnd, IDCANCEL), w - 98, btnY, 90, btnH, TRUE);

    // 滚动条范围
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = ctx->contentH + 8;
    si.nPage = static_cast<UINT>((std::max)(1, previewY - 8));
    si.nPos = ctx->scrollY;
    ::SetScrollInfo(ctx->hwnd, SB_VERT, &si, TRUE);

    ShellRefreshPreview(ctx);
}

void ShellBuildSegment(ShellCtx* ctx, Segment& s) {
    ShellFreeSegment(s);
    s.combo = ::CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        0, 0, 0, 0, ctx->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_PARAM)), nullptr, nullptr);
    ApplyFont(s.combo, g_app.font);
    const auto& cmds = ShellCommands();
    for (const auto& c : cmds)
        ::SendMessageW(s.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.name));
    if (s.cmdIndex < 0 || s.cmdIndex >= static_cast<int>(cmds.size())) s.cmdIndex = 0;
    ::SendMessageW(s.combo, CB_SETCURSEL, static_cast<WPARAM>(s.cmdIndex), 0);

    const ShellCmdDef& def = cmds[static_cast<size_t>(s.cmdIndex)];
    for (size_t i = 0; i < def.params.size(); ++i) {
        const ShellParamDef& pd = def.params[i];
        SegParamUi pu;
        pu.index = static_cast<int>(i);
        pu.chk = ::CreateWindowExW(0, L"BUTTON", pd.label, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   0, 0, 0, 0, ctx->hwnd, nullptr, nullptr, nullptr);
        ApplyFont(pu.chk, g_app.font);
        if (pd.hasValue) {
            pu.edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        0, 0, 0, 0, ctx->hwnd, nullptr, nullptr, nullptr);
            ApplyFont(pu.edit, g_app.fontMono);
            ::SendMessageW(pu.edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"可选：参数值"));
            ::EnableWindow(pu.edit, FALSE);
        }
        s.params.push_back(pu);
    }
}

void ShellAddSegment(ShellCtx* ctx) {
    if (ctx->segs.size() >= 8) return;   // 再长就没法在一屏里编辑了
    Segment s;
    s.cmdIndex = 0;
    ctx->segs.push_back(s);
    ShellBuildSegment(ctx, ctx->segs.back());
    ShellLayout(ctx);
}

void ShellDelSegment(ShellCtx* ctx) {
    if (ctx->segs.size() <= 1) return;
    ShellFreeSegment(ctx->segs.back());
    ctx->segs.pop_back();
    ShellLayout(ctx);
}
LRESULT CALLBACK ShellProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<ShellCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = static_cast<ShellCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;

            ctx->preview = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PREVIEW), nullptr, nullptr);
            ApplyFont(ctx->preview, g_app.fontMono);

            HWND add = ::CreateWindowExW(0, L"BUTTON", L"增加管道段",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_PIPE_ADD), nullptr, nullptr);
            HWND del = ::CreateWindowExW(0, L"BUTTON", L"删除最后一段",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_PIPE_DEL), nullptr, nullptr);
            HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            for (HWND c : {add, del, ok, cancel}) ApplyFont(c, g_app.font);

            ::SetWindowTextW(hwnd, L"编辑 shell 命令（三段式：命令 → 参数 → 管道后继续）");
            ShellAddSegment(ctx);

            // 已经有值就按 " | " 拆成若干段：只还原命令名，参数值留给用户
            if (ctx->out && !ctx->out->empty()) {
                std::wstring rest = *ctx->out;
                size_t pos = 0;
                int idx = 0;
                for (;;) {
                    size_t sep = rest.find(L" | ", pos);
                    std::wstring piece = rest.substr(pos, sep == std::wstring::npos
                                                              ? std::wstring::npos : sep - pos);
                    const auto& cmds = ShellCommands();
                    for (size_t i = 0; i < cmds.size(); ++i) {
                        if (piece == cmds[i].name || adb::StartsWith(piece, std::wstring(cmds[i].name) + L" ")) {
                            if (idx >= static_cast<int>(ctx->segs.size())) ShellAddSegment(ctx);
                            ctx->segs[static_cast<size_t>(idx)].cmdIndex = static_cast<int>(i);
                            ShellBuildSegment(ctx, ctx->segs[static_cast<size_t>(idx)]);
                            break;
                        }
                    }
                    ++idx;
                    if (sep == std::wstring::npos) break;
                    pos = sep + 3;
                }
                ShellLayout(ctx);
            }
            return 0;
        }
        case WM_SIZE:
            ShellLayout(ctx);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            ::GetScrollInfo(hwnd, SB_VERT, &si);
            int old = ctx->scrollY;
            switch (LOWORD(wp)) {
                case SB_LINEUP:   ctx->scrollY -= 24; break;
                case SB_LINEDOWN: ctx->scrollY += 24; break;
                case SB_PAGEUP:   ctx->scrollY -= 120; break;
                case SB_PAGEDOWN: ctx->scrollY += 120; break;
                case SB_THUMBTRACK: ctx->scrollY = si.nTrackPos; break;
                default: break;
            }
            if (ctx->scrollY < 0) ctx->scrollY = 0;
            if (ctx->scrollY > ctx->contentH) ctx->scrollY = ctx->contentH;
            if (ctx->scrollY != old) ShellLayout(ctx);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            ctx->scrollY -= delta / 120 * 48;
            if (ctx->scrollY < 0) ctx->scrollY = 0;
            if (ctx->scrollY > ctx->contentH) ctx->scrollY = ctx->contentH;
            ShellLayout(ctx);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_PIPE_ADD && code == BN_CLICKED) { ShellAddSegment(ctx); return 0; }
            if (id == IDC_PIPE_DEL && code == BN_CLICKED) { ShellDelSegment(ctx); return 0; }
            if (id == IDC_COMBO_PARAM && code == CBN_SELCHANGE) {
                // 找到是哪个段的下拉变了，重建它的参数行
                HWND ctl = reinterpret_cast<HWND>(lp);
                for (size_t i = 0; i < ctx->segs.size(); ++i) {
                    if (ctx->segs[i].combo != ctl) continue;
                    int sel = static_cast<int>(::SendMessageW(ctl, CB_GETCURSEL, 0, 0));
                    ctx->segs[i].cmdIndex = sel < 0 ? 0 : sel;
                    ShellBuildSegment(ctx, ctx->segs[i]);
                    ShellLayout(ctx);
                    break;
                }
                return 0;
            }
            if (code == BN_CLICKED) {
                // 参数复选框：勾上才启用它的值输入框
                for (auto& s : ctx->segs) {
                    for (auto& p : s.params) {
                        if (p.chk != reinterpret_cast<HWND>(lp)) continue;
                        bool on = ::SendMessageW(p.chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (p.edit) ::EnableWindow(p.edit, on ? TRUE : FALSE);
                        ShellRefreshPreview(ctx);
                        return 0;
                    }
                }
            }
            if (code == EN_CHANGE) {
                ShellRefreshPreview(ctx);
                return 0;
            }
            if (id == IDOK) {
                if (ctx->out) *ctx->out = ShellFullText(ctx);
                ctx->accepted = true;
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            for (auto& s : ctx->segs) ShellFreeSegment(s);
            ctx->segs.clear();
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
// ------------------------------------------------------------- 截图取点
struct ShotCtx : PickerBase {
    HBITMAP bmp = nullptr;
    int  imgW = 0, imgH = 0;
    HWND canvas = nullptr;
    HWND hint = nullptr;
    std::wstring* out = nullptr;
    bool   swipe = false;

    bool   dragging = false;
    POINT  p0{}, p1{};
    bool   hasP0 = false, hasP1 = false;
};

// 屏幕坐标 → 图片像素坐标
POINT CanvasToImage(const ShotCtx* ctx, POINT pt, const RECT& area) {
    if (ctx->imgW <= 0 || ctx->imgH <= 0) return pt;
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    if (aw <= 0 || ah <= 0) return pt;
    int x = pt.x - area.left, y = pt.y - area.top;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > aw) x = aw;
    if (y > ah) y = ah;
    POINT out;
    out.x = static_cast<LONG>(static_cast<long long>(x) * ctx->imgW / aw);
    out.y = static_cast<LONG>(static_cast<long long>(y) * ctx->imgH / ah);
    return out;
}

// 图片坐标 → 画布坐标
POINT ImageToCanvas(const ShotCtx* ctx, POINT pt, const RECT& area) {
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    POINT out;
    out.x = area.left + (ctx->imgW > 0 ? static_cast<LONG>(static_cast<long long>(pt.x) * aw / ctx->imgW) : 0);
    out.y = area.top + (ctx->imgH > 0 ? static_cast<LONG>(static_cast<long long>(pt.y) * ah / ctx->imgH) : 0);
    return out;
}

RECT FitArea(HWND canvas) {
    RECT rc;
    ::GetClientRect(canvas, &rc);
    return rc;
}

void ShotPaint(ShotCtx* ctx, HWND canvas, HDC dc) {
    RECT rc;
    ::GetClientRect(canvas, &rc);
    ::FillRect(dc, &rc, theme::Brush(RGB(0x20, 0x24, 0x2A)));

    if (!ctx->bmp) {
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(0xE5, 0xE7, 0xEB));
        const wchar_t* msg = L"没能拿到设备截图\r\n"
                             L"· 确认手机已连接并授权 USB 调试\r\n"
                             L"· 点左下角「重新截图」再试一次";
        // DT_VCENTER 只认单行，这里先量高度再自己居中
        RECT measure = rc;
        ::DrawTextW(dc, msg, -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        const int textH = measure.bottom - measure.top;
        RECT t = rc;
        t.top = rc.top + ((rc.bottom - rc.top) - textH) / 2;
        ::DrawTextW(dc, msg, -1, &t, DT_CENTER | DT_WORDBREAK);
        return;
    }
    if (ctx->bmp && ctx->imgW > 0 && ctx->imgH > 0) {
        HDC mem = ::CreateCompatibleDC(dc);
        HGDIOBJ old = ::SelectObject(mem, ctx->bmp);
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        ::StretchBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, ctx->imgW, ctx->imgH, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteDC(mem);
    }

    // 已选的点 / 拖动轨迹
    if (ctx->hasP0) {
        POINT a = ImageToCanvas(ctx, ctx->p0, rc);
        POINT b = ctx->hasP1 ? ImageToCanvas(ctx, ctx->p1, rc) : a;
        HPEN pen = ::CreatePen(PS_SOLID, 2, theme::kAccent);
        HGDIOBJ oldPen = ::SelectObject(dc, pen);
        HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
        if (ctx->swipe) ::MoveToEx(dc, a.x, a.y, nullptr), ::LineTo(dc, b.x, b.y);
        const int r = 6;
        ::Ellipse(dc, a.x - r, a.y - r, a.x + r, a.y + r);
        if (ctx->swipe && ctx->hasP1) ::Ellipse(dc, b.x - r, b.y - r, b.x + r, b.y + r);
        ::SelectObject(dc, oldPen);
        ::SelectObject(dc, oldBrush);
        ::DeleteObject(pen);
    }
}

LRESULT CALLBACK ShotCanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<ShotCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!ctx) return 0;
            SetCapture(hwnd);
            RECT area = FitArea(hwnd);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ctx->p0 = CanvasToImage(ctx, pt, area);
            ctx->p1 = ctx->p0;
            ctx->hasP0 = true;
            ctx->hasP1 = false;
            ctx->dragging = false;
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!ctx || !ctx->hasP0 || ::GetCapture() != hwnd) return 0;
            RECT area = FitArea(hwnd);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ctx->p1 = CanvasToImage(ctx, pt, area);
            if (ctx->p1.x != ctx->p0.x || ctx->p1.y != ctx->p0.y) {
                ctx->hasP1 = true;
                ctx->dragging = true;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!ctx) return 0;
            ::ReleaseCapture();
            RECT area = FitArea(hwnd);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ctx->p1 = CanvasToImage(ctx, pt, area);
            if (ctx->p1.x != ctx->p0.x || ctx->p1.y != ctx->p0.y) {
                ctx->hasP1 = true;
                ctx->dragging = true;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            if (ctx) ShotPaint(ctx, hwnd, dc);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// 把设备截图拉回本地（失败返回空）
std::wstring GrabDeviceShot() {
    auto& session = adb::DeviceSession::Instance();
    if (session.CurrentSerial().empty()) return {};
    std::wstring dir = adb::Settings::Instance().DownloadDir();
    adb::EnsureDirectory(dir);
    std::wstring local = adb::JoinPath(dir, L"adbrt-pick-" + adb::NowStamp() + L".png");
    std::wstring remote = L"/sdcard/adbrt-pick.png";

    adb::RunOptions o1;
    o1.timeoutMs = 20000;
    auto shot = adb::RunProcess(session.AdbExe(),
                                session.WithSerial({L"shell", L"screencap", L"-p", remote}), o1);
    if (!shot.started || shot.exitCode != 0) return {};

    adb::RunOptions o2;
    o2.timeoutMs = 30000;
    auto pull = adb::RunProcess(session.AdbExe(), session.WithSerial({L"pull", remote, local}), o2);
    adb::RunOptions o3;
    o3.timeoutMs = 8000;
    adb::RunProcess(session.AdbExe(), session.WithSerial({L"shell", L"rm", L"-f", remote}), o3);
    return pull.Ok() ? local : std::wstring();
}

LRESULT CALLBACK ShotProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<ShotCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = static_cast<ShotCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;

            // 画布必须是可接收鼠标消息的真窗口：
            // STATIC 默认吞掉 WM_LBUTTONDOWN/WM_MOUSEMOVE，截图取点就点不动了。
            HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            WNDCLASSEXW cwc{};
            cwc.cbSize = sizeof(cwc);
            cwc.lpfnWndProc = ShotCanvasProc;
            cwc.hInstance = inst;
            cwc.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
            cwc.hbrBackground = nullptr;      // 自己画，避免闪烁
            cwc.lpszClassName = L"AdbRTShotCanvas";
            ::RegisterClassExW(&cwc);

            ctx->canvas = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"AdbRTShotCanvas", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_SHOT_CANVAS), inst, nullptr);
            ::SetWindowLongPtrW(ctx->canvas, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            ctx->hint = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          0, 0, 0, 0, hwnd,
                                          reinterpret_cast<HMENU>(IDC_STATIC_HINT), nullptr, nullptr);
            HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            HWND again = ::CreateWindowExW(0, L"BUTTON", L"重新截图",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_REFRESH2), nullptr, nullptr);
            for (HWND c : {ctx->hint, ok, cancel, again}) ApplyFont(c, g_app.font);

            ::SetWindowTextW(hwnd, ctx->swipe ? L"截图取点 - 按住鼠标拖出滑动轨迹"
                                              : L"截图取点 - 在截图里点一下");

            // 载图（GDI+；失败就照样开窗，让用户点「重新截图」）
            std::wstring file = GrabDeviceShot();
            if (!file.empty()) {
                Gdiplus::GdiplusStartupInput gi;
                ULONG_PTR tok = 0;
                if (Gdiplus::GdiplusStartup(&tok, &gi, nullptr) == Gdiplus::Ok) {
                    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromFile(file.c_str());
                    if (bm && bm->GetLastStatus() == Gdiplus::Ok) {
                        Gdiplus::Bitmap* conv = bm->Clone(0, 0, bm->GetWidth(), bm->GetHeight(),
                                                          PixelFormat24bppRGB);
                        if (conv) {
                            HBITMAP hb = nullptr;
                            if (conv->GetHBITMAP(Gdiplus::Color(0, 0, 0), &hb) == Gdiplus::Ok) {
                                ctx->bmp = hb;
                                ctx->imgW = static_cast<int>(bm->GetWidth());
                                ctx->imgH = static_cast<int>(bm->GetHeight());
                            }
                            delete conv;
                        }
                    }
                    delete bm;
                    Gdiplus::GdiplusShutdown(tok);
                }
            }
            ::SetWindowTextW(ctx->hint, ctx->swipe
                ? L"在下面的截图里按住左键拖一条线：起点 = 按下处，终点 = 松手处"
                : L"在下面的截图里用左键点一下即可（坐标来自手机真实分辨率）");
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(ctx->hint, 8, 6, w - 16, 20, TRUE);
            ::MoveWindow(ctx->canvas, 8, 30, w - 16, h - 30 - 40, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDC_BTN_REFRESH2), 8, h - 34, 96, 26, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDOK), w - 194, h - 34, 90, 26, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDCANCEL), w - 98, h - 34, 90, 26, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDOK) {
                if (!ctx->hasP0) {
                    ::MessageBoxW(hwnd, ctx->swipe ? L"请先在截图里拖一条滑动轨迹。"
                                                   : L"请先在截图里点一下。",
                                  L"AdbRT", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                if (ctx->out) {
                    if (ctx->swipe) {
                        POINT b = ctx->hasP1 ? ctx->p1 : ctx->p0;
                        *ctx->out = adb::Format(L"%ld %ld %ld %ld",
                                                ctx->p0.x, ctx->p0.y, b.x, b.y);
                    } else {
                        *ctx->out = adb::Format(L"%ld %ld", ctx->p0.x, ctx->p0.y);
                    }
                }
                ctx->accepted = true;
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            if (id == IDC_BTN_REFRESH2) {
                // 重新抓一张：直接退出去让上层再点一次（避免这里再写一份线程）
                ::DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (ctx->bmp) ::DeleteObject(ctx->bmp);
            ctx->bmp = nullptr;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// 注册窗口类。重复注册会失败，所以先探测一次：
// 原来每次调用都 CreateSolidBrush，类已存在时那个画刷就白漏了。
void RegisterOnce(const wchar_t* cls, WNDPROC proc, HINSTANCE inst, COLORREF bg) {
    WNDCLASSEXW probe{};
    probe.cbSize = sizeof(probe);
    if (::GetClassInfoExW(inst, cls, &probe)) return;   // 已经注册过了

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = theme::Brush(bg);   // 缓存画刷，进程内复用
    wc.lpszClassName = cls;
    ::RegisterClassExW(&wc);
}


namespace {

// ------------------------------------------------------------- 局域网扫描
constexpr UINT WM_LAN_SCAN_DONE = WM_APP + 91;
constexpr UINT WM_LAN_RESOLVED  = WM_APP + 92;

// 后台线程只认这个句柄组：它比窗口活得久，窗口没了也不会用悬垂指针。
// （线程和窗口谁先结束不确定，共享状态用 shared_ptr，取消用原子标志。）
struct LanShared {
    std::atomic<bool> cancel{false};
};

struct LanCtx : PickerBase {
    std::vector<adb::LocalNet> nets;
    std::vector<std::wstring>  rows;       // 显示行："IP   说明"
    std::vector<std::wstring>  values;     // 行 → IP
    std::wstring* out = nullptr;
    HWND combo = nullptr;
    HWND list = nullptr;
    HWND scanBtn = nullptr;
    HWND status = nullptr;
    HWND manual = nullptr;
    std::shared_ptr<LanShared> shared;
    std::string  lastNote;                 // 上一次扫描的耗时 / 线程数，界面上显示
};

struct LanScanArg {
    HWND hwnd = nullptr;
    std::wstring prefix;
    std::shared_ptr<LanShared> shared;
};

struct ResolveArg {
    HWND hwnd = nullptr;
    std::wstring host;
    std::shared_ptr<LanShared> shared;
};

void LanFill(LanCtx* ctx) {
    ::SendMessageW(ctx->list, LB_RESETCONTENT, 0, 0);
    ctx->values.clear();
    for (const auto& line : ctx->rows) {
        ::SendMessageW(ctx->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        // 行首故意留一格空白（列表看着不贴边），取 IP 时先跳过它
        size_t b = line.find_first_not_of(L' ');
        if (b == std::wstring::npos) { ctx->values.push_back(line); continue; }
        size_t e = line.find(L' ', b);
        ctx->values.push_back(e == std::wstring::npos ? line.substr(b) : line.substr(b, e - b));
    }
}

// 后台线程：扫完把结果一次性发回 UI（窗口没了就自己释放内存）
DWORD WINAPI LanScanProc(LPVOID param) {
    auto* arg = static_cast<LanScanArg*>(param);
    auto* result = new adb::ScanResult();
    *result = adb::ScanLan(arg->prefix, adb::kLanMaxLatencyMs, 5555,
                           arg->shared ? &arg->shared->cancel : nullptr);
    if (!::PostMessageW(arg->hwnd, WM_LAN_SCAN_DONE, 0, reinterpret_cast<LPARAM>(result)))
        delete result;
    delete arg;
    return 0;
}

// 后台线程：给选中的主机找 adb 端口（可能要扫端口段）
DWORD WINAPI ResolveProc(LPVOID param) {
    auto* arg = static_cast<ResolveArg*>(param);
    std::wstring endpoint;
    if (arg->host.find(L':') != std::wstring::npos) {
        endpoint = arg->host;                     // 已经带端口
    } else {
        std::wstring ep = adb::FindAdbEndpoint(arg->host, adb::kLanMaxLatencyMs,
                                               arg->shared ? &arg->shared->cancel : nullptr);
        endpoint = ep.empty() ? (arg->host + L":5555") : ep;
    }
    auto* heap = new std::wstring(endpoint);
    if (!::PostMessageW(arg->hwnd, WM_LAN_RESOLVED, 0, reinterpret_cast<LPARAM>(heap)))
        delete heap;
    delete arg;
    return 0;
}

// 起一个后台线程；句柄立刻关掉，线程结束时系统自己回收
bool StartThread(LPTHREAD_START_ROUTINE proc, void* arg) {
    DWORD tid = 0;
    HANDLE th = ::CreateThread(nullptr, 0, proc, arg, 0, &tid);
    if (!th) return false;
    ::CloseHandle(th);
    return true;
}

LRESULT CALLBACK LanProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<LanCtx*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            ctx = static_cast<LanCtx*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            ctx->hwnd = hwnd;
            ctx->shared = std::make_shared<LanShared>();

            ctx->combo = ::CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_COMBO_PARAM), nullptr, nullptr);
            ctx->scanBtn = ::CreateWindowExW(0, L"BUTTON", L"开始扫描",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_REFRESH2), nullptr, nullptr);
            ctx->manual = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_VALUE), nullptr, nullptr);
            ::SendMessageW(ctx->manual, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"也可以直接手填 IP:端口"));
            ctx->list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_MAIN), nullptr, nullptr);
            ctx->status = ::CreateWindowExW(0, L"STATIC", L"选一个网段，然后点「开始扫描」。",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATUSBAR), nullptr, nullptr);
            HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            for (HWND c : {ctx->scanBtn, ok, cancel, ctx->status, ctx->manual})
                ApplyFont(c, g_app.font);
            ApplyFont(ctx->list, g_app.fontMono);

            ctx->nets = adb::ListLocalNets();
            for (const auto& n : ctx->nets) {
                std::wstring text = n.adapter + L"  " + n.localIp + L"/24";
                if (!n.gateway.empty()) text += L"  网关 " + n.gateway;
                ::SendMessageW(ctx->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
            if (ctx->nets.empty()) {
                ::SendMessageW(ctx->combo, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(L"（没找到可用网卡）"));
            }
            ::SendMessageW(ctx->combo, CB_SETCURSEL, 0, 0);
            ::SetWindowTextW(hwnd, L"扫描局域网设备");
            return 0;
        }
        case WM_LAN_RESOLVED: {
            auto* endpoint = reinterpret_cast<std::wstring*>(lp);
            ::EnableWindow(ctx->scanBtn, TRUE);
            if (endpoint) {
                if (ctx->out) *ctx->out = *endpoint;
                delete endpoint;
                ctx->accepted = true;
                ::DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_LAN_SCAN_DONE: {
            auto* result = reinterpret_cast<adb::ScanResult*>(lp);
            ::EnableWindow(ctx->scanBtn, TRUE);
            ::SetWindowTextW(ctx->scanBtn, L"开始扫描");
            if (result) {
                ctx->rows.clear();
                for (const auto& h : result->hosts) {
                    std::wstring rtt = h.rttMs >= 0
                        ? adb::Format(L"%d ms", h.rttMs) : std::wstring(L"—");
                    std::wstring tail = h.adb ? L"adb 端口已开"
                                              : L"选中后自动找 adb 端口";
                    // 行首留一格空白，LanFill 取 IP 时按空格切第一段
                    ctx->rows.push_back(L" " + h.ip + L"    " + rtt + L"    " + tail);
                }
                SetTextOf(ctx->status,
                          adb::Format(L"在线 %zu 台，其中 %zu 台开着 adb 端口；"
                                      L"扫描 %d 个地址用了 %llu ms（%d 个线程）。",
                                      result->alive.size(), result->adbHosts.size(),
                                      result->probed,
                                      static_cast<unsigned long long>(result->elapsedMs),
                                      result->threads));
                delete result;
            }
            LanFill(ctx);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            ::MoveWindow(ctx->combo, 8, 8, w - 8 - 128, 240, TRUE);
            ::MoveWindow(ctx->scanBtn, w - 124, 8, 116, 24, TRUE);
            ::MoveWindow(ctx->manual, 8, 38, w - 16, 24, TRUE);
            int listH = h - 70 - 34 - 30;
            ::MoveWindow(ctx->list, 8, 70, w - 16, listH > 40 ? listH : 40, TRUE);
            ::MoveWindow(ctx->status, 8, h - 62, w - 16, 20, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDOK), w - 194, h - 30, 90, 24, TRUE);
            ::MoveWindow(::GetDlgItem(hwnd, IDCANCEL), w - 98, h - 30, 90, 24, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_BTN_REFRESH2 && code == BN_CLICKED) {
                if (ctx->nets.empty()) {
                    ::MessageBoxW(hwnd, L"没有找到可用的局域网网卡。", L"AdbRT",
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                int sel = static_cast<int>(::SendMessageW(ctx->combo, CB_GETCURSEL, 0, 0));
                if (sel < 0 || sel >= static_cast<int>(ctx->nets.size())) sel = 0;

                ctx->rows.clear();
                LanFill(ctx);
                ::EnableWindow(ctx->scanBtn, FALSE);
                ::SetWindowTextW(ctx->scanBtn, L"扫描中…"
                );
                SetTextOf(ctx->status,
                          adb::Format(L"正在并发扫描 254 个地址（时延上限 %d ms，"
                                      L"超过的算不在同一局域网）…", adb::kLanMaxLatencyMs));

                auto* arg = new LanScanArg();
                arg->hwnd = hwnd;
                arg->prefix = ctx->nets[static_cast<size_t>(sel)].prefix;
                arg->shared = ctx->shared;
                if (!StartThread(LanScanProc, arg)) {
                    delete arg;
                    ::EnableWindow(ctx->scanBtn, TRUE);
                    ::SetWindowTextW(ctx->scanBtn, L"开始扫描");
                    SetTextOf(ctx->status, L"启动扫描线程失败。");
                }
                return 0;
            }
            if (id == IDC_LIST_MAIN && code == LBN_DBLCLK) {
                int sel = static_cast<int>(::SendMessageW(ctx->list, LB_GETCURSEL, 0, 0));
                if (sel < 0 || sel >= static_cast<int>(ctx->values.size())) return 0;
                auto* arg = new ResolveArg();
                arg->hwnd = hwnd;
                arg->host = ctx->values[static_cast<size_t>(sel)];
                arg->shared = ctx->shared;
                ::EnableWindow(ctx->scanBtn, FALSE);
                SetTextOf(ctx->status, L"正在查找这台设备的 adb 端口…");
                if (!StartThread(ResolveProc, arg)) {
                    delete arg;
                    ::EnableWindow(ctx->scanBtn, TRUE);
                }
                return 0;
            }
            if (id == IDOK) {
                std::wstring manual = adb::Trim(GetTextOf(ctx->manual));
                std::wstring picked;
                int sel = static_cast<int>(::SendMessageW(ctx->list, LB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(ctx->values.size()))
                    picked = ctx->values[static_cast<size_t>(sel)];
                if (!manual.empty()) picked = manual;
                if (picked.empty()) {
                    ::MessageBoxW(hwnd, L"请扫描后选一台设备，或者手动填 IP:端口。", L"AdbRT",
                                  MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                auto* arg = new ResolveArg();
                arg->hwnd = hwnd;
                arg->host = picked;
                arg->shared = ctx->shared;
                ::EnableWindow(ctx->scanBtn, FALSE);
                SetTextOf(ctx->status, L"正在查找这台设备的 adb 端口…");
                if (!StartThread(ResolveProc, arg)) {
                    delete arg;
                    ::EnableWindow(ctx->scanBtn, TRUE);
                }
                return 0;
            }
            if (id == IDCANCEL) { ::DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            // 通知后台线程收工；它们持有 shared_ptr，不会碰到已销毁的 ctx
            if (ctx && ctx->shared) ctx->shared->cancel = true;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- 对外接口
void ShowFileChecklist(HWND parent, const std::wstring& folder, std::vector<std::wstring>* out) {
    if (!out) return;
    HINSTANCE inst = InstOf(parent);
    RegisterOnce(L"AdbRTFileCheck", FilesProc, inst, theme::kWindowBg);

    FilesCtx ctx;
    ctx.folder = folder;
    ctx.out = out;
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, L"AdbRTFileCheck",
                                  L"勾选要推送的文件",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 620,
                                  RootOf(parent), nullptr, inst, &ctx);
    if (!hwnd) return;
    RunModal(parent, hwnd);
    ::DestroyWindow(hwnd);
}

bool ShowPropPicker(HWND parent, std::wstring* text) {
    HINSTANCE inst = InstOf(parent);
    RegisterOnce(L"AdbRTPropPicker", PropsProc, inst, theme::kWindowBg);

    PropsCtx ctx;
    ctx.current = text ? *text : std::wstring();
    ctx.out = text;
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, L"AdbRTPropPicker",
                                  L"选择系统属性",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 760, 660,
                                  RootOf(parent), nullptr, inst, &ctx);
    if (!hwnd) return false;
    RunModal(parent, hwnd);
    ::DestroyWindow(hwnd);
    return ctx.accepted;
}

bool ShowShellPipelineEditor(HWND parent, std::wstring* text) {
    HINSTANCE inst = InstOf(parent);
    RegisterOnce(L"AdbRTShellEdit", ShellProc, inst, theme::kWindowBg);

    ShellCtx ctx;
    ctx.out = text;
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, L"AdbRTShellEdit",
                                  L"编辑 shell 命令",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 780, 620,
                                  RootOf(parent), nullptr, inst, &ctx);
    if (!hwnd) return false;
    RunModal(parent, hwnd);
    ::DestroyWindow(hwnd);
    return ctx.accepted;
}

void ShowShotPicker(HWND parent, const adb::CommandSpec* spec, std::wstring* out) {
    if (!spec || !out) return;
    HINSTANCE inst = InstOf(parent);
    RegisterOnce(L"AdbRTShotPicker", ShotProc, inst, theme::kWindowBg);

    ShotCtx ctx;
    ctx.out = out;
    ctx.swipe = (std::string(spec->key) == "input.swipe");

    HCURSOR old = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, L"AdbRTShotPicker",
                                  L"截图取点",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 520, 900,
                                  RootOf(parent), nullptr, inst, &ctx);
    ::SetCursor(old);
    if (!hwnd) return;
    RunModal(parent, hwnd);
    ::DestroyWindow(hwnd);
}

bool ShowLanScanner(HWND parent, std::wstring* out) {
    if (!out) return false;
    HINSTANCE inst = InstOf(parent);
    RegisterOnce(L"AdbRTLanScan", LanProc, inst, theme::kWindowBg);

    LanCtx ctx;
    ctx.out = out;
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW, L"AdbRTLanScan",
                                  L"扫描局域网设备",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 620, 620,
                                  RootOf(parent), nullptr, inst, &ctx);
    if (!hwnd) return false;
    RunModal(parent, hwnd);
    ::DestroyWindow(hwnd);
    return ctx.accepted;
}

}  // namespace adbgui
