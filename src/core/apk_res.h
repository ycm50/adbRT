#pragma once
// ---------------------------------------------------------------------------
// APK 资源解析：从二进制 AndroidManifest.xml + resources.arsc 里取"应用名称"
//
// 为什么要自己写：
//   · 主机上通常没有 aapt / aapt2（build-tools 单独安装）
//   · 设备上的 dumpsys package **不输出**界面名称（label 在资源里）
//   · 这两个格式是稳定且有文档的，自己解析 ≈ 300 行，不引入任何依赖
//
// 只做一件事：给定 APK 里的 AndroidManifest.xml 与 resources.arsc 字节，
// 返回 android:label 的人类可读值（取不到就返回空串）。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace adb {

// manifest 里 android:label 的解析结果
struct ManifestLabel {
    bool         hasLiteral = false;   // true = manifest 里直接写了字符串
    std::wstring literal;              // hasLiteral 时有效
    uint32_t     resId = 0;            // 否则是资源引用（0x7fxxxxxx）
};

// 解析二进制 AndroidManifest.xml（AXML），找 <application android:label=...>
bool ParseManifestLabel(const std::vector<uint8_t>& manifest, ManifestLabel* out);

// 在 resources.arsc 里按 resId 查字符串值
bool LookupStringResource(const std::vector<uint8_t>& arsc, uint32_t resId, std::wstring* out,
                          int depth = 0);

// 两步合一：先看 manifest，是字面量就直接返回，是引用就查 arsc
std::wstring ResolveAppLabel(const std::vector<uint8_t>& manifest,
                             const std::vector<uint8_t>& arsc);

}  // namespace adb