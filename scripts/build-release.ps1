<#
.SYNOPSIS
    把 QtPlayer 打包成免安装的「绿色版」目录，别人解压后直接双击 QtPlayer.exe 即可运行。

.DESCRIPTION
    流程：CMake Release 构建 → 拷贝 exe → windeployqt 收集 Qt 依赖
          → 拷贝 FFmpeg 运行库 →（可选）压成 zip。

    为什么不在仓库里放打包好的目录：
    本程序使用 shared 版 FFmpeg，可运行目录约 250 MB（其中 avcodec-62.dll 约 93 MB、
    avfilter-11.dll 约 90 MB）。二进制放进 Git 会让仓库体积爆炸、历史无法瘦身，
    且这两个 DLL 已接近 GitHub 单文件 100 MB 硬上限。分发请用 GitHub Release 附件。

.PARAMETER QtDir
    Qt 安装目录（含 bin/windeployqt.exe），例如 C:\Qt\6.11.0\msvc2022_64。
    省略时自动在 C:\Qt\6.* 下找版本最高的 msvc*_64 目录。

.PARAMETER FfmpegPath
    FFmpeg 安装目录（需含 include/、lib/、bin/）。省略时用 CMakeLists.txt 中的默认值。

.PARAMETER Version
    用于产物命名，例如 v2.0 → dist\QtPlayer-v2.0-win64\ 与 QtPlayer-v2.0-win64.zip。

.PARAMETER DryRun
    只探测并打印各项路径与体积预估，不做任何构建或写文件。

.EXAMPLE
    pwsh -File scripts\build-release.ps1
    pwsh -File scripts\build-release.ps1 -Version v2.0 -NoSoftwareGL
    pwsh -File scripts\build-release.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string] $QtDir      = '',
    [string] $FfmpegPath = '',
    [string] $Version    = 'v2.0',
    [string] $BuildDir   = '',
    [string] $OutDir     = '',
    [string] $Generator  = 'Visual Studio 17 2022',
    [switch] $NoSoftwareGL,
    [switch] $NoCompilerRuntime,
    [switch] $SkipBuild,
    [switch] $NoZip,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------- 输出helper
function Say  ([string]$m) { Write-Host "[打包] $m" -ForegroundColor Cyan }
function Good ([string]$m) { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Warn ([string]$m) { Write-Host "[警告] $m" -ForegroundColor Yellow }
function Die  ([string]$m) { Write-Host "[失败] $m" -ForegroundColor Red; exit 1 }
function MB   ($bytes) { '{0:N1} MB' -f ($bytes / 1MB) }

# ---------------------------------------------------------------- 路径解析
$RepoRoot   = Split-Path $PSScriptRoot -Parent
$ProjectDir = Join-Path $RepoRoot 'Player\QtPlayer'

if (-not (Test-Path (Join-Path $ProjectDir 'CMakeLists.txt'))) {
    Die "找不到 CMakeLists.txt，脚本应放在 <仓库根>\scripts\ 下。当前仓库根：$RepoRoot"
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $ProjectDir 'build-pack' }
if ([string]::IsNullOrWhiteSpace($OutDir))   { $OutDir   = Join-Path $RepoRoot 'dist' }

# --- cmake：PATH 优先，其次 Qt 自带的 CMake
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCmd) { $cmakeCmd.Source } else { $null }
if (-not $cmake) {
    foreach ($c in (Get-ChildItem 'C:\Qt\Tools' -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -like 'CMake*' })) {
        $exe = Join-Path $c.FullName 'bin\cmake.exe'
        if (Test-Path $exe) { $cmake = $exe; break }
    }
}

# --- Qt：参数优先，其次自动探测
if ([string]::IsNullOrWhiteSpace($QtDir)) {
    $best = $null
    foreach ($v in (Get-ChildItem 'C:\Qt' -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^6\.' })) {
        foreach ($kit in (Get-ChildItem $v.FullName -Directory -ErrorAction SilentlyContinue |
                          Where-Object { $_.Name -match '^msvc\d+_64$' })) {
            if (-not $best -or ([version]$v.Name) -gt ([version]$best.Version)) {
                $best = [pscustomobject]@{ Version = $v.Name; Path = $kit.FullName }
            }
        }
    }
    if ($best) { $QtDir = $best.Path }
}

# --- FFmpeg：参数优先，否则沿用 CMakeLists.txt 的默认值
if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
    $FfmpegPath = 'C:/Program Files/ffmpeg/ffmpeg8.1'
    $m = [regex]::Match((Get-Content (Join-Path $ProjectDir 'CMakeLists.txt') -Raw),
                        'set\(FFMPEG_PATH\s+"([^"]+)"')
    if ($m.Success) { $FfmpegPath = $m.Groups[1].Value }
}

$windeployqt = if ($QtDir) { Join-Path $QtDir 'bin\windeployqt.exe' } else { $null }
$ffmpegBin   = Join-Path $FfmpegPath 'bin'

# ---------------------------------------------------------------- 前置检查
Say "仓库根    : $RepoRoot"
Say "工程目录  : $ProjectDir"
Say "CMake     : $(if ($cmake) { $cmake } else { '未找到' })"
Say "Qt 目录   : $(if ($QtDir) { $QtDir } else { '未找到' })"
Say "windeployqt: $(if ($windeployqt -and (Test-Path $windeployqt)) { $windeployqt } else { '未找到' })"
Say "FFmpeg    : $FfmpegPath"
Say "构建目录  : $BuildDir"
Say "输出目录  : $OutDir"

if (-not $cmake) { Die "找不到 cmake.exe。请安装 CMake，或确认 C:\Qt\Tools\CMake_64\bin\cmake.exe 存在。" }
if (-not $QtDir -or -not (Test-Path $windeployqt)) { Die "找不到 windeployqt.exe。请用 -QtDir 显式指定 Qt 目录（如 C:\Qt\6.11.0\msvc2022_64）。" }
if (-not (Test-Path $ffmpegBin)) { Die "找不到 FFmpeg 的 bin 目录：$ffmpegBin。请用 -FfmpegPath 指定 FFmpeg 安装目录。" }

# 需要随程序分发的 FFmpeg 运行库（与 CMakeLists.txt 的链接库一一对应）
$ffmpegDlls = 'avcodec', 'avformat', 'avutil', 'swresample', 'swscale', 'avfilter'

$resolved = @()
foreach ($name in $ffmpegDlls) {
    $hit = Get-ChildItem $ffmpegBin -Filter "$name-*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit) { Die "缺少运行库 $name-*.dll（在 $ffmpegBin）" }
    $resolved += $hit
}
$ffSize = ($resolved | Measure-Object Length -Sum).Sum
Say ("FFmpeg 运行库: {0} 个，合计 {1}" -f $resolved.Count, (MB $ffSize))
foreach ($r in $resolved) { Write-Host ("         {0,-22} {1,9}" -f $r.Name, (MB $r.Length)) }

if ($DryRun) {
    Write-Host ''
    Good 'DryRun 完成：路径探测正常，未做任何构建或写文件。'
    exit 0
}

# ---------------------------------------------------------------- 1) 构建
if (-not $SkipBuild) {
    Say '1/4 配置 CMake…'
    & $cmake -S $ProjectDir -B $BuildDir -G $Generator -A x64 `
             "-DCMAKE_PREFIX_PATH=$QtDir" `
             "-DFFMPEG_PATH=$($FfmpegPath -replace '\\', '/')"
    if ($LASTEXITCODE -ne 0) { Die "CMake 配置失败（exit $LASTEXITCODE）" }

    Say '2/4 编译 Release…'
    & $cmake --build $BuildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Die "编译失败（exit $LASTEXITCODE）" }
    Good '编译完成'
} else {
    Warn '已指定 -SkipBuild，跳过编译，直接使用现有构建产物'
}

$exeSrc = Get-ChildItem $BuildDir -Recurse -Filter 'QtPlayer.exe' -File -ErrorAction SilentlyContinue |
          Where-Object { $_.DirectoryName -notmatch 'CMakeFiles' } |
          Select-Object -First 1
if (-not $exeSrc) { Die "在 $BuildDir 里找不到 QtPlayer.exe，请先正常编译一次。" }
Good ("找到可执行文件: {0}（{1}）" -f $exeSrc.FullName, (MB $exeSrc.Length))

# ---------------------------------------------------------------- 2) 组装目录
$pkgName = "QtPlayer-$Version-win64"
$pkgDir  = Join-Path $OutDir $pkgName
if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null

Say '3/4 组装运行目录…'
$exeDst = Join-Path $pkgDir 'QtPlayer.exe'
Copy-Item $exeSrc.FullName $exeDst

# windeployqt 的 --compiler-runtime 依赖 VCINSTALLDIR 环境变量；在 IDE 之外运行时它通常没被设置，
# 于是 windeployqt 会【静默跳过】MSVC 运行库，导致没装 VC++ 可再发行组件的机器打不开。
# 这里既补上 VCINSTALLDIR，也在下面显式拷贝一份 CRT，双保险。
$vcRedistDir = $null
$crtDlls = @()
if (-not $NoCompilerRuntime) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = $null
    if (Test-Path $vswhere) {
        $vsRoot = (& $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1)
    }
    if ($vsRoot) {
        $env:VCINSTALLDIR = Join-Path $vsRoot 'VC'
        $verDir = Get-ChildItem (Join-Path $env:VCINSTALLDIR 'Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
                  Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if ($verDir) {
            $crtDir = Get-ChildItem (Join-Path $verDir.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
                      Select-Object -First 1
            if ($crtDir) {
                $vcRedistDir = $crtDir.FullName
                $crtDlls = @(Get-ChildItem $vcRedistDir -Filter '*.dll' -File)
            }
        }
    }
}

# 注意：CRT 由下面显式拷贝（app-local），所以对 windeployqt 关掉它自己的运行时拷贝，
# 否则它会多塞一个 ~25MB 的 vc_redist.x64.exe 安装包，对「绿色版」是多余的。
$wdArgs = @('--release', '--no-translations', '--no-compiler-runtime')
if ($NoSoftwareGL) { $wdArgs += '--no-opengl-sw' }
$wdArgs += $exeDst
& $windeployqt @wdArgs
if ($LASTEXITCODE -ne 0) { Die "windeployqt 失败（exit $LASTEXITCODE）" }

foreach ($dll in $resolved) {
    Copy-Item $dll.FullName (Join-Path $pkgDir $dll.Name) -Force
}
Good "Qt 依赖与 FFmpeg 运行库已就位"

if (-not $NoCompilerRuntime) {
    if ($crtDlls.Count -gt 0) {
        foreach ($d in $crtDlls) { Copy-Item $d.FullName (Join-Path $pkgDir $d.Name) -Force }
        Good ("MSVC 运行库已补齐：{0} 个（{1}）" -f $crtDlls.Count, $vcRedistDir)
    } else {
        Warn '未找到 MSVC 运行库（vcruntime140.dll 等）；目标机器若未装 VC++ 2015-2022 可再发行组件将无法启动'
    }
}

# 兜底自检：把 Qt6Core 与 avcodec 同时缺失视为明显异常
if (-not (Get-ChildItem $pkgDir -Recurse -Filter 'Qt6Core.dll' -ErrorAction SilentlyContinue)) {
    Warn ' 未在输出目录找到 Qt6Core.dll，windeployqt 可能没生效'
}

# ---------------------------------------------------------------- 3) 汇总
$files = Get-ChildItem $pkgDir -Recurse -File
$total = ($files | Measure-Object Length -Sum).Sum
Write-Host ''
Say ("4/4 打包完成：{0} 个文件，合计 {1}" -f $files.Count, (MB $total))
Say "输出目录: $pkgDir"
if ($total -gt 200MB) {
    Warn ("体积 {0}：其中绝大部分是 FFmpeg 的 DLL（shared 版）。" -f (MB $total))
    Warn ' 这正是不建议把它提交进 Git 的原因，请走 GitHub Release 附件分发。'
}

# ---------------------------------------------------------------- 4) 压缩
if (-not $NoZip) {
    $zip = Join-Path $OutDir "$pkgName.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Say '正在压缩…'
    Compress-Archive -Path $pkgDir -DestinationPath $zip -CompressionLevel Optimal
    Good ("zip: {0}（{1}）" -f $zip, (MB (Get-Item $zip).Length))
}

Write-Host ''
Good '完成。自测：直接双击输出目录里的 QtPlayer.exe（或解压 zip 后双击）。'
Write-Host '发布：在 GitHub 上新建 Release，把 zip 作为附件上传，用户下载解压即可运行。' -ForegroundColor DarkGray
