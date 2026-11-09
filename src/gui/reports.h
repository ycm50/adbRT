#pragma once
// ---------------------------------------------------------------------------
// 纯文本报告生成：GUI 窗口与 CLI 共用同一份实现
//   · GUI 把它塞进文本窗口 / 列表框
//   · CLI（--run device.info 等）直接打印，于是"界面内部命令"也能被脚本全量测试
// ---------------------------------------------------------------------------
#include <string>

namespace adbgui {

std::wstring BuildDeviceListText();
std::wstring BuildDeviceInfoText();
std::wstring BuildAppListText(bool thirdPartyOnly);
std::wstring BuildDoctorText();
std::wstring BuildAboutText();

}  // namespace adbgui
