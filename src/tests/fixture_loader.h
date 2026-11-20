#pragma once
// 测试样本：最小可用的 AndroidManifest.xml（AXML）与 resources.arsc
// 由 tmpscripts/mkfixture.py 生成，放进仓库是为了让标签解析器有无设备可测的固定输入。
//   · manifest 里 <application android:label="@string/...">，资源 id = 0x7f010000
//   · arsc 里 0x7f010000 → "AdbRT 测试应用"

#include <cstdint>
#include <string>
#include <vector>

namespace adbtest {

// 从源文件同目录读取样本（CMake 会把它们拷到可执行文件旁边）
bool LoadFixture(const wchar_t* fileName, std::vector<uint8_t>* out);

}  // namespace adbtest