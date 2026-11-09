// ---------------------------------------------------------------------------
// fake-adb：给端到端测试用的 adb 桩
//   · 输出形状与真实 adb 一致，用来验证"无设备也能测"的链路
//   · 用法：fake-adb.exe devices -l / shell <cmd> / logcat ...
// ---------------------------------------------------------------------------
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    auto has = [&](const wchar_t* s) {
        for (const auto& a : args) if (a == s) return true;
        return false;
    };
    auto startsWith = [&](const wchar_t* s) {
        if (args.empty()) return false;
        return args[0] == s;
    };

    if (startsWith(L"version")) {
        std::printf("Android Debug Bridge version 1.0.41\nVersion 36.0.2-fake\n");
        return 0;
    }
    if (startsWith(L"devices")) {
        if (has(L"-l")) {
            std::printf("List of devices attached\n"
                        "FAKE0001\tdevice product:fake model:FakePhone device:fake transport_id:1\n"
                        "FAKE0002\toffline\n");
        } else {
            std::printf("List of devices attached\nFAKE0001\tdevice\nFAKE0002\toffline\n");
        }
        return 0;
    }
    if (startsWith(L"shell")) {
        std::wstring cmd = args.size() > 1 ? args[1] : L"";
        if (cmd.find(L"getprop ro.product.model") != std::wstring::npos) {
            std::printf("FakePhone\n");
        } else if (cmd.find(L"getprop") != std::wstring::npos) {
            std::printf("fake-value\n");
        } else if (cmd.find(L"wm size") != std::wstring::npos) {
            std::printf("Physical size: 1080x2400\n");
        } else if (cmd.find(L"pm list packages") != std::wstring::npos) {
            std::printf("package:com.example.alpha\npackage:com.example.beta\npackage:com.example.gamma\n");
        } else if (cmd.find(L"pm path") != std::wstring::npos) {
            std::printf("package:/data/app/~~x/com.example.alpha/base.apk\n");
        } else if (cmd.find(L"ls -1") != std::wstring::npos) {
            std::printf("Download\nDCIM\nMusic\n");
        } else if (cmd.find(L"ls -1d") != std::wstring::npos) {
            std::printf("/sdcard/Download\n");
        } else if (cmd.find(L"screencap") != std::wstring::npos) {
            return 0;
        } else if (cmd.find(L"mkdir") != std::wstring::npos) {
            return 0;
        } else {
            std::printf("fake-shell-ok\n");
        }
        return 0;
    }
    if (startsWith(L"logcat")) {
        for (int i = 0; i < 5; ++i) {
            std::printf("01-01 00:00:0%d.000  1234  1234 I FakeTag : line %d\n", i, i);
            std::fflush(stdout);
        }
        return 0;
    }
    if (startsWith(L"install") || startsWith(L"uninstall") || startsWith(L"pull") ||
        startsWith(L"push") || startsWith(L"reboot")) {
        std::printf("Success\n");
        return 0;
    }
    std::printf("fake-adb: unhandled\n");
    return 0;
}
