<#
  build.ps1 - builds AudioConverter.exe with MinGW-w64 (g++).

  Incremental: object files are reused unless their source, or any header in the
  tree, is newer. Pass -Clean to rebuild from scratch.

  Examples:
    powershell -ExecutionPolicy Bypass -File build.ps1
    powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -Verbose
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$DebugBuild,
    [switch]$NoStatic
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }
function Write-Detail($message) { Write-Host "    $message" -ForegroundColor DarkGray }

# ---------------------------------------------------------------- toolchain
function Find-Compiler {
    $candidates = @()
    $onPath = Get-Command g++ -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $candidates += @(
        "$env:LOCALAPPDATA\AudioConverterTools\mingw64\bin\g++.exe",
        "$env:ProgramFiles\mingw64\bin\g++.exe",
        "C:\mingw64\bin\g++.exe",
        "C:\msys64\mingw64\bin\g++.exe",
        "C:\ProgramData\mingw64\bin\g++.exe"
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    throw "g++ was not found. Install MinGW-w64 (for example WinLibs) and add its bin folder to PATH."
}

$gxx = Find-Compiler
$binDir = Split-Path -Parent $gxx
$windres = Join-Path $binDir 'windres.exe'
Write-Step "Toolchain: $gxx"
& $gxx --version | Select-Object -First 1 | ForEach-Object { Write-Detail $_ }

# ------------------------------------------------------------------- layout
$buildDir = Join-Path $root 'build'
$objDir = Join-Path $buildDir 'obj'
$output = Join-Path $buildDir 'AudioConverter.exe'
$resourceObject = Join-Path $objDir 'app_res.o'

if ($Clean) {
    Write-Step 'Cleaning build directory'
    if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$sources = @(
    'src\main.cpp',
    'src\cli.cpp',
    'src\core\util.cpp',
    'src\core\crypto.cpp',
    'src\core\ncm.cpp',
    'src\core\audio.cpp',
    'src\core\decode_miniaudio.cpp',
    'src\core\decode_mediafoundation.cpp',
    'src\core\flac_encoder.cpp',
    'src\core\metadata.cpp',
    'src\core\i18n.cpp',
    'src\core\converter.cpp',
    'src\gui\theme.cpp',
    'src\gui\main_window.cpp'
)

$commonFlags = @(
    '-std=c++17',
    '-municode',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-Wno-cast-function-type'
)

if ($DebugBuild) {
    $commonFlags += @('-g', '-O0', '-D_DEBUG')
    Write-Detail 'Configuration: Debug'
} else {
    $commonFlags += @('-O2', '-DNDEBUG')
    Write-Detail 'Configuration: Release'
}
$commonFlags += @('-Isrc', '-Isrc\core', '-Isrc\gui', '-Isrc\third_party')

$linkFlags = @(
    '-mwindows',
    '-static', '-static-libgcc', '-static-libstdc++',
    '-lgdiplus', '-lmfplat', '-lmfreadwrite', '-lmfuuid',
    '-lole32', '-luuid', '-lshell32', '-lshlwapi', '-ldwmapi', '-lwinmm'
)
if ($NoStatic) { $linkFlags = $linkFlags | Where-Object { $_ -notlike '-static*' } }

# ------------------------------------------------------------------ compile
# Any header newer than an object forces a full rebuild, which keeps the
# dependency tracking honest without a separate depfile pass.
$newestHeader = Get-ChildItem -Path (Join-Path $root 'src') -Recurse -Include *.h, *.hpp |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

$objects = @()
$compiled = 0
$reused = 0
foreach ($source in $sources) {
    $sourcePath = Join-Path $root $source
    if (-not (Test-Path $sourcePath)) { throw "Missing source file: $source" }
    $objectPath = Join-Path $objDir (($source -replace '[\\/]', '_') -replace '\.cpp$', '.o')
    $objects += $objectPath

    $needsBuild = $true
    if ((Test-Path $objectPath) -and -not $Clean) {
        $objectTime = (Get-Item $objectPath).LastWriteTime
        $sourceTime = (Get-Item $sourcePath).LastWriteTime
        $needsBuild = ($sourceTime -gt $objectTime)
        if (-not $needsBuild -and $newestHeader -and $newestHeader.LastWriteTime -gt $objectTime) {
            $needsBuild = $true
        }
    }
    if ($needsBuild) {
        Write-Detail "CC  $source"
        & $gxx @commonFlags -c $sourcePath -o $objectPath
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
        $compiled++
    } else {
        $reused++
    }
}
Write-Step "Compiled $compiled file(s), reused $reused object file(s)"

# ----------------------------------------------------------------- resource
$resourceSource = Join-Path $root 'res\app.rc'
if (-not (Test-Path $windres)) {
    Write-Host "    windres.exe not found; building without the icon" -ForegroundColor Yellow
} else {
    $needResource = $true
    if ((Test-Path $resourceObject) -and -not $Clean) {
        $objectTime = (Get-Item $resourceObject).LastWriteTime
        $needResource = (Get-Item $resourceSource).LastWriteTime -gt $objectTime -or
                        (Get-Item (Join-Path $root 'res\app.ico')).LastWriteTime -gt $objectTime
    }
    if ($needResource) {
        Write-Detail 'RC  res\app.rc'
        & $windres (Join-Path $root 'res\app.rc') -O coff -o $resourceObject
        if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    }
    $objects += $resourceObject
}

# --------------------------------------------------------------------- link
Write-Step 'Linking AudioConverter.exe'
& $gxx @commonFlags @objects -o $output @linkFlags
if ($LASTEXITCODE -ne 0) { throw 'Link failed' }

$info = Get-Item $output
Write-Host ''
Write-Host '  BUILD COMPLETE' -ForegroundColor Green
Write-Host "  $output" -ForegroundColor Green
Write-Host ("  {0:N0} bytes" -f $info.Length) -ForegroundColor DarkGray
Write-Host ''
