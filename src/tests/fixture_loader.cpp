#include "fixture_loader.h"

#include <windows.h>

#include <string>
#include <vector>

namespace adbtest {

bool LoadFixture(const wchar_t* fileName, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();

    wchar_t exePath[MAX_PATH * 2];
    DWORD n = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (n == 0) return false;

    std::wstring path(exePath, n);
    size_t slash = path.find_last_of(L'\\');
    std::wstring dir = slash == std::wstring::npos ? path : path.substr(0, slash);

    HANDLE h = ::CreateFileW((dir + L"\\" + fileName).c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { ::CloseHandle(h); return false; }
    out->resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    BOOL ok = ::ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) { out->clear(); return false; }
    out->resize(got);
    return !out->empty();
}

}  // namespace adbtest