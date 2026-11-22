# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-3.0-or-later

$ErrorActionPreference = "Stop"

# Check if running as administrator
try {
    net session 1>$null 2>$null
} catch {
    Write-Host "This script must be run with administrator privileges!"
    Exit 1
}

$VulkanSDKVer = "1.4.341.1"
if ((Get-WmiObject -Class Win32_ComputerSystem).SystemType -match '(x64)' -eq "True") {
    $VulkanSDKArch = "X64"
    $VulkanSDKOs = "windows"
} else {
    $VulkanSDKArch = "ARM64"
    $VulkanSDKOs = "warm"
}
$VULKAN_SDK = "C:/VulkanSDK/$VulkanSDKVer"
$ExeFile = "vulkansdk-windows-$VulkanSDKArch-$VulkanSDKVer.exe"
$Uri = "https://sdk.lunarg.com/sdk/download/$VulkanSDKVer/$VulkanSDKOs/$ExeFile"
$Destination = "./$ExeFile"

# Check if Vulkan SDK is already installed
if (Test-Path $VULKAN_SDK) {
    Write-Host "-- Vulkan SDK already installed at $VULKAN_SDK"
    return
}

echo "Downloading Vulkan SDK $VulkanSDKVer from $Uri"
# LunarG's download server fails a CI job now and then; try it a few times before giving up.
$Attempts = 4
for ($Attempt = 1; $Attempt -le $Attempts; $Attempt++) {
    try {
        $WebClient = New-Object System.Net.WebClient
        $WebClient.DownloadFile($Uri, $Destination)
        if ((Get-Item $Destination).Length -gt 50MB) {
            break
        }
        throw "the download is only $((Get-Item $Destination).Length) bytes"
    } catch {
        if ($Attempt -eq $Attempts) {
            throw "Downloading $ExeFile failed $Attempts times: $_"
        }
        $Wait = 15 * $Attempt
        echo "Download attempt $Attempt failed ($_); retrying in $Wait s"
        Remove-Item -Force -ErrorAction SilentlyContinue $Destination
        Start-Sleep -Seconds $Wait
    }
}
echo "Finished downloading $ExeFile"

$Arguments = "--root `"$VULKAN_SDK`" --accept-licenses --default-answer --confirm-command install"

echo "Installing Vulkan SDK $VulkanSDKVer"
$InstallProcess = Start-Process -FilePath $Destination -NoNewWindow -PassThru -Wait -ArgumentList $Arguments
$ExitCode = $InstallProcess.ExitCode

if ($ExitCode -ne 0) {
    echo "Error installing Vulkan SDK $VulkanSDKVer (Error: $ExitCode)"
    Exit $ExitCode
}

echo "Finished installing Vulkan SDK $VulkanSDKVer"

if ("$env:GITHUB_ACTIONS" -eq "true") {
    echo "VULKAN_SDK=$VULKAN_SDK" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    echo "$VULKAN_SDK/Bin" | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
}
