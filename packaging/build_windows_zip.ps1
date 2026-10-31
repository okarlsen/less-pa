# Builds the Windows distribution of Less PA: a .zip with the VST3 bundle,
# an INSTALL.txt, the licence and the third-party notices.
#
# Produces packaging/build/Less-PA-<version>-Windows-VST3.zip from an
# existing Release build in plugin/build. Run by the "Windows VST3" GitHub
# workflow; also runs by hand on a Windows machine after:
#
#   cmake -B plugin/build -S plugin -A x64 -DLESSPA_INSTALL_AFTER_BUILD=OFF -DLESSPA_BUILD_LABEL=
#   cmake --build plugin/build --config Release --target PAEchoCanceller_VST3
#
# Like the macOS packaging scripts, it refuses a labelled test build and a
# bundle whose version is not the one in plugin/CMakeLists.txt.
#
# Usage: pwsh packaging/build_windows_zip.ps1 [-BuildDir plugin/build]

param(
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "plugin/build" }

$cmakeLists = Get-Content (Join-Path $RepoRoot "plugin/CMakeLists.txt") -Raw
if ($cmakeLists -notmatch 'project\(PAEchoCanceller VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)') {
    throw "could not read the version from plugin/CMakeLists.txt"
}
$Version = $Matches[1]
Write-Host "Less PA $Version -- building Windows VST3 zip"

# A labelled build is a test build (the panel would read "v1.1.7 RC").
$labelFile = Join-Path $BuildDir "lesspa_build_label.txt"
if (-not (Test-Path $labelFile)) {
    throw "$labelFile is missing; reconfigure with -DLESSPA_BUILD_LABEL= (empty)."
}
$label = (Get-Content $labelFile -Raw)
if ($label -and $label.Trim()) {
    throw "the build is labelled '$($label.Trim())', a test build. Reconfigure with -DLESSPA_BUILD_LABEL= (empty) and rebuild."
}
Write-Host "  checked: no test label"

$bundle = Join-Path $BuildDir "PAEchoCanceller_artefacts/Release/VST3/Less PA.vst3"
$binary = Join-Path $bundle "Contents/x86_64-win/Less PA.vst3"
if (-not (Test-Path $binary)) {
    throw "no Release VST3 at $bundle. Build it with: cmake --build plugin/build --config Release --target PAEchoCanceller_VST3"
}

# The DLL's version resource comes from the CMake project version, so a
# bundle built before a version bump carries the old one.
$binaryVersion = (Get-Item $binary).VersionInfo.ProductVersion
if ($binaryVersion -notmatch "^$([regex]::Escape($Version))(\.0)?$") {
    throw "Less PA.vst3 is version '$binaryVersion', expected $Version. Rebuild after the version bump."
}
Write-Host "  checked: bundle is version $Version"

$outDir = Join-Path $RepoRoot "packaging/build"
$folderName = "Less PA $Version Windows VST3"
$stage = Join-Path $outDir $folderName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -Recurse $bundle $stage
Copy-Item (Join-Path $RepoRoot "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $RepoRoot "THIRD-PARTY-NOTICES.md") (Join-Path $stage "THIRD-PARTY-NOTICES.md")

@"
Less PA $Version for Windows -- VST3 install
==========================================

Copy the folder "Less PA.vst3" into:

    C:\Program Files\Common Files\VST3\

That is the standard VST3 folder every Windows DAW scans. Windows asks for
administrator rights when you copy into it. Then restart your DAW, or
trigger a plugin rescan, so it picks up the new plugin.

Requirements: 64-bit Windows 10 or 11 on an Intel or AMD processor, and a
VST3 host (Reaper, Cubase, Nuendo, Ableton Live, Studio One, and others).
The PA reference goes into the plugin's sidechain input, as on macOS.

Questions or issues: https://github.com/okarlsen/less-pa
"@ -replace "`r?`n", "`r`n" | Set-Content -NoNewline -Encoding ascii (Join-Path $stage "INSTALL.txt")

$zip = Join-Path $outDir "Less-PA-$Version-Windows-VST3.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "  wrote $zip"
