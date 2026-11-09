// ---------------------------------------------------------------------------
// 主窗口（重做版）：分组卡片式布局
//
//   ┌ 顶栏：图标 + AdbRT + 设备下拉 + 刷新 + Fastboot 开关 ····· 设置 关于 ┐
//   ├ 左栏：搜索框 + 命令树（分组 = 大类，条目带危险标记）                  ┤
//   └ 右栏：命令标题 + 危险徽标                                            │
//           参数区（滚动）                                                 │
//           命令行预览（深色代码块）+ 复制                                  │
//           执行 / 取消                                                  │
//           运行结果（可清空 / 保存）                                      │
//   └ 状态栏                                                              ┘
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../core/command_builder.h"
#include "../core/device.h"
#include "../core/parsers.h"
#include "../core/settings.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "ui_util.h"

namespace adbgui {

namespace {

using namespace theme;

constexpr int kLeftWidth   = 236;
constexpr int kHeaderH     = 56;
constexpr int kStatusH     = 30;
constexpr int kSearchH     = 26;
constexpr int kTitleH      = 32;
constexpr int kBtnH        = 32;
constexpr int kPreviewH    = 58;

HBRUSH g_bgBrush = nullptr;
HBRUSH g_paneBrush = nullptr;

// --------------------------------------------------------------- 命令树
void AddGroupNode(HWND tree, const wchar_t* text, adb::GroupId group) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
    ins.item.stateMask = TVIS_EXPANDED | TVIS_BOLD;
    ins.item.state = TVIS_EXPANDED | TVIS_BOLD;
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

// 过滤：搜索框的内容同时匹配 key、中文标题、分组名
bool Matches(const adb::CommandSpec& spec, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring n = adb::ToLowerAscii(needle);
    std::wstring hay = adb::ToLowerAscii(adb::Utf8ToWide(spec.key));
    hay += L" ";
    hay += adb::ToLowerAscii(adb::CommandTitle(spec));
    hay += L" ";
    hay += adb::ToLowerAscii(adb::GroupName(spec.group));
    return hay.find(n) != std::wstring::npos;
}

void PopulateTree(HWND tree, bool showFastboot, const std::wstring& filter) {
    ::SendMessageW(tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

    for (uint8_t gi = 0; gi < static_cast<uint8_t>(adb::GroupId::Count); ++gi) {
        auto group = static_cast<adb::GroupId>(gi);
        if (group == adb::GroupId::Fastboot && !showFastboot) continue;

        // 先数一下这个分组下有多少条命中，为 0 就不建分组
        int hits = 0;
        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const adb::CommandSpec* s = &adb::CommandTable()[i];
            if (s->group == group && Matches(*s, filter)) ++hits;
        }
        if (hits == 0) continue;

        AddGroupNode(tree, adb::GroupName(group), group);
        HTREEITEM hGroup = FindGroupItem(tree, group);

        for (size_t i = 0; i < adb::CommandTableSize(); ++i) {
            const adb::CommandSpec* spec = &adb::CommandTable()[i];
            if (spec->group != group) continue;
            if (!Matches(*spec, filter)) continue;

            std::wstring title = adb::CommandTitle(*spec);
            // 危险级别用前缀符号，扫一眼就能分辨
            switch (spec->danger) {
                case adb::Danger::Destructive: title = L"!  " + title; break;
                case adb::Danger::Breaker:     title = L"!!  " + title; break;
                case adb::Danger::Careful:     title = L"-  " + title; break;
                default: break;
            }
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
    ::EnableWindow(combo, !devices.empty());
}

void MakeLabelFont(HWND h) { ApplyFont(h, FontBold()); }

void LayoutChildren(HWND hwnd) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // ---- 顶栏：图标 + 标题占左侧 200px，之后才是设备区 ----
    ::MoveWindow(g_app.hSettings, w - Dpi(156), Dpi(14), Dpi(68), Dpi(28), TRUE);
    ::MoveWindow(g_app.hAbout,    w - Dpi(82),  Dpi(14), Dpi(68), Dpi(28), TRUE);

    const int devX = Dpi(206);
    const int devW = (w - Dpi(176) - devX) > Dpi(420) ? Dpi(320) : (w - Dpi(176) - devX - Dpi(240));
    ::MoveWindow(g_app.hDeviceCombo, devX, Dpi(14), devW > Dpi(160) ? devW : Dpi(160), Dpi(28), TRUE);
    ::MoveWindow(g_app.hRefreshing,  devX + (devW > Dpi(160) ? devW : Dpi(160)) + Dpi(8),
                 Dpi(14), Dpi(78), Dpi(28), TRUE);
    ::MoveWindow(g_app.btnFastboot,  devX + (devW > Dpi(160) ? devW : Dpi(160)) + Dpi(92),
                 Dpi(14), Dpi(120), Dpi(28), TRUE);

    // ---- 左栏 ----
    const int leftW = Dpi(kLeftWidth);
    ::MoveWindow(g_app.hSearch, Dpi(kGap), Dpi(kHeaderH + kGap),
                 leftW - Dpi(kGap) * 2, Dpi(kSearchH), TRUE);
    ::MoveWindow(g_app.hTree, Dpi(kGap), Dpi(kHeaderH + kGap + kSearchH + 6),
                 leftW - Dpi(kGap) * 2, h - Dpi(kHeaderH + kGap * 2 + kSearchH + 6) - Dpi(kStatusH + kGap),
                 TRUE);

    // ---- 右栏 ----
    const int rx = leftW + Dpi(kGap) * 2;
    const int rw = w - rx - Dpi(kGap);
    const int top = Dpi(kHeaderH + kGap);

    ::MoveWindow(g_app.hCmdTitle, rx, top, rw - Dpi(90), Dpi(kTitleH), TRUE);
    ::MoveWindow(g_app.hDanger,   rx + rw - Dpi(88), top + Dpi(6), Dpi(88), Dpi(20), TRUE);

    const int paramsTop = top + Dpi(kTitleH + kGapSmall);
    const int paramsH = Dpi(150);
    ::MoveWindow(g_app.hParamsLabel, rx, paramsTop, rw, Dpi(18), TRUE);
    ::MoveWindow(g_app.hParamHost, rx, paramsTop + Dpi(20), rw, paramsH, TRUE);

    const int previewTop = paramsTop + Dpi(20) + paramsH + Dpi(kGap);
    ::MoveWindow(g_app.hPreviewLabel, rx, previewTop, rw - Dpi(96), Dpi(18), TRUE);
    ::MoveWindow(g_app.hCopy, rx + rw - Dpi(90), previewTop - Dpi(2), Dpi(90), Dpi(22), TRUE);
    ::MoveWindow(g_app.hPreview, rx, previewTop + Dpi(20), rw, Dpi(kPreviewH), TRUE);

    const int btnTop = previewTop + Dpi(20) + Dpi(kPreviewH) + Dpi(kGap);
    ::MoveWindow(g_app.hRun,    rx, btnTop, Dpi(96), Dpi(kBtnH), TRUE);
    ::MoveWindow(g_app.hCancel, rx + Dpi(104), btnTop, Dpi(96), Dpi(kBtnH), TRUE);

    const int outLabelTop = btnTop + Dpi(kBtnH + kGap);
    ::MoveWindow(g_app.hOutputLabel, rx, outLabelTop, rw - Dpi(150), Dpi(18), TRUE);
    ::MoveWindow(g_app.hSaveOut,  rx + rw - Dpi(146), outLabelTop - Dpi(2), Dpi(70), Dpi(22), TRUE);
    ::MoveWindow(g_app.hClearOut, rx + rw - Dpi(72),  outLabelTop - Dpi(2), Dpi(70), Dpi(22), TRUE);

    const int outTop = outLabelTop + Dpi(20);
    const int outH = h - outTop - Dpi(kStatusH + kGap);
    ::MoveWindow(g_app.hOutput, rx, outTop, rw, outH > Dpi(60) ? outH : Dpi(60), TRUE);

    // ---- 状态栏 ----
    ::MoveWindow(g_app.hStatus, Dpi(kGap), h - Dpi(kStatusH), w - Dpi(kGap) * 2,
                 Dpi(kStatusH), TRUE);
}

void PaintHeader(HWND hwnd, HDC dc) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    const int headerH = Dpi(kHeaderH);

    // 顶栏底色
    RECT header{0, 0, rc.right, headerH};
    FillRectColor(dc, header, kHeaderBg);
    // 底部分隔线
    RECT line{0, headerH - 1, rc.right, headerH};
    FillRectColor(dc, line, kBorder);

    // 应用图标 + 标题
    HICON icon = static_cast<HICON>(::LoadImageW(
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
        MAKEINTRESOURCEW(IDI_ADBRT), IMAGE_ICON, Dpi(24), Dpi(24), LR_DEFAULTCOLOR));
    if (icon) {
        ::DrawIconEx(dc, Dpi(kGap) + 2, Dpi(14), icon, Dpi(24), Dpi(24), 0, nullptr, DI_NORMAL);
        ::DestroyIcon(icon);
    }

    RECT title{Dpi(kGap) + Dpi(34), Dpi(9), Dpi(kGap) + Dpi(190), Dpi(29)};
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kText);
    HFONT old = static_cast<HFONT>(::SelectObject(dc, FontTitle()));
    ::DrawTextW(dc, L"AdbRT", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);

    RECT sub{Dpi(kGap) + Dpi(34), Dpi(28), Dpi(kGap) + Dpi(190), Dpi(46)};
    ::SetTextColor(dc, kTextDim);
    old = static_cast<HFONT>(::SelectObject(dc, FontNormal()));
    ::DrawTextW(dc, L"Android 平台工具", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);
}

void PaintDangerBadge(HWND hwnd, HDC dc) {
    if (!g_app.hDanger) return;
    RECT rc;
    ::GetClientRect(g_app.hDanger, &rc);
    ::GetWindowRect(g_app.hDanger, &rc);
    ::MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
    const adb::CommandSpec* spec = g_app.current;
    std::wstring text = L"安全";
    COLORREF fg = kOk, bg = kOkSoft;
    if (spec) {
        switch (spec->danger) {
            case adb::Danger::Breaker:     text = L"危险";   fg = kDanger; bg = kDangerSoft; break;
            case adb::Danger::Destructive: text = L"破坏性"; fg = kDanger; bg = kDangerSoft; break;
            case adb::Danger::Careful:     text = L"谨慎";   fg = kWarn;   bg = kWarnSoft;   break;
            default:                       text = L"安全";   fg = kOk;     bg = kOkSoft;     break;
        }
    }
    // 徽标只由父窗口绘制：子静态控件隐藏，避免两套颜色打架
    DrawPill(dc, rc, text.c_str(), fg, bg);
}

void PaintBody(HWND hwnd, HDC dc) {
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    const int headerH = Dpi(kHeaderH);
    const int statusH = Dpi(kStatusH);
    const int leftW = Dpi(kLeftWidth);

    // 左右分栏的分隔线
    RECT leftPane{Dpi(kGap) - 4, headerH + Dpi(kGap), Dpi(kGap) + leftW - Dpi(kGap) + 4,
                  rc.bottom - statusH - Dpi(kGap)};
    (void)leftPane;

    // 右栏卡片底色（比窗口底色略亮，形成层次）
    RECT right{leftW + Dpi(kGap), headerH + Dpi(kGapSmall),
               rc.right - Dpi(kGap), rc.bottom - statusH - Dpi(kGapSmall)};
    FillRectColor(dc, right, kPaneBg);
    DrawBorder(dc, right, kBorder);

    PaintDangerBadge(hwnd, dc);

    // 状态栏
    RECT status{0, rc.bottom - statusH, rc.right, rc.bottom};
    FillRectColor(dc, status, kHeaderBg);
    RECT statusLine{0, rc.bottom - statusH, rc.right, rc.bottom - statusH + 1};
    FillRectColor(dc, statusLine, kBorder);
}

// 徽标在父窗口的 WM_PAINT 里画（见 PaintDangerBadge(HWND, HDC)），这里只需要重绘父窗口
void InvalidateDangerBadge() {
    if (g_app.hwnd) ::InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

}  // namespace

// ------------------------------------------------------------- 对外接口
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

    if (devices.empty()) {
        SetStatus(L"未检测到设备 — 请连接手机并开启「USB 调试」");
    } else {
        std::wstring s = adb::Format(L"%zu 台设备 · 当前：", devices.size());
        s += current.empty() ? std::wstring(L"未选择") : current;
        SetStatus(s.c_str());
    }
    UpdatePreview();
}

void SelectTreeItemById(adb::CommandId id);

void SelectCommand(const adb::CommandSpec* spec) {
    g_app.current = spec;
    BuildParamPanel(g_app.hParamHost, spec);
    if (g_app.hCmdTitle) {
        ::SetWindowTextW(g_app.hCmdTitle, spec ? adb::CommandTitle(*spec) : L"请选择左侧的命令");
    }
    if (g_app.hParamsLabel) {
        ::SetWindowTextW(g_app.hParamsLabel,
                         spec && (spec->flagCount > 0 || spec->param != adb::ParamKind::None)
                             ? L"参数" : L"参数（这条命令没有可选项）");
    }
    InvalidateDangerBadge();
    UpdatePreview();
}

void SelectTreeItemById(adb::CommandId id) {
    HTREEITEM item = reinterpret_cast<HTREEITEM>(
        ::SendMessageW(g_app.hTree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
    while (item) {
        HTREEITEM child = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(g_app.hTree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(item)));
        while (child) {
            TVITEMW ti{};
            ti.mask = TVIF_PARAM;
            ti.hItem = child;
            ::SendMessageW(g_app.hTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&ti));
            if (ti.lParam == static_cast<LPARAM>(id)) {
                ::SendMessageW(g_app.hTree, TVM_SELECTITEM, TVGN_CARET,
                               reinterpret_cast<LPARAM>(child));
                return;
            }
            child = reinterpret_cast<HTREEITEM>(
                ::SendMessageW(g_app.hTree, TVM_GETNEXTITEM, TVGN_NEXT,
                               reinterpret_cast<LPARAM>(child)));
        }
        item = reinterpret_cast<HTREEITEM>(
            ::SendMessageW(g_app.hTree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(item)));
    }
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
    if (!g_app.hPreview) return;
    adb::BuildInput in;
    std::wstring error;
    if (g_app.current && CollectInput(&in, &error)) {
        adb::BuiltCommand built;
        adb::BuildError berr;
        if (adb::BuildCommand(in, &built, &berr)) {
            std::wstring text = adb::FileNameOf(in.toolExe) + L" " + built.display;
            SetTextOf(g_app.hPreview, text);
            return;
        }
        SetTextOf(g_app.hPreview, L"（" + berr.message + L"）");
        return;
    }
    SetTextOf(g_app.hPreview, error);
}

void CopyPreviewToClipboard(HWND hwnd) {
    std::wstring text = GetTextOf(g_app.hPreview);
    if (text.empty()) return;
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
    SetStatus(L"命令行已复制到剪贴板");
}

void SaveOutputToFile(HWND hwnd) {
    std::wstring text = GetTextOf(g_app.hOutput);
    if (text.empty()) { SetStatus(L"没有可保存的输出"); return; }
    wchar_t buf[MAX_PATH * 2] = {0};
    std::wstring dir = adb::Settings::Instance().DownloadDir();
    adb::EnsureDirectory(dir);
    lstrcpynW(buf, adb::JoinPath(dir, L"adbrt-output-" + adb::NowStamp() + L".txt").c_str(),
              MAX_PATH * 2);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"文本文件\0*.txt\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"保存运行结果";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&ofn)) return;
    HANDLE h = ::CreateFileW(buf, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(L"保存失败：文件不可写"); return; }
    std::string utf8 = adb::WideToUtf8(text);
    DWORD written = 0;
    ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(h);
    SetStatus((L"已保存：" + std::wstring(buf)).c_str());
}

// ------------------------------------------------------------- 窗口过程
namespace {

void OnCommand(HWND hwnd, int id, int code, HWND ctl) {
    (void)ctl;
    switch (id) {
        case IDC_BTN_REFRESH:   RefreshDevices(hwnd, true); break;
        case IDC_BTN_FASTBOOT: {
            g_app.showFastboot = !g_app.showFastboot;
            PopulateTree(g_app.hTree, g_app.showFastboot, GetTextOf(g_app.hSearch));
            break;
        }
        case IDC_BTN_RUN:       StartRun(); break;
        case IDC_BTN_CANCEL:    CancelRun(); break;
        case IDC_BTN_SETTINGS:  ShowSettings(hwnd); break;
        case IDC_BTN_ABOUT: {
            const adb::CommandSpec* about = adb::FindCommandByKey("tool.about");
            if (about) RunBuiltinCommand(hwnd, *about, nullptr);
            break;
        }
        case IDC_BTN_COPY:      CopyPreviewToClipboard(hwnd); break;
        case IDC_BTN_CLEAROUT:  ClearOutput(); SetStatus(L"已清空运行结果"); break;
        case IDC_BTN_SAVEOUT:   SaveOutputToFile(hwnd); break;
        case IDC_EDIT_SEARCH:
            if (code == EN_CHANGE)
                PopulateTree(g_app.hTree, g_app.showFastboot, GetTextOf(g_app.hSearch));
            break;
        case IDC_DEVICE_COMBO:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(::SendMessageW(g_app.hDeviceCombo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(g_app.devices.size())) {
                    g_app.serial = g_app.devices[sel].serial;
                    adb::DeviceSession::Instance().SetCurrentSerial(g_app.serial);
                    SetStatus((L"当前设备：" + g_app.serial).c_str());
                    UpdatePreview();
                }
            }
            break;
        default: break;
    }
}

BOOL DrawDangerBadge(HWND hwnd, LPDRAWITEMSTRUCT dis) {
    const adb::CommandSpec* spec = g_app.current;
    std::wstring text = L"安全";
    COLORREF fg = kOk, bg = kOkSoft;
    if (spec) {
        switch (spec->danger) {
            case adb::Danger::Breaker:     text = L"危险";   fg = kDanger; bg = kDangerSoft; break;
            case adb::Danger::Destructive: text = L"破坏性"; fg = kDanger; bg = kDangerSoft; break;
            case adb::Danger::Careful:     text = L"谨慎";   fg = kWarn;   bg = kWarnSoft;   break;
            default:                       text = L"安全";   fg = kOk;     bg = kOkSoft;     break;
        }
    }
    RECT rc = dis->rcItem;
    FillRectColor(dis->hDC, rc, kWindowBg);
    DrawPill(dis->hDC, rc, text.c_str(), fg, bg);
    return TRUE;
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_app.hwnd = hwnd;
            InitForWindow(hwnd);
            g_app.font = FontNormal();
            g_app.fontMono = FontMono();
            g_bgBrush = Brush(kWindowBg);
            g_paneBrush = Brush(kPaneBg);

            INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
            ::InitCommonControlsEx(&icc);

            HINSTANCE inst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

            g_app.hDeviceCombo = ::CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_DEVICE_COMBO), inst, nullptr);
            g_app.hRefreshing = ::CreateWindowExW(0, L"BUTTON", L"刷新设备",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_REFRESH), inst, nullptr);
            g_app.btnFastboot = ::CreateWindowExW(0, L"BUTTON", L"Fastboot 专家",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_FASTBOOT), inst, nullptr);
            g_app.hSettings = ::CreateWindowExW(0, L"BUTTON", L"设置",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_SETTINGS), inst, nullptr);
            g_app.hAbout = ::CreateWindowExW(0, L"BUTTON", L"关于",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_ABOUT), inst, nullptr);

            g_app.hSearch = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_EDIT_SEARCH), inst, nullptr);
            ::SendMessageW(g_app.hSearch, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"搜索命令（支持中文与 key）"));

            g_app.hTree = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                    TVS_FULLROWSELECT | WS_TABSTOP,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TREE_COMMANDS), inst, nullptr);

            g_app.hCmdTitle = ::CreateWindowExW(0, L"STATIC", L"请选择左侧的命令",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATIC_CMDTITLE), inst, nullptr);

            // 只是占位（父窗口按它的矩形绘制徽标），不真正显示
            g_app.hDanger = ::CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATIC_DANGER), inst, nullptr);

            g_app.hParamsLabel = ::CreateWindowExW(0, L"STATIC", L"参数",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATIC_PARAMS), inst, nullptr);

            g_app.hParamHost = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_PARAM_PANEL), inst, nullptr);

            g_app.hPreviewLabel = ::CreateWindowExW(0, L"STATIC", L"将执行的命令",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATIC_PREVIEW), inst, nullptr);
            g_app.hCopy = ::CreateWindowExW(0, L"BUTTON", L"复制",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_COPY), inst, nullptr);

            g_app.hPreview = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_PREVIEW), inst, nullptr);

            g_app.hRun = ::CreateWindowExW(0, L"BUTTON", L"执行",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_RUN), inst, nullptr);
            g_app.hCancel = ::CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_CANCEL), inst, nullptr);
            ::EnableWindow(g_app.hCancel, FALSE);

            g_app.hOutputLabel = ::CreateWindowExW(0, L"STATIC", L"运行结果",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_STATIC_OUTPUT), inst, nullptr);
            g_app.hSaveOut = ::CreateWindowExW(0, L"BUTTON", L"保存",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_SAVEOUT), inst, nullptr);
            g_app.hClearOut = ::CreateWindowExW(0, L"BUTTON", L"清空",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_BTN_CLEAROUT), inst, nullptr);

            g_app.hOutput = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_OUTPUT), inst, nullptr);

            g_app.hStatus = ::CreateWindowExW(0, L"STATIC", L"就绪",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATUSBAR), inst, nullptr);

            for (HWND c : {g_app.hDeviceCombo, g_app.hRefreshing, g_app.btnFastboot,
                           g_app.hSettings, g_app.hAbout, g_app.hSearch, g_app.hTree,
                           g_app.hPreview, g_app.hOutput, g_app.hRun, g_app.hCancel,
                           g_app.hCopy, g_app.hSaveOut, g_app.hClearOut, g_app.hStatus})
                ApplyFont(c, FontNormal());
            for (HWND c : {g_app.hCmdTitle})
                ApplyFont(c, FontTitle());
            for (HWND c : {g_app.hParamsLabel, g_app.hPreviewLabel, g_app.hOutputLabel})
                MakeLabelFont(c);
            ApplyFont(g_app.hPreview, FontMono());
            ApplyFont(g_app.hOutput, FontMono());

            PopulateTree(g_app.hTree, g_app.showFastboot, L"");
            RefreshDevices(hwnd, true);
            SetStatus(L"就绪");
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, g_bgBrush ? g_bgBrush : Brush(kWindowBg));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            PaintBody(hwnd, dc);
            PaintHeader(hwnd, dc);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            ::SetBkMode(dc, TRANSPARENT);
            if (ctl == g_app.hCmdTitle) {
                ::SetTextColor(dc, kText);
                return reinterpret_cast<LRESULT>(g_bgBrush);
            }
            if (ctl == g_app.hStatus) {
                ::SetTextColor(dc, kTextDim);
                return reinterpret_cast<LRESULT>(g_bgBrush);
            }
            if (ctl == g_app.hDanger) {
                const adb::CommandSpec* spec = g_app.current;
                COLORREF fg = kOk;
                if (spec) {
                    switch (spec->danger) {
                        case adb::Danger::Breaker:
                        case adb::Danger::Destructive: fg = kDanger; break;
                        case adb::Danger::Careful:     fg = kWarn;   break;
                        default:                       fg = kOk;     break;
                    }
                }
                ::SetTextColor(dc, fg);
                ::SelectObject(dc, FontBold());
                return reinterpret_cast<LRESULT>(g_bgBrush);
            }
            if (ctl == g_app.hParamsLabel || ctl == g_app.hPreviewLabel ||
                ctl == g_app.hOutputLabel) {
                ::SetTextColor(dc, kTextDim);
                ::SelectObject(dc, FontBold());
                return reinterpret_cast<LRESULT>(g_bgBrush);
            }
            // 只读 EDIT 走的是 STATIC 分支：命令行预览要深色代码块的观感
            if (ctl == g_app.hPreview) {
                ::SetBkMode(dc, OPAQUE);
                ::SetBkColor(dc, kCodeBg);
                ::SetTextColor(dc, kCodeText);
                return reinterpret_cast<LRESULT>(Brush(kCodeBg));
            }
            if (ctl == g_app.hOutput) {
                ::SetBkMode(dc, OPAQUE);
                ::SetBkColor(dc, kPaneBg);
                ::SetTextColor(dc, kText);
                return reinterpret_cast<LRESULT>(Brush(kPaneBg));
            }
            ::SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
        // 命令行预览：深色代码块；运行结果：白底
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl == g_app.hPreview) {
                ::SetBkMode(dc, OPAQUE);
                ::SetBkColor(dc, kCodeBg);
                ::SetTextColor(dc, kCodeText);
                return reinterpret_cast<LRESULT>(Brush(kCodeBg));
            }
            ::SetBkMode(dc, OPAQUE);
            ::SetBkColor(dc, kPaneBg);
            ::SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(Brush(kPaneBg));
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
            if (dis->CtlID == IDC_STATIC_DANGER) return DrawDangerBadge(hwnd, dis);
            break;
        }
        case WM_SIZE:
            LayoutChildren(hwnd);
            ::InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = Dpi(900);
            mmi->ptMinTrackSize.y = Dpi(620);
            return 0;
        }
        case WM_COMMAND:
            OnCommand(hwnd, LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
            return 0;
        case WM_APP_TASK_START:
            StartRun();
            return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<LPNMHDR>(lp);
            if (nm->idFrom == IDC_TREE_COMMANDS && nm->code == TVN_SELCHANGEDW) {
                auto* tv = reinterpret_cast<LPNMTREEVIEWW>(lp);
                LPARAM param = tv->itemNew.lParam;
                if (param >= 0) {
                    const adb::CommandSpec* spec =
                        adb::FindCommand(static_cast<adb::CommandId>(param));
                    if (spec) SelectCommand(spec);
                } else {
                    SelectCommand(nullptr);
                }
            }
            return 0;
        }
        case WM_APP_TASK_LINE: {
            auto* line = reinterpret_cast<std::wstring*>(lp);
            if (line) { AppendOutput(*line + L"\r\n"); delete line; }
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

}  // namespace

bool CreateMainWindow(HINSTANCE hinst, const AppContext& ctx) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hinst;
    wc.hIcon = ::LoadIconW(hinst, MAKEINTRESOURCEW(IDI_ADBRT));
    wc.hIconSm = ::LoadIconW(hinst, MAKEINTRESOURCEW(IDI_ADBRT));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;      // 自己画，避免闪烁
    wc.lpszClassName = L"AdbRTMainWindow2";
    if (!::RegisterClassExW(&wc)) return false;

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"AdbRT",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
                                  nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return false;

    if (!ctx.initialPath.empty())
        adb::Settings::Instance().SetString("ui.initialPath", adb::WideToUtf8(ctx.initialPath));

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    return true;
}

int RunApp(HINSTANCE hinst, const AppContext& ctx, int showCmd) {
    (void)showCmd;
    if (!CreateMainWindow(hinst, ctx)) return 1;

    if (!ctx.initialSelect.empty()) {
        if (const adb::CommandSpec* spec = adb::FindCommandByKey(ctx.initialSelect)) {
            SelectTreeItemById(spec->id);
            SelectCommand(spec);
        }
    }

    if (!ctx.initialCommand.empty()) {
        const adb::CommandSpec* spec = adb::FindCommandByKey(ctx.initialCommand);
        if (spec) {
            SelectCommand(spec);
            ::PostMessageW(g_app.hwnd, WM_APP_TASK_START, 0, 0);
        }
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == g_app.hwnd && msg.message == WM_KEYDOWN) {
            bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (msg.wParam == VK_RETURN && !ctrl) { StartRun(); continue; }
            if (ctrl && (msg.wParam == 'F')) { ::SetFocus(g_app.hSearch); continue; }
            if (msg.wParam == VK_F5) { RefreshDevices(g_app.hwnd, true); continue; }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace adbgui