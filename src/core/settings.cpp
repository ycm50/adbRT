#include "settings.h"

#include <algorithm>
#include <map>

namespace adb {

// ------------------------------------------------------------------- JSON
namespace json {

std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case (char)0x5c: out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out.push_back(s[i]); continue; }
        char n = s[++i];
        switch (n) {
            case '"':  out.push_back('"');  break;
            case (char)0x5c: out.push_back((char)0x5c); break;
            case '/':  out.push_back('/');  break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'u': {
                unsigned code = 0;
                int k = 0;
                for (; k < 4 && i + 1 < s.size(); ++k) {
                    char h = s[++i];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                }
                if (code == 0) break;
                out += WideToUtf8(std::wstring(1, static_cast<wchar_t>(code)));
                break;
            }
            default: out.push_back(n);
        }
    }
    return out;
}

static const char* FindValue(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return nullptr;
    pos += needle.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                 text[pos] == '\r' || text[pos] == '\n')) ++pos;
    if (pos >= text.size() || text[pos] != ':') return nullptr;
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                 text[pos] == '\r' || text[pos] == '\n')) ++pos;
    return text.c_str() + pos;
}

std::string GetString(const std::string& text, const std::string& key, const std::string& def) {
    const char* p = FindValue(text, key);
    if (!p || *p != '"') return def;
    ++p;
    std::string raw;
    for (; *p && *p != '"'; ++p) {
        if (*p == (char)0x5c && p[1]) { raw.push_back(*p++); raw.push_back(*p); continue; }
        raw.push_back(*p);
    }
    return Unescape(raw);
}

bool GetBool(const std::string& text, const std::string& key, bool def) {
    const char* p = FindValue(text, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

int GetInt(const std::string& text, const std::string& key, int def) {
    const char* p = FindValue(text, key);
    if (!p) return def;
    return atoi(p);
}

}  // namespace json

// ------------------------------------------------------------------ 状态
namespace {
std::map<std::string, std::string> g_kv;    // key → JSON 字面量（字符串已含引号）
std::wstring g_configPath;
std::wstring g_note;
uint64_t     g_lastLoad = 0;
}  // namespace

static const char* kKeyAdb = "adb.path";
static const char* kKeyFastboot = "fastboot.path";
static const char* kKeyDevice = "device.current";
static const char* kKeyExpert = "expert.fastboot";
static const char* kKeyRemoteDir = "ui.defaultRemoteDir";
static const char* kKeyCapture = "ui.captureDir";

static std::wstring ConfigDirPath() {
    auto appdata = GetEnvVar(L"APPDATA");
    std::wstring root = appdata ? *appdata : GetExeDir();
    return JoinPath(root, L"AdbRT");
}

Settings& Settings::Instance() {
    static Settings inst;
    return inst;
}

std::wstring Settings::Path() const {
    if (g_configPath.empty()) g_configPath = JoinPath(ConfigDirPath(), L"config.json");
    return g_configPath;
}

const std::wstring& Settings::LastLoadNote() const { return g_note; }

void Settings::Reload(bool force) {
    uint64_t now = ::GetTickCount64();
    if (!force && g_lastLoad != 0 && now - g_lastLoad < 1000) return;
    g_lastLoad = now;

    std::wstring path = Path();
    std::string text;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
            text.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            ::ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
            text.resize(got);
        }
        ::CloseHandle(h);
    }

    g_kv.clear();
    if (text.empty()) {
        g_note = L"未找到配置文件，使用默认值";
        return;
    }

    // 只解析"扁平的 key: string/bool/int"这一层，够用且不会误吞嵌套结构
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        size_t q = text.find('"', i);
        if (q == std::string::npos) break;
        size_t qe = q + 1;
        std::string keyRaw;
        while (qe < n && text[qe] != '"') {
            if (text[qe] == (char)0x5c && qe + 1 < n) { keyRaw.push_back(text[qe++]); }
            keyRaw.push_back(text[qe++]);
        }
        std::string key = json::Unescape(keyRaw);
        size_t colon = text.find(':', qe);
        if (colon == std::string::npos) break;
        size_t v = colon + 1;
        while (v < n && (text[v] == ' ' || text[v] == '\t' || text[v] == '\r' || text[v] == '\n')) ++v;
        if (v >= n) break;

        if (text[v] == '"') {
            size_t ve = v + 1;
            std::string raw;
            while (ve < n && text[ve] != '"') {
                if (text[ve] == (char)0x5c && ve + 1 < n) { raw.push_back(text[ve++]); }
                raw.push_back(text[ve++]);
            }
            g_kv[key] = "\"" + raw + "\"";
            i = ve + 1;
        } else {
            size_t ve = v;
            while (ve < n && text[ve] != ',' && text[ve] != '\n' && text[ve] != '}') ++ve;
            std::string lit = WideToUtf8(Trim(Utf8ToWide(text.substr(v, ve - v))));
            if (!lit.empty()) g_kv[key] = lit;
            i = ve;
        }
    }
    g_note = Format(L"已加载 %zu 项配置", g_kv.size());
}

bool Settings::Save() {
    std::wstring path = Path();
    if (!EnsureDirectory(ParentOf(path))) return false;

    std::string text = "{\r\n";
    size_t idx = 0;
    for (const auto& [k, v] : g_kv) {
        text += "  \"" + json::Escape(k) + "\": " + v;
        if (++idx != g_kv.size()) text += ",";
        text += "\r\n";
    }
    text += "}\r\n";

    std::wstring tmp = path + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    ::CloseHandle(h);
    if (!ok || written != text.size()) { ::DeleteFileW(tmp.c_str()); return false; }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string Settings::GetString(const std::string& key, const std::string& def) const {
    auto it = g_kv.find(key);
    if (it == g_kv.end()) return def;
    const std::string& v = it->second;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        return json::Unescape(v.substr(1, v.size() - 2));
    return v;
}

bool Settings::GetBool(const std::string& key, bool def) const {
    auto it = g_kv.find(key);
    if (it == g_kv.end()) return def;
    if (it->second == "true" || it->second == "1") return true;
    if (it->second == "false" || it->second == "0") return false;
    return def;
}

int Settings::GetInt(const std::string& key, int def) const {
    auto it = g_kv.find(key);
    if (it == g_kv.end()) return def;
    return atoi(it->second.c_str());
}

void Settings::SetString(const std::string& key, const std::string& value) {
    g_kv[key] = "\"" + json::Escape(value) + "\"";
}

void Settings::SetBool(const std::string& key, bool value) {
    g_kv[key] = value ? "true" : "false";
}

void Settings::SetInt(const std::string& key, int value) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", value);
    g_kv[key] = buf;
}

bool Settings::Has(const std::string& key) const { return g_kv.count(key) != 0; }
size_t Settings::KeyCount() const { return g_kv.size(); }

void Settings::ResetForTest() {
    g_kv.clear();
    g_configPath.clear();
    g_lastLoad = 0;
    g_note.clear();
}

// ------------------------------------------------------------- 语义化访问
std::wstring Settings::AdbSpec() const {
    return Utf8ToWide(GetString(kKeyAdb, ""));
}
std::wstring Settings::FastbootSpec() const {
    return Utf8ToWide(GetString(kKeyFastboot, ""));
}
void Settings::SetAdbSpec(const std::wstring& spec) { SetString(kKeyAdb, WideToUtf8(spec)); }
void Settings::SetFastbootSpec(const std::wstring& spec) {
    SetString(kKeyFastboot, WideToUtf8(spec));
}

namespace {
std::wstring g_transientAdb;
}

void SetTransientAdbOverride(const std::wstring& path) { g_transientAdb = path; }
void ClearTransientAdbOverride() { g_transientAdb.clear(); }

std::wstring Settings::AdbPath() const {
    if (!g_transientAdb.empty()) return g_transientAdb;
    std::wstring spec = AdbSpec();
    if (!spec.empty()) {
        std::wstring resolved = ResolveAgainstExeDir(spec);
        if (PathExists(resolved)) return resolved;
    }
    return const_cast<Settings*>(this)->DetectAdb();
}

std::wstring Settings::FastbootPath() const {
    std::wstring spec = FastbootSpec();
    if (!spec.empty()) {
        std::wstring resolved = ResolveAgainstExeDir(spec);
        if (PathExists(resolved)) return resolved;
    }
    return const_cast<Settings*>(this)->DetectFastboot();
}

std::wstring Settings::CurrentDevice() const {
    return Utf8ToWide(GetString(kKeyDevice, ""));
}
void Settings::SetCurrentDevice(const std::wstring& serial) {
    SetString(kKeyDevice, WideToUtf8(serial));
}
bool Settings::ExpertFastboot() const { return GetBool(kKeyExpert, false); }
void Settings::SetExpertFastboot(bool on) { SetBool(kKeyExpert, on); }

std::wstring Settings::DefaultRemoteDir() const {
    std::wstring v = Utf8ToWide(GetString(kKeyRemoteDir, ""));
    return v.empty() ? L"/sdcard/Download" : v;
}

std::wstring Settings::CaptureDir() const {
    std::wstring v = Utf8ToWide(GetString(kKeyCapture, ""));
    if (!v.empty()) return ResolveAgainstExeDir(v);
    // 默认：<图片>AdbRT（没有"图片"就退回 下载AdbRT）
    if (auto pics = GetEnvVar(L"USERPROFILE")) {
        std::wstring dir = JoinPath(*pics, L"Pictures\\AdbRT");
        return dir;
    }
    return JoinPath(DownloadDir(), L"AdbRT");
}

std::wstring Settings::DownloadDir() const {
    auto up = GetEnvVar(L"USERPROFILE");
    std::wstring base = up ? *up : GetExeDir();
    return JoinPath(base, L"Downloads");
}

// 探测候选：既支持相对目录（相对 exe 目录），也支持绝对目录
static std::wstring ProbeTool(const std::wstring& exeName,
                              const std::vector<std::wstring>& extraDirs) {
    std::vector<std::wstring> dirs;
    dirs.push_back(JoinPath(GetExeDir(), L"platform-tools"));
    dirs.push_back(GetExeDir());
    for (const auto& d : extraDirs) dirs.push_back(d);

    if (auto home = GetEnvVar(L"ANDROID_HOME")) dirs.push_back(JoinPath(*home, L"platform-tools"));
    if (auto sdk = GetEnvVar(L"ANDROID_SDK_ROOT")) dirs.push_back(JoinPath(*sdk, L"platform-tools"));
    if (auto la = GetEnvVar(L"LOCALAPPDATA"))
        dirs.push_back(JoinPath(*la, L"Android\\Sdk\\platform-tools"));

    for (const auto& d : dirs) {
        if (d.empty()) continue;
        std::wstring candidate = JoinPath(d, exeName);
        if (PathExists(candidate)) return NormalizePath(candidate);
    }

    // 扫用户 PATH（安装器写进去的那一项）与系统 PATH
    auto scanPath = [&](const std::wstring& pathValue) -> std::wstring {
        size_t start = 0;
        while (start <= pathValue.size()) {
            size_t semi = pathValue.find(L';', start);
            std::wstring seg = Trim(pathValue.substr(
                start, semi == std::wstring::npos ? std::wstring::npos : semi - start));
            if (!seg.empty()) {
                std::wstring dir = NormalizePath(ExpandEnvVars(seg));
                if (PathIsDirectory(dir)) {
                    std::wstring candidate = JoinPath(dir, exeName);
                    if (PathExists(candidate)) return NormalizePath(candidate);
                }
            }
            if (semi == std::wstring::npos) break;
            start = semi + 1;
        }
        return {};
    };
    for (const auto& e : GetUserPathEntries()) {
        std::wstring dir = NormalizePath(ExpandEnvVars(e));
        if (!PathIsDirectory(dir)) continue;
        std::wstring candidate = JoinPath(dir, exeName);
        if (PathExists(candidate)) return NormalizePath(candidate);
    }
    if (auto systemPath = GetEnvVar(L"PATH")) {
        if (std::wstring hit = scanPath(*systemPath); !hit.empty()) return hit;
    }
    return {};
}

std::wstring Settings::DetectAdb() {
    return ProbeTool(L"adb.exe", {});
}

std::wstring Settings::DetectFastboot() {
    return ProbeTool(L"fastboot.exe", {});
}

}  // namespace adb