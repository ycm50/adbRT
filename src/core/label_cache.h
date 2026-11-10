#pragma once
// ---------------------------------------------------------------------------
// 应用名称缓存
//   · 从设备拉取每个 APK 的 AndroidManifest.xml + resources.arsc（zip 里前几 MB）
//   · 用 apk_res 解析出 android:label
//   · 结果缓存在 %LOCALAPPDATA%\AdbRT\labels\<serial>.txt，第二次打开秒开
//
// 为什么不在设备上取：
//   dumpsys package 不输出 label；设备侧也没有 aapt。
//   主机的 build-tools 也不一定装。所以自己解析 APK 里的资源。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace adb {

struct AppLabel {
    std::wstring package;
    std::wstring label;     // 解析失败时为空
    bool         system = false;
};

// 缓存文件路径（按设备序列号区分）
std::wstring LabelCachePath(const std::wstring& serial);

// 读取缓存；返回 package → label
bool LoadLabelCache(const std::wstring& serial, std::vector<AppLabel>* out);

// 写入缓存
bool SaveLabelCache(const std::wstring& serial, const std::vector<AppLabel>& items);

// 解析单个 APK 的 label（本地文件路径）
std::wstring ReadApkLabel(const std::wstring& apkPath);

}  // namespace adb
