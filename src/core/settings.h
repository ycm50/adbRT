#pragma once
// ---------------------------------------------------------------------------
// 配置：%APPDATA%\AdbRT\config.json
//
// ★ 产品决策：**不写注册表**。所有持久化都在这个文件里。
//   唯一的例外是安装脚本把安装目录写进"用户 PATH"（用户明确要求），
//   那是安装器行为，不是本程序运行时行为。
//
// 路径类配置项支持三种写法（用户要求「相对目录和绝对目录都要支持」）：
//   绝对：   A:\platform-tools\adb.exe
//   相对：   .\platform-tools\adb.exe      （相对 exe 所在目录）
//   环境变量： %LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "core.h"

namespace adb {

class Settings {
public:
    static Settings& Instance();

    void Reload(bool force = false);
    bool Save();
    std::wstring Path() const;
    const std::wstring& LastLoadNote() const;

    std::string GetString(const std::string& key, const std::string& def = {}) const;
    bool        GetBool(const std::string& key, bool def) const;
    int         GetInt(const std::string& key, int def) const;
    void        SetString(const std::string& key, const std::string& value);
    void        SetBool(const std::string& key, bool value);
    void        SetInt(const std::string& key, int value);
    bool        Has(const std::string& key) const;
    size_t      KeyCount() const;
    void        ResetForTest();

    // ---- 语义化访问 ----
    std::wstring AdbSpec() const;          // 原始写法（可能是相对路径）
    std::wstring FastbootSpec() const;
    std::wstring AdbPath() const;          // 解析后的绝对路径
    std::wstring FastbootPath() const;
    void         SetAdbSpec(const std::wstring& spec);
    void         SetFastbootSpec(const std::wstring& spec);
    std::wstring CurrentDevice() const;
    void         SetCurrentDevice(const std::wstring& serial);
    bool         ExpertFastboot() const;
    void         SetExpertFastboot(bool on);
    std::wstring DefaultRemoteDir() const;
    std::wstring CaptureDir() const;
    std::wstring DownloadDir() const;

    // 自动探测（设置里的路径无效时才用）：
    // ① exe 目录\platform-tools  ② exe 目录  ③ 用户 PATH 里的 adb
    // ④ %ANDROID_HOME%/platform-tools  ⑤ %LOCALAPPDATA%\Android\Sdk\platform-tools
    std::wstring DetectAdb();
    std::wstring DetectFastboot();
};

// 命令行 --adb 的临时覆盖（不写配置文件，进程退出即失效）
void SetTransientAdbOverride(const std::wstring& path);
void ClearTransientAdbOverride();

// 手写 JSON 取值（零第三方依赖）
namespace json {
std::string GetString(const std::string& text, const std::string& key, const std::string& def);
bool        GetBool(const std::string& text, const std::string& key, bool def);
int         GetInt(const std::string& text, const std::string& key, int def);
std::string Escape(const std::string& s);
std::string Unescape(const std::string& s);
}  // namespace json

}  // namespace adb