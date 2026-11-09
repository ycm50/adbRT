#include "command_builder.h"

#include <algorithm>

namespace adb {

namespace {

constexpr wchar_t kBs = L'\x5c';

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

// Windows 命令行引号规则（与 CommandLineToArgvW 的解析一致）
std::wstring QuoteArgImpl(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    bool need = false;
    for (wchar_t c : arg) {
        if (IsSpace(c) || c == L'"' || c == L'\t') { need = true; break; }
    }
    if (!need) return arg;
    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == kBs) {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, kBs);
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, kBs);
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, kBs);
    out.push_back(L'"');
    return out;
}

std::wstring RenderTemplate(const std::wstring& tpl, const BuildInput& in,
                            const std::vector<std::wstring>& pathArgs, BuildError* err) {
    // 先按空格切成 token，再把 {placeholder} 展开成若干 argv 元素
    std::vector<std::wstring> tokens = SplitWhitespace(tpl);
    std::vector<std::wstring> out;
    for (const auto& tok : tokens) {
        if (tok == L"{paths}") {
            if (pathArgs.empty()) {
                if (err) { err->message = L"缺少本地路径"; err->field = "paths"; }
                return {};
            }
            out.insert(out.end(), pathArgs.begin(), pathArgs.end());
            continue;
        }
        if (tok == L"{flags}") continue;  // flags 在别处处理，占位只是为了排版
        if (!tok.empty() && tok.front() == L'{' && tok.back() == L'}') {
            std::string key = WideToUtf8(tok.substr(1, tok.size() - 2));
            auto it = in.params.find(key);
            if (it == in.params.end() || Trim(it->second).empty()) {
                if (err) {
                    err->message = Format(L"缺少参数：%ls", Utf8ToWide(key).c_str());
                    err->field = key.c_str();
                }
                return {};
            }
            out.push_back(Trim(it->second));
            continue;
        }
        out.push_back(tok);
    }
    return Join(out, L"\x1f");   // 用不可见分隔符串起来，再拆开，避免值里带空格被切碎
}

}  // namespace

std::wstring QuoteArg(const std::wstring& arg) { return QuoteArgImpl(arg); }

bool IsValidPackageName(std::wstring_view s) {
    if (s.empty() || s.size() > 255) return false;
    // 不允许首尾点，也不允许连续点（否则 "com..app" 会被当成合法包名）
    if (s.front() == L'.' || s.back() == L'.') return false;
    if (s.find(L"..") != std::wstring_view::npos) return false;
    bool dot = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'.') { dot = true; continue; }
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'$'))
            return false;
        if (i == 0 && !((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_'))
            return false;
    }
    return dot;
}

bool IsValidRemotePath(std::wstring_view s) {
    if (s.empty()) return false;
    if (s[0] != L'/') return false;
    for (wchar_t c : s)
        if (c == L'\'' || c == L'"' || c == L'\n' || c == L'\r' || c == 0) return false;
    return true;
}

bool IsValidHostPort(std::wstring_view s) {
    if (s.empty()) return false;
    std::wstring ip = std::wstring(s);
    int colon = -1;
    for (size_t i = 0; i < ip.size(); ++i)
        if (ip[i] == L':') colon = static_cast<int>(i);
    std::wstring host = colon < 0 ? ip : ip.substr(0, static_cast<size_t>(colon));
    if (host.empty()) return false;
    for (wchar_t c : host)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
              (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-'))
            return false;
    if (colon >= 0) {
        std::wstring port = ip.substr(static_cast<size_t>(colon) + 1);
        if (port.empty() || port.size() > 5) return false;
        for (wchar_t c : port)
            if (c < L'0' || c > L'9') return false;
    }
    return true;
}

bool IsValidPartitionName(std::wstring_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (wchar_t c : s) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L':' || c == L'.'))
            return false;
    }
    return true;
}

// 坐标类参数：只允许数字、负号与空格，且段数必须是 2 或 5
// （这一段会被透传到设备侧 shell，所以必须在这里把注入面堵死）
bool IsValidCoords(std::wstring_view s, size_t expectCount) {
    if (s.empty() || s.size() > 64) return false;
    int count = 0;
    bool inNumber = false;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') { inNumber = true; continue; }
        if (c == L'-') {
            if (inNumber) return false;
            inNumber = true;
            continue;
        }
        if (c == L' ') {
            if (!inNumber) return false;
            inNumber = false;
            ++count;
            continue;
        }
        return false;   // ; & | $ ` 等一律拒绝
    }
    if (inNumber) ++count;
    return static_cast<size_t>(count) == expectCount;
}

bool IsValidSerial(std::wstring_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'"' || c == L'\'') return false;
    }
    return true;
}

bool BuildCommand(const BuildInput& in, BuiltCommand* out, BuildError* err) {
    if (!out) return false;
    *out = BuiltCommand{};
    if (!in.spec) {
        if (err) err->message = L"内部错误：命令表项为空";
        return false;
    }
    const CommandSpec& spec = *in.spec;
    if (err) *err = BuildError{};

    if (spec.exec == ExecKind::Internal) {
        if (err) err->message = L"该命令由界面内部处理，不产生命令行";
        return false;
    }

    // ---- 参数 ----
    std::vector<std::wstring> pathArgs = in.paths;

    // ---- 模板渲染 ----
    std::wstring tpl = spec.argvTemplate ? Utf8ToWide(spec.argvTemplate) : std::wstring();
    std::vector<std::wstring> commandTexts;
    {
        // 多命令用 " ; " 分隔
        size_t start = 0;
        std::wstring rest = tpl;
        for (;;) {
            size_t sep = rest.find(L" ; ", start);
            commandTexts.push_back(rest.substr(start, sep == std::wstring::npos
                                                           ? std::wstring::npos : sep - start));
            if (sep == std::wstring::npos) break;
            start = sep + 3;
        }
    }

    // 坐标类参数前置校验（在模板渲染之前，避免脏值进入 argv）
    if (spec.param == ParamKind::Coords && spec.paramKey) {
        auto it = in.params.find(spec.paramKey);
        std::wstring v = (it == in.params.end()) ? std::wstring() : Trim(it->second);
        size_t want = (std::string(spec.paramKey) == "xys") ? 5 : 2;
        if (v.empty()) {
            if (err) { err->message = L"缺少坐标参数"; err->field = spec.paramKey; }
            return false;
        }
        if (!IsValidCoords(v, want)) {
            if (err) {
                err->message = Format(L"坐标只能是 %zu 个数字，用空格分隔（如 640 1200）", want);
                err->field = spec.paramKey;
            }
            return false;
        }
    }

    for (const auto& one : commandTexts) {
        std::wstring rendered = RenderTemplate(one, in, pathArgs, err);
        if (rendered.empty()) return false;

        std::vector<std::wstring> argv;
        size_t pos = 0;
        for (;;) {
            size_t sep = rendered.find(L'\x1f', pos);
            argv.push_back(rendered.substr(pos, sep == std::wstring::npos
                                                     ? std::wstring::npos : sep - pos));
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }

        // ---- flags：在模板里没有位置概念，统一插到 argv 的合适位置 ----
        // 约定：模板中的 {flags} 会被展开成"该命令适用的全部 flag"，位置在 token 序列里
        //       但 RenderTemplate 把 {flags} 丢掉了，所以这里按模板里 {flags} 的位置重建。
        std::vector<std::wstring> flagArgs;
        for (uint8_t i = 0; i < spec.flagCount; ++i) {
            const FlagSpec& f = spec.flags[i];
            auto it = in.flags.find(f.key);
            bool on = (it != in.flags.end() && it->second == L"1");
            if (f.kind == FlagKind::Value) {
                // 值型没有勾选框：填了值就进 argv
                if (it == in.flags.end()) continue;
                std::wstring v = Trim(it->second);
                if (v.empty()) continue;
                std::wstring arg = Utf8ToWide(f.adbArg ? f.adbArg : "");
                arg = ReplaceAll(arg, L"{v}", v);
                flagArgs.push_back(arg);
                continue;
            }
            if (!on) continue;
            std::wstring arg = Utf8ToWide(f.adbArg ? f.adbArg : "");
            if (f.kind == FlagKind::ToggleValue) {
                std::wstring v = it != in.flags.end() ? Trim(it->second) : L"";
                if (it != in.flags.end() && it->second != L"1" && it->second != L"0")
                    v = Trim(it->second);
                arg = v.empty() ? arg : ReplaceAll(arg, L"{v}", v);
                // ToggleValue 勾上但没值 → 用默认形态（adbArg 里若含 ={v} 要去掉）
                if (v.empty()) arg = ReplaceAll(arg, L"={v}", L"");
            }
            flagArgs.push_back(arg);
        }

        // 把 flag 插到模板里 {flags} 应该在的位置：找第一个以 "-" 开头之后的固定位置太难，
        // 因此采用**显式锚点**：argvTemplate 里若含 {flags}，flag 插在它出现的位置；
        // 否则插在 argv 开头（adb/fastboot 的子命令参数通常可以放最前面）。
        std::vector<std::wstring> tokens = SplitWhitespace(one);
        size_t insertAt = 0;
        bool anchored = false;
        size_t consumed = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == L"{flags}") { insertAt = consumed; anchored = true; break; }
            if (tokens[i] == L"{paths}") consumed += pathArgs.size();
            else consumed += 1;
        }
        if (!anchored) insertAt = argv.size();
        insertAt = std::min(insertAt, argv.size());
        argv.insert(argv.begin() + static_cast<std::ptrdiff_t>(insertAt),
                    flagArgs.begin(), flagArgs.end());

        // ---- 设备注入：所有 adb 命令都带上 -s <serial> ----
        if (spec.needsDevice && !spec.needsFastboot) {
            if (!in.serial.empty()) {
                argv.insert(argv.begin(), {L"-s", in.serial});
            }
        }
        if (spec.needsFastboot && !in.serial.empty()) {
            argv.insert(argv.begin(), {L"-s", in.serial});
        }
        out->argvList.push_back(argv);
    }

    out->display = BuildDisplayText(*out);
    return true;
}

std::wstring BuildDisplayText(const BuiltCommand& cmd) {
    std::wstring out;
    for (size_t i = 0; i < cmd.argvList.size(); ++i) {
        if (i) out += L"\r\n";
        for (size_t j = 0; j < cmd.argvList[i].size(); ++j) {
            if (j) out.push_back(L' ');
            out += QuoteArgImpl(cmd.argvList[i][j]);
        }
    }
    return out;
}

}  // namespace adb