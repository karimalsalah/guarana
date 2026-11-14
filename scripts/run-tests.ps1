# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
Builds and runs the unit tests the way CI's build-check does on Windows, then the Python tool tests.

.DESCRIPTION
Configures build\tests through scripts\build-windows.ps1 like the windows-clang-cl job of
.github/workflows/build-check.yml (clang-cl, Release, no ThinLTO, no PGO, BUILD_TESTING and
LICHII_TESTS on, update checker on), builds every target and runs ctest. Then it runs the Python
tests of tools\nx2 and tools\bench, which need no build. Exits 1 when anything fails.

    powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1

.PARAMETER Config
Release (default) or Debug.

.PARAMETER Jobs
Parallel build and test jobs. Default: as scripts\build-windows.ps1 chooses.

.PARAMETER BuildDir
The test build directory. Default build\tests (build\tests-debug for Debug).

.PARAMETER Filter
Runs only the ctest tests whose name matches this regular expression (ctest -R).

.PARAMETER March
The -march value for the test build. Default none, as CI.

.PARAMETER NoBuild
Runs the tests of the existing test build without building.

.PARAMETER PythonOnly
Runs only the Python tool tests: no C++ toolchain needed.

.PARAMETER NoPython
Skips the Python tool tests.

.PARAMETER CMakeArgs
Arguments starting with -D, -U or -- go to the configure step, for example -DENABLE_WERROR=OFF.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1 -Filter config_tests
#>
# No positional parameters: every value that is not a named option is a CMake argument.
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [int]$Jobs = 0,
    [string]$BuildDir = '',
    [string]$Filter = '',
    [string]$March = 'none',
    [switch]$NoBuild,
    [switch]$PythonOnly,
    [switch]$NoPython,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

# The Python tests CI runs, and the bench tests (AGENTS.md, "Verify a change"). The Linux-only probe
# tests (test_switches.py, thread_worker_wake.py) run in scripts/run-tests.sh.
$PythonTests = @(
    @('tools\nx2\test_debug_mcp.py'),
    @('tools\nx2\test_cache_metrics.py'),
    @('tools\nx2\test_cache_bench.py'),
    @('-I', 'tools\bench\test_bench.py')
)

function Find-Python {
    foreach ($candidate in @(@('py', '-3'), @('python'), @('python3'))) {
        $exe = $candidate[0]
        $prefix = @($candidate | Select-Object -Skip 1)
        if (-not (Get-Command $exe -CommandType Application -ErrorAction SilentlyContinue)) { continue }
        $ErrorActionPreference = 'Continue'
        $version = & $exe @($prefix + @('-c', 'import sys; print(*sys.version_info[:2], sep=chr(46))')) 2>$null
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -eq 0 -and "$version" -match '^\d+\.\d+$' -and [version]"$version" -ge [version]'3.9') {
            return [pscustomobject]@{ Exe = $exe; Prefix = $prefix }
        }
    }
    return $null
}

function Read-CMakeCache([string]$Path) {
    $cache = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^([A-Za-z0-9_.+-]+):[A-Z]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    return $cache
}

$failures = @()
$savedPath = $env:PATH
try {
    if ($PythonOnly -and $NoPython) { throw '-PythonOnly and -NoPython leave nothing to run' }
    if (-not $BuildDir) {
        $BuildDir = 'build\tests'
        if ($Config -eq 'Debug') { $BuildDir = 'build\tests-debug' }
    }
    if ([IO.Path]::IsPathRooted($BuildDir)) { $Build = $BuildDir } else { $Build = Join-Path $Root $BuildDir }
    $Build = [IO.Path]::GetFullPath($Build)
    if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

    if (-not $PythonOnly) {
        if (-not $NoBuild) {
            $named = @{ Config = $Config; Tests = $true; NoPgo = $true; NoLto = $true; March = $March; BuildDir = $Build }
            if ($PSBoundParameters.ContainsKey('Jobs')) { $named['Jobs'] = $Jobs }
            # build-check.yml configures its test build with the update checker on.
            $extra = @('-DENABLE_UPDATE_CHECKER=ON') + $CMakeArgs
            & (Join-Path $PSScriptRoot 'build-windows.ps1') @named @extra
            if ($LASTEXITCODE -ne 0) { throw 'The test build failed; see above.' }
        }
        $cachePath = Join-Path $Build 'CMakeCache.txt'
        if (-not (Test-Path -LiteralPath $cachePath)) { throw "No build in $Build; run without -NoBuild." }
        $cache = Read-CMakeCache $cachePath
        if ($cache['LICHII_TESTS'] -ne 'ON') { throw "$Build was configured without LICHII_TESTS; run without -NoBuild." }
        $ctest = Join-Path (Split-Path -Parent $cache['CMAKE_COMMAND']) 'ctest.exe'
        if (-not (Test-Path -LiteralPath $ctest)) { $ctest = 'ctest' }
        # Test programs that link Qt6::Test find the Qt DLLs the build used, as CI's Qt on PATH does.
        if ($cache['Qt6_DIR']) {
            $qtBin = Join-Path (Split-Path (Split-Path (Split-Path $cache['Qt6_DIR']))) 'bin'
            if (Test-Path -LiteralPath $qtBin) { $env:PATH = "$qtBin;$env:PATH" }
        }
        $ctestArgs = @('--test-dir', $Build, '--output-on-failure', '--no-tests=error', '--parallel', "$Jobs")
        if ($Filter) { $ctestArgs += @('-R', $Filter) }
        Write-Host "==> ctest $($ctestArgs -join ' ')" -ForegroundColor Cyan
        & $ctest @ctestArgs
        if ($LASTEXITCODE -ne 0) { $failures += 'ctest' }
    }

    if (-not $NoPython) {
        $python = Find-Python
        if (-not $python) {
            Write-Host 'warning: Python 3.9 or newer was not found; the Python tool tests did not run' -ForegroundColor Yellow
            $failures += 'python (not found)'
        } else {
            Push-Location -LiteralPath $Root
            try {
                foreach ($test in $PythonTests) {
                    Write-Host "==> python $($test -join ' ')" -ForegroundColor Cyan
                    & $python.Exe @($python.Prefix + $test)
                    if ($LASTEXITCODE -ne 0) { $failures += ($test[-1]) }
                }
            } finally {
                Pop-Location
            }
        }
    }
} catch {
    Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
    $failures += 'setup'
} finally {
    $env:PATH = $savedPath
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "FAILED: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All tests passed.' -ForegroundColor Green
exit 0
