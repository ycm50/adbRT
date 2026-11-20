# ---------------------------------------------------------------------------
# 把 AdbRT.exe 依赖的非系统 DLL 收集到指定目录，并校验导入符号全部可解析
#
#   pwsh -File collect-runtime-deps.ps1 -Exe <exe 路径> -Out <输出目录> [-ToolBin <工具链 bin>]
#
# 为什么要有这一步：
#   MSYS2 UCRT64 默认动态链接 libstdc++-6.dll / libgcc_s_seh-1.dll /
#   libwinpthread-1.dll。这三个 Windows 不自带，而用户 PATH 里往往躺着别人的
#   （更旧或更新的）同名副本 —— 版本对不上时 exe 连入口点都找不到，直接
#   0xC0000139 起不来。所以发布包必须把「本次构建用的那几个 DLL」原样带上，
#   放 exe 旁边（同目录优先于 PATH 搜索）。
#
# 只复制非系统 DLL：System32 里已有的（含 api-ms-win-crt-*）一律交给操作系统。
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Out,
    # 含 objdump.exe 的目录；留空则依次从 PATH、UCRT_PATH 里找
    [string]$ToolBin = ""
)

$ErrorActionPreference = "Stop"

function Step([string]$t) { Write-Host "==> $t" -ForegroundColor Cyan }
function Ok([string]$t)   { Write-Host "    [OK] $t" -ForegroundColor Green }
function Note([string]$t) { Write-Host "    [i]  $t" -ForegroundColor Gray }

function Stop-Here([string]$text) {
    Write-Host "    [X]  $text" -ForegroundColor Red
    exit 1
}

# ------------------------------------------------------------------ 找 objdump
$objdump = $null
if ($ToolBin) {
    $cand = Join-Path $ToolBin "objdump.exe"
    if (Test-Path $cand) { $objdump = (Resolve-Path $cand).Path }
}
if (-not $objdump) {
    $cmd = Get-Command "objdump.exe" -ErrorAction SilentlyContinue
    if ($cmd) { $objdump = $cmd.Source }
}
if (-not $objdump) {
    Stop-Here "找不到 objdump.exe：用 -ToolBin 指定工具链 bin 目录，或者把它放进 PATH"
}

# --------------------------------------------------- 解析依赖时去哪找同名 DLL
# 顺序很关键：工具链目录排第一，保证收上来的是「跟这次构建配套」的那一份。
$searchDirs = New-Object System.Collections.Generic.List[string]
function Add-Dir([string]$d) {
    if (-not $d) { return }
    $d = $d.Trim().TrimEnd("\")
    if (-not $d) { return }
    if (-not (Test-Path $d)) { return }
    foreach ($e in $searchDirs) { if ($e -eq $d) { return } }
    $searchDirs.Add($d)
}
if ($ToolBin) { Add-Dir $ToolBin }
Add-Dir (Split-Path -Parent $objdump)
if ($env:UCRT_PATH) { Add-Dir $env:UCRT_PATH }
foreach ($p in ($env:PATH -split ";")) { Add-Dir $p }

# ------------------------------------------------------------------ PE 读取
# 导入表：@{ DLL 名 -> List[符号名] }
function Get-ImportMap([string]$path) {
    $map = @{}
    $cur = ""
    foreach ($line in (& $objdump -p $path)) {
        if ($line -match 'DLL Name: (.+)$') {
            $cur = $matches[1].Trim()
            $map[$cur] = New-Object System.Collections.Generic.List[string]
            continue
        }
        if ($cur -and $line -match '<none>') {
            $m = [regex]::Match($line, '^\s+\w+\s+<none>\s+\w+\s+(.+?)\s*$')
            if ($m.Success) { $map[$cur].Add($m.Groups[1].Value) }
        }
    }
    return $map
}

function Test-SystemDll([string]$name) {
    if ($name -like "api-ms-win-*") { return $true }
    $sys = Join-Path $env:SystemRoot ("System32" + [System.IO.Path]::DirectorySeparatorChar + $name)
    return (Test-Path $sys)
}

function Find-Dll([string]$name) {
    foreach ($d in $searchDirs) {
        $p = Join-Path $d $name
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    return $null
}

# 导出表里有没有这个符号（objdump -x 的文本边界匹配，避免子串误判）
function Test-Exported([string]$dllPath, [string]$symbol) {
    $text = (& $objdump -x $dllPath) -join "`n"
    $pat = '(\s|\[)' + [regex]::Escape($symbol) + '(\s|\r|\n|$)'
    return ($text -match $pat)
}

# ------------------------------------------------------------------- 主流程
$exePath = (Resolve-Path $Exe).Path
if (-not (Test-Path $exePath)) { Stop-Here "找不到 $Exe" }
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$Out = (Resolve-Path $Out).Path

Step "解析依赖：$exePath"
Note "objdump：$objdump"
Note "搜索目录：$($searchDirs -join ' | ')"

$pending  = New-Object System.Collections.Generic.List[string]
$resolved = [ordered]@{}      # DLL 名 -> 源路径
$reqs     = @{}               # DLL 小写名 -> HashSet[符号]
$visited  = @{}

$exeImports = Get-ImportMap $exePath
foreach ($k in $exeImports.Keys) {
    if (-not $reqs.ContainsKey($k.ToLower())) { $reqs[$k.ToLower()] = New-Object System.Collections.Generic.HashSet[string] }
    foreach ($s in $exeImports[$k]) { [void]$reqs[$k.ToLower()].Add($s) }
    $pending.Add($k)
}

while ($pending.Count -gt 0) {
    $name = $pending[0]
    $pending.RemoveAt(0)
    $key = $name.ToLower()
    if ($visited.ContainsKey($key)) { continue }
    $visited[$key] = $true

    if (Test-SystemDll $name) { continue }        # 系统自带，不打包

    $src = Find-Dll $name
    if (-not $src) { Stop-Here "无法解析 $name（系统目录、工具链、PATH 里都没有）" }
    $resolved[$name] = $src

    # 这个 DLL 自己还依赖什么，继续往下走
    $sub = Get-ImportMap $src
    foreach ($k in $sub.Keys) {
        if (-not $reqs.ContainsKey($k.ToLower())) { $reqs[$k.ToLower()] = New-Object System.Collections.Generic.HashSet[string] }
        foreach ($s in $sub[$k]) { [void]$reqs[$k.ToLower()].Add($s) }
        $pending.Add($k)
    }
}

if ($resolved.Count -eq 0) {
    Ok "没有非系统依赖，exe 已经是自包含的"
    exit 0
}

Step "校验导出的符号对得上"
$bad = 0
foreach ($name in $resolved.Keys) {
    $src = $resolved[$name]
    $want = $reqs[$name.ToLower()]
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($s in $want) {
        if (-not (Test-Exported $src $s)) { $missing.Add($s) }
    }
    if ($missing.Count -gt 0) {
        $bad++
        Write-Host "    [X]  $name 缺 $($missing.Count) 个导入符号：" -ForegroundColor Red
        foreach ($s in $missing) { Write-Host "         $s" -ForegroundColor Red }
    } else {
        Ok "$name"
    }
}
if ($bad -gt 0) {
    Stop-Here "有 $bad 个 DLL 与 exe 的导入表不匹配，装上去也起不来"
}

Step "复制到 $Out"
Get-ChildItem -Path $Out -Filter "*.dll" -ErrorAction SilentlyContinue | Remove-Item -Force
foreach ($name in $resolved.Keys) {
    $dst = Join-Path $Out $name
    Copy-Item $resolved[$name] $dst -Force
    $ver = (Get-Item $dst).Length
    Ok "$name  <- $($resolved[$name])  ($ver 字节)"
}

Write-Host ""
Write-Host "收集完成：$($resolved.Count) 个运行时 DLL" -ForegroundColor Green
exit 0
