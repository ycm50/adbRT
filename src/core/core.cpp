#include "core.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace adb {

// ------------------------------------------------------------------- 字符串
std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToLowerAscii(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

std::wstring Trim(std::wstring_view s) {
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

std::wstring ReplaceAll(std::wstring s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return s;
    std::wstring out;
    out.reserve(s.size());
    size_t pos = 0;
    for (;;) {
        size_t hit = s.find(from, pos);
        if (hit == std::wstring::npos) { out.append(s, pos, std::wstring::npos); break; }
        out.append(s, pos, hit - pos);
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t stackBuf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(stackBuf, 1024, fmt, ap);
    va_end(ap);
    if (n < 0) {
        std::vector<wchar_t> big(8192);
        va_start(ap, fmt);
        n = _vsnwprintf(big.data(), big.size(), fmt, ap);
        va_end(ap);
        if (n < 0) return std::wstring();
        return std::wstring(big.data(), static_cast<size_t>(n));
    }
    return std::wstring(stackBuf, static_cast<size_t>(n));
}

std::vector<std::wstring> SplitLines(std::wstring_view text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    if (text.empty()) return out;
    for (;;) {
        size_t nl = text.find(L'\n', start);
        size_t end = (nl == std::wstring_view::npos) ? text.size() : nl;
        std::wstring line(text.substr(start, end - start));
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        out.push_back(line);
        if (nl == std::wstring_view::npos) break;
        start = nl + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::vector<std::wstring> SplitWhitespace(std::wstring_view text) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
        size_t b = i;
        while (i < text.size() && text[i] != L' ' && text[i] != L'\t') ++i;
        if (i > b) out.emplace_back(text.substr(b, i - b));
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

// ---------------------------------------------------------------------- 路径
static bool IsSep(wchar_t c) { return c == L'\x5c' || c == L'/'; }

bool IsAbsolutePath(std::wstring_view p) {
    if (p.size() >= 2 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
        p[1] == L':')
        return true;
    if (p.size() >= 2 && IsSep(p[0]) && IsSep(p[1])) return true;   // UNC
    return false;
}

const wchar_t kBs = L'\x5c';

std::wstring NormalizePath(std::wstring p) {
    if (p.empty()) return {};
    for (auto& c : p)
        if (c == L'/') c = kBs;

    std::wstring prefix;
    size_t i = 0;
    if (IsAbsolutePath(p)) {
        if (p.size() >= 2 && p[1] == L':') {
            prefix = p.substr(0, 2);
            i = 2;
        } else {
            size_t s1 = p.find(kBs, 2);
            size_t s2 = (s1 == std::wstring::npos) ? std::wstring::npos : p.find(kBs, s1 + 1);
            if (s2 == std::wstring::npos) return p;
            prefix = p.substr(0, s2);
            i = s2;
        }
    }

    std::vector<std::wstring> parts;
    while (i <= p.size()) {
        size_t next = p.find(kBs, i);
        std::wstring seg =
            p.substr(i, (next == std::wstring::npos ? p.size() : next) - i);
        if (!seg.empty() && seg != L".") {
            if (seg == L"..") {
                if (!parts.empty() && parts.back() != L"..") parts.pop_back();
                else if (prefix.empty()) parts.push_back(L"..");
            } else {
                parts.push_back(seg);
            }
        }
        if (next == std::wstring::npos) break;
        i = next + 1;
    }

    std::wstring out = prefix;
    for (const auto& seg : parts) {
        if (!out.empty() && out.back() != kBs) out.push_back(kBs);
        out += seg;
    }
    if (out.empty()) out = prefix;
    if (!prefix.empty() && out.size() == prefix.size()) out += kBs;
    if (out.empty()) out = L".";
    return out;
}

bool PathExists(std::wstring_view p) {
    DWORD a = ::GetFileAttributesW(std::wstring(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool PathIsDirectory(std::wstring_view p) {
    DWORD a = ::GetFileAttributesW(std::wstring(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectory(std::wstring_view p) {
    if (p.empty()) return false;
    std::wstring path = NormalizePath(std::wstring(p));
    if (PathIsDirectory(path)) return true;
    std::vector<std::wstring> stack;
    std::wstring cur = path;
    while (!cur.empty() && !PathIsDirectory(cur)) {
        stack.push_back(cur);
        std::wstring parent = ParentOf(cur);
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (!::CreateDirectoryW(it->c_str(), nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return PathIsDirectory(path);
}

std::wstring JoinPath(std::wstring_view a, std::wstring_view b) {
    if (a.empty()) return NormalizePath(std::wstring(b));
    if (b.empty()) return NormalizePath(std::wstring(a));
    std::wstring out(a);
    if (out.back() != kBs) out.push_back(kBs);
    out.append(b);
    return NormalizePath(out);
}

std::wstring FileNameOf(std::wstring_view p) {
    size_t pos = p.find_last_of(L"\x5c/");
    return pos == std::wstring_view::npos ? std::wstring(p) : std::wstring(p.substr(pos + 1));
}

std::wstring ParentOf(std::wstring_view p) {
    std::wstring s = NormalizePath(std::wstring(p));
    size_t pos = s.find_last_of(kBs);
    if (pos == std::wstring::npos) return {};
    // 盘符根：C: 加一个反斜杠，长度 3
    if (pos == 2 && s.size() >= 2 && s[1] == L':') return s.substr(0, 3);
    std::wstring parent = s.substr(0, pos);
    if (parent.size() == 2 && parent[1] == L':') parent += kBs;
    return parent;
}

std::wstring GetExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    return std::wstring(buf, n);
}

std::wstring GetExeDir() {
    std::wstring exe = GetExePath();
    return exe.empty() ? std::wstring() : ParentOf(exe);
}

std::wstring ExpandEnvVars(std::wstring_view in) {
    std::wstring out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == L'%') {
            size_t end = in.find(L'%', i + 1);
            if (end == std::wstring_view::npos) { out.append(in.substr(i)); break; }
            std::wstring name(in.substr(i + 1, end - i - 1));
            if (name.empty()) { out.push_back(L'%'); i = end + 1; continue; }
            wchar_t val[32768];
            DWORD n = ::GetEnvironmentVariableW(name.c_str(), val,
                                                static_cast<DWORD>(std::size(val)));
            if (n > 0 && n < std::size(val)) out.append(val, n);
            else out.append(in.substr(i, end - i + 1));
            i = end + 1;
        } else {
            out.push_back(in[i++]);
        }
    }
    return out;
}

std::wstring ResolvePathSpec(std::wstring_view spec, std::wstring_view baseDir) {
    std::wstring s = Trim(spec);
    if (s.empty()) return {};
    s = ExpandEnvVars(s);
    if (s == L"~" || StartsWith(s, L"~\x5c") || StartsWith(s, L"~/")) {
        s = (s.size() > 1) ? JoinPath(baseDir, s.substr(2)) : NormalizePath(std::wstring(baseDir));
    }
    if (!IsAbsolutePath(s)) s = JoinPath(baseDir, s);
    return NormalizePath(s);
}

std::wstring ResolveAgainstExeDir(std::wstring_view spec) {
    return ResolvePathSpec(spec, GetExeDir());
}

std::wstring MakeRelativeTo(std::wstring_view baseDir, std::wstring_view absPath) {
    std::wstring b = NormalizePath(std::wstring(baseDir));
    std::wstring a = NormalizePath(std::wstring(absPath));
    if (b.empty() || a.empty()) return {};
    std::wstring bl = ToLowerAscii(b), al = ToLowerAscii(a);
    if (bl.back() != kBs) bl.push_back(kBs);
    if (al.size() <= bl.size() || al.compare(0, bl.size(), bl) != 0) return {};
    std::wstring rel = a.substr(bl.size());
    std::replace(rel.begin(), rel.end(), kBs, L'/');
    return rel;
}

std::wstring DescribePathSpec(std::wstring_view spec) {
    std::wstring s = Trim(spec);
    if (s.empty()) return L"（未设置）";
    std::wstring expanded = ExpandEnvVars(s);
    if (IsAbsolutePath(expanded)) return expanded;
    return Format(L"%ls（相对程序目录）", ResolveAgainstExeDir(s).c_str());
}

// ---------------------------------------------------------------------- 环境
std::optional<std::wstring> GetEnvVar(const wchar_t* name) {
    DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    DWORD got = ::GetEnvironmentVariableW(name, buf.data(), n);
    if (got == 0) return std::nullopt;
    buf.resize(got);
    return buf;
}

static const wchar_t* kUserEnvKey = L"Environment";

bool BroadcastEnvironmentChange() {
    DWORD_PTR rc = 0;
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"Environment"),
                          SMTO_ABORTIFHUNG, 3000, &rc);
    return true;
}

bool SetUserEnvVar(const wchar_t* name, std::wstring_view value) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUserEnvKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (value.empty()) {
        rc = ::RegDeleteValueW(key, name);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    } else {
        rc = ::RegSetValueExW(key, name, 0, REG_EXPAND_SZ,
                              reinterpret_cast<const BYTE*>(value.data()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    ::RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::vector<std::wstring> GetUserPathEntries() {
    std::vector<std::wstring> out;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUserEnvKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return out;
    DWORD type = 0, size = 0;
    if (::RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        size > 0) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
        if (::RegQueryValueExW(key, L"Path", nullptr, &type,
                               reinterpret_cast<BYTE*>(buf.data()), &size) == ERROR_SUCCESS) {
            std::wstring all(buf.data());
            size_t start = 0;
            while (start <= all.size()) {
                size_t semi = all.find(L';', start);
                std::wstring seg = Trim(all.substr(
                    start, semi == std::wstring::npos ? std::wstring::npos : semi - start));
                if (!seg.empty()) out.push_back(seg);
                if (semi == std::wstring::npos) break;
                start = semi + 1;
            }
        }
    }
    ::RegCloseKey(key);
    return out;
}

bool AddToUserPath(std::wstring_view dir) {
    std::wstring want = NormalizePath(std::wstring(dir));
    if (want.empty()) return false;
    auto entries = GetUserPathEntries();
    for (const auto& e : entries)
        if (EqualsNoCase(NormalizePath(e), want)) return true;   // 幂等
    entries.insert(entries.begin(), want);
    if (!SetUserEnvVar(L"Path", Join(entries, L";"))) return false;
    BroadcastEnvironmentChange();
    return true;
}

bool RemoveFromUserPath(std::wstring_view dir) {
    std::wstring want = NormalizePath(std::wstring(dir));
    auto entries = GetUserPathEntries();
    std::vector<std::wstring> kept;
    bool removed = false;
    for (const auto& e : entries) {
        if (EqualsNoCase(NormalizePath(e), want)) { removed = true; continue; }
        kept.push_back(e);
    }
    if (!removed) return true;
    SetUserEnvVar(L"Path", Join(kept, L";"));
    BroadcastEnvironmentChange();
    return true;
}

// ---------------------------------------------------------------------- 日志
namespace {
std::mutex   g_logMutex;
HANDLE       g_logFile = INVALID_HANDLE_VALUE;
std::wstring g_logPath;
LogLevel     g_logLevel = LogLevel::Info;

const char* LevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        default:              return "DEBUG";
    }
}
}  // namespace

std::wstring DefaultLogDir() {
    auto base = GetEnvVar(L"LOCALAPPDATA");
    std::wstring root = base ? *base : GetExeDir();
    return JoinPath(root, L"AdbRT\x5c" L"logs");
}

void LogInit(const std::wstring& dir, LogLevel minLevel) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logLevel = minLevel;
    if (g_logFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    std::wstring d = dir.empty() ? DefaultLogDir() : dir;
    if (d.empty() || !EnsureDirectory(d)) return;
    g_logPath = JoinPath(d, Format(L"adbrt-%ls.log", NowStamp().c_str()));
    g_logFile = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) g_logPath.clear();
}

void LogSetLevel(LogLevel lv) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logLevel = lv;
}

LogLevel LogGetLevel() { return g_logLevel; }

void LogWrite(LogLevel lv, const char* category, const std::string& msg) {
    if (lv > g_logLevel) return;
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char head[64];
    _snprintf_s(head, sizeof(head), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
    std::string full = std::string(head) + LevelName(lv) + " [" +
                       (category ? category : "-") + "] " + msg + "\r\n";
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(g_logFile, full.data(), static_cast<DWORD>(full.size()), &written, nullptr);
    }
    ::OutputDebugStringA(full.c_str());
}

void LogFlush() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) ::FlushFileBuffers(g_logFile);
}

std::wstring LogFilePath() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    return g_logPath;
}

// ------------------------------------------------------------------ 实用工具
std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return Format(u == 0 ? L"%.0f %ls" : L"%.1f %ls", v, units[u]);
}

std::wstring NowStamp() {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    return Format(L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
}

std::wstring FormatDuration(uint64_t ms) {
    if (ms < 1000) return Format(L"%llu ms", static_cast<unsigned long long>(ms));
    if (ms < 60000) return Format(L"%.1f s", static_cast<double>(ms) / 1000.0);
    return Format(L"%llu 分 %llu 秒", static_cast<unsigned long long>(ms / 60000),
                  static_cast<unsigned long long>((ms % 60000) / 1000));
}

}  // namespace adb