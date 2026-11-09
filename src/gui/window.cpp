#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "ui_util.h"

namespace adbgui {

namespace {

constexpr int kLeftWidth = 260;
constexpr int kPadding = 8;

void AddGroupNode(HWND tree, const wchar_t* text, adb::GroupId group) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    ins.item.stateMask = TVIS_EXPANDED;
    ins.item.state = TVIS_EXPANDED;
    ins.item.pszText = const_cast<wchar_t*>(text);
    ins.item.lParam = -1 - static_cast<LPARAM>(group);
    ::SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ins));
}

HTREEITEM FindGroupItem(HWND tree, adb::GroupId group) {
    HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    while (item) {
        TVITEMW ti{};
        ti.mask = TVIF_PARAM;
        ti.hItem = item;
        ::SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&ti));
        if (ti.lParam == -1 - static_cast<LPARAM>(group)) return item;
        item = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(item)));
    }
    return nullptr;
}

void PopulateTree(HWND tree, bool showFastboot) {
    ::SendMessageW(tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

    for (uint8_t gi = 0; gi < static_cast<uint8_t>(adb::GroupId::Count); ++gi) {
        auto group = static_cast<adb::GroupId>(gi);
        if (group == adb::GroupId::Fastboot && !showFastboot) continue;

        AddGroupNode(tree, adb::GroupName(group), group);
        HTREEITEM hGroup = FindGroupItem(tree, group);

        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const adb::CommandSpec* spec = &adb::CommandTable()[i];
            if (spec->group != group) continue;

            std::wstring title = adb::CommandTitle(*spec);
            TVINSERTSTRUCTW ins{};
            ins.hParent = hGroup;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM;
            ins.item.pszText = const_cast<wchar_t*>(title.c_str());
            ins.item.lParam = static_cast<LPARAM>(spec->id);
            ::SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ins));
        }
    }
}

void RebuildDevicesCombo(const std::vector<adb::DeviceEntry>& devices, const std::wstring& current) {
    HWND combo = g_app.hDeviceCombo;
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int select = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::wstring text = devices[i].Describe();
        int idx = static_cast<int>(::SendMessageW(combo, CB_ADDSTRING, 0,
                                                  reinterpret_cast<LPARAM>(text.c_str())));
        if (devices[i].serial == current) select = idx;
    }
    if (select >= 0) ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    else if (!devices.empty()) ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

}  // namespace

void RefreshDevices(HWND hwnd, bool keepSelection) {
    (void)hwnd;
    auto& session = adb::DeviceSession::Instance();
    std::wstring current = keepSelection ? session.CurrentSerial() : L"";

    std::vector<adb::DeviceEntry> devices = session.ListAdb();
    g_app.devices = devices;
    if (current.empty() && devices.size() == 1) current = devices.front().serial;
    g_app.serial = current;
    if (!current.empty()) session.SetCurrentSerial(current);

    RebuildDevicesCombo(devices, current);
    if (devices.empty()) SetStatus(L"未检测到设备：请连接手机并开启 USB 调试");
    else SetStatus(L"就绪");
}

void SelectCommand(const adb::CommandSpec* spec) {
    g_app.current = spec;
    BuildParamPanel(g_app.hParamHost, spec);
    UpdatePreview();
}

bool CollectInput(adb::BuildInput* in, std::wstring* error) {
    if (!g_app.current) {
        if (error) *error = L"请先在左侧选择一条命令。";
        return false;
    }
    in->spec = g_app.current;
    in->serial = g_app.serial;
    in->toolExe = g_app.current->needsFastboot ? adb::DeviceSession::Instance().FastbootExe()
                                               : adb::DeviceSession::Instance().AdbExe();
    ReadParamPanel(in, error);

    if (!g_app.current->needsDevice) return true;

    // 设备要求
    if (g_app.current->needsFastboot) {
        if (adb::DeviceSession::Instance().FastbootExe().empty()) {
            if (error) *error = L"找不到 fastboot.exe，请在「设置」里指定路径。";
            return false;
        }
        return true;
    }
    if (adb::DeviceSession::Instance().AdbExe().empty()) {
        if (error) *error = L"找不到 adb.exe，请在「设置」里指定路径。";
        return false;
    }
    if (in->serial.empty()) {
        if (error) *error = L"没有可用设备：请连接手机后点「刷新」。";
        return false;
    }
    return true;
}

void UpdatePreview() {
    adb::BuildInput in;
    std::wstring error;
    if (g_app.current && CollectInput(&in, &error)) {
        adb::BuiltCommand built;
        adb::BuildError berr;
        if (adb::BuildCommand(in, &built, &berr)) {
            std::wstring text = adb::BuildCommandLineForLog(in.toolExe, {});
            text = adb::FileNameOf(in.toolExe) + L" " + built.display;
            SetTextOf(g_app.hPreview, text);
            return;
        }
        SetTextOf(g_app.hPreview, L"（" + berr.message + L"）");
        return;
    }
    SetTextOf(g_app.hPreview, error);
}

static void LayoutChildren(HWND hwnd) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top - 24;

    ::MoveWindow(g_app.hDeviceCombo, 8, 8, 260, 220, TRUE);
    ::MoveWindow(g_app.btnFastboot, 300, 8, 140, 24, TRUE);
    ::MoveWindow(g_app.hRefreshing, 448, 8, 80, 24, TRUE);

    ::MoveWindow(g_app.hTree, 8, 40, kLeftWidth, h - 48, TRUE);

    int rightX = kLeftWidth + 16;
    int rightW = w - rightX - 8;
    ::MoveWindow(g_app.hParamHost, rightX, 40, rightW, h / 2, TRUE);
    ::MoveWindow(g_app.hPreview, rightX, 40 + h / 2 + 8, rightW, 46, TRUE);
    ::MoveWindow(g_app.hRun, rightX, 40 + h / 2 + 60, 80, 24, TRUE);
    ::MoveWindow(g_app.hCancel, rightX + 88, 40 + h / 2 + 60, 80, 24, TRUE);
    ::MoveWindow(g_app.hOutput, rightX, 40 + h / 2 + 90, rightW, h - (h / 2 + 98), TRUE);

    ::SendMessageW(g_app.hStatus, WM_SIZE, 0, 0);
}

static void OnCommand(HWND hwnd, int id, int code, HWND ctl) {
    (void)code;
    (void)ctl;
    switch (id) {
        case IDC_BTN_REFRESH:
            RefreshDevices(hwnd, true);
            break;
        case IDC_BTN_FASTBOOT: {
            g_app.showFastboot = !g_app.showFastboot;
            PopulateTree(g_app.hTree, g_app.showFastboot);
            break;
        }
        case IDC_BTN_RUN:
            StartRun();
            break;
        case IDC_BTN_CANCEL:
            CancelRun();
            break;
        case IDC_BTN_TOOLS:
            ShowSettings(hwnd);
            break;
        case IDC_BTN_COPY: {
            std::wstring text = GetTextOf(g_app.hPreview);
            if (!text.empty()) {
                ::OpenClipboard(hwnd);
                ::EmptyClipboard();
                size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (mem) {
                    void* dst = ::GlobalLock(mem);
                    if (dst) { memcpy(dst, text.c_str(), bytes); ::GlobalUnlock(mem); }
                    ::SetClipboardData(CF_UNICODETEXT, mem);
                }
                ::CloseClipboard();
            }
            break;
        }
        default:
            break;
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_app.hwnd = hwnd;
            g_app.font = CreateUiFont(9, false);
            g_app.fontMono = CreateUiFont(9, true);

            INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
            ::InitCommonControlsEx(&icc);

            g_app.hDeviceCombo = ::CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_DEVICE_COMBO),
                reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), nullptr);
            g_app.btnFastboot = ::CreateWindowExW(0, L"BUTTON", L"Fastboot 专家模式",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_FASTBOOT), nullptr, nullptr);
            g_app.hRefreshing = ::CreateWindowExW(0, L"BUTTON", L"刷新设备",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_REFRESH), nullptr, nullptr);

            g_app.hTree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TREE_COMMANDS), nullptr, nullptr);

            g_app.hParamHost = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_PARAM_PANEL), nullptr, nullptr);

            g_app.hPreview = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PREVIEW), nullptr, nullptr);

            g_app.hOutput = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_OUTPUT), nullptr, nullptr);

            g_app.hRun = ::CreateWindowExW(0, L"BUTTON", L"执行",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_RUN), nullptr, nullptr);
            g_app.hCancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_CANCEL), nullptr, nullptr);
            ::EnableWindow(g_app.hCancel, FALSE);

            HWND hCopy = ::CreateWindowExW(0, L"BUTTON", L"复制命令行",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_COPY), nullptr, nullptr);
            (void)hCopy;

            g_app.hStatus = ::CreateWindowExW(0, L"STATIC", L"就绪",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATUSBAR), nullptr, nullptr);

            for (HWND c : {g_app.hDeviceCombo, g_app.btnFastboot, g_app.hRefreshing,
                           g_app.hTree, g_app.hPreview, g_app.hOutput, g_app.hRun,
                           g_app.hCancel, hCopy, g_app.hStatus})
                ApplyFont(c, g_app.font);
            ApplyFont(g_app.hPreview, g_app.fontMono);
            ApplyFont(g_app.hOutput, g_app.fontMono);

            PopulateTree(g_app.hTree, g_app.showFastboot);
            RefreshDevices(hwnd, false);
            SetStatus(L"就绪");
            return 0;
        }
        case WM_SIZE:
            LayoutChildren(hwnd);
            return 0;
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<LPNMHDR>(lp);
            if (nm->idFrom == IDC_TREE_COMMANDS && nm->code == TVN_SELCHANGEDW) {
                auto* tv = reinterpret_cast<LPNMTREEVIEWW>(lp);
                LPARAM param = tv->itemNew.lParam;
                if (param >= 0) {
                    const adb::CommandSpec* spec = adb::FindCommand(static_cast<adb::CommandId>(param));
                    if (spec) SelectCommand(spec);
                } else {
                    SelectCommand(nullptr);
                }
            }
            return 0;
        }
        case WM_APP_TASK_LINE: {
            auto* line = reinterpret_cast<std::wstring*>(lp);
            if (line) {
                AppendOutput(*line + L"\r\n");
                delete line;
            }
            return 0;
        }
        case WM_APP_TASK_DONE: {
            auto* outcome = reinterpret_cast<TaskOutcome*>(lp);
            if (outcome) {
                g_app.busy = false;
                ::EnableWindow(g_app.hRun, TRUE);
                ::EnableWindow(g_app.hCancel, FALSE);
                SetStatus(outcome->message.c_str());
                delete outcome;
            }
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateMainWindow(HINSTANCE hinst, const AppContext& ctx) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hinst;
    wc.hIcon = ::LoadIconW(hinst, MAKEINTRESOURCEW(IDI_ADBRT));
    wc.hIconSm = ::LoadIconW(hinst, MAKEINTRESOURCEW(IDI_ADBRT));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AdbRTMainWindow";
    wc.hIconSm = wc.hIcon;
    if (!::RegisterClassExW(&wc)) return false;

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"AdbRT", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                                  nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return false;

    if (!ctx.initialPath.empty()) {
        adb::Settings::Instance().SetString("ui.initialPath", adb::WideToUtf8(ctx.initialPath));
    }

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    return true;
}

int RunApp(HINSTANCE hinst, const AppContext& ctx, int showCmd) {
    (void)showCmd;
    if (!CreateMainWindow(hinst, ctx)) return 1;

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == g_app.hwnd && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            // Enter 触发执行（焦点不在多行编辑框时）
            StartRun();
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace adbgui