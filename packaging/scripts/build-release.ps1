# ---------------------------------------------------------------------------
# 组装发布包：build\dist\AdbRT-<版本>-win-x64.zip
#   pwsh -File packaging\scripts\build-release.ps1 -Config Release
# 产物结构：
#   app\AdbRT.exe  app\icons\...  install.ps1  uninstall.ps1  使用说明.txt  README.md  LICENSE
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet("Debug","Release")][string]$Config = "Release",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
# 脚本在 packaging\scripts\ 下，仓库根是它上面第 3 层
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $root "build" }

$identityPath = Join-Path $root "packaging\identity.json"
if (-not (Test-Path $identityPath)) { throw "缺少 packaging\identity.json" }
$identity = Get-Content -Raw $identityPath | ConvertFrom-Json
$version = $identity.version

$cfgLower = $Config.ToLower()
$exe = Join-Path $BuildDir "$cfgLower\src\gui\AdbRT.exe"
if (-not (Test-Path $exe)) { throw "找不到 $exe，先构建 $Config" }

$dist = Join-Path $BuildDir "dist"
$stage = Join-Path $dist "AdbRT-$version-win-x64"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "app\icons") | Out-Null

Write-Host "==> 归集 $Config 产物" -ForegroundColor Cyan
Copy-Item $exe (Join-Path $stage "app\AdbRT.exe") -Force
$assets = Join-Path $root "packaging\assets"
foreach ($f in @("adbrt.ico","adbrt.png","adbrt.svg")) {
    $p = Join-Path $assets $f
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage "app") -Force }
}
foreach ($f in @("install.ps1","uninstall.ps1")) {
    Copy-Item (Join-Path $root "packaging\scripts\$f") $stage -Force
}
foreach ($f in @("README.md","LICENSE")) {
    $p = Join-Path $root $f
    if (Test-Path $p) { Copy-Item $p $stage -Force }
}

$readme = @"
AdbRT $version —— Android 平台工具（adb / fastboot）的 Windows 图形外壳

安装（不会写注册表）：
    pwsh -File install.ps1
    pwsh -File install.ps1 -InstallDir D:\tools\AdbRT    # 指定位置（绝对或相对都行）
    pwsh -File install.ps1 -NoPath                        # 不改用户 PATH

卸载：
    pwsh -File "<安装目录>\uninstall.ps1"

配置：%APPDATA%\AdbRT\config.json（adb.path 支持绝对路径 / 相对程序目录 / 环境变量）
"@
$readme | Out-File -FilePath (Join-Path $stage "使用说明.txt") -Encoding utf8

Write-Host "==> 打 zip" -ForegroundColor Cyan
$zip = Join-Path $dist "AdbRT-$version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host ""
Write-Host "产出：" -ForegroundColor Green
Write-Host "  $zip  ($([math]::Round((Get-Item $zip).Length / 1KB, 1)) KB)"
Write-Host "  暂存目录：$stage"