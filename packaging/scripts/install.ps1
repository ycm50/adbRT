# ---------------------------------------------------------------------------
# AdbRT 安装脚本（不写注册表）
#
#   pwsh -File install.ps1 [-InstallDir <绝对或相对目录>] [-NoPath] [-NoDesktop] [-DryRun]
#
# 它做三件事：
#   1. 把 app\ 里的文件铺到安装目录（相对/绝对都支持）
#   2. 把安装目录写进「用户 PATH」（由 AdbRT.exe --install 完成，幂等）
#   3. 创建桌面快捷方式（由 AdbRT.exe --install 完成）
# 不创建、不修改、不删除任何注册表项。
#
# 不指定 -InstallDir 时，默认装到「脚本所在盘的根目录\adbRT」
# （脚本跟着发布包放在工作空间那一盘上，所以就是该盘根下的 adbRT 文件夹）。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$InstallDir = "",
    [switch]$NoPath,
    [switch]$NoDesktop,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$script:fail = 0

function Step([string]$text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Ok([string]$text)   { Write-Host "    [OK] $text" -ForegroundColor Green }
function Warn([string]$text) { Write-Host "    [!]  $text" -ForegroundColor Yellow }
function Fail([string]$text) { Write-Host "    [X]  $text" -ForegroundColor Red; $script:fail++ }

# 脚本所在目录要先算出来：默认安装位置就取它所在的那块盘的根
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    # 默认：<脚本所在盘>:\adbRT
    # 脚本随发布包放在工作空间的盘上，所以这就是"工作空间盘根目录下的 adbRT 文件夹"。
    $driveRoot = [System.IO.Path]::GetPathRoot($here)
    if ([string]::IsNullOrWhiteSpace($driveRoot)) {
        $driveRoot = [System.IO.Path]::GetPathRoot($PWD.Path)   # UNC 路径取不到盘符时退回当前盘
    }
    if ([string]::IsNullOrWhiteSpace($driveRoot)) {
        $InstallDir = $PWD.Path                                  # 连盘符都没有，只能装当前目录
    } else {
        $InstallDir = Join-Path $driveRoot "adbRT"
    }
    Write-Host "  未指定 -InstallDir，默认装到脚本所在盘的根目录：$InstallDir" -ForegroundColor Gray
}
if (-not [System.IO.Path]::IsPathRooted($InstallDir)) {
    $InstallDir = Join-Path $PWD.Path $InstallDir
}
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)

Write-Host ""
Write-Host "AdbRT 安装程序（不写注册表）" -ForegroundColor White
Write-Host "  安装目录：$InstallDir"
if ($DryRun) { Write-Host "  ** DRY RUN：不会修改任何东西 **" -ForegroundColor Yellow }
Write-Host ""

# 发布包里 install.ps1 与 app\ 同级；源码树里在 packaging\scripts\ 下
$appDir = Join-Path $here "app"
$root = $here
if (-not (Test-Path $appDir)) {
    $root = Split-Path -Parent (Split-Path -Parent $here)
    $appDir = Join-Path $root "app"
}
if (-not (Test-Path $appDir)) {
    $exeGuess = Join-Path $here "AdbRT.exe"
    if (Test-Path $exeGuess) { $appDir = $here; $root = $here }
    else { Fail "找不到 app 目录（已试过 $here\app 与 $root\app）"; exit 1 }
}

Step "检查源文件"
$exeSrc = Join-Path $appDir "AdbRT.exe"
if (-not (Test-Path $exeSrc)) { Fail "缺少 AdbRT.exe"; exit 1 }
Ok "找到 $exeSrc"

Step "准备安装目录"
if ($DryRun) {
    Ok "（dry run）将创建 $InstallDir"
} else {
    if (-not (Test-Path $InstallDir)) { New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null }
    Ok "目录就绪"
}

Step "停止正在运行的 AdbRT"
if (-not $DryRun) {
    Get-Process -Name "AdbRT" -ErrorAction SilentlyContinue | ForEach-Object {
        $_.Kill(); $_.WaitForExit(3000)
    }
}
Ok "完成"

Step "复制文件"
if ($DryRun) {
    Ok "（dry run）将复制 $appDir\* -> $InstallDir"
} else {
    # 装到发布包目录本身 / app 目录的子目录时，复制会变成"用自身覆盖项"
    # 或"把容器复制到自己的子容器"，两个都会中途报错并留下半成品。
    if ($InstallDir.TrimEnd("\") -eq $here.TrimEnd("\") -or $InstallDir -like "$appDir\*") {
        Fail "安装目录不能是发布包目录本身或 app 目录的子目录（请换个位置，或删掉 app 内容后重新解压）"
        exit 1
    }
    Copy-Item -Path (Join-Path $appDir "*") -Destination $InstallDir -Recurse -Force
    foreach ($name in @("README.md","LICENSE","uninstall.ps1")) {
        $p = Join-Path $root $name
        if (Test-Path $p) { Copy-Item $p $InstallDir -Force }
    }
    Ok "复制完成"
}

Step "写用户 PATH / 创建桌面快捷方式"
if ($DryRun) {
    Ok "（dry run）将调用 AdbRT.exe --install $InstallDir"
} else {
    $exe = Join-Path $InstallDir "AdbRT.exe"
    $exeArgs = @("--install", $InstallDir)
    if ($NoPath) { $exeArgs += "--no-path" }
    if ($NoDesktop) { $exeArgs += "--no-desktop" }
    $p = Start-Process -FilePath $exe -ArgumentList $exeArgs -Wait -PassThru
    if ($p.ExitCode -eq 0) { Ok "已写入用户 PATH / 已建快捷方式" } else { Fail "AdbRT.exe --install 退出码 $($p.ExitCode)" }
}

Write-Host ""
if ($script:fail -eq 0) {
    Write-Host "安装完成。" -ForegroundColor Green
    Write-Host "  · 没有写入任何注册表项"
    Write-Host "  · 用户 PATH 已更新（新开的终端才生效）"
    Write-Host "  · 卸载：pwsh -File `"$InstallDir\uninstall.ps1`""
    exit 0
} else {
    Write-Host "安装过程中有 $script:fail 个问题。" -ForegroundColor Red
    exit 1
}