# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
Builds Guarana on Windows from a fresh clone.

.DESCRIPTION
Checks every prerequisite and installs the ones it can under .deps\ (LLVM through
tools\windows\install-llvm.ps1, Qt 6.8.3 through aqtinstall, CMake and Ninja through pip), loads the
Visual Studio environment, configures build\release with the release flags (clang-cl, ThinLTO,
-fprofile-use=lichii-windows.profdata, -march=x86-64-v3) and builds guarana.exe and guarana-cli.exe (lichii.exe and eden-cli.exe in a tree from before the rename).
A second run reuses everything and rebuilds only what changed.

Visual Studio 2022 Build Tools (C++ workload) and the Vulkan SDK are checked, not installed; the
script says how to install them.

    powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1

.PARAMETER Config
Release (default) or Debug. Debug has no ThinLTO and no PGO and builds in build\debug.

.PARAMETER Tests
Also configures the unit tests (BUILD_TESTING and LICHII_TESTS) and builds every target.
scripts\run-tests.ps1 builds and runs them in their own tree, as CI does.

.PARAMETER NoPgo
Leaves out -fprofile-use=lichii-windows.profdata. Builds in build\release-nopgo.

.PARAMETER NoLto
Leaves out ThinLTO, for much faster links. Builds in build\release-nolto.

.PARAMETER Jobs
Parallel jobs. Default: the logical processors, at most one per 2 GB of RAM.

.PARAMETER March
The -march value, or none. Default x86-64-v3 (AVX2: Intel Haswell, AMD Zen and newer).

.PARAMETER BuildDir
The build directory, relative to the repository root unless absolute.

.PARAMETER QtDir
A Qt 6.8 msvc2022_64 folder (the one holding bin\ and lib\cmake\) to use instead of searching.

.PARAMETER LlvmDir
An LLVM folder (the one holding bin\clang-cl.exe) to use instead of searching.

.PARAMETER Target
Targets to build, comma separated. Default lichii,lichii-cmd; with -Tests every target.

.PARAMETER Reconfigure
Configures from scratch (cmake --fresh). Objects are kept, so only what changed is rebuilt.

.PARAMETER DryRun
Checks the prerequisites and prints the configure and build commands. Installs, configures and
builds nothing.

.PARAMETER CMakeArgs
Every other argument that starts with -D, -U or -- goes to the configure step after the script's
own, so it wins, for example -DENABLE_WERROR=OFF. Write -DNAME=VALUE without a space.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Tests -Jobs 8

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -NoPgo -NoLto -DENABLE_WERROR=OFF
#>
# No positional parameters: every value that is not a named option is a CMake argument.
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [switch]$Tests,
    [switch]$NoPgo,
    [switch]$NoLto,
    [int]$Jobs = 0,
    [string]$March = 'x86-64-v3',
    [string]$BuildDir = '',
    [string]$QtDir = '',
    [string]$LlvmDir = '',
    [string[]]$Target = @(),
    [switch]$Reconfigure,
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest in Windows PowerShell is many times slower while it draws a progress bar.
$ProgressPreference = 'SilentlyContinue'

$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Deps = Join-Path $Root '.deps'
# The Qt that .github/workflows/release-binaries.yml and build-check.yml install.
$QtVersion = '6.8.3'
$QtArch = 'win64_msvc2022_64'
# cmake_minimum_required in CMakeLists.txt. The pip fallback stays on the CMake that Visual Studio
# 2022 17.14 bundles (3.31), which is what the Windows CI jobs use.
$MinCMake = [version]'3.31'
$CMakePip = 'cmake>=3.31,<3.32'

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Info([string]$Text) { Write-Host "    $Text" }
function Write-Warn([string]$Text) { Write-Host "    warning: $Text" -ForegroundColor Yellow }
function ConvertTo-Slash([string]$Path) { return $Path.Replace('\', '/') }

function Get-FirstLine {
    # The first non-empty line a program prints, or '' when it is missing or fails. Never throws.
    param([string]$Exe, [string[]]$Arguments = @())
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments 2>$null
        if ($LASTEXITCODE -ne 0) { return '' }
        foreach ($line in @($output)) {
            $text = "$line".Trim()
            if ($text) { return $text }
        }
    } catch { }
    return ''
}

function Invoke-Tool {
    # Runs a program with its output on the console; throws when it fails.
    param([string]$Exe, [string[]]$Arguments = @())
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE" }
}

function Get-SortableVersion([string]$Text) {
    $version = $null
    if ([version]::TryParse($Text, [ref]$version)) { return $version }
    return [version]'0.0'
}

function Find-Python {
    foreach ($candidate in @(@('py', '-3'), @('python'), @('python3'))) {
        $exe = $candidate[0]
        $prefix = @($candidate | Select-Object -Skip 1)
        if (-not (Get-Command $exe -CommandType Application -ErrorAction SilentlyContinue)) { continue }
        $version = Get-FirstLine $exe ($prefix + @('-c', 'import sys; print(*sys.version_info[:2], sep=chr(46))'))
        if ($version -match '^\d+\.\d+$' -and [version]$version -ge [version]'3.9') {
            return [pscustomobject]@{ Exe = $exe; Prefix = $prefix; Version = $version }
        }
    }
    return $null
}

function Initialize-Deps {
    if (-not (Test-Path -LiteralPath $Deps)) { New-Item -ItemType Directory -Path $Deps | Out-Null }
    # Everything under .deps\ is downloaded; keep it out of git without touching .gitignore.
    $ignore = Join-Path $Deps '.gitignore'
    if (-not (Test-Path -LiteralPath $ignore)) { [IO.File]::WriteAllText($ignore, "*`n") }
}

function Get-VenvScripts {
    # .deps\venv\Scripts, created on first use with the system Python.
    $scripts = Join-Path $Deps 'venv\Scripts'
    if (-not (Test-Path -LiteralPath (Join-Path $scripts 'python.exe'))) {
        $python = Find-Python
        if (-not $python) {
            throw ('Python 3.9 or newer is needed to install the missing tools into .deps\. Install it ' +
                   '(winget install Python.Python.3.13), then run this script again.')
        }
        Initialize-Deps
        Write-Info "Creating .deps\venv with Python $($python.Version)"
        Invoke-Tool $python.Exe ($python.Prefix + @('-m', 'venv', (Join-Path $Deps 'venv')))
    }
    return $scripts
}

function Install-PipPackage([string[]]$Requirements) {
    $scripts = Get-VenvScripts
    Write-Info "pip install $($Requirements -join ' ') into .deps\venv"
    Invoke-Tool (Join-Path $scripts 'python.exe') (@('-m', 'pip', 'install', '--disable-pip-version-check', '--quiet') + $Requirements)
    return $scripts
}

function Find-Tool([string]$Name, [scriptblock]$IsGood) {
    # The first $Name.exe on PATH or in .deps\venv that $IsGood accepts.
    $paths = @()
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $paths += $command.Path }
    $paths += (Join-Path $Deps "venv\Scripts\$Name.exe")
    foreach ($path in $paths) {
        if ((Test-Path -LiteralPath $path) -and (& $IsGood $path)) { return $path }
    }
    return $null
}

function Get-CMakeVersion([string]$Exe) {
    $line = Get-FirstLine $Exe @('--version')
    if ($line -match 'version (\d+\.\d+(\.\d+)?)') { return [version]$Matches[1] }
    return $null
}

function Get-ClangClVersion([string]$Dir) {
    if (-not $Dir) { return $null }
    $exe = Join-Path $Dir 'bin\clang-cl.exe'
    if (-not (Test-Path -LiteralPath $exe)) { return $null }
    $line = Get-FirstLine $exe @('--version')
    if ($line -match 'clang version (\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    return $null
}

function Install-Llvm {
    # install-llvm.ps1 is the CI step: it downloads to and installs under $env:RUNNER_TEMP and
    # reports LLVM_ROOT in $env:GITHUB_ENV, so both point into .deps\ while it runs.
    Initialize-Deps
    $envFile = Join-Path $Deps 'install-llvm.env'
    Remove-Item -LiteralPath $envFile -ErrorAction SilentlyContinue
    $savedTemp = $env:RUNNER_TEMP
    $savedEnv = $env:GITHUB_ENV
    $env:RUNNER_TEMP = $Deps
    $env:GITHUB_ENV = $envFile
    try {
        & (Join-Path $Root 'tools\windows\install-llvm.ps1') | Out-Host
    } finally {
        $env:RUNNER_TEMP = $savedTemp
        $env:GITHUB_ENV = $savedEnv
        Remove-Item -LiteralPath (Join-Path $Deps 'llvm.exe') -ErrorAction SilentlyContinue
    }
    $line = @(Get-Content -LiteralPath $envFile -ErrorAction SilentlyContinue) |
        Where-Object { $_ -match '^LLVM_ROOT=' } | Select-Object -Last 1
    Remove-Item -LiteralPath $envFile -ErrorAction SilentlyContinue
    if (-not $line) { throw 'tools\windows\install-llvm.ps1 did not report where it installed LLVM' }
    return ($line -replace '^LLVM_ROOT=', '').Trim()
}

function Find-VulkanSdk {
    $candidates = @()
    if ($env:VULKAN_SDK) { $candidates += $env:VULKAN_SDK }
    $sdkRoot = Join-Path $env:SystemDrive 'VulkanSDK'
    if (Test-Path -LiteralPath $sdkRoot) {
        $candidates += @(Get-ChildItem -LiteralPath $sdkRoot -Directory |
            Sort-Object -Descending -Property @{ Expression = { Get-SortableVersion $_.Name } } |
            ForEach-Object { $_.FullName })
    }
    foreach ($dir in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $dir 'Bin\glslangValidator.exe')) { return $dir }
    }
    return $null
}

function Get-QtVersion([string]$Dir) {
    # The version of an MSVC Qt 6 prefix, or $null when $Dir is not one.
    if (-not $Dir -or -not (Test-Path -LiteralPath (Join-Path $Dir 'lib\cmake\Qt6\Qt6Config.cmake'))) { return $null }
    if (-not (Test-Path -LiteralPath (Join-Path $Dir 'lib\Qt6Core.lib'))) { return $null }
    $pri = Join-Path $Dir 'mkspecs\qconfig.pri'
    if (Test-Path -LiteralPath $pri) {
        $match = Select-String -LiteralPath $pri -Pattern '^QT_VERSION\s*=\s*(\d+\.\d+\.\d+)' | Select-Object -First 1
        if ($match) { return [version]$match.Matches[0].Groups[1].Value }
    }
    return [version]'0.0.0'
}

function Find-Qt {
    $candidates = @()
    if ($env:QT_ROOT_DIR) { $candidates += $env:QT_ROOT_DIR }
    if ($env:Qt6_DIR) {
        try { $candidates += (Split-Path (Split-Path (Split-Path $env:Qt6_DIR))) } catch { }
    }
    $candidates += (Join-Path $Deps "qt\$QtVersion\msvc2022_64")
    $qtRoot = Join-Path $env:SystemDrive 'Qt'
    if (Test-Path -LiteralPath $qtRoot) {
        $candidates += @(Get-ChildItem -LiteralPath $qtRoot -Directory -Filter '6.8.*' |
            Sort-Object -Descending -Property @{ Expression = { Get-SortableVersion $_.Name } } |
            ForEach-Object { Join-Path $_.FullName 'msvc2022_64' })
    }
    foreach ($dir in $candidates) {
        $version = Get-QtVersion $dir
        if ($version -and $version.Major -eq 6 -and $version.Minor -eq 8) { return $dir }
    }
    return $null
}

function Install-Qt {
    $scripts = Install-PipPackage @('aqtinstall>=3.2,<4')
    $out = Join-Path $Deps 'qt'
    Write-Info "Installing Qt $QtVersion ($QtArch with Qt Charts, as CI) into .deps\qt"
    # aqtinstall writes aqtinstall.log into the current folder.
    Push-Location -LiteralPath $Deps
    try {
        Invoke-Tool (Join-Path $scripts 'python.exe') @('-m', 'aqt', 'install-qt', 'windows', 'desktop', $QtVersion, $QtArch, '-m', 'qtcharts', '--outputdir', $out)
    } finally {
        Pop-Location
    }
    return (Join-Path $out "$QtVersion\msvc2022_64")
}

function ConvertTo-FlagPath([string]$Path) {
    # Forward slashes, and quotes when there is a space, so the path stays one compiler argument.
    $slashed = ConvertTo-Slash $Path
    if ($slashed.Contains(' ')) { return '"' + $slashed + '"' }
    return $slashed
}

function ConvertTo-CMakeString([string]$Text) {
    return $Text.Replace('\', '\\').Replace('"', '\"').Replace('$', '\$')
}

function Format-Command([string[]]$Words) {
    $quoted = foreach ($word in $Words) { if ($word -match '[\s"]') { "'" + $word.Replace("'", "''") + "'" } else { $word } }
    return ($quoted -join ' ')
}

function Get-DefaultJobs {
    $cpus = [Environment]::ProcessorCount
    $ramGb = 0
    try { $ramGb = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB) } catch { }
    if ($ramGb -gt 0) { return [int][math]::Max(1, [math]::Min($cpus, [math]::Floor($ramGb / 2))) }
    return [int]$cpus
}

# Restored at the end, so running the script inside an open PowerShell leaves its environment as it
# was (the Visual Studio environment and the PATH additions stay inside the script).
$SavedEnvironment = @{}
foreach ($item in @(Get-ChildItem Env:)) { $SavedEnvironment[$item.Name] = $item.Value }

function Restore-Environment {
    foreach ($item in @(Get-ChildItem Env:)) {
        if (-not $SavedEnvironment.ContainsKey($item.Name)) { [Environment]::SetEnvironmentVariable($item.Name, $null, 'Process') }
    }
    foreach ($name in @($SavedEnvironment.Keys)) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -ne $SavedEnvironment[$name]) {
            [Environment]::SetEnvironmentVariable($name, $SavedEnvironment[$name], 'Process')
        }
    }
}

$exitCode = 0
$missing = @()
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    if ($env:OS -ne 'Windows_NT') { throw 'This script is for Windows; on Linux use: bash scripts/build-linux.sh' }
    if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw "This script builds for x86-64; this machine is $env:PROCESSOR_ARCHITECTURE" }
    if ($Jobs -lt 0) { throw '-Jobs must be 1 or more' }
    foreach ($arg in $CMakeArgs) {
        if ($arg -notmatch '^(-D|-U|--)') { throw "Unknown argument '$arg'. Script options are listed by: Get-Help $PSCommandPath -Detailed; CMake arguments start with -D, -U or --." }
    }
    $Target = @($Target | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

    $release = $Config -eq 'Release'
    $useLto = $release -and -not $NoLto
    $usePgo = $release -and -not $NoPgo
    if ($BuildDir) {
        if ([IO.Path]::IsPathRooted($BuildDir)) { $Build = $BuildDir } else { $Build = Join-Path $Root $BuildDir }
    } else {
        $dirName = $Config.ToLowerInvariant()
        if ($release -and -not $usePgo) { $dirName += '-nopgo' }
        if ($release -and -not $useLto) { $dirName += '-nolto' }
        $Build = Join-Path $Root "build\$dirName"
    }
    $Build = [IO.Path]::GetFullPath($Build)
    if ($Jobs -eq 0) { $Jobs = Get-DefaultJobs }

    Write-Step 'Visual Studio 2022 C++ build tools'
    if ($env:VCToolsInstallDir -and $env:WindowsSdkDir -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
        Write-Info "Using the x64 environment already loaded ($env:VCToolsInstallDir)"
    } else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vs = ''
        if (Test-Path -LiteralPath $vswhere) {
            $vs = Get-FirstLine $vswhere @('-latest', '-products', '*', '-version', '[17.0,)', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
        }
        if (-not $vs) {
            throw ("Visual Studio 2022 Build Tools with the C++ workload are not installed. Install them with`n" +
                   "    winget install Microsoft.VisualStudio.2022.BuildTools --override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"`n" +
                   "or from https://visualstudio.microsoft.com/downloads/ (Build Tools for Visual Studio 2022, workload 'Desktop development with C++'), then run this script again.")
        }
        # vcvarsall.bat calls vswhere.exe by name.
        $env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH
        & (Join-Path $Root 'tools\windows\load-msvc-env.ps1') | Out-Host
        if (-not $env:VCToolsInstallDir) { throw "Loading the Visual Studio environment of $vs failed" }
    }
    if (-not $env:WindowsSdkDir) { throw 'No Windows SDK: add a Windows 11 SDK in the Visual Studio Installer (Modify, Individual components), then run this script again.' }
    Write-Info "MSVC $env:VCToolsVersion, Windows SDK $("$env:WindowsSDKVersion".TrimEnd('\'))"

    Write-Step "CMake $MinCMake or newer, and Ninja"
    $cmakeOk = { param($path) $version = Get-CMakeVersion $path; [bool]($version -and $version -ge $MinCMake) }
    $ninjaOk = { param($path) [bool](Get-FirstLine $path @('--version')) }
    $cmake = Find-Tool 'cmake' $cmakeOk
    $ninja = Find-Tool 'ninja' $ninjaOk
    $pipWants = @()
    if (-not $cmake) { $pipWants += $CMakePip }
    if (-not $ninja) { $pipWants += 'ninja' }
    if ($pipWants.Count -gt 0) {
        if ($DryRun) {
            Write-Info "would pip install $($pipWants -join ' ') into .deps\venv"
            $scripts = Join-Path $Deps 'venv\Scripts'
        } else {
            $scripts = Install-PipPackage $pipWants
        }
        if (-not $cmake) { $cmake = Join-Path $scripts 'cmake.exe' }
        if (-not $ninja) { $ninja = Join-Path $scripts 'ninja.exe' }
    }
    Write-Info "$cmake ($(Get-CMakeVersion $cmake))"
    Write-Info "$ninja ($(Get-FirstLine $ninja @('--version')))"

    Write-Step 'LLVM (clang-cl, lld-link, llvm-lib)'
    $pinned = $null
    $pinMatch = Select-String -LiteralPath (Join-Path $Root 'tools\windows\install-llvm.ps1') -Pattern '^\s*\$version\s*=\s*''([0-9.]+)''' | Select-Object -First 1
    if ($pinMatch) { $pinned = [version]$pinMatch.Matches[0].Groups[1].Value }
    if (-not $pinned) { throw 'Cannot read the LLVM version that tools\windows\install-llvm.ps1 pins' }
    $llvm = $null
    if ($LlvmDir) {
        $version = Get-ClangClVersion $LlvmDir
        if (-not $version) { throw "-LlvmDir $LlvmDir has no working bin\clang-cl.exe" }
        if ($usePgo -and $version -ne $pinned) {
            throw "-LlvmDir has clang-cl $version, but the PGO build needs ${pinned}: lichii-windows.profdata is read only by the LLVM that wrote it. Pass -NoPgo, or point -LlvmDir at LLVM $pinned."
        }
        $llvm = $LlvmDir
    } else {
        $sameMajor = $null
        $candidates = @()
        if ($env:LLVM_ROOT) { $candidates += $env:LLVM_ROOT }
        $candidates += (Join-Path $Deps 'llvm')
        if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles 'LLVM') }
        foreach ($dir in $candidates) {
            $version = Get-ClangClVersion $dir
            if (-not $version) { continue }
            if ($version -eq $pinned) { $llvm = $dir; break }
            if (-not $sameMajor -and $version.Major -eq $pinned.Major) { $sameMajor = $dir }
        }
        if (-not $llvm -and $sameMajor -and -not $usePgo) { $llvm = $sameMajor }
        if (-not $llvm) {
            if ($DryRun) {
                Write-Info "would install LLVM $pinned into .deps\llvm with tools\windows\install-llvm.ps1"
                $llvm = Join-Path $Deps 'llvm'
            } else {
                Write-Info "Installing LLVM $pinned into .deps\llvm with tools\windows\install-llvm.ps1 (about 400 MB; the installer may ask for administrator rights)"
                $llvm = Install-Llvm
            }
        }
    }
    $llvm = [IO.Path]::GetFullPath($llvm)
    $llvmVersion = Get-ClangClVersion $llvm
    if ($llvmVersion -and $llvmVersion -ne $pinned) { Write-Warn "clang-cl $llvmVersion; CI builds with $pinned" }
    if ($llvmVersion) { Write-Info "$llvm (clang-cl $llvmVersion)" } else { Write-Info "$llvm (not installed yet)" }

    Write-Step 'Vulkan SDK (glslangValidator)'
    $vulkan = Find-VulkanSdk
    if ($vulkan) {
        $env:VULKAN_SDK = $vulkan
        $env:PATH = (Join-Path $vulkan 'Bin') + ';' + $env:PATH
        Write-Info $vulkan
    } elseif (Get-Command glslangValidator -CommandType Application -ErrorAction SilentlyContinue) {
        Write-Info "glslangValidator from PATH: $((Get-Command glslangValidator -CommandType Application | Select-Object -First 1).Path)"
    } else {
        $message = ('The Vulkan SDK is not installed (the build compiles its shaders with its glslangValidator). ' +
                    'From an elevated PowerShell run: powershell -ExecutionPolicy Bypass -File tools\windows\install-vulkan-sdk.ps1 ' +
                    '(or install it from https://vulkan.lunarg.com/sdk/home#windows), then run this script again.')
        if (-not $DryRun) { throw $message }
        $missing += $message
    }

    Write-Step "Qt $QtVersion ($QtArch)"
    if ($QtDir) {
        $version = Get-QtVersion $QtDir
        if (-not $version) { throw "-QtDir $QtDir is not an MSVC Qt 6 folder (it needs lib\cmake\Qt6 and lib\Qt6Core.lib)" }
        if ($version.Major -ne 6 -or $version.Minor -ne 8) { Write-Warn "-QtDir holds Qt $version; CI builds with Qt $QtVersion" }
        $qt = $QtDir
    } else {
        $qt = Find-Qt
        if (-not $qt) {
            if ($DryRun) {
                Write-Info "would install Qt $QtVersion into .deps\qt with aqtinstall"
                $qt = Join-Path $Deps "qt\$QtVersion\msvc2022_64"
            } else {
                $qt = Install-Qt
                if (-not (Get-QtVersion $qt)) { throw "aqtinstall did not produce a Qt in $qt" }
            }
        }
    }
    $qt = [IO.Path]::GetFullPath($qt)
    Write-Info "$qt (Qt $(Get-QtVersion $qt))"

    Write-Step 'Compiler cache'
    if (Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue) {
        $launcher = @('-DUSE_CCACHE=ON')
        Write-Info 'ccache'
    } elseif (Get-Command sccache -CommandType Application -ErrorAction SilentlyContinue) {
        $sccache = ConvertTo-Slash (Get-Command sccache -CommandType Application | Select-Object -First 1).Path
        $launcher = @('-DUSE_CCACHE=OFF', "-DCMAKE_C_COMPILER_LAUNCHER=$sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache")
        Write-Info 'sccache'
    } else {
        $launcher = @('-DUSE_CCACHE=OFF', '-DCMAKE_C_COMPILER_LAUNCHER=', '-DCMAKE_CXX_COMPILER_LAUNCHER=')
        Write-Info 'none (installing ccache makes rebuilds after a configure change much faster)'
    }

    if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
        Write-Warn 'git is not on PATH: the configure step reads the commit with it'
    }
    try {
        $free = (Get-Item -LiteralPath $Root).PSDrive.Free
        if ($free -and $free -lt 30GB) { Write-Warn "$([math]::Round($free / 1GB)) GB free on this drive; a release build with its dependencies needs about 30 GB" }
    } catch { }

    # The release flags of the windows-x86_64-pgo job in .github/workflows/release-binaries.yml,
    # plus -march. They go into an initial-cache file because a quoted path inside a -D argument
    # does not survive Windows PowerShell's native argument passing.
    $flags = @('-DWIN32', '-D_WINDOWS', '-DNOMINMAX')
    if ($useLto) { $flags += '-flto=thin' }
    if ($March -and $March -ne 'none') { $flags += "-march=$March" }
    if ($usePgo) {
        $profileData = Join-Path $Root 'lichii-windows.profdata'
        if (-not (Test-Path -LiteralPath $profileData) -or (Get-Item -LiteralPath $profileData).Length -eq 0) {
            throw "$profileData is missing or empty; run with -NoPgo"
        }
        $flags += "-fprofile-use=$(ConvertTo-FlagPath $profileData)"
        $flags += @('-Wno-profile-instr-out-of-date', '-Wno-profile-instr-unprofiled', '-Wno-backend-plugin')
    }
    $cFlags = $flags -join ' '
    $cxxFlags = "-EHsc $cFlags"
    $linkFlags = ''
    if ($useLto) { $linkFlags = "/lldltocache:$(ConvertTo-FlagPath (Join-Path $Build 'lto-cache'))" }
    $initCache = Join-Path $Build 'build-windows-flags.cmake'
    $initText = (@(
        '# Written by scripts/build-windows.ps1 before every configure.',
        "set(CMAKE_C_FLAGS `"$(ConvertTo-CMakeString $cFlags)`" CACHE STRING `"`" FORCE)",
        "set(CMAKE_CXX_FLAGS `"$(ConvertTo-CMakeString $cxxFlags)`" CACHE STRING `"`" FORCE)",
        "set(CMAKE_EXE_LINKER_FLAGS `"$(ConvertTo-CMakeString $linkFlags)`" CACHE STRING `"`" FORCE)"
    ) -join "`n") + "`n"

    $onOff = 'OFF'
    if ($Tests) { $onOff = 'ON' }
    $llvmBin = (ConvertTo-Slash $llvm) + '/bin'
    $qtSlash = ConvertTo-Slash $qt
    # Local builds leave the update checker off: they have no build-N tag to compare.
    $configureArgs = @(
        '-C', (ConvertTo-Slash $initCache),
        '-S', (ConvertTo-Slash $Root), '-B', (ConvertTo-Slash $Build), '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DBUILD_TESTING=$onOff", "-DLICHII_TESTS=$onOff",
        '-DLICHII_CMD=ON', '-DLICHII_ROOM=OFF', '-DLICHII_ROOM_STANDALONE=OFF',
        '-DLICHII_USE_BUNDLED_QT=OFF', '-DENABLE_QT_TRANSLATION=ON', '-DLICHII_USE_BUNDLED_SDL3=ON',
        '-DENABLE_UPDATE_CHECKER=OFF', '-DUSE_DISCORD_PRESENCE=OFF',
        "-DCMAKE_PREFIX_PATH=$qtSlash", "-DQt6_DIR=$qtSlash/lib/cmake/Qt6",
        "-DCMAKE_C_COMPILER=$llvmBin/clang-cl.exe", "-DCMAKE_CXX_COMPILER=$llvmBin/clang-cl.exe",
        "-DCMAKE_LINKER=$llvmBin/lld-link.exe", "-DCMAKE_AR=$llvmBin/llvm-lib.exe",
        "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-Slash $ninja)"
    ) + $launcher + $CMakeArgs

    $buildArgs = @('--build', (ConvertTo-Slash $Build), '--parallel', "$Jobs")
    if ($Target.Count -gt 0) {
        $buildArgs += '--target'
        $buildArgs += $Target
    } elseif (-not $Tests) {
        $buildArgs += @('--target', 'lichii', 'lichii-cmd')
    }

    $describe = @($Config)
    if ($useLto) { $describe += 'ThinLTO' }
    if ($usePgo) { $describe += 'PGO' }
    if ($March -and $March -ne 'none') { $describe += "-march=$March" }
    if ($Tests) { $describe += 'tests' }

    $stampPath = Join-Path $Build '.configure-args'
    $stampText = ($configureArgs -join "`n") + "`n" + $initText
    $previous = ''
    if (Test-Path -LiteralPath $stampPath) { $previous = [IO.File]::ReadAllText($stampPath) }
    $fresh = [bool]$Reconfigure
    if ($previous -and -not $fresh) {
        # A different compiler or Qt in an existing cache leaves stale results behind: start over.
        $toolchain = '^-D(CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|CMAKE_PREFIX_PATH|Qt6_DIR)='
        $old = @($previous -split "`n" | Where-Object { $_ -match $toolchain }) -join "`n"
        $new = @($configureArgs | Where-Object { $_ -match $toolchain }) -join "`n"
        if ($old -ne $new) { $fresh = $true }
    }
    $needConfigure = $fresh -or ($previous -ne $stampText) -or -not (Test-Path -LiteralPath (Join-Path $Build 'build.ninja'))
    $configureCommand = @()
    if ($fresh) { $configureCommand += '--fresh' }
    $configureCommand += $configureArgs

    if ($DryRun) {
        Write-Step "Dry run: $($describe -join ', ') in $Build"
        Write-Host "$initCache would contain:"
        Write-Host $initText
        if ($needConfigure) { Write-Host (Format-Command (@($cmake) + $configureCommand)) } else { Write-Host '(configuration unchanged: no configure step)' }
        Write-Host (Format-Command (@($cmake) + $buildArgs))
        foreach ($item in $missing) { Write-Host "missing: $item" -ForegroundColor Yellow }
        if ($missing.Count -gt 0) { $exitCode = 1 }
    } else {
        $env:PATH = (Split-Path -Parent $cmake) + ';' + (Split-Path -Parent $ninja) + ';' + $env:PATH
        if ($needConfigure) {
            Write-Step "Configure $Build ($($describe -join ', '))"
            if (-not (Test-Path -LiteralPath (Join-Path $Root '.cache\cpm'))) {
                Write-Info 'The first configure downloads the dependencies into .cache\cpm; that takes a while.'
            }
            New-Item -ItemType Directory -Force -Path $Build | Out-Null
            [IO.File]::WriteAllText($initCache, $initText)
            Remove-Item -LiteralPath $stampPath -ErrorAction SilentlyContinue
            & $cmake @configureCommand
            if ($LASTEXITCODE -ne 0) {
                throw "Configure failed (exit code $LASTEXITCODE). The CMake error is above; $Build\CMakeFiles\CMakeConfigureLog.yaml has the details."
            }
            [IO.File]::WriteAllText($stampPath, $stampText)
        } else {
            Write-Step "Configure $Build (unchanged, skipped)"
        }

        Write-Step "Build with $Jobs jobs"
        & $cmake @buildArgs
        if ($LASTEXITCODE -ne 0) { throw "The build failed (exit code $LASTEXITCODE); the first error is above." }

        # clangd finds build\compile_commands.json on its own.
        $commands = Join-Path $Build 'compile_commands.json'
        $buildRoot = Join-Path $Root 'build'
        if ((Test-Path -LiteralPath $commands) -and $Build.StartsWith($buildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $commands -Destination (Join-Path $buildRoot 'compile_commands.json') -Force
        }

        $minutes = [math]::Round($clock.Elapsed.TotalMinutes, 1)
        Write-Host ''
        Write-Host "Built ($($describe -join ', ')) in $minutes min:" -ForegroundColor Green
        foreach ($exe in @('guarana.exe', 'guarana-cli.exe', 'lichii.exe', 'eden-cli.exe')) {
            $path = Join-Path $Build "bin\$exe"
            if (Test-Path -LiteralPath $path) { Write-Host "  $path" }
        }
        Write-Host 'Tests:   powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1'
        Write-Host 'Package: powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1'
    }
} catch {
    Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    Restore-Environment
}
exit $exitCode
