#include "command_builder.h"

#include <algorithm>

namespace adb {

namespace {

constexpr wchar_t kBs = 0x5c;      // 反斜杠
constexpr wchar_t kTick = 0x60;    // 反引号

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
            std::wstring name = tok.substr(1, tok.size() - 2);
            bool optional = !name.empty() && name.back() == L'?';
            if (optional) name.pop_back();
            std::string key = WideToUtf8(name);
            auto it = in.params.find(key);
            if (it == in.params.end() || Trim(it->second).empty()) {
                if (optional) continue;   // {key?}：没填就整段消失
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

bool HasShellMeta(std::wstring_view s) {
    // 这些字符在设备侧 shell 里有特殊含义；凡是最终会被拼进 "adb shell <cmd>"
    // 的参数都必须先把它们挡掉（否则等于把注入面交给用户）
    for (wchar_t c : s) {
        switch (c) {
            case L';': case L'&': case L'|': case L'$': case kTick:
            case L'(': case L')': case L'<': case L'>':
            case L'\n': case L'\r': case L'"': case L'\'':
                return true;
            default: break;
        }
    }
    return false;
}

bool IsAllDigits(std::wstring_view s) {
    if (s.empty() || s.size() > 9) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    return true;
}

bool IsValidRemotePath(std::wstring_view s) {
    if (s.empty() || s.size() > 512) return false;
    if (s[0] != L'/') return false;
    if (s.find(L"..") != std::wstring_view::npos) return false;   // 不爬上级目录
    return !HasShellMeta(s);
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

// 坐标类参数：只允许数字、负号与空格
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
        return false;   // ; & | $ 等一律拒绝
    }
    if (inNumber) ++count;
    return static_cast<size_t>(count) == expectCount;
}

// 多值参数：包名 / 属性名可以用 ";" 或换行分隔，构造时展开成多条命令
std::vector<std::wstring> SplitMultiValue(const std::wstring& v) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : v) {
        if (c == L';' || c == L'\n' || c == L'\r') {
            std::wstring t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::wstring t = Trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
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

    // ---- 参数清洗（必须在模板渲染之前，脏值绝不进 argv） ----
    BuildInput work = in;
    auto paramOf = [&](const char* key) -> std::wstring {
        if (!key) return {};
        auto it = work.params.find(key);
        return it == work.params.end() ? std::wstring() : Trim(it->second);
    };
    if (spec.paramKey) {
        std::wstring v = paramOf(spec.paramKey);
        const bool remote = (spec.param == ParamKind::RemotePath ||
                            spec.param == ParamKind::RemotePick ||
                            spec.param == ParamKind::RemoteDyn);
        if (remote && !v.empty() && !IsValidRemotePath(v)) {
            if (err) {
                err->message = L"设备路径不合法：必须以 / 开头，且不能含 ; & | $ 或 ..";
                err->field = spec.paramKey;
            }
            return false;
        }
        if (spec.param == ParamKind::Text && std::string(spec.key) == "input.text" && !v.empty()) {
            if (HasShellMeta(v)) {
                if (err) {
                    err->message = L"输入文本不能包含 ; & | $ 等 shell 特殊字符";
                    err->field = spec.paramKey;
                }
                return false;
            }
            work.params[spec.paramKey] = ReplaceAll(v, L" ", L"%s");   // input text 的空格写法
        }
    }
    if (spec.param2 == ParamKind::Duration && spec.paramKey2) {
        std::wstring d = paramOf(spec.paramKey2);
        if (!d.empty() && !IsAllDigits(d)) {
            if (err) { err->message = L"滑动时长只能是毫秒数字（如 300）"; err->field = spec.paramKey2; }
            return false;
        }
    }

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
        std::wstring v = paramOf(spec.paramKey);
        const bool swipe = (std::string(spec.paramKey) == "xys");
        if (v.empty()) {
            if (err) { err->message = L"缺少坐标参数"; err->field = spec.paramKey; }
            return false;
        }
        // 点击 = 2 段；滑动 = 4 段（时长走另一个可选参数），也接受把时长写进坐标的 5 段写法
        const bool okLen = swipe ? (IsValidCoords(v, 4) || IsValidCoords(v, 5))
                                 : IsValidCoords(v, 2);
        if (!okLen) {
            if (err) {
                err->message = swipe
                    ? L"滑动坐标必须是 4 或 5 个数字，用空格分隔（如 640 1200 640 400 300）"
                    : L"点击坐标必须是 2 个数字，用空格分隔（如 640 1200）";
                err->field = spec.paramKey;
            }
            return false;
        }
    }

    std::vector<std::wstring> pathArgs = work.paths;

    for (const auto& one : commandTexts) {
        std::wstring rendered = RenderTemplate(one, work, pathArgs, err);
        if (rendered.empty()) return false;

        std::vector<std::wstring> argv;
        size_t pos = 0;
        for (;;) {
            size_t sep = rendered.find(0x1f, pos);
            argv.push_back(rendered.substr(pos, sep == std::wstring::npos
                                                     ? std::wstring::npos : sep - pos));
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }

        // ---- flags ----
        std::vector<std::wstring> flagArgs;
        for (uint8_t i = 0; i < spec.flagCount; ++i) {
            const FlagSpec& f = spec.flags[i];
            auto it = work.flags.find(f.key);
            bool on = (it != work.flags.end() && it->second == L"1");
            if (f.kind == FlagKind::Value) {
                // 值型没有勾选框：填了值就进 argv
                if (it == work.flags.end()) continue;
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
                std::wstring v = it != work.flags.end() ? Trim(it->second) : L"";
                if (it != work.flags.end() && it->second != L"1" && it->second != L"0")
                    v = Trim(it->second);
                arg = v.empty() ? arg : ReplaceAll(arg, L"{v}", v);
                // ToggleValue 勾上但没值 → 用默认形态（adbArg 里若含 ={v} 要去掉）
                if (v.empty()) arg = ReplaceAll(arg, L"={v}", L"");
            }
            flagArgs.push_back(arg);
        }

        // 把 flag 插到模板里 {flags} 应该在的位置（显式锚点，避免依赖子命令顺序）
        std::vector<std::wstring> tokens = SplitWhitespace(one);
        size_t insertAt = 0;
        bool anchored = false;
        size_t consumed = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == L"{flags}") { insertAt = consumed; anchored = true; break; }
            if (tokens[i] == L"{paths}") { consumed += pathArgs.size(); continue; }
            // 可选占位 {key?} 没填值时不会产出 argv 元素，锚点计数要跟着跳过
            if (tokens[i].size() > 3 && tokens[i].front() == L'{' && tokens[i].back() == L'}') {
                std::wstring nm = tokens[i].substr(1, tokens[i].size() - 2);
                if (!nm.empty() && nm.back() == L'?') {
                    nm.pop_back();
                    auto it = work.params.find(WideToUtf8(nm));
                    if (it == work.params.end() || Trim(it->second).empty()) continue;
                }
            }
            consumed += 1;
        }
        if (!anchored) insertAt = argv.size();
        insertAt = std::min(insertAt, argv.size());
        argv.insert(argv.begin() + static_cast<std::ptrdiff_t>(insertAt),
                    flagArgs.begin(), flagArgs.end());

        // ---- 设备注入：所有 adb 命令都带上 -s <serial> ----
        if (spec.needsDevice && !spec.needsFastboot) {
            if (!work.serial.empty()) argv.insert(argv.begin(), {L"-s", work.serial});
        }
        if (spec.needsFastboot && !work.serial.empty())
            argv.insert(argv.begin(), {L"-s", work.serial});

        // ---- 多值展开：一条命令变成多条（每条只带一个包 / 一个属性） ----
        std::vector<std::wstring> multiValues;
        std::wstring multiRaw;
        const bool multi = (spec.param == ParamKind::PackageName ||
                            spec.param == ParamKind::PropKeys);
        if (multi && spec.paramKey) {
            auto it = work.params.find(spec.paramKey);
            if (it != work.params.end()) {
                multiRaw = Trim(it->second);
                multiValues = SplitMultiValue(it->second);
            }
        }

        if (multiValues.size() > 1) {
            for (size_t pi = 0; pi < multiValues.size(); ++pi) {
                std::vector<std::wstring> single = argv;
                for (auto& a : single) {
                    if (a == multiRaw) { a = multiValues[pi]; break; }
                }
                out->argvList.push_back(single);
            }
        } else {
            out->argvList.push_back(argv);
        }
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