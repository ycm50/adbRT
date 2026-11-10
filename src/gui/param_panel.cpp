// ---------------------------------------------------------------------------
// 参数面板：按 ParamKind 决定控件形态
//   · 下拉框：重启目标 / 槽位 / 分区名 / 推送目标 / 拉取目录
//   · 相对路径：设备目录下拉 + 相对路径输入，拼成 /sdcard/xxx
//   · 本机文件：文件复选与"整个文件夹推送"互斥，结果进 BuildInput::paths
//   · 复杂输入（属性复选 / 三段式 shell / 截图取点）打开专用窗口
// ---------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
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

// 面板里的一个控件：设计坐标（未缩放）
struct Item {
    HWND hwnd = nullptr;
    int  x = 0, y = 0, w = 0, h = 0;
};

struct CtrlRef {
    HWND hwnd = nullptr;
    std::string key;
    adb::ParamKind kind = adb::ParamKind::None;
    adb::FlagKind  flagKind = adb::FlagKind::Toggle;
};

// 面板的跨控件状态（远程根目录、本机文件选择等都在这里）
struct PanelState {
    adb::ParamKind kind = adb::ParamKind::None;

    // 远程路径
    std::wstring remoteRoot = L"/sdcard";
    std::wstring remoteRel;

    // 本机文件
    std::wstring folder;
    std::vector<std::wstring> files;
    bool wholeFolder = false;

    // 复杂输入
    std::wstring props;
    std::wstring shell;
    std::wstring coords;
    std::wstring coords2;

    HWND combo = nullptr, edit = nullptr, chkFolder = nullptr;
    HWND btnFiles = nullptr, btnPick = nullptr, btnExtra = nullptr;
    HWND summary = nullptr;
};

std::vector<CtrlRef> g_params;
std::vector<CtrlRef> g_flags;
std::vector<Item>    g_items;
PanelState           g_panel;
WNDPROC              g_oldParamProc = nullptr;
int                  g_panelHeightDesign = 0;

void Track(HWND hwnd, int x, int y, int w, int h) {
    g_items.push_back(Item{hwnd, x, y, w, h});
}

void RelayoutParams(HWND host) {
    for (const auto& it : g_items)
        ::MoveWindow(it.hwnd, theme::Dpi(it.x), theme::Dpi(it.y),
                     theme::Dpi(it.w), theme::Dpi(it.h), TRUE);
    (void)host;
}

HWND MakeLabel(HWND host, const std::wstring& text, int x, int y, int w) {
    HWND label = ::CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, host, nullptr, nullptr, nullptr);
    ApplyFont(label, g_app.font);
    Track(label, x, y, w, 20);   // 18 太紧，中文字的下缘会被切掉
    return label;
}

HWND MakeEdit(HWND host, int x, int y, int w, int id, bool readOnly) {
    HWND e = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                   (readOnly ? ES_READONLY : 0),
                               0, 0, 0, 0, host,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ApplyFont(e, g_app.fontMono);
    Track(e, x, y, w, 24);
    return e;
}

HWND MakeButton(HWND host, const wchar_t* text, int x, int y, int w, int id) {
    HWND b = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 0, 0, host,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ApplyFont(b, g_app.font);
    Track(b, x, y, w, 24);
    return b;
}

HWND MakeCheck(HWND host, const wchar_t* text, int x, int y, int w, int id) {
    HWND c = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, host,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ApplyFont(c, g_app.font);
    Track(c, x, y, w, 22);
    return c;
}

HWND MakeCombo(HWND host, int x, int y, int w, int id) {
    HWND c = ::CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                               0, 0, 0, 0, host,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ApplyFont(c, g_app.font);
    Track(c, x, y, w, 240);
    return c;
}

void FillCombo(HWND combo, const std::vector<std::wstring>& items, const std::wstring& current) {
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        int idx = static_cast<int>(::SendMessageW(combo, CB_ADDSTRING, 0,
                                                  reinterpret_cast<LPARAM>(items[i].c_str())));
        if (items[i] == current) sel = idx;
    }
    if (sel < 0 && !items.empty()) sel = 0;
    ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(sel < 0 ? 0 : sel), 0);
}

std::wstring ComboText(HWND combo) {
    int sel = static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0) return {};
    int len = static_cast<int>(::SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0));
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L' ');
    ::SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(sel),
                   reinterpret_cast<LPARAM>(buf.data()));
    buf.resize(static_cast<size_t>(len));
    return buf;
}

// 远程根目录 + 相对路径 → 设备绝对路径
std::wstring JoinRemote(const std::wstring& root, const std::wstring& rel) {
    std::wstring r = adb::Trim(rel);
    for (auto& c : r)
        if (c == L'\x5c') c = L'/';
    while (!r.empty() && r.front() == L'/') r.erase(r.begin());
    while (!r.empty() && r.back() == L'/') r.pop_back();
    if (r.empty()) return root;
    return root == L"/" ? (L"/" + r) : (root + L"/" + r);
}

std::wstring FileSummary(const PanelState& s) {
    if (s.wholeFolder) return s.folder.empty() ? std::wstring() : (L"[整个文件夹] " + s.folder);
    if (s.files.empty()) return {};
    if (s.files.size() == 1) return s.files.front();
    return adb::Format(L"%zu 个文件：%ls 等", s.files.size(), adb::FileNameOf(s.files.front()).c_str());
}

std::wstring PropSummary(const PanelState& s) {
    if (s.props.empty()) return {};
    std::wstring out;
    for (wchar_t c : s.props) {
        if (c == L';') out += L", ";
        else out.push_back(c);
    }
    return out;
}
// ------------------------------------------------------------ 包 / 属性 / 分区来源

// 分区名：优先问设备，问不到用常见清单兜底
std::vector<std::wstring> PartitionsForCombo() {
    std::vector<std::wstring> out = adb::DeviceSession::Instance().Partitions();
    if (out.empty()) return adb::CommonPartitions();
    for (const auto& p : adb::CommonPartitions())
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

const std::vector<std::wstring>& RebootTargets() {
    static const std::vector<std::wstring> k = {
        L"系统（开机）", L"recovery", L"bootloader", L"fastboot", L"sideload", L"edl",
    };
    return k;
}

// "系统（开机）" = 不带目标，其余原样传给 reboot
std::wstring RebootTargetArg(const std::wstring& picked) {
    if (picked.empty() || picked == L"系统（开机）") return {};
    return picked;
}

// 推送 / 新建目录的根目录
const std::vector<std::wstring>& RemoteRoots() {
    static const std::vector<std::wstring> k = {L"/sdcard", L"/tmp"};
    return k;
}

// 拉取：动态列 /sdcard 下的文件夹
std::vector<std::wstring> DeviceFolders() {
    auto& session = adb::DeviceSession::Instance();
    std::vector<std::wstring> out;
    out.push_back(L"/sdcard");
    // ls -1p 会给目录名补一个 "/"，据此只留文件夹（文件不该出现在"拉取目录"里）
    std::wstring reply = session.Shell(L"ls -1p /sdcard", 12000);
    for (const auto& line : adb::SplitLines(reply)) {
        std::wstring name = adb::Trim(line);
        if (name.empty() || name == L"/sdcard") continue;
        if (name.back() != L'/') continue;
        name.pop_back();
        if (name.empty()) continue;
        out.push_back(L"/sdcard/" + name);
    }
    // 兜底：设备读不到时也给几个常见目录，避免下拉框是空的
    if (out.size() <= 1) {
        for (const wchar_t* d : {L"/sdcard/Download", L"/sdcard/DCIM", L"/sdcard/Pictures",
                                 L"/sdcard/Movies", L"/sdcard/Documents", L"/sdcard/Music"})
            out.push_back(d);
    }
    return out;
}
// ------------------------------------------------------------ 各类控件的生成
// 每个 block 负责一行（或多行）控件，返回下一个可用的 y（设计坐标）
int BuildParamBlock(HWND host, const adb::CommandSpec* spec, bool second, int y) {
    const int left = 8;
    const int width = 540;
    const adb::ParamKind kind = second ? spec->param2 : spec->param;
    const char* key = second ? spec->paramKey2 : spec->paramKey;
    const int editId = second ? IDC_PARAM_HOST_2 : IDC_PARAM_HOST;
    const int btnId  = second ? IDC_BTN_ACTION2   : IDC_BTN_ACTION1;

    MakeLabel(host, adb::ParamLabel(kind), left, y, width);
    y += 20;

    switch (kind) {
        case adb::ParamKind::RebootTarget: {
            HWND combo = MakeCombo(host, left, y, 260, IDC_COMBO_PARAM);
            FillCombo(combo, RebootTargets(), L"系统（开机）");
            g_panel.kind = kind;
            g_panel.combo = combo;
            g_params.push_back({combo, key, kind, adb::FlagKind::Value});
            y += 30;
            break;
        }
        case adb::ParamKind::SlotName: {
            HWND combo = MakeCombo(host, left, y, 200, IDC_COMBO_PARAM);
            FillCombo(combo, {L"a", L"b"}, L"a");
            g_panel.kind = kind;
            g_panel.combo = combo;
            g_params.push_back({combo, key, kind, adb::FlagKind::Value});
            y += 30;
            break;
        }
        case adb::ParamKind::Partition: {
            HWND combo = MakeCombo(host, left, y, 320, IDC_COMBO_PARAM);
            FillCombo(combo, PartitionsForCombo(), L"boot");
            g_panel.kind = kind;
            g_panel.combo = combo;
            g_params.push_back({combo, key, kind, adb::FlagKind::Value});
            y += 30;
            break;
        }
        case adb::ParamKind::RemotePick:
        case adb::ParamKind::RemoteDyn: {
            // 根目录下拉 + 相对路径输入
            std::vector<std::wstring> roots = (kind == adb::ParamKind::RemoteDyn)
                ? DeviceFolders() : RemoteRoots();
            HWND combo = MakeCombo(host, left, y, 220, IDC_COMBO_PARAM);
            FillCombo(combo, roots, g_panel.remoteRoot);
            g_panel.kind = kind;
            g_panel.combo = combo;

            HWND rel = MakeEdit(host, left + 226, y - 1, width - 226, editId, false);
            g_panel.edit = rel;
            ::SendMessageW(rel, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(kind == adb::ParamKind::RemoteDyn
                               ? L"相对文件名（可留空 = 整个目录）"
                               : L"相对路径（留空 = 根目录）"));
            g_params.push_back({rel, key, kind, adb::FlagKind::Value});
            MakeLabel(host, L"路径 = 上面选的根目录 + 这里填的相对路径", left, y + 26, width);
            y += 48;
            break;
        }
        case adb::ParamKind::LocalFiles: {
            // 本机文件：文件复选 与 整个文件夹 互斥
            HWND summary = MakeEdit(host, left, y, width, editId, true);
            ::SendMessageW(summary, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"还没选文件 / 文件夹"));
            g_panel.summary = summary;
            g_params.push_back({summary, key, kind, adb::FlagKind::Value});
            y += 28;

            g_panel.btnFiles = MakeButton(host, L"选择文件…",   left,       y, 88, IDC_BTN_ACTION3);
            MakeButton(host, L"选择文件夹…",                    left + 92,  y, 96, IDC_BTN_ACTION4);
            MakeButton(host, L"勾选文件…",                      left + 192, y, 88, IDC_BTN_ACTION5);
            g_panel.chkFolder = MakeCheck(host, L"整个文件夹推送（与文件复选互斥）",
                                          left + 288, y + 1, 252, IDC_CHK_WHOLEFOLDER);
            y += 32;
            break;
        }
        case adb::ParamKind::PropKeys: {
            HWND summary = MakeEdit(host, left, y, width - 96, editId, true);
            ::SendMessageW(summary, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"还没选择属性"));
            g_panel.summary = summary;
            g_panel.btnExtra = MakeButton(host, L"选择属性…", left + width - 92, y, 92, IDC_BTN_ACTION1);
            g_params.push_back({summary, key, kind, adb::FlagKind::Value});
            MakeLabel(host, L"勾选多个属性会依次读取（每条一个 getprop）", left, y + 26, width);
            y += 48;
            break;
        }
        case adb::ParamKind::ShellPipeline: {
            HWND edit = MakeEdit(host, left, y, width - 96, editId, true);
            ::SendMessageW(edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"还没编辑命令"));
            g_panel.summary = edit;
            g_panel.btnExtra = MakeButton(host, L"编辑命令…", left + width - 92, y, 92, IDC_BTN_ACTION1);
            g_params.push_back({edit, key, kind, adb::FlagKind::Value});
            y += 32;
            break;
        }
        case adb::ParamKind::Coords: {
            HWND edit = MakeEdit(host, left, y, width - 96, editId, false);
            ::SendMessageW(edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"点「截图取点」在手机截图上用鼠标点/拖"));
            // 截图取点回填要靠 g_panel.summary，漏掉它坐标就"选完不显示"
            g_panel.summary = edit;
            g_panel.btnExtra = MakeButton(host, L"截图取点…", left + width - 92, y, 92, IDC_BTN_ACTION1);
            g_params.push_back({edit, key, kind, adb::FlagKind::Value});
            y += 32;
            break;
        }
        case adb::ParamKind::Permission: {
            HWND combo = MakeCombo(host, left, y, width, editId);
            FillCombo(combo, {L"android.permission.CAMERA", L"android.permission.RECORD_AUDIO",
                              L"android.permission.ACCESS_FINE_LOCATION",
                              L"android.permission.READ_EXTERNAL_STORAGE",
                              L"android.permission.WRITE_EXTERNAL_STORAGE",
                              L"android.permission.POST_NOTIFICATIONS",
                              L"android.permission.READ_CONTACTS",
                              L"android.permission.READ_SMS"}, L"");
            g_params.push_back({combo, key, kind, adb::FlagKind::Value});
            y += 30;
            break;
        }
        case adb::ParamKind::LanHost: {
            // 手填 IP:端口，旁边给「扫描局域网」按钮
            HWND edit = MakeEdit(host, left, y, width - 116, editId, false);
            ::SendMessageW(edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"192.168.1.23:5555"));
            g_panel.summary = edit;
            g_params.push_back({edit, key, kind, adb::FlagKind::Value});
            MakeButton(host, L"扫描局域网…", left + width - 112, y, 112, btnId);
            y += 32;
            break;
        }
        case adb::ParamKind::Duration: {
            // 毫秒数：可留空（留空时 adb 用系统默认时长）
            HWND edit = MakeEdit(host, left, y, 160, editId, false);
            ::SendMessageW(edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(L"如 300，留空=默认"));
            g_params.push_back({edit, key, kind, adb::FlagKind::Value});
            y += 32;
            break;
        }
        default: {
            bool hasBrowse = (kind != adb::ParamKind::Text &&
                              kind != adb::ParamKind::ShellCommand &&
                              kind != adb::ParamKind::PropKey &&
                              kind != adb::ParamKind::UserId);
            int editW = width;
            if (hasBrowse) editW = width - (kind == adb::ParamKind::PackageName ? 96 : 76);
            HWND edit = MakeEdit(host, left, y, editW, editId, false);
            g_params.push_back({edit, key, kind, adb::FlagKind::Value});
            if (kind == adb::ParamKind::PackageName)
                MakeButton(host, L"选择应用…", left + width - 92, y, 92, btnId);
            else if (hasBrowse)
                MakeButton(host, L"浏览…", left + width - 72, y, 72, btnId);
            y += 32;
            break;
        }
    }
    return y;
}
// ------------------------------------------------------------ flags
int BuildFlagRows(HWND host, const adb::CommandSpec* spec, int y) {
    const int left = 8;
    const int width = 540;
    for (uint8_t i = 0; i < spec->flagCount; ++i) {
        const adb::FlagSpec& f = spec->flags[i];
        std::wstring label = adb::FlagLabel(f);
        if (f.kind == adb::FlagKind::Value) {
            MakeLabel(host, label, left, y, width);
            y += 20;
            HWND e = MakeEdit(host, left, y, width, 0, false);
            g_flags.push_back({e, f.key, adb::ParamKind::None, f.kind});
            y += 30;
        } else {
            HWND chk = MakeCheck(host, label.c_str(), left, y, width, 0);
            ::SendMessageW(chk, BM_SETCHECK, f.defaultOn ? BST_CHECKED : BST_UNCHECKED, 0);
            g_flags.push_back({chk, f.key, adb::ParamKind::None, f.kind});
            y += 26;
        }
    }
    return y;
}

// ------------------------------------------------------------ 浏览 / 选择
void BrowseForParam(HWND host, const adb::CommandSpec* spec, bool second) {
    if (!spec) return;
    const adb::ParamKind kind = second ? spec->param2 : spec->param;

    auto setEdit = [&](const std::wstring& v) {
        if (g_panel.summary) SetTextOf(g_panel.summary, v);
        UpdatePreview();
    };

    if (kind == adb::ParamKind::PackageName) {
        HWND edit = g_params.empty() ? nullptr : g_params.front().hwnd;
        ShowPackagePicker(host, !spec->pkgHideSystem,
                          [edit](const std::vector<std::wstring>& pkgs) {
            if (pkgs.empty() || !edit) return;
            std::wstring joined;
            for (size_t i = 0; i < pkgs.size(); ++i) {
                if (i) joined += L";";
                joined += pkgs[i];
            }
            ::SetWindowTextW(edit, joined.c_str());
            UpdatePreview();
        });
        return;
    }
    if (kind == adb::ParamKind::LocalFiles) {
        // 选择文件（可多选）——与"整个文件夹"互斥
        std::wstring picked = AskFilesMulti(host, L"选择要推送的文件",
                                            L"所有文件\0*.*\0");
        if (picked.empty()) return;
        g_panel.files.clear();
        std::wstring cur;
        for (wchar_t c : picked) {
            if (c == L';') { if (!cur.empty()) g_panel.files.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) g_panel.files.push_back(cur);
        if (!g_panel.files.empty()) {
            g_panel.wholeFolder = false;
            if (g_panel.chkFolder) ::SendMessageW(g_panel.chkFolder, BM_SETCHECK, BST_UNCHECKED, 0);
            g_panel.folder.clear();
        }
        setEdit(FileSummary(g_panel));
        return;
    }
    if (kind == adb::ParamKind::PropKeys) {
        std::wstring sel = g_panel.props;
        if (ShowPropPicker(host, &sel)) {
            g_panel.props = sel;
            setEdit(PropSummary(g_panel));
        }
        return;
    }
    if (kind == adb::ParamKind::ShellPipeline) {
        std::wstring cmd = g_panel.shell;
        if (ShowShellPipelineEditor(host, &cmd)) {
            g_panel.shell = cmd;
            setEdit(cmd);
        }
        return;
    }
    if (kind == adb::ParamKind::Coords) {
        std::wstring v = g_panel.coords;
        ShowShotPicker(host, spec, &v);
        if (!v.empty()) {
            g_panel.coords = v;
            setEdit(v);
        }
        return;
    }
    if (kind == adb::ParamKind::LanHost) {
        std::wstring picked;
        if (ShowLanScanner(host, &picked)) {
            if (g_panel.summary) SetTextOf(g_panel.summary, picked);
            UpdatePreview();
        }
        return;
    }
    if (kind == adb::ParamKind::ImageFile) {
        std::wstring f = AskFile(host, L"选择镜像文件", L"镜像文件\0*.img;*.bin\0所有文件\0*.*\0");
        if (!f.empty()) setEdit(f);
        return;
    }
    if (kind == adb::ParamKind::LocalPaths) {
        std::wstring d = AskFolder(host, L"选择目录");
        if (!d.empty()) setEdit(d);
        return;
    }
    std::wstring f = AskFile(host, L"选择文件", L"APK\0*.apk\0所有文件\0*.*\0");
    if (!f.empty()) setEdit(f);
}

// 选择文件夹：整夹推送
void PickWholeFolder(HWND host) {
    std::wstring d = AskFolder(host, L"选择要整个推送的文件夹");
    if (d.empty()) return;
    g_panel.folder = d;
    g_panel.files.clear();
    g_panel.wholeFolder = true;
    if (g_panel.chkFolder) ::SendMessageW(g_panel.chkFolder, BM_SETCHECK, BST_CHECKED, 0);
    if (g_panel.summary) SetTextOf(g_panel.summary, FileSummary(g_panel));
    UpdatePreview();
}

// 文件夹里挑文件（与整夹互斥）
void PickFilesFromFolder(HWND host) {
    std::wstring d = g_panel.folder;
    if (d.empty()) d = AskFolder(host, L"先选一个文件夹");
    if (d.empty()) return;
    g_panel.folder = d;
    std::vector<std::wstring> chosen;
    ShowFileChecklist(host, d, &chosen);
    if (chosen.empty()) return;
    g_panel.files.clear();
    for (const auto& c : chosen) g_panel.files.push_back(adb::JoinPath(d, c));
    g_panel.wholeFolder = false;
    if (g_panel.chkFolder) ::SendMessageW(g_panel.chkFolder, BM_SETCHECK, BST_UNCHECKED, 0);
    if (g_panel.summary) SetTextOf(g_panel.summary, FileSummary(g_panel));
    UpdatePreview();
}
LRESULT CALLBACK ParamHostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            const adb::CommandSpec* spec = g_app.current;
            if (!spec) break;

            if (code == BN_CLICKED && (id == IDC_BTN_ACTION1 || id == IDC_BTN_ACTION2)) {
                const bool second = (id == IDC_BTN_ACTION2);
                BrowseForParam(hwnd, spec, second);
                return 0;
            }
            if (code == BN_CLICKED && id == IDC_BTN_ACTION3) { PickFilesFromFolder(hwnd); return 0; }
            if (code == BN_CLICKED && id == IDC_BTN_ACTION4) { PickWholeFolder(hwnd); return 0; }
            if (code == BN_CLICKED && id == IDC_BTN_ACTION5) { PickFilesFromFolder(hwnd); return 0; }
            if (code == BN_CLICKED && id == IDC_CHK_WHOLEFOLDER) {
                bool on = ::SendMessageW(g_panel.chkFolder, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (on && g_panel.folder.empty()) {
                    // 勾"整个文件夹"但还没选文件夹 → 立刻让用户选
                    std::wstring d = AskFolder(hwnd, L"选择要整个推送的文件夹");
                    if (d.empty()) {
                        ::SendMessageW(g_panel.chkFolder, BM_SETCHECK, BST_UNCHECKED, 0);
                        return 0;
                    }
                    g_panel.folder = d;
                    g_panel.files.clear();
                }
                g_panel.wholeFolder = on;
                if (g_panel.summary) SetTextOf(g_panel.summary, FileSummary(g_panel));
                UpdatePreview();
                return 0;
            }
            if (id == IDC_COMBO_PARAM && code == CBN_SELCHANGE) {
                UpdatePreview();
                return 0;
            }
            if (code == EN_CHANGE) {
                // 相对路径 / 参数值改动：立刻重算命令行预览
                UpdatePreview();
                return 0;
            }
            break;
        }
        case WM_SIZE:
            RelayoutParams(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, theme::Brush(theme::kPaneBg));
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, theme::Brush(theme::kPaneBg));
            return 1;
        }
        // 主题化的复选框会通过 DrawThemeParentBackground 反过来要求父窗口重绘，
        // 走的就是 WM_PRINTCLIENT。不响应它，父窗口什么都不画，底下残留的旧像素
        // 就露出来了 —— 表现就是"每行复选框后面一条灰底"和"按钮重影"。
        case WM_PRINTCLIENT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            ::FillRect(dc, &rc, theme::Brush(theme::kPaneBg));
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, theme::kText);
            return reinterpret_cast<LRESULT>(theme::Brush(theme::kPaneBg));
        }
        // 复选框默认拿 COLOR_BTNFACE 画底，在白色参数区上会留下一道灰条
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            ::SetBkColor(dc, theme::kPaneBg);
            ::SetBkMode(dc, OPAQUE);
            return reinterpret_cast<LRESULT>(theme::Brush(theme::kPaneBg));
        }
        default:
            break;
    }
    return ::CallWindowProcW(g_oldParamProc, hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- 对外接口
std::wstring AskFile(HWND parent, const wchar_t* title, const wchar_t* filter) {
    wchar_t buf[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!::GetOpenFileNameW(&ofn)) return {};
    return adb::NormalizePath(buf);
}

std::wstring AskFilesMulti(HWND parent, const wchar_t* title, const wchar_t* filter) {
    std::vector<wchar_t> buf(64 * 1024, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!::GetOpenFileNameW(&ofn)) return {};

    // 多选时返回 "目录\0文件1\0文件2\0\0"
    std::wstring joined;
    const wchar_t* p = buf.data();
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == L'\0') return adb::NormalizePath(dir);   // 只选了一个
    for (; *p; p += std::wcslen(p) + 1) {
        if (!joined.empty()) joined += L";";
        joined += adb::JoinPath(dir, p);
    }
    return joined;
}

std::wstring AskFolder(HWND parent, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = parent;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return {};
    wchar_t path[MAX_PATH] = {0};
    bool ok = ::SHGetPathFromIDListW(idl, path) != FALSE;
    ::CoTaskMemFree(idl);
    return ok ? std::wstring(path) : std::wstring();
}

void BuildParamPanel(HWND host, const adb::CommandSpec* spec) {
    // 清空
    HWND child = ::GetWindow(host, GW_CHILD);
    while (child) {
        HWND next = ::GetWindow(child, GW_HWNDNEXT);
        ::DestroyWindow(child);
        child = next;
    }
    g_params.clear();
    g_flags.clear();
    g_items.clear();
    g_panel = PanelState{};
    g_panelHeightDesign = 0;

    if (!g_oldParamProc) {
        // WS_CLIPSIBLINGS：面板里的下拉框/输入框/按钮会整块移动，缺了它会互相覆盖
        LONG_PTR st = ::GetWindowLongPtrW(host, GWL_STYLE);
        ::SetWindowLongPtrW(host, GWL_STYLE, st | WS_CLIPSIBLINGS);
        g_oldParamProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
            host, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParamHostProc)));
    }

    if (!spec) return;

    int y = 0;
    if (spec->param != adb::ParamKind::None && spec->paramKey)
        y = BuildParamBlock(host, spec, false, y);
    if (spec->param2 != adb::ParamKind::None && spec->paramKey2)
        y = BuildParamBlock(host, spec, true, y);
    if (spec->flagCount) {
        // 下方没有参数时加一点呼吸空间
        if (spec->param == adb::ParamKind::None && spec->param2 == adb::ParamKind::None) y += 2;
        y = BuildFlagRows(host, spec, y);
    }
    g_panelHeightDesign = y;

    RelayoutParams(host);
}

int ParamPanelHeight() {
    int h = g_panelHeightDesign > 0 ? g_panelHeightDesign : 0;
    // 没有可选项时彻底收起，不要留一条空白带（看起来像残影）
    if (h <= 0) return 0;
    if (h < 40) h = 40;
    if (h > 300) h = 300;      // 面板本身有上限，给下面的预览/按钮/结果留地方
    return theme::Dpi(h);
}

bool ReadParamPanel(adb::BuildInput* in, std::wstring* error) {
    (void)error;
    in->paths.clear();

    for (const auto& ref : g_params) {
        std::wstring v;
        switch (ref.kind) {
            case adb::ParamKind::RebootTarget:
                v = RebootTargetArg(ComboText(ref.hwnd));
                break;
            case adb::ParamKind::SlotName:
            case adb::ParamKind::Partition:
            case adb::ParamKind::Permission:
                v = adb::Trim(ComboText(ref.hwnd));
                break;
            case adb::ParamKind::RemotePick: {
                std::wstring root = g_panel.combo ? adb::Trim(ComboText(g_panel.combo))
                                                  : g_panel.remoteRoot;
                std::wstring rel = g_panel.edit ? adb::Trim(GetTextOf(g_panel.edit))
                                                : g_panel.remoteRel;
                if (root.empty()) root = L"/sdcard";
                v = JoinRemote(root, rel);
                break;
            }
            case adb::ParamKind::RemoteDyn: {
                std::wstring root = g_panel.combo ? adb::Trim(ComboText(g_panel.combo))
                                                  : L"/sdcard";
                std::wstring rel = g_panel.edit ? adb::Trim(GetTextOf(g_panel.edit)) : std::wstring();
                if (root.empty()) root = L"/sdcard";
                v = JoinRemote(root, rel);
                break;
            }
            case adb::ParamKind::LocalFiles:
                if (g_panel.wholeFolder) {
                    if (!g_panel.folder.empty()) in->paths.push_back(g_panel.folder);
                } else {
                    for (const auto& f : g_panel.files) in->paths.push_back(f);
                }
                v = FileSummary(g_panel);
                break;
            case adb::ParamKind::PropKeys:   v = g_panel.props;  break;
            case adb::ParamKind::ShellPipeline: v = g_panel.shell;  break;
            case adb::ParamKind::Coords:     v = adb::Trim(GetTextOf(ref.hwnd)); break;
            default:                         v = adb::Trim(GetTextOf(ref.hwnd)); break;
        }
        in->params[ref.key] = v;
    }

    for (const auto& ref : g_flags) {
        if (ref.flagKind == adb::FlagKind::Value) {
            in->flags[ref.key] = adb::Trim(GetTextOf(ref.hwnd));
        } else {
            bool checked = ::SendMessageW(ref.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
            in->flags[ref.key] = checked ? L"1" : L"0";
        }
    }
    return true;
}

}  // namespace adbgui