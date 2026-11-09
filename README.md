# AdbRT

> 把 `adb` / `fastboot` 那些记不住的命令，放回你**已经在的地方** —— Windows 的图形界面。
> 不用背参数：选设备 → 选命令 → 勾选项 → 看真实命令行 → 执行、看实时输出。

**C++20 + Win32，零第三方依赖**（MSYS2 UCRT64 / MinGW-w64，CMake + Ninja）。

---

## 它能做什么

| 能力 | 状态 |
| --- | --- |
| **设备栏** | 列出在线设备（`adb devices -l`），支持多设备与"当前设备"，状态机识别 device/offline/unauthorized/recovery/sideload/bootloader | ✅ |
| **声明式命令表** | 40+ 条 adb / fastboot 命令全是数据表项；菜单、参数面板、CLI 共用同一份定义 | ✅ |
| **参数面板 + 实时命令行预览** | 每条命令的复选/值型参数（中文标签）；执行前就能看到**真实 argv**（深色代码块），一键复制 | ✅ |
| **重做的主界面** | 顶栏（图标/设备/刷新/专家开关/设置/关于）+ 左树右卡两栏 + 危险徽标（安全·谨慎·破坏性·危险）+ 搜索过滤 + F5 刷新 + Ctrl+F 搜索 + 回车执行 | ✅ |
| **流式执行** | 输出逐行回传、可取消（Job object 保证不留孤儿进程），push/pull 解析进度百分比 | ✅ |
| **安装/卸载：不写注册表** | 只做两件事：把安装目录加进**用户 PATH**、建**桌面快捷方式** | ✅ |
| **路径：相对目录 + 绝对目录** | `adb.path` 可写绝对路径、相对程序目录的路径，或带 `%环境变量%` | ✅ |
| **图标** | 矢量母版 `packaging/assets/adbrt.svg`，同一份几何定义光栅化出 PNG / ICO | ✅ |
| 无窗口自检 / 单元测试 | `--selftest`（25 项）+ `AdbRT.CoreTests`（147 项），都不需要真机 | ✅ |
| **设置窗口** | 路径可编辑/浏览（相对、绝对、%环境变量%）、专家模式开关、用户 PATH 与桌面快捷方式勾选 | ✅ |
| **设备列表 / 设备信息** | 设备清单（含 offline/unauthorized 解释）、getprop + wm size/density + df + meminfo | ✅ |
| **已安装应用 / 导出 APK** | 第三方应用列表 + 过滤；选中即从 \`pm path\` 拉取 APK | ✅ |
| **设备文件浏览器** | 目录进出、拉取到本地 | ✅ |
| **截屏** | 设备侧 screencap → pull → 自动打开 | ✅ |
| **实时 logcat** | 流式刷新（≤2 帧/秒）、关键词过滤、暂停/继续、保存到文件 | ✅ |
| **交互终端** | 新开 cmd 窗口直连 \`adb -s <serial> shell\` | ✅ |
| **Breaker 级确认** | fastboot 刷写/擦除/解锁需**手打设备序列号**才放行 | ✅ |

---

## 快速开始

### 用发布包装

```powershell
pwsh -File install.ps1                                   # 装到当前目录
pwsh -File install.ps1 -InstallDir D:\tools\AdbRT        # 指定位置（绝对或相对都行）
pwsh -File install.ps1 -NoPath                           # 不动用户 PATH
pwsh -File install.ps1 -NoDesktop                        # 不建桌面快捷方式
pwsh -File install.ps1 -DryRun                           # 只看会做什么

pwsh -File "<安装目录>\uninstall.ps1"                     # 卸载
```

安装器**不创建、不修改、不删除任何注册表项**。唯二改动的系统状态是：

1. 用户环境变量 `PATH`（加入安装目录，卸载时移除）—— 这样在新终端里直接敲 `AdbRT` 就能启动；
2. 桌面上的 `AdbRT.lnk` 快捷方式。

### 从源码构建

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=A:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_RC_COMPILER=A:/msys64/ucrt64/bin/windres.exe
cmake --build build/release

# 测试
build\release\src\tests\AdbRT.CoreTests.exe
build\release\src\gui\AdbRT.exe --selftest selftest.txt

# 打包
pwsh -File packaging\scripts\build-release.ps1 -Config Release
```

---

## 配置：相对目录与绝对目录

配置在 `%APPDATA%\AdbRT\config.json`（扁平键值表，未知键原样保留）。

路径类配置**三种写法都支持**：

| 写法 | 例子 | 解析方式 |
| --- | --- | --- |
| 绝对路径 | `A:\platform-tools\adb.exe` | 原样 |
| 相对路径 | `.\platform-tools\adb.exe` 或 `platform-tools\adb.exe` | **相对于 AdbRT.exe 所在目录** |
| 环境变量 | `%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe` | 先展开变量再解析 |
| 波浪号 | `~\platform-tools\adb.exe` | 等价于 `.\platform-tools` |

```jsonc
{
  "adb.path": ".\\platform-tools\\adb.exe",
  "fastboot.path": "%LOCALAPPDATA%\\Android\\Sdk\\platform-tools\\fastboot.exe",
  "device.current": "3B661M00VQ000000",
  "expert.fastboot": false,
  "ui.defaultRemoteDir": "/sdcard/Download"
}
```

没填时按顺序自动探测：`<exe目录>\platform-tools` → `<exe目录>` → 用户 `PATH` →
`%ANDROID_HOME%` → `%ANDROID_SDK_ROOT%` → `%LOCALAPPDATA%\Android\Sdk\platform-tools`。

---

## 项目结构

```
src/core/      命令表、argv 构造、进程封装、adb 输出解析、设备会话、配置、日志
src/gui/       Win32 GUI：主窗口、参数面板、任务执行、自检、安装/卸载命令
src/tests/     无窗口核心单元测试（CTest）
cmake/         身份与版本派生（唯一真相源 packaging/identity.json）
packaging/     identity.json、assets（svg/png/ico）、install/uninstall/build-release 脚本
tools/         gen-icon.mjs（SVG → PNG/ICO 生成器）
.github/       手动触发的构建 + 打包 + 发布流水线
```

## 设计要点

- **argv 只有一个来源**：`command_table.cpp` 声明式命令表 → `command_builder`，
  参数面板、CLI、未来的 AI 助手都走同一条路，永远不拼 shell 字符串。
- **UI 线程零阻塞**：所有 adb 调用都在工作线程，通过 `PostMessage` 回传输出行。
- **取消是真的取消**：子进程挂在 Job object 上，`TerminateJobObject` 连同子进程树一起结束。
- **可测性是设计出来的**：解析器全是纯函数，涉及进程的逻辑都有 `--dry-run` 式的 argv 断言。

## 许可证

MIT