#pragma once
// ---------------------------------------------------------------------------
// GUI 主窗口：设备栏 / 命令树 / 参数面板 / 命令预览 / 实时输出
//   · 与 gitRT 同构，但上下文是"设备"而不是"仓库"
//   · 所有 adb 调用都在工作线程里，UI 线程只收消息
// ---------------------------------------------------------------------------

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/adb_runner.h"
#include "../core/command_builder.h"
#include "../core/command_spec.h"
#include "../core/parsers.h"

namespace adbgui {

constexpr UINT WM_APP_TASK_LINE   = WM_APP + 1;   // wp = 任务序号，lp = new std::wstring*
constexpr UINT WM_APP_TASK_DONE   = WM_APP + 2;   // wp = 任务序号，lp = new TaskOutcome*
constexpr UINT WM_APP_TASK_START  = WM_APP + 3;
constexpr UINT WM_APP_TASK_PROGRESS = WM_APP + 4;

struct TaskOutcome {
    bool         ok = false;
    bool         cancelled = false;
    DWORD        exitCode = 0;
    std::wstring message;
    uint64_t     elapsedMs = 0;
};

struct AppContext {
    std::wstring initialPath;      // 命令行传入的本地路径
    bool         fromApk = false;
    std::string  initialCommand;   // --open <key>：启动后直接执行这条内置命令
    std::string  initialSelect;    // --select <key>：只选中，不执行
};

struct AppState {
    HWND  hwnd = nullptr;
    HWND  hDeviceCombo = nullptr;
    HWND  hTree = nullptr;
    HWND  hParamHost = nullptr;
    HWND  hPreview = nullptr;
    HWND  hOutput = nullptr;
    HWND  hStatus = nullptr;
    HWND  hRun = nullptr;
    HWND  hCancel = nullptr;
    HWND  hRefreshing = nullptr;
    HWND  btnFastboot = nullptr;
    HWND  hSearch = nullptr;
    HWND  hCmdTitle = nullptr;
    HWND  hDanger = nullptr;
    HWND  hHeader = nullptr;
    HWND  hParamsLabel = nullptr;
    HWND  hOutputLabel = nullptr;
    HWND  hPreviewLabel = nullptr;
    HWND  hSettings = nullptr;
    HWND  hAbout = nullptr;
    HWND  hClearOut = nullptr;
    HWND  hSaveOut = nullptr;
    HWND  hCopy = nullptr;
    bool  treeDirty = false;

    const adb::CommandSpec* current = nullptr;
    std::vector<adb::DeviceEntry> devices;
    std::wstring serial;
    bool   showFastboot = false;
    bool   busy = false;

    HFONT font = nullptr;
    HFONT fontMono = nullptr;

    std::atomic<bool> cancelFlag{false};
    int  nextTaskId = 1;
    int  activeTaskId = 0;
    HANDLE workerThread = nullptr;

    // 参数控件句柄（按 spec.flag key 索引）
    std::vector<std::pair<std::string, HWND>> flagControls;
    std::vector<std::pair<std::string, HWND>> paramControls;

    adb::BuiltCommand preview;
    std::wstring pendingOutput;
};

extern AppState g_app;

int  RunApp(HINSTANCE hinst, const AppContext& ctx, int showCmd);
int  RunSelfTest(const std::wstring& reportPath);

// CLI 门面（scripting）：走同一条 BuildCommand 路径，不存在绕过白名单的旁路
int  RunCli(const std::vector<std::wstring>& args);
int  ListCommands(const std::wstring& outFile);

// 安装/卸载（不写注册表，只动用户 PATH 与桌面快捷方式）
int  RunInstall(const std::wstring& installDir, bool addPath, bool desktopShortcut, bool quiet);
int  RunUninstall(bool removePath, bool desktopShortcut, bool quiet);
bool DesktopShortcutExists();
bool SetDesktopShortcut(bool create);
bool IsInUserPath(const std::wstring& dir);

// 主窗口（main_window.cpp）
bool CreateMainWindow(HINSTANCE hinst, const AppContext& ctx);
void RefreshDevices(HWND hwnd, bool keepSelection);
void SelectCommand(const adb::CommandSpec* spec);
void UpdatePreview();
void CopyPreviewToClipboard(HWND hwnd);
void SaveOutputToFile(HWND hwnd);
void LayoutChildren(HWND hwnd);
void SetStatus(const wchar_t* text);
bool CollectInput(adb::BuildInput* in, std::wstring* error);
void StartRun();
void CancelRun();

// 参数面板（param_panel.cpp）
void BuildParamPanel(HWND host, const adb::CommandSpec* spec);
bool ReadParamPanel(adb::BuildInput* in, std::wstring* error);

// 任务（task.cpp）
void PostLine(int taskId, const std::wstring& line);
void PostDone(int taskId, bool ok, bool cancelled, DWORD exitCode, const std::wstring& msg);
void AppendOutput(const std::wstring& text);
void ClearOutput();

// 对话框（settings_dialog.cpp）
void ShowSettings(HWND parent);
bool ShowConfirmDialog(HWND parent, const std::wstring& title, const std::wstring& message);
bool ShowBreakerDialog(HWND parent, const std::wstring& title, const std::wstring& message,
                       const std::wstring& expectText);

// 内嵌功能（builtin.cpp）：设备列表 / 信息 / 应用列表 / 截屏 / 文件浏览 / logcat / 终端
bool RunBuiltinCommand(HWND hwnd, const adb::CommandSpec& spec, std::wstring* error);

// 各内嵌窗口（builtin.cpp）
void ShowDeviceList(HWND parent);
void ShowDeviceInfo(HWND parent);
void ShowInstalledApps(HWND parent);
void ShowPullApk(HWND parent);
void ShowFileBrowser(HWND parent);
void ShowLogcat(HWND parent);
void CaptureScreenshot(HWND parent);
void OpenInteractiveShell(HWND parent);

// 通用文本窗口（只读，可复制）
void ShowTextView(HWND parent, const std::wstring& title, const std::wstring& text);
void ShowTextViewAsync(HWND parent, const std::wstring& title, const std::wstring& placeholder,
                       std::function<std::wstring()> loader);

std::wstring AskFile(HWND parent, const wchar_t* title, const wchar_t* filter);
std::wstring AskFolder(HWND parent, const wchar_t* title);

}  // namespace adbgui