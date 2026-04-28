[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$Generator = "MinGW Makefiles",
    [string]$BuildType = "Debug",
    [string]$LibDataChannelInstallDir = "",
    [string]$OpenSslBinDir = "",
    [string]$CCompiler = "",
    [string]$CxxCompiler = "",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Command
    )

    if ($Command.Length -gt 1) {
        & $Command[0] $Command[1..($Command.Length - 1)]
    } else {
        & $Command[0]
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $($LASTEXITCODE): $($Command -join ' ')"
    }
}

function Resolve-OpenSslBinDir {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        return $ExplicitPath
    }

    $candidates = @()
    if ($env:OPENSSL_ROOT_DIR) {
        $candidates += (Join-Path $env:OPENSSL_ROOT_DIR "bin")
    }

    $candidates += @(
        "C:\Program Files\OpenSSL-Win64\bin",
        "C:\Program Files (x86)\OpenSSL-Win32\bin",
        "D:\itffmpeg\OpenSSL-Win64\bin",
        "D:\itffmpeg\OpenSSL\bin",
        "D:\itffmpeg\x86_64-15.2.0-release-posix-seh-ucrt-rt_v13-rev1\mingw64\opt\bin"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return ""
}

function Copy-OpenSslRuntimeDlls {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceBinDir,
        [Parameter(Mandatory = $true)]
        [string[]]$TargetDirs
    )

    $dllPatterns = @("libssl-*.dll", "libcrypto-*.dll")
    $dlls = foreach ($pattern in $dllPatterns) {
        Get-ChildItem -Path $SourceBinDir -Filter $pattern -ErrorAction SilentlyContinue
    }

    foreach ($targetDir in $TargetDirs) {
        if (-not (Test-Path $targetDir)) {
            continue
        }

        foreach ($dll in $dlls) {
            Copy-Item -LiteralPath $dll.FullName -Destination $targetDir -Force
        }
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build-rtc"
}
if (-not $LibDataChannelInstallDir) {
    $LibDataChannelInstallDir = Join-Path $repoRoot "third_party/libdatachannel-install"
}

$packageConfig = Join-Path $LibDataChannelInstallDir "lib/cmake/LibDataChannel/LibDataChannelConfig.cmake"
if (-not (Test-Path $packageConfig)) {
    throw "LibDataChannelConfig.cmake was not found under $LibDataChannelInstallDir. Run scripts/setup_libdatachannel.ps1 first."
}

$cmakeConfigureArgs = @(
    "-S", $repoRoot,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DLIVE_STUDIO_BUILD_QT_PANEL=OFF",
    "-DLIVE_STUDIO_BUILD_RTC_PROBES=ON",
    "-DLIVE_STUDIO_LIBDATACHANNEL_INSTALL_DIR=$LibDataChannelInstallDir"
)

if ($Generator -eq "MinGW Makefiles") {
    $cmakeConfigureArgs += "-DCMAKE_SH=CMAKE_SH-NOTFOUND"
}
if ($CCompiler) {
    $cmakeConfigureArgs += "-DCMAKE_C_COMPILER=$CCompiler"
}
if ($CxxCompiler) {
    $cmakeConfigureArgs += "-DCMAKE_CXX_COMPILER=$CxxCompiler"
}

$cmakeConfigureCommand = @("cmake") + $cmakeConfigureArgs
Invoke-CheckedCommand -Command $cmakeConfigureCommand

if ($Build) {
    Invoke-CheckedCommand -Command @(
        "cmake", "--build", $BuildDir, "--config", $BuildType, "--target", "rtc_probe", "rtc_guest_probe", "--parallel"
    )

    $resolvedOpenSslBinDir = Resolve-OpenSslBinDir -ExplicitPath $OpenSslBinDir
    if ($resolvedOpenSslBinDir) {
        Copy-OpenSslRuntimeDlls -SourceBinDir $resolvedOpenSslBinDir -TargetDirs @(
            (Join-Path $BuildDir "apps/rtc_probe"),
            (Join-Path $BuildDir "apps/rtc_guest_probe")
        )
        Write-Host "Copied OpenSSL runtime DLLs from: $resolvedOpenSslBinDir"
    } else {
        Write-Warning "OpenSSL runtime DLLs were not copied because no bin directory was found. Probes may still need libssl/libcrypto on PATH."
    }
}

Write-Host ""
Write-Host "RTC probe build directory is ready: $BuildDir"
