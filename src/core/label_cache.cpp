#include "label_cache.h"

#include <windows.h>

#include <map>

#include "apk_res.h"
#include "core.h"
#include "inflate.h"
#include "settings.h"

namespace adb {

namespace {

// 从 zip 里取一个条目（只读局部：先读中央目录，再 seek 读该条目）
bool ExtractZipEntry(const std::wstring& zipPath, const char* entryName,
                     std::vector<uint8_t>* out) {
    if (!out) return false;
    HANDLE h = ::CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    auto close = [&]() { ::CloseHandle(h); };
    auto readAt = [&](uint64_t off, void* buf, DWORD len, DWORD* got) -> bool {
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(off);
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        return ::ReadFile(h, buf, len, got, nullptr) != FALSE;
    };

    // 找 EOCD（末尾最多 64KB）
    LARGE_INTEGER sizeLi{};
    if (!::GetFileSizeEx(h, &sizeLi)) { close(); return false; }
    uint64_t fileSize = static_cast<uint64_t>(sizeLi.QuadPart);
    uint64_t tailLen = fileSize < 65536 ? fileSize : 65536;
    std::vector<uint8_t> tail(static_cast<size_t>(tailLen));
    DWORD got = 0;
    if (!readAt(fileSize - tailLen, tail.data(), static_cast<DWORD>(tailLen), &got)) {
        close(); return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() >= 22 ? tail.size() - 22 + 1 : 0; i-- > 0;) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 &&
            tail[i + 3] == 0x06) { eocd = i; break; }
    }
    if (eocd == std::string::npos) { close(); return false; }

    auto rd16 = [](const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); };
    auto rd32 = [](const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };

    uint16_t count = rd16(tail.data() + eocd + 10);
    uint32_t cdSize = rd32(tail.data() + eocd + 12);
    uint32_t cdOff = rd32(tail.data() + eocd + 16);
    if (count == 0 || cdSize == 0) { close(); return false; }

    std::vector<uint8_t> cd(cdSize);
    if (!readAt(cdOff, cd.data(), cdSize, &got) || got != cdSize) { close(); return false; }

    size_t p = 0;
    for (uint16_t i = 0; i < count && p + 46 <= cd.size(); ++i) {
        if (!(cd[p] == 0x50 && cd[p + 1] == 0x4b && cd[p + 2] == 0x01 && cd[p + 3] == 0x02))
            break;
        uint16_t nameLen = rd16(cd.data() + p + 28);
        uint16_t extraLen = rd16(cd.data() + p + 30);
        uint16_t commentLen = rd16(cd.data() + p + 32);
        uint32_t compSize = rd32(cd.data() + p + 20);
        uint32_t uncompSize = rd32(cd.data() + p + 24);
        uint32_t localOff = rd32(cd.data() + p + 42);
        uint16_t method = rd16(cd.data() + p + 10);

        std::string name(reinterpret_cast<const char*>(cd.data() + p + 46), nameLen);
        if (name == entryName && (method == 0 || method == 8)) {
            // 读本地头，跳过可变长字段
            uint8_t lh[30];
            if (!readAt(localOff, lh, 30, &got) || got != 30) break;
            uint16_t lhNameLen = rd16(lh + 26);
            uint16_t lhExtraLen = rd16(lh + 28);
            uint64_t dataOff = static_cast<uint64_t>(localOff) + 30 + lhNameLen + lhExtraLen;

            std::vector<uint8_t> raw(compSize);
            if (!readAt(dataOff, raw.data(), compSize, &got) || got != compSize) break;

                        if (method == 0) {
                *out = std::move(raw);
            } else {
                // deflate：AndroidManifest.xml 基本都是压缩的，必须解压才能读 label
                if (!InflateRaw(raw.data(), raw.size(), uncompSize, out)) out->clear();
            }
            close();
            return !out->empty();
        }
        p += 46 + nameLen + extraLen + commentLen;
    }
    close();
    return false;
}

std::wstring LabelDir() {
    auto la = GetEnvVar(L"LOCALAPPDATA");
    std::wstring root = la ? *la : GetExeDir();
    return JoinPath(root, L"AdbRT\\labels");
}

}  // namespace

std::wstring LabelCachePath(const std::wstring& serial) {
    std::wstring safe;
    for (wchar_t c : serial) {
        if (c == L':' || c == L'/' || c == L'\\') safe.push_back(L'_');
        else safe.push_back(c);
    }
    return JoinPath(LabelDir(), safe + L".txt");
}

bool LoadLabelCache(const std::wstring& serial, std::vector<AppLabel>* out) {
    if (!out) return false;
    out->clear();
    HANDLE h = ::CreateFileW(LabelCachePath(serial).c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string text;
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 22)) {
        text.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ::ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
        text.resize(got);
    }
    ::CloseHandle(h);

    for (const auto& line : SplitLines(Utf8ToWide(text))) {
        // 格式：S<TAB>包名<TAB>名称   （S = 系统，U = 第三方）
        if (line.size() < 3) continue;
        std::vector<std::wstring> parts;
        std::wstring cur;
        for (wchar_t c : line) {
            if (c == L'\t') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
        if (parts.size() < 3) continue;
        AppLabel a;
        a.system = parts[0] == L"S";
        a.package = parts[1];
        a.label = parts[2];
        if (!a.package.empty()) out->push_back(a);
    }
    return !out->empty();
}

bool SaveLabelCache(const std::wstring& serial, const std::vector<AppLabel>& items) {
    std::wstring path = LabelCachePath(serial);
    EnsureDirectory(ParentOf(path));
    std::wstring text;
    for (const auto& a : items) {
        text += (a.system ? L"S" : L"U");
        text += L"\t" + a.package;
        text += L"\t" + a.label;
        text += L"\n";
    }
    std::string utf8 = WideToUtf8(text);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(h);
    return ok != FALSE;
}

std::wstring ReadApkLabel(const std::wstring& apkPath) {
    std::vector<uint8_t> manifest, arsc;
    if (!ExtractZipEntry(apkPath, "AndroidManifest.xml", &manifest)) return {};
    ExtractZipEntry(apkPath, "resources.arsc", &arsc);
    return ResolveAppLabel(manifest, arsc);
}

}  // namespace adb