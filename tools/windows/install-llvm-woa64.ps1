# SPDX-License-Identifier: GPL-3.0-or-later

# The Windows ARM64 counterpart of install-llvm.ps1: installs LLVM's native ARM64 build (woa64) of
# the same release on a Windows ARM64 runner and exports its location as LLVM_ROOT. A separate
# script because build-check keys its compiler cache on install-llvm.ps1's hash, which an edit to
# that file would reset. Pinned by checksum, so a runner image update never changes the compiler.

$ErrorActionPreference = 'Stop'

$version = '22.1.8'
$sha256 = '76f44ef1ba6eeb5a65904e9500f042f588fade49952778ce48f0374daa934396'

if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') {
    throw "install-llvm-woa64.ps1 is for Windows ARM64 runners; this one is $env:PROCESSOR_ARCHITECTURE"
}
$installer = Join-Path $env:RUNNER_TEMP 'llvm-woa64.exe'
Invoke-WebRequest -OutFile $installer "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/LLVM-$version-woa64.exe"
if ((Get-FileHash -Algorithm SHA256 $installer).Hash -ne $sha256) {
    throw 'LLVM installer checksum mismatch'
}
$root = Join-Path $env:RUNNER_TEMP 'llvm'
Start-Process -Wait -FilePath $installer -ArgumentList '/S', "/D=$root"
$reported = & "$root/bin/clang-cl.exe" --version
$reported
if (-not ($reported -match "clang version $([regex]::Escape($version))")) {
    throw "Unexpected clang-cl: $reported"
}
if (-not ($reported -match 'Target: aarch64-pc-windows-msvc')) {
    throw "clang-cl does not target ARM64 Windows by default: $reported"
}
"LLVM_ROOT=$($root.Replace('\', '/'))" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV
