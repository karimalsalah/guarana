# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
Packages a Windows build as a zip with its Qt runtime, license texts and a SHA-256 file.

.DESCRIPTION
Copies the build's bin folder without test programs and linker leftovers into
build\package\<name>\, runs windeployqt on the copy with the layout of the build's own deploy
step (Qt plugins in plugins\, as qt.conf says), adds LICENSE.txt, the license texts of LICENSES\
and the license files of the third-party code the build compiled, zips the folder's contents (extract
and run guarana.exe, as the release zips) and writes <name>.zip.sha256 in sha256sum format.

    powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1

The third-party notices are collected from the sources the build used (.cache\cpm and externals\)
and are best effort: read THIRD-PARTY-NOTICES.txt before you publish a zip. As with the release
zips, the Microsoft Visual C++ 2015-2022 redistributable must be installed where the zip runs.

.PARAMETER BuildDir
The build to package, relative to the repository root unless absolute. Default build\release.

.PARAMETER OutDir
Where the folder, the zip and its .sha256 go. Default build\package.

.PARAMETER Version
The version in the file name. Default: git describe of the build's source tree, without the v.

.PARAMETER Name
The package name. Default guarana-<version>-windows-x86_64, with -pgo for a PGO build and
-debug for a Debug build, as the release files are named.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$BuildDir = 'build\release',
    [string]$OutDir = 'build\package',
    [string]$Version = '',
    [string]$Name = ''
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
# The product name in file names, as release-binaries.yml names the release zips.
$Product = 'guarana'
# Test programs and linker leftovers that the bin folder may hold.
$Excluded = @('*.pdb', '*.ilk', '*.exp', '*.lib', 'tests.exe', '*_tests.exe', '*_test.exe', '*_fuzzer.exe', 'event_wait_probe.exe')
$LicensePattern = '^(licen[cs]e|copying|notice|copyright|unlicense)([._-].*)?$'

function Resolve-UnderRoot([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Get-FirstLine {
    param([string]$Exe, [string[]]$Arguments = @())
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments 2>$null
        if ($LASTEXITCODE -ne 0) { return '' }
        foreach ($line in @($output)) { if ("$line".Trim()) { return "$line".Trim() } }
    } catch { }
    return ''
}

function Read-CMakeCache([string]$Path) {
    $cache = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^([A-Za-z0-9_.+-]+):[A-Z]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    return $cache
}

function Write-Utf8([string]$Path, [string]$Text) {
    # UTF-8 without a byte order mark, LF line ends.
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
}

function Test-Excluded([string]$FileName) {
    foreach ($pattern in $Excluded) { if ($FileName -like $pattern) { return $true } }
    return $false
}

function Get-LicenseFiles([string]$Dir) {
    $files = @(Get-ChildItem -LiteralPath $Dir -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $LicensePattern -and $_.Length -lt 1MB })
    if ($files.Count -eq 0) {
        $files = @(Get-ChildItem -LiteralPath $Dir -File -Recurse -Depth 2 -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match $LicensePattern -and $_.Length -lt 1MB })
    }
    return $files
}

$script:Notices = @()
$script:NoLicense = @()

function Add-ThirdParty([string]$Label, [string]$Dir, [string]$Folder) {
    $files = @(Get-LicenseFiles $Dir)
    if ($files.Count -eq 0) {
        $script:NoLicense += $Label
        return
    }
    $destination = Join-Path $LicenseDir "third-party\$Folder"
    $base = $Dir.TrimEnd('\').Length + 1
    foreach ($file in $files) {
        $target = Join-Path $destination $file.FullName.Substring($base)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
    $script:Notices += "${Label}: licenses/third-party/$Folder/"
}

$exitCode = 0
try {
    $Build = Resolve-UnderRoot $BuildDir
    $Bin = Join-Path $Build 'bin'
    # guarana.exe and guarana-cli.exe; a build from before the rename has lichii.exe and eden-cli.exe.
    $gui = 'guarana.exe'
    if (-not (Test-Path -LiteralPath (Join-Path $Bin $gui))) { $gui = 'lichii.exe' }
    $cli = 'guarana-cli.exe'
    if (-not (Test-Path -LiteralPath (Join-Path $Bin $cli))) { $cli = 'eden-cli.exe' }
    foreach ($exe in @($gui, $cli)) {
        if (-not (Test-Path -LiteralPath (Join-Path $Bin $exe))) {
            throw "$Bin\$exe is missing; build first: powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1"
        }
    }
    $cache = Read-CMakeCache (Join-Path $Build 'CMakeCache.txt')
    # Package what this build compiled, even when it came from another checkout.
    $Source = $Root
    if ($cache['CMAKE_HOME_DIRECTORY'] -and (Test-Path -LiteralPath $cache['CMAKE_HOME_DIRECTORY'])) {
        $Source = [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY'])
    }
    if (-not $cache['Qt6_DIR']) { throw "$Build\CMakeCache.txt names no Qt6_DIR" }
    $QtRoot = Split-Path (Split-Path (Split-Path $cache['Qt6_DIR']))
    $windeployqt = Join-Path $QtRoot 'bin\windeployqt.exe'
    if (-not (Test-Path -LiteralPath $windeployqt)) { throw "No windeployqt.exe in $QtRoot\bin" }
    $qtVersion = 'unknown'
    $pri = Join-Path $QtRoot 'mkspecs\qconfig.pri'
    if (Test-Path -LiteralPath $pri) {
        $match = Select-String -LiteralPath $pri -Pattern '^QT_VERSION\s*=\s*(\S+)' | Select-Object -First 1
        if ($match) { $qtVersion = $match.Matches[0].Groups[1].Value }
    }
    $isDebug = $cache['CMAKE_BUILD_TYPE'] -eq 'Debug'
    $isPgo = "$($cache['CMAKE_CXX_FLAGS'])" -match '-fprofile-use'

    if (-not $Version) {
        $Version = Get-FirstLine 'git' @('-C', $Source, 'describe', '--tags', '--match', 'v[0-9]*', '--always', '--dirty')
        if (-not $Version) { $Version = 'unknown' }
        $Version = $Version -replace '^v', ''
    }
    if (-not $Name) {
        $Name = "$Product-$Version-windows-x86_64"
        if ($isPgo) { $Name += '-pgo' }
        if ($isDebug) { $Name += '-debug' }
    }

    $Out = Resolve-UnderRoot $OutDir
    $Stage = Join-Path $Out $Name
    $Zip = Join-Path $Out "$Name.zip"
    $LicenseDir = Join-Path $Stage 'licenses'
    Write-Host "==> Staging $Stage" -ForegroundColor Cyan
    if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath $Bin -Force)) {
        if ($item.PSIsContainer) {
            Copy-Item -LiteralPath $item.FullName -Destination $Stage -Recurse -Force
        } elseif (-not (Test-Excluded $item.Name)) {
            Copy-Item -LiteralPath $item.FullName -Destination $Stage -Force
        }
    }

    Write-Host "==> windeployqt (Qt $qtVersion)" -ForegroundColor Cyan
    # The arguments of the deploy step in src/lichii/CMakeLists.txt, so the layout matches qt.conf.
    $mode = '--release'
    if ($isDebug) { $mode = '--debug' }
    $deployArgs = @($mode, '--no-translations', '--no-compiler-runtime', '--no-opengl-sw', '--no-system-d3d-compiler',
                    '--dir', $Stage, '--libdir', $Stage, '--plugindir', (Join-Path $Stage 'plugins'), '--verbose', '0',
                    (Join-Path $Stage $gui))
    & $windeployqt @deployArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
    $qtConf = Join-Path $Stage 'qt.conf'
    if (-not (Test-Path -LiteralPath $qtConf)) { Write-Utf8 $qtConf "[Paths]`nPrefix=.`nPlugins=plugins`n" }

    Write-Host '==> Licenses' -ForegroundColor Cyan
    Copy-Item -LiteralPath (Join-Path $Source 'LICENSE.txt') -Destination $Stage -Force
    New-Item -ItemType Directory -Force -Path $LicenseDir | Out-Null
    $spdx = Join-Path $Source 'LICENSES'
    if (Test-Path -LiteralPath $spdx) { Copy-Item -Path (Join-Path $spdx '*') -Destination $LicenseDir -Force }

    # CPM packages: the version folders the build's compile commands use, else the newest one.
    $cpm = Join-Path $Source '.cache\cpm'
    $used = @{}
    $commands = Join-Path $Build 'compile_commands.json'
    if (Test-Path -LiteralPath $commands) {
        $hits = Select-String -LiteralPath $commands -Pattern '\.cache[\\/]+cpm[\\/]+([^\\/"\s]+)[\\/]+([^\\/"\s]+)' -AllMatches
        foreach ($hit in $hits) {
            foreach ($m in $hit.Matches) { $used["$($m.Groups[1].Value)/$($m.Groups[2].Value)"] = $true }
        }
    }
    if (Test-Path -LiteralPath $cpm) {
        foreach ($package in @(Get-ChildItem -LiteralPath $cpm -Directory)) {
            $versionDirs = @(Get-ChildItem -LiteralPath $package.FullName -Directory | Where-Object { $used.ContainsKey("$($package.Name)/$($_.Name)") })
            if ($versionDirs.Count -eq 0) {
                $versionDirs = @(Get-ChildItem -LiteralPath $package.FullName -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
            }
            foreach ($versionDir in $versionDirs) {
                Add-ThirdParty "$($package.Name) $($versionDir.Name)" $versionDir.FullName "$($package.Name)-$($versionDir.Name)"
            }
        }
    }
    # Code kept in the tree under externals\ (cmake-modules holds build scripts only).
    $externals = Join-Path $Source 'externals'
    if (Test-Path -LiteralPath $externals) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $externals -Directory | Where-Object { $_.Name -ne 'cmake-modules' })) {
            Add-ThirdParty "externals/$($dir.Name)" $dir.FullName $dir.Name
        }
    }
    $qtNotice = Join-Path $LicenseDir 'third-party\qt'
    New-Item -ItemType Directory -Force -Path $qtNotice | Out-Null
    Write-Utf8 (Join-Path $qtNotice 'NOTICE.txt') (@(
        "This package contains the Qt $qtVersion libraries and plugins, used under the GNU Lesser General",
        'Public License version 3 (licenses/LGPL-3.0-or-later.txt, which builds on',
        'licenses/GPL-3.0-or-later.txt). They are separate DLLs: you may replace them with your own build of',
        "the same Qt version. Qt's source code: https://download.qt.io/archive/qt/ (version $qtVersion)."
    ) -join "`n")
    $script:Notices += "Qt ${qtVersion}: licenses/third-party/qt/NOTICE.txt"

    $lines = @(
        "Third-party notices for $Name",
        '',
        "This program is free software under the GNU General Public License v3.0 or later: see",
        'LICENSE.txt. The license texts of the code in its source tree are in licenses/.',
        '',
        'It also contains code from the projects below, under their own licenses. This list was',
        'collected automatically from the sources the build compiled.',
        ''
    ) + ($script:Notices | Sort-Object | ForEach-Object { "- $_" })
    if ($script:NoLicense.Count -gt 0) {
        $lines += @('', 'No license file was found in these folders. Vendored sources carry SPDX headers; prebuilt',
                    'bundles (FFmpeg, OpenSSL and others) ship none, so add their notices by hand before you publish:', '')
        $lines += ($script:NoLicense | Sort-Object | ForEach-Object { "- $_" })
    }
    Write-Utf8 (Join-Path $Stage 'THIRD-PARTY-NOTICES.txt') (($lines -join "`n") + "`n")
    Write-Host "    $($script:Notices.Count) components with license files, $($script:NoLicense.Count) folders without"

    Write-Host "==> $Zip" -ForegroundColor Cyan
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    $prefix = $Stage.TrimEnd('\') + '\'
    $stream = [IO.File]::Open($Zip, [IO.FileMode]::CreateNew)
    try {
        $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in @(Get-ChildItem -LiteralPath $Stage -Recurse -File -Force)) {
                # Forward slashes, so every unzip tool reads the folders.
                $entry = $file.FullName.Substring($prefix.Length).Replace('\', '/')
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()
    Write-Utf8 "$Zip.sha256" "$hash  $Name.zip`n"
    $sizeMb = [math]::Round((Get-Item -LiteralPath $Zip).Length / 1MB, 1)
    Write-Host ''
    Write-Host "Packaged $Name ($sizeMb MB):" -ForegroundColor Green
    Write-Host "  $Zip"
    Write-Host "  $Zip.sha256  ($hash)"
    Write-Host "  $Stage  (the same files, unzipped)"
} catch {
    Write-Host "error: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
exit $exitCode
