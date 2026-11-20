# AdbRT 全量真机测试：对已连接的 adb 设备逐条跑命令表
param([string]$Serial = "", [string]$Out = "A:\Downloads\tmpscripts\T-results.txt")

$ErrorActionPreference = "Continue"
$exe = "A:\Downloads\adbRT\build\release\src\gui\AdbRT.exe"
$env:Path = "A:\platform-tools;" + $env:Path

if ([string]::IsNullOrWhiteSpace($Serial)) {
    $line = (& adb devices -l | Select-String " device ") | Select-Object -First 1
    if (-not $line) { Write-Host "no online device" -ForegroundColor Red; exit 1 }
    $Serial = ($line.ToString() -split "\s+")[0]
}
Write-Host "target device: $Serial" -ForegroundColor Cyan

$script:results = New-Object System.Collections.ArrayList
$script:pass = 0
$script:fail = 0

function Run-Case {
    param([string]$Name, [string[]]$CliArgs, [int]$Expect = 0)

    $tmp = Join-Path $env:TEMP "adbrt-case.txt"
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue

    # 用临时响应/参数文件由 cmd 调起，彻底绕开 PowerShell 的引号规则
    $argList = @()
    foreach ($a in $CliArgs) { $argList += $a }
    $argList += @("--serial", $Serial, "--out", $tmp)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($a in $argList) { [void]$psi.ArgumentList.Add($a) }
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.WaitForExit()
    $code = $proc.ExitCode

    $body = if (Test-Path $tmp) { Get-Content $tmp -Raw -Encoding utf8 } else { "" }
    $ok = ($code -eq $Expect)
    if ($ok) { $script:pass++ } else { $script:fail++ }
    $tag = if ($ok) { "PASS" } else { "FAIL" }
    $line = "[{0}] {1,-24} exit={2}" -f $tag, $Name, $code
    Write-Host $line -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
    [void]$script:results.Add("=== $Name ===")
    [void]$script:results.Add($line)
    if ($body) { [void]$script:results.Add((($body.Trim()) -split "`r?`n" | Select-Object -First 10) -join "`n") }
    [void]$script:results.Add("")
    return $body
}

[void]$script:results.Add("AdbRT full real-device test  device=$Serial  $(Get-Date -Format o)")
[void]$script:results.Add("")

Run-Case "device.list"    @("--run","device.list")   | Out-Null
Run-Case "device.select"  @("--run","device.select") | Out-Null
Run-Case "device.info"    @("--run","device.info")   | Out-Null
Run-Case "tool.doctor"    @("--run","tool.doctor")   | Out-Null
Run-Case "tool.about"     @("--run","tool.about")    | Out-Null
Run-Case "app.list"       @("--run","app.list")      | Out-Null

Run-Case "shell.getprop"  @("--run","shell.getprop","--param","prop=ro.product.model") | Out-Null
Run-Case "shell.run"      @("--run","shell.run","--param","cmd=echo hello-adbrt") | Out-Null
Run-Case "logcat.dump"    @("--run","logcat.dump") | Out-Null

$pkgs = Run-Case "pm list -3" @("--run","shell.run","--param","cmd=pm list packages -3")
$pkg = "com.android.settings"
if ($pkgs -match "package:([\w\.]+)") { $pkg = $matches[1] }
Write-Host "target package: $pkg" -ForegroundColor DarkGray
Run-Case "app.forceStop"  @("--run","app.forceStop","--param","pkg=$pkg") | Out-Null
Run-Case "app.start"      @("--run","app.start","--param","pkg=$pkg") | Out-Null
Run-Case "pm path"        @("--run","shell.run","--param","cmd=pm path $pkg") | Out-Null

$src = Join-Path $env:TEMP "adbrt-push.txt"
"hello from adbrt" | Set-Content -Encoding utf8 $src
$pullDir = Join-Path $env:TEMP "adbrt-pull"
New-Item -ItemType Directory -Force -Path $pullDir | Out-Null
Remove-Item (Join-Path $pullDir "*") -Force -ErrorAction SilentlyContinue

Run-Case "file.mkdir"     @("--run","file.mkdir","--param","remote=/sdcard/adbrt-test") | Out-Null
Run-Case "file.push"      @("--run","file.push","--param","remote=/sdcard/adbrt-test",$src) | Out-Null
Run-Case "file.push(sync)" @("--run","file.push","--param","remote=/sdcard/adbrt-test","--flag","sync=1",$src) | Out-Null
Run-Case "verify pushed"  @("--run","shell.run","--param","cmd=ls -l /sdcard/adbrt-test") | Out-Null
Run-Case "file.pull"      @("--run","file.pull","--param","remote=/sdcard/adbrt-test/adbrt-push.txt","--param","local=$pullDir") | Out-Null
$pulled = Test-Path (Join-Path $pullDir "adbrt-push.txt")
Write-Host "pull landed on disk: $pulled" -ForegroundColor $(if ($pulled) { "Green" } else { "Red" })
if (-not $pulled) { $script:fail++ } else { $script:pass++ }

Run-Case "input.swipe" @("--run","input.swipe","--param","xys=640 1600 640 800","--param","dur=300") | Out-Null
Run-Case "input.tap"      @("--run","input.tap","--param","xy=640 1200") | Out-Null
Run-Case "input.swipe"    @("--run","input.swipe","--param","xys=640 1600 640 800 300") | Out-Null
Run-Case "input.text"     @("--run","input.text","--param","text=adbrt") | Out-Null
Run-Case "screencap"      @("--run","shell.run","--param","cmd=screencap -p /sdcard/adbrt-test/shot.png") | Out-Null
Run-Case "screen.record(3s)" @("--run","screen.record","--flag","timelimit=3") | Out-Null

Run-Case "shell.getprop"  @("--run","shell.getprop","--param","prop=ro.product.model") | Out-Null

[void]$script:results.Add("----------------------------------------")
[void]$script:results.Add("pass $($script:pass) fail $($script:fail)")
$script:results | Set-Content -Encoding utf8 $Out
Write-Host ""
Write-Host "pass $($script:pass)  fail $($script:fail)" -ForegroundColor $(if ($script:fail -eq 0) { "Green" } else { "Red" })
exit $(if ($script:fail -eq 0) { 0 } else { 1 })