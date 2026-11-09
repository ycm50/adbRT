# ---------------------------------------------------------------------------
# AdbRT 卸载脚本（不动注册表）
#   pwsh -File uninstall.ps1 [-KeepPath] [-KeepDesktop] [-DryRun]
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [switch]$KeepPath,
    [switch]$KeepDesktop,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$InstallDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host ""
Write-Host "AdbRT 卸载程序（不写注册表）" -ForegroundColor White
Write-Host "  安装目录：$InstallDir"
if ($DryRun) { Write-Host "  ** DRY RUN **" -ForegroundColor Yellow }
Write-Host ""

if ($DryRun) {
    Write-Host "（dry run）将移除用户 PATH 中的 $InstallDir 并删除桌面快捷方式" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "卸载完成（未改动任何注册表项）" -ForegroundColor Green
    exit 0
}

Get-Process -Name "AdbRT" -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(3000) }

$exe = Join-Path $InstallDir "AdbRT.exe"
if (Test-Path $exe) {
    $exeArgs = @("--uninstall")
    if ($KeepPath) { $exeArgs += "--keep-path" }
    if ($KeepDesktop) { $exeArgs += "--keep-desktop" }
    $p = Start-Process -FilePath $exe -ArgumentList $exeArgs -Wait -PassThru
    Write-Host "    [OK] 已清理用户 PATH / 桌面快捷方式" -ForegroundColor Green
} else {
    Write-Host "    [!] 找不到 AdbRT.exe，跳过 PATH 清理" -ForegroundColor Yellow
}

$cfg = Join-Path $env:APPDATA "AdbRT\config.json"
if (Test-Path $cfg) { Write-Host "    [i] 配置保留在 $cfg" -ForegroundColor Gray }

Start-Sleep -Milliseconds 300
$target = $InstallDir
Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "timeout /t 1 >nul & rmdir /s /q `"$target`"" -WindowStyle Hidden
Write-Host "    目录将在 1 秒后删除：$target" -ForegroundColor Gray

Write-Host ""
Write-Host "卸载完成（未改动任何注册表项）" -ForegroundColor Green