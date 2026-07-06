#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"

Write-Host "==> Installing Visual Studio 2026 Build Tools"

# Visual Studio 2026 (product line 18, GA "stable" channel) ships the MSVC
# 14.5x toolset. This must match the developers' local toolchain: CI previously
# ran VS 2022 (17.x / MSVC 14.44), which does NOT support C++23 delimited escape
# sequences (\u{...} etc.) that the 14.5x toolset accepts, causing "green
# locally, red in CI" divergence. The VS 18 channel is `stable` (NOT `release`
# or `pre`, which do not resolve for product line 18).
$installerUrl = "https://aka.ms/vs/18/stable/vs_buildtools.exe"
$installerPath = "$env:TEMP\vs_buildtools.exe"

Invoke-WebRequest -Uri $installerUrl -OutFile $installerPath -UseBasicParsing

# Install the MSVC toolset, Windows SDK, MSBuild, ATL, and ASan. The component
# IDs below are version-stable across VS product lines; VC.Tools.x86.x64 pulls
# the default MSVC toolset for the installed VS version (14.5x on VS 2026).
$args = @(
    "--quiet",
    "--wait",
    "--norestart",
    "--nocache",
    "--add", "Microsoft.VisualStudio.Workload.VCTools",
    "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "--add", "Microsoft.VisualStudio.Component.Windows11SDK.22621",
    "--add", "Microsoft.Component.MSBuild",
    "--add", "Microsoft.VisualStudio.Component.VC.ATL",
    "--add", "Microsoft.VisualStudio.Component.VC.ASAN",
    "--includeRecommended"
)

Write-Host "==> Running VS Build Tools installer (this may take a while)..."
$process = Start-Process -FilePath $installerPath -ArgumentList $args -Wait -PassThru -NoNewWindow
if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010) {
    throw "VS Build Tools installation failed with exit code $($process.ExitCode)"
}

Remove-Item $installerPath -Force -ErrorAction SilentlyContinue

Write-Host "==> Visual Studio 2026 Build Tools installed"
